/**************************************************************************/
/*  lrt_cache.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
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
/* The above copyright notice and this permission notice shall be        */
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

#include "lrt_cache.h"

#include "core/io/marshalls.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/templates/hashfuncs.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace lrt {

namespace {

// Bump when the stored layout changes, so old files are ignored instead of misread.
constexpr uint32_t CACHE_FORMAT_VERSION = 3;
constexpr uint32_t SDF_ALGORITHM_VERSION = 2;
constexpr char CACHE_MAGIC[8] = { 'L', 'R', 'T', 'S', 'D', 'F', '0', '3' };
std::mutex shared_fields_mutex;
struct SharedGeometryFieldEntry {
	std::shared_ptr<const SdfGeometryField> field;
	uint64_t bytes = 0;
	uint64_t last_used = 0;
};
std::map<uint64_t, SharedGeometryFieldEntry> shared_fields;
std::map<uint64_t, std::weak_ptr<const SdfInstanceField>> shared_instance_fields;
uint64_t shared_fields_bytes = 0;
uint64_t shared_fields_use_counter = 0;

uint64_t shared_fields_budget_bytes() {
	const int budget_mb = MAX(16, int(GLOBAL_GET("rendering/global_illumination/lrt/cache/memory_budget_mb")));
	return uint64_t(budget_mb) * 1024ull * 1024ull;
}

void trim_shared_fields() {
	const uint64_t budget = shared_fields_budget_bytes();
	while (shared_fields_bytes > budget) {
		auto oldest = shared_fields.end();
		for (auto entry = shared_fields.begin(); entry != shared_fields.end(); ++entry) {
			// A field referenced by a live volume is not cache memory and cannot be reclaimed here.
			if (entry->second.field.use_count() != 1) {
				continue;
			}
			if (oldest == shared_fields.end() || entry->second.last_used < oldest->second.last_used) {
				oldest = entry;
			}
		}
		if (oldest == shared_fields.end()) {
			return;
		}
		shared_fields_bytes -= oldest->second.bytes;
		shared_fields.erase(oldest);
	}
}

String resolve_cache_directory() {
	const String directory = ProjectSettings::get_singleton()->is_using_datapack() ? "user://lrt_cache" : "res://.godot/lrt";
	const Error error = DirAccess::make_dir_recursive_absolute(directory);
	ERR_FAIL_COND_V_MSG(error != OK, String(), "Cannot create the LRT derived cache directory: " + directory);
	return directory;
}

template <typename T>
bool read_values(Ref<FileAccess> p_file, std::vector<T> &r_values) {
	const uint64_t count = p_file->get_32();
	const uint64_t bytes = count * sizeof(T);
	if (p_file->get_position() > p_file->get_length() || bytes > p_file->get_length() - p_file->get_position()) {
		return false;
	}
	r_values.resize(size_t(count));
	return bytes == 0 || p_file->get_buffer(reinterpret_cast<uint8_t *>(r_values.data()), bytes) == bytes;
}

template <typename T>
void store_values(Ref<FileAccess> p_file, const std::vector<T> &p_values) {
	p_file->store_32(uint32_t(p_values.size()));
	if (p_values.empty()) {
		return;
	}
	PackedByteArray bytes;
	bytes.resize(int(p_values.size() * sizeof(T)));
	memcpy(bytes.ptrw(), p_values.data(), bytes.size());
	p_file->store_buffer(bytes);
}

} // namespace

uint64_t texture_content_signature(RID p_texture, RSE::TextureType p_type) {
	if (p_texture.is_null()) {
		return 0;
	}
	Vector<Ref<Image>> images;
	switch (p_type) {
		case RSE::TEXTURE_TYPE_2D:
			images.push_back(RS::get_singleton()->texture_2d_get(p_texture));
			break;
		case RSE::TEXTURE_TYPE_LAYERED:
			images = RS::get_singleton()->texture_2d_layered_get(p_texture);
			break;
		case RSE::TEXTURE_TYPE_3D:
			images = RS::get_singleton()->texture_3d_get(p_texture);
			break;
	}
	uint32_t signature = hash_murmur3_one_32(p_type);
	signature = hash_murmur3_one_32(images.size(), signature);
	for (const Ref<Image> &image : images) {
		if (image.is_null()) {
			signature = hash_murmur3_one_32(0, signature);
			continue;
		}
		signature = hash_murmur3_one_32(image->get_width(), signature);
		signature = hash_murmur3_one_32(image->get_height(), signature);
		signature = hash_murmur3_one_32(image->get_format(), signature);
		signature = hash_murmur3_one_32(image->has_mipmaps(), signature);
		signature = hash_murmur3_one_32(Variant(image->get_data()).hash(), signature);
	}
	return hash_fmix32(signature);
}

uint64_t asset_signature(const std::vector<MeshTriangle> &p_triangles, int p_resolution) {
	// FNV-1a over geometry only: normals and material attributes do not change signed distance.
	// Two different assets with identical positions intentionally share the same specification.
	uint64_t hash = 1469598103934665603ull;
	for (const MeshTriangle &triangle : p_triangles) {
		for (const Vec3 &position : triangle.position) {
			const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&position);
			for (size_t i = 0; i < sizeof(Vec3); i++) {
				hash ^= uint64_t(bytes[i]);
				hash *= 1099511628211ull;
			}
		}
	}
	hash ^= uint64_t(uint32_t(p_resolution));
	hash *= 1099511628211ull;
	hash ^= uint64_t(SDF_ALGORITHM_VERSION);
	hash *= 1099511628211ull;
	return hash;
}

String asset_cache_directory() {
	// Resolved once: the answer cannot change while the process runs.
	static String directory = resolve_cache_directory();
	return directory;
}

String asset_cache_path(uint64_t p_signature) {
	return asset_cache_directory().path_join(vformat("asset_%016x.sdf", p_signature));
}

String packed_asset_path(uint64_t p_signature) {
	return vformat("res://.godot/lrt_export/asset_%016x.sdf", p_signature);
}

bool decode_multimesh_capture(const Dictionary &p_capture, const std::shared_ptr<const std::vector<MeshTriangle>> &p_triangles,
		const std::vector<int> &p_triangle_surfaces, const std::vector<MeshDrawSurface> &p_draw_surfaces,
		std::shared_ptr<const std::vector<MeshCopy>> &r_copies, String &r_error) {
	const PackedByteArray buffer = p_capture.get("instance_buffer", PackedByteArray());
	const PackedByteArray commands = p_capture.get("instance_commands", PackedByteArray());
	const int stride = p_capture.get("instance_stride", 0);
	const int visible = p_capture.get("instance_count", 0);
	const bool format_2d = p_capture.get("instance_format_2d", false);
	if (stride < (format_2d ? 8 : 12) || buffer.size() % (stride * int(sizeof(float))) != 0 ||
			visible < 0 || visible > buffer.size() / (stride * int(sizeof(float))) ||
			!p_triangles || p_triangle_surfaces.size() != p_triangles->size()) {
		r_error = "Invalid geometry instance buffer layout.";
		return false;
	}
	const int allocated = buffer.size() / (stride * int(sizeof(float)));
	const bool indirect = p_capture.has("instance_commands");
	const int command_stride = p_capture.get("instance_command_stride", 0);
	if (indirect && (command_stride < 5 || commands.size() != int64_t(p_draw_surfaces.size()) * command_stride * sizeof(uint32_t))) {
		r_error = "Invalid geometry indirect command layout.";
		return false;
	}
	struct DrawRange {
		uint32_t first_instance = 0;
		uint32_t instance_count = 0;
		uint32_t first_triangle = 0;
		uint32_t triangle_count = 0;
	};
	std::vector<DrawRange> ranges(p_draw_surfaces.size());
	std::vector<bool> contributing_surface(ranges.size(), false);
	for (int surface : p_triangle_surfaces) {
		if (surface < 0 || size_t(surface) >= ranges.size()) {
			r_error = "Invalid geometry surface mapping.";
			return false;
		}
		contributing_surface[surface] = true;
	}
	for (size_t surface = 0; surface < ranges.size(); surface++) {
		DrawRange &range = ranges[surface];
		const int elements = p_draw_surfaces[surface].elements;
		if (!contributing_surface[surface]) {
			continue;
		}
		range.instance_count = visible;
		range.triangle_count = elements / 3;
		if (!indirect) {
			continue;
		}
		const uint8_t *command = commands.ptr() + surface * command_stride * sizeof(uint32_t);
		const bool indexed = p_draw_surfaces[surface].indexed;
		const uint32_t count = decode_uint32(command);
		const uint32_t first = decode_uint32(command + 8);
		range.instance_count = decode_uint32(command + 4);
		range.first_instance = decode_uint32(command + (indexed ? 16 : 12));
		if (uint64_t(first) + count > uint64_t(elements) || first % 3 != 0 ||
				uint64_t(range.first_instance) + range.instance_count > uint64_t(allocated) ||
				(indexed && decode_uint32(command + 12) != 0)) {
			r_error = "Indirect geometry capture requires triangle-aligned draws within the source mesh and instance buffer.";
			return false;
		}
		range.first_triangle = first / 3;
		range.triangle_count = count / 3;
	}
	auto copies = std::make_shared<std::vector<MeshCopy>>();
	std::map<std::vector<int>, std::shared_ptr<const std::vector<MeshTriangle>>> subsets;
	const int count = indirect ? allocated : visible;
	std::vector<int> draw_key;
	draw_key.reserve(ranges.size() * 3);
	for (int instance = 0; instance < count; instance++) {
		bool full_geometry = true;
		size_t selected_count = 0;
		if (indirect) {
			draw_key.clear();
			for (size_t surface = 0; surface < ranges.size(); surface++) {
				if (!contributing_surface[surface]) {
					continue;
				}
				const DrawRange &range = ranges[surface];
				const bool drawn = range.triangle_count > 0 && uint32_t(instance) >= range.first_instance &&
						uint32_t(instance) - range.first_instance < range.instance_count;
				full_geometry = full_geometry && drawn && range.first_triangle == 0 &&
						range.triangle_count == uint32_t(p_draw_surfaces[surface].elements / 3);
				if (drawn) {
					draw_key.push_back(int(surface));
					draw_key.push_back(int(range.first_triangle));
					draw_key.push_back(int(range.triangle_count));
					selected_count += range.triangle_count;
				}
			}
			if (draw_key.empty()) {
				continue;
			}
		}
		const uint8_t *data = buffer.ptr() + int64_t(instance) * stride * sizeof(float);
		auto value = [data](int p_index) { return double(decode_float(data + p_index * sizeof(float))); };
		MeshCopy copy;
		copy.transform.origin = Vec3(value(3), value(7), format_2d ? 0.0 : value(11));
		copy.transform.basis_x = Vec3(value(0), value(4), format_2d ? 0.0 : value(8));
		copy.transform.basis_y = Vec3(value(1), value(5), format_2d ? 0.0 : value(9));
		copy.transform.basis_z = format_2d ? Vec3(0, 0, 1) : Vec3(value(2), value(6), value(10));
		const double determinant = dot(copy.transform.basis_x, cross(copy.transform.basis_y, copy.transform.basis_z));
		// Inactive GPU particles have a zero basis and an infinite translation.
		if (determinant == 0.0) {
			continue;
		}
		if (!std::isfinite(determinant) || !std::isfinite(copy.transform.origin.x) ||
				!std::isfinite(copy.transform.origin.y) || !std::isfinite(copy.transform.origin.z)) {
			r_error = "Non-finite geometry instance transform.";
			return false;
		}
		if (full_geometry) {
			copy.triangles = p_triangles;
		} else {
			auto found = subsets.find(draw_key);
			if (found == subsets.end()) {
				auto triangles = std::make_shared<std::vector<MeshTriangle>>();
				triangles->reserve(selected_count);
				std::vector<uint32_t> surface_triangle(ranges.size(), 0);
				for (size_t triangle = 0; triangle < p_triangles->size(); triangle++) {
					const int surface = p_triangle_surfaces[triangle];
					const DrawRange &range = ranges[surface];
					const uint32_t local_triangle = surface_triangle[surface]++;
					if (uint32_t(instance) >= range.first_instance && uint32_t(instance) - range.first_instance < range.instance_count &&
							local_triangle >= range.first_triangle && local_triangle - range.first_triangle < range.triangle_count) {
						triangles->push_back((*p_triangles)[triangle]);
					}
				}
				found = subsets.emplace(draw_key, triangles).first;
			}
			copy.triangles = found->second;
		}
		copies->push_back(std::move(copy));
	}
	r_copies = copies;
	return true;
}

bool decode_particle_capture(const Dictionary &p_capture, const std::shared_ptr<const std::vector<MeshTriangle>> &p_triangles,
		const std::vector<int> &p_triangle_surfaces, const std::vector<MeshDrawSurface> &p_draw_surfaces,
		const std::vector<MeshTriangleSkin> &p_skin, std::shared_ptr<const std::vector<MeshCopy>> &r_copies, String &r_error) {
	const int steps = p_capture.get("particle_trail_steps", 0);
	const int count = p_capture.get("instance_count", 0);
	const PackedByteArray buffer = p_capture.get("instance_buffer", PackedByteArray());
	const PackedByteArray trail_buffer = p_capture.get("particle_trail_buffer", buffer);
	if (steps < 1 || count < 0 || count % steps != 0 || buffer.size() != int64_t(count) * 20 * sizeof(float) ||
			trail_buffer.size() != buffer.size() || !p_triangles || p_skin.size() != p_triangles->size() || p_triangle_surfaces.size() != p_triangles->size()) {
		r_error = "Invalid particle geometry snapshot layout.";
		return false;
	}
	const Transform3D world_to_local = bool(p_capture.get("particle_local_coords", true)) ? Transform3D() : Transform3D(p_capture["transform"]).affine_inverse();
	PrimitiveTransform local;
	local.origin = Vec3(world_to_local.origin.x, world_to_local.origin.y, world_to_local.origin.z);
	const Vector3 x = world_to_local.basis.get_column(0);
	const Vector3 y = world_to_local.basis.get_column(1);
	const Vector3 z = world_to_local.basis.get_column(2);
	local.basis_x = Vec3(x.x, x.y, x.z);
	local.basis_y = Vec3(y.x, y.y, y.z);
	local.basis_z = Vec3(z.x, z.y, z.z);
	size_t rigid_count = 0;
	bool has_trails = false;
	for (size_t triangle = 0; triangle < p_triangles->size(); triangle++) {
		const int surface = p_triangle_surfaces[triangle];
		if (surface < 0 || size_t(surface) >= p_draw_surfaces.size()) {
			r_error = "Invalid particle surface mapping.";
			return false;
		}
		if (p_draw_surfaces[surface].particle_rigid) {
			rigid_count++;
		}
		has_trails = has_trails || p_draw_surfaces[surface].particle_trails;
	}
	auto copies = std::make_shared<std::vector<MeshCopy>>();
	if (rigid_count > 0) {
		std::shared_ptr<const std::vector<MeshTriangle>> rigid = p_triangles;
		std::vector<int> rigid_surfaces;
		if (rigid_count != p_triangles->size()) {
			auto subset = std::make_shared<std::vector<MeshTriangle>>();
			subset->reserve(rigid_count);
			for (size_t triangle = 0; triangle < p_triangles->size(); triangle++) {
				const int surface = p_triangle_surfaces[triangle];
				if (p_draw_surfaces[surface].particle_rigid) {
					subset->push_back((*p_triangles)[triangle]);
					rigid_surfaces.push_back(surface);
				}
			}
			rigid = subset;
		}
		std::shared_ptr<const std::vector<MeshCopy>> rigid_copies;
		if (!decode_multimesh_capture(p_capture, rigid, rigid_count == p_triangles->size() ? p_triangle_surfaces : rigid_surfaces, p_draw_surfaces, rigid_copies, r_error)) {
			return false;
		}
		for (const MeshCopy &source : *rigid_copies) {
			MeshCopy copy = source;
			copy.transform = local * source.transform;
			copies->push_back(std::move(copy));
		}
	}
	if (has_trails) {
		for (int particle = 0; particle < count / steps; particle++) {
			auto triangles = std::make_shared<std::vector<MeshTriangle>>();
			for (size_t index = 0; index < p_triangles->size(); index++) {
				if (!p_draw_surfaces[p_triangle_surfaces[index]].particle_trails) {
					continue;
				}
				MeshTriangle triangle = (*p_triangles)[index];
				bool active = true;
				for (int corner = 0; corner < 3; corner++) {
					Vec3 position;
					for (int influence = 0; influence < 4; influence++) {
						const double weight = p_skin[index].weights[corner][influence];
						if (influence > 0 && weight <= 0.001) {
							continue;
						}
						const int bone = p_skin[index].bones[corner][influence];
						if (bone < 0 || bone >= steps) {
							r_error = "Particle trail bone is outside the render snapshot.";
							return false;
						}
						const uint8_t *data = trail_buffer.ptr() + int64_t(particle * steps + bone) * 20 * sizeof(float);
						const Vec3 &source = triangle.position[corner];
						Vec3 point;
						for (int axis = 0; axis < 3; axis++) {
							const uint8_t *row = data + axis * 4 * sizeof(float);
							point[axis] = decode_float(row) * source.x + decode_float(row + 4) * source.y + decode_float(row + 8) * source.z + decode_float(row + 12);
							active = active && std::isfinite(point[axis]);
						}
						position = position + point * weight;
					}
					triangle.position[corner] = local.xform(position);
				}
				const Vec3 area = cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]);
				if (active && dot(area, area) > 0.0) {
					triangles->push_back(triangle);
				}
			}
			if (!triangles->empty()) {
				MeshCopy copy;
				copy.triangles = triangles;
				copies->push_back(std::move(copy));
			}
		}
	}
	r_copies = copies;
	return true;
}

uint64_t mesh_deformation_signature(MeshInstance3D *p_instance) {
	if (p_instance == nullptr) {
		return 0;
	}
	uint64_t signature = 0;
	for (int shape = 0; shape < p_instance->get_blend_shape_count(); shape++) {
		signature = hash_murmur3_one_64(Variant(p_instance->get_blend_shape_value(shape)).hash(), signature);
	}
	const Ref<SkinReference> skin = p_instance->get_skin_reference();
	if (skin.is_valid()) {
		for (const Transform3D &transform : skin->get_bone_transforms()) {
			signature = hash_murmur3_one_64(Variant(transform).hash(), signature);
		}
	}
	return signature;
}

bool mesh_triangles(const Ref<Mesh> &p_mesh, std::vector<MeshTriangle> &r_triangles, MeshInstance3D *p_instance, const Vector<int> *p_surfaces) {
	r_triangles.clear();
	if (p_mesh.is_null()) {
		return false;
	}
	Vector<Transform3D> bone_transforms;
	if (p_instance != nullptr) {
		const Ref<SkinReference> skin = p_instance->get_skin_reference();
		if (skin.is_valid()) {
			bone_transforms = skin->get_bone_transforms();
		}
	}
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		if (p_surfaces != nullptr && !p_surfaces->has(surface)) {
			continue;
		}
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(surface);
		if (arrays.size() < Mesh::ARRAY_MAX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY) {
			continue;
		}
		const PackedVector3Array base_points = arrays[Mesh::ARRAY_VERTEX];
		PackedVector3Array points = base_points;
		if (p_instance != nullptr && p_instance->get_blend_shape_count() > 0) {
			const Ref<ArrayMesh> array_mesh = p_mesh;
			ERR_FAIL_COND_V(array_mesh.is_null(), false);
			const Array shapes = array_mesh->surface_get_blend_shape_arrays(surface);
			for (int shape = 0; shape < shapes.size(); shape++) {
				const float weight = p_instance->get_blend_shape_value(shape);
				if (weight == 0.0f) {
					continue;
				}
				const Array shape_arrays = shapes[shape];
				const PackedVector3Array shape_points = shape_arrays[Mesh::ARRAY_VERTEX];
				ERR_FAIL_COND_V(shape_points.size() != points.size(), false);
				for (int vertex = 0; vertex < points.size(); vertex++) {
					const Vector3 delta = array_mesh->get_blend_shape_mode() == Mesh::BLEND_SHAPE_MODE_NORMALIZED ? shape_points[vertex] - base_points[vertex] : shape_points[vertex];
					points.set(vertex, points[vertex] + delta * weight);
				}
			}
		}
		const PackedInt32Array bones = arrays[Mesh::ARRAY_BONES];
		const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
		if (!bone_transforms.is_empty() && !bones.is_empty()) {
			const int influences = (p_mesh->surface_get_format(surface) & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS) ? 8 : 4;
			ERR_FAIL_COND_V(bones.size() != points.size() * influences || weights.size() != bones.size(), false);
			for (int vertex = 0; vertex < points.size(); vertex++) {
				const Vector3 source = points[vertex];
				Vector3 position;
				for (int influence = 0; influence < influences; influence++) {
					const int index = vertex * influences + influence;
					if (weights[index] == 0.0f) {
						continue;
					}
					ERR_FAIL_INDEX_V(bones[index], bone_transforms.size(), false);
					position += bone_transforms[bones[index]].xform(source) * weights[index];
				}
				points.set(vertex, position);
			}
		}
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const int count = indices.is_empty() ? points.size() : indices.size();
		for (int index = 0; index + 2 < count; index += 3) {
			MeshTriangle triangle;
			for (int vertex = 0; vertex < 3; vertex++) {
				const int source = indices.is_empty() ? index + vertex : indices[index + vertex];
				ERR_FAIL_INDEX_V(source, points.size(), false);
				const Vector3 position = points[source];
				triangle.position[vertex] = Vec3(position.x, position.y, position.z);
				triangle.color[vertex] = Vec3(1.0, 1.0, 1.0);
			}
			r_triangles.push_back(triangle);
		}
	}
	return !r_triangles.empty();
}

Dictionary prepare_mesh_asset(const Ref<Mesh> &p_mesh, int p_resolution) {
	Dictionary report;
	report["ok"] = false;
	std::vector<MeshTriangle> triangles;
	if (!mesh_triangles(p_mesh, triangles)) {
		report["error"] = "Mesh has no triangle surfaces.";
		return report;
	}
	p_resolution = MAX(8, p_resolution);
	const uint64_t signature = asset_signature(triangles, p_resolution);
	report["signature"] = int64_t(signature);
	report["path"] = asset_cache_path(signature);
	report["packed_path"] = packed_asset_path(signature);
	SdfGeometryField field;
	const bool loaded = load_asset_field(signature, field);
	if (!loaded) {
		const int threads = CLAMP(OS::get_singleton()->get_processor_count() - 1, 1, 16);
		const MeshSdfBakeResult baked = bake_mesh_sdf(build_triangle_mesh(std::move(triangles)), p_resolution, nullptr, threads);
		if (baked.error != MESH_SDF_BAKE_OK) {
			report["error"] = vformat("SDF preparation failed (error %d).", int(baked.error));
			return report;
		}
		field = baked.field;
	}
	// Export preparation must materialize the exact dependency, even after a process cache hit.
	if (!store_asset_field(signature, field)) {
		report["error"] = "Cannot write the prepared SDF dependency.";
		return report;
	}
	report["ok"] = true;
	report["loaded"] = loaded;
	report["bytes"] = int64_t(field.distance.size() * sizeof(int16_t));
	return report;
}

bool load_asset_field(uint64_t p_signature, SdfGeometryField &r_field) {
	const String packed_path = packed_asset_path(p_signature);
	Ref<FileAccess> file = FileAccess::open(FileAccess::exists(packed_path) ? packed_path : asset_cache_path(p_signature), FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	char magic[8] = {};
	file->get_buffer(reinterpret_cast<uint8_t *>(magic), 8);
	if (memcmp(magic, CACHE_MAGIC, 8) != 0 || file->get_32() != CACHE_FORMAT_VERSION) {
		return false;
	}
	SdfGeometryField field;
	for (int axis = 0; axis < 3; axis++) {
		field.min[axis] = file->get_double();
	}
	field.cell = file->get_double();
	field.distance_scale = file->get_double();
	for (int axis = 0; axis < 3; axis++) {
		field.size[axis] = int(file->get_32());
	}
	field.closed_shell_count = int(file->get_32());
	field.open_shell_count = int(file->get_32());
	field.surface_voxels = int(file->get_32());
	field.ray_queries = file->get_64();
	if (!read_values(file, field.distance) || file->get_error() != OK || field.distance.empty() ||
			field.size[0] <= 0 || field.size[1] <= 0 || field.size[2] <= 0 ||
			uint64_t(field.size[0]) * field.size[1] * field.size[2] != field.distance.size()) {
		return false;
	}
	r_field = field;
	return true;
}

bool store_asset_field(uint64_t p_signature, const SdfGeometryField &p_field) {
	if (p_field.distance.empty()) {
		return false;
	}
	const String path = asset_cache_path(p_signature);
	// Write beside the target and rename, so a crash cannot leave a half file that a later run
	// would read as a valid field.
	const String temporary = path + ".tmp";
	{
		Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE);
		if (file.is_null()) {
			return false;
		}
		file->store_buffer(reinterpret_cast<const uint8_t *>(CACHE_MAGIC), 8);
		file->store_32(CACHE_FORMAT_VERSION);
		for (int axis = 0; axis < 3; axis++) {
			file->store_double(p_field.min[axis]);
		}
		file->store_double(p_field.cell);
		file->store_double(p_field.distance_scale);
		for (int axis = 0; axis < 3; axis++) {
			file->store_32(uint32_t(p_field.size[axis]));
		}
		file->store_32(uint32_t(p_field.closed_shell_count));
		file->store_32(uint32_t(p_field.open_shell_count));
		file->store_32(uint32_t(p_field.surface_voxels));
		file->store_64(p_field.ray_queries);
		store_values(file, p_field.distance);
		if (file->get_error() != OK) {
			return false;
		}
	}
	Ref<DirAccess> dir = DirAccess::open(asset_cache_directory());
	if (dir.is_null()) {
		return false;
	}
	return dir->rename(temporary.get_file(), path.get_file()) == OK;
}

String instance_cache_path(uint64_t p_signature) {
	return asset_cache_directory().path_join(vformat("instance_%016x.lrt", p_signature));
}

String packed_instance_path(uint64_t p_signature) {
	return vformat("res://.godot/lrt_export/instance_%016x.lrt", p_signature);
}

bool load_instance_field(uint64_t p_signature, SdfInstanceField &r_field) {
	const String packed = packed_instance_path(p_signature);
	Ref<FileAccess> file = FileAccess::open(FileAccess::exists(packed) ? packed : instance_cache_path(p_signature), FileAccess::READ);
	if (file.is_null() || file->get_32() != CACHE_FORMAT_VERSION || file->get_64() != p_signature) {
		return false;
	}
	SdfInstanceField field;
	uint64_t count = 1;
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = int(file->get_32());
		if (field.color_size[axis] <= 0 || field.color_size[axis] > 1024) {
			return false;
		}
		count *= field.color_size[axis];
	}
	if (!read_values(file, field.albedo) || field.albedo.size() != count * 3 ||
			!read_values(file, field.emission) || (!field.emission.empty() && field.emission.size() != count * 3) || file->get_error() != OK) {
		return false;
	}
	r_field = std::move(field);
	return true;
}

bool store_instance_field(uint64_t p_signature, const SdfInstanceField &p_field) {
	const String path = instance_cache_path(p_signature);
	const String temporary = path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_32(CACHE_FORMAT_VERSION);
	file->store_64(p_signature);
	for (int axis = 0; axis < 3; axis++) {
		file->store_32(p_field.color_size[axis]);
	}
	store_values(file, p_field.albedo);
	store_values(file, p_field.emission);
	const Error error = file->get_error();
	file.unref();
	if (error != OK) {
		return false;
	}
	Ref<DirAccess> directory = DirAccess::open(asset_cache_directory());
	return directory.is_valid() && directory->rename(temporary.get_file(), path.get_file()) == OK;
}

std::shared_ptr<const SdfGeometryField> find_shared_asset_field(uint64_t p_signature) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_fields.find(p_signature);
	if (found == shared_fields.end()) {
		return nullptr;
	}
	found->second.last_used = ++shared_fields_use_counter;
	return found->second.field;
}

std::shared_ptr<const SdfGeometryField> share_asset_field(uint64_t p_signature, SdfGeometryField p_field) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_fields.find(p_signature);
	if (found != shared_fields.end()) {
		found->second.last_used = ++shared_fields_use_counter;
		return found->second.field;
	}
	std::shared_ptr<const SdfGeometryField> shared = std::make_shared<const SdfGeometryField>(std::move(p_field));
	SharedGeometryFieldEntry entry;
	entry.field = shared;
	entry.bytes = asset_field_bytes(*shared);
	entry.last_used = ++shared_fields_use_counter;
	shared_fields_bytes += entry.bytes;
	shared_fields[p_signature] = std::move(entry);
	trim_shared_fields();
	return shared;
}

uint64_t instance_field_cache_signature(uint64_t p_geometry_signature, uint64_t p_material_signature) {
	uint64_t hash = 1469598103934665603ull;
	for (const uint64_t value : { p_geometry_signature, p_material_signature }) {
		const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&value);
		for (size_t i = 0; i < sizeof(uint64_t); i++) {
			hash ^= uint64_t(bytes[i]);
			hash *= 1099511628211ull;
		}
	}
	return hash;
}

std::shared_ptr<const SdfInstanceField> find_shared_instance_field(uint64_t p_signature) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_instance_fields.find(p_signature);
	if (found == shared_instance_fields.end()) {
		return nullptr;
	}
	std::shared_ptr<const SdfInstanceField> shared = found->second.lock();
	if (!shared) {
		shared_instance_fields.erase(found);
	}
	return shared;
}

std::shared_ptr<const SdfInstanceField> share_instance_field(uint64_t p_signature, SdfInstanceField p_field) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_instance_fields.find(p_signature);
	if (found != shared_instance_fields.end()) {
		std::shared_ptr<const SdfInstanceField> shared = found->second.lock();
		if (shared) {
			return shared;
		}
	}
	std::shared_ptr<const SdfInstanceField> shared = std::make_shared<const SdfInstanceField>(std::move(p_field));
	shared_instance_fields[p_signature] = shared;
	if (shared_instance_fields.size() > 1024) {
		for (auto entry = shared_instance_fields.begin(); entry != shared_instance_fields.end();) {
			if (entry->second.expired()) {
				entry = shared_instance_fields.erase(entry);
			} else {
				++entry;
			}
		}
	}
	return shared;
}

void clear_shared_asset_fields() {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	shared_fields.clear();
	shared_instance_fields.clear();
	shared_fields_bytes = 0;
	shared_fields_use_counter = 0;
}

uint64_t asset_field_bytes(const SdfGeometryField &p_field) {
	return uint64_t(sizeof(SdfGeometryField)) + uint64_t(p_field.distance.capacity() * sizeof(int16_t));
}

} // namespace lrt
