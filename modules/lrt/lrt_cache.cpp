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
#include "lrt_transfer_material.h"

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
		if (material[3] > 0.5f) {
			continue;
		}
		const LocalRecord &record = p_field.get_local_record(probe);
		if (material[1] == 0.0f && !record.has_contribution()) {
			continue;
		}
		const Vec3 reflectance = record.reflection_dc();
		float *pixel = pixels.data() + probe * 4;
		for (int channel = 0; channel < 3; channel++) {
			// DC response averages over the whole sphere, including unobstructed directions.
			pixel[channel] = p_emission ? float(C0 * record.emission[size_t(channel) * 4]) : float(reflectance[channel]);
		}
		// Visibility-only black occluders remain valid diagnostics without reflected energy.
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
				const Vec3 center = p_field.min + Vec3(x, y, z) * p_field.cell;
				if (p_field.subcells.indices.empty()) {
					cells.insert(cells.end(), { float(center.x), float(center.y), float(center.z), 1.0f });
					continue;
				}
				const uint32_t slot = p_field.subcells.indices[index];
				if (slot == SubcellCoverage::NO_MASK) {
					continue;
				}
				const uint64_t mask = p_field.subcells.masks[slot];
				const double cell = p_field.cell / SubcellCoverage::RESOLUTION;
				const Vec3 low = center - Vec3(0.5, 0.5, 0.5) * p_field.cell;
				for (int bit = 0; bit < 64; bit++) {
					if (!(mask & (uint64_t(1) << bit))) {
						continue;
					}
					const Vec3 child = low + Vec3((bit & 3) + 0.5, ((bit >> 2) & 3) + 0.5, (bit >> 4) + 0.5) * cell;
					cells.insert(cells.end(), { float(child.x), float(child.y), float(child.z), 1.0f });
				}
			}
		}
	}
	return cells;
}

namespace {

// Bump when the stored layout changes, so old files are ignored instead of misread.
constexpr uint32_t CACHE_FORMAT_VERSION = 6;
constexpr uint32_t INSTANCE_CACHE_FORMAT_VERSION = 6;
constexpr uint32_t SDF_ALGORITHM_VERSION = 6;
constexpr char CACHE_MAGIC[8] = { 'L', 'R', 'T', 'G', 'E', 'O', '0', '5' };
std::mutex shared_fields_mutex;
std::mutex capture_cache_mutex;
std::mutex transfer_cache_mutex;
std::map<uint64_t, std::weak_ptr<const CoarseGeometryField>> shared_fields;
std::map<uint64_t, std::weak_ptr<const SurfaceMaterialField>> shared_instance_fields;
std::map<uint64_t, std::weak_ptr<const TransferVisibilityField>> shared_transfer_visibility;
std::map<uint64_t, std::weak_ptr<const TransferMaterialField>> shared_transfer_material;
std::map<uint64_t, std::weak_ptr<const TransferCoverage>> shared_transfer_coverage;

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
	if (file.is_null() || file->get_32() != 0x4c565843 || file->get_32() != 3 || file->get_64() != p_signature) {
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
	images["subcell_indices"] = p_images.get("subcell_indices", PackedByteArray());
	images["subcell_masks"] = p_images.get("subcell_masks", PackedByteArray());
	images["coverage_filtered"] = p_images.get("coverage_filtered", false);
	images["coverage_removed"] = p_images.get("coverage_removed", int64_t(0));
	images["coverage_filter_usec"] = p_images.get("coverage_filter_usec", int64_t(0));
	std::lock_guard<std::mutex> lock(capture_cache_mutex);
	Ref<FileAccess> file = FileAccess::open_compressed(static_voxel_capture_path(p_signature), FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	if (file.is_null()) {
		return false;
	}
	file->store_32(0x4c565843);
	file->store_32(3);
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

double geometry_cell_size(const Object *p_instance) {
	if (p_instance && p_instance->has_meta(VOXEL_CELL_SIZE_META)) {
		const double override_size = p_instance->get_meta(VOXEL_CELL_SIZE_META);
		if (override_size > 0.0 && Math::is_finite(override_size)) {
			return MAX(0.001, override_size);
		}
	}
	return MAX(0.001, double(GLOBAL_GET("rendering/global_illumination/lrt/voxel/cell_size")));
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
	if (!read_values(file, field.cells) || !read_values(file, field.subcells.indices) || !read_values(file, field.subcells.masks) ||
			!field.subcells.valid_for(field.cells) || file->get_error() != OK || field.cells.empty() ||
			field.size[0] <= 0 || field.size[1] <= 0 || field.size[2] <= 0 ||
			uint64_t(field.size[0]) * field.size[1] * field.size[2] != field.cells.size()) {
		return false;
	}
	r_field = field;
	return true;
}

bool store_asset_field(uint64_t p_signature, const CoarseGeometryField &p_field) {
	if (p_field.cells.empty() || !p_field.subcells.valid_for(p_field.cells)) {
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
		store_values(file, p_field.subcells.indices);
		store_values(file, p_field.subcells.masks);
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

GeometryRepresentation geometry_representation(const Object *p_instance) {
	return p_instance && p_instance->has_meta(REPRESENTATION_META) ?
			GeometryRepresentation(int(p_instance->get_meta(REPRESENTATION_META))) : GEOMETRY_VOXEL;
}

TransferBakeSettings transfer_bake_settings(const Object *p_instance) {
	const auto setting = [p_instance](const char *p_metadata, const char *p_project) -> Variant {
		return p_instance && p_instance->has_meta(p_metadata) ? p_instance->get_meta(p_metadata) : GLOBAL_GET(p_project);
	};
	TransferBakeSettings result;
	result.support_radius = setting(TRANSFER_RADIUS_META, "rendering/global_illumination/lrt/transfer/support_radius");
	result.spacing = setting(TRANSFER_SPACING_META, "rendering/global_illumination/lrt/transfer/sample_spacing");
	result.angular_samples = setting(TRANSFER_ANGULAR_SAMPLES_META, "rendering/global_illumination/lrt/transfer/angular_samples");
	return result;
}

double occupancy_band_width(const Object *p_instance) {
	return p_instance && p_instance->has_meta(OCCUPANCY_BAND_WIDTH_META) ?
			double(p_instance->get_meta(OCCUPANCY_BAND_WIDTH_META)) :
			double(GLOBAL_GET("rendering/global_illumination/lrt/transfer/occupancy_band_width"));
}

uint64_t transfer_configuration_signature(const Object *p_instance) {
	const GeometryRepresentation representation = geometry_representation(p_instance);
	const TransferBakeSettings settings = transfer_bake_settings(p_instance);
	uint64_t signature = instance_field_cache_signature(1, uint64_t(representation));
	for (double value : { settings.support_radius, representation == GEOMETRY_PRECOMPUTED_TRANSFER ? settings.spacing : 0.0,
			 representation == GEOMETRY_PRECOMPUTED_TRANSFER ? occupancy_band_width(p_instance) : 0.0 }) {
		uint64_t bits;
		memcpy(&bits, &value, sizeof(bits));
		signature = instance_field_cache_signature(signature, bits);
	}
	if (representation == GEOMETRY_PRECOMPUTED_TRANSFER) {
		signature = instance_field_cache_signature(signature, uint64_t(settings.angular_samples));
		if (p_instance != nullptr && p_instance->has_meta(TRANSFER_MANIFEST_META)) {
			signature = instance_field_cache_signature(signature, p_instance->get_meta(TRANSFER_MANIFEST_META).hash());
		}
	}
	return signature;
}

uint64_t transfer_bake_signature(const TransferBakeSettings &p_settings) {
	// Version identifies the angular sequence, spherical reflection kernel and visibility encoding.
	uint64_t result = 4;
	for (double value : { p_settings.support_radius, p_settings.spacing }) {
		uint64_t bits;
		memcpy(&bits, &value, sizeof(bits));
		result = instance_field_cache_signature(result, bits);
	}
	return instance_field_cache_signature(result, uint64_t(p_settings.angular_samples));
}

uint64_t transfer_visibility_signature(uint64_t p_geometry, uint64_t p_coverage, const TransferBakeSettings &p_settings) {
	return instance_field_cache_signature(instance_field_cache_signature(p_geometry, p_coverage), transfer_bake_signature(p_settings));
}

uint64_t transfer_material_signature(uint64_t p_visibility, uint64_t p_reflectance, uint64_t p_emission) {
	return instance_field_cache_signature(p_visibility, instance_field_cache_signature(p_reflectance, p_emission));
}

String transfer_cache_path(uint64_t p_signature, bool p_visibility) {
	return asset_cache_directory().path_join(vformat("%s_%016x.lrt", p_visibility ? "visibility" : "transfer", p_signature));
}

String packed_transfer_path(uint64_t p_signature, bool p_visibility) {
	return vformat("res://.godot/lrt_export/%s_%016x.lrt", p_visibility ? "visibility" : "transfer", p_signature);
}

bool load_transfer_field(uint64_t p_signature, bool p_visibility, TransferField &r_field) {
	const String packed = packed_transfer_path(p_signature, p_visibility);
	Ref<FileAccess> file = FileAccess::open_compressed(FileAccess::exists(packed) ? packed : transfer_cache_path(p_signature, p_visibility), FileAccess::READ);
	if (file.is_null() || file->get_length() > INT32_MAX) {
		return false;
	}
	std::vector<uint8_t> data(size_t(file->get_length()));
	if (file->get_buffer(data.data(), data.size()) != data.size()) {
		return false;
	}
	return decode_transfer_field(data, p_signature, p_visibility, r_field);
}

bool load_transfer_instance(uint64_t p_visibility, uint64_t p_material, const TransferBakeSettings &p_settings, TransferInstance &r_instance) {
	if (p_visibility == 0 || p_material == 0 ||
			(!FileAccess::exists(packed_transfer_coverage_path(p_visibility)) && !FileAccess::exists(transfer_coverage_path(p_visibility)))) {
		return false;
	}
	TransferField field;
	if (!load_transfer_field(p_visibility, true, field) ||
			!load_transfer_field(p_material, false, field) || field.support_radius != p_settings.support_radius ||
			field.spacing != p_settings.spacing || field.angular_samples != p_settings.angular_samples) {
		return false;
	}
	TransferInstance instance;
	instance.coverage = load_transfer_coverage(p_visibility);
	if (!instance.coverage) {
		return false;
	}
	instance.visibility = share_transfer_visibility(p_visibility, field);
	instance.material = share_transfer_material(p_material, field);
	r_instance = std::move(instance);
	return true;
}

bool store_transfer_field(uint64_t p_signature, bool p_visibility, const TransferField &p_field) {
	std::lock_guard<std::mutex> lock(transfer_cache_mutex);
	const std::vector<uint8_t> data = encode_transfer_field(p_field, p_signature, p_visibility);
	if (data.empty()) {
		return false;
	}
	const String path = transfer_cache_path(p_signature, p_visibility);
	const String temporary = path + ".tmp";
	Ref<FileAccess> file = FileAccess::open_compressed(temporary, FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	if (file.is_null()) {
		return false;
	}
	file->store_buffer(data.data(), data.size());
	file->flush();
	const Error error = file->get_error();
	file.unref();
	if (error != OK) {
		return false;
	}
	Ref<DirAccess> directory = DirAccess::open(asset_cache_directory());
	return directory.is_valid() && directory->rename(temporary.get_file(), path.get_file()) == OK;
}

String transfer_coverage_path(uint64_t p_signature) {
	return asset_cache_directory().path_join(vformat("coverage_%016x.lrt", p_signature));
}

String packed_transfer_coverage_path(uint64_t p_signature) {
	return vformat("res://.godot/lrt_export/coverage_%016x.lrt", p_signature);
}

bool store_transfer_coverage(uint64_t p_signature, const TransferCoverage &p_coverage) {
	std::lock_guard<std::mutex> lock(transfer_cache_mutex);
	Dictionary payload;
	payload["version"] = 1;
	payload["signature"] = int64_t(p_signature);
	PackedVector3Array positions;
	positions.resize(p_coverage.triangles->size() * 3);
	for (size_t triangle = 0; triangle < p_coverage.triangles->size(); triangle++) {
		for (int vertex = 0; vertex < 3; vertex++) {
			const Vec3 &point = (*p_coverage.triangles)[triangle].position[vertex];
			positions.set(triangle * 3 + vertex, Vector3(point.x, point.y, point.z));
		}
	}
	payload["positions"] = positions;
	PackedInt32Array mapping;
	for (const auto &entry : p_coverage.triangle_mapping) {
		mapping.push_back(entry.first);
		mapping.push_back(entry.second);
	}
	payload["mapping"] = mapping;
	Array surfaces;
	for (const TransferCoverageAtlas &surface : p_coverage.surfaces) {
		Dictionary data;
		data["size"] = Vector2i(surface.width, surface.height);
		data["tile_size"] = surface.tile_size;
		PackedByteArray covered;
		covered.resize(surface.covered.size());
		memcpy(covered.ptrw(), surface.covered.data(), surface.covered.size());
		data["covered"] = covered;
		surfaces.push_back(data);
	}
	payload["surfaces"] = surfaces;
	const String path = transfer_coverage_path(p_signature);
	Ref<FileAccess> file = FileAccess::open_compressed(path + ".tmp", FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	if (file.is_null()) {
		return false;
	}
	file->store_var(payload);
	file->flush();
	const Error error = file->get_error();
	file.unref();
	Ref<DirAccess> directory = DirAccess::open(asset_cache_directory());
	return error == OK && directory.is_valid() && directory->rename(path.get_file() + ".tmp", path.get_file()) == OK;
}

std::shared_ptr<const TransferCoverage> load_transfer_coverage(uint64_t p_signature) {
	{
		std::lock_guard<std::mutex> lock(shared_fields_mutex);
		const auto found = shared_transfer_coverage.find(p_signature);
		if (found != shared_transfer_coverage.end()) {
			const auto retained = found->second.lock();
			if (retained) {
				return retained;
			}
		}
	}
	const String packed = packed_transfer_coverage_path(p_signature);
	Ref<FileAccess> file = FileAccess::open_compressed(FileAccess::exists(packed) ? packed : transfer_coverage_path(p_signature), FileAccess::READ);
	if (file.is_null()) {
		return nullptr;
	}
	const Variant decoded = file->get_var(false);
	if (file->get_error() != OK || decoded.get_type() != Variant::DICTIONARY) {
		return nullptr;
	}
	const Dictionary payload = decoded;
	if (int(payload.get("version", 0)) != 1 || uint64_t(int64_t(payload.get("signature", int64_t(0)))) != p_signature ||
			payload.get("positions", Variant()).get_type() != Variant::PACKED_VECTOR3_ARRAY ||
			payload.get("mapping", Variant()).get_type() != Variant::PACKED_INT32_ARRAY || payload.get("surfaces", Variant()).get_type() != Variant::ARRAY) {
		return nullptr;
	}
	const PackedVector3Array positions = payload["positions"];
	const PackedInt32Array mapping = payload["mapping"];
	const Array surfaces = payload["surfaces"];
	if (positions.is_empty() || positions.size() % 3 != 0 || mapping.size() != positions.size() / 3 * 2) {
		return nullptr;
	}
	TransferCoverage coverage;
	for (const Variant &entry : surfaces) {
		if (entry.get_type() != Variant::DICTIONARY) {
			return nullptr;
		}
		const Dictionary surface = entry;
		if (surface.get("size", Variant()).get_type() != Variant::VECTOR2I || surface.get("covered", Variant()).get_type() != Variant::PACKED_BYTE_ARRAY) {
			return nullptr;
		}
		const Vector2i size = surface["size"];
		const int tile_size = surface.get("tile_size", 0);
		const PackedByteArray covered = surface["covered"];
		if (size.x <= 0 || size.y <= 0 || tile_size < 8 || size.x % tile_size != 0 || size.y % tile_size != 0 ||
				uint64_t(size.x) * size.y != uint64_t(covered.size())) {
			return nullptr;
		}
		TransferCoverageAtlas atlas;
		atlas.width = size.x;
		atlas.height = size.y;
		atlas.tile_size = tile_size;
		atlas.covered.assign(covered.ptr(), covered.ptr() + covered.size());
		if (std::any_of(atlas.covered.begin(), atlas.covered.end(), [](uint8_t p_value) { return p_value > 1; })) {
			return nullptr;
		}
		coverage.surfaces.push_back(std::move(atlas));
	}
	auto triangles = std::make_shared<std::vector<MeshTriangle>>();
	triangles->resize(positions.size() / 3);
	for (int triangle = 0; triangle < positions.size() / 3; triangle++) {
		const int surface = mapping[triangle * 2];
		const int local_triangle = mapping[triangle * 2 + 1];
		if (surface < 0 || surface >= int(coverage.surfaces.size()) || local_triangle < 0) {
			return nullptr;
		}
		const TransferCoverageAtlas &atlas = coverage.surfaces[surface];
		if (uint64_t(local_triangle) >= uint64_t(atlas.width / atlas.tile_size) * (atlas.height / atlas.tile_size)) {
			return nullptr;
		}
		coverage.triangle_mapping.emplace_back(surface, local_triangle);
		for (int vertex = 0; vertex < 3; vertex++) {
			const Vector3 point = positions[triangle * 3 + vertex];
			if (!point.is_finite()) {
				return nullptr;
			}
			(*triangles)[triangle].position[vertex] = Vec3(point.x, point.y, point.z);
		}
	}
	coverage.triangles = std::move(triangles);
	coverage.geometry = std::make_shared<TransferGeometry>(coverage.triangles);
	return share_field(shared_transfer_coverage, p_signature, std::move(coverage));
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

std::shared_ptr<const TransferVisibilityField> find_shared_transfer_visibility(uint64_t p_signature) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_transfer_visibility.find(p_signature);
	return found == shared_transfer_visibility.end() ? nullptr : found->second.lock();
}

std::shared_ptr<const TransferVisibilityField> share_transfer_visibility(uint64_t p_signature, const TransferField &p_field) {
	const auto existing = find_shared_transfer_visibility(p_signature);
	return existing ? existing : share_field(shared_transfer_visibility, p_signature, TransferVisibilityField(p_field));
}

std::shared_ptr<const TransferMaterialField> find_shared_transfer_material(uint64_t p_signature) {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	const auto found = shared_transfer_material.find(p_signature);
	return found == shared_transfer_material.end() ? nullptr : found->second.lock();
}

std::shared_ptr<const TransferMaterialField> share_transfer_material(uint64_t p_signature, const TransferField &p_field) {
	const auto existing = find_shared_transfer_material(p_signature);
	return existing ? existing : share_field(shared_transfer_material, p_signature, TransferMaterialField(p_field));
}

void clear_shared_asset_fields() {
	std::lock_guard<std::mutex> lock(shared_fields_mutex);
	shared_fields.clear();
	shared_instance_fields.clear();
	shared_transfer_visibility.clear();
	shared_transfer_material.clear();
	shared_transfer_coverage.clear();
}

uint64_t asset_field_bytes(const CoarseGeometryField &p_field) {
	return uint64_t(sizeof(CoarseGeometryField)) + uint64_t(p_field.cells.capacity() * sizeof(uint32_t)) +
			uint64_t(p_field.subcells.indices.capacity() * sizeof(uint32_t)) +
			uint64_t(p_field.subcells.masks.capacity() * sizeof(uint64_t));
}

} // namespace lrt
