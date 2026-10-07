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
#include "scene/resources/mesh.h"

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

bool mesh_triangles(const Ref<Mesh> &p_mesh, std::vector<MeshTriangle> &r_triangles) {
	r_triangles.clear();
	if (p_mesh.is_null()) {
		return false;
	}
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
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
