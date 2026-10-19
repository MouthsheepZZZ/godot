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

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/templates/hashfuncs.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace lrt {

std::vector<float> local_material_rgba(const LocalField &p_field, bool p_emission) {
	std::vector<float> pixels(p_field.material.size(), 0.0f);
	for (size_t probe = 0; probe < p_field.material.size() / 4; probe++) {
		const float *material = p_field.material.data() + probe * 4;
		const int blocked_count = int(material[1]);
		if (material[3] > 0.5f || blocked_count == 0) {
			continue;
		}
		const LocalRecord &record = p_field.get_local_record(probe);
		const Vec3 reflectance = record.get_reflectance();
		float *pixel = pixels.data() + probe * 4;
		for (int channel = 0; channel < 3; channel++) {
			// EmissionSH DC is WEIGHT * C0 times the sum of blocked-surface emission.
			pixel[channel] = p_emission ? float(record.emission[size_t(channel) * 4] / (WEIGHT * C0 * blocked_count)) : float(reflectance[channel]);
		}
		// A black occluder has no local record, but remains a valid surface diagnostic.
		pixel[3] = 1.0f;
	}
	return pixels;
}

std::vector<float> sdf_surface_cells(const CoarseGeometryField &p_field) {
	std::vector<float> cells;

	for (int z = 0; z < p_field.size[2]; z++) {
		for (int y = 0; y < p_field.size[1]; y++) {
			for (int x = 0; x < p_field.size[0]; x++) {
				const size_t index = size_t(x) + size_t(p_field.size[0]) * (size_t(y) + size_t(p_field.size[1]) * size_t(z));

				if ((p_field.cells[index] & ((1u << DIRECTION_COUNT) - 1u)) == 0) {
					continue;
				}
				cells.push_back(float(p_field.min.x + x * p_field.cell));
				cells.push_back(float(p_field.min.y + y * p_field.cell));
				cells.push_back(float(p_field.min.z + z * p_field.cell));
				cells.push_back(1.0f);
			}
		}
	}
	return cells;
}

namespace {

// Bump when the stored layout changes, so old files are ignored instead of misread.
constexpr uint32_t CACHE_FORMAT_VERSION = 5;
constexpr uint32_t INSTANCE_CACHE_FORMAT_VERSION = 6;
constexpr uint32_t SDF_ALGORITHM_VERSION = 6;
constexpr char CACHE_MAGIC[8] = { 'L', 'R', 'T', 'G', 'E', 'O', '0', '5' };
std::mutex shared_fields_mutex;
std::mutex capture_cache_mutex;
std::map<uint64_t, std::weak_ptr<const CoarseGeometryField>> shared_fields;
std::map<uint64_t, std::weak_ptr<const SurfaceMaterialField>> shared_instance_fields;

// The registry does not own fields. The last Volume releases both the allocation and
// its registry entry, rather than retaining unused assets up to an arbitrary budget.
template <typename T>
std::shared_ptr<const T> share_field(std::map<uint64_t, std::weak_ptr<const T>> &p_fields, uint64_t p_signature, T p_field) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = p_fields.find(p_signature);
	if (found != p_fields.end()) {
		std::shared_ptr<const T> shared = found->second.lock();
		if (shared) {
			return shared;
		}
	}
	std::shared_ptr<const T> shared(new T(std::move(p_field)), [&p_fields, p_signature](const T *p_value) {
		delete p_value;
		std::lock_guard<std::mutex> release_lock(shared_fields_mutex);
		const auto entry = p_fields.find(p_signature);
		// clear_shared_asset_fields() may have detached this allocation and a new
		// user may already have published a replacement with the same signature.
		if (entry != p_fields.end() && entry->second.expired()) {
			p_fields.erase(entry);
		}
	});
	p_fields[p_signature] = shared;
	return shared;
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

String static_voxel_capture_path(uint64_t p_signature) {
	return asset_cache_directory().path_join(vformat("capture_%016x.lrt", p_signature));
}

bool load_static_voxel_capture(uint64_t p_signature, Dictionary &r_images) {
	std::lock_guard<std::mutex> lock(capture_cache_mutex);
	const String packed = vformat("res://.godot/lrt_export/capture_%016x.lrt", p_signature);
	const String path = FileAccess::exists(packed) ? packed : static_voxel_capture_path(p_signature);
	if (!FileAccess::exists(path)) {
		return false;
	}
	Ref<FileAccess> file = FileAccess::open_compressed(path, FileAccess::READ);
	if (file.is_null() || file->get_32() != 0x4c565843 || file->get_32() != 1 || file->get_64() != p_signature) {
		return false;
	}
	const Variant data = file->get_var(false);
	if (data.get_type() != Variant::DICTIONARY || file->get_error() != OK) {
		return false;
	}
	r_images = data;
	return !r_images.is_empty();
}

bool store_static_voxel_capture(uint64_t p_signature, const Dictionary &p_images) {
	Dictionary images;
	for (const char *key : { "size", "material_size", "albedo_directions", "normal_size", "bounds", "transform",
			"albedo", "emission", "emission_aniso", "normal_bits" }) {
		if (!p_images.has(key)) {
			return false;
		}
		images[key] = p_images[key];
	}
	std::lock_guard<std::mutex> lock(capture_cache_mutex);
	Ref<FileAccess> file = FileAccess::open_compressed(static_voxel_capture_path(p_signature), FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	if (file.is_null()) {
		return false;
	}
	file->store_32(0x4c565843);
	file->store_32(1);
	file->store_64(p_signature);
	file->store_var(images, false);
	file->flush();
	return file->get_error() == OK;
}

// Native shadow invalidation still follows the rendered pose; GI voxels do not.
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

bool mesh_triangles(const Ref<Mesh> &p_mesh, std::vector<MeshTriangle> &r_triangles, const Vector<int> *p_surfaces) {
	r_triangles.clear();
	if (p_mesh.is_null()) {
		return false;
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
		const PackedVector3Array points = arrays[Mesh::ARRAY_VERTEX];
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

double geometry_cell_size() {
	return MAX(0.001, double(GLOBAL_GET("rendering/global_illumination/lrt/geometry/target_cell_size")));
}

int estimate_mesh_geometry_resolution(const std::vector<MeshTriangle> &p_triangles) {
	if (p_triangles.empty()) {
		return 1;
	}
	Vec3 low = p_triangles[0].position[0];
	Vec3 high = low;
	for (const MeshTriangle &triangle : p_triangles) {
		for (const Vec3 &point : triangle.position) {
			for (int axis = 0; axis < 3; axis++) {
				low[axis] = std::min(low[axis], point[axis]);
				high[axis] = std::max(high[axis], point[axis]);
			}
		}
	}
	const double longest = std::max({ high.x - low.x, high.y - low.y, high.z - low.z });
	return int(std::max(1.0, std::min(double(INT32_MAX), std::ceil(longest / geometry_cell_size()))));
}

Dictionary prepare_mesh_asset(const Ref<Mesh> &p_mesh, int p_resolution) {
	Dictionary report;
	report["ok"] = false;
	std::vector<MeshTriangle> triangles;
	if (!mesh_triangles(p_mesh, triangles)) {
		report["error"] = "Mesh has no triangle surfaces.";
		return report;
	}
	p_resolution = p_resolution > 0 ? p_resolution : estimate_mesh_geometry_resolution(triangles);
	report["resolution"] = p_resolution;
	const uint64_t signature = asset_signature(triangles, p_resolution);
	report["signature"] = int64_t(signature);
	report["path"] = asset_cache_path(signature);
	report["packed_path"] = packed_asset_path(signature);
	CoarseGeometryField field;
	const bool loaded = load_asset_field(signature, field);
	if (!loaded) {
		const int threads = CLAMP(OS::get_singleton()->get_processor_count() - 1, 1, 16);
		const MeshSdfBakeResult baked = bake_mesh_sdf(build_triangle_mesh(std::move(triangles)), p_resolution, nullptr, threads);
		if (baked.error != MESH_SDF_BAKE_OK) {
			report["error"] = vformat("Coarse geometry preparation failed (error %d).", int(baked.error));
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
	report["bytes"] = int64_t(field.cells.size() * sizeof(uint32_t));
	return report;
}

bool load_asset_field(uint64_t p_signature, CoarseGeometryField &r_field) {
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
	CoarseGeometryField field;
	for (int axis = 0; axis < 3; axis++) {
		field.min[axis] = file->get_double();
	}
	field.cell = file->get_double();

	for (int axis = 0; axis < 3; axis++) {
		field.size[axis] = int(file->get_32());
	}
	field.closed_shell_count = int(file->get_32());
	field.open_shell_count = int(file->get_32());
	field.surface_voxels = int(file->get_32());
	if (!read_values(file, field.cells) || file->get_error() != OK || field.cells.empty() ||
			field.size[0] <= 0 || field.size[1] <= 0 || field.size[2] <= 0 ||
			uint64_t(field.size[0]) * field.size[1] * field.size[2] != field.cells.size()) {
		return false;
	}
	r_field = field;
	return true;
}

bool store_asset_field(uint64_t p_signature, const CoarseGeometryField &p_field) {
	if (p_field.cells.empty()) {
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

		for (int axis = 0; axis < 3; axis++) {
			file->store_32(uint32_t(p_field.size[axis]));
		}
		file->store_32(uint32_t(p_field.closed_shell_count));
		file->store_32(uint32_t(p_field.open_shell_count));
		file->store_32(uint32_t(p_field.surface_voxels));
		store_values(file, p_field.cells);
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

bool load_instance_field(uint64_t p_signature, SurfaceMaterialField &r_field) {
	const String packed = packed_instance_path(p_signature);
	Ref<FileAccess> file = FileAccess::open(FileAccess::exists(packed) ? packed : instance_cache_path(p_signature), FileAccess::READ);
	if (file.is_null() || file->get_32() != INSTANCE_CACHE_FORMAT_VERSION || file->get_64() != p_signature) {
		return false;
	}
	SurfaceMaterialField field;
	field.cell_stride = int(file->get_32());
	if (field.cell_stride <= 0) {
		return false;
	}
	uint64_t count = 1;
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = int(file->get_32());
		if (field.color_size[axis] <= 0) {
			return false;
		}
		count *= field.color_size[axis];
	}
	if (!read_values(file, field.albedo) || field.albedo.size() != count * 3 ||
			!read_values(file, field.emission) || (!field.emission.empty() && field.emission.size() != count * 3) ||
			!read_values(file, field.emission_sides) || (!field.emission_sides.empty() && field.emission_sides.size() != count) ||
			std::any_of(field.emission_sides.begin(), field.emission_sides.end(), [](uint8_t p_sides) { return p_sides > EMISSION_BOTH; }) || file->get_error() != OK) {
		return false;
	}
	r_field = std::move(field);
	return true;
}

bool store_instance_field(uint64_t p_signature, const SurfaceMaterialField &p_field) {
	const String path = instance_cache_path(p_signature);
	const String temporary = path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_32(INSTANCE_CACHE_FORMAT_VERSION);
	file->store_64(p_signature);
	file->store_32(p_field.cell_stride);
	for (int axis = 0; axis < 3; axis++) {
		file->store_32(p_field.color_size[axis]);
	}
	store_values(file, p_field.albedo);
	store_values(file, p_field.emission);
	store_values(file, p_field.emission_sides);
	const Error error = file->get_error();
	file.unref();
	if (error != OK) {
		return false;
	}
	Ref<DirAccess> directory = DirAccess::open(asset_cache_directory());
	return directory.is_valid() && directory->rename(temporary.get_file(), path.get_file()) == OK;
}

std::shared_ptr<const CoarseGeometryField> find_shared_asset_field(uint64_t p_signature) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_fields.find(p_signature);
	if (found == shared_fields.end()) {
		return nullptr;
	}
	return found->second.lock();
}

std::shared_ptr<const CoarseGeometryField> share_asset_field(uint64_t p_signature, CoarseGeometryField p_field) {
	return share_field(shared_fields, p_signature, std::move(p_field));
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

std::shared_ptr<const SurfaceMaterialField> find_shared_instance_field(uint64_t p_signature) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_instance_fields.find(p_signature);
	if (found == shared_instance_fields.end()) {
		return nullptr;
	}
	std::shared_ptr<const SurfaceMaterialField> shared = found->second.lock();
	if (!shared) {
		shared_instance_fields.erase(found);
	}
	return shared;
}

std::shared_ptr<const SurfaceMaterialField> share_instance_field(uint64_t p_signature, SurfaceMaterialField p_field) {
	return share_field(shared_instance_fields, p_signature, std::move(p_field));
}

void clear_shared_asset_fields() {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	shared_fields.clear();
	shared_instance_fields.clear();
}

uint64_t asset_field_bytes(const CoarseGeometryField &p_field) {
	return uint64_t(sizeof(CoarseGeometryField)) + uint64_t(p_field.cells.capacity() * sizeof(uint32_t));
}

} // namespace lrt
