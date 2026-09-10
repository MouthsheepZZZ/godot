#include "local_lrt_volume_3d.h"

#include "lrt_propagate.glsl.gen.h"

#include "core/io/image.h"
#include "core/math/face3.h"
#include "core/math/geometry_3d.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/texture.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/lrt_runtime.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

namespace {

constexpr float LRT_BIG_DISTANCE = 1e20f;
constexpr float LRT_SH_L0 = 0.28209479177f;
constexpr float LRT_SH_L1 = 0.48860251190f;
constexpr int LRT_TRANSPORT_RAY_COUNT = 32;
constexpr int LRT_SKY_RAY_COUNT = 128;

struct LRTTriangle {
	Vector3 vertex[3];
	Vector2 uv[3];
	Color vertex_color[3];
	Color albedo = Color(1, 1, 1);
	Ref<Image> albedo_image;
	bool use_vertex_color = false;
	bool vertex_color_srgb = false;
	bool repeat_texture = true;
	bool closed = false;
};

struct LRTLight {
	RSE::LightType type = RSE::LIGHT_DIRECTIONAL;
	Vector3 position;
	Vector3 direction;
	Color color;
	float energy = 1.0f;
	float range = 1.0f;
	float attenuation = 1.0f;
	float spot_cos = 0.0f;
	float spot_attenuation = 1.0f;
};

static uint64_t edge_key(uint32_t p_a, uint32_t p_b) {
	if (p_a > p_b) {
		SWAP(p_a, p_b);
	}
	return (uint64_t(p_a) << 32) | p_b;
}

static Color sample_image(const Ref<Image> &p_image, Vector2 p_uv, bool p_repeat) {
	if (p_image.is_null() || p_image->is_empty()) {
		return Color(1, 1, 1);
	}
	if (p_repeat) {
		p_uv.x = Math::fposmod(p_uv.x, 1.0f);
		p_uv.y = Math::fposmod(p_uv.y, 1.0f);
	} else {
		p_uv.x = CLAMP(p_uv.x, 0.0f, 1.0f);
		p_uv.y = CLAMP(p_uv.y, 0.0f, 1.0f);
	}
	const int x = CLAMP(int(p_uv.x * p_image->get_width()), 0, p_image->get_width() - 1);
	const int y = CLAMP(int((1.0f - p_uv.y) * p_image->get_height()), 0, p_image->get_height() - 1);
	return p_image->get_pixel(x, y).srgb_to_linear();
}

static void edt_1d(float *p_values, int32_t *p_labels, int p_stride, int p_count) {
	Vector<float> output;
	Vector<int32_t> output_labels;
	Vector<int> sites;
	Vector<float> boundaries;
	output.resize(p_count);
	output_labels.resize(p_count);
	sites.resize(p_count);
	boundaries.resize(p_count + 1);

	int k = 0;
	sites.write[0] = 0;
	boundaries.write[0] = -LRT_BIG_DISTANCE;
	boundaries.write[1] = LRT_BIG_DISTANCE;
	for (int q = 1; q < p_count; q++) {
		float separation = ((p_values[q * p_stride] + q * q) - (p_values[sites[k] * p_stride] + sites[k] * sites[k])) / (2.0f * (q - sites[k]));
		while (k > 0 && separation <= boundaries[k]) {
			k--;
			separation = ((p_values[q * p_stride] + q * q) - (p_values[sites[k] * p_stride] + sites[k] * sites[k])) / (2.0f * (q - sites[k]));
		}
		k++;
		sites.write[k] = q;
		boundaries.write[k] = separation;
		boundaries.write[k + 1] = LRT_BIG_DISTANCE;
	}

	k = 0;
	for (int q = 0; q < p_count; q++) {
		while (boundaries[k + 1] < q) {
			k++;
		}
		const int delta = q - sites[k];
		output.write[q] = delta * delta + p_values[sites[k] * p_stride];
		output_labels.write[q] = p_labels[sites[k] * p_stride];
	}
	for (int q = 0; q < p_count; q++) {
		p_values[q * p_stride] = output[q];
		p_labels[q * p_stride] = output_labels[q];
	}
}

static bool mesh_is_closed(const Vector<LRTTriangle> &p_triangles, int p_begin) {
	HashMap<Vector3i, uint32_t> vertex_ids;
	HashMap<uint64_t, uint32_t> edge_counts;
	uint32_t next_id = 0;
	for (int triangle_index = p_begin; triangle_index < p_triangles.size(); triangle_index++) {
		uint32_t ids[3];
		for (int corner = 0; corner < 3; corner++) {
			const Vector3 &v = p_triangles[triangle_index].vertex[corner];
			const Vector3i key(Math::round(v.x * 10000.0f), Math::round(v.y * 10000.0f), Math::round(v.z * 10000.0f));
			HashMap<Vector3i, uint32_t>::Iterator found = vertex_ids.find(key);
			if (found) {
				ids[corner] = found->value;
			} else {
				ids[corner] = next_id;
				vertex_ids.insert(key, next_id++);
			}
		}
		for (int edge = 0; edge < 3; edge++) {
			const uint64_t key = edge_key(ids[edge], ids[(edge + 1) % 3]);
			HashMap<uint64_t, uint32_t>::Iterator found = edge_counts.find(key);
			if (found) {
				found->value++;
			} else {
				edge_counts.insert(key, 1);
			}
		}
	}
	if (edge_counts.is_empty()) {
		return false;
	}
	for (const KeyValue<uint64_t, uint32_t> &edge : edge_counts) {
		if (edge.value != 2) {
			return false;
		}
	}
	return true;
}

} // namespace

int LocalLRTVolume3D::_index(const Vector3i &p, const Vector3i &p_size) {
	return p.x + p_size.x * (p.y + p_size.y * p.z);
}

LocalLRTVolume3D::Grid LocalLRTVolume3D::_create_grid(int p_resolution) const {
	Grid grid;
	const float longest_axis = MAX(volume_size.x, MAX(volume_size.y, volume_size.z));
	grid.cell_size = longest_axis / p_resolution;
	grid.size = Vector3i(
			MAX(1, int(Math::ceil(volume_size.x / grid.cell_size))),
			MAX(1, int(Math::ceil(volume_size.y / grid.cell_size))),
			MAX(1, int(Math::ceil(volume_size.z / grid.cell_size))));
	grid.origin = -volume_size * 0.5f;
	const int count = grid.size.x * grid.size.y * grid.size.z;
	grid.surface.resize(count);
	grid.closed_surface.resize(count);
	grid.distance.resize(count);
	grid.surface_distance_squared.resize(count);
	grid.nearest_surface.resize(count);
	grid.surface_normal.resize(count);
	grid.surface_owner.resize(count);
	grid.color.resize(count);
	grid.color_weight.resize(count);
	grid.surface.fill(0);
	grid.closed_surface.fill(0);
	grid.color.fill(Color(0, 0, 0, 0));
	grid.color_weight.fill(0);
	grid.surface_distance_squared.fill(LRT_BIG_DISTANCE);
	grid.nearest_surface.fill(-1);
	grid.surface_normal.fill(Vector3());
	grid.surface_owner.fill(0);
	return grid;
}

bool LocalLRTVolume3D::_collect_meshes(Node *p_node, Vector<MeshInstance3D *> &r_meshes) const {
	if (MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node)) {
		if (mesh_instance->is_lrt_enabled() && mesh_instance->is_visible_in_tree() && mesh_instance->get_mesh().is_valid()) {
			r_meshes.push_back(mesh_instance);
		}
	}
	for (int child_index = 0; child_index < p_node->get_child_count(); child_index++) {
		Node *child = p_node->get_child(child_index);
		if (!child->is_internal()) {
			_collect_meshes(child, r_meshes);
		}
	}
	return !r_meshes.is_empty();
}

void LocalLRTVolume3D::_collect_lights(Node *p_node, Vector<Light3D *> &r_lights) const {
	if (Light3D *light = Object::cast_to<Light3D>(p_node)) {
		if (light->is_visible_in_tree() && light->get_bake_mode() != Light3D::BAKE_DISABLED && !light->is_negative()) {
			r_lights.push_back(light);
		}
	}
	for (int child_index = 0; child_index < p_node->get_child_count(); child_index++) {
		Node *child = p_node->get_child(child_index);
		if (!child->is_internal()) {
			_collect_lights(child, r_lights);
		}
	}
}

bool LocalLRTVolume3D::_rasterize_mesh(MeshInstance3D *p_mesh_instance, Grid &r_sdf, Grid &r_color, String &r_error) {
	const Ref<Mesh> mesh = p_mesh_instance->get_mesh();
	const Transform3D to_volume = get_global_transform().affine_inverse() * p_mesh_instance->get_global_transform();
	const Vector3 mesh_scale = to_volume.basis.get_scale().abs();
	if (!Math::is_equal_approx(mesh_scale.x, mesh_scale.y) || !Math::is_equal_approx(mesh_scale.x, mesh_scale.z)) {
		r_error = vformat("%s uses unsupported non-uniform scaling.", p_mesh_instance->get_path());
		return false;
	}
	if (p_mesh_instance->get_skin().is_valid()) {
		r_error = vformat("%s uses unsupported skeletal deformation.", p_mesh_instance->get_path());
		return false;
	}
	Vector<LRTTriangle> triangles;
	const int mesh_triangle_begin = triangles.size();

	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		if (mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		Ref<Material> source_material = p_mesh_instance->get_active_material(surface);
		Ref<StandardMaterial3D> material = source_material;
		if (source_material.is_valid() && material.is_null()) {
			r_error = vformat("%s uses unsupported material %s on surface %d.", p_mesh_instance->get_path(), source_material->get_class(), surface);
			return false;
		}
		if (material.is_valid() && material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED) {
			r_error = vformat("%s uses unsupported transparent material on surface %d.", p_mesh_instance->get_path(), surface);
			return false;
		}

		Color albedo = material.is_valid() ? material->get_albedo().srgb_to_linear() : Color(1, 1, 1);
		Ref<Image> albedo_image;
		bool use_vertex_color = false;
		bool vertex_color_srgb = false;
		bool repeat_texture = true;
		if (material.is_valid()) {
			const Ref<Texture2D> albedo_texture = material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
			if (albedo_texture.is_valid()) {
				albedo_image = albedo_texture->get_image();
				if (albedo_image.is_valid()) {
					albedo_image = albedo_image->duplicate();
					if (albedo_image->is_compressed() && albedo_image->decompress() != OK) {
						r_error = vformat("Could not decompress albedo texture used by %s.", p_mesh_instance->get_path());
						return false;
					}
					albedo_image->convert(Image::FORMAT_RGBA8);
				}
			}
			use_vertex_color = material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR);
			vertex_color_srgb = material->get_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR);
			repeat_texture = material->get_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT);
		}

		const Array arrays = mesh->surface_get_arrays(surface);
		const Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
		const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
		const Vector<Vector2> uvs = arrays[Mesh::ARRAY_TEX_UV];
		const Vector<Color> colors = arrays[Mesh::ARRAY_COLOR];
		const int element_count = indices.is_empty() ? vertices.size() : indices.size();
		for (int element = 0; element + 2 < element_count; element += 3) {
			LRTTriangle triangle;
			triangle.albedo = albedo;
			triangle.albedo_image = albedo_image;
			triangle.use_vertex_color = use_vertex_color && colors.size() == vertices.size();
			triangle.vertex_color_srgb = vertex_color_srgb;
			triangle.repeat_texture = repeat_texture;
			for (int corner = 0; corner < 3; corner++) {
				const int vertex_index = indices.is_empty() ? element + corner : indices[element + corner];
				if (vertex_index < 0 || vertex_index >= vertices.size()) {
					r_error = vformat("%s has an invalid triangle index on surface %d.", p_mesh_instance->get_path(), surface);
					return false;
				}
				triangle.vertex[corner] = to_volume.xform(vertices[vertex_index]);
				triangle.uv[corner] = uvs.size() == vertices.size() ? uvs[vertex_index] : Vector2();
				triangle.vertex_color[corner] = colors.size() == vertices.size() ? colors[vertex_index] : Color(1, 1, 1);
			}
			triangles.push_back(triangle);
		}
	}

	const bool closed = mesh_is_closed(triangles, mesh_triangle_begin);
	for (int triangle_index = mesh_triangle_begin; triangle_index < triangles.size(); triangle_index++) {
		triangles.write[triangle_index].closed = closed;
	}

	auto rasterize = [&](Grid &grid) {
		for (const LRTTriangle &triangle : triangles) {
			AABB triangle_bounds(triangle.vertex[0], Vector3());
			triangle_bounds.expand_to(triangle.vertex[1]);
			triangle_bounds.expand_to(triangle.vertex[2]);
			Vector3i from;
			Vector3i to;
			for (int axis = 0; axis < 3; axis++) {
				from[axis] = CLAMP(int(Math::floor((triangle_bounds.position[axis] - grid.origin[axis]) / grid.cell_size)), 0, grid.size[axis] - 1);
				to[axis] = CLAMP(int(Math::floor((triangle_bounds.get_end()[axis] - grid.origin[axis]) / grid.cell_size)), 0, grid.size[axis] - 1);
			}
			for (int z = from.z; z <= to.z; z++) {
				for (int y = from.y; y <= to.y; y++) {
					for (int x = from.x; x <= to.x; x++) {
						const Vector3i cell(x, y, z);
						const Vector3 center = grid.origin + (Vector3(cell) + Vector3(0.5f, 0.5f, 0.5f)) * grid.cell_size;
						if (!Geometry3D::triangle_box_overlap(center, Vector3(grid.cell_size * 0.5f, grid.cell_size * 0.5f, grid.cell_size * 0.5f), triangle.vertex)) {
							continue;
						}
						const int index = _index(cell, grid.size);
						grid.surface.write[index] = 1;
						if (triangle.closed) {
							grid.closed_surface.write[index] = 1;
						}
						const Face3 face(triangle.vertex[0], triangle.vertex[1], triangle.vertex[2]);
						const Vector3 closest = face.get_closest_point_to(center);
						const float distance_squared = center.distance_squared_to(closest);
						if (distance_squared >= grid.surface_distance_squared[index]) {
							continue;
						}
						const Vector3 barycentric = Geometry3D::triangle_get_barycentric_coords(triangle.vertex[0], triangle.vertex[1], triangle.vertex[2], closest);
						const Vector2 uv = triangle.uv[0] * barycentric.x + triangle.uv[1] * barycentric.y + triangle.uv[2] * barycentric.z;
						Color sampled = triangle.albedo * sample_image(triangle.albedo_image, uv, triangle.repeat_texture);
						if (triangle.use_vertex_color) {
							Color vertex_color = triangle.vertex_color[0] * barycentric.x + triangle.vertex_color[1] * barycentric.y + triangle.vertex_color[2] * barycentric.z;
							if (triangle.vertex_color_srgb) {
								vertex_color = vertex_color.srgb_to_linear();
							}
							sampled *= vertex_color;
						}
						grid.surface_distance_squared.write[index] = distance_squared;
						grid.surface_normal.write[index] = Plane(triangle.vertex[0], triangle.vertex[1], triangle.vertex[2]).normal;
						grid.surface_owner.write[index] = uint64_t(p_mesh_instance->get_instance_id());
						grid.color.write[index] = sampled;
						grid.color_weight.write[index] = 1;
					}
				}
			}
		}
	};

	rasterize(r_sdf);
	rasterize(r_color);
	return true;
}

void LocalLRTVolume3D::_compute_distance(Grid &r_grid) {
	const int count = r_grid.size.x * r_grid.size.y * r_grid.size.z;
	bool has_surface = false;
	for (int index = 0; index < count; index++) {
		has_surface |= r_grid.surface[index] != 0;
		r_grid.distance.write[index] = r_grid.surface[index] ? 0.0f : LRT_BIG_DISTANCE;
		r_grid.nearest_surface.write[index] = r_grid.surface[index] ? index : -1;
	}
	if (!has_surface) {
		return;
	}
	for (int z = 0; z < r_grid.size.z; z++) {
		for (int y = 0; y < r_grid.size.y; y++) {
			const int offset = _index(Vector3i(0, y, z), r_grid.size);
			edt_1d(r_grid.distance.ptrw() + offset, r_grid.nearest_surface.ptrw() + offset, 1, r_grid.size.x);
		}
	}
	for (int z = 0; z < r_grid.size.z; z++) {
		for (int x = 0; x < r_grid.size.x; x++) {
			const int offset = _index(Vector3i(x, 0, z), r_grid.size);
			edt_1d(r_grid.distance.ptrw() + offset, r_grid.nearest_surface.ptrw() + offset, r_grid.size.x, r_grid.size.y);
		}
	}
	for (int y = 0; y < r_grid.size.y; y++) {
		for (int x = 0; x < r_grid.size.x; x++) {
			const int offset = _index(Vector3i(x, y, 0), r_grid.size);
			edt_1d(r_grid.distance.ptrw() + offset, r_grid.nearest_surface.ptrw() + offset, r_grid.size.x * r_grid.size.y, r_grid.size.z);
		}
	}
	for (int index = 0; index < count; index++) {
		r_grid.distance.write[index] = Math::sqrt(r_grid.distance[index]) * r_grid.cell_size;
	}
}

void LocalLRTVolume3D::_classify_inside(Grid &r_grid) {
	const int count = r_grid.size.x * r_grid.size.y * r_grid.size.z;
	Vector<uint8_t> exterior;
	exterior.resize(count);
	exterior.fill(0);
	Vector<Vector3i> queue;

	auto enqueue = [&](const Vector3i &p) {
		const int index = _index(p, r_grid.size);
		if (!r_grid.closed_surface[index] && !exterior[index]) {
			exterior.write[index] = 1;
			queue.push_back(p);
		}
	};
	for (int z = 0; z < r_grid.size.z; z++) {
		for (int y = 0; y < r_grid.size.y; y++) {
			enqueue(Vector3i(0, y, z));
			enqueue(Vector3i(r_grid.size.x - 1, y, z));
		}
	}
	for (int z = 0; z < r_grid.size.z; z++) {
		for (int x = 0; x < r_grid.size.x; x++) {
			enqueue(Vector3i(x, 0, z));
			enqueue(Vector3i(x, r_grid.size.y - 1, z));
		}
	}
	for (int y = 0; y < r_grid.size.y; y++) {
		for (int x = 0; x < r_grid.size.x; x++) {
			enqueue(Vector3i(x, y, 0));
			enqueue(Vector3i(x, y, r_grid.size.z - 1));
		}
	}

	static const Vector3i directions[6] = {
		Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 1, 0),
		Vector3i(0, -1, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1)
	};
	for (int cursor = 0; cursor < queue.size(); cursor++) {
		for (const Vector3i &direction : directions) {
			const Vector3i next = queue[cursor] + direction;
			if (next.x >= 0 && next.y >= 0 && next.z >= 0 && next.x < r_grid.size.x && next.y < r_grid.size.y && next.z < r_grid.size.z) {
				enqueue(next);
			}
		}
	}
	for (int index = 0; index < count; index++) {
		if (!exterior[index] && !r_grid.closed_surface[index]) {
			r_grid.distance.write[index] = -r_grid.distance[index];
		}
	}
}

void LocalLRTVolume3D::_publish_lighting() {
	if (!is_inside_tree()) {
		LRTRuntime::clear(get_instance_id());
		return;
	}
	LRTRuntime::State state;
	state.owner_id = get_instance_id();
	state.enabled = lighting_enabled && irradiance_textures[0].is_valid() && irradiance_textures[1].is_valid() && irradiance_textures[2].is_valid() && is_inside_tree();
	state.indirect_only = indirect_only && is_inside_tree();
	for (int channel = 0; channel < 3; channel++) {
		state.irradiance_textures[channel] = irradiance_textures[channel].is_valid() ? irradiance_textures[channel]->get_rid() : RID();
	}
	state.sky_visibility_texture = sky_visibility_texture.is_valid() ? sky_visibility_texture->get_rid() : RID();
	state.sky_energy = lighting_cleared ? 0.0f : sky_energy;
	const bool has_grid = !sdf_grid.distance.is_empty();
	const Vector3 grid_size = has_grid ? Vector3(sdf_grid.size) * sdf_grid.cell_size : volume_size;
	state.enabled = state.enabled && has_grid;
	state.bounds_min = has_grid ? get_global_transform().xform(sdf_grid.origin) : get_global_position() - volume_size * 0.5f;
	state.bounds_inv_size = Vector3(1.0f / grid_size.x, 1.0f / grid_size.y, 1.0f / grid_size.z);
	LRTRuntime::publish(state);
}

Error LocalLRTVolume3D::inject_first_bounce() {
	if (sdf_grid.distance.is_empty()) {
		bake_status = "Bake geometry before injecting first bounce";
		return ERR_UNCONFIGURED;
	}
	Vector<Light3D *> scene_lights;
	Node *scan_root = get_parent();
	if (scan_root) {
		_collect_lights(scan_root, scene_lights);
	}
	Vector<LRTLight> lights;
	const Transform3D to_volume = get_global_transform().affine_inverse();
	for (Light3D *light : scene_lights) {
		LRTLight item;
		item.type = light->get_light_type();
		item.position = to_volume.xform(light->get_global_position());
		item.direction = to_volume.basis.xform(-light->get_global_basis().get_column(2)).normalized();
		item.color = light->get_color().srgb_to_linear();
		item.energy = light->get_param(Light3D::PARAM_ENERGY) * light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
		item.range = light->get_param(Light3D::PARAM_RANGE);
		item.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
		item.spot_cos = Math::cos(Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE)));
		item.spot_attenuation = light->get_param(Light3D::PARAM_SPOT_ATTENUATION);
		lights.push_back(item);
	}

	const int cell_count = sdf_grid.size.x * sdf_grid.size.y * sdf_grid.size.z;
	direct_outgoing.resize(cell_count);
	direct_outgoing.fill(Color(0, 0, 0, 1));
	propagation_albedo.resize(cell_count);
	propagation_albedo.fill(Color(0, 0, 0, 1));
	propagation_normal.resize(cell_count);
	propagation_normal.fill(Color(0, 0, 0, 0));

	auto cell_center = [&](int p_index) {
		const int xy = sdf_grid.size.x * sdf_grid.size.y;
		const int z = p_index / xy;
		const int rest = p_index - z * xy;
		const int y = rest / sdf_grid.size.x;
		const int x = rest - y * sdf_grid.size.x;
		return sdf_grid.origin + (Vector3(x, y, z) + Vector3(0.5f, 0.5f, 0.5f)) * sdf_grid.cell_size;
	};
	auto sample_index = [&](const Vector3 &p_position) {
		Vector3i cell;
		for (int axis = 0; axis < 3; axis++) {
			cell[axis] = int(Math::floor((p_position[axis] - sdf_grid.origin[axis]) / sdf_grid.cell_size));
			if (cell[axis] < 0 || cell[axis] >= sdf_grid.size[axis]) {
				return -1;
			}
		}
		return _index(cell, sdf_grid.size);
	};
	auto free_space_normal = [&](int p_surface_index) {
		Vector3 normal = sdf_grid.surface_normal[p_surface_index].normalized();
		if (normal.is_zero_approx()) {
			return Vector3(0, 1, 0);
		}
		const Vector3 center = cell_center(p_surface_index);
		const int positive_index = sample_index(center + normal * sdf_grid.cell_size * 1.5f);
		const int negative_index = sample_index(center - normal * sdf_grid.cell_size * 1.5f);
		const float positive_distance = positive_index >= 0 ? sdf_grid.distance[positive_index] : LRT_BIG_DISTANCE;
		const float negative_distance = negative_index >= 0 ? sdf_grid.distance[negative_index] : LRT_BIG_DISTANCE;
		return positive_distance >= negative_distance ? normal : -normal;
	};
	auto visible = [&](const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance, int p_ignore_surface) {
		float distance = sdf_grid.cell_size * 1.6f;
		while (distance < p_max_distance) {
			const int index = sample_index(p_from + p_direction * distance);
			if (index < 0) {
				return true;
			}
			const float sdf = Math::abs(sdf_grid.distance[index]);
			if (sdf <= sdf_grid.cell_size * 0.55f && sdf_grid.nearest_surface[index] != p_ignore_surface) {
				return false;
			}
			distance += MAX(sdf, sdf_grid.cell_size * 0.5f);
		}
		return true;
	};

	for (int surface_index = 0; surface_index < cell_count; surface_index++) {
		if (!sdf_grid.surface[surface_index]) {
			continue;
		}
		const Vector3 position = cell_center(surface_index);
		const Vector3 normal = free_space_normal(surface_index);
		propagation_normal.write[surface_index] = Color(normal.x, normal.y, normal.z, 1.0f);
		propagation_albedo.write[surface_index] = sdf_grid.color[surface_index];
		Color irradiance(0, 0, 0, 1);
		for (const LRTLight &light : lights) {
			Vector3 to_light;
			float attenuation = 1.0f;
			float max_distance = volume_size.length();
			if (light.type == RSE::LIGHT_DIRECTIONAL) {
				to_light = -light.direction;
			} else {
				const Vector3 offset = light.position - position;
				const float distance = offset.length();
				if (distance <= 0.0001f || distance >= light.range) {
					continue;
				}
				to_light = offset / distance;
				max_distance = distance;
				float normalized_distance = distance / light.range;
				normalized_distance *= normalized_distance;
				normalized_distance *= normalized_distance;
				const float smooth_range = MAX(1.0f - normalized_distance, 0.0f);
				attenuation = smooth_range * smooth_range * Math::pow(MAX(distance, 0.0001f), -light.attenuation);
				if (light.type == RSE::LIGHT_SPOT) {
					const float cone = light.direction.dot(-to_light);
					if (cone <= light.spot_cos) {
						continue;
					}
					const float rim = (1.0f - cone) / MAX(1.0f - light.spot_cos, 0.0001f);
					attenuation *= 1.0f - Math::pow(CLAMP(rim, 0.0001f, 1.0f), light.spot_attenuation);
				}
			}
			const float cosine = MAX(normal.dot(to_light), 0.0f);
			if (cosine <= 0.0f || !visible(position, to_light, max_distance, surface_index)) {
				continue;
			}
			irradiance += light.color * (light.energy * attenuation * cosine);
		}
		const Color albedo = sdf_grid.color[surface_index];
		direct_outgoing.write[surface_index] = Color(irradiance.r * albedo.r / Math::PI, irradiance.g * albedo.g / Math::PI, irradiance.b * albedo.b / Math::PI, 1.0f);
	}

	transport_links.resize(cell_count * LRT_TRANSPORT_RAY_COUNT);
	for (TransportLink &link : transport_links) {
		link = TransportLink();
	}
	sky_visibility_sh.resize(cell_count);
	sky_visibility_sh.fill(Color(0, 0, 0, 0));
	const float max_trace_distance = volume_size.length();
	for (int receiver_index = 0; receiver_index < cell_count; receiver_index++) {
		if (!sdf_grid.surface[receiver_index]) {
			continue;
		}
		const Vector3 position = cell_center(receiver_index);
		const Vector3 normal = free_space_normal(receiver_index);
		const Vector3 tangent = normal.cross(Math::abs(normal.y) < 0.95f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
		const Vector3 bitangent = normal.cross(tangent);
		Color sky_coefficients(0, 0, 0, 0);
		for (int ray = 0; ray < LRT_TRANSPORT_RAY_COUNT; ray++) {
			const float u = (ray + 0.5f) / LRT_TRANSPORT_RAY_COUNT;
			const float phi = 2.0f * Math::PI * Math::fposmod(ray * 0.61803398875f, 1.0f);
			const float radial = Math::sqrt(MAX(1.0f - u * u, 0.0f));
			const Vector3 direction = (tangent * (Math::cos(phi) * radial) + bitangent * (Math::sin(phi) * radial) + normal * u).normalized();
			TransportLink &link = transport_links.write[receiver_index * LRT_TRANSPORT_RAY_COUNT + ray];
			link.direction[0] = direction.x;
			link.direction[1] = direction.y;
			link.direction[2] = direction.z;
			float distance = sdf_grid.cell_size * 1.6f;
			while (distance < max_trace_distance) {
				const int sample = sample_index(position + direction * distance);
				if (sample < 0) {
					break;
				}
				const float sdf = Math::abs(sdf_grid.distance[sample]);
				const int hit = sdf_grid.nearest_surface[sample];
				if (sdf <= sdf_grid.cell_size * 0.55f && hit >= 0 && hit != receiver_index) {
					link.source = hit;
					break;
				}
				distance += MAX(sdf, sdf_grid.cell_size * 0.5f);
			}
		}
		for (int ray = 0; ray < LRT_SKY_RAY_COUNT; ray++) {
			const float u = (ray + 0.5f) / LRT_SKY_RAY_COUNT;
			const float phi = 2.0f * Math::PI * Math::fposmod(ray * 0.61803398875f, 1.0f);
			const float radial = Math::sqrt(MAX(1.0f - u * u, 0.0f));
			const Vector3 direction = (tangent * (Math::cos(phi) * radial) + bitangent * (Math::sin(phi) * radial) + normal * u).normalized();
			float distance = sdf_grid.cell_size * 1.6f;
			bool escaped = false;
			while (distance < max_trace_distance) {
				const int sample = sample_index(position + direction * distance);
				if (sample < 0) {
					escaped = true;
					break;
				}
				const float sdf = Math::abs(sdf_grid.distance[sample]);
				const int hit = sdf_grid.nearest_surface[sample];
				if (sdf <= sdf_grid.cell_size * 0.55f && hit >= 0 && hit != receiver_index) {
					break;
				}
				distance += MAX(sdf, sdf_grid.cell_size * 0.5f);
			}
			if (escaped) {
				const float weight = 2.0f * Math::PI / LRT_SKY_RAY_COUNT;
				sky_coefficients += Color(LRT_SH_L0, LRT_SH_L1 * direction.x, LRT_SH_L1 * direction.y, LRT_SH_L1 * direction.z) * weight;
			}
		}
		sky_visibility_sh.write[receiver_index] = sky_coefficients;
	}

	radiance_sh.resize(cell_count);
	memset(radiance_sh.ptrw(), 0, radiance_sh.size() * sizeof(SH4));
	_clear_gpu_resources();
	const Error propagation_error = _run_gpu_propagation(1);
	if (propagation_error != OK) {
		bake_status = "Could not run the first LRT propagation step";
		return propagation_error;
	}
	bake_status = vformat("First bounce injected from %d light(s) into %dx%dx%d", lights.size(), sdf_grid.size.x, sdf_grid.size.y, sdf_grid.size.z);
	emit_signal(SNAME("lighting_updated"), bake_status);
	return OK;
}

void LocalLRTVolume3D::_clear_gpu_resources() {
	if (!propagation_rd) {
		return;
	}
	for (RID &uniform_set : propagation_uniform_sets) {
		if (uniform_set.is_valid()) {
			propagation_rd->free_rid(uniform_set);
			uniform_set = RID();
		}
	}
	for (RID &buffer : propagation_sh_buffers) {
		if (buffer.is_valid()) {
			propagation_rd->free_rid(buffer);
			buffer = RID();
		}
	}
	for (RID &buffer : propagation_fixed_buffers) {
		if (buffer.is_valid()) {
			propagation_rd->free_rid(buffer);
			buffer = RID();
		}
	}
	if (propagation_pipeline.is_valid()) {
		propagation_rd->free_rid(propagation_pipeline);
		propagation_pipeline = RID();
	}
	if (propagation_shader.is_valid()) {
		propagation_rd->free_rid(propagation_shader);
		propagation_shader = RID();
	}
	memdelete(propagation_rd);
	propagation_rd = nullptr;
	propagation_current_buffer = 0;
}

Error LocalLRTVolume3D::_create_gpu_resources() {
	static_assert(sizeof(Color) == 16);
	static_assert(sizeof(SH4) == 48);
	static_assert(sizeof(TransportLink) == 16);
	_clear_gpu_resources();
	propagation_rd = RenderingServer::get_singleton()->create_local_rendering_device();
	ERR_FAIL_NULL_V_MSG(propagation_rd, ERR_CANT_CREATE, "Unable to create a local RenderingDevice for LRT propagation.");
	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	const Error parse_error = shader_file->parse_versions_from_text(lrt_propagate_shader_glsl);
	if (parse_error != OK) {
		shader_file->print_errors("LRT propagation shader");
		_clear_gpu_resources();
		return parse_error;
	}
	propagation_shader = propagation_rd->shader_create_from_spirv(shader_file->get_spirv_stages());
	if (propagation_shader.is_null()) {
		_clear_gpu_resources();
		return ERR_CANT_CREATE;
	}
	propagation_pipeline = propagation_rd->compute_pipeline_create(propagation_shader);
	propagation_fixed_buffers[0] = propagation_rd->storage_buffer_create(direct_outgoing.size() * sizeof(Color), direct_outgoing.span().reinterpret<uint8_t>());
	propagation_fixed_buffers[1] = propagation_rd->storage_buffer_create(propagation_albedo.size() * sizeof(Color), propagation_albedo.span().reinterpret<uint8_t>());
	propagation_fixed_buffers[2] = propagation_rd->storage_buffer_create(propagation_normal.size() * sizeof(Color), propagation_normal.span().reinterpret<uint8_t>());
	propagation_fixed_buffers[3] = propagation_rd->storage_buffer_create(transport_links.size() * sizeof(TransportLink), transport_links.span().reinterpret<uint8_t>());
	propagation_fixed_buffers[4] = propagation_rd->storage_buffer_create(sky_visibility_sh.size() * sizeof(Color), sky_visibility_sh.span().reinterpret<uint8_t>());
	propagation_sh_buffers[0] = propagation_rd->storage_buffer_create(radiance_sh.size() * sizeof(SH4), radiance_sh.span().reinterpret<uint8_t>());
	propagation_sh_buffers[1] = propagation_rd->storage_buffer_create(radiance_sh.size() * sizeof(SH4));
	for (const RID &buffer : propagation_fixed_buffers) {
		if (buffer.is_null()) {
			_clear_gpu_resources();
			return ERR_CANT_CREATE;
		}
	}
	if (propagation_pipeline.is_null() || propagation_sh_buffers[0].is_null() || propagation_sh_buffers[1].is_null()) {
		_clear_gpu_resources();
		return ERR_CANT_CREATE;
	}

	for (int pass = 0; pass < 2; pass++) {
		Vector<RD::Uniform> uniforms;
		for (int binding = 0; binding < 4; binding++) {
			RD::Uniform uniform;
			uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			uniform.binding = binding;
			uniform.append_id(propagation_fixed_buffers[binding]);
			uniforms.push_back(uniform);
		}
		for (int binding = 4; binding < 6; binding++) {
			RD::Uniform uniform;
			uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			uniform.binding = binding;
			uniform.append_id(propagation_sh_buffers[(pass + binding - 4) & 1]);
			uniforms.push_back(uniform);
		}
		RD::Uniform sky_uniform;
		sky_uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		sky_uniform.binding = 6;
		sky_uniform.append_id(propagation_fixed_buffers[4]);
		uniforms.push_back(sky_uniform);
		propagation_uniform_sets[pass] = propagation_rd->uniform_set_create(uniforms, propagation_shader, 0);
		if (propagation_uniform_sets[pass].is_null()) {
			_clear_gpu_resources();
			return ERR_CANT_CREATE;
		}
	}
	propagation_current_buffer = 0;
	return OK;
}

Error LocalLRTVolume3D::_run_gpu_propagation(int p_iterations) {
	ERR_FAIL_COND_V(p_iterations < 1, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(direct_outgoing.is_empty() || transport_links.is_empty() || radiance_sh.size() != direct_outgoing.size(), ERR_UNCONFIGURED);
	lighting_cleared = false;
	bool created_resources = false;
	if (!propagation_rd) {
		const Error create_error = _create_gpu_resources();
		if (create_error != OK) {
			_clear_gpu_resources();
			return create_error;
		}
		created_resources = true;
	}
	struct PushConstant {
		uint32_t cell_count;
		uint32_t ray_count;
		float bounce_feedback;
		float sky_energy;
	} push_constant = { uint32_t(radiance_sh.size()), LRT_TRANSPORT_RAY_COUNT, bounce_feedback, sky_energy };

	for (int iteration = 0; iteration < p_iterations; iteration++) {
		RD::ComputeListID compute_list = propagation_rd->compute_list_begin();
		propagation_rd->compute_list_bind_compute_pipeline(compute_list, propagation_pipeline);
		propagation_rd->compute_list_bind_uniform_set(compute_list, propagation_uniform_sets[propagation_current_buffer], 0);
		propagation_rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		propagation_rd->compute_list_dispatch(compute_list, Math::division_round_up(uint32_t(radiance_sh.size()), 64u), 1, 1);
		propagation_rd->compute_list_end();
		propagation_current_buffer ^= 1;
	}
	propagation_rd->submit();
	propagation_rd->sync();
	const Vector<uint8_t> result = propagation_rd->buffer_get_data(propagation_sh_buffers[propagation_current_buffer]);
	if (result.size() != radiance_sh.size() * sizeof(SH4)) {
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	memcpy(radiance_sh.ptrw(), result.ptr(), result.size());
	return _upload_sh_textures(created_resources);
}

Error LocalLRTVolume3D::_upload_sh_textures(bool p_update_sky) {
	ERR_FAIL_COND_V(radiance_sh.is_empty(), ERR_UNCONFIGURED);
	const int channel_count = p_update_sky ? 4 : 3;
	for (int channel = 0; channel < channel_count; channel++) {
		Vector<Ref<Image>> slices;
		slices.resize(sdf_grid.size.z);
		for (int z = 0; z < sdf_grid.size.z; z++) {
			Ref<Image> image = Image::create_empty(sdf_grid.size.x, sdf_grid.size.y, false, Image::FORMAT_RGBAH);
			for (int y = 0; y < sdf_grid.size.y; y++) {
				for (int x = 0; x < sdf_grid.size.x; x++) {
					const int index = _index(Vector3i(x, y, z), sdf_grid.size);
					const int nearest = sdf_grid.nearest_surface[index];
					Color value(0, 0, 0, 0);
					if (nearest >= 0) {
						if (channel == 0) {
							value = radiance_sh[nearest].red;
						} else if (channel == 1) {
							value = radiance_sh[nearest].green;
						} else if (channel == 2) {
							value = radiance_sh[nearest].blue;
						} else {
							value = sky_visibility_sh[nearest];
						}
					}
					image->set_pixel(x, y, value);
				}
			}
			slices.write[z] = image;
		}
		if (channel < 3) {
			if (irradiance_textures[channel].is_valid() && irradiance_textures[channel]->get_width() == sdf_grid.size.x && irradiance_textures[channel]->get_height() == sdf_grid.size.y && irradiance_textures[channel]->get_depth() == sdf_grid.size.z) {
				irradiance_textures[channel]->update(slices);
			} else {
				irradiance_textures[channel].instantiate();
				ERR_FAIL_COND_V(irradiance_textures[channel]->create(Image::FORMAT_RGBAH, sdf_grid.size.x, sdf_grid.size.y, sdf_grid.size.z, false, slices) != OK, ERR_CANT_CREATE);
			}
		} else {
			if (sky_visibility_texture.is_valid() && sky_visibility_texture->get_width() == sdf_grid.size.x && sky_visibility_texture->get_height() == sdf_grid.size.y && sky_visibility_texture->get_depth() == sdf_grid.size.z) {
				sky_visibility_texture->update(slices);
			} else {
				sky_visibility_texture.instantiate();
				ERR_FAIL_COND_V(sky_visibility_texture->create(Image::FORMAT_RGBAH, sdf_grid.size.x, sdf_grid.size.y, sdf_grid.size.z, false, slices) != OK, ERR_CANT_CREATE);
			}
		}
	}
	_publish_lighting();
	return OK;
}

Error LocalLRTVolume3D::propagate(int p_iterations) {
	const Error error = _run_gpu_propagation(CLAMP(p_iterations, 1, 64));
	if (error == OK) {
		bake_status = vformat("Propagated %d additional LRT iteration(s)", CLAMP(p_iterations, 1, 64));
		emit_signal(SNAME("lighting_updated"), bake_status);
	}
	return error;
}

void LocalLRTVolume3D::_clear_debug() {
	if (debug_instance) {
		debug_instance->queue_free();
		debug_instance = nullptr;
	}
}

void LocalLRTVolume3D::_update_debug() {
	_clear_debug();
	if (debug_mode == DEBUG_DISABLED || sdf_grid.distance.is_empty()) {
		return;
	}
	const Grid &grid = debug_mode == DEBUG_SURFACE_COLOR ? color_grid : sdf_grid;
	const int slice = CLAMP(int(Math::round(debug_slice_position * (grid.size[debug_slice_axis] - 1))), 0, grid.size[debug_slice_axis] - 1);
	Vector<Transform3D> transforms;
	Vector<Color> colors;

	for (int z = 0; z < grid.size.z; z++) {
		for (int y = 0; y < grid.size.y; y++) {
			for (int x = 0; x < grid.size.x; x++) {
				const Vector3i cell(x, y, z);
				if (cell[debug_slice_axis] != slice) {
					continue;
				}
				const int index = _index(cell, grid.size);
				Color color;
				if (debug_mode == DEBUG_SURFACE_COLOR) {
					if (!grid.surface[index] || grid.color_weight[index] == 0) {
						continue;
					}
					color = grid.color[index] / grid.color_weight[index];
					color.a = 1.0f;
				} else if (debug_mode == DEBUG_INSIDE_OUTSIDE) {
					color = grid.surface[index] ? Color(1, 1, 1, 0.9f) : (grid.distance[index] < 0.0f ? Color(0.05f, 0.25f, 1.0f, 0.55f) : Color(1.0f, 0.2f, 0.05f, 0.18f));
				} else {
					const float normalized_distance = CLAMP(Math::abs(grid.distance[index]) / (grid.cell_size * 8.0f), 0.0f, 1.0f);
					color = Color(1.0f - normalized_distance, 0.15f, normalized_distance, 0.65f);
				}
				Transform3D transform;
				transform.origin = grid.origin + (Vector3(cell) + Vector3(0.5f, 0.5f, 0.5f)) * grid.cell_size;
				transforms.push_back(transform);
				colors.push_back(color);
				if (transforms.size() >= max_debug_cells) {
					z = grid.size.z;
					y = grid.size.y;
					break;
				}
			}
		}
	}

	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(grid.cell_size, grid.cell_size, grid.cell_size) * 0.92f);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material->set_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR, false);
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	material->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
	material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	box->set_material(material);

	Ref<MultiMesh> multimesh;
	multimesh.instantiate();
	multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	multimesh->set_use_colors(true);
	multimesh->set_mesh(box);
	multimesh->set_instance_count(transforms.size());
	for (int index = 0; index < transforms.size(); index++) {
		multimesh->set_instance_transform(index, transforms[index]);
		multimesh->set_instance_color(index, colors[index]);
	}

	debug_instance = memnew(MultiMeshInstance3D);
	debug_instance->set_name("LRTDebugSlice");
	debug_instance->set_multimesh(multimesh);
	debug_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	add_child(debug_instance, false, Node::INTERNAL_MODE_BACK);
}

Error LocalLRTVolume3D::bake() {
	if (!is_inside_tree()) {
		bake_status = "Volume must be inside the scene tree";
		return ERR_UNCONFIGURED;
	}
	Vector<MeshInstance3D *> meshes;
	Node *scan_root = get_parent();
	if (!scan_root || !_collect_meshes(scan_root, meshes)) {
		bake_status = "No participating MeshInstance3D found";
		clear();
		return ERR_DOES_NOT_EXIST;
	}

	int effective_sdf_resolution = sdf_resolution;
	int effective_color_resolution = color_resolution;
	for (MeshInstance3D *mesh : meshes) {
		if (mesh->get_lrt_sdf_resolution() > 0) {
			effective_sdf_resolution = MAX(effective_sdf_resolution, mesh->get_lrt_sdf_resolution());
		}
		if (mesh->get_lrt_color_resolution() > 0) {
			effective_color_resolution = MAX(effective_color_resolution, mesh->get_lrt_color_resolution());
		}
	}
	sdf_grid = _create_grid(effective_sdf_resolution);
	color_grid = _create_grid(effective_color_resolution);
	for (MeshInstance3D *mesh : meshes) {
		String error;
		if (!_rasterize_mesh(mesh, sdf_grid, color_grid, error)) {
			bake_status = error;
			clear();
			return ERR_INVALID_DATA;
		}
	}
	_compute_distance(sdf_grid);
	_classify_inside(sdf_grid);
	bake_status = vformat("Baked %d mesh(es): SDF %dx%dx%d, color %dx%dx%d", meshes.size(), sdf_grid.size.x, sdf_grid.size.y, sdf_grid.size.z, color_grid.size.x, color_grid.size.y, color_grid.size.z);
	_update_debug();
	emit_signal(SNAME("bake_completed"), bake_status);
	return OK;
}

void LocalLRTVolume3D::clear() {
	sdf_grid = Grid();
	color_grid = Grid();
	clear_lighting();
	_clear_debug();
	if (bake_status.is_empty()) {
		bake_status = "Not baked";
	}
}

void LocalLRTVolume3D::clear_lighting() {
	radiance_sh.clear();
	lighting_cleared = true;
	if (sdf_grid.distance.is_empty()) {
		_clear_gpu_resources();
		direct_outgoing.clear();
		propagation_albedo.clear();
		propagation_normal.clear();
		transport_links.clear();
		sky_visibility_sh.clear();
		for (Ref<ImageTexture3D> &texture : irradiance_textures) {
			texture.unref();
		}
		sky_visibility_texture.unref();
		LRTRuntime::clear(get_instance_id());
		return;
	}
	radiance_sh.resize(sdf_grid.distance.size());
	memset(radiance_sh.ptrw(), 0, radiance_sh.size() * sizeof(SH4));
	if (propagation_rd) {
		for (RID &buffer : propagation_sh_buffers) {
			propagation_rd->buffer_update(buffer, 0, radiance_sh.size() * sizeof(SH4), radiance_sh.ptr());
		}
		propagation_current_buffer = 0;
	}
	if (sky_visibility_sh.size() != sdf_grid.distance.size()) {
		sky_visibility_sh.resize(sdf_grid.distance.size());
		sky_visibility_sh.fill(Color(0, 0, 0, 0));
	}
	if (_upload_sh_textures() != OK) {
		for (Ref<ImageTexture3D> &texture : irradiance_textures) {
			texture.unref();
		}
		sky_visibility_texture.unref();
		LRTRuntime::clear(get_instance_id());
	}
}

void LocalLRTVolume3D::set_lighting_enabled(bool p_enabled) {
	lighting_enabled = p_enabled;
	_publish_lighting();
}

bool LocalLRTVolume3D::is_lighting_enabled() const {
	return lighting_enabled;
}

void LocalLRTVolume3D::set_indirect_only(bool p_enabled) {
	indirect_only = p_enabled;
	_publish_lighting();
}

bool LocalLRTVolume3D::is_indirect_only() const {
	return indirect_only;
}

void LocalLRTVolume3D::set_volume_size(const Vector3 &p_size) {
	volume_size = p_size.max(Vector3(0.01f, 0.01f, 0.01f));
	update_gizmos();
}

Vector3 LocalLRTVolume3D::get_volume_size() const {
	return volume_size;
}

void LocalLRTVolume3D::set_sdf_resolution(int p_resolution) {
	sdf_resolution = CLAMP(p_resolution, 8, 256);
}

int LocalLRTVolume3D::get_sdf_resolution() const {
	return sdf_resolution;
}

void LocalLRTVolume3D::set_color_resolution(int p_resolution) {
	color_resolution = CLAMP(p_resolution, 8, 256);
}

int LocalLRTVolume3D::get_color_resolution() const {
	return color_resolution;
}

void LocalLRTVolume3D::set_debug_mode(DebugMode p_mode) {
	debug_mode = p_mode;
	_update_debug();
}

LocalLRTVolume3D::DebugMode LocalLRTVolume3D::get_debug_mode() const {
	return debug_mode;
}

void LocalLRTVolume3D::set_debug_slice_axis(int p_axis) {
	debug_slice_axis = CLAMP(p_axis, 0, 2);
	_update_debug();
}

int LocalLRTVolume3D::get_debug_slice_axis() const {
	return debug_slice_axis;
}

void LocalLRTVolume3D::set_debug_slice_position(float p_position) {
	debug_slice_position = CLAMP(p_position, 0.0f, 1.0f);
	_update_debug();
}

float LocalLRTVolume3D::get_debug_slice_position() const {
	return debug_slice_position;
}

void LocalLRTVolume3D::set_max_debug_cells(int p_count) {
	max_debug_cells = CLAMP(p_count, 1, 1048576);
	_update_debug();
}

int LocalLRTVolume3D::get_max_debug_cells() const {
	return max_debug_cells;
}

String LocalLRTVolume3D::get_bake_status() const {
	return bake_status;
}

Dictionary LocalLRTVolume3D::get_field_summary() const {
	Dictionary summary;
	summary["status"] = bake_status;
	summary["sdf_size"] = sdf_grid.size;
	summary["color_size"] = color_grid.size;
	int surface_cells = 0;
	int inside_cells = 0;
	int color_cells = 0;
	float lighting_energy_sum = 0.0f;
	float lighting_energy_max = 0.0f;
	for (int index = 0; index < sdf_grid.surface.size(); index++) {
		surface_cells += sdf_grid.surface[index] != 0;
		inside_cells += sdf_grid.distance[index] < 0.0f;
		if (sdf_grid.surface[index] && index < radiance_sh.size() && index < propagation_normal.size()) {
			const Color &packed_normal = propagation_normal[index];
			const Color basis(LRT_SH_L0, (2.0f / 3.0f) * LRT_SH_L1 * packed_normal.r, (2.0f / 3.0f) * LRT_SH_L1 * packed_normal.g, (2.0f / 3.0f) * LRT_SH_L1 * packed_normal.b);
			const SH4 &value = radiance_sh[index];
			auto evaluate = [&](const Color &p_coefficients) {
				return MAX(p_coefficients.r * basis.r + p_coefficients.g * basis.g + p_coefficients.b * basis.b + p_coefficients.a * basis.a, 0.0f);
			};
			const float luminance = evaluate(value.red) * 0.2126f + evaluate(value.green) * 0.7152f + evaluate(value.blue) * 0.0722f;
			lighting_energy_sum += luminance;
			lighting_energy_max = MAX(lighting_energy_max, luminance);
		}
	}
	for (uint8_t occupied : color_grid.surface) {
		color_cells += occupied != 0;
	}
	summary["surface_cells"] = surface_cells;
	summary["inside_cells"] = inside_cells;
	summary["color_cells"] = color_cells;
	summary["lighting_energy_sum"] = lighting_energy_sum;
	summary["lighting_energy_max"] = lighting_energy_max;
	return summary;
}

Dictionary LocalLRTVolume3D::sample_nearest_surface(const Vector3 &p_local_position) const {
	Dictionary result;
	if (sdf_grid.distance.is_empty()) {
		return result;
	}
	Vector3i cell;
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = int(Math::floor((p_local_position[axis] - sdf_grid.origin[axis]) / sdf_grid.cell_size));
		if (cell[axis] < 0 || cell[axis] >= sdf_grid.size[axis]) {
			return result;
		}
	}
	const int index = _index(cell, sdf_grid.size);
	const int surface_index = sdf_grid.nearest_surface[index];
	if (surface_index < 0) {
		return result;
	}
	result["distance"] = sdf_grid.distance[index];
	result["normal"] = sdf_grid.surface_normal[surface_index];
	result["color"] = sdf_grid.color[surface_index];
	result["owner_instance_id"] = sdf_grid.surface_owner[surface_index];
	return result;
}

float LocalLRTVolume3D::sample_sdf(const Vector3 &p_local_position) const {
	if (sdf_grid.distance.is_empty()) {
		return LRT_BIG_DISTANCE;
	}
	Vector3i cell;
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = int(Math::floor((p_local_position[axis] - sdf_grid.origin[axis]) / sdf_grid.cell_size));
		if (cell[axis] < 0 || cell[axis] >= sdf_grid.size[axis]) {
			return LRT_BIG_DISTANCE;
		}
	}
	return sdf_grid.distance[_index(cell, sdf_grid.size)];
}

Color LocalLRTVolume3D::sample_surface_color(const Vector3 &p_local_position) const {
	if (color_grid.color.is_empty()) {
		return Color(0, 0, 0, 0);
	}
	Vector3i cell;
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = int(Math::floor((p_local_position[axis] - color_grid.origin[axis]) / color_grid.cell_size));
		if (cell[axis] < 0 || cell[axis] >= color_grid.size[axis]) {
			return Color(0, 0, 0, 0);
		}
	}
	const int index = _index(cell, color_grid.size);
	return color_grid.surface[index] ? color_grid.color[index] : Color(0, 0, 0, 0);
}

Color LocalLRTVolume3D::sample_first_bounce(const Vector3 &p_local_position) const {
	if (radiance_sh.is_empty()) {
		return Color(0, 0, 0, 1);
	}
	Vector3i cell;
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = int(Math::floor((p_local_position[axis] - sdf_grid.origin[axis]) / sdf_grid.cell_size));
		if (cell[axis] < 0 || cell[axis] >= sdf_grid.size[axis]) {
			return Color(0, 0, 0, 1);
		}
	}
	const int nearest = sdf_grid.nearest_surface[_index(cell, sdf_grid.size)];
	if (nearest < 0 || nearest >= radiance_sh.size()) {
		return Color(0, 0, 0, 1);
	}
	const Color packed_normal = propagation_normal[nearest];
	const Vector3 normal(packed_normal.r, packed_normal.g, packed_normal.b);
	const Color basis(LRT_SH_L0, (2.0f / 3.0f) * LRT_SH_L1 * normal.x, (2.0f / 3.0f) * LRT_SH_L1 * normal.y, (2.0f / 3.0f) * LRT_SH_L1 * normal.z);
	const SH4 &value = radiance_sh[nearest];
	auto dot_coefficients = [&](const Color &p_coefficients) {
		return p_coefficients.r * basis.r + p_coefficients.g * basis.g + p_coefficients.b * basis.b + p_coefficients.a * basis.a;
	};
	return Color(MAX(dot_coefficients(value.red), 0.0f), MAX(dot_coefficients(value.green), 0.0f), MAX(dot_coefficients(value.blue), 0.0f), 1.0f);
}

float LocalLRTVolume3D::sample_sky_visibility(const Vector3 &p_local_position, const Vector3 &p_local_normal) const {
	if (sky_visibility_sh.is_empty()) {
		return 0.0f;
	}
	Vector3i cell;
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = int(Math::floor((p_local_position[axis] - sdf_grid.origin[axis]) / sdf_grid.cell_size));
		if (cell[axis] < 0 || cell[axis] >= sdf_grid.size[axis]) {
			return 0.0f;
		}
	}
	const int nearest = sdf_grid.nearest_surface[_index(cell, sdf_grid.size)];
	if (nearest < 0) {
		return 0.0f;
	}
	const Vector3 normal = p_local_normal.normalized();
	const Color basis(LRT_SH_L0, (2.0f / 3.0f) * LRT_SH_L1 * normal.x, (2.0f / 3.0f) * LRT_SH_L1 * normal.y, (2.0f / 3.0f) * LRT_SH_L1 * normal.z);
	const Color &coefficients = sky_visibility_sh[nearest];
	return MAX(coefficients.r * basis.r + coefficients.g * basis.g + coefficients.b * basis.b + coefficients.a * basis.a, 0.0f);
}

void LocalLRTVolume3D::set_propagation_iterations(int p_iterations) {
	propagation_iterations = CLAMP(p_iterations, 1, 64);
}

int LocalLRTVolume3D::get_propagation_iterations() const {
	return propagation_iterations;
}

void LocalLRTVolume3D::set_bounce_feedback(float p_feedback) {
	bounce_feedback = CLAMP(p_feedback, 0.0f, 1.0f);
}

float LocalLRTVolume3D::get_bounce_feedback() const {
	return bounce_feedback;
}

void LocalLRTVolume3D::set_sky_energy(float p_energy) {
	sky_energy = MAX(p_energy, 0.0f);
	_publish_lighting();
}

float LocalLRTVolume3D::get_sky_energy() const {
	return sky_energy;
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		_publish_lighting();
		update_configuration_warnings();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		LRTRuntime::clear(get_instance_id());
	}
}

PackedStringArray LocalLRTVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!get_transform().basis.is_orthogonal() || !get_transform().basis.get_scale().is_equal_approx(Vector3(1, 1, 1))) {
		warnings.push_back(RTR("LocalLRTVolume3D must remain axis-aligned with unit scale."));
	}
	return warnings;
}

void LocalLRTVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_volume_size", "size"), &LocalLRTVolume3D::set_volume_size);
	ClassDB::bind_method(D_METHOD("get_volume_size"), &LocalLRTVolume3D::get_volume_size);
	ClassDB::bind_method(D_METHOD("set_sdf_resolution", "resolution"), &LocalLRTVolume3D::set_sdf_resolution);
	ClassDB::bind_method(D_METHOD("get_sdf_resolution"), &LocalLRTVolume3D::get_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_color_resolution", "resolution"), &LocalLRTVolume3D::set_color_resolution);
	ClassDB::bind_method(D_METHOD("get_color_resolution"), &LocalLRTVolume3D::get_color_resolution);
	ClassDB::bind_method(D_METHOD("set_debug_mode", "mode"), &LocalLRTVolume3D::set_debug_mode);
	ClassDB::bind_method(D_METHOD("get_debug_mode"), &LocalLRTVolume3D::get_debug_mode);
	ClassDB::bind_method(D_METHOD("set_debug_slice_axis", "axis"), &LocalLRTVolume3D::set_debug_slice_axis);
	ClassDB::bind_method(D_METHOD("get_debug_slice_axis"), &LocalLRTVolume3D::get_debug_slice_axis);
	ClassDB::bind_method(D_METHOD("set_debug_slice_position", "position"), &LocalLRTVolume3D::set_debug_slice_position);
	ClassDB::bind_method(D_METHOD("get_debug_slice_position"), &LocalLRTVolume3D::get_debug_slice_position);
	ClassDB::bind_method(D_METHOD("set_max_debug_cells", "count"), &LocalLRTVolume3D::set_max_debug_cells);
	ClassDB::bind_method(D_METHOD("get_max_debug_cells"), &LocalLRTVolume3D::get_max_debug_cells);
	ClassDB::bind_method(D_METHOD("get_bake_status"), &LocalLRTVolume3D::get_bake_status);
	ClassDB::bind_method(D_METHOD("get_field_summary"), &LocalLRTVolume3D::get_field_summary);
	ClassDB::bind_method(D_METHOD("sample_nearest_surface", "local_position"), &LocalLRTVolume3D::sample_nearest_surface);
	ClassDB::bind_method(D_METHOD("sample_sdf", "local_position"), &LocalLRTVolume3D::sample_sdf);
	ClassDB::bind_method(D_METHOD("sample_surface_color", "local_position"), &LocalLRTVolume3D::sample_surface_color);
	ClassDB::bind_method(D_METHOD("sample_first_bounce", "local_position"), &LocalLRTVolume3D::sample_first_bounce);
	ClassDB::bind_method(D_METHOD("sample_sky_visibility", "local_position", "local_normal"), &LocalLRTVolume3D::sample_sky_visibility);
	ClassDB::bind_method(D_METHOD("bake"), &LocalLRTVolume3D::bake);
	ClassDB::bind_method(D_METHOD("clear"), &LocalLRTVolume3D::clear);
	ClassDB::bind_method(D_METHOD("inject_first_bounce"), &LocalLRTVolume3D::inject_first_bounce);
	ClassDB::bind_method(D_METHOD("propagate", "iterations"), &LocalLRTVolume3D::propagate, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("clear_lighting"), &LocalLRTVolume3D::clear_lighting);
	ClassDB::bind_method(D_METHOD("set_lighting_enabled", "enabled"), &LocalLRTVolume3D::set_lighting_enabled);
	ClassDB::bind_method(D_METHOD("is_lighting_enabled"), &LocalLRTVolume3D::is_lighting_enabled);
	ClassDB::bind_method(D_METHOD("set_indirect_only", "enabled"), &LocalLRTVolume3D::set_indirect_only);
	ClassDB::bind_method(D_METHOD("is_indirect_only"), &LocalLRTVolume3D::is_indirect_only);
	ClassDB::bind_method(D_METHOD("set_propagation_iterations", "iterations"), &LocalLRTVolume3D::set_propagation_iterations);
	ClassDB::bind_method(D_METHOD("get_propagation_iterations"), &LocalLRTVolume3D::get_propagation_iterations);
	ClassDB::bind_method(D_METHOD("set_bounce_feedback", "feedback"), &LocalLRTVolume3D::set_bounce_feedback);
	ClassDB::bind_method(D_METHOD("get_bounce_feedback"), &LocalLRTVolume3D::get_bounce_feedback);
	ClassDB::bind_method(D_METHOD("set_sky_energy", "energy"), &LocalLRTVolume3D::set_sky_energy);
	ClassDB::bind_method(D_METHOD("get_sky_energy"), &LocalLRTVolume3D::get_sky_energy);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size", PROPERTY_HINT_NONE, "suffix:m"), "set_volume_size", "get_volume_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sdf_resolution", PROPERTY_HINT_RANGE, "8,256,1,suffix:px"), "set_sdf_resolution", "get_sdf_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "color_resolution", PROPERTY_HINT_RANGE, "8,256,1,suffix:px"), "set_color_resolution", "get_color_resolution");
	ADD_GROUP("Lighting", "lighting_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lighting_enabled"), "set_lighting_enabled", "is_lighting_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lighting_indirect_only"), "set_indirect_only", "is_indirect_only");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_iterations", PROPERTY_HINT_RANGE, "1,64,1"), "set_propagation_iterations", "get_propagation_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bounce_feedback", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_bounce_feedback", "get_bounce_feedback");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_sky_energy", "get_sky_energy");
	ADD_GROUP("Debug", "debug_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Disabled,Distance,Inside/Outside,Surface Color"), "set_debug_mode", "get_debug_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_slice_axis", PROPERTY_HINT_ENUM, "X,Y,Z"), "set_debug_slice_axis", "get_debug_slice_axis");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_slice_position", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_debug_slice_position", "get_debug_slice_position");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_max_cells", PROPERTY_HINT_RANGE, "1,1048576,1,or_greater"), "set_max_debug_cells", "get_max_debug_cells");
	ADD_SIGNAL(MethodInfo("bake_completed", PropertyInfo(Variant::STRING, "status")));
	ADD_SIGNAL(MethodInfo("lighting_updated", PropertyInfo(Variant::STRING, "status")));

	BIND_ENUM_CONSTANT(DEBUG_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_DISTANCE);
	BIND_ENUM_CONSTANT(DEBUG_INSIDE_OUTSIDE);
	BIND_ENUM_CONSTANT(DEBUG_SURFACE_COLOR);
}

LocalLRTVolume3D::~LocalLRTVolume3D() {
	LRTRuntime::clear(get_instance_id());
	_clear_gpu_resources();
}

