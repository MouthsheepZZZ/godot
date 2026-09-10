#include "local_lrt_volume_3d.h"

#include "core/io/image.h"
#include "core/math/face3.h"
#include "core/math/geometry_3d.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/texture.h"

namespace {

constexpr float LRT_BIG_DISTANCE = 1e20f;

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
	_clear_debug();
	if (bake_status.is_empty()) {
		bake_status = "Not baked";
	}
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
	for (int index = 0; index < sdf_grid.surface.size(); index++) {
		surface_cells += sdf_grid.surface[index] != 0;
		inside_cells += sdf_grid.distance[index] < 0.0f;
	}
	for (uint8_t occupied : color_grid.surface) {
		color_cells += occupied != 0;
	}
	summary["surface_cells"] = surface_cells;
	summary["inside_cells"] = inside_cells;
	summary["color_cells"] = color_cells;
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

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		update_configuration_warnings();
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
	ClassDB::bind_method(D_METHOD("bake"), &LocalLRTVolume3D::bake);
	ClassDB::bind_method(D_METHOD("clear"), &LocalLRTVolume3D::clear);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size", PROPERTY_HINT_NONE, "suffix:m"), "set_volume_size", "get_volume_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sdf_resolution", PROPERTY_HINT_RANGE, "8,256,1,suffix:px"), "set_sdf_resolution", "get_sdf_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "color_resolution", PROPERTY_HINT_RANGE, "8,256,1,suffix:px"), "set_color_resolution", "get_color_resolution");
	ADD_GROUP("Debug", "debug_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Disabled,Distance,Inside/Outside,Surface Color"), "set_debug_mode", "get_debug_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_slice_axis", PROPERTY_HINT_ENUM, "X,Y,Z"), "set_debug_slice_axis", "get_debug_slice_axis");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_slice_position", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_debug_slice_position", "get_debug_slice_position");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_max_cells", PROPERTY_HINT_RANGE, "1,1048576,1,or_greater"), "set_max_debug_cells", "get_max_debug_cells");
	ADD_SIGNAL(MethodInfo("bake_completed", PropertyInfo(Variant::STRING, "status")));

	BIND_ENUM_CONSTANT(DEBUG_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_DISTANCE);
	BIND_ENUM_CONSTANT(DEBUG_INSIDE_OUTSIDE);
	BIND_ENUM_CONSTANT(DEBUG_SURFACE_COLOR);
}

