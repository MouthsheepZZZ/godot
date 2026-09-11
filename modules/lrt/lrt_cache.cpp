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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/templates/hashfuncs.h"

#include <cstring>

namespace lrt {

namespace {

// Bump when the stored layout changes, so old files are ignored instead of misread.
constexpr uint32_t CACHE_FORMAT_VERSION = 1;
constexpr char CACHE_MAGIC[8] = { 'L', 'R', 'T', 'S', 'D', 'F', '0', '1' };

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
	// FNV-1a over the raw triangle bytes: a 64-bit content digest, so two different assets
	// sharing a cache entry is not a practical risk, and identical content still deduplicates.
	uint64_t hash = 1469598103934665603ull;
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(p_triangles.data());
	const size_t count = p_triangles.size() * sizeof(MeshTriangle);
	for (size_t i = 0; i < count; i++) {
		hash ^= uint64_t(bytes[i]);
		hash *= 1099511628211ull;
	}
	hash ^= uint64_t(uint32_t(p_resolution));
	hash *= 1099511628211ull;
	return hash;
}

String asset_cache_directory() {
	// Resolved once: the answer cannot change while the process runs.
	static String directory = resolve_cache_directory();
	return directory;
}

bool load_asset_field(uint64_t p_signature, ColorSdfField &r_field) {
	Ref<FileAccess> file = FileAccess::open(entry_path(p_signature), FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	char magic[8] = {};
	file->get_buffer(reinterpret_cast<uint8_t *>(magic), 8);
	if (memcmp(magic, CACHE_MAGIC, 8) != 0 || file->get_32() != CACHE_FORMAT_VERSION) {
		return false;
	}
	ColorSdfField field;
	for (int axis = 0; axis < 3; axis++) {
		field.min[axis] = file->get_double();
	}
	field.cell = file->get_double();
	field.distance_scale = file->get_double();
	for (int axis = 0; axis < 3; axis++) {
		field.size[axis] = int(file->get_32());
	}
	read_values(file, field.distance);
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = int(file->get_32());
	}
	read_values(file, field.color);
	if (file->get_error() != OK || field.distance.empty() || field.color.empty()) {
		return false;
	}
	r_field = field;
	return true;
}

bool store_asset_field(uint64_t p_signature, const ColorSdfField &p_field) {
	if (p_field.distance.empty() || p_field.color.empty()) {
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
		store_values(file, p_field.distance);
		for (int axis = 0; axis < 3; axis++) {
			file->store_32(uint32_t(p_field.color_size[axis]));
		}
		store_values(file, p_field.color);
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

} // namespace lrt
