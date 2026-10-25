/**************************************************************************/
/*  lrt_geometry_baker.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "lrt_geometry_baker.h"

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/io/marshalls.h"
#include "core/math/triangle_mesh.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/texture.h"

namespace {

struct BakeMaterial {
	Ref<BaseMaterial3D> material;
	Ref<Image> albedo;
	Ref<Image> emission;
	Ref<Image> metallic;
	bool orm = false;

	static Error read_texture(const Ref<Texture2D> &p_texture, Ref<Image> &r_image) {
		if (p_texture.is_null()) {
			return OK;
		}
		Ref<Image> image = p_texture->get_image();
		ERR_FAIL_COND_V_MSG(image.is_null() || image->is_empty(), ERR_CANT_ACQUIRE_RESOURCE, "LRT cannot read a material texture.");
		r_image = image->duplicate();
		if (r_image->is_compressed()) {
			const Error error = r_image->decompress();
			ERR_FAIL_COND_V_MSG(error != OK, error, "LRT cannot decompress a material texture.");
		}
		return OK;
	}

	Error prepare(const Ref<Material> &p_material) {
		material = p_material;
		if (p_material.is_null()) {
			// A missing material uses the engine's default StandardMaterial3D.
			Ref<StandardMaterial3D> default_material;
			default_material.instantiate();
			material = default_material;
		}
		ERR_FAIL_COND_V_MSG(material.is_null(), ERR_UNAVAILABLE, "LRT CPU material baking requires StandardMaterial3D or ORMMaterial3D; ShaderMaterial is not supported yet.");
		orm = Object::cast_to<ORMMaterial3D>(material.ptr()) != nullptr;
		ERR_FAIL_COND_V_MSG(material->get_next_pass().is_valid() ||
						material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED ||
						material->get_blend_mode() != BaseMaterial3D::BLEND_MODE_MIX ||
						material->get_billboard_mode() != BaseMaterial3D::BILLBOARD_DISABLED ||
						material->is_grow_enabled() ||
						material->get_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE) ||
						material->get_flag(BaseMaterial3D::FLAG_FIXED_SIZE) ||
						material->get_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR) ||
						material->get_flag(BaseMaterial3D::FLAG_UV2_USE_TRIPLANAR) ||
						material->get_flag(BaseMaterial3D::FLAG_ALBEDO_TEXTURE_MSDF) ||
						material->get_feature(BaseMaterial3D::FEATURE_REFRACTION) ||
						material->get_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING) ||
						material->get_feature(BaseMaterial3D::FEATURE_DETAIL) ||
						material->get_feature(BaseMaterial3D::FEATURE_PARTICLES_ANIMATION),
				ERR_UNAVAILABLE, "LRT material baking requires opaque, undeformed UV-mapped materials without extra passes or detail layers.");
		Error error = read_texture(material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO), albedo);
		if (error != OK) {
			return error;
		}
		if (material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
			error = read_texture(material->get_texture(BaseMaterial3D::TEXTURE_EMISSION), emission);
			if (error != OK) {
				return error;
			}
		}
		const BaseMaterial3D::TextureParam metallic_param = orm ? BaseMaterial3D::TEXTURE_ORM : BaseMaterial3D::TEXTURE_METALLIC;
		return read_texture(material->get_texture(metallic_param), metallic);
	}

	Color texel(const Ref<Image> &p_image, int p_x, int p_y, bool p_srgb) const {
		if (material->get_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT)) {
			p_x = Math::posmod(p_x, p_image->get_width());
			p_y = Math::posmod(p_y, p_image->get_height());
		} else {
			p_x = CLAMP(p_x, 0, p_image->get_width() - 1);
			p_y = CLAMP(p_y, 0, p_image->get_height() - 1);
		}
		const Color color = p_image->get_pixel(p_x, p_y);
		return p_srgb ? color.srgb_to_linear() : color;
	}

	Color texture(const Ref<Image> &p_image, const Vector2 &p_uv, bool p_srgb) const {
		const Vector2 position = p_uv * Vector2(p_image->get_width(), p_image->get_height());
		if (material->get_texture_filter() == BaseMaterial3D::TEXTURE_FILTER_NEAREST ||
				material->get_texture_filter() == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS ||
				material->get_texture_filter() == BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC) {
			return texel(p_image, int(Math::floor(position.x)), int(Math::floor(position.y)), p_srgb);
		}
		const Vector2 shifted = position - Vector2(0.5, 0.5);
		const Vector2i cell(int(Math::floor(shifted.x)), int(Math::floor(shifted.y)));
		const Vector2 fraction = shifted - Vector2(cell);
		const Color top = texel(p_image, cell.x, cell.y, p_srgb).lerp(texel(p_image, cell.x + 1, cell.y, p_srgb), fraction.x);
		const Color bottom = texel(p_image, cell.x, cell.y + 1, p_srgb).lerp(texel(p_image, cell.x + 1, cell.y + 1, p_srgb), fraction.x);
		return top.lerp(bottom, fraction.y);
	}

	void sample(const Vector2 &p_uv, const Vector2 &p_uv2, const Color &p_vertex_color, Color &r_albedo, Color &r_emission) const {
		const Vector3 uv_scale = material->get_uv1_scale();
		const Vector3 uv_offset = material->get_uv1_offset();
		const Vector2 uv = p_uv * Vector2(uv_scale.x, uv_scale.y) + Vector2(uv_offset.x, uv_offset.y);
		r_albedo = material->get_albedo().srgb_to_linear();
		if (albedo.is_valid()) {
			r_albedo *= texture(albedo, uv, true);
		}
		if (material->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR)) {
			r_albedo *= p_vertex_color;
		}
		float metal = orm ? 1.0f : material->get_metallic();
		if (metallic.is_valid()) {
			const Color value = texture(metallic, uv, false);
			if (orm) {
				metal = value.b;
			} else {
				switch (material->get_metallic_texture_channel()) {
					case BaseMaterial3D::TEXTURE_CHANNEL_RED:
						metal *= value.r;
						break;
					case BaseMaterial3D::TEXTURE_CHANNEL_GREEN:
						metal *= value.g;
						break;
					case BaseMaterial3D::TEXTURE_CHANNEL_BLUE:
						metal *= value.b;
						break;
					case BaseMaterial3D::TEXTURE_CHANNEL_ALPHA:
						metal *= value.a;
						break;
					case BaseMaterial3D::TEXTURE_CHANNEL_GRAYSCALE:
						metal *= (value.r + value.g + value.b) / 3.0f;
						break;
					case BaseMaterial3D::TEXTURE_CHANNEL_MAX:
						break;
				}
			}
		}
		r_albedo *= 1.0f - CLAMP(metal, 0.0f, 1.0f);
		r_albedo.a = 1.0f;

		r_emission = Color(0, 0, 0);
		if (!material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
			return;
		}
		r_emission = material->get_emission().srgb_to_linear();
		if (emission.is_valid()) {
			Vector2 emission_uv = uv;
			if (material->get_flag(BaseMaterial3D::FLAG_EMISSION_ON_UV2)) {
				const Vector3 scale = material->get_uv2_scale();
				const Vector3 offset = material->get_uv2_offset();
				emission_uv = p_uv2 * Vector2(scale.x, scale.y) + Vector2(offset.x, offset.y);
			}
			const Color value = texture(emission, emission_uv, true);
			r_emission = material->get_emission_operator() == BaseMaterial3D::EMISSION_OP_ADD ? r_emission + value : r_emission * value;
		}
		if (emission.is_null() && material->get_emission_operator() == BaseMaterial3D::EMISSION_OP_MULTIPLY) {
			r_emission = Color(0, 0, 0);
		}
		float energy = material->get_emission_energy_multiplier();
		if (GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units")) {
			energy *= material->get_emission_intensity();
		}
		r_emission *= energy;
	}
};

struct FaceAttributes {
	Vector2 uv[3];
	Vector2 uv2[3];
	Color color[3] = { Color(1, 1, 1), Color(1, 1, 1), Color(1, 1, 1) };
	int material = 0;
};

struct Geometry {
	Ref<TriangleMesh> mesh;
	Vector<FaceAttributes> attributes;
	Vector<BakeMaterial> materials;

	Error prepare(MeshInstance3D *p_instance) {
		const Ref<Mesh> source = p_instance->get_mesh();
		ERR_FAIL_COND_V_MSG(source.is_null(), ERR_INVALID_DATA, "LRT cannot bake an instance without a mesh.");
		ERR_FAIL_COND_V_MSG(p_instance->get_material_overlay().is_valid() || p_instance->get_skin().is_valid(),
				ERR_UNAVAILABLE, "LRT Color SDF baking does not support material overlays or skinned meshes.");
		for (int shape = 0; shape < source->get_blend_shape_count(); shape++) {
			ERR_FAIL_COND_V_MSG(p_instance->get_blend_shape_value(shape) != 0, ERR_UNAVAILABLE, "LRT Color SDF baking requires undeformed mesh geometry.");
		}

		Vector<Vector3> faces;
		for (int surface = 0; surface < source->get_surface_count(); surface++) {
			if (source->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			BakeMaterial material;
			Error error = material.prepare(p_instance->get_active_material(surface));
			if (error != OK) {
				return error;
			}
			const int material_index = materials.size();
			materials.push_back(material);
			const Array arrays = source->surface_get_arrays(surface);
			const Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
			const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
			const Vector<Vector2> uv = arrays[Mesh::ARRAY_TEX_UV];
			const Vector<Vector2> uv2 = arrays[Mesh::ARRAY_TEX_UV2];
			const Vector<Color> colors = arrays[Mesh::ARRAY_COLOR];
			ERR_FAIL_COND_V_MSG((material.albedo.is_valid() || material.metallic.is_valid() ||
										(material.emission.is_valid() && !material.material->get_flag(BaseMaterial3D::FLAG_EMISSION_ON_UV2))) &&
							uv.is_empty(),
					ERR_INVALID_DATA, "LRT material textures require UV coordinates.");
			ERR_FAIL_COND_V_MSG(material.emission.is_valid() && material.material->get_flag(BaseMaterial3D::FLAG_EMISSION_ON_UV2) && uv2.is_empty(),
					ERR_INVALID_DATA, "LRT emission on UV2 requires UV2 coordinates.");
			const int count = indices.is_empty() ? vertices.size() : indices.size();
			ERR_FAIL_COND_V(count % 3 != 0, ERR_INVALID_DATA);
			for (int face = 0; face < count; face += 3) {
				FaceAttributes values;
				values.material = material_index;
				for (int corner = 0; corner < 3; corner++) {
					const int index = indices.is_empty() ? face + corner : indices[face + corner];
					ERR_FAIL_INDEX_V(index, vertices.size(), ERR_INVALID_DATA);
					ERR_FAIL_COND_V(!vertices[index].is_finite(), ERR_INVALID_DATA);
					faces.push_back(vertices[index]);
					if (!uv.is_empty()) {
						values.uv[corner] = uv[index];
					}
					if (!uv2.is_empty()) {
						values.uv2[corner] = uv2[index];
					}
					if (!colors.is_empty()) {
						values.color[corner] = material.material->get_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR) ? colors[index].srgb_to_linear() : colors[index];
					}
				}
				attributes.push_back(values);
			}
		}
		ERR_FAIL_COND_V_MSG(faces.is_empty(), ERR_INVALID_DATA, "LRT needs triangle geometry to bake.");
		mesh.instantiate();
		mesh->create(faces);

		struct Edge {
			int count = 0;
			int orientation = 0;
		};
		HashMap<uint64_t, Edge> edges;
		for (const TriangleMesh::Triangle &triangle : mesh->get_triangles()) {
			const Vector<Vector3> &vertices = mesh->get_vertices();
			ERR_FAIL_COND_V_MSG(Face3(vertices[triangle.indices[0]], vertices[triangle.indices[1]], vertices[triangle.indices[2]]).is_degenerate(),
					ERR_INVALID_DATA, "LRT Color SDF requires nondegenerate triangles at the engine BVH's 0.0001 object-unit precision.");
			for (int edge = 0; edge < 3; edge++) {
				const int from = triangle.indices[edge];
				const int to = triangle.indices[(edge + 1) % 3];
				const uint64_t key = (uint64_t(MIN(from, to)) << 32) | uint32_t(MAX(from, to));
				Edge &value = edges[key];
				value.count++;
				value.orientation += from < to ? 1 : -1;
			}
		}
		for (const KeyValue<uint64_t, Edge> &edge : edges) {
			ERR_FAIL_COND_V_MSG(edge.value.count != 2 || edge.value.orientation != 0,
					ERR_INVALID_DATA, "LRT Color SDF requires a closed, consistently oriented manifold mesh for its distance sign.");
		}
		return OK;
	}

	Face3 face(int p_index) const {
		const TriangleMesh::Triangle &triangle = mesh->get_triangles()[p_index];
		const Vector<Vector3> &vertices = mesh->get_vertices();
		return Face3(vertices[triangle.indices[0]], vertices[triangle.indices[1]], vertices[triangle.indices[2]]);
	}

	static real_t aabb_distance_squared(const AABB &p_bounds, const Vector3 &p_point) {
		const Vector3 delta = (p_bounds.position - p_point).maxf(0) + (p_point - p_bounds.get_end()).maxf(0);
		return delta.length_squared();
	}

	void nearest(int p_node, const Vector3 &p_point, real_t &r_distance_squared, int &r_face, Vector3 &r_closest) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (aabb_distance_squared(node.aabb, p_point) > r_distance_squared) {
			return;
		}
		if (node.face_index >= 0) {
			const Vector3 point = face(node.face_index).get_closest_point_to(p_point);
			const real_t distance_squared = point.distance_squared_to(p_point);
			if (distance_squared < r_distance_squared) {
				r_distance_squared = distance_squared;
				r_face = node.face_index;
				r_closest = point;
			}
			return;
		}
		const bool left_first = aabb_distance_squared(mesh->get_bvh()[node.left].aabb, p_point) <= aabb_distance_squared(mesh->get_bvh()[node.right].aabb, p_point);
		nearest(left_first ? node.left : node.right, p_point, r_distance_squared, r_face, r_closest);
		nearest(left_first ? node.right : node.left, p_point, r_distance_squared, r_face, r_closest);
	}

	static double edge(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_point) {
		return (double(p_b.y) - p_a.y) * (double(p_point.z) - p_a.z) - (double(p_b.z) - p_a.z) * (double(p_point.y) - p_a.y);
	}

	static bool covered(double p_edge, const Vector3 &p_a, const Vector3 &p_b) {
		// A half-open projected triangle counts shared edges exactly once.
		return p_edge > 0 || (p_edge == 0 && (p_b.z > p_a.z || (p_b.z == p_a.z && p_b.y < p_a.y)));
	}

	int winding(int p_node, const Vector3 &p_point) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (!node.aabb.intersects_ray(p_point, Vector3(1, 0, 0))) {
			return 0;
		}
		if (node.face_index < 0) {
			return winding(node.left, p_point) + winding(node.right, p_point);
		}
		const Face3 triangle = face(node.face_index);
		Vector3 a = triangle.vertex[0], b = triangle.vertex[1], c = triangle.vertex[2];
		double area = edge(a, b, c);
		if (area == 0) {
			return 0;
		}
		const int sign = area > 0 ? 1 : -1;
		if (area < 0) {
			SWAP(b, c);
			area = -area;
		}
		const double e0 = edge(b, c, p_point), e1 = edge(c, a, p_point), e2 = edge(a, b, p_point);
		if (!covered(e0, b, c) || !covered(e1, c, a) || !covered(e2, a, b)) {
			return 0;
		}
		const double x = (a.x * e0 + b.x * e1 + c.x * e2) / area;
		return x >= p_point.x ? sign : 0;
	}

	void sample_material(int p_face, const Vector3 &p_closest, Color &r_albedo, Color &r_emission) const {
		const Face3 triangle = face(p_face);
		const Vector3 u = triangle.vertex[1] - triangle.vertex[0];
		const Vector3 v = triangle.vertex[2] - triangle.vertex[0];
		const Vector3 point = p_closest - triangle.vertex[0];
		const real_t denominator = u.dot(u) * v.dot(v) - u.dot(v) * u.dot(v);
		const real_t b = (v.dot(v) * point.dot(u) - u.dot(v) * point.dot(v)) / denominator;
		const real_t c = (u.dot(u) * point.dot(v) - u.dot(v) * point.dot(u)) / denominator;
		const real_t a = 1 - b - c;
		const FaceAttributes &values = attributes[p_face];
		materials[values.material].sample(values.uv[0] * a + values.uv[1] * b + values.uv[2] * c,
				values.uv2[0] * a + values.uv2[1] * b + values.uv2[2] * c,
				values.color[0] * a + values.color[1] * b + values.color[2] * c, r_albedo, r_emission);
	}
};

} // namespace

Error LRTGeometryBaker::bake_instance(MeshInstance3D *p_instance, int p_resolution, Ref<LRTInstanceGeometryData> &r_data, BakeStepFunc p_step) {
	ERR_FAIL_NULL_V(p_instance, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_resolution < 6, ERR_INVALID_PARAMETER);

	Geometry geometry;
	Error error = geometry.prepare(p_instance);
	if (error != OK) {
		return error;
	}
	const AABB mesh_bounds = geometry.mesh->get_aabb();
	const int longest_axis = mesh_bounds.get_longest_axis_index();
	const real_t cell_size = mesh_bounds.size[longest_axis] / (p_resolution - 5);
	ERR_FAIL_COND_V(cell_size <= 0, ERR_INVALID_DATA);
	Vector3i grid;
	for (int axis = 0; axis < 3; axis++) {
		grid[axis] = axis == longest_axis ? p_resolution : int(Math::ceil(double(mesh_bounds.size[axis]) / cell_size)) + 5;
	}
	int64_t count = grid.x;
	ERR_FAIL_COND_V(count > INT32_MAX / 4 / grid.y, ERR_OUT_OF_MEMORY);
	count *= grid.y;
	ERR_FAIL_COND_V(count > INT32_MAX / 4 / grid.z, ERR_OUT_OF_MEMORY);
	count *= grid.z;
	const AABB bounds(mesh_bounds.position - Vector3(1, 1, 1) * cell_size * 2, Vector3(grid - Vector3i(1, 1, 1)) * cell_size);
	const float distance_scale = bounds.size.length();

	Vector<uint8_t> distance;
	Vector<uint8_t> color;
	Vector<float> emission;
	ERR_FAIL_COND_V(distance.resize(count * 2) != OK || color.resize(count * 4) != OK, ERR_OUT_OF_MEMORY);
	bool has_emission = false;
	for (const BakeMaterial &material : geometry.materials) {
		has_emission |= material.material->get_feature(BaseMaterial3D::FEATURE_EMISSION);
	}
	if (has_emission) {
		ERR_FAIL_COND_V(emission.resize(count * 3) != OK, ERR_OUT_OF_MEMORY);
	}
	uint8_t *distance_write = distance.ptrw();
	uint8_t *color_write = color.ptrw();
	float *emission_write = emission.ptrw();
	const int root = geometry.mesh->get_bvh().size() - 1;
	for (int z = 0; z < grid.z; z++) {
		if (p_step && p_step(z, grid.z)) {
			return ERR_SKIP;
		}
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				const int64_t index = (int64_t(z) * grid.y + y) * grid.x + x;
				const Vector3 point = bounds.position + Vector3(x, y, z) * cell_size;
				real_t distance_squared = INFINITY;
				int face = -1;
				Vector3 closest;
				geometry.nearest(root, point, distance_squared, face, closest);
				ERR_FAIL_COND_V(face < 0, ERR_INVALID_DATA);
				const bool inside = geometry.winding(root, point) != 0;
				const real_t signed_distance = Math::sqrt(distance_squared) * (inside ? -1 : 1);
				const int quantized = CLAMP(int(Math::round(signed_distance / distance_scale * 32767.0)), -32767, 32767);
				encode_uint16(uint16_t(int16_t(quantized)), distance_write + index * 2);

				Color albedo;
				Color light;
				geometry.sample_material(face, closest, albedo, light);
				color_write[index * 4 + 0] = uint8_t(CLAMP(Math::round(albedo.r * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 1] = uint8_t(CLAMP(Math::round(albedo.g * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 2] = uint8_t(CLAMP(Math::round(albedo.b * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 3] = 255;
				if (has_emission) {
					emission_write[index * 3 + 0] = light.r;
					emission_write[index * 3 + 1] = light.g;
					emission_write[index * 3 + 2] = light.b;
				}
			}
		}
	}

	Dictionary data;
	data["bounds"] = bounds;
	data["grid_size"] = grid;
	data["distance_scale"] = distance_scale;
	data["distance"] = distance;
	data["color"] = color;
	data["emission"] = emission;
	data["source_mesh"] = p_instance->get_mesh();
	Array source_materials;
	for (int surface = 0; surface < p_instance->get_mesh()->get_surface_count(); surface++) {
		source_materials.push_back(p_instance->get_active_material(surface));
	}
	data["source_materials"] = source_materials;
	data["source_resolution"] = p_resolution;
	Ref<LRTInstanceGeometryData> baked;
	baked.instantiate();
	baked->set_data(data);
	ERR_FAIL_COND_V(!baked->has_data(), ERR_INVALID_DATA);
	r_data = baked;
	return OK;
}
