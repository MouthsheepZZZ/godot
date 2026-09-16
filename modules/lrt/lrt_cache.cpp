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
	// .godot/ is the editor's own derived directory: writable while the editor (or a dev build)
	// runs from a project on disk, unwritable in an export, which is what selects the fallback.
	const String primary = "res://.godot/lrt";
	if (DirAccess::make_dir_recursive_absolute(primary) == OK) {
		return primary;
	}
	const String fallback = "user://lrt_cache";
	DirAccess::make_dir_recursive_absolute(fallback);
	return fallback;
}

String entry_path(uint64_t p_signature) {
	return asset_cache_directory().path_join(vformat("asset_%016x.sdf", p_signature));
}

template <typename T>
void read_values(Ref<FileAccess> p_file, std::vector<T> &r_values) {
	r_values.resize(size_t(p_file->get_32()));
	if (r_values.empty()) {
		return;
	}
	PackedByteArray bytes = p_file->get_buffer(int(r_values.size() * sizeof(T)));
	memcpy(r_values.data(), bytes.ptr(), r_values.size() * sizeof(T));
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

bool load_asset_field(uint64_t p_signature, SdfGeometryField &r_field) {
	Ref<FileAccess> file = FileAccess::open(entry_path(p_signature), FileAccess::READ);
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
	read_values(file, field.distance);
	if (file->get_error() != OK || field.distance.empty()) {
		return false;
	}
	r_field = field;
	return true;
}

bool store_asset_field(uint64_t p_signature, const SdfGeometryField &p_field) {
	if (p_field.distance.empty()) {
		return false;
	}
	const String path = entry_path(p_signature);
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
