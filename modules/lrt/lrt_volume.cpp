/**************************************************************************/
/*  lrt_volume.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
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

#include "lrt_volume.h"

#include "core/config/project_settings.h"

#include "lrt_cache.h"

#include "lrt_inject.glsl.gen.h"
#include "lrt_local_patch.glsl.gen.h"
#include "lrt_propagate.glsl.gen.h"
#include "lrt_render_bridge.h"
#include "lrt_sky_project.glsl.gen.h"
#include "lrt_sky_solve.glsl.gen.h"
#include "lrt_boundary_inc.glsl.gen.h"

#include "core/config/engine.h"
#include "core/io/image.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "scene/resources/environment.h"
#include "scene/resources/texture.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <set>

namespace {

constexpr int WORKGROUP_SIZE = 64;
constexpr int MAX_LOCAL_PATCH_PROBES = 8192;
constexpr int PROBES_PER_LOCAL_TRUNK = 8 * 8 * 8;
// Dirty-Trunk work inside one incremental build is short but not free; this bounded fan-out keeps
// the publish inside the frame that detects the edit without paying the full bake fan-out.
constexpr int LRT_INCREMENTAL_BAKE_THREADS = 8;
// A direct resolve waits at most this many frames for the decal atlas to bind a projector before
// publishing without its cookie. The atlas needs a frame or two after the light appears.
constexpr int NATIVE_PROJECTOR_WAIT_FRAMES = 8;
constexpr int SKY_FACE_RESOLUTION = 8;
constexpr int SKY_DIRECTION_COUNT = LRTVolume::SKY_DIRECTION_COUNT;
static_assert(SKY_DIRECTION_COUNT == 6 * SKY_FACE_RESOLUTION * SKY_FACE_RESOLUTION);
static_assert(lrt::DIRECTION_COUNT <= 32);
constexpr double SH_C0 = 0.2820947918;
constexpr double SH_C1 = 0.4886025119;
constexpr uint32_t LOCAL_CACHE_FORMAT_VERSION = 26;
constexpr char LOCAL_CACHE_MAGIC[8] = { 'L', 'R', 'T', 'L', 'O', 'C', '0', '1' };
std::atomic<bool> lrt_gpu_profiling_enabled{ false };

uint64_t debug_texture_bytes(uint32_t p_fields, uint64_t p_probe_count) {
	return ((p_fields & LRTVolume::DEBUG_DIRTY) ? p_probe_count : 1) * 4 * sizeof(float);
}

template <typename T>
void store_local_cache_values(const Ref<FileAccess> &p_file, const std::vector<T> &p_values) {
	p_file->store_64(uint64_t(p_values.size()));
	if (!p_values.empty()) {
		p_file->store_buffer(reinterpret_cast<const uint8_t *>(p_values.data()), uint64_t(p_values.size() * sizeof(T)));
	}
}

std::vector<float> dirty_mask_rgba(const std::vector<uint8_t> &p_mask) {
	std::vector<float> pixels(p_mask.size() * 4, 0.0f);
	for (size_t probe = 0; probe < p_mask.size(); probe++) {
		pixels[probe * 4] = p_mask[probe] ? 1.0f : 0.0f;
		pixels[probe * 4 + 3] = 1.0f;
	}
	return pixels;
}

std::vector<float> sample_sdf_diagnostic_rgba(const lrt::LocalCache &p_cache, size_t p_count) {
	std::vector<float> pixels(p_count * 4, 0.0f);
	if (p_cache.samples.size() != p_count || p_cache.sampled.size() != p_count) {
		return pixels;
	}
	for (size_t probe = 0; probe < p_count; probe++) {
		if (!p_cache.sampled[probe]) {
			continue;
		}
		float *pixel = pixels.data() + probe * 4;
		pixel[0] = 1.0f;
		pixel[3] = 1.0f;
	}
	return pixels;
}

template <typename T>
bool read_local_cache_values(const Ref<FileAccess> &p_file, std::vector<T> &r_values, uint64_t p_max_count) {
	const uint64_t count = p_file->get_64();
	if (count > p_max_count || count > uint64_t(INT_MAX)) {
		return false;
	}
	r_values.resize(size_t(count));
	if (!r_values.empty()) {
		const uint64_t byte_count = count * sizeof(T);
		const PackedByteArray bytes = p_file->get_buffer(byte_count);
		if (uint64_t(bytes.size()) != byte_count) {
			return false;
		}
		memcpy(r_values.data(), bytes.ptr(), size_t(byte_count));
	}
	return true;
}

String local_cache_path(uint64_t p_fingerprint) {
	return lrt::asset_cache_directory().path_join(vformat("volume_%016x.lrt", p_fingerprint));
}

struct SharedShaderResources {
	RenderingDevice *device = nullptr;
	std::array<Ref<RDShaderSPIRV>, 5> spirv;
	std::array<RID, 5> shaders;
	std::array<RID, 5> pipelines;
};

SharedShaderResources lrt_shared_shaders;

// std140 layout shared by both compute shaders.
struct ParamsData {
	int32_t grid_size[4] = { 0, 0, 0, 0 };
	float grid_min[4] = { 0, 0, 0, 0 };
	int32_t counts[4] = { 0, 0, 0, 0 };
	float flags[4] = { 0, 0, 0, 0 }; // x reserved, y receive sky, z coarse-geometry backend, w reserved
	float sky_link_radiance[3][lrt::DIRECTION_COUNT][4] = {};
};

struct NativeLightStateData {
	float volume_to_light[16] = {};
	float shadow_matrix[4][16] = {};
	float color_kind[4] = {};
	float position_range[4] = {};
	float direction_attenuation[4] = {};
	float cone_area[4] = {};
	float projector[4] = {};
	float shadow_rect[4][4] = {};
	float shadow_parameters[4] = {};
	float cascade_splits[4] = {};
	float cascade_blend[4] = {};
	float camera_depth[4] = {};
	float shadow_bias[4] = {};
	float atlas_flags[4] = {};
	float area_projector[4] = {};
};
static_assert(sizeof(NativeLightStateData) == 576);
static_assert(offsetof(NativeLightStateData, color_kind) == 320);
static_assert(offsetof(NativeLightStateData, shadow_parameters) == 464);
static_assert(offsetof(NativeLightStateData, camera_depth) == 512);
static_assert(offsetof(NativeLightStateData, area_projector) == 560);

static void store_transform_mat4(const Transform3D &p_transform, float *p_array) {
	p_array[0] = p_transform.basis.rows[0][0];
	p_array[1] = p_transform.basis.rows[1][0];
	p_array[2] = p_transform.basis.rows[2][0];
	p_array[3] = 0;
	p_array[4] = p_transform.basis.rows[0][1];
	p_array[5] = p_transform.basis.rows[1][1];
	p_array[6] = p_transform.basis.rows[2][1];
	p_array[7] = 0;
	p_array[8] = p_transform.basis.rows[0][2];
	p_array[9] = p_transform.basis.rows[1][2];
	p_array[10] = p_transform.basis.rows[2][2];
	p_array[11] = 0;
	p_array[12] = p_transform.origin.x;
	p_array[13] = p_transform.origin.y;
	p_array[14] = p_transform.origin.z;
	p_array[15] = 1;
}

static void store_projection_mat4(const Projection &p_projection, float *p_array) {
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			p_array[column * 4 + row] = p_projection.columns[column][row];
		}
	}
}

struct SkyDirection {
	Vector3 direction;
	float weight = 0.0f;
	int first_link = 0;
};

template <typename T>
uint64_t vector_bytes(const std::vector<T> &p_values) {
	return uint64_t(p_values.capacity()) * sizeof(T);
}

template <typename T>
uint64_t transfer_field_bytes(const T &p_field) {
	return sizeof(T) + p_field.bricks.size() * sizeof(typename decltype(p_field.bricks)::value_type);
}

uint64_t primitive_bytes(const std::vector<lrt::GeometryInstance> &p_primitives) {
	uint64_t bytes = vector_bytes(p_primitives);
	std::set<const lrt::TransferInstance *> instances;
	for (const auto &primitive : p_primitives) {
		if (primitive.precomputed && instances.insert(primitive.precomputed.get()).second) {
			bytes += sizeof(lrt::TransferInstance);
		}
	}
	return bytes;
}

uint64_t local_field_bytes(const lrt::LocalField &p_field) {
	return vector_bytes(p_field.material) + vector_bytes(p_field.local_records) + vector_bytes(p_field.local_record_indices) +
			vector_bytes(p_field.local_record_owners) + vector_bytes(p_field.free_local_records) + vector_bytes(p_field.gpu_local_records) +
			vector_bytes(p_field.local_visibility) +
			vector_bytes(p_field.dirty_mask) + vector_bytes(p_field.dirty_probes) +
			vector_bytes(p_field.changed_occupancy);
}

void seed_recycled_local_field(const lrt::LocalField &p_field, const lrt::LocalCache &p_cache,
		lrt::LocalField &r_recycled, lrt::LocalCache &r_cache) {
	r_recycled = lrt::LocalField();
	r_cache = lrt::LocalCache();
	r_recycled.material = p_field.material;
	r_recycled.local_records = p_field.local_records;
	r_recycled.local_record_indices = p_field.local_record_indices;
	r_recycled.local_record_owners = p_field.local_record_owners;
	r_recycled.free_local_records = p_field.free_local_records;
	r_recycled.local_visibility = p_field.local_visibility;
	r_cache.grid_key = p_cache.grid_key;
	r_cache.trunk_signatures = p_cache.trunk_signatures;
	r_cache.input_signatures = p_cache.input_signatures;
	r_cache.samples = p_cache.samples;
	r_cache.sampled = p_cache.sampled;
	r_cache.local = &r_recycled;
}

void encode_record(const lrt::LocalRecord &p_record, float *r_values) {
	memcpy(r_values, &p_record, sizeof(lrt::LocalRecord));
}

// Diagnostics expand sparse RGB operators and source metadata into sixteen vec4 planes.
std::vector<float> encode_local_records(const lrt::LocalField &p_field, int p_probe_count) {
	if (p_field.local_record_indices.empty()) {
		return {};
	}
	std::vector<float> values(size_t(p_probe_count) * 64);
	for (int probe = 0; probe < p_probe_count; probe++) {
		float encoded[64];
		encode_record(p_field.get_local_record(size_t(probe)), encoded);
		for (int row = 0; row < 16; row++) {
			memcpy(values.data() + (size_t(row) * p_probe_count + probe) * 4, encoded + row * 4, 4 * sizeof(float));
		}
	}
	return values;
}

void build_gpu_local_records(lrt::LocalField &r_field) {
	r_field.gpu_local_records.resize(r_field.local_records.size() * 64);
	if (!r_field.local_records.empty()) {
		memcpy(r_field.gpu_local_records.data(), r_field.local_records.data(),
				r_field.local_records.size() * sizeof(lrt::LocalRecord));
	}
}

uint64_t local_cache_bytes(const lrt::LocalCache &p_cache) {
	return vector_bytes(p_cache.trunk_signatures) + vector_bytes(p_cache.input_signatures) + vector_bytes(p_cache.samples) + vector_bytes(p_cache.sampled);
}

uint64_t material_capture_bytes(const lrt::MaterialCapture &p_capture) {
	return vector_bytes(p_capture.albedo) + vector_bytes(p_capture.emission) + vector_bytes(p_capture.occupied) + vector_bytes(p_capture.emission_sides);
}

int round_ratio(int p_numerator, int p_denominator) {
	const int absolute = p_numerator < 0 ? -p_numerator : p_numerator;
	const int magnitude = (absolute + p_denominator / 2) / p_denominator;
	return p_numerator < 0 ? -magnitude : magnitude;
}

int link_direction_index(const Vector3i &p_offset) {
	const lrt::Direction *directions = lrt::directions();
	for (int index = 0; index < lrt::DIRECTION_COUNT; index++) {
		if (directions[index].offset[0] == p_offset.x && directions[index].offset[1] == p_offset.y && directions[index].offset[2] == p_offset.z) {
			return index;
		}
	}
	return -1;
}

double cube_area_element(double p_x, double p_y) {
	return Math::atan2(p_x * p_y, Math::sqrt(p_x * p_x + p_y * p_y + 1.0));
}

double cube_texel_solid_angle(double p_u0, double p_v0, double p_u1, double p_v1) {
	return cube_area_element(p_u1, p_v1) - cube_area_element(p_u0, p_v1) -
			cube_area_element(p_u1, p_v0) + cube_area_element(p_u0, p_v0);
}

const std::array<SkyDirection, SKY_DIRECTION_COUNT> &sky_directions() {
	static const std::array<SkyDirection, SKY_DIRECTION_COUNT> directions = []() {
		std::array<SkyDirection, SKY_DIRECTION_COUNT> result;
		int direction_index = 0;
		for (int face = 0; face < 6; face++) {
			for (int y = 0; y < SKY_FACE_RESOLUTION; y++) {
				for (int x = 0; x < SKY_FACE_RESOLUTION; x++) {
					const int u = x * 2 + 1 - SKY_FACE_RESOLUTION;
					const int v = y * 2 + 1 - SKY_FACE_RESOLUTION;
					Vector3i endpoint;
					switch (face) {
						case 0:
							endpoint = Vector3i(SKY_FACE_RESOLUTION, -v, -u);
							break;
						case 1:
							endpoint = Vector3i(-SKY_FACE_RESOLUTION, -v, u);
							break;
						case 2:
							endpoint = Vector3i(u, SKY_FACE_RESOLUTION, v);
							break;
						case 3:
							endpoint = Vector3i(u, -SKY_FACE_RESOLUTION, -v);
							break;
						case 4:
							endpoint = Vector3i(u, -v, SKY_FACE_RESOLUTION);
							break;
						default:
							endpoint = Vector3i(-u, -v, -SKY_FACE_RESOLUTION);
							break;
					}

					SkyDirection &direction = result[direction_index++];
					direction.direction = Vector3(endpoint).normalized();
					const double u0 = double(x * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					const double v0 = double(y * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					const double u1 = double((x + 1) * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					const double v1 = double((y + 1) * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					direction.weight = float(cube_texel_solid_angle(u0, v0, u1, v1));
					direction.first_link = link_direction_index(Vector3i(
							round_ratio(endpoint.x, SKY_FACE_RESOLUTION),
							round_ratio(endpoint.y, SKY_FACE_RESOLUTION),
							round_ratio(endpoint.z, SKY_FACE_RESOLUTION)));
					DEV_ASSERT(direction.first_link >= 0);
				}
			}
		}
		return result;
	}();
	return directions;
}

String direction_initializer() {
	const lrt::Direction *directions = lrt::directions();
	String text;
	for (int i = 0; i < lrt::DIRECTION_COUNT; i++) {
		if (i > 0) {
			text += ", ";
		}
		text += vformat("ivec3(%d, %d, %d)", directions[i].offset[0], directions[i].offset[1], directions[i].offset[2]);
	}
	return text;
}

Vector<uint8_t> bytes_of(const void *p_data, size_t p_size) {
	Vector<uint8_t> bytes;
	bytes.resize(int64_t(p_size));
	memcpy(bytes.ptrw(), p_data, p_size);
	return bytes;
}

// CPU bake records retain their logical vec4 layout; GPU records omit unused trailing words.

} // namespace

// Texture2DRD intentionally does not own the RenderingDevice RID it exposes. LRT display
// materials can outlive the solver during scene teardown, so the RID must follow the final
// texture reference rather than the solver's buffer lifetime.
class LRTDisplayTexture : public Texture2DRD {
	RID owned_texture;

	static void _free_owned_texture(RID p_texture) {
		RenderingDevice *rendering_device = RenderingDevice::get_singleton();
		if (rendering_device && rendering_device->texture_is_valid(p_texture)) {
			rendering_device->free_rid(p_texture);
		}
	}

public:
	void set_owned_texture(RID p_texture) {
		owned_texture = p_texture;
		_set_texture_rd_rid(p_texture);
	}

	~LRTDisplayTexture() {
		if (!owned_texture.is_valid() || !RenderingServer::get_singleton()) {
			return;
		}
		// Queue removal of the RenderingServer proxy before freeing its underlying RD texture.
		set_texture_rd_rid(RID());
		RenderingServer::get_singleton()->call_on_render_thread(
				callable_mp_static(&LRTDisplayTexture::_free_owned_texture).bind(owned_texture));
		owned_texture = RID();
	}
};

LRTVolume::LRTVolume() {
	native_light_states.resize(size_t(native_light_capacity));
}

LRTVolume::~LRTVolume() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (device && rendering_server) {
		if (rendering_server->is_on_render_thread()) {
			_free_render_thread();
		} else {
			rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_free_render_thread));
			rendering_server->sync();
		}
	}
}

void LRTVolume::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "spacing"), &LRTVolume::configure);
	ClassDB::bind_method(D_METHOD("configure_with_bounds", "spacing", "bounds_min", "bounds_max"), &LRTVolume::configure_with_bounds);
	ClassDB::bind_method(D_METHOD("configure_sized", "spacing", "min", "size"), &LRTVolume::configure_sized);
	ClassDB::bind_method(D_METHOD("configure_sized_with_bounds", "spacing", "min", "size", "bounds_min", "bounds_max"), &LRTVolume::configure_sized_with_bounds);
	ClassDB::bind_method(D_METHOD("set_boxes", "boxes"), &LRTVolume::set_boxes);
	ClassDB::bind_method(D_METHOD("set_meshes", "meshes"), &LRTVolume::set_meshes);
	ClassDB::bind_method(D_METHOD("set_mesh_sdf_resolution", "resolution"), &LRTVolume::set_mesh_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_receive_sky", "enabled"), &LRTVolume::set_receive_sky);
	ClassDB::bind_method(D_METHOD("advance_sky"), &LRTVolume::advance_sky);
	ClassDB::bind_method(D_METHOD("read_sky_visibility"), &LRTVolume::read_sky_visibility);
	ClassDB::bind_method(D_METHOD("set_sky", "sky"), &LRTVolume::set_sky);
	ClassDB::bind_method(D_METHOD("set_sky_radiance", "radiance"), &LRTVolume::set_sky_radiance);
	ClassDB::bind_method(D_METHOD("get_sky_radiance"), &LRTVolume::get_sky_radiance);
	ClassDB::bind_method(D_METHOD("read_environment_panorama", "environment", "size"), &LRTVolume::read_environment_panorama);
	ClassDB::bind_method(D_METHOD("read_environment_radiance", "environment", "size"), &LRTVolume::read_environment_radiance);
	ClassDB::bind_method(D_METHOD("read_environment_radiance_sh", "environment", "size", "sky_to_local"), &LRTVolume::read_environment_radiance_sh);
	ClassDB::bind_method(D_METHOD("project_panorama_radiance_sh", "panorama", "sky_to_local"), &LRTVolume::project_panorama_radiance_sh);
	ClassDB::bind_method(D_METHOD("set_debug_fields", "mask"), &LRTVolume::set_debug_fields);
	ClassDB::bind_method(D_METHOD("set_external_gi_enabled", "enabled"), &LRTVolume::set_external_gi_enabled);
	ClassDB::bind_method(D_METHOD("has_external_gi_buffers"), &LRTVolume::has_external_gi_buffers);
	ClassDB::bind_method(D_METHOD("build_local_field", "backend"), &LRTVolume::build_local_field);
	ClassDB::bind_method(D_METHOD("bake_local_field", "backend"), &LRTVolume::bake_local_field);
	ClassDB::bind_method(D_METHOD("apply_local_field", "preserve_history"), &LRTVolume::apply_local_field, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("estimate_staged_gpu_bytes", "preserve_history"), &LRTVolume::estimate_staged_gpu_bytes);
	ClassDB::bind_method(D_METHOD("request_cancel"), &LRTVolume::request_cancel);
	ClassDB::bind_method(D_METHOD("clear_cancel"), &LRTVolume::clear_cancel);
	ClassDB::bind_method(D_METHOD("is_cancel_requested"), &LRTVolume::is_cancel_requested);
	ClassDB::bind_method(D_METHOD("has_local_field"), &LRTVolume::has_local_field);
	ClassDB::bind_method(D_METHOD("inject"), &LRTVolume::inject);
	ClassDB::bind_method(D_METHOD("step", "iterations"), &LRTVolume::step);
	ClassDB::bind_method(D_METHOD("reset"), &LRTVolume::reset);
	ClassDB::bind_method(D_METHOD("is_injection_pending"), &LRTVolume::is_injection_pending);
	ClassDB::bind_method(D_METHOD("get_pending_step_iterations"), &LRTVolume::get_pending_step_iterations);
	ClassDB::bind_method(D_METHOD("get_iteration"), &LRTVolume::get_iteration);
	ClassDB::bind_method(D_METHOD("get_grid"), &LRTVolume::get_grid);
	ClassDB::bind_method(D_METHOD("refresh_display"), &LRTVolume::refresh_display);
	ClassDB::bind_method(D_METHOD("get_texture", "name"), &LRTVolume::get_texture);
	ClassDB::bind_method(D_METHOD("read_field", "name"), &LRTVolume::read_field);
	ClassDB::bind_method(D_METHOD("read_local_visibility"), &LRTVolume::read_local_visibility);
	ClassDB::bind_method(D_METHOD("read_local_record_indices"), &LRTVolume::read_local_record_indices);
	ClassDB::bind_method(D_METHOD("sample_geometry", "point"), &LRTVolume::sample_geometry);
	ClassDB::bind_method(D_METHOD("get_stats"), &LRTVolume::get_stats);
	ClassDB::bind_method(D_METHOD("get_performance_stats"), &LRTVolume::get_performance_stats);
	ClassDB::bind_method(D_METHOD("get_memory_stats"), &LRTVolume::get_memory_stats);
	ClassDB::bind_method(D_METHOD("get_debug_resources"), &LRTVolume::get_debug_resources);
	ClassDB::bind_method(D_METHOD("refresh_performance_stats"), &LRTVolume::refresh_performance_stats);
	ClassDB::bind_method(D_METHOD("set_render_frame_profiling_enabled", "enabled"), &LRTVolume::set_render_frame_profiling_enabled);
	ClassDB::bind_method(D_METHOD("get_render_frame_profile"), &LRTVolume::get_render_frame_profile);
	ClassDB::bind_method(D_METHOD("get_preparation_status"), &LRTVolume::get_preparation_status);
	ClassDB::bind_static_method("LRTVolume", D_METHOD("clear_shared_sdf_cache"), &LRTVolume::clear_shared_sdf_cache);
}

static bool same_grid_layout(const lrt::Grid &p_left, const lrt::Grid &p_right) {
	return p_left.count == p_right.count && p_left.spacing == p_right.spacing &&
			p_left.min.x == p_right.min.x && p_left.min.y == p_right.min.y && p_left.min.z == p_right.min.z &&
			p_left.size[0] == p_right.size[0] && p_left.size[1] == p_right.size[1] && p_left.size[2] == p_right.size[2];
}

void LRTVolume::configure(double p_spacing) {
	const lrt::Grid next = lrt::make_grid(p_spacing);
	const bool compatible = configured && same_grid_layout(grid, next);
	grid = next;
	configured = true;
	has_local = has_local && compatible;
	has_staged = false;
}

void LRTVolume::configure_with_bounds(double p_spacing, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max) {
	const lrt::Grid next = lrt::make_grid(p_spacing,
			lrt::Vec3(p_bounds_min.x, p_bounds_min.y, p_bounds_min.z),
			lrt::Vec3(p_bounds_max.x, p_bounds_max.y, p_bounds_max.z));
	const bool compatible = configured && same_grid_layout(grid, next);
	grid = next;
	configured = true;
	has_local = has_local && compatible;
	has_staged = false;
}

// LRTVolume3D owns the grid region (its own size around its own position); the lattice rule
// is the prototype's, so a (6, 4, 6) volume at (0, 1.5, 0) reproduces the fixed
// [-3,-0.5,-3]..[3,3.5,3] lab region exactly.
void LRTVolume::configure_sized(double p_spacing, const Vector3 &p_min, const Vector3 &p_size) {
	const lrt::Grid next = lrt::make_grid_sized(p_spacing,
			lrt::Vec3(p_min.x, p_min.y, p_min.z),
			lrt::Vec3(p_size.x, p_size.y, p_size.z));
	const bool compatible = configured && same_grid_layout(grid, next);
	grid = next;
	configured = true;
	has_local = has_local && compatible;
	has_staged = false;
}

void LRTVolume::configure_sized_with_bounds(double p_spacing, const Vector3 &p_min, const Vector3 &p_size, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max) {
	const lrt::Grid next = lrt::make_grid_sized(p_spacing,
			lrt::Vec3(p_min.x, p_min.y, p_min.z),
			lrt::Vec3(p_size.x, p_size.y, p_size.z),
			lrt::Vec3(p_bounds_min.x, p_bounds_min.y, p_bounds_min.z),
			lrt::Vec3(p_bounds_max.x, p_bounds_max.y, p_bounds_max.z));
	const bool compatible = configured && same_grid_layout(grid, next);
	grid = next;
	configured = true;
	has_local = has_local && compatible;
	has_staged = false;
}

// Box inputs may arrive as doubles: the prototype's constants are doubles and its Color
// SDF quantizes both geometry and color, where a float32 round trip (0.7 -> 0.69999999,
// -1.4 -> -1.39999998) can flip an exact .5 tie or a gradient tie and change the field.
static lrt::Vec3 dictionary_vec3(const Dictionary &p_entry, const char *p_key) {
	const Variant value = p_entry.get(p_key, Vector3());
	if (value.get_type() == Variant::PACKED_FLOAT64_ARRAY) {
		const PackedFloat64Array array = value;
		if (array.size() >= 3) {
			return lrt::Vec3(array[0], array[1], array[2]);
		}
		return lrt::Vec3();
	}
	const Vector3 vector = value;
	return lrt::Vec3(vector.x, vector.y, vector.z);
}

void LRTVolume::set_boxes(const Array &p_boxes) {
	std::vector<BoxInstance> instances;
	instances.reserve(size_t(p_boxes.size()));
	for (int i = 0; i < p_boxes.size(); i++) {
		const Dictionary entry = p_boxes[i];
		BoxInstance box;
		box.world_min = dictionary_vec3(entry, "min");
		box.world_max = dictionary_vec3(entry, "max");
		box.color = dictionary_vec3(entry, "color");
		box.emission = dictionary_vec3(entry, "emission");
		// The dictionary form is the prototype's world axis-aligned box. Its SDF path is
		// the same box baked in its own frame and placed at the box centre.
		box.local_extent = box.world_max - box.world_min;
		box.transform.origin = (box.world_max + box.world_min) * 0.5;
		instances.push_back(box);
	}
	set_box_instances(instances);
}

// One entry per imported mesh volume: flat triangle soup with no transform. The prototype
// bakes one Color SDF per glTF mesh instance, so the caller supplies that grouping.
void LRTVolume::set_meshes(const Array &p_meshes) {
	std::vector<MeshInstance> instances;
	instances.reserve(size_t(p_meshes.size()));
	std::map<uint64_t, std::shared_ptr<const std::vector<lrt::MeshTriangle>>> shared_triangles;
	for (int volume = 0; volume < p_meshes.size(); volume++) {
		const Dictionary entry = p_meshes[volume];
		const PackedVector3Array vertices = entry.get("vertices", PackedVector3Array());
		PackedVector3Array colors = entry.get("colors", PackedVector3Array());
		if (vertices.size() < 3) {
			continue;
		}
		if (colors.size() != vertices.size()) {
			colors.resize(vertices.size());
			for (int i = 0; i < colors.size(); i++) {
				colors.set(i, Vector3(1, 1, 1));
			}
		}
		MeshInstance instance;
		instance.sdf_resolution = mesh_sdf_resolution;
		const int count = vertices.size() / 3;
		instance.transform = lrt::PrimitiveTransform();
		std::shared_ptr<std::vector<lrt::MeshTriangle>> triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
		triangles->reserve(size_t(count));
		for (int i = 0; i < count; i++) {
			lrt::MeshTriangle triangle;
			for (int v = 0; v < 3; v++) {
				const Vector3 position = vertices[i * 3 + v];
				const Vector3 color = colors[i * 3 + v];
				triangle.position[v] = lrt::Vec3(position.x, position.y, position.z);
				triangle.color[v] = lrt::Vec3(color.x, color.y, color.z);
			}
			triangles->push_back(triangle);
		}
		instance.material_signature = lrt::material_field_input_signature(*triangles, nullptr);
		const uint64_t input_signature = lrt::instance_field_cache_signature(
				lrt::asset_signature(*triangles, mesh_sdf_resolution), instance.material_signature);
		const auto shared = shared_triangles.find(input_signature);
		if (shared != shared_triangles.end()) {
			instance.triangles = shared->second;
		} else {
			instance.triangles = triangles;
			shared_triangles[input_signature] = std::move(triangles);
		}
		instances.push_back(std::move(instance));
	}
	set_mesh_instances(instances);
}

void LRTVolume::set_box_instances(const std::vector<BoxInstance> &p_boxes) {
	box_instances = p_boxes;
	has_staged = false;
}

bool LRTVolume::set_mesh_instances(const std::vector<MeshInstance> &p_meshes, bool p_cull_to_volume) {
	ERR_FAIL_COND_V(!configured, false);
	std::vector<MeshInstance> instances;
	std::map<const std::vector<lrt::MeshTriangle> *, AABB> geometry_bounds;
	// Include the halo of every trunk, including the final partially occupied trunk.
	const Vector3 region_min(grid.min.x - grid.spacing, grid.min.y - grid.spacing, grid.min.z - grid.spacing);
	Vector3 region_size;
	for (int axis = 0; axis < 3; axis++) {
		region_size[axis] = (((grid.size[axis] + lrt::TRUNK - 1) / lrt::TRUNK) * lrt::TRUNK + 2) * grid.spacing;
	}
	const AABB region(region_min, region_size);
	for (const MeshInstance &source : p_meshes) {
		if (source.precomputed) {
			auto append = [&](const lrt::PrimitiveTransform &p_transform) {
				lrt::GeometryInstance primitive;
				if (!lrt::make_precomputed_instance(*source.precomputed, p_transform, source.support_radius,
							source.occupancy_band_width, source.transfer_signatures, primitive, source.layer_mask)) {
					return false;
				}
				const lrt::Vec3 extent = primitive.transfer_support_extent();
				const lrt::Vec3 low = primitive.bounds_min - extent;
				const lrt::Vec3 high = primitive.bounds_max + extent;
				const AABB bounds(Vector3(low.x, low.y, low.z), Vector3(high.x - low.x, high.y - low.y, high.z - low.z));
				if (!p_cull_to_volume || region.intersects_inclusive(bounds)) {
					MeshInstance instance = source;
					instance.transform = p_transform;
					instance.mesh_to_volume = p_transform;
					instance.instanced = false;
					instance.copies.reset();
					instances.push_back(std::move(instance));
				}
				return true;
			};
			if (source.instanced) {
				ERR_FAIL_COND_V(!source.copies, false);
				for (const lrt::MeshCopy &copy : *source.copies) {
					if (!append(source.transform * copy.transform)) {
						return false;
					}
				}
			} else if (!append(source.transform)) {
				return false;
			}
			continue;
		}
		if (source.raster_capture) {
			const auto &capture = source.raster_capture;
			const auto &field = capture->geometry;
			if (field.cells.empty()) {
				continue;
			}
			const AABB local_bounds(
					Vector3(field.min.x, field.min.y, field.min.z) - Vector3(1, 1, 1) * (field.cell * 0.5),
					Vector3(field.size[0], field.size[1], field.size[2]) * field.cell);
			auto append = [&](const lrt::PrimitiveTransform &p_transform) {
				MeshInstance instance = source;
				instance.mesh_to_volume = p_transform;
				instance.transform = p_transform * capture->transform;
				instance.instanced = false;
				instance.copies.reset();
				const auto &value = instance.transform;
				const Transform3D transform(Basis(
						Vector3(value.basis_x.x, value.basis_x.y, value.basis_x.z),
						Vector3(value.basis_y.x, value.basis_y.y, value.basis_y.z),
						Vector3(value.basis_z.x, value.basis_z.y, value.basis_z.z)),
						Vector3(value.origin.x, value.origin.y, value.origin.z));
				AABB bounds = transform.xform(local_bounds);
				const lrt::Vec3 support = p_transform.xform_extent(lrt::Vec3(source.support_radius, source.support_radius, source.support_radius));
				const Vector3 extent = Vector3(support.x, support.y, support.z) + Vector3(1, 1, 1) * (grid.spacing * 0.5);
				bounds.position -= extent;
				bounds.size += extent * 2;
				if (!p_cull_to_volume || region.intersects_inclusive(bounds)) {
					instances.push_back(std::move(instance));
				}
			};
			if (source.instanced) {
				ERR_FAIL_COND_V(!source.copies, false);
				for (const lrt::MeshCopy &copy : *source.copies) {
					append(source.transform * copy.transform);
				}
			} else {
				append(source.transform);
			}
			continue;
		}
		if (!source.instanced) {
			MeshInstance instance = source;
			instance.mesh_to_volume = source.transform;
			instances.push_back(std::move(instance));
			continue;
		}
		ERR_FAIL_COND_V(!source.copies, false);
		MeshInstance instance = source;
		instance.copies.reset();
		instance.instanced = false;
		for (const lrt::MeshCopy &copy : *source.copies) {
			instance.transform = source.transform * copy.transform;
			instance.mesh_to_volume = instance.transform;
			if (copy.triangles->empty()) {
				continue;
			}
			auto bounds = geometry_bounds.find(copy.triangles.get());
			if (bounds == geometry_bounds.end()) {
				const lrt::Vec3 &first = copy.triangles->front().position[0];
				AABB local_bounds(Vector3(first.x, first.y, first.z), Vector3());
				for (const lrt::MeshTriangle &triangle : *copy.triangles) {
					for (const lrt::Vec3 &position : triangle.position) {
						local_bounds.expand_to(Vector3(position.x, position.y, position.z));
					}
				}
				bounds = geometry_bounds.emplace(copy.triangles.get(), local_bounds).first;
			}
			const lrt::PrimitiveTransform &transform = instance.transform;
			const Basis basis(
					Vector3(transform.basis_x.x, transform.basis_x.y, transform.basis_x.z),
					Vector3(transform.basis_y.x, transform.basis_y.y, transform.basis_y.z),
					Vector3(transform.basis_z.x, transform.basis_z.y, transform.basis_z.z));
			const Transform3D world_transform(basis, Vector3(transform.origin.x, transform.origin.y, transform.origin.z));
			if (p_cull_to_volume && !region.intersects_inclusive(world_transform.xform(bounds->second.grow(source.support_radius)).grow(grid.spacing * 0.5))) {
				continue;
			}
			instance.material_transform = copy.transform;
			instance.triangles = copy.triangles;
			instance.material_signature = lrt::primitive_signature(source.material_signature, 0, copy.transform);
			instances.push_back(instance);
		}
	}
	mesh_instances = std::move(instances);
	has_staged = false;
	return true;
}

void LRTVolume::request_cancel() {
	cancel_flag.store(true);
}

void LRTVolume::clear_cancel() {
	cancel_flag.store(false);
}

bool LRTVolume::is_cancel_requested() const {
	return cancel_flag.load();
}

bool LRTVolume::has_local_field() const {
	return has_local;
}

void LRTVolume::set_mesh_sdf_resolution(int p_resolution) {
	mesh_sdf_resolution = MAX(8, p_resolution);
}

bool LRTVolume::reset_native_lights(int p_count) {
	ERR_FAIL_COND_V(p_count < 0, false);
	const int capacity = MAX(1, p_count);
	const bool resize_buffers = capacity != native_light_capacity;
	{
		MutexLock lock(params_mutex);
		native_light_capacity = capacity;
		native_light_count = p_count;
		native_light_states.assign(size_t(native_light_capacity), NativeLightState());
		for (int i = 0; i < p_count; i++) {
			native_light_states[size_t(i)].enabled = true;
		}
	}
	if (has_local) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL_V(rendering_server, false);
		if (resize_buffers) {
			rendering_server->call_on_render_thread(
					callable_mp(this, &LRTVolume::_resize_native_light_buffers_render_thread).bind(native_light_capacity));
		}
	}
	return true;
}

void LRTVolume::set_native_light_scale(int p_slot, const Vector3 &p_scale) {
	ERR_FAIL_INDEX(p_slot, native_light_count);
	MutexLock lock(params_mutex);
	native_light_states[p_slot].scale = p_scale;
	native_light_states[p_slot].enabled = true;
}

void LRTVolume::set_native_light_influence(int p_slot, const Vector3 &p_origin, float p_radius, uint32_t p_cull_mask) {
	ERR_FAIL_INDEX(p_slot, native_light_count);
	MutexLock lock(params_mutex);
	NativeLightState &state = native_light_states[p_slot];
	state.influence_origin = p_origin;
	state.influence_radius = p_radius;
	state.cull_mask = p_cull_mask;
}


void LRTVolume::queue_direct_native_light_resolve(const NativeLightResolve &p_resolve) {
	MutexLock lock(native_resolve_mutex);
	queued_direct_resolves.push_back(p_resolve);
}

void LRTVolume::commit_queued_direct_resolves() {
	std::vector<NativeLightResolve> queued;
	{
		MutexLock lock(native_resolve_mutex);
		if (queued_direct_resolves.empty()) {
			return;
		}
		queued.swap(queued_direct_resolves);
	}
	for (const NativeLightResolve &resolve : queued) {
		_resolve_native_light_field(resolve);
	}
}

void LRTVolume::_resolve_native_light_field(const NativeLightResolve &p_resolve) {
	bool schedule = false;
	{
		MutexLock lock(native_resolve_mutex);
		pending_native_resolves.push_back(p_resolve);
		if (!native_resolve_pending.exchange(true)) {
			native_resolve_error = String();
			schedule = true;
		}
	}
	if (schedule) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTVolume::_resolve_native_lights_render_thread));
	}
}

String LRTVolume::get_native_light_resolve_error() {
	MutexLock lock(native_resolve_mutex);
	return native_resolve_error;
}

void LRTVolume::commit_native_light_input(int p_slot, uint64_t p_instance_id, uint64_t p_input_usec) {
	ERR_FAIL_INDEX(p_slot, native_light_count);
	MutexLock lock(params_mutex);
	NativeLightState &state = native_light_states[p_slot];
	state.instance_id = p_instance_id;
	state.input_usec = p_input_usec;
}

bool LRTVolume::is_native_light_resolve_pending() const {
	if (native_resolve_pending.load()) {
		return true;
	}
	MutexLock lock(native_resolve_mutex);
	return !queued_direct_resolves.empty();
}

void LRTVolume::set_sky(const Vector3 &p_sky) {
	PackedVector4Array radiance;
	radiance.resize(3);
	for (int channel = 0; channel < 3; channel++) {
		radiance.set(channel, Vector4(p_sky[channel] / SH_C0, 0.0, 0.0, 0.0));
	}
	set_sky_radiance(radiance);
}

void LRTVolume::set_sky_radiance(const PackedVector4Array &p_radiance) {
	ERR_FAIL_COND_MSG(p_radiance.size() != 3, "LRT sky radiance requires exactly three RGB SH4 coefficient vectors.");
	MutexLock lock(params_mutex);
	bool changed = false;
	for (int channel = 0; channel < 3; channel++) {
		changed = changed || sky_radiance[channel] != p_radiance[channel];
	}
	if (!changed) {
		return;
	}
	for (int channel = 0; channel < 3; channel++) {
		sky_radiance[channel] = p_radiance[channel];
	}
	const double weight = 4.0 * Math::PI / lrt::DIRECTION_COUNT;
	for (int index = 0; index < lrt::DIRECTION_COUNT; index++) {
		const lrt::Vec3 &direction = lrt::directions()[index].direction;
		const Vector4 basis(SH_C0, SH_C1 * direction.x, SH_C1 * direction.y, SH_C1 * direction.z);
		const float projected[4] = {
			float(weight * SH_C0),
			float(weight * SH_C1 * direction.x),
			float(weight * SH_C1 * direction.y),
			float(weight * SH_C1 * direction.z),
		};
		for (int channel = 0; channel < 3; channel++) {
			const float radiance = MAX(0.0f, sky_radiance[channel].dot(basis));
			for (int component = 0; component < 4; component++) {
				sky_link_radiance[channel][index][component] = projected[component] * radiance;
			}
		}
	}
	sky_projection_dirty.store(true);
}

PackedVector4Array LRTVolume::get_sky_radiance() const {
	PackedVector4Array result;
	result.resize(3);
	for (int channel = 0; channel < 3; channel++) {
		result.set(channel, sky_radiance[channel]);
	}
	return result;
}

Ref<Image> LRTVolume::read_environment_panorama(const Ref<Environment> &p_environment, const Vector2i &p_size) {
	if (p_environment.is_null()) {
		return Ref<Image>();
	}
	const Environment::BGMode background = p_environment->get_background();
	const Environment::AmbientSource ambient = p_environment->get_ambient_source();
	// Only the sources that actually light geometry in the engine count: the sky as a
	// background or ambient, the ambient color, and a background color used as ambient.
	const bool uses_sky = p_environment->get_sky().is_valid() &&
			(background == Environment::BG_SKY || ambient == Environment::AMBIENT_SOURCE_SKY);
	const bool uses_ambient = ambient == Environment::AMBIENT_SOURCE_COLOR ||
			(ambient == Environment::AMBIENT_SOURCE_BG && background != Environment::BG_SKY);
	if (!uses_sky && !uses_ambient) {
		return Ref<Image>();
	}
	const Size2i size(MAX(1, p_size.x), MAX(1, p_size.y));
	return RS::get_singleton()->environment_bake_panorama(p_environment->get_rid(), false, size);
}

// The engine's own environment readout for ambient and sky lighting. This historical
// scalar API returns the spherical mean; production uses read_environment_radiance_sh().
Vector3 LRTVolume::read_environment_radiance(const Ref<Environment> &p_environment, const Vector2i &p_size) {
	const PackedVector4Array coefficients = read_environment_radiance_sh(p_environment, p_size, Basis());
	if (coefficients.size() != 3) {
		return Vector3();
	}
	return Vector3(coefficients[0].x, coefficients[1].x, coefficients[2].x) * SH_C0;
}

// Projects Godot's linear HDR environment panorama into the same real SH2 convention as
// the LRT fields. The panorama is authored in sky space; p_sky_to_local rotates its l=1
// coefficients into the volume's local frame without touching exposure or tone mapping.
PackedVector4Array LRTVolume::read_environment_radiance_sh(const Ref<Environment> &p_environment, const Vector2i &p_size, const Basis &p_sky_to_local) {
	return project_panorama_radiance_sh(read_environment_panorama(p_environment, p_size), p_sky_to_local);
}

PackedVector4Array LRTVolume::project_panorama_radiance_sh(const Ref<Image> &p_panorama, const Basis &p_sky_to_local) const {
	PackedVector4Array result;
	result.resize(3);
	result.fill(Vector4());
	if (p_panorama.is_null() || p_panorama->is_empty()) {
		return result;
	}
	const Size2i size = p_panorama->get_size();
	double weight_sum = 0.0;
	Vector4 sums[3];
	for (int y = 0; y < size.y; y++) {
		const double theta = Math::PI * (y + 0.5) / size.y;
		const double weight = Math::sin(theta);
		weight_sum += weight * size.x;
		for (int x = 0; x < size.x; x++) {
			const double phi = Math::TAU * (x + 0.5) / size.x;
			// Must match CopyEffects::copy_octmap_to_panorama() exactly: the baked panorama's
			// equirectangular convention negates X and Z.
			const Vector3 sky_direction(-Math::sin(theta) * Math::sin(phi), Math::cos(theta), -Math::sin(theta) * Math::cos(phi));
			const Vector3 local_direction = p_sky_to_local.xform(sky_direction).normalized();
			const Vector4 basis(SH_C0, SH_C1 * local_direction.x, SH_C1 * local_direction.y, SH_C1 * local_direction.z);
			const Color texel = p_panorama->get_pixel(x, y);
			sums[0] += basis * (texel.r * weight);
			sums[1] += basis * (texel.g * weight);
			sums[2] += basis * (texel.b * weight);
		}
	}
	if (weight_sum <= 0.0) {
		return result;
	}
	const double solid_angle_scale = Math::TAU * 2.0 / weight_sum;
	for (int channel = 0; channel < 3; channel++) {
		Vector4 coefficient = sums[channel] * solid_angle_scale;
		const Vector3 directional(coefficient.y, coefficient.z, coefficient.w);
		// Midpoint quadrature leaves round-off-scale l=1 residue for a mathematically constant
		// panorama. Canonicalize only that numerical residue so rotating a uniform sky remains
		// exactly input-stable and does not schedule false source updates.
		if (directional.length() <= MAX(1e-7, Math::abs(coefficient.x) * 1e-7)) {
			coefficient.y = 0.0;
			coefficient.z = 0.0;
			coefficient.w = 0.0;
		}
		result.set(channel, coefficient);
	}
	return result;
}

bool LRTVolume::set_debug_fields(uint32_t p_fields) {
	if (requested_debug_fields.load() == p_fields) {
		return false;
	}
	requested_debug_fields.store(p_fields);
	if (!has_local || apply_pending || !device) {
		local_debug_textures_dirty = p_fields != 0;
		return true;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, true);
	rendering_server->call_on_render_thread(
			callable_mp(this, &LRTVolume::_set_debug_fields_render_thread).bind(p_fields));
	return true;
}

void LRTVolume::set_external_gi_enabled(bool p_enabled) {
	if (external_gi_enabled.exchange(p_enabled) == p_enabled || !device || !has_local || apply_pending) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_set_external_gi_enabled_render_thread).bind(p_enabled));
	rendering_server->sync();
}

bool LRTVolume::has_external_gi_buffers() const {
	return external_gi_buffers_full_size.load();
}

Error LRTVolume::_ensure_device() {
	if (device) {
		return OK;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, ERR_UNAVAILABLE);
	device = rendering_server->get_rendering_device();
	ERR_FAIL_NULL_V(device, ERR_UNAVAILABLE);
	static const char *pass_names[GPU_TIMING_PASS_COUNT] = { "Inject", "Light Resolve", "Propagate" };
	for (int pass = 0; pass < GPU_TIMING_PASS_COUNT; pass++) {
		timestamp_begin_names[pass] = vformat("LRT %d %s Begin", get_instance_id(), pass_names[pass]);
		timestamp_end_names[pass] = vformat("LRT %d %s End", get_instance_id(), pass_names[pass]);
	}
	return OK;
}

Error LRTVolume::_create_shaders() {
	if (shader_inject.is_valid()) {
		return OK;
	}
	ERR_FAIL_COND_V(lrt_shared_shaders.device && lrt_shared_shaders.device != device, ERR_ALREADY_IN_USE);
	if (lrt_shared_shaders.pipelines[0].is_valid()) {
		shader_inject = lrt_shared_shaders.shaders[0];
		shader_propagate = lrt_shared_shaders.shaders[1];
		shader_sky_project = lrt_shared_shaders.shaders[2];
		shader_local_patch = lrt_shared_shaders.shaders[3];
		shader_sky_solve = lrt_shared_shaders.shaders[4];
		pipeline_inject = lrt_shared_shaders.pipelines[0];
		pipeline_propagate = lrt_shared_shaders.pipelines[1];
		pipeline_sky_project = lrt_shared_shaders.pipelines[2];
		pipeline_local_patch = lrt_shared_shaders.pipelines[3];
		pipeline_sky_solve = lrt_shared_shaders.pipelines[4];
		return OK;
	}
	lrt_shared_shaders.device = device;
	const String offsets = direction_initializer();
	const String sources[5] = {
		String(lrt_inject_shader_glsl).replace("%LRT_DIRECTIONS%", offsets),
		String(lrt_propagate_shader_glsl).replace("%LRT_DIRECTIONS%", offsets).replace("%LRT_BOUNDARY%", lrt_boundary_inc_shader_glsl),
		String(lrt_sky_project_shader_glsl).replace("%LRT_DIRECTIONS%", offsets),
		String(lrt_local_patch_shader_glsl),
		String(lrt_sky_solve_shader_glsl).replace("%LRT_DIRECTIONS%", offsets),
	};
	const char *shader_names[5] = {
		"LRT injection shader",
		"LRT propagation shader",
		"LRT sky projection shader",
		"LRT local patch shader",
		"LRT sky visibility shader",
	};
	RID shaders[5] = { shader_inject, shader_propagate, shader_sky_project, shader_local_patch, shader_sky_solve };
	for (int i = 0; i < 5; i++) {
		if (lrt_shared_shaders.spirv[i].is_null()) {
			Ref<RDShaderFile> shader_file;
			shader_file.instantiate();
			const Error parse_error = shader_file->parse_versions_from_text(sources[i]);
			if (parse_error != OK) {
				shader_file->print_errors(shader_names[i]);
				return parse_error;
			}
			lrt_shared_shaders.spirv[i] = shader_file->get_spirv();
		}
		shaders[i] = device->shader_create_from_spirv(lrt_shared_shaders.spirv[i]->get_stages());
		ERR_FAIL_COND_V(shaders[i].is_null(), ERR_CANT_CREATE);
		lrt_shared_shaders.shaders[i] = shaders[i];
	}
	shader_inject = shaders[0];
	shader_propagate = shaders[1];
	shader_sky_project = shaders[2];
	shader_local_patch = shaders[3];
	shader_sky_solve = shaders[4];
	pipeline_inject = device->compute_pipeline_create(shader_inject);
	pipeline_propagate = device->compute_pipeline_create(shader_propagate);
	pipeline_sky_project = device->compute_pipeline_create(shader_sky_project);
	pipeline_local_patch = device->compute_pipeline_create(shader_local_patch);
	pipeline_sky_solve = device->compute_pipeline_create(shader_sky_solve);
	ERR_FAIL_COND_V(pipeline_inject.is_null() || pipeline_propagate.is_null() ||
			pipeline_sky_project.is_null() || pipeline_local_patch.is_null() || pipeline_sky_solve.is_null(), ERR_CANT_CREATE);
	lrt_shared_shaders.pipelines = { pipeline_inject, pipeline_propagate,
		pipeline_sky_project, pipeline_local_patch, pipeline_sky_solve };
	return OK;
}

Error LRTVolume::_create_buffers() {
	ERR_FAIL_COND_V(_create_grid_buffers() != OK, ERR_CANT_CREATE);
	return _create_content_buffers();
}

RID LRTVolume::_create_diagnostic_texture(int p_width, int p_height, const std::vector<float> *p_values, Ref<LRTDisplayTexture> &r_texture) {
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	format.width = p_width;
	format.height = p_height;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	Vector<Vector<uint8_t>> initial_data;
	if (p_values && !p_values->empty()) {
		initial_data.push_back(bytes_of(p_values->data(), p_values->size() * sizeof(float)));
	}
	RID texture_rid = device->texture_create(format, RD::TextureView(), initial_data);
	ERR_FAIL_COND_V(texture_rid.is_null(), RID());
	if (!p_values || p_values->empty()) {
		device->texture_clear(texture_rid, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	r_texture.instantiate();
	r_texture->set_owned_texture(texture_rid);
	return texture_rid;
}

Error LRTVolume::_create_debug_textures(uint32_t p_fields) {
	const uint32_t changed_fields = p_fields ^ allocated_debug_fields.load();
	auto width_for = [&](uint32_t p_field) { return (p_fields & p_field) ? grid.width : 1; };
	auto height_for = [&](uint32_t p_field) { return (p_fields & p_field) ? grid.height : 1; };
	if (diagnostic_dirty_texture_rid.is_null() || (changed_fields & DEBUG_DIRTY)) {
		const std::vector<float> dirty_pixels = (p_fields & DEBUG_DIRTY) ? dirty_mask_rgba(local.dirty_mask) : std::vector<float>();
		diagnostic_dirty_texture_rid = _create_diagnostic_texture(width_for(DEBUG_DIRTY), height_for(DEBUG_DIRTY),
				(p_fields & DEBUG_DIRTY) ? &dirty_pixels : nullptr, diagnostic_dirty_texture);
	}
	ERR_FAIL_COND_V(diagnostic_dirty_texture_rid.is_null(), ERR_CANT_CREATE);
	allocated_debug_fields.store(p_fields);
	return OK;
}

void LRTVolume::_set_debug_fields_render_thread(uint32_t p_fields) {
	if (!device || !has_local || apply_pending || allocated_debug_fields.load() == p_fields) {
		return;
	}

	if (!(p_fields & DEBUG_COARSE_GEOMETRY)) {
		_free_debug_geometry_resources();
	}
	if (_create_debug_textures(p_fields) != OK) {
		return;
	}
	if (p_fields != 0) {
		_upload_local_textures();
	} else {
		local_debug_textures_dirty = true;
	}
}

void LRTVolume::_set_external_gi_enabled_render_thread(bool p_enabled) {
	if (!device || !has_local || apply_pending || external_gi_buffers_full_size.load() == p_enabled) {
		return;
	}
	_free_uniform_sets();
	const uint32_t bytes = p_enabled ? uint32_t(lrt::boundary_connection_count(grid.size[0], grid.size[1], grid.size[2]) * 4 * sizeof(float)) : 16;
	device->free_rid(external_gi_buffer);
	external_gi_buffer = device->storage_buffer_create(bytes);
	if (external_gi_buffer.is_null() || device->buffer_clear(external_gi_buffer, 0, bytes) != OK || _create_uniform_sets() != OK) {
		ERR_PRINT("LRT boundary resource allocation failed; excluding the Volume from composition.");
		_free_gpu_resources();
		return;
	}
	external_gi_buffers_full_size.store(p_enabled);
}

// Grid histories survive geometry edits. Sparse local records and Source grow independently
// without discarding propagated radiance or visibility.
Error LRTVolume::_create_grid_buffers() {
	local_grid_banks_synchronized = false;

	const int count = grid.count;
	params_buffer = device->uniform_buffer_create(sizeof(ParamsData));
	uploaded_params_data.clear();

	material_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	local_visibility_buffer = device->storage_buffer_create(count * sizeof(lrt::SH4));
	local_record_capacity.store(MAX(uint32_t(1), uint32_t(local.local_records.size())));
	matrix_buffer = device->storage_buffer_create(local_record_capacity.load() * sizeof(lrt::LocalRecord));
	record_index_buffer = device->storage_buffer_create(count * sizeof(uint32_t));
	staged_material_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	staged_local_visibility_buffer = device->storage_buffer_create(count * sizeof(lrt::SH4));
	staged_matrix_buffer = device->storage_buffer_create(local_record_capacity.load() * sizeof(lrt::LocalRecord));
	staged_record_index_buffer = device->storage_buffer_create(count * sizeof(uint32_t));
	local_patch_buffer = device->storage_buffer_create(uint32_t(MIN(size_t(MAX_LOCAL_PATCH_PROBES), size_t(count)) * sizeof(LocalPatchData)));
	for (int i = 0; i < 3; i++) {
		source_buffers[i] = device->storage_buffer_create(local_record_capacity.load() * 4 * sizeof(float));
	}
	const uint32_t external_bytes = external_gi_enabled.load() ? uint32_t(lrt::boundary_connection_count(grid.size[0], grid.size[1], grid.size[2]) * 4 * sizeof(float)) : 16;
	external_gi_buffer = device->storage_buffer_create(external_bytes);
	ERR_FAIL_COND_V(external_gi_buffer.is_null(), ERR_CANT_CREATE);
	ERR_FAIL_COND_V(device->buffer_clear(external_gi_buffer, 0, external_bytes) != OK, ERR_CANT_CREATE);
	external_gi_buffers_full_size.store(external_gi_enabled.load());
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			radiance_buffers[buffer][channel] = device->storage_buffer_create(count * 4 * sizeof(float));
		}
		sky_visibility_work_buffers[buffer] = device->storage_buffer_create(count * sizeof(uint32_t));
	}
	published_sky_visibility_buffer = device->storage_buffer_create(count * sizeof(lrt::SH4));
	for (int channel = 0; channel < 3; channel++) {
		sky_buffers[channel] = device->storage_buffer_create(count * 4 * sizeof(float));
	}
	for (int channel = 0; channel < 3; channel++) {
		ERR_FAIL_COND_V(source_buffers[channel].is_null() || sky_buffers[channel].is_null(), ERR_CANT_CREATE);
		for (int buffer = 0; buffer < 2; buffer++) {
			ERR_FAIL_COND_V(radiance_buffers[buffer][channel].is_null(), ERR_CANT_CREATE);
		}
	}
	ERR_FAIL_COND_V(params_buffer.is_null() || material_buffer.is_null() || local_visibility_buffer.is_null() ||
					matrix_buffer.is_null() || record_index_buffer.is_null() || staged_record_index_buffer.is_null() ||
					staged_material_buffer.is_null() ||
					staged_local_visibility_buffer.is_null() || staged_matrix_buffer.is_null() ||
					local_patch_buffer.is_null() ||
					sky_visibility_work_buffers[0].is_null() || sky_visibility_work_buffers[1].is_null() ||
					published_sky_visibility_buffer.is_null(),
			ERR_CANT_CREATE);
	_clear_source_buffers();
	for (RID buffer : sky_visibility_work_buffers) {
		device->buffer_clear(buffer, 0, count * sizeof(uint32_t));
	}
	device->buffer_clear(published_sky_visibility_buffer, 0, count * sizeof(lrt::SH4));
	for (RID buffer : sky_buffers) {
		device->buffer_clear(buffer, 0, count * 4 * sizeof(float));
	}
	sky_published_version.store(0);
	sky_current = 0;
	return _create_debug_textures(requested_debug_fields.load());
}

Error LRTVolume::_grow_local_record_buffers() {
	const uint32_t old_capacity = local_record_capacity.load();
	const uint32_t required = uint32_t(local.local_records.size());
	if (required <= old_capacity) {
		return OK;
	}
	const uint32_t capacity = MIN(uint32_t(grid.count), MAX(required, old_capacity * 2));
	RID next[5];
	const RID previous[5] = { matrix_buffer, staged_matrix_buffer, source_buffers[0], source_buffers[1], source_buffers[2] };
	auto release_next = [&]() {
		for (RID buffer : next) {
			if (buffer.is_valid()) {
				device->free_rid(buffer);
			}
		}
	};
	for (int buffer = 0; buffer < 5; buffer++) {
		const uint32_t stride = buffer < 2 ? sizeof(lrt::LocalRecord) : 4 * sizeof(float);
		next[buffer] = device->storage_buffer_create(capacity * stride);
		if (next[buffer].is_null()) {
			release_next();
			return ERR_CANT_CREATE;
		}
		if (buffer >= 2) {
			device->buffer_clear(next[buffer], 0, capacity * stride);
		}
		if (device->buffer_copy(previous[buffer], next[buffer], 0, 0, old_capacity * stride) != OK) {
			release_next();
			return ERR_CANT_ACQUIRE_RESOURCE;
		}
	}
	for (RID buffer : previous) {
		device->free_rid(buffer);
	}
	matrix_buffer = next[0];
	staged_matrix_buffer = next[1];
	for (int channel = 0; channel < 3; channel++) {
		source_buffers[channel] = next[channel + 2];
	}
	local_record_capacity.store(capacity);
	return OK;
}

// Native lighting retains only parameters, atlas inputs and Trunk candidate IDs.
Error LRTVolume::_create_content_buffers() {
	native_light_capacity = MAX(1, native_light_count);
	allocated_native_light_capacity = native_light_capacity;
	native_light_states.resize(size_t(native_light_capacity));
	native_light_state_buffer = device->storage_buffer_create(sizeof(NativeLightStateData) * native_light_capacity);
	const uint64_t trunks = uint64_t((grid.size[0] + 7) / 8) * ((grid.size[1] + 7) / 8) * ((grid.size[2] + 7) / 8);
	native_light_trunk_bytes.store((5 + trunks) * sizeof(uint32_t));
	native_light_trunk_buffer = device->storage_buffer_create(uint32_t(native_light_trunk_bytes.load()));
	for (RID &texture : native_light_textures) {
		texture = RID();
	}
	uploaded_native_light_state_data.clear();
	uploaded_light_trunk_inputs.clear();
	light_trunks_dirty = true;
	RD::SamplerState sampler_state;
	sampler_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
	sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	native_light_sampler = device->sampler_create(sampler_state);
	RD::SamplerState linear_sampler_state = sampler_state;
	linear_sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear_sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	linear_sampler_state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	native_light_linear_sampler = device->sampler_create(linear_sampler_state);
	RD::TextureFormat dummy_format;
	dummy_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	dummy_format.width = 1;
	dummy_format.height = 1;
	dummy_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	native_light_dummy_texture = device->texture_create(dummy_format, RD::TextureView());
	if (native_light_dummy_texture.is_valid()) {
		device->texture_clear(native_light_dummy_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	ERR_FAIL_COND_V(native_light_trunk_buffer.is_null() ||
			native_light_state_buffer.is_null() || native_light_sampler.is_null() || native_light_linear_sampler.is_null() ||
			native_light_dummy_texture.is_null(),
			ERR_CANT_CREATE);
	ERR_FAIL_COND_V(directional_shadows.update(device, AABB(), {}, 512, 0.0f, Transform3D()) != OK, ERR_CANT_CREATE);
	return OK;
}

Error LRTVolume::_create_uniform_sets() {
	ERR_FAIL_COND_V(_create_inject_uniform_sets() != OK, ERR_CANT_CREATE);
	auto make_uniform = [](RD::UniformType p_type, uint32_t p_binding, RID p_id) {
		RD::Uniform uniform;
		uniform.uniform_type = p_type;
		uniform.binding = p_binding;
		uniform.append_id(p_id);
		return uniform;
	};
	for (int bank = 0; bank < 2; bank++) {
		Vector<RD::Uniform> patch_uniforms;
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, local_patch_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1,
				bank == 0 ? material_buffer : staged_material_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2,
				bank == 0 ? local_visibility_buffer : staged_local_visibility_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3,
				bank == 0 ? matrix_buffer : staged_matrix_buffer));
		for (int channel = 0; channel < 3; channel++) {
			patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10 + channel, source_buffers[channel]));
		}

		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9,
				bank == 0 ? record_index_buffer : staged_record_index_buffer));
		RID &patch_set = bank == 0 ? uniform_set_local_patch : staged_uniform_set_local_patch;
		patch_set = device->uniform_set_create(patch_uniforms, shader_local_patch, 0);
		ERR_FAIL_COND_V(patch_set.is_null(), ERR_CANT_CREATE);
	}
	for (int bank = 0; bank < 2; bank++) {
		for (int buffer = 0; buffer < 2; buffer++) {
			Vector<RD::Uniform> uniforms;
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1,
					bank == 0 ? material_buffer : staged_material_buffer));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2,
				bank == 0 ? local_visibility_buffer : staged_local_visibility_buffer));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3,
				bank == 0 ? matrix_buffer : staged_matrix_buffer));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4,
					bank == 0 ? record_index_buffer : staged_record_index_buffer));
			for (int i = 0; i < 3; i++) {
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, source_buffers[i]));
			}
			for (int i = 0; i < 3; i++) {
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9 + i, radiance_buffers[buffer][i]));
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13 + i, radiance_buffers[1 - buffer][i]));
			}
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, external_gi_buffer));
			RID &propagate_set = bank == 0 ? uniform_set_propagate[buffer] : staged_uniform_set_propagate[buffer];
			propagate_set = device->uniform_set_create(uniforms, shader_propagate, 0);
			ERR_FAIL_COND_V(propagate_set.is_null(), ERR_CANT_CREATE);

			Vector<RD::Uniform> sky_uniforms;
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, bank == 0 ? material_buffer : staged_material_buffer));
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, published_sky_visibility_buffer));
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, sky_visibility_work_buffers[buffer]));
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, sky_visibility_work_buffers[1 - buffer]));
			RID &sky_set = bank == 0 ? uniform_set_sky_solve[buffer] : staged_uniform_set_sky_solve[buffer];
			sky_set = device->uniform_set_create(sky_uniforms, shader_sky_solve, 0);
			ERR_FAIL_COND_V(sky_set.is_null(), ERR_CANT_CREATE);
		}
	}
	Vector<RD::Uniform> projection;
	projection.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
	projection.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, published_sky_visibility_buffer));
	for (int channel = 0; channel < 3; channel++) {
		projection.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19 + channel, sky_buffers[channel]));
	}
	uniform_set_sky_project = device->uniform_set_create(projection, shader_sky_project, 0);
	ERR_FAIL_COND_V(uniform_set_sky_project.is_null(), ERR_CANT_CREATE);

	return OK;
}

void LRTVolume::_free_uniform_sets() {
	if (!device) {
		return;
	}
	RID sets[13] = { uniform_set_inject, staged_uniform_set_inject,
		uniform_set_propagate[0], uniform_set_propagate[1],
		staged_uniform_set_propagate[0], staged_uniform_set_propagate[1],
		uniform_set_sky_project,
		uniform_set_sky_solve[0], uniform_set_sky_solve[1], staged_uniform_set_sky_solve[0], staged_uniform_set_sky_solve[1],

		uniform_set_local_patch, staged_uniform_set_local_patch };
	for (RID &set : sets) {
		if (set.is_valid() && device->uniform_set_is_valid(set)) {
			device->free_rid(set);
			set = RID();
		}
	}
	uniform_set_inject = RID();
	staged_uniform_set_inject = RID();
	uniform_set_propagate[0] = RID();
	uniform_set_propagate[1] = RID();
	staged_uniform_set_propagate[0] = RID();
	staged_uniform_set_propagate[1] = RID();
	uniform_set_sky_project = RID();
	for (int buffer = 0; buffer < 2; buffer++) {
		uniform_set_sky_solve[buffer] = RID();
		staged_uniform_set_sky_solve[buffer] = RID();
	}

	uniform_set_local_patch = RID();
	staged_uniform_set_local_patch = RID();
}

void LRTVolume::_free_content_buffers() {
	if (!device) {
		return;
	}
	RID content[5] = { native_light_trunk_buffer, native_light_state_buffer, native_light_sampler,
		native_light_linear_sampler, native_light_dummy_texture };
	native_light_trunk_buffer = RID();
	native_light_trunk_bytes.store(0);
	native_light_state_buffer = RID();
	native_light_sampler = RID();
	native_light_linear_sampler = RID();
	native_light_dummy_texture = RID();
	allocated_native_light_capacity = 0;
	for (const RID &buffer : content) {
		if (buffer.is_valid()) {
			device->free_rid(buffer);
		}
	}
}

void LRTVolume::_free_gpu_resources() {
	has_local = false;
	if (!device) {
		return;
	}
	directional_shadows.clear(device);
	_free_debug_geometry_resources();
	_free_uniform_sets();
	_free_content_buffers();
	has_applied_grid = false;

	RID buffers[64];
	int buffer_count = 0;
	buffers[buffer_count++] = params_buffer;

	buffers[buffer_count++] = material_buffer;
	buffers[buffer_count++] = local_visibility_buffer;
	buffers[buffer_count++] = matrix_buffer;
	buffers[buffer_count++] = record_index_buffer;
	buffers[buffer_count++] = staged_record_index_buffer;
	buffers[buffer_count++] = staged_material_buffer;
	buffers[buffer_count++] = staged_local_visibility_buffer;
	buffers[buffer_count++] = staged_matrix_buffer;
	buffers[buffer_count++] = local_patch_buffer;
	params_buffer = RID();

	material_buffer = RID();
	local_visibility_buffer = RID();
	matrix_buffer = RID();
	record_index_buffer = RID();
	staged_record_index_buffer = RID();
	local_record_capacity.store(0);
	staged_material_buffer = RID();
	staged_local_visibility_buffer = RID();
	staged_matrix_buffer = RID();
	local_patch_buffer = RID();
	for (int i = 0; i < 3; i++) {
		buffers[buffer_count++] = source_buffers[i];
		source_buffers[i] = RID();
	}
	buffers[buffer_count++] = external_gi_buffer;
	external_gi_buffer = RID();
	external_gi_buffers_full_size.store(false);
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			buffers[buffer_count++] = radiance_buffers[buffer][channel];
			radiance_buffers[buffer][channel] = RID();
		}
		buffers[buffer_count++] = sky_visibility_work_buffers[buffer];
		sky_visibility_work_buffers[buffer] = RID();
	}
	buffers[buffer_count++] = published_sky_visibility_buffer;
	published_sky_visibility_buffer = RID();
	sky_visibility_iterations_remaining.store(0);
	for (int channel = 0; channel < 3; channel++) {
		buffers[buffer_count++] = sky_buffers[channel];
		sky_buffers[channel] = RID();
	}
	for (int i = 0; i < buffer_count; i++) {
		if (buffers[i].is_valid()) {
			device->free_rid(buffers[i]);
		}
	}

	diagnostic_dirty_texture.unref();

	diagnostic_dirty_texture_rid = RID();
	local_grid_banks_synchronized = false;
	pipeline_inject = RID();
	pipeline_propagate = RID();
	pipeline_sky_project = RID();
	pipeline_sky_solve = RID();

	pipeline_local_patch = RID();
	shader_inject = RID();
	shader_propagate = RID();
	shader_sky_project = RID();
	shader_sky_solve = RID();

	shader_local_patch = RID();
}

void LRTVolume::free_shared_gpu_resources() {
	if (!lrt_shared_shaders.device) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server && !rendering_server->is_on_render_thread()) {
		rendering_server->call_on_render_thread(callable_mp_static(&LRTVolume::free_shared_gpu_resources));
		rendering_server->sync();
		return;
	}
	for (const RID &pipeline : lrt_shared_shaders.pipelines) {
		if (pipeline.is_valid()) {
			lrt_shared_shaders.device->free_rid(pipeline);
		}
	}
	for (const RID &shader : lrt_shared_shaders.shaders) {
		if (shader.is_valid()) {
			lrt_shared_shaders.device->free_rid(shader);
		}
	}
	lrt_shared_shaders = SharedShaderResources();
}

void LRTVolume::_include_source_probes(const Vector3i &p_minimum, const Vector3i &p_maximum) {
	const Vector3i grid_size(grid.size[0], grid.size[1], grid.size[2]);
	const Vector3i low = p_minimum.clamp(Vector3i(), grid_size);
	const Vector3i high = p_maximum.clamp(Vector3i(), grid_size);
	if (low.x >= high.x || low.y >= high.y || low.z >= high.z) {
		return;
	}
	if (source_dirty_minimum == source_dirty_maximum) {
		source_dirty_minimum = low;
		source_dirty_maximum = high;
	} else {
		source_dirty_minimum = source_dirty_minimum.min(low);
		source_dirty_maximum = source_dirty_maximum.max(high);
	}
}

void LRTVolume::_include_source_light(const SourceLightSnapshot &p_light) {
	if (p_light.radius < 0.0f) {
		source_full_dirty = true;
		return;
	}
	Vector3i low;
	Vector3i high;
	for (int axis = 0; axis < 3; axis++) {
		const double minimum = (p_light.origin[axis] - p_light.radius - grid.min[axis]) / grid.spacing;
		const double maximum = (p_light.origin[axis] + p_light.radius - grid.min[axis]) / grid.spacing;
		low[axis] = int(Math::floor(CLAMP(minimum, 0.0, double(grid.size[axis]))));
		high[axis] = int(Math::ceil(CLAMP(maximum, 0.0, double(grid.size[axis]))));
	}
	_include_source_probes(low, high);
}

LRTVolume::SourceRegion LRTVolume::_consume_source_region() {
	// params_mutex protects the exact light snapshot consumed by this Source dispatch.
	std::map<RID, SourceLightSnapshot> current_lights;
	for (int slot = 0; slot < native_light_count; slot++) {
		const NativeLightState &state = native_light_states[slot];
		if (!state.enabled || !state.has_input || state.cull_mask == 0) {
			continue;
		}
		const uint8_t *parameters = uploaded_native_light_state_data.ptr() + slot * sizeof(NativeLightStateData);
		NativeLightStateData data;
		memcpy(&data, parameters, sizeof(data));
		if (data.color_kind[3] == 0.0f) {
			continue;
		}
		SourceLightSnapshot next;
		next.parameters = bytes_of(parameters, sizeof(data));
		next.origin = state.influence_origin;
		next.radius = state.influence_radius;
		next.signature = state.input.source_signature;
		if (data.shadow_parameters[0] > 0.0f) {
			next.textures[state.input.directional ? 0 : 1] = native_light_textures[state.input.directional ? 0 : 1];
		}
		if (state.input.area) {
			next.textures[2] = native_light_textures[2];
		}
		if (state.input.projector_requested) {
			next.textures[3] = native_light_textures[3];
		}
		const auto previous = source_light_snapshots.find(state.input.scene_light_instance);
		if (previous == source_light_snapshots.end()) {
			_include_source_light(next);
		} else {
			const SourceLightSnapshot &old = previous->second;
			if (old.origin != next.origin || old.radius != next.radius || old.signature != next.signature ||
					old.textures != next.textures || memcmp(old.parameters.ptr(), parameters, sizeof(data)) != 0) {
				_include_source_light(old);
				_include_source_light(next);
			}
		}
		current_lights.emplace(state.input.scene_light_instance, std::move(next));
	}
	for (const auto &previous : source_light_snapshots) {
		if (current_lights.count(previous.first) == 0) {
			_include_source_light(previous.second);
		}
	}
	source_light_snapshots = std::move(current_lights);
	if (source_full_dirty) {
		source_dirty_minimum = Vector3i();
		source_dirty_maximum = Vector3i(grid.size[0], grid.size[1], grid.size[2]);
	}
	SourceRegion region;
	for (int axis = 0; axis < 3; axis++) {
		region.minimum[axis] = source_dirty_minimum[axis];
		region.size[axis] = source_dirty_maximum[axis] - source_dirty_minimum[axis];
	}
	region.size[3] = region.size[0] * region.size[1] * region.size[2];
	source_dirty_minimum = Vector3i();
	source_dirty_maximum = Vector3i();
	source_full_dirty = false;
	return region;
}

bool LRTVolume::_upload_params(std::vector<NativeLightInput> *r_light_inputs, bool p_upload_lights, SourceRegion *r_source_region) {
	MutexLock lock(params_mutex);
	if (r_light_inputs != nullptr) {
		r_light_inputs->clear();
		for (int i = 0; i < native_light_count; i++) {
			const NativeLightState &state = native_light_states[size_t(i)];
			NativeLightInput input;
			input.instance_id = state.instance_id;
			input.oldest_usec = state.input_usec;
			input.newest_usec = input.oldest_usec;
			r_light_inputs->push_back(input);
		}
	}
	ParamsData params = {};
	params.grid_size[0] = grid.size[0];
	params.grid_size[1] = grid.size[1];
	params.grid_size[2] = grid.size[2];
	params.grid_size[3] = grid.count;
	params.grid_min[0] = float(grid.min.x);
	params.grid_min[1] = float(grid.min.y);
	params.grid_min[2] = float(grid.min.z);
	params.grid_min[3] = float(grid.spacing);
	params.counts[1] = native_light_count;
	params.counts[2] = SKY_DIRECTION_COUNT;
	params.counts[3] = (allocated_debug_fields.load() & DEBUG_SKY_VISIBILITY) ? 1 : 0;
	params.flags[0] = 0.0f;
	params.flags[1] = receive_sky.load() ? 1.0f : 0.0f;
	params.flags[2] = local_backend == "sdf" ? 1.0f : 0.0f;
	memcpy(params.sky_link_radiance, sky_link_radiance, sizeof(sky_link_radiance));
	if (p_upload_lights && !_upload_native_light_state_buffer()) {
		return false;
	}
	if (uploaded_params_data.size() != sizeof(params) || memcmp(uploaded_params_data.ptr(), &params, sizeof(params)) != 0) {
		ERR_FAIL_COND_V(device->buffer_update(params_buffer, 0, sizeof(params), &params) != OK, false);
		uploaded_params_data = bytes_of(&params, sizeof(params));
	}
	if (r_source_region != nullptr) {
		*r_source_region = _consume_source_region();
	}
	return true;
}

void LRTVolume::_clear_source_buffers() {
	source_full_dirty = true;
	source_light_snapshots.clear();
	for (RID buffer : source_buffers) {
		device->buffer_clear(buffer, 0, local_record_capacity.load() * 4 * sizeof(float));
	}

}

void LRTVolume::_upload_local_buffers() {
	if (local.material.empty()) {
		return;
	}
	const uint64_t buffers_started_usec = OS::get_singleton()->get_ticks_usec();
	device->buffer_update(material_buffer, 0, local.material.size() * sizeof(float), local.material.data());
	device->buffer_update(staged_material_buffer, 0, local.material.size() * sizeof(float), local.material.data());
	device->buffer_update(local_visibility_buffer, 0, local.local_visibility.size() * sizeof(lrt::SH4), local.local_visibility.data());
	device->buffer_update(staged_local_visibility_buffer, 0, local.local_visibility.size() * sizeof(lrt::SH4), local.local_visibility.data());
	device->buffer_update(record_index_buffer, 0, local.local_record_indices.size() * sizeof(uint32_t), local.local_record_indices.data());
	device->buffer_update(staged_record_index_buffer, 0, local.local_record_indices.size() * sizeof(uint32_t), local.local_record_indices.data());
	if (!local.gpu_local_records.empty()) {
		device->buffer_update(matrix_buffer, 0, local.gpu_local_records.size() * sizeof(float), local.gpu_local_records.data());
		device->buffer_update(staged_matrix_buffer, 0, local.gpu_local_records.size() * sizeof(float), local.gpu_local_records.data());
	}
	apply_full_upload_bytes += 2 * (local.material.size() * sizeof(float) + local.local_visibility.size() * sizeof(lrt::SH4) +
			local.gpu_local_records.size() * sizeof(float) + local.local_record_indices.size() * sizeof(uint32_t));
	local_grid_banks_synchronized = true;
	apply_buffer_upload_ms = double(OS::get_singleton()->get_ticks_usec() - buffers_started_usec) / 1000.0;
	const uint64_t textures_started_usec = OS::get_singleton()->get_ticks_usec();

	if (requested_debug_fields.load() == 0) {
		local_debug_textures_dirty = true;
		apply_texture_upload_ms = double(OS::get_singleton()->get_ticks_usec() - textures_started_usec) / 1000.0;
		return;
	}
	_upload_local_textures();
	apply_texture_upload_ms = double(OS::get_singleton()->get_ticks_usec() - textures_started_usec) / 1000.0;
}

void LRTVolume::_upload_staged_local_buffers() {
	// Stage the complete snapshot in one render-thread submission. RenderingDevice records
	// asynchronous transfers; inserting a frame wait between transfer blocks only makes the
	// visible field lag behind continuous input, particularly for large dirty regions.
	if (apply_sparse_patch) {
		if (local.dirty_trunk_count == 0) {
			return;
		}
		if (!staged_local_patches.empty()) {
			const uint32_t bytes = uint32_t(staged_local_patches.size() * sizeof(LocalPatchData));
			device->buffer_update(local_patch_buffer, 0, bytes, staged_local_patches.data());
			apply_local_patch_upload_bytes += bytes;
		}
		if (!local_grid_banks_synchronized) {
			device->buffer_copy(material_buffer, staged_material_buffer, 0, 0, uint32_t(local.material.size() * sizeof(float)));
			device->buffer_copy(local_visibility_buffer, staged_local_visibility_buffer, 0, 0, uint32_t(local.local_visibility.size() * sizeof(lrt::SH4)));
			device->buffer_copy(matrix_buffer, staged_matrix_buffer, 0, 0, local_record_capacity.load() * sizeof(lrt::LocalRecord));
			device->buffer_copy(record_index_buffer, staged_record_index_buffer, 0, 0, uint32_t(local.local_record_indices.size() * sizeof(uint32_t)));
		}
		return;
	}

	struct BufferUpload {
		RID target;
		const void *data;
		size_t bytes;
	};
	const BufferUpload uploads[] = {
		{ staged_material_buffer, local.material.data(), local.material.size() * sizeof(float) },
		{ staged_local_visibility_buffer, local.local_visibility.data(), local.local_visibility.size() * sizeof(lrt::SH4) },
		{ staged_matrix_buffer, local.gpu_local_records.data(), local.gpu_local_records.size() * sizeof(float) },
		{ staged_record_index_buffer, local.local_record_indices.data(), local.local_record_indices.size() * sizeof(uint32_t) },
	};
	for (const BufferUpload &upload : uploads) {
		if (upload.bytes == 0) {
			continue;
		}
		device->buffer_update(upload.target, 0, uint32_t(upload.bytes), upload.data);
		apply_full_upload_bytes += upload.bytes;
	}
}

void LRTVolume::_upload_local_textures() {
	const uint32_t fields = allocated_debug_fields.load();
	if (fields & DEBUG_DIRTY) {
		const std::vector<float> dirty_pixels = dirty_mask_rgba(local.dirty_mask);
		device->texture_update(diagnostic_dirty_texture_rid, 0, bytes_of(dirty_pixels.data(), dirty_pixels.size() * sizeof(float)));
	}
	local_debug_textures_dirty = false;
}

// The bake already runs on a worker thread, so it spreads over the remaining cores; the cap
// keeps a 32-thread machine from oversubscribing memory bandwidth for no gain.
static int lrt_bake_thread_count() {
	return CLAMP(OS::get_singleton()->get_processor_count() - 1, 1, 16);
}

// Geometry distance fields are immutable and process-wide shared by content + precision.
// Material fields use a separate content cache, so recolouring never changes the SDF.
bool LRTVolume::_build_primitives(const String &p_backend, int p_threads, std::vector<lrt::GeometryInstance> &r_primitives,
		std::vector<lrt::Box> &r_boxes) {
	if (p_backend == "analytic") {
		for (const BoxInstance &box : box_instances) {
			if (!box.axis_aligned) {
				return false;
			}
			lrt::Box analytic;
			analytic.min = box.world_min;
			analytic.max = box.world_max;
			analytic.color = box.color;
			r_boxes.push_back(analytic);
		}
		return true;
	}

	r_primitives.reserve(box_instances.size() + mesh_instances.size());
	std::set<uint64_t> active_specs;
	std::set<const lrt::SurfaceMaterialField *> active_instance_fields;
	std::set<int> active_resolutions;
	auto record_primitive = [&](uint64_t p_signature, const std::shared_ptr<const lrt::CoarseGeometryField> &p_geometry,
									const std::shared_ptr<const lrt::SurfaceMaterialField> &p_instance, const lrt::PrimitiveTransform &p_transform, uint32_t p_layer_mask, uint64_t p_instance_cache_signature, const lrt::RasterGeometryCapture *p_capture = nullptr, const MeshInstance *p_mesh = nullptr) {
		if (active_specs.insert(p_signature).second) {
			sdf_bytes += lrt::asset_field_bytes(*p_geometry);
		}
		if (active_instance_fields.insert(p_instance.get()).second) {
			instance_field_bytes += uint64_t(sizeof(lrt::SurfaceMaterialField)) +
					uint64_t(p_instance->albedo.capacity()) + uint64_t(p_instance->emission.capacity() * sizeof(float)) + p_instance->emission_sides.capacity();
		}
		const uint64_t material_signature = p_capture != nullptr ? p_capture->material_signature : lrt::instance_field_signature(*p_instance);
		r_primitives.push_back(lrt::make_sdf_primitive(p_geometry, p_instance, p_transform,
				lrt::primitive_signature(p_signature, material_signature, p_transform), p_layer_mask,
				p_signature, material_signature, p_capture != nullptr ? &p_capture->material_signatures : nullptr));
		lrt::GeometryInstance &primitive = r_primitives.back();
		primitive.instance_cache_signature = p_instance_cache_signature;
		if (p_mesh != nullptr) {
			primitive.support_radius = p_mesh->support_radius;
			primitive.mesh_to_volume = p_mesh->mesh_to_volume;
		}
		if (p_mesh != nullptr && p_mesh->static_gi) {
			primitive.static_triangles = p_mesh->triangles;
			primitive.static_mesh_signature = p_mesh->static_mesh_signature;
		}
	};
	for (const BoxInstance &box : box_instances) {
		const uint64_t field_signature = lrt::box_field_signature(box.local_extent, box.sdf_resolution);
		std::shared_ptr<const lrt::CoarseGeometryField> geometry = lrt::find_shared_asset_field(field_signature);
		if (!geometry) {
			lrt::CoarseGeometryField baked;
			if (!lrt::load_asset_field(field_signature, baked)) {
				baked = lrt::bake_box_sdf(box.local_extent, box.sdf_resolution, &cancel_flag);
			}
			if (baked.cells.empty()) {
				preparation_error = cancel_flag.load() ? lrt::MESH_SDF_BAKE_CANCELLED : lrt::MESH_SDF_BAKE_TOO_LARGE;
				return false;
			}
			geometry = lrt::share_asset_field(field_signature, std::move(baked));
		}
		lrt::SurfaceMaterialField baked_instance = lrt::bake_constant_instance_field(box.color, box.emission, box.emission_sides);
		const uint64_t instance_signature = lrt::instance_field_cache_signature(field_signature, lrt::instance_field_signature(baked_instance));
		std::shared_ptr<const lrt::SurfaceMaterialField> instance = lrt::find_shared_instance_field(instance_signature);
		if (!instance) {
			instance = lrt::share_instance_field(instance_signature, std::move(baked_instance));
		}
		record_primitive(field_signature, geometry, instance, box.transform, box.layer_mask, instance_signature);
	}

	// One job owns each unique geometry + precision pair. Instance material fields are handled
	// separately below, so repeated meshes never duplicate the geometry SDF.
	struct AssetJob {
		int instance = -1;
		uint64_t signature = 0;
		lrt::TriangleMesh mesh;
		lrt::MeshSdfBakeResult baked;
		std::shared_ptr<const lrt::CoarseGeometryField> field;
		double bake_ms = 0.0;
	};
	std::vector<AssetJob> jobs;
	std::vector<int> job_of_instance(mesh_instances.size(), -1);
	std::map<uint64_t, int> job_by_signature;
	for (int i = 0; i < int(mesh_instances.size()); i++) {
		const MeshInstance &instance = mesh_instances[size_t(i)];
		if (instance.precomputed || instance.raster_capture || !instance.triangles || instance.triangles->empty()) {
			continue;
		}
		active_resolutions.insert(instance.sdf_resolution);
		const uint64_t signature_begin = OS::get_singleton()->get_ticks_usec();
		const uint64_t signature = lrt::asset_signature(*instance.triangles, instance.sdf_resolution);
		signature_ms += double(OS::get_singleton()->get_ticks_usec() - signature_begin) / 1000.0;
		const auto identical = job_by_signature.find(signature);
		if (identical != job_by_signature.end()) {
			job_of_instance[size_t(i)] = identical->second;
			continue;
		}
		job_by_signature[signature] = int(jobs.size());
		AssetJob job;
		job.instance = i;
		job.signature = signature;
		job.field = lrt::find_shared_asset_field(signature);
		assets_memory += job.field ? 1 : 0;
		job_of_instance[size_t(i)] = int(jobs.size());
		jobs.push_back(std::move(job));
	}
	assets_requested = int(jobs.size());
	preparation_total.store(assets_requested);
	preparation_completed.store(assets_memory);
	// A previous run may already have baked this exact asset; the derived cache is keyed by the
	// triangle content, so an edited mesh simply misses it.
	const uint64_t cache_read_begin = OS::get_singleton()->get_ticks_usec();
	for (AssetJob &job : jobs) {
		if (job.field) {
			continue;
		}
		lrt::CoarseGeometryField loaded;
		if (lrt::load_asset_field(job.signature, loaded)) {
			job.field = lrt::share_asset_field(job.signature, std::move(loaded));
			assets_loaded++;
			preparation_completed.fetch_add(1);
		}
	}
	cache_read_ms += double(OS::get_singleton()->get_ticks_usec() - cache_read_begin) / 1000.0;
	// Resolve each unique coarse geometry specification once before assembling instance inputs.
	int baked_assets = 0;
	auto bake_job = [&](int p_job, int p_job_threads) {
		AssetJob &job = jobs[size_t(p_job)];
		if (job.field || cancel_flag.load()) {
			return;
		}
		const uint64_t topology_begin = OS::get_singleton()->get_ticks_usec();
		job.mesh = lrt::build_triangle_mesh(*mesh_instances[size_t(job.instance)].triangles);
		topology_ms += double(OS::get_singleton()->get_ticks_usec() - topology_begin) / 1000.0;
		const uint64_t bake_begin = OS::get_singleton()->get_ticks_usec();
		job.baked = lrt::bake_mesh_sdf(job.mesh, mesh_instances[size_t(job.instance)].sdf_resolution, &cancel_flag, p_job_threads);
		job.bake_ms = double(OS::get_singleton()->get_ticks_usec() - bake_begin) / 1000.0;
		if (job.baked.error == lrt::MESH_SDF_BAKE_OK) {
			preparation_completed.fetch_add(1);
		}
	};
	for (int job_index = 0; job_index < int(jobs.size()); job_index++) {
		bake_job(job_index, p_threads);
	}
	if (cancel_flag.load()) {
		return false;
	}
	const uint64_t cache_write_begin = OS::get_singleton()->get_ticks_usec();
	for (AssetJob &job : jobs) {
		if (job.field) {
			continue;
		}
		if (job.baked.error != lrt::MESH_SDF_BAKE_OK || job.baked.field.cells.empty()) {
			preparation_error = int(job.baked.error);
			return false;
		}
		baked_assets++;
		lrt::store_asset_field(job.signature, job.baked.field);
		job.field = lrt::share_asset_field(job.signature, std::move(job.baked.field));
	}
	cache_write_ms += double(OS::get_singleton()->get_ticks_usec() - cache_write_begin) / 1000.0;
	assets_baked += baked_assets;
	assets_prepared = assets_requested;
	for (const AssetJob &job : jobs) {
		if (!job.field) {
			continue;
		}
		closed_mesh_assets += job.field->closed_shell_count > 0 ? 1 : 0;
		open_mesh_assets += job.field->open_shell_count > 0 ? 1 : 0;
		surface_voxels += job.field->surface_voxels;
		sdf_samples += job.field->cells.size();
		if (job.field->cells.size() > largest_sdf_samples) {
			largest_sdf_samples = job.field->cells.size();
			largest_sdf_triangles = int(mesh_instances[size_t(job.instance)].triangles->size());
		}
		voxelize_ms += job.baked.voxelize_ms;
		flood_fill_ms += job.baked.flood_fill_ms;
		distance_ms += job.baked.distance_ms;
		longest_asset_bake_ms = MAX(longest_asset_bake_ms, job.bake_ms);
	}
	const uint64_t instance_field_begin = OS::get_singleton()->get_ticks_usec();
	for (int i = 0; i < int(mesh_instances.size()); i++) {
		const MeshInstance &instance = mesh_instances[size_t(i)];
		if (instance.precomputed) {
			lrt::GeometryInstance primitive;
			if (!lrt::make_precomputed_instance(*instance.precomputed, instance.transform, instance.support_radius,
						instance.occupancy_band_width, instance.transfer_signatures, primitive, instance.layer_mask)) {
				return false;
			}
			r_primitives.push_back(std::move(primitive));
			continue;
		}
		if (instance.raster_capture) {
			const auto &capture = instance.raster_capture;
			std::shared_ptr<const lrt::CoarseGeometryField> geometry(capture, &capture->geometry);
			std::shared_ptr<const lrt::SurfaceMaterialField> material(capture, &capture->material);
			const uint64_t material_signature = lrt::instance_field_cache_signature(capture->signature, capture->material_signature);
			if (active_specs.find(capture->signature) == active_specs.end()) {
				closed_mesh_assets += geometry->closed_shell_count > 0 ? 1 : 0;
				open_mesh_assets += geometry->open_shell_count > 0 ? 1 : 0;
				surface_voxels += geometry->surface_voxels;
				sdf_samples += geometry->cells.size();
				if (geometry->cells.size() > largest_sdf_samples) {
					largest_sdf_samples = geometry->cells.size();
					largest_sdf_triangles = 0;
				}
			}
			active_resolutions.insert(instance.sdf_resolution);
			record_primitive(capture->signature, geometry, material, instance.transform, instance.layer_mask, material_signature, capture.get(), &instance);
			continue;
		}
		if (!instance.triangles || instance.triangles->empty()) {
			continue;
		}
		const int job_index = job_of_instance[size_t(i)];
		if (job_index < 0 || !jobs[size_t(job_index)].field) {
			return false;
		}
		AssetJob &job = jobs[size_t(job_index)];
		const uint64_t material_signature = lrt::instance_field_cache_signature(job.signature, instance.material_signature);
		std::shared_ptr<const lrt::SurfaceMaterialField> material = lrt::find_shared_instance_field(material_signature);
		if (!material) {
			lrt::SurfaceMaterialField loaded;
			if (lrt::load_instance_field(material_signature, loaded)) {
				material = lrt::share_instance_field(material_signature, std::move(loaded));
			}
		}
		if (!material) {
			if (job.mesh.triangles.empty()) {
				const uint64_t topology_begin = OS::get_singleton()->get_ticks_usec();
				job.mesh = lrt::build_triangle_mesh(*instance.triangles);
				topology_ms += double(OS::get_singleton()->get_ticks_usec() - topology_begin) / 1000.0;
			}
			const MeshInstance &representative = mesh_instances[size_t(job.instance)];
			lrt::TriangleMesh instance_mesh;
			const lrt::TriangleMesh *material_mesh = &job.mesh;
			if (!instance.material && instance.triangles != representative.triangles) {
				const uint64_t topology_begin = OS::get_singleton()->get_ticks_usec();
				instance_mesh = lrt::build_triangle_mesh(*instance.triangles);
				topology_ms += double(OS::get_singleton()->get_ticks_usec() - topology_begin) / 1000.0;
				material_mesh = &instance_mesh;
			}
			lrt::SurfaceMaterialField baked_material = lrt::bake_mesh_instance_field(*material_mesh, *job.field, instance.material.get(), &cancel_flag, p_threads, instance.material_transform);
			if (baked_material.albedo.empty()) {
				return false;
			}
			material = lrt::share_instance_field(material_signature, std::move(baked_material));
			if (!lrt::store_instance_field(material_signature, *material)) {
				WARN_PRINT("Cannot persist the LRT instance material field.");
			}
		}
		record_primitive(job.signature, job.field, material, instance.transform, instance.layer_mask, material_signature, nullptr, &instance);
	}
	instance_field_ms += double(OS::get_singleton()->get_ticks_usec() - instance_field_begin) / 1000.0;
	sdf_specs = int(active_specs.size());
	sdf_instance_references = int(std::count_if(r_primitives.begin(), r_primitives.end(), [](const lrt::GeometryInstance &p_instance) { return !p_instance.precomputed; }));
	sdf_resolutions.assign(active_resolutions.begin(), active_resolutions.end());
	return true;
}

int LRTVolume::_mesh_instance_count() const {
	int count = 0;
	for (const MeshInstance &instance : mesh_instances) {
		if (instance.precomputed || (instance.triangles && !instance.triangles->empty())) {
			count++;
		}
	}
	return count;
}

uint64_t LRTVolume::_input_bytes() const {
	uint64_t bytes = vector_bytes(box_instances) + vector_bytes(mesh_instances);
	std::set<const std::vector<lrt::MeshTriangle> *> triangle_sets;
	std::set<const lrt::MaterialCapture *> material_sets;
	std::set<const lrt::TransferInstance *> transfer_instances;
	std::set<const lrt::TransferVisibilityField *> visibility_fields;
	std::set<const lrt::TransferMaterialField *> transfer_materials;
	std::set<const lrt::TransferCoverage *> coverages;
	std::set<const lrt::TransferGeometry *> transfer_geometries;
	for (const MeshInstance &instance : mesh_instances) {
		if (instance.precomputed) {
			const auto &asset = *instance.precomputed;
			if (transfer_instances.insert(&asset).second) {
				bytes += sizeof(lrt::TransferInstance);
			}
			if (visibility_fields.insert(asset.visibility.get()).second) {
				bytes += transfer_field_bytes(*asset.visibility);
			}
			if (transfer_materials.insert(asset.material.get()).second) {
				bytes += transfer_field_bytes(*asset.material);
			}
			const auto &coverage = *asset.coverage;
			if (coverages.insert(&coverage).second) {
				bytes += sizeof(lrt::TransferCoverage) + vector_bytes(coverage.surfaces) + vector_bytes(coverage.triangle_mapping);
				for (const auto &surface : coverage.surfaces) {
					bytes += vector_bytes(surface.covered);
				}
			}
			if (transfer_geometries.insert(coverage.geometry.get()).second) {
				bytes += coverage.geometry->payload_bytes();
			}
			if (triangle_sets.insert(coverage.triangles.get()).second) {
				bytes += vector_bytes(*coverage.triangles);
			}
		}
		if (instance.triangles && triangle_sets.insert(instance.triangles.get()).second) {
			bytes += vector_bytes(*instance.triangles);
		}
		if (instance.material && material_sets.insert(instance.material.get()).second) {
			bytes += sizeof(lrt::MaterialCapture) + material_capture_bytes(*instance.material);
		}
	}
	return bytes;
}

uint64_t LRTVolume::_active_cpu_bytes() const {
	return _input_bytes() + local_field_bytes(local) + local_cache_bytes(local_cache) +
			primitive_bytes(primitives) + vector_bytes(pending_changed_probes) +
			sdf_bytes + instance_field_bytes + spatial_index_payload_bytes.load();
}

uint64_t LRTVolume::_staged_cpu_bytes() const {
	return local_field_bytes(staged_local) + local_cache_bytes(staged_cache) +
			local_field_bytes(recycled_local) + local_cache_bytes(recycled_cache) +
			primitive_bytes(staged_primitives) + vector_bytes(staged_local_patches);
}

uint64_t LRTVolume::_gpu_bytes(Dictionary *r_breakdown) const {
	if (!has_local) {
		if (r_breakdown != nullptr) {
			(*r_breakdown)["allocated"] = false;
			(*r_breakdown)["total_bytes"] = uint64_t(0);
		}
		return 0;
	}

	const uint64_t probe_count = uint64_t(grid.count);
	const uint64_t params_bytes = sizeof(ParamsData);

	const uint64_t material_bytes = probe_count * 4 * sizeof(float);
	const uint64_t local_visibility_bytes = probe_count * sizeof(lrt::SH4);
	const uint64_t local_record_bytes = uint64_t(local_record_capacity.load()) * sizeof(lrt::LocalRecord);
	const uint64_t record_index_bytes = probe_count * sizeof(uint32_t);
	const uint64_t staged_local_field_bytes = material_bytes + local_visibility_bytes + local_record_bytes + record_index_bytes;
	const uint64_t local_patch_bytes = MIN(uint64_t(MAX_LOCAL_PATCH_PROBES), probe_count) * sizeof(LocalPatchData);
	const uint64_t source_bytes = uint64_t(local_record_capacity.load()) * 3 * 4 * sizeof(float);
	const uint64_t external_gi_bytes = external_gi_buffers_full_size.load() ? lrt::boundary_connection_count(grid.size[0], grid.size[1], grid.size[2]) * 4 * sizeof(float) : uint64_t(16);
	const uint64_t radiance_history_bytes = probe_count * 2 * 3 * 4 * sizeof(float);
	const uint64_t sky_projection_bytes = probe_count * 3 * 4 * sizeof(float);
	const uint64_t sky_visibility_bytes = probe_count * (sizeof(lrt::SH4) + 2 * sizeof(uint32_t));
	const uint32_t debug_fields = allocated_debug_fields.load();
	const uint64_t grid_storage_bytes = params_bytes + material_bytes + local_visibility_bytes + local_record_bytes +
			record_index_bytes + source_bytes + external_gi_bytes + radiance_history_bytes +
			sky_visibility_bytes + sky_projection_bytes + staged_local_field_bytes +
			local_patch_bytes;
	// Debug descriptors stay valid with 1x1 placeholders; only requested fields use the grid atlas.
	const uint64_t runtime_texture_bytes = 0;
	const uint64_t diagnostic_texture_bytes = debug_texture_bytes(debug_fields, probe_count);
	const uint64_t native_trunk_bytes = native_light_trunk_bytes.load();
	const uint64_t native_light_state_bytes = sizeof(NativeLightStateData) * uint64_t(allocated_native_light_capacity);
	const uint64_t native_light_input_texture_bytes = native_light_dummy_texture.is_valid() ? 4 : 0;
	const uint64_t total_bytes = grid_storage_bytes + runtime_texture_bytes + diagnostic_texture_bytes +
			native_trunk_bytes + native_light_state_bytes + native_light_input_texture_bytes + debug_geometry_buffer_bytes.load() + directional_shadows.get_bytes();
	if (r_breakdown == nullptr) {
		return total_bytes;
	}
	Dictionary &result = *r_breakdown;

	result["allocated"] = true;
	result["probe_count"] = probe_count;
	result["native_light_capacity"] = native_light_capacity;
	result["directional_shadow"] = directional_shadows.get_stats();
	result["params_bytes"] = params_bytes;

	result["material_bytes"] = material_bytes;
	result["local_visibility_bytes"] = local_visibility_bytes;
	result["local_record_bytes"] = local_record_bytes;
	result["record_index_bytes"] = record_index_bytes;
	result["local_record_capacity"] = local_record_capacity.load();
	result["staged_local_field_bytes"] = staged_local_field_bytes;
	result["local_patch_bytes"] = local_patch_bytes;
	result["source_bytes"] = source_bytes;
	result["external_gi_bytes"] = external_gi_bytes;
	result["radiance_history_bytes"] = radiance_history_bytes;
	result["sky_projection_bytes"] = sky_projection_bytes;
	result["sky_visibility_bytes"] = sky_visibility_bytes;
	result["sky_visibility_encoding"] = "sh4_f32";
	result["sky_visibility_work_encoding"] = "direction26_u32";
	result["grid_storage_bytes"] = grid_storage_bytes;
	result["runtime_texture_bytes"] = runtime_texture_bytes;
	result["diagnostic_texture_bytes"] = diagnostic_texture_bytes;
	result["geometry_debug_buffer_bytes"] = debug_geometry_buffer_bytes.load();
	result["debug_texture_mask"] = debug_fields;
	result["native_light_trunk_bytes"] = native_trunk_bytes;
	result["native_light_state_bytes"] = native_light_state_bytes;
	result["native_light_input_texture_bytes"] = native_light_input_texture_bytes;
	result["total_bytes"] = total_bytes;
	result["resource_object_overhead_included"] = false;
	return total_bytes;
}

Dictionary LRTVolume::_gpu_memory_breakdown() const {
	Dictionary result;
	_gpu_bytes(&result);
	return result;
}

uint64_t LRTVolume::estimate_staged_gpu_bytes(bool p_preserve_history) const {
	if (!has_staged || grid.count <= 0) {
		return 0;
	}
	return _estimate_field_gpu_bytes(staged_local, p_preserve_history);
}

uint64_t LRTVolume::_estimate_field_gpu_bytes(const lrt::LocalField &p_field, bool p_preserve_history) const {
	const uint64_t probes = uint64_t(grid.count);
	const uint64_t light_capacity = uint64_t(MAX(1, native_light_count));
	const uint32_t debug_fields = requested_debug_fields.load();
	const uint64_t required_records = MAX(uint64_t(1), uint64_t(p_field.local_records.size()));
	const uint64_t previous_records = local_record_capacity.load();
	uint64_t record_capacity = required_records;
	if (p_preserve_history && previous_records != 0) {
		record_capacity = required_records <= previous_records ? previous_records :
				MIN(probes, MAX(required_records, previous_records * 2));
	}
	const uint64_t grid_bytes = sizeof(ParamsData) +
			2 * probes * (8 * sizeof(float) + sizeof(uint32_t)) +
			2 * record_capacity * sizeof(lrt::LocalRecord) +
			record_capacity * 3 * 4 * sizeof(float) +
			(external_gi_enabled.load() ? lrt::boundary_connection_count(grid.size[0], grid.size[1], grid.size[2]) * 4 * sizeof(float) : uint64_t(16)) +
			probes * 2 * 3 * 4 * sizeof(float) +
			probes * 3 * 4 * sizeof(float) + probes * (sizeof(lrt::SH4) + 2 * sizeof(uint32_t)) +
			MIN(uint64_t(MAX_LOCAL_PATCH_PROBES), probes) * sizeof(LocalPatchData);
	const uint64_t runtime_textures = 0;
	const uint64_t debug_textures = debug_texture_bytes(debug_fields, probes);
	const uint64_t trunks = uint64_t((grid.size[0] + 7) / 8) * ((grid.size[1] + 7) / 8) * ((grid.size[2] + 7) / 8);
	const uint64_t native_trunk_bytes = (5 + trunks + light_capacity * (trunks + 1)) * sizeof(uint32_t);
	return grid_bytes + runtime_textures + debug_textures +
			native_trunk_bytes + sizeof(NativeLightStateData) * light_capacity + 4 +
			(p_preserve_history ? debug_geometry_buffer_bytes.load() : 0);
}

// The CPU half of the bake: the part that dominates a cold carriage build. It only touches
// plain data, so LRTVolume3D can run it on a worker thread; every GPU call stays in
// apply_local_field() on the main thread.
LRTVolume::LocalBakeResult LRTVolume::bake_local_field_data(bool p_analytic, bool p_prepare_cached_assets) {
	LocalBakeResult result;
	if (!configured) {
		return result;
	}
	{
		MutexLock lock(params_mutex);
		local_backend = p_analytic ? "analytic" : "sdf";
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	const int full_bake_threads = lrt_bake_thread_count();
	// Cached incremental edits touch only a few trunks, so the full bake fan-out would spend more on
	// scheduling and memory bandwidth than it wins. A bounded fan-out still parallelizes the dirty
	// Trunk work, which at single-thread speed fills an entire 60 Hz frame and delays the publish
	// that the dynamic-geometry response gate measures.
	const int threads = local_cache.local != nullptr ? MIN(full_bake_threads, LRT_INCREMENTAL_BAKE_THREADS) : full_bake_threads;
	preparation_phase.store(1);
	preparation_total.store(0);
	preparation_completed.store(0);
	assets_loaded = 0;
	assets_baked = 0;
	assets_memory = 0;
	assets_requested = 0;
	assets_prepared = 0;
	closed_mesh_assets = 0;
	open_mesh_assets = 0;
	surface_voxels = 0;
	preparation_error = lrt::MESH_SDF_BAKE_OK;
	sdf_specs = 0;
	sdf_instance_references = 0;
	static_occupancy_built.store(0);
	static_occupancy_reused.store(0);
	static_occupancy_candidates.store(0);
	static_occupancy_ms.store(0.0);
	sdf_bytes = 0;
	instance_field_bytes = 0;
	input_bytes = 0;
	active_cpu_bytes = 0;
	staged_cpu_bytes = 0;
	cpu_peak_bytes = 0;
	sdf_scratch_peak_bytes = 0;
	signature_ms = 0.0;
	topology_ms = 0.0;
	cache_read_ms = 0.0;
	voxelize_ms = 0.0;
	flood_fill_ms = 0.0;
	distance_ms = 0.0;
	cache_write_ms = 0.0;
	instance_field_ms = 0.0;
	sdf_samples = 0;
	largest_sdf_samples = 0;
	largest_sdf_triangles = 0;
	longest_asset_bake_ms = 0.0;
	sdf_resolutions.clear();
	std::vector<lrt::GeometryInstance> bake_primitives;
	std::vector<lrt::Box> analytic_boxes;
	if (!_build_primitives(local_backend, threads, bake_primitives, analytic_boxes)) {
		result.cancelled = cancel_flag.load();
		result.needs_axis_aligned = p_analytic && !result.cancelled;
		result.assets_requested = assets_requested;
		result.assets_prepared = preparation_completed.load();
		result.preparation_error = preparation_error;
		preparation_phase.store(result.cancelled ? 0 : 6);
		return result;
	}
	const uint64_t after_assets = OS::get_singleton()->get_ticks_usec();
	// A fingerprint-verified cache load already supplies the complete local field. Its
	// dependency job restores only the primitives; no Trunk solve or GPU upload is needed.
	// Every Trunk digest still matches the previous build: the field, the sparse records and all
	// derived passes are already on the GPU, so nothing here would be solved. Return before the
	// field arrays are allocated, cleared and refilled.
	if (!p_analytic) {
		const uint64_t index_started = OS::get_singleton()->get_ticks_usec();
		spatial_index.update(grid, bake_primitives);
		const uint64_t occupancy_started = OS::get_singleton()->get_ticks_usec();
		signature_ms += double(occupancy_started - index_started) / 1000.0;
		if (!spatial_index.prepare_probe_occupancy(grid, bake_primitives, &cancel_flag, threads)) {
			result.cancelled = true;
			preparation_phase.store(0);
			return result;
		}
		const auto &index_stats = spatial_index.get_update_stats();
		static_occupancy_built.store(index_stats.occupancy_built);
		static_occupancy_reused.store(index_stats.occupancy_reused);
		static_occupancy_candidates.store(index_stats.occupancy_candidates);
		static_occupancy_ms.store(double(OS::get_singleton()->get_ticks_usec() - occupancy_started) / 1000.0);
		spatial_index_payload_bytes.store(spatial_index.payload_bytes());
		spatial_index_registrations.store(spatial_index.get_registration_count());
		spatial_index_candidate_references.store(spatial_index.get_candidate_references());
		spatial_index_overlap_tests.store(spatial_index.get_update_stats().overlap_tests);
	}
	if (!p_analytic && (p_prepare_cached_assets || local_cache.local != nullptr)) {
		const std::vector<uint64_t> &signatures = spatial_index.get_signatures();
		if (p_prepare_cached_assets || (lrt::grid_signature(grid) == local_cache.grid_key &&
				signatures.size() == local_cache.trunk_signatures.size() &&
				signatures == local_cache.trunk_signatures)) {
			const lrt::LocalField &existing_field = local;
			active_cpu_bytes = _active_cpu_bytes();
			staged_cpu_bytes = _staged_cpu_bytes();
			cpu_peak_bytes = active_cpu_bytes + MAX(staged_cpu_bytes, sdf_scratch_peak_bytes);
			result.ok = true;
			result.unchanged = true;
			result.cache_loaded = p_prepare_cached_assets;
			if (p_prepare_cached_assets) {
				staged_primitives = std::move(bake_primitives);
			}
			// The update-region debug view reports what this build solved. Nothing was solved, so
			// the mask goes to zero and the debug textures refresh from it.
			std::fill(local.dirty_mask.begin(), local.dirty_mask.end(), 0);
			local_debug_textures_dirty = true;
			result.solid = existing_field.solid_count;
			result.surface = existing_field.surface_count;
			result.trunks = existing_field.trunk_count;
			result.dirty_trunks = 0;
			result.mismatches = existing_field.classification_mismatches;
			result.mesh_volumes = _mesh_instance_count();
			result.assets_memory = assets_memory;
			result.assets_requested = assets_requested;
			result.assets_prepared = assets_prepared;
			result.assets_loaded = assets_loaded;
			result.assets_baked = assets_baked;
			result.closed_mesh_assets = closed_mesh_assets;
			result.open_mesh_assets = open_mesh_assets;
			result.surface_voxels = surface_voxels;
			result.sdf_specs = sdf_specs;
			result.sdf_instance_references = sdf_instance_references;
			result.sdf_bytes = sdf_bytes;
			result.instance_field_bytes = instance_field_bytes;
			result.input_bytes = input_bytes;
			result.active_cpu_bytes = active_cpu_bytes;
			result.staged_cpu_bytes = staged_cpu_bytes;
			result.cpu_peak_bytes = cpu_peak_bytes;
			result.sdf_scratch_peak_bytes = sdf_scratch_peak_bytes;
			result.signature_ms = signature_ms;
			result.topology_ms = topology_ms;
			result.cache_read_ms = cache_read_ms;
			result.voxelize_ms = voxelize_ms;
			result.flood_fill_ms = flood_fill_ms;
			result.distance_ms = distance_ms;
			result.cache_write_ms = cache_write_ms;
			result.instance_field_ms = instance_field_ms;
			result.sdf_samples = sdf_samples;
			result.largest_sdf_samples = largest_sdf_samples;
			result.largest_sdf_triangles = largest_sdf_triangles;
			result.longest_asset_bake_ms = longest_asset_bake_ms;
			result.sdf_resolutions = sdf_resolutions;
			result.assets_ms = double(after_assets - start) / 1000.0;
			result.build_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
			preparation_phase.store(5);
			return result;
		}
	}
	preparation_phase.store(2);
	const bool active_cache_on_grid = !p_analytic && local_cache.local == &local &&
			local_cache.grid_key == lrt::grid_signature(grid) && local.material.size() == size_t(grid.count) * 4;
	if (p_analytic) {
		staged_local = lrt::build_local_data(grid, lrt::BoxQuery(analytic_boxes), &cancel_flag, threads);
	} else {
		// The incremental path lives in the SDF backend, exactly like the prototype's
		// src/sdf-local.js; the analytic backend has no dirty-region support there either.
		staged_cache = std::move(recycled_cache);
		staged_local = lrt::build_sdf_local_data(
				grid, bake_primitives, &cancel_flag, threads, &local_cache, &staged_cache, &recycled_local, &spatial_index);
	}
	const uint64_t after_local = OS::get_singleton()->get_ticks_usec();
	if (cancel_flag.load()) {
		has_staged = false;
		result.cancelled = true;
		preparation_phase.store(0);
		return result;
	}
	preparation_phase.store(3);
	const lrt::LocalField *previous_local = !p_analytic && local_cache.local != nullptr ? &local : nullptr;
	const bool sparse_patch_valid = staged_local.dirty_trunk_count <= MAX_LOCAL_PATCH_PROBES / PROBES_PER_LOCAL_TRUNK;
	if (previous_local != nullptr && sparse_patch_valid) {
		std::vector<float>().swap(staged_local.gpu_local_records);
	} else {
		build_gpu_local_records(staged_local);
	}
	staged_local_patches.clear();
	if (staged_local.dirty_trunk_count > 0 && sparse_patch_valid) {
		staged_local_patches.reserve(MIN(grid.count, staged_local.dirty_trunk_count * PROBES_PER_LOCAL_TRUNK));
		for (size_t dirty_index = 0; dirty_index < staged_local.dirty_probes.size(); dirty_index++) {
			const int probe = staged_local.dirty_probes[dirty_index];
			LocalPatchData patch;
			patch.header[0] = uint32_t(probe);
			memcpy(patch.visibility, staged_local.local_visibility[size_t(probe)].data(), sizeof(patch.visibility));
			memcpy(patch.material, staged_local.material.data() + size_t(probe) * 4, sizeof(patch.material));
			patch.record_slot = staged_local.local_record_indices[size_t(probe)];
			memcpy(patch.records, &staged_local.get_local_record(size_t(probe)), sizeof(patch.records));
			staged_local_patches.push_back(patch);
		}
	}
	if (!p_analytic && !active_cache_on_grid) {
		// Build the inactive CPU bank on the bake worker. The first dynamic edit can then
		// update only changed Trunks instead of cloning the entire published field.
		seed_recycled_local_field(staged_local, staged_cache, recycled_local, recycled_cache);
	}
	const uint64_t after_local_pack = OS::get_singleton()->get_ticks_usec();
	staged_primitives = std::move(bake_primitives);
	const uint64_t after_display = OS::get_singleton()->get_ticks_usec();
	has_staged = true;
	input_bytes = _input_bytes();
	active_cpu_bytes = _active_cpu_bytes();
	staged_cpu_bytes = _staged_cpu_bytes();
	cpu_peak_bytes = active_cpu_bytes + MAX(staged_cpu_bytes, sdf_scratch_peak_bytes);

	result.ok = true;
	result.solid = staged_local.solid_count;
	result.surface = staged_local.surface_count;
	result.trunks = staged_local.trunk_count;
	result.dirty_trunks = staged_local.dirty_trunk_count;
	result.mismatches = staged_local.classification_mismatches;
	result.mesh_volumes = _mesh_instance_count();
	result.assets_loaded = assets_loaded;
	result.assets_baked = assets_baked;
	result.assets_memory = assets_memory;
	result.assets_requested = assets_requested;
	result.assets_prepared = assets_prepared;
	result.closed_mesh_assets = closed_mesh_assets;
	result.open_mesh_assets = open_mesh_assets;
	result.surface_voxels = surface_voxels;
	result.preparation_error = preparation_error;
	result.sdf_specs = sdf_specs;
	result.sdf_instance_references = sdf_instance_references;
	result.sdf_bytes = sdf_bytes;
	result.instance_field_bytes = instance_field_bytes;
	result.input_bytes = input_bytes;
	result.active_cpu_bytes = active_cpu_bytes;
	result.staged_cpu_bytes = staged_cpu_bytes;
	result.cpu_peak_bytes = cpu_peak_bytes;
	result.sdf_scratch_peak_bytes = sdf_scratch_peak_bytes;
	result.signature_ms = signature_ms;
	result.topology_ms = topology_ms;
	result.cache_read_ms = cache_read_ms;
	result.voxelize_ms = voxelize_ms;
	result.flood_fill_ms = flood_fill_ms;
	result.distance_ms = distance_ms;
	result.cache_write_ms = cache_write_ms;
	result.instance_field_ms = instance_field_ms;
	result.sdf_samples = sdf_samples;
	result.largest_sdf_samples = largest_sdf_samples;
	result.largest_sdf_triangles = largest_sdf_triangles;
	result.longest_asset_bake_ms = longest_asset_bake_ms;
	result.sdf_resolutions = sdf_resolutions;
	result.assets_ms = double(after_assets - start) / 1000.0;
	result.local_ms = double(after_local - after_assets) / 1000.0;
	result.sdf_setup_ms = staged_local.sdf_setup_ms;
	result.sdf_sample_ms = staged_local.sdf_sample_ms;
	result.sdf_transfer_ms = staged_local.sdf_transfer_ms;
	result.sdf_merge_ms = staged_local.sdf_merge_ms;
	result.local_pack_ms = double(after_local_pack - after_local) / 1000.0;
	result.display_ms = double(after_display - after_local_pack) / 1000.0;
	result.build_ms = double(after_display - start) / 1000.0;
	preparation_phase.store(5);
	return result;
}

PackedStringArray LRTVolume::store_prepared_dependencies(bool p_staged) const {
	PackedStringArray paths;
	std::set<uint64_t> geometry_keys;
	std::set<uint64_t> instance_keys;
	std::vector<lrt::GeometryInstance> snapshot;
	if (p_staged) {
		snapshot = staged_primitives;
	} else {
		MutexLock lock(debug_geometry_mutex);
		snapshot = primitives;
	}
	for (const lrt::GeometryInstance &primitive : snapshot) {
		if (primitive.precomputed) {
			continue; // The export plugin validates and packs the authored transfer manifest.
		}
		if (geometry_keys.insert(primitive.asset_signature).second) {
			if (!lrt::store_asset_field(primitive.asset_signature, *primitive.geometry)) {
				return PackedStringArray();
			}
			paths.push_back(lrt::asset_cache_path(primitive.asset_signature));
		}
		if (instance_keys.insert(primitive.instance_cache_signature).second) {
			if (!lrt::store_instance_field(primitive.instance_cache_signature, *primitive.instance)) {
				return PackedStringArray();
			}
			paths.push_back(lrt::instance_cache_path(primitive.instance_cache_signature));
		}
	}
	for (const MeshInstance &mesh : mesh_instances) {
		if (mesh.precomputed) {
			const uint64_t visibility = mesh.transfer_signatures.geometry;
			const String dependencies[] = {
				lrt::transfer_cache_path(visibility, true),
				lrt::transfer_cache_path(mesh.transfer_material_signature, false),
				lrt::transfer_coverage_path(visibility),
			};
			for (const String &dependency : dependencies) {
				if (!FileAccess::exists(dependency)) {
					return PackedStringArray();
				}
				if (!paths.has(dependency)) {
					paths.push_back(dependency);
				}
			}
			continue;
		}
		if (!mesh.raster_capture || mesh.local_capture_key == 0) {
			continue;
		}
		const String path = lrt::static_voxel_capture_path(mesh.local_capture_key);
		if (paths.has(path)) {
			continue;
		}
		if (!FileAccess::exists(path)) {
			Dictionary images;
			if (!lrt::load_static_voxel_capture(mesh.local_capture_key, images) ||
					!lrt::store_static_voxel_capture(mesh.local_capture_key, images)) {
				return PackedStringArray();
			}
		}
		paths.push_back(path);
	}
	return paths;
}

bool LRTVolume::store_local_field_cache(uint64_t p_fingerprint, bool p_staged, const PackedStringArray &p_extra_dependencies) const {
	if (!(p_staged ? has_staged : has_local) || p_fingerprint == 0) {
		return false;
	}
	const lrt::LocalField &field = p_staged ? staged_local : local;
	const lrt::LocalCache &cache = p_staged ? staged_cache : local_cache;
	if (local_backend == "sdf" && (cache.grid_key != lrt::grid_signature(grid) ||
			cache.trunk_signatures.size() != size_t(field.trunk_count) ||
			cache.input_signatures.size() != size_t(field.trunk_count) ||
			cache.samples.size() != size_t(grid.count) || cache.sampled.size() != size_t(grid.count))) {
		return false;
	}
	const String path = local_cache_path(p_fingerprint);
	PackedStringArray dependencies = store_prepared_dependencies(p_staged);
	if (!(p_staged ? staged_primitives : primitives).empty() && dependencies.is_empty()) {
		return false;
	}
	for (const String &dependency : p_extra_dependencies) {
		if (!dependencies.has(dependency)) {
			dependencies.push_back(dependency);
		}
	}
	const String temporary = path + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_buffer(reinterpret_cast<const uint8_t *>(LOCAL_CACHE_MAGIC), 8);
	file->store_32(LOCAL_CACHE_FORMAT_VERSION);
	file->store_64(p_fingerprint);
	for (int axis = 0; axis < 3; axis++) {
		file->store_double(grid.min[axis]);
		file->store_32(uint32_t(grid.size[axis]));
	}
	file->store_double(grid.spacing);
	file->store_32(uint32_t(grid.width));
	file->store_32(uint32_t(grid.height));
	file->store_32(uint32_t(grid.count));
	file->store_pascal_string(local_backend);
	file->store_32(uint32_t(field.solid_count));
	file->store_32(uint32_t(field.surface_count));
	file->store_32(uint32_t(field.classification_mismatches));
	file->store_32(uint32_t(field.trunk_count));
	file->store_32(uint32_t(field.dirty_trunk_count));
	store_local_cache_values(file, field.material);
	store_local_cache_values(file, field.local_records);
	store_local_cache_values(file, field.local_record_indices);
	store_local_cache_values(file, field.local_record_owners);
	store_local_cache_values(file, field.free_local_records);
	store_local_cache_values(file, field.local_visibility);
	store_local_cache_values(file, field.dirty_mask);
	// Persist original samples for incremental probe updates.
	file->store_64(cache.grid_key);
	store_local_cache_values(file, cache.trunk_signatures);
	store_local_cache_values(file, cache.input_signatures);
	std::vector<double> sample_values(cache.samples.size() * 13);
	std::vector<uint32_t> sample_layers(cache.samples.size());
	std::vector<uint8_t> sample_flags(cache.samples.size());
	for (size_t i = 0; i < cache.samples.size(); i++) {
		const lrt::SurfaceSample &sample = cache.samples[i];
		double *values = sample_values.data() + i * 13;
		values[0] = sample.fraction;
		for (int axis = 0; axis < 3; axis++) {
			values[1 + axis] = sample.position[axis];
			values[4 + axis] = sample.normal[axis];
			values[7 + axis] = sample.color[axis];
			values[10 + axis] = sample.emission[axis];
		}
		sample_layers[i] = sample.layer_mask;
		sample_flags[i] = cache.sampled[i] | (sample.valid ? 2 : 0);
	}
	store_local_cache_values(file, sample_values);
	store_local_cache_values(file, sample_layers);
	store_local_cache_values(file, sample_flags);
	// The export manifest is readable without loading the field or creating a rendering device.
	const uint64_t manifest_offset = file->get_position();
	file->store_32(dependencies.size());
	for (const String &dependency : dependencies) {
		file->store_pascal_string(dependency.get_file());
	}
	file->store_64(manifest_offset);
	const Error write_error = file->get_error();
	file.unref();
	if (write_error != OK) {
		return false;
	}
	Ref<DirAccess> directory = DirAccess::open(lrt::asset_cache_directory());
	return directory.is_valid() && directory->rename(temporary.get_file(), path.get_file()) == OK;
}

Dictionary LRTVolume::get_cached_export_dependencies(uint64_t p_fingerprint) {
	Dictionary report;
	report["ok"] = false;
	const String packed = vformat("res://.godot/lrt_export/volume_%016x.lrt", p_fingerprint);
	const String path = FileAccess::exists(packed) ? packed : local_cache_path(p_fingerprint);
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (p_fingerprint == 0 || file.is_null() || file->get_length() < 28) {
		report["error"] = "LRT build data is missing. Rebuild LRT and save the scene before exporting.";
		return report;
	}
	char magic[8] = {};
	file->get_buffer(reinterpret_cast<uint8_t *>(magic), 8);
	if (memcmp(magic, LOCAL_CACHE_MAGIC, 8) != 0 || file->get_32() != LOCAL_CACHE_FORMAT_VERSION || file->get_64() != p_fingerprint) {
		report["error"] = "LRT build data is outdated. Rebuild LRT and save the scene before exporting.";
		return report;
	}
	file->seek_end(-8);
	const uint64_t manifest_offset = file->get_64();
	if (manifest_offset < 20 || manifest_offset > file->get_length() - 12) {
		report["error"] = "Invalid LRT export manifest: " + path;
		return report;
	}
	file->seek(manifest_offset);
	const uint32_t count = file->get_32();
	if (count > (file->get_length() - manifest_offset - 12) / 4) {
		report["error"] = "Invalid LRT export dependency count: " + path;
		return report;
	}
	PackedStringArray dependencies;
	dependencies.push_back(path);
	for (uint32_t index = 0; index < count; index++) {
		const String name = file->get_pascal_string();
		const String dependency = path.get_base_dir().path_join(name);
		if (name.is_empty() || name != name.get_file() || !FileAccess::exists(dependency)) {
			report["error"] = "Missing or invalid LRT export dependency: " + dependency;
			return report;
		}
		dependencies.push_back(dependency);
	}
	if (file->get_error() != OK || file->get_position() != file->get_length() - 8) {
		report["error"] = "Invalid LRT export manifest layout: " + path;
		return report;
	}
	report["ok"] = true;
	report["assets"] = dependencies;
	return report;
}

bool LRTVolume::load_local_field_cache(uint64_t p_fingerprint, LocalBakeResult &r_result) {
	if (p_fingerprint == 0 || !bool(get_cached_export_dependencies(p_fingerprint).get("ok", false))) {
		return false;
	}
	const String packed = vformat("res://.godot/lrt_export/volume_%016x.lrt", p_fingerprint);
	Ref<FileAccess> file = FileAccess::open(FileAccess::exists(packed) ? packed : local_cache_path(p_fingerprint), FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	char magic[8] = {};
	file->get_buffer(reinterpret_cast<uint8_t *>(magic), 8);
	if (memcmp(magic, LOCAL_CACHE_MAGIC, 8) != 0 || file->get_32() != LOCAL_CACHE_FORMAT_VERSION || file->get_64() != p_fingerprint) {
		return false;
	}
	lrt::Grid cached_grid;
	for (int axis = 0; axis < 3; axis++) {
		cached_grid.min[axis] = file->get_double();
		cached_grid.size[axis] = int(file->get_32());
	}
	cached_grid.spacing = file->get_double();
	cached_grid.width = int(file->get_32());
	cached_grid.height = int(file->get_32());
	cached_grid.count = int(file->get_32());
	if (cached_grid.count <= 0 || cached_grid.width <= 0 || cached_grid.height <= 0 ||
			cached_grid.size[0] <= 0 || cached_grid.size[1] <= 0 || cached_grid.size[2] <= 0) {
		return false;
	}
	const String cached_backend = file->get_pascal_string();
	lrt::LocalField cached;
	cached.solid_count = int(file->get_32());
	cached.surface_count = int(file->get_32());
	cached.classification_mismatches = int(file->get_32());
	cached.trunk_count = int(file->get_32());
	cached.dirty_trunk_count = int(file->get_32());
	const uint64_t count = uint64_t(cached_grid.count);
	if (!read_local_cache_values(file, cached.material, count * 4) || cached.material.size() != count * 4 ||
			!read_local_cache_values(file, cached.local_records, count) ||
			!read_local_cache_values(file, cached.local_record_indices, count) || cached.local_record_indices.size() != count ||
			!read_local_cache_values(file, cached.local_record_owners, count) ||
			!read_local_cache_values(file, cached.free_local_records, count) || !cached.validate_local_records() ||
			!read_local_cache_values(file, cached.local_visibility, count) || cached.local_visibility.size() != count ||
			!read_local_cache_values(file, cached.dirty_mask, count) || cached.dirty_mask.size() != count || file->get_error() != OK) {
		return false;
	}
	for (const auto &visibility : cached.local_visibility) {
		for (float coefficient : visibility) {
			if (!std::isfinite(coefficient)) {
				return false;
			}
		}
	}
	lrt::LocalCache cached_cpu;
	cached_cpu.grid_key = file->get_64();
	std::vector<double> sample_values;
	std::vector<uint32_t> sample_layers;
	std::vector<uint8_t> sample_flags;
	const uint64_t trunk_count = uint64_t((cached_grid.size[0] + lrt::TRUNK - 1) / lrt::TRUNK) *
			uint64_t((cached_grid.size[1] + lrt::TRUNK - 1) / lrt::TRUNK) *
			uint64_t((cached_grid.size[2] + lrt::TRUNK - 1) / lrt::TRUNK);
	if (!read_local_cache_values(file, cached_cpu.trunk_signatures, trunk_count) ||
			!read_local_cache_values(file, cached_cpu.input_signatures, trunk_count) ||
			!read_local_cache_values(file, sample_values, count * 13) ||
			!read_local_cache_values(file, sample_layers, count) ||
			!read_local_cache_values(file, sample_flags, count) || file->get_error() != OK) {
		return false;
	}
	if (cached_backend == "sdf") {
		if (cached_cpu.grid_key != lrt::grid_signature(cached_grid) ||
				cached_cpu.trunk_signatures.size() != trunk_count || cached_cpu.input_signatures.size() != trunk_count || sample_values.size() != count * 13 ||
				sample_layers.size() != count || sample_flags.size() != count) {
			return false;
		}
		cached_cpu.samples.resize(size_t(count));
		cached_cpu.sampled.resize(size_t(count));
		for (size_t i = 0; i < size_t(count); i++) {
			if (sample_flags[i] > 3) {
				return false;
			}
			lrt::SurfaceSample &sample = cached_cpu.samples[i];
			const double *values = sample_values.data() + i * 13;
			sample.fraction = values[0];
			for (int axis = 0; axis < 3; axis++) {
				sample.position[axis] = values[1 + axis];
				sample.normal[axis] = values[4 + axis];
				sample.color[axis] = values[7 + axis];
				sample.emission[axis] = values[10 + axis];
			}
			sample.layer_mask = sample_layers[i];
			sample.valid = (sample_flags[i] & 2) != 0;
			cached_cpu.sampled[i] = sample_flags[i] & 1;
		}
	}
	cached.changed_occupancy.clear();
	cached.changed_occupancy_valid = false;
	build_gpu_local_records(cached);
	grid = cached_grid;
	configured = true;
	local_backend = cached_backend;
	staged_local = std::move(cached);
	staged_cache = std::move(cached_cpu);
	if (cached_backend == "sdf") {
		seed_recycled_local_field(staged_local, staged_cache, recycled_local, recycled_cache);
	}
	staged_primitives.clear();
	staged_local_patches.clear();
	has_staged = true;
	r_result = LocalBakeResult();
	r_result.ok = true;
	r_result.cache_loaded = true;
	r_result.solid = staged_local.solid_count;
	r_result.surface = staged_local.surface_count;
	r_result.trunks = staged_local.trunk_count;
	r_result.dirty_trunks = staged_local.dirty_trunk_count;
	r_result.mismatches = staged_local.classification_mismatches;
	r_result.active_cpu_bytes = _active_cpu_bytes();
	r_result.staged_cpu_bytes = _staged_cpu_bytes();
	r_result.cpu_peak_bytes = r_result.active_cpu_bytes + r_result.staged_cpu_bytes;
	return true;
}

Dictionary LRTVolume::bake_local_field(const String &p_backend) {
	Dictionary result;
	ERR_FAIL_COND_V_MSG(!configured, result, "configure() the LRT volume before building its local field.");
	const LocalBakeResult baked = bake_local_field_data(p_backend == "analytic");
	if (!baked.ok) {
		if (baked.needs_axis_aligned) {
			result["error"] = "analytic backend requires axis-aligned boxes";
		}
		if (baked.cancelled) {
			result["cancelled"] = true;
		}
		result["error"] = baked.error;
		result["preparation_error"] = baked.preparation_error;
		result["assets_requested"] = baked.assets_requested;
		result["assets_prepared"] = baked.assets_prepared;
		return result;
	}
	result["ok"] = true;
	result["backend"] = local_backend;
	result["solid"] = baked.solid;
	result["surface"] = baked.surface;
	result["trunks"] = baked.trunks;
	result["dirty_trunks"] = baked.dirty_trunks;
	result["classification_mismatches"] = baked.mismatches;
	result["mesh_volumes"] = baked.mesh_volumes;
	result["assets_loaded"] = baked.assets_loaded;
	result["assets_baked"] = baked.assets_baked;
	result["assets_memory"] = baked.assets_memory;
	result["assets_requested"] = baked.assets_requested;
	result["assets_prepared"] = baked.assets_prepared;
	result["closed_mesh_assets"] = baked.closed_mesh_assets;
	result["open_mesh_assets"] = baked.open_mesh_assets;
	result["surface_voxels"] = baked.surface_voxels;
	result["preparation_error"] = baked.preparation_error;
	result["sdf_specs"] = baked.sdf_specs;
	result["sdf_instance_references"] = baked.sdf_instance_references;
	result["sdf_bytes"] = int64_t(baked.sdf_bytes);
	result["instance_field_bytes"] = int64_t(baked.instance_field_bytes);
	result["input_bytes"] = int64_t(baked.input_bytes);
	result["active_cpu_bytes"] = int64_t(baked.active_cpu_bytes);
	result["staged_cpu_bytes"] = int64_t(baked.staged_cpu_bytes);
	result["cpu_peak_bytes"] = int64_t(baked.cpu_peak_bytes);
	result["sdf_scratch_peak_bytes"] = int64_t(baked.sdf_scratch_peak_bytes);
	result["signature_ms"] = baked.signature_ms;
	result["topology_ms"] = baked.topology_ms;
	result["cache_read_ms"] = baked.cache_read_ms;
	result["voxelize_ms"] = baked.voxelize_ms;
	result["flood_fill_ms"] = baked.flood_fill_ms;
	result["distance_ms"] = baked.distance_ms;
	result["cache_write_ms"] = baked.cache_write_ms;
	result["instance_field_ms"] = baked.instance_field_ms;
	result["sdf_setup_ms"] = baked.sdf_setup_ms;
	result["sdf_sample_ms"] = baked.sdf_sample_ms;
	result["sdf_transfer_ms"] = baked.sdf_transfer_ms;
	result["sdf_merge_ms"] = baked.sdf_merge_ms;
	result["sdf_samples"] = int64_t(baked.sdf_samples);
	result["largest_sdf_samples"] = int64_t(baked.largest_sdf_samples);
	result["largest_sdf_triangles"] = baked.largest_sdf_triangles;
	result["longest_asset_bake_ms"] = baked.longest_asset_bake_ms;
	PackedInt32Array resolutions;
	resolutions.resize(int64_t(baked.sdf_resolutions.size()));
	for (size_t i = 0; i < baked.sdf_resolutions.size(); i++) {
		resolutions.set(int64_t(i), baked.sdf_resolutions[i]);
	}
	result["sdf_resolutions"] = resolutions;
	result["build_ms"] = baked.build_ms;
	return result;
}

// The GPU half: recreate the field buffers for the staged local field and reset the state.
// Prototype src/lab.js clearChangedOccupancy: the probes that switched between solid and air
// lose their radiance and visibility, every other probe keeps the propagated field. Consecutive
// probes are cleared in one buffer update, which is the same set of pixels the prototype
// scissors one by one.
void LRTVolume::_clear_changed_occupancy(const std::vector<int> &p_probes) {
	if (!device || p_probes.empty() || !has_local) {
		return;
	}
	size_t index = 0;
	while (index < p_probes.size()) {
		size_t run_end = index + 1;
		while (run_end < p_probes.size() && p_probes[run_end] == p_probes[run_end - 1] + 1) {
			run_end++;
		}
		const size_t run_count = run_end - index;
		const uint32_t offset = uint32_t(p_probes[index]) * 4 * sizeof(float);
		const uint32_t bytes = uint32_t(run_count * 4 * sizeof(float));
		for (int buffer = 0; buffer < 2; buffer++) {
			for (int channel = 0; channel < 3; channel++) {
				device->buffer_clear(radiance_buffers[buffer][channel], offset, bytes);
			}
		}
		index = run_end;
	}
}

void LRTVolume::_free_render_thread() {
	_free_gpu_resources();
	device = nullptr;
}

void LRTVolume::_prepare_shared_gpu_resources_render_thread() {
	_create_shaders();
}

void LRTVolume::prepare_shared_gpu_resources() {
	if (_ensure_device() != OK) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_prepare_shared_gpu_resources_render_thread));
}

Dictionary LRTVolume::apply_local_field(bool p_preserve_history) {
	Dictionary result;
	ERR_FAIL_COND_V(!begin_apply_local_field(p_preserve_history), result);
	return finish_apply_local_field(true);
}

bool LRTVolume::begin_apply_local_field(bool p_preserve_history) {
	ERR_FAIL_COND_V_MSG(apply_pending, false, "An LRT local field apply is already pending.");
	ERR_FAIL_COND_V_MSG(!has_staged, false, "bake_local_field() must run before apply_local_field().");

	apply_started_usec = OS::get_singleton()->get_ticks_usec();
	// The prototype's temporal policy: a change on the same grid with the same backend keeps the
	// propagated field and only clears the probes whose solid/air occupancy changed.
	// `has_local` is not part of the test: the input setters clear it while a bake is pending,
	// which says nothing about the field still living on the GPU.
	const bool same_grid = has_applied_grid &&
			applied_grid.count == grid.count && applied_grid.spacing == grid.spacing &&
			applied_grid.min.x == grid.min.x && applied_grid.min.y == grid.min.y && applied_grid.min.z == grid.min.z &&
			applied_grid.size[0] == grid.size[0] && applied_grid.size[1] == grid.size[1] && applied_grid.size[2] == grid.size[2];
	const bool preserve = p_preserve_history && same_grid && applied_backend == local_backend;
	ERR_FAIL_COND_V(_ensure_device() != OK, false);
	std::vector<int> changed;
	if (preserve) {
		if (staged_local.changed_occupancy_valid) {
			changed = std::move(staged_local.changed_occupancy);
		} else {
			for (int i = 0; i < grid.count; i++) {
				if (local.material[size_t(i) * 4 + 3] != staged_local.material[size_t(i) * 4 + 3]) {
					changed.push_back(i);
				}
			}
		}
	}
	const bool seeded_recycled_bank = recycled_cache.local == &recycled_local &&
			recycled_cache.grid_key == staged_cache.grid_key &&
			recycled_cache.trunk_signatures == staged_cache.trunk_signatures &&
			recycled_local.material.size() == staged_local.material.size() &&
			recycled_local.local_record_indices.size() == staged_local.local_record_indices.size();
	if (!seeded_recycled_bank) {
		recycled_local = std::move(local);
	}
	local = std::move(staged_local);
	_publish_primitives();
	// The incremental cache and the field it describes must always switch together.
	if (!seeded_recycled_bank) {
		recycled_cache = std::move(local_cache);
		recycled_cache.local = &recycled_local;
	}
	local_cache = std::move(staged_cache);
	local_cache.local = &local;
	has_staged = false;
	pending_changed_probes = std::move(changed);
	// The bake worker already measured active + staged allocations before this move-only handoff;
	// recomputing the same peak here would rescan all mesh inputs on the main thread.
	apply_error = OK;
	apply_done.store(false);
	apply_propagation_safe.store(false);
	apply_pending = true;
	apply_preserve_history = preserve;
	apply_sparse_patch = preserve && local.changed_occupancy_valid &&
			local.dirty_trunk_count <= MAX_LOCAL_PATCH_PROBES / PROBES_PER_LOCAL_TRUNK &&
			staged_local_patches.size() <= MAX_LOCAL_PATCH_PROBES;
	if (local.gpu_local_records.empty() && !apply_sparse_patch) {
		build_gpu_local_records(local);
	}
	apply_grid_bank_switch = preserve && (!apply_sparse_patch || local.dirty_trunk_count > 0);
	apply_visibility_changed = !preserve || local.visibility_changed;
	apply_submit_ms = 0.0;
	apply_local_patch_upload_bytes = 0;
	apply_full_upload_bytes = 0;
	_submit_apply();
	return true;
}

bool LRTVolume::is_apply_pending() const {
	return apply_pending;
}

bool LRTVolume::can_step_while_applying() const {
	return apply_pending && apply_preserve_history && apply_propagation_safe.load();
}

Dictionary LRTVolume::finish_apply_local_field(bool p_wait) {
	Dictionary result;
	if (p_wait && apply_pending && !apply_done.load()) {
		RenderingServer::get_singleton()->sync();
	}

	if (!apply_pending || !apply_done.load()) {
		return result;
	}
	apply_pending = false;
	staged_local_patches.clear();
	ERR_FAIL_COND_V(apply_error != OK, result);
	has_local = true;
	applied_grid = grid;
	applied_backend = local_backend;
	has_applied_grid = true;
	if (!apply_preserve_history) {
		iteration = 0;
	}
	if (allocated_debug_fields.load() != requested_debug_fields.load()) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_set_debug_fields_render_thread).bind(requested_debug_fields.load()));
		rendering_server->sync();
	}
	if (external_gi_buffers_full_size.load() != external_gi_enabled.load()) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_set_external_gi_enabled_render_thread).bind(external_gi_enabled.load()));
		rendering_server->sync();
	}

	const uint64_t upload_bytes = vector_bytes(local.gpu_local_records);
	std::vector<float>().swap(local.gpu_local_records);
	active_cpu_bytes -= MIN(active_cpu_bytes, upload_bytes);
	result = _local_field_report();
	result["preserved_history"] = apply_preserve_history;
	result["cleared_probes"] = int(pending_changed_probes.size());
	result["upload_submit_ms"] = apply_submit_ms;
	result["upload_ms"] = double(OS::get_singleton()->get_ticks_usec() - apply_started_usec) / 1000.0;
	result["upload_resources_ms"] = apply_resources_ms;
	result["upload_buffers_ms"] = apply_buffer_upload_ms;
	result["upload_textures_ms"] = apply_texture_upload_ms;
	result["upload_finalize_ms"] = apply_finalize_ms;
	result["local_patch_upload_bytes"] = int64_t(apply_local_patch_upload_bytes);
	result["full_upload_bytes"] = int64_t(apply_full_upload_bytes);
	return result;
}

// Field-level half of the apply report: what the local field currently is, independent of whether
// this frame uploaded it. A build that found nothing to solve reports through the same keys.
Dictionary LRTVolume::_local_field_report() const {
	Dictionary result;
	result["backend"] = local_backend;
	result["solid"] = local.solid_count;
	result["surface"] = local.surface_count;
	result["trunks"] = local.trunk_count;
	result["snapshot_reused"] = local.snapshot_reused;
	result["snapshot_sync_probes"] = local.snapshot_sync_probes;
	result["geometry_dirty_trunks"] = local.geometry_dirty_trunks;
	result["reflectance_dirty_trunks"] = local.reflectance_dirty_trunks;
	result["emission_dirty_trunks"] = local.emission_dirty_trunks;
	result["occupancy_probes_updated"] = local.occupancy_probes_updated;
	result["geometry_records_built"] = local.geometry_records_built;
	result["reflectance_records_updated"] = local.reflectance_records_updated;
	result["emission_records_updated"] = local.emission_records_updated;
	result["surface_workspaces_built"] = local.surface_workspaces_built;
	result["surface_workspace_instances"] = local.surface_workspace_instances;
	result["surface_workspace_payload_bytes"] = local.surface_workspace_payload_bytes;
	result["classification_mismatches"] = local.classification_mismatches;
	result["mesh_volumes"] = _mesh_instance_count();
	result["assets_loaded"] = assets_loaded;
	result["assets_baked"] = assets_baked;
	result["assets_memory"] = assets_memory;
	result["assets_requested"] = assets_requested;
	result["assets_prepared"] = assets_prepared;
	result["closed_mesh_assets"] = closed_mesh_assets;
	result["open_mesh_assets"] = open_mesh_assets;
	result["surface_voxels"] = surface_voxels;
	result["preparation_error"] = preparation_error;
	result["sdf_specs"] = sdf_specs;
	result["sdf_instance_references"] = sdf_instance_references;
	result["sdf_bytes"] = int64_t(sdf_bytes);
	result["instance_field_bytes"] = int64_t(instance_field_bytes);
	result["input_bytes"] = int64_t(input_bytes);
	result["active_cpu_bytes"] = int64_t(_active_cpu_bytes());
	result["staged_cpu_bytes"] = int64_t(_staged_cpu_bytes());
	result["cpu_peak_bytes"] = int64_t(cpu_peak_bytes);
	result["sdf_scratch_peak_bytes"] = int64_t(sdf_scratch_peak_bytes);
	result["gpu_bytes"] = int64_t(_gpu_bytes());
	result["signature_ms"] = signature_ms;
	result["topology_ms"] = topology_ms;
	result["cache_read_ms"] = cache_read_ms;
	result["voxelize_ms"] = voxelize_ms;
	result["flood_fill_ms"] = flood_fill_ms;
	result["distance_ms"] = distance_ms;
	result["cache_write_ms"] = cache_write_ms;
	result["instance_field_ms"] = instance_field_ms;
	result["sdf_samples"] = int64_t(sdf_samples);
	result["largest_sdf_samples"] = int64_t(largest_sdf_samples);
	result["largest_sdf_triangles"] = largest_sdf_triangles;
	result["longest_asset_bake_ms"] = longest_asset_bake_ms;
	PackedInt32Array resolutions;
	resolutions.resize(int64_t(sdf_resolutions.size()));
	for (size_t i = 0; i < sdf_resolutions.size(); i++) {
		resolutions.set(int64_t(i), sdf_resolutions[i]);
	}
	result["sdf_resolutions"] = resolutions;
	return result;
}

void LRTVolume::finish_cached_asset_preparation() {
	_publish_primitives();
}

Dictionary LRTVolume::describe_unchanged_local_field() {
	if (device && (allocated_debug_fields.load() & DEBUG_DIRTY)) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTVolume::_refresh_debug_textures));
	}
	return _local_field_report();
}

void LRTVolume::_apply_render_thread(bool p_preserve_history) {
	const uint64_t resources_started_usec = OS::get_singleton()->get_ticks_usec();
	apply_buffer_upload_ms = 0.0;
	apply_texture_upload_ms = 0.0;
	apply_finalize_ms = 0.0;
	bool recreate_uniform_sets = !p_preserve_history;
	if (p_preserve_history) {
		if (local.local_records.size() > local_record_capacity.load()) {
			_free_uniform_sets();
			recreate_uniform_sets = true;
			apply_error = _grow_local_record_buffers();
		}
	} else {
		_free_gpu_resources();
		apply_error = _create_buffers();
	}
	if (apply_error == OK) {
		apply_error = _create_shaders();
	}
	if (apply_error == OK && recreate_uniform_sets) {
		apply_error = _create_uniform_sets();
	}
	if (apply_error != OK) {
		ERR_PRINT(vformat("LRT local field GPU resource preparation failed (error %d, estimated allocation %.1f MiB). The Volume is excluded from rendering.",
				apply_error, double(_estimate_field_gpu_bytes(local, p_preserve_history)) / (1024.0 * 1024.0)));
		_free_gpu_resources();
		apply_done.store(true);
		return;
	}
	apply_propagation_safe.store(p_preserve_history && !recreate_uniform_sets);
	apply_resources_ms = double(OS::get_singleton()->get_ticks_usec() - resources_started_usec) / 1000.0;
	const uint64_t buffers_started_usec = OS::get_singleton()->get_ticks_usec();
	if (p_preserve_history) {
		_upload_staged_local_buffers();
		apply_buffer_upload_ms += double(OS::get_singleton()->get_ticks_usec() - buffers_started_usec) / 1000.0;
		const uint64_t textures_started_usec = OS::get_singleton()->get_ticks_usec();
		if (requested_debug_fields.load() != 0) {
			_upload_local_textures();
		} else {
			local_debug_textures_dirty = true;
		}
		apply_texture_upload_ms = double(OS::get_singleton()->get_ticks_usec() - textures_started_usec) / 1000.0;
	} else {
		_upload_local_buffers();
	}
	has_local = true;
	const uint64_t finalize_started_usec = OS::get_singleton()->get_ticks_usec();
	if (p_preserve_history) {
		apply_propagation_safe.store(false);
	}
	if (p_preserve_history && apply_grid_bank_switch) {
		if (apply_sparse_patch) {
			struct PatchPushConstant {
				int32_t patch_count;
				int32_t write_source;
				int32_t pad;
				int32_t pad2;
			} push_constant = { int32_t(staged_local_patches.size()), 1, 0, 0 };
			RD::ComputeListID patch_list = device->compute_list_begin();
			device->compute_list_bind_compute_pipeline(patch_list, pipeline_local_patch);
			device->compute_list_bind_uniform_set(patch_list, staged_uniform_set_local_patch, 0);
			device->compute_list_set_push_constant(patch_list, &push_constant, sizeof(push_constant));
			device->compute_list_dispatch(patch_list,
					Math::division_round_up(uint32_t(staged_local_patches.size()), uint32_t(WORKGROUP_SIZE)), 1, 1);
			// Both bank dispatches update the shared material atlas; order their writes.
			device->compute_list_add_barrier(patch_list);
			push_constant.write_source = 0;
			device->compute_list_bind_uniform_set(patch_list, uniform_set_local_patch, 0);
			device->compute_list_set_push_constant(patch_list, &push_constant, sizeof(push_constant));
			device->compute_list_dispatch(patch_list,
					Math::division_round_up(uint32_t(staged_local_patches.size()), uint32_t(WORKGROUP_SIZE)), 1, 1);
			device->compute_list_end();
			local_grid_banks_synchronized = true;
		} else {
			local_grid_banks_synchronized = false;
		}
		SWAP(material_buffer, staged_material_buffer);
		SWAP(local_visibility_buffer, staged_local_visibility_buffer);
		SWAP(matrix_buffer, staged_matrix_buffer);
		SWAP(record_index_buffer, staged_record_index_buffer);
		SWAP(uniform_set_inject, staged_uniform_set_inject);
		SWAP(uniform_set_local_patch, staged_uniform_set_local_patch);
		for (int buffer = 0; buffer < 2; buffer++) {
			SWAP(uniform_set_propagate[buffer], staged_uniform_set_propagate[buffer]);
			SWAP(uniform_set_sky_solve[buffer], staged_uniform_set_sky_solve[buffer]);
		}
	}
	if (!p_preserve_history) {
		source_full_dirty = true;
	} else {
		for (int probe : local.dirty_probes) {
			const Vector3i coordinate(probe % grid.size[0], probe / (grid.size[0] * grid.size[2]),
					(probe / grid.size[0]) % grid.size[2]);
			_include_source_probes(coordinate, coordinate + Vector3i(1, 1, 1));
		}
	}
	_upload_params(nullptr, true);
	local_geometry_input_usec.store(staged_geometry_input_usec);
	local_field_version.fetch_add(1);
	debug_local_frame = Engine::get_singleton()->get_frames_drawn();
	if (p_preserve_history) {
		_clear_changed_occupancy(pending_changed_probes);
		if (apply_visibility_changed) {
			_restart_sky_visibility();
		}
		// Initialize new display probes while retaining existing radiance history until
		// the next timed presentation tick, including across occupancy changes.
		_refresh_debug_textures();
	} else {
		current = 0;
		_reset_render_thread();
		_restart_sky_visibility();
	}
	apply_finalize_ms = double(OS::get_singleton()->get_ticks_usec() - finalize_started_usec) / 1000.0;
	apply_done.store(true);
}

void LRTVolume::_submit_apply() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		apply_error = ERR_UNAVAILABLE;
		apply_done.store(true);
		return;
	}
	const uint64_t submit_start = OS::get_singleton()->get_ticks_usec();
	rendering_server->call_on_render_thread(
			callable_mp(this, &LRTVolume::_apply_render_thread).bind(apply_preserve_history));
	apply_submit_ms += double(OS::get_singleton()->get_ticks_usec() - submit_start) / 1000.0;
}

Dictionary LRTVolume::build_local_field(const String &p_backend) {
	Dictionary result = bake_local_field(p_backend);
	if (!result.has("ok")) {
		return result;
	}
	Dictionary applied = apply_local_field();
	if (applied.is_empty()) {
		return Dictionary();
	}
	applied["build_ms"] = result["build_ms"];
	applied["mesh_volumes"] = result["mesh_volumes"];
	return applied;
}

void LRTVolume::inject() {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before inject().");
	ERR_FAIL_COND(_ensure_device() != OK);
	injection_dirty.store(true);
	if (injection_pending.exchange(true)) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_inject_render_thread));
}

bool LRTVolume::is_injection_pending() const {
	return injection_pending.load();
}

void LRTVolume::_inject_render_thread() {
	{
		MutexLock lock(params_mutex);
		bool requires_view = false;
		bool inputs_pending = native_resolve_pending.load();
		{
			MutexLock resolve_lock(native_resolve_mutex);
			inputs_pending |= !queued_direct_resolves.empty();
		}
		for (int i = 0; i < native_light_count; i++) {
			const NativeLightState &state = native_light_states[i];
			requires_view |= state.has_input && (state.input.shadow_enabled || state.input.projector_requested || state.input.area);
			inputs_pending |= state.has_input && state.instance_id == 0;
		}
		if ((requires_view && !LRTRenderBridge::is_native_lighting_ready(render_owner)) || inputs_pending) {
			LRTRenderBridge::defer_native_light_resolve(callable_mp(this, &LRTVolume::_inject_render_thread));
			return;
		}
	}
	while (true) {
		_update_gpu_timing();
		const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
		injection_dirty.store(false);
		std::vector<NativeLightInput> light_inputs;
		SourceRegion region;
		if (!_upload_params(&light_inputs, true, &region)) {
			if (native_light_inputs_waiting) {
				LRTRenderBridge::defer_native_light_resolve(callable_mp(this, &LRTVolume::_inject_render_thread));
				return;
			}
			has_local = false;
			injection_pending.store(false);
			return;
		}
		if (region.size[3] == 0) {
			injection_pending.store(false);
			if (!injection_dirty.exchange(false) || injection_pending.exchange(true)) {
				return;
			}
			continue;
		}
		debug_source_region = region;
		debug_source_frame = Engine::get_singleton()->get_frames_drawn();
		source_last_probe_count.store(region.size[3]);
		source_total_probe_count.fetch_add(region.size[3]);
		const uint64_t batch_version = source_version.fetch_add(1) + 1;
		const bool timing_active = _begin_gpu_timestamp(GPU_TIMING_INJECT, 1, batch_version);
		RD::ComputeListID list = device->compute_list_begin();
		device->compute_list_bind_compute_pipeline(list, pipeline_inject);
		device->compute_list_bind_uniform_set(list, uniform_set_inject, 0);
		device->compute_list_set_push_constant(list, &region, sizeof(region));
		device->compute_list_dispatch(list, Math::division_round_up(uint32_t(region.size[3]), uint32_t(WORKGROUP_SIZE)), 1, 1);
		device->compute_list_end();

		{
			MutexLock lock(light_input_mutex);
			injected_light_inputs = std::move(light_inputs);
			injected_light_source_version = batch_version;
			injected_light_submission_frame = Engine::get_singleton()->get_frames_drawn();
		}
		_end_gpu_timestamp(GPU_TIMING_INJECT, timing_active);
		gpu_pass_dispatches[GPU_TIMING_INJECT].fetch_add(1);
		// A propagation batch queued by the same main-thread frame publishes both the new source
		// and its resulting radiance. Updates without a queued propagation batch still
		// receive an immediate diagnostic source snapshot.
		if (pending_step_iterations.load() == 0) {
			_refresh_debug_textures();
		}
		last_render_thread_pass_ms[GPU_TIMING_INJECT].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
		injection_pending.store(false);
		if (!injection_dirty.exchange(false) || injection_pending.exchange(true)) {
			break;
		}
	}
}


void LRTVolume::_resize_native_light_buffers_render_thread(int p_capacity) {
	if (!device || !has_local || p_capacity <= 0) {
		return;
	}
	const RID next = device->storage_buffer_create(sizeof(NativeLightStateData) * p_capacity);
	if (next.is_null()) {
		has_local = false;
		MutexLock lock(native_resolve_mutex);
		native_resolve_error = "LRT native light parameter allocation failed.";
		return;
	}
	_free_uniform_sets();
	device->free_rid(native_light_state_buffer);
	native_light_state_buffer = next;
	allocated_native_light_capacity = p_capacity;
	uploaded_native_light_state_data.clear();
	ERR_FAIL_COND(_create_uniform_sets() != OK);
}


// Bind the current record bank, total Source and native lighting inputs.
Error LRTVolume::_create_inject_uniform_sets() {
	for (int bank = 0; bank < 2; bank++) {
		RID &set = bank == 0 ? uniform_set_inject : staged_uniform_set_inject;
		if (set.is_valid() && device->uniform_set_is_valid(set)) {
			device->free_rid(set);
		}
		set = RID();
		Vector<RD::Uniform> uniforms;
		const auto add = [&uniforms](RD::UniformType p_type, uint32_t p_binding, RID p_id) {
			RD::Uniform uniform;
			uniform.uniform_type = p_type;
			uniform.binding = p_binding;
			uniform.append_id(p_id);
			uniforms.push_back(uniform);
		};
		add(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer);
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, bank == 0 ? material_buffer : staged_material_buffer);
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, bank == 0 ? matrix_buffer : staged_matrix_buffer);
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, bank == 0 ? record_index_buffer : staged_record_index_buffer);
		for (int channel = 0; channel < 3; channel++) {
			add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + channel, source_buffers[channel]);
			add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27 + channel, sky_buffers[channel]);
		}
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, native_light_state_buffer);
		add(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, native_light_trunk_buffer);
		for (int texture = 1; texture < 4; texture++) {
			RD::Uniform uniform;
			uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			uniform.binding = 22 + texture;
			uniform.append_id(texture < 2 ? native_light_sampler : native_light_linear_sampler);
			uniform.append_id(native_light_textures[texture].is_valid() ? native_light_textures[texture] : native_light_dummy_texture);
			uniforms.push_back(uniform);
		}
		RD::Uniform shadow;
		shadow.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		shadow.binding = 30;
		shadow.append_id(native_light_sampler);
		shadow.append_id(directional_shadows.get_texture());
		uniforms.push_back(shadow);
		set = device->uniform_set_create(uniforms, shader_inject, 0);
		ERR_FAIL_COND_V(set.is_null(), ERR_CANT_CREATE);
	}
	bound_directional_shadow = directional_shadows.get_texture();
	return OK;
}

void LRTVolume::refresh_native_source_for_view() {
	if (!has_local || native_light_count == 0 || injection_pending.load()) {
		return;
	}
	bool changed = false;
	{
		MutexLock lock(params_mutex);
		if (!_upload_native_light_state_buffer(&changed)) {
			return;
		}
	}
	if (changed && !injection_pending.exchange(true)) {
		_inject_render_thread();
	}
}

bool LRTVolume::_upload_native_light_state_buffer(bool *r_changed) {
	if (!native_light_state_buffer.is_valid()) {
		return false;
	}
	if (r_changed) {
		*r_changed = false;
	}
	native_light_inputs_waiting = false;
	auto *storage = RendererRD::LightStorage::get_singleton();
	std::vector<NativeLightStateData> data;
	data.resize(size_t(native_light_capacity));
	RID textures[4];
	std::vector<LRTDirectionalShadow::Light> directional_lights;
	Transform3D capture_volume_to_world;
	for (int i = 0; i < native_light_count; i++) {
		const NativeLightState &state = native_light_states[i];
		if (!state.enabled || !state.has_input || state.cull_mask == 0 || !state.input.directional || !state.input.shadow_enabled) {
			continue;
		}
		const RID instance = LRTRenderBridge::get_native_light_instance(state.input.scene_light_instance);
		if (instance.is_null() || !storage->owns_light_instance(instance)) {
			continue;
		}
		capture_volume_to_world = state.input.volume_to_world;
		directional_lights.push_back({ state.input.scene_light_instance, state.input.light_direction,
				storage->light_get_shadow_caster_mask(storage->light_instance_get_base_light(instance)), state.input.source_signature,
				storage->light_get_reverse_cull_face_mode(storage->light_instance_get_base_light(instance)) });
	}
	const uint64_t previous_shadow_revision = directional_shadows.get_revision();
	{
		const AABB bounds(Vector3(grid.min.x, grid.min.y, grid.min.z),
				Vector3(grid.size[0], grid.size[1], grid.size[2]) * grid.spacing);
		const int resolution = int(GLOBAL_GET("rendering/global_illumination/lrt/directional_shadow/resolution"));
		ERR_FAIL_COND_V_MSG(resolution < 64 || resolution > 2048, false, "LRT directional shadow resolution must be between 64 and 2048.");
		const Transform3D volume_to_world = render_owner.is_valid() && !directional_lights.empty() ? LRTRenderBridge::get_volume_to_world(render_owner) : capture_volume_to_world;
		const Error error = directional_shadows.update(device, bounds, directional_lights,
				resolution, grid.spacing, volume_to_world, lrt_gpu_profiling_enabled.load());
		if (error != OK) {
			MutexLock lock(native_resolve_mutex);
			native_resolve_error = "LRT directional depth rendering failed: " + itos(error);
			return false;
		}
	}
	const bool shadows_changed = previous_shadow_revision != directional_shadows.get_revision();
	if (shadows_changed) {
		source_full_dirty = true;
	}
	int directional_layer = 0;
	const auto xyz = [](const Vector3 &p_vector, float *r_data) {
		r_data[0] = p_vector.x;
		r_data[1] = p_vector.y;
		r_data[2] = p_vector.z;
	};
	const auto rect = [](const Rect2 &p_rect, float *r_data) {
		r_data[0] = p_rect.position.x;
		r_data[1] = p_rect.position.y;
		r_data[2] = p_rect.size.x;
		r_data[3] = p_rect.size.y;
	};
	for (int i = 0; i < native_light_count; i++) {
		NativeLightState &state = native_light_states[i];
		if (!state.enabled || !state.has_input || state.cull_mask == 0) {
			continue;
		}
		const NativeLightResolve &input = state.input;
		const RID native_instance = LRTRenderBridge::get_native_light_instance(input.scene_light_instance);
		if (native_instance.is_null() || !storage->owns_light_instance(native_instance)) {
			continue;
		}
		NativeLightStateData &light = data[i];
		store_transform_mat4(input.volume_to_source, light.volume_to_light);
		xyz(state.scale, light.color_kind);
		light.color_kind[3] = float(input.direct_kind);
		xyz(input.light_position, light.position_range);
		light.position_range[3] = float(input.source_range);
		xyz(-input.light_direction, light.direction_attenuation);
		light.direction_attenuation[3] = float(input.attenuation);
		light.cone_area[0] = float(input.spot_cos_angle);
		light.cone_area[1] = float(input.spot_cone_attenuation);
		light.cone_area[2] = input.area_half_size.x;
		light.cone_area[3] = input.area_half_size.y;
		light.atlas_flags[3] = input.area_normalize_energy ? 1.0f : 0.0f;
		const RID base = storage->light_instance_get_base_light(native_instance);
		light.shadow_parameters[2] = storage->light_get_param(base, RSE::LIGHT_PARAM_SHADOW_OPACITY);
		light.shadow_parameters[3] = float(input.source_range + (input.area ? input.area_half_size.length() : 0.0));
		Projection bias;
		bias.set_light_bias();
		Projection correction;
		correction.set_depth_correction(false, true, false);
		if (input.directional && input.shadow_enabled) {
			const LRTDirectionalShadow::View &view = directional_shadows.get_view(directional_layer);
			store_projection_mat4(view.sample, light.shadow_matrix[0]);
			light.shadow_rect[0][0] = float(directional_layer++);
			light.shadow_parameters[0] = 1.0f;
			light.shadow_parameters[1] = MAX(0.0f, float(GLOBAL_GET("rendering/global_illumination/lrt/directional_shadow/bias"))) / view.depth_span;
		} else if (!input.directional) {
			LRTRenderBridge::PositionalShadowSample shadow;
			if (input.shadow_enabled) {
				shadow = LRTRenderBridge::get_positional_shadow_sample(input.scene_light_instance);
			}
			if (shadow.valid) {
				textures[1] = shadow.texture;
				rect(shadow.atlas_rect, light.shadow_rect[0]);
				light.shadow_parameters[0] = 1.0f;
				light.shadow_parameters[1] = shadow.shadow_bias;
				light.atlas_flags[0] = shadow.flip_offset.x;
				light.atlas_flags[1] = shadow.flip_offset.y;
			}
			if (input.direct_kind == 4) {
				Projection camera;
				camera.set_perspective(Math::rad_to_deg(Math::acos(input.spot_cos_angle)) * 2.0, 1.0, 0.01, input.source_range);
				if (shadow.valid) {
					camera = shadow.shadow_camera;
				}
				store_projection_mat4(bias * correction * camera * Projection(input.volume_to_source), light.shadow_matrix[0]);
			}
		}
		if (input.projector_requested) {
			const auto projector = LRTRenderBridge::get_light_projector_sample(input.scene_light_instance);
			if (!projector.valid) {
				if (++state.atlas_wait_frames <= NATIVE_PROJECTOR_WAIT_FRAMES) {
					native_light_inputs_waiting = true;
					return false;
				}
				MutexLock lock(native_resolve_mutex);
				native_resolve_error = "LRT native projector atlas did not publish the requested texture.";
				return false;
			}
			state.atlas_wait_frames = 0;
			textures[3] = projector.texture;
			light.projector[0] = projector.rect.x;
			light.projector[1] = projector.rect.y;
			light.projector[2] = projector.rect.z;
			light.projector[3] = projector.rect.w;
			light.atlas_flags[2] = 1.0f;
		}
		if (input.area) {
			store_transform_mat4(input.volume_to_source.affine_inverse(), light.shadow_matrix[1]);
			const auto area = LRTRenderBridge::get_area_light_atlas_sample(input.scene_light_instance);
			if (input.area_texture_requested && !area.valid) {
				if (++state.atlas_wait_frames <= NATIVE_PROJECTOR_WAIT_FRAMES) {
					native_light_inputs_waiting = true;
					return false;
				}
				MutexLock lock(native_resolve_mutex);
				native_resolve_error = "LRT native area-light atlas did not publish the requested texture.";
				return false;
			}
			if (area.valid) {
				state.atlas_wait_frames = 0;
				textures[2] = area.texture;
				rect(area.projector_rect, light.area_projector);
				light.cascade_splits[0] = area.max_mipmap;
			}
		}
	}
	std::vector<LightTrunkInput> trunk_inputs;
	trunk_inputs.resize(size_t(native_light_count));
	for (int i = 0; i < native_light_count; i++) {
		LightTrunkInput &input = trunk_inputs[size_t(i)];
		input.kind = int(data[i].color_kind[3]);
		if (input.kind >= 3) {
			input.origin = native_light_states[i].influence_origin;
			input.radius = native_light_states[i].influence_radius;
		}
	}
	bool recreate = bound_directional_shadow != directional_shadows.get_texture();
	if (!_upload_light_trunks(std::move(trunk_inputs), recreate)) {
		return false;
	}
	for (int texture = 0; texture < 4; texture++) {
		const RID current_texture = textures[texture].is_valid() ? textures[texture] : native_light_dummy_texture;
		if (current_texture != native_light_textures[texture]) {
			native_light_textures[texture] = current_texture;
			recreate = true;
		}
	}
	const size_t state_bytes = data.size() * sizeof(NativeLightStateData);
	const bool changed = uploaded_native_light_state_data.size() != state_bytes ||
			memcmp(uploaded_native_light_state_data.ptr(), data.data(), state_bytes) != 0;
	if (changed) {
		ERR_FAIL_COND_V(device->buffer_update(native_light_state_buffer, 0, state_bytes, data.data()) != OK, false);
		uploaded_native_light_state_data = bytes_of(data.data(), state_bytes);
	}
	if (recreate) {
		ERR_FAIL_COND_V(_create_inject_uniform_sets() != OK, false);
	}
	if (r_changed) {
		*r_changed = changed || recreate || shadows_changed;
	}
	return true;
}

bool LRTVolume::_upload_light_trunks(std::vector<LightTrunkInput> p_inputs, bool &r_recreate) {
	if (!light_trunks_dirty && p_inputs == uploaded_light_trunk_inputs) {
		return true;
	}
	// Directional IDs are global; local lights are registered conservatively per Trunk.
	const uint32_t tx = (grid.size[0] + 7) / 8;
	const uint32_t ty = (grid.size[1] + 7) / 8;
	const uint32_t tz = (grid.size[2] + 7) / 8;
	std::vector<uint32_t> candidates = { tx, ty, tz, 0 };
	for (int i = 0; i < int(p_inputs.size()); i++) {
		if (p_inputs[i].kind == 2) {
			candidates.push_back(i);
			candidates[3]++;
		}
	}
	const size_t offset_start = candidates.size();
	const uint32_t trunk_count = tx * ty * tz;
	candidates.resize(offset_start + trunk_count + 1);
	for (uint32_t y = 0; y < ty; y++) {
		for (uint32_t z = 0; z < tz; z++) {
			for (uint32_t x = 0; x < tx; x++) {
				candidates[offset_start + x + tx * (z + tz * y)] = uint32_t(candidates.size());
				const Vector3 low(grid.min.x + x * 8 * grid.spacing, grid.min.y + y * 8 * grid.spacing, grid.min.z + z * 8 * grid.spacing);
				const Vector3 high(grid.min.x + MIN((x + 1) * 8, uint32_t(grid.size[0])) * grid.spacing,
						grid.min.y + MIN((y + 1) * 8, uint32_t(grid.size[1])) * grid.spacing,
						grid.min.z + MIN((z + 1) * 8, uint32_t(grid.size[2])) * grid.spacing);
				for (int i = 0; i < int(p_inputs.size()); i++) {
					if (p_inputs[i].kind < 3) {
						continue;
					}
					const LightTrunkInput &input = p_inputs[i];
					const Vector3 nearest = input.origin.clamp(low, high);
					if (nearest.distance_squared_to(input.origin) <= input.radius * input.radius) {
						candidates.push_back(i);
					}
				}
			}
		}
	}
	candidates[offset_start + trunk_count] = uint32_t(candidates.size());
	const uint64_t bytes = candidates.size() * sizeof(uint32_t);
	if (bytes != native_light_trunk_bytes.load()) {
		const RID next = device->storage_buffer_create(uint32_t(bytes));
		ERR_FAIL_COND_V(next.is_null(), false);
		device->free_rid(native_light_trunk_buffer);
		native_light_trunk_buffer = next;
		native_light_trunk_bytes.store(bytes);
		r_recreate = true;
	}
	ERR_FAIL_COND_V(device->buffer_update(native_light_trunk_buffer, 0, bytes, candidates.data()) != OK, false);
	uploaded_light_trunk_inputs = std::move(p_inputs);
	light_trunks_dirty = false;
	return true;
}

void LRTVolume::_resolve_native_lights_render_thread() {
	std::vector<NativeLightResolve> resolves;
	{
		MutexLock lock(native_resolve_mutex);
		resolves.swap(pending_native_resolves);
	}
	{
		MutexLock lock(params_mutex);
		for (const NativeLightResolve &resolve : resolves) {
			if (resolve.light_slot >= 0 && resolve.light_slot < native_light_count) {
				NativeLightState &state = native_light_states[resolve.light_slot];
				state.input = resolve;
				state.has_input = true;
			}
		}
	}
	native_resolve_pending.store(false);
}


void LRTVolume::_restart_sky_visibility() {
	sky_geometry_version.fetch_add(1);
	// A fixed direction advances monotonically along at least one grid axis.
	// No boundary dependency can be farther away than the longest grid extent.
	sky_visibility_iterations_remaining.store(MAX(grid.size[0], MAX(grid.size[1], grid.size[2])));
	sky_current = 0;
	for (RID buffer : sky_visibility_work_buffers) {
		device->buffer_clear(buffer, 0, size_t(grid.count) * sizeof(uint32_t));
	}
}

void LRTVolume::set_receive_sky(bool p_enabled) {
	if (receive_sky.exchange(p_enabled) == p_enabled || !has_local) {
		return;
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &LRTVolume::_set_receive_sky_render_thread));
}

void LRTVolume::_set_receive_sky_render_thread() {
	if (!has_local) {
		return;
	}
	if (receive_sky.load() && sky_published_version.load() != 0 &&
			sky_published_version.load() == sky_geometry_version.load()) {
		if (_project_sky_render_thread(false)) {
			_refresh_debug_textures();
		}
		return;
	}
	for (RID buffer : sky_buffers) {
		device->buffer_clear(buffer, 0, grid.count * 4 * sizeof(float));
	}
	source_full_dirty = true;

	injection_dirty.store(true);
	if (!injection_pending.exchange(true)) {
		_inject_render_thread();
	}
	_refresh_debug_textures();
}

void LRTVolume::advance_sky() {
	if (!receive_sky.load() || !has_local || apply_pending || (sky_visibility_iterations_remaining.load() == 0 && !sky_projection_dirty.load())) {
		return;
	}
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (sky_last_queued_frame == frame) {
		return;
	}
	sky_last_queued_frame = frame;
	sky_task_submissions.fetch_add(1);
	RenderingServer::get_singleton()->call_on_render_thread(
			callable_mp(this, &LRTVolume::_advance_sky_render_thread).bind(frame));
}

bool LRTVolume::_project_sky_render_thread(bool p_visibility) {
	sky_projection_dirty.exchange(false);
	if (!_upload_params(nullptr, false)) {
		has_local = false;
		return false;
	}
	struct PushConstant {
		int32_t probe_count;
		int32_t project_visibility;
		int32_t pad1;
		int32_t pad2;
	} push_constant = { grid.count, p_visibility ? 1 : 0, 0, 0 };
	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, pipeline_sky_project);
	device->compute_list_bind_uniform_set(list, uniform_set_sky_project, 0);
	device->compute_list_set_push_constant(list, &push_constant, sizeof(push_constant));
	device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
	device->compute_list_end();
	sky_projection_dispatches.fetch_add(1);
	source_full_dirty = true;

	injection_dirty.store(true);
	if (!injection_pending.exchange(true)) {
		_inject_render_thread();
	}
	return true;
}

void LRTVolume::_advance_sky_render_thread(uint64_t p_frame) {
	if (!receive_sky.load() || !has_local || apply_pending || sky_last_frame == p_frame) {
		return;
	}
	sky_last_frame = p_frame;
	if (sky_visibility_iterations_remaining.load() > 0) {
		if (!_upload_params(nullptr, false)) {
			has_local = false;
			return;
		}
		const int rounds = MIN(32, sky_visibility_iterations_remaining.load());
		int32_t publish[4] = {};
		RD::ComputeListID list = device->compute_list_begin();
		device->compute_list_bind_compute_pipeline(list, pipeline_sky_solve);
		device->compute_list_set_push_constant(list, publish, sizeof(publish));
		for (int round = 0; round < rounds; round++) {
			device->compute_list_bind_uniform_set(list, uniform_set_sky_solve[sky_current], 0);
			device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
			device->compute_list_add_barrier(list);
			sky_current = 1 - sky_current;
		}
		sky_visibility_rounds.fetch_add(rounds);
		const int remaining = sky_visibility_iterations_remaining.fetch_sub(rounds) - rounds;
		if (remaining == 0) {
			publish[0] = 1;
			device->compute_list_set_push_constant(list, publish, sizeof(publish));
			device->compute_list_bind_uniform_set(list, uniform_set_sky_solve[sky_current], 0);
			device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
		}
		device->compute_list_end();
		if (remaining == 0) {
			if (!_project_sky_render_thread(true)) {
				has_local = false;
				return;
			}
			sky_published_version.store(sky_geometry_version.load());
			sky_publications.fetch_add(1);
			debug_sky_frame = Engine::get_singleton()->get_frames_drawn();
			_refresh_debug_textures();
			return;
		}
	}
	if (sky_published_version.load() != 0 && sky_projection_dirty.load()) {
		if (_project_sky_render_thread(false)) {
			_refresh_debug_textures();
		}
	}
}

PackedFloat32Array LRTVolume::read_sky_visibility() const {
	PackedFloat32Array result;
	if (!has_local || !(allocated_debug_fields.load() & DEBUG_SKY_VISIBILITY)) {
		return result;
	}
	result.resize(visibility_cpu.size());
	for (size_t probe = 0; probe < visibility_cpu.size(); probe++) {
		result.set(probe, visibility_cpu[probe]);
	}
	return result;
}

void LRTVolume::step(int p_iterations) {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before step().");
	ERR_FAIL_COND(p_iterations < 1);
	ERR_FAIL_COND(_ensure_device() != OK);
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	const uint64_t wait_start = OS::get_singleton()->get_ticks_usec();
	const int start_iteration = iteration;
	pending_step_iterations.fetch_add(p_iterations);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_step_render_thread).bind(p_iterations, start_iteration));
	const double total_ms = double(OS::get_singleton()->get_ticks_usec() - wait_start) / 1000.0;
	last_cpu_wait_ms.store(0.0);
	last_cpu_submit_ms.store(total_ms);
	iteration += p_iterations;
}

void LRTVolume::step_radiance_only(int p_iterations) {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before step().");
	ERR_FAIL_COND(p_iterations < 1);
	ERR_FAIL_COND(_ensure_device() != OK);
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	const uint64_t wait_start = OS::get_singleton()->get_ticks_usec();
	const int start_iteration = iteration;
	pending_step_iterations.fetch_add(p_iterations);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_step_render_thread).bind(p_iterations, start_iteration));
	const double total_ms = double(OS::get_singleton()->get_ticks_usec() - wait_start) / 1000.0;
	last_cpu_wait_ms.store(0.0);
	last_cpu_submit_ms.store(total_ms);
	iteration += p_iterations;
}

void LRTVolume::_step_render_thread(int p_iterations, int p_start_iteration) {
	if (injection_pending.load()) {
		LRTRenderBridge::defer_native_light_resolve(callable_mp(this, &LRTVolume::_step_render_thread).bind(p_iterations, p_start_iteration));
		return;
	}
	{
		MutexLock lock(light_input_mutex);
		propagated_light_inputs = injected_light_inputs;
		propagated_light_source_version = injected_light_source_version;
		propagated_geometry_input_usec = local_geometry_input_usec.load();
		propagated_geometry_local_version = local_field_version.load();
	}
	_update_gpu_timing();
	_upload_params();
	const bool timing_active = _begin_gpu_timestamp(GPU_TIMING_PROPAGATE, p_iterations, uint64_t(p_start_iteration + p_iterations));
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	RD::ComputeListID list = device->compute_list_begin();
	for (int i = 0; i < p_iterations; i++) {
		struct PushConstant {
			int32_t reserved;
			int32_t reserved2;
			int32_t pad;
			int32_t external_gi_enabled;
		};
		PushConstant push_constant = { 0, 0, 0,
			external_gi_buffers_full_size.load() ? 1 : 0 };
		device->compute_list_bind_compute_pipeline(list, pipeline_propagate);
		device->compute_list_bind_uniform_set(list, uniform_set_propagate[current], 0);
		device->compute_list_set_push_constant(list, &push_constant, sizeof(PushConstant));
		device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
		current = 1 - current;
		if (i + 1 < p_iterations) {
			device->compute_list_add_barrier(list);
		}
	}
	device->compute_list_end();
	_end_gpu_timestamp(GPU_TIMING_PROPAGATE, timing_active);
	gpu_pass_dispatches[GPU_TIMING_PROPAGATE].fetch_add(1);
	_refresh_debug_textures();
	const double elapsed_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
	last_cpu_submit_ms.store(elapsed_ms);
	last_render_thread_pass_ms[GPU_TIMING_PROPAGATE].store(elapsed_ms);
	debug_rgb_frame = Engine::get_singleton()->get_frames_drawn();
	pending_step_iterations.fetch_sub(p_iterations);
}

int LRTVolume::get_pending_step_iterations() const {
	return pending_step_iterations.load();
}

double LRTVolume::measure_step_gpu_completion_ms(int p_iterations) {
	ERR_FAIL_COND_V_MSG(!has_local, 0.0, "build_local_field() must run before measuring propagation.");
	ERR_FAIL_COND_V(p_iterations <= 0, 0.0);
	ERR_FAIL_COND_V(_ensure_device() != OK, 0.0);
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, 0.0);
	const bool profiling_was_enabled = lrt_gpu_profiling_enabled.exchange(true);
	const int start_iteration = iteration;
	pending_step_iterations.fetch_add(p_iterations);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_step_render_thread)
			.bind(p_iterations, start_iteration));
	// The main RenderingDevice is submitted by RenderingServer, so calling RenderingDevice::sync()
	// on it is invalid. Force one diagnostic frame, then query the timestamps captured around the
	// propagation dispatch from the render thread. Production never enters this blocking path.
	rendering_server->draw(false, 0.0);
	// An on-demand measurement must observe the ranges it just forced, so it bypasses the
	// once-per-frame readback throttle.
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_update_gpu_timing).bind(true));
	rendering_server->sync();
	lrt_gpu_profiling_enabled.store(profiling_was_enabled);
	iteration += p_iterations;
	return last_gpu_ms.load();
}

void LRTVolume::reset() {
	ERR_FAIL_COND_MSG(!has_local, "build_local_field() must run before reset().");
	ERR_FAIL_COND(_ensure_device() != OK);
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_reset_render_thread));
	rendering_server->sync();
	iteration = 0;
}

void LRTVolume::_reset_render_thread() {
	{
		MutexLock lock(light_input_mutex);
		propagated_light_inputs.clear();
		propagated_light_source_version = 0;
		propagated_geometry_input_usec = 0;
		propagated_geometry_local_version = 0;
	}
	const size_t bytes = size_t(grid.count) * 4 * sizeof(float);
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			device->buffer_clear(radiance_buffers[buffer][channel], 0, bytes);
		}
	}
	current = 0;
	_refresh_debug_textures();
}

void LRTVolume::_refresh_debug_textures() {
	if (local_debug_textures_dirty.exchange(false)) {
		_upload_local_textures();
	}
}

uint32_t LRTVolume::get_debug_update_activity() const {
	const uint64_t frame = Engine::get_singleton()->get_frames_drawn();
	auto recent = [frame](uint64_t p_last) { return p_last != UINT64_MAX && frame - p_last <= 1; };
	return (recent(debug_local_frame) ? 1u : 0u) | (recent(debug_source_frame) ? 2u : 0u) |
			(recent(debug_rgb_frame) ? 4u : 0u) | (recent(debug_sky_frame) ? 8u : 0u);
}

AABB LRTVolume::get_debug_source_region() const {
	return AABB(Vector3(debug_source_region.minimum[0], debug_source_region.minimum[1], debug_source_region.minimum[2]),
			Vector3(debug_source_region.size[0], debug_source_region.size[1], debug_source_region.size[2]));
}

void LRTVolume::mark_gathered() {
	MutexLock lock(light_input_mutex);
	displayed_light_inputs = propagated_light_inputs;
	displayed_light_source_version = propagated_light_source_version;
	displayed_geometry_input_usec = propagated_geometry_input_usec;
	displayed_geometry_local_version = propagated_geometry_local_version;
	displayed_light_submission_frame = Engine::get_singleton()->get_frames_drawn();
}

void LRTVolume::_update_gpu_timing(bool p_force) {
	// The captured results only change once the device records a new frame, so repeated calls from
	// the injection and resolve loops in the same frame re-read the same readback. That readback
	// stalls the render thread, and its cost grew with the number of capture passes, i.e. with the
	// light count: it must never be paid per pass.
	const uint64_t drawn = Engine::get_singleton()->get_frames_drawn();
	if (!p_force) {
		if (drawn == gpu_timing_processed_frame) {
			return;
		}
		gpu_timing_processed_frame = drawn;
	}
	const uint64_t readback_started_usec = OS::get_singleton()->get_ticks_usec();
	uint64_t begin[GPU_TIMING_PASS_COUNT] = {};
	uint64_t latest_end[GPU_TIMING_PASS_COUNT] = {};
	double totals_ms[GPU_TIMING_PASS_COUNT] = {};
	int samples[GPU_TIMING_PASS_COUNT] = {};
	int begin_matches[GPU_TIMING_PASS_COUNT] = {};
	int end_matches[GPU_TIMING_PASS_COUNT] = {};
	const uint32_t count = device->get_captured_timestamps_count();
	const uint64_t captured_frame = device->get_captured_timestamps_frame();
	last_gpu_timestamp_frame.store(captured_frame);
	for (uint32_t index = 0; index < count; index++) {
		const String name = device->get_captured_timestamp_name(index);
		for (int pass = 0; pass < GPU_TIMING_PASS_COUNT; pass++) {
			if (name == timestamp_begin_names[pass]) {
				begin[pass] = device->get_captured_timestamp_gpu_time(index);
				begin_matches[pass]++;
			} else if (name == timestamp_end_names[pass] && begin[pass] > 0) {
				end_matches[pass]++;
				const uint64_t end = device->get_captured_timestamp_gpu_time(index);
				if (end >= begin[pass]) {
					totals_ms[pass] += double(end - begin[pass]) / 1000000.0;
					samples[pass]++;
					latest_end[pass] = end;
				}
				begin[pass] = 0;
			}
		}
	}
	bool propagation_updated = false;
	for (int pass = 0; pass < GPU_TIMING_PASS_COUNT; pass++) {
		last_gpu_timestamp_begin_matches[pass].store(begin_matches[pass]);
		last_gpu_timestamp_end_matches[pass].store(end_matches[pass]);
		if (samples[pass] > 0 && latest_end[pass] != last_completed_gpu_timestamp_end[pass]) {
			last_gpu_pass_ms[pass].store(totals_ms[pass]);
			last_gpu_pass_samples[pass].store(samples[pass]);
			last_gpu_pass_work_items[pass].store(gpu_timestamp_pending_work_items[pass]);
			last_gpu_pass_batch_version[pass].store(gpu_timestamp_pending_batch_version[pass]);
			last_gpu_pass_local_version[pass].store(gpu_timestamp_pending_local_version[pass]);
			last_gpu_pass_source_version[pass].store(gpu_timestamp_pending_source_version[pass]);
			last_gpu_pass_submission_frame[pass].store(gpu_timestamp_pending_submission_frame[pass]);
			completed_gpu_pass_ranges[pass].fetch_add(uint64_t(samples[pass]));
			gpu_timestamp_pending[pass] = false;
			last_completed_gpu_timestamp_end[pass] = latest_end[pass];
			propagation_updated = propagation_updated || pass == GPU_TIMING_PROPAGATE;
		} else if (gpu_timestamp_pending[pass] && captured_frame >= gpu_timestamp_pending_result_frame[pass] + 4) {
			// Timestamp pools can be reset while a solver is created or profiling toggles. A
			// missing range must not permanently disable production timing.
			gpu_timestamp_pending[pass] = false;
			dropped_gpu_timestamp_ranges[pass].fetch_add(1);
		}
	}
	if (propagation_updated) {
		last_gpu_ms.store(totals_ms[GPU_TIMING_PROPAGATE]);
	}
	last_gpu_timing_readback_ms.store(double(OS::get_singleton()->get_ticks_usec() - readback_started_usec) / 1000.0);
}

bool LRTVolume::_begin_gpu_timestamp(GpuTimingPass p_pass, int p_work_items, uint64_t p_batch_version) {
	latest_gpu_pass_batch_version[p_pass].store(p_batch_version);
	latest_gpu_pass_local_version[p_pass].store(local_field_version.load());
	latest_gpu_pass_source_version[p_pass].store(source_version.load());
	latest_gpu_pass_submission_frame[p_pass].store(Engine::get_singleton()->get_frames_drawn());
	// Production GPU ranges report measured cost. Multiple non-propagation passes in one
	// frame are summed; propagation keeps one range with a matching iteration count.
	if (gpu_timestamp_pending[p_pass] &&
			(p_pass == GPU_TIMING_PROPAGATE || gpu_timestamp_pending_result_frame[p_pass] != device->get_captured_timestamps_frame())) {
		return false;
	}
	device->capture_timestamp(timestamp_begin_names[p_pass]);
	gpu_timestamp_pending[p_pass] = true;
	gpu_timestamp_pending_work_items[p_pass] = MAX(1, p_work_items);
	gpu_timestamp_pending_batch_version[p_pass] = p_batch_version;
	gpu_timestamp_pending_local_version[p_pass] = local_field_version.load();
	gpu_timestamp_pending_source_version[p_pass] = source_version.load();
	gpu_timestamp_pending_submission_frame[p_pass] = Engine::get_singleton()->get_frames_drawn();
	gpu_timestamp_pending_result_frame[p_pass] = device->get_captured_timestamps_frame();
	return true;
}

void LRTVolume::_end_gpu_timestamp(GpuTimingPass p_pass, bool p_active) {
	if (p_active) {
		device->capture_timestamp(timestamp_end_names[p_pass]);
	}
}

int LRTVolume::get_iteration() const {
	return iteration;
}

Dictionary LRTVolume::get_grid() const {
	Dictionary result;
	result["min"] = Vector3(grid.min.x, grid.min.y, grid.min.z);
	result["size"] = Vector3i(grid.size[0], grid.size[1], grid.size[2]);
	result["spacing"] = grid.spacing;
	result["width"] = grid.width;
	result["height"] = grid.height;
	result["count"] = grid.count;
	return result;
}

Dictionary LRTVolume::get_debug_resources() const {
	Dictionary result = LRTRenderBridge::get_gather_resources(render_owner);
	result["local_visibility_buffer"] = local_visibility_buffer;
	result["material_buffer"] = material_buffer;
	result["published_sky_visibility_buffer"] = published_sky_visibility_buffer;
	result["radiance_r_buffer"] = radiance_buffers[current][0];
	result["radiance_g_buffer"] = radiance_buffers[current][1];
	result["radiance_b_buffer"] = radiance_buffers[current][2];
	result["debug_direct_shadow_bound"] = debug_direct_shadow_bound;
	result["directional_shadow_texture"] = directional_shadows.get_texture();
	result["deferred_resolve_flushes"] = int64_t(LRTRenderBridge::get_deferred_resolve_flushes());
	return result;
}

void LRTVolume::_publish_primitives() {
	MutexLock lock(debug_geometry_mutex);
	primitives = std::move(staged_primitives);
	debug_geometry_version++;
}

void LRTVolume::_free_debug_geometry_resources() {
	for (const auto &asset : debug_geometry_assets) {
		if (asset.second.buffer.is_valid()) {
			device->free_rid(asset.second.buffer);
		}
	}
	debug_geometry_assets.clear();
	debug_geometry_draws.clear();
	debug_geometry_uploaded_version = UINT64_MAX;
	debug_geometry_buffer_bytes.store(0);
}

Array LRTVolume::get_geometry_debug_resources() {
	// Only called on the render thread. Snapshot immutable assets and transforms together;
	// the main thread can publish another geometry build while the buffers are generated.
	if (!(requested_debug_fields.load() & DEBUG_COARSE_GEOMETRY)) {
		return Array();
	}
	if (apply_pending) {
		return debug_geometry_draws;
	}
	std::vector<lrt::GeometryInstance> snapshot;
	uint64_t version;
	{
		MutexLock lock(debug_geometry_mutex);
		if (debug_geometry_uploaded_version == debug_geometry_version) {
			return debug_geometry_draws;
		}
		snapshot = primitives;
		version = debug_geometry_version;
	}
	std::set<DebugGeometryKey> used_assets;
	debug_geometry_draws.clear();
	uint64_t buffer_bytes = 0;
	for (const lrt::GeometryInstance &primitive : snapshot) {
		const auto &geometry = primitive.geometry;
		if (!geometry) {
			continue;
		}
		const DebugGeometryKey key(geometry, primitive.instance);
		used_assets.insert(key);
		auto asset = debug_geometry_assets.find(key);
		if (asset == debug_geometry_assets.end()) {
			DebugGeometryAsset resource;
			const std::vector<float> positions = lrt::sdf_surface_cells(*geometry);
			resource.cells = uint32_t(positions.size() / 4);
			std::vector<float> cells;
			cells.reserve(size_t(resource.cells) * 12);
			for (size_t cell = 0; cell < positions.size(); cell += 4) {
				const lrt::Vec3 position(positions[cell], positions[cell + 1], positions[cell + 2]);
				const lrt::SurfaceSample surface = lrt::sample_sdf_fields(*geometry, *primitive.instance, position);
				cells.insert(cells.end(), { float(position.x), float(position.y), float(position.z), 1.0f,
						float(surface.color.x), float(surface.color.y), float(surface.color.z), 0.0f,
						float(surface.emission.x), float(surface.emission.y), float(surface.emission.z), 0.0f });
			}
			if (!cells.empty()) {
				resource.buffer = device->storage_buffer_create(uint32_t(cells.size() * sizeof(float)), bytes_of(cells.data(), cells.size() * sizeof(float)));
				ERR_FAIL_COND_V(resource.buffer.is_null(), Array());
				debug_geometry_buffer_bytes.fetch_add(cells.size() * sizeof(float));
			}
			asset = debug_geometry_assets.emplace(key, resource).first;
		}
		if (asset->second.cells == 0) {
			continue;
		}
		const Basis basis(
				Vector3(primitive.basis_x.x, primitive.basis_x.y, primitive.basis_x.z),
				Vector3(primitive.basis_y.x, primitive.basis_y.y, primitive.basis_y.z),
				Vector3(primitive.basis_z.x, primitive.basis_z.y, primitive.basis_z.z));
		Dictionary draw;
		draw["buffer"] = asset->second.buffer;
		draw["cells"] = asset->second.cells;
		draw["cell_size"] = geometry->cell / (geometry->subcells.indices.empty() ? 1 : lrt::SubcellCoverage::RESOLUTION);
		draw["transform"] = Transform3D(basis, Vector3(primitive.origin.x, primitive.origin.y, primitive.origin.z));
		debug_geometry_draws.push_back(draw);
	}
	for (auto asset = debug_geometry_assets.begin(); asset != debug_geometry_assets.end();) {
		if (used_assets.count(asset->first)) {
			buffer_bytes += uint64_t(asset->second.cells) * 12 * sizeof(float);
			asset++;
			continue;
		}
		if (asset->second.buffer.is_valid()) {
			device->free_rid(asset->second.buffer);
		}
		asset = debug_geometry_assets.erase(asset);
	}
	debug_geometry_buffer_bytes.store(buffer_bytes);
	debug_geometry_uploaded_version = version;
	return debug_geometry_draws;
}

void LRTVolume::_read_back_render_thread() {
	readback_error = OK;
	const size_t bytes = size_t(grid.count) * 4 * sizeof(float);
	for (int channel = 0; channel < 3; channel++) {
		const Vector<uint8_t> data = device->buffer_get_data(radiance_buffers[current][channel]);
		if (int64_t(data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		radiance_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(radiance_cpu[channel].data(), data.ptr(), bytes);
		const Vector<uint8_t> sky_data = device->buffer_get_data(sky_buffers[channel]);
		if (int64_t(sky_data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		sky_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(sky_cpu[channel].data(), sky_data.ptr(), bytes);
	}
	if (allocated_debug_fields.load() & DEBUG_SKY_VISIBILITY) {
		const Vector<uint8_t> visibility_data = device->buffer_get_data(published_sky_visibility_buffer);
		if (int64_t(visibility_data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		visibility_cpu.resize(size_t(grid.count) * 4);
		memcpy(visibility_cpu.data(), visibility_data.ptr(), bytes);

	} else {
		visibility_cpu.clear();
	}
	for (int channel = 0; channel < 3; channel++) {
		const Vector<uint8_t> data = device->buffer_get_data(source_buffers[channel]);
		if (int64_t(data.size()) != int64_t(local_record_capacity.load()) * 4 * sizeof(float)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		source_cpu[channel].assign(size_t(grid.count) * 4, 0.0f);
		for (int probe = 0; probe < grid.count; probe++) {
			const uint32_t slot = local.local_record_indices[size_t(probe)];
			if (slot != lrt::LocalField::NO_RECORD) {
				memcpy(source_cpu[channel].data() + size_t(probe) * 4,
						data.ptr() + size_t(slot) * 4 * sizeof(float), 4 * sizeof(float));
			}
		}

	}
	if (external_gi_buffers_full_size.load()) {
		const uint64_t boundary_bytes = lrt::boundary_connection_count(grid.size[0], grid.size[1], grid.size[2]) * 4 * sizeof(float);
		const Vector<uint8_t> data = device->buffer_get_data(external_gi_buffer);
		if (uint64_t(data.size()) != boundary_bytes) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		external_gi_cpu.resize(boundary_bytes / sizeof(float));
		memcpy(external_gi_cpu.data(), data.ptr(), boundary_bytes);
	} else {
		external_gi_cpu.clear();
	}
}

void LRTVolume::refresh_display() {
	if (!has_local || _ensure_device() != OK) {
		return;
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_read_back_render_thread));
	rendering_server->sync();
	last_readback_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
	if (readback_error == OK) {
		diagnostic_readbacks++;
	}
}

Ref<Texture2D> LRTVolume::get_texture(const String &p_name) const {
	if (p_name == "diagnostic_dirty") {
		return diagnostic_dirty_texture;
	}
	return Ref<Texture2D>();
}

PackedFloat32Array LRTVolume::read_field(const String &p_name) const {
	const std::vector<float> *values = nullptr;
	std::vector<float> record_values;
	if (p_name == "radiance_r") {
		values = &radiance_cpu[0];
	} else if (p_name == "radiance_g") {
		values = &radiance_cpu[1];
	} else if (p_name == "radiance_b") {
		values = &radiance_cpu[2];
	} else if (p_name == "sky_r") {
		values = &sky_cpu[0];
	} else if (p_name == "sky_g") {
		values = &sky_cpu[1];
	} else if (p_name == "sky_b") {
		values = &sky_cpu[2];
	} else if (p_name == "visibility") {
		values = &visibility_cpu;
	} else if (p_name == "source_r") {
		values = &source_cpu[0];
	} else if (p_name == "source_g") {
		values = &source_cpu[1];
	} else if (p_name == "source_b") {
		values = &source_cpu[2];
	} else if (p_name == "external_boundary") {
		values = &external_gi_cpu;
	} else if (p_name == "material") {
		values = &local.material;
	} else if (p_name == "emission_sh_r" || p_name == "emission_sh_g" || p_name == "emission_sh_b") {
		int channel = 0;
		if (p_name == "emission_sh_g") {
			channel = 1;
		} else if (p_name == "emission_sh_b") {
			channel = 2;
		}
		record_values.resize(local.local_record_indices.size() * 4);
		for (size_t probe = 0; probe < local.local_record_indices.size(); probe++) {
			memcpy(record_values.data() + probe * 4, local.get_local_record(probe).emission.data() + channel * 4, 4 * sizeof(float));
		}
		values = &record_values;
	} else if (p_name == "matrices") {
		record_values = encode_local_records(local, grid.count);
		values = &record_values;
	} else if (p_name == "diagnostic_sdf" || p_name == "diagnostic_albedo" ||
			p_name == "diagnostic_emission" || p_name == "diagnostic_dirty") {
		std::vector<float> pixels;
		if (p_name == "diagnostic_dirty") {
			pixels = dirty_mask_rgba(local.dirty_mask);
		} else if (p_name == "diagnostic_sdf") {
			pixels = sample_sdf_diagnostic_rgba(local_cache, size_t(grid.count));
		} else {
			pixels = lrt::local_material_rgba(local, p_name == "diagnostic_emission");
		}
		PackedFloat32Array result;
		result.resize(pixels.size());
		if (!pixels.empty()) {
			memcpy(result.ptrw(), pixels.data(), pixels.size() * sizeof(float));
		}
		return result;
	}
	PackedFloat32Array result;
	if (!values || values->empty()) {
		return result;
	}
	result.resize(values->size());
	memcpy(result.ptrw(), values->data(), values->size() * sizeof(float));
	return result;
}

PackedFloat32Array LRTVolume::read_local_visibility() const {
	PackedFloat32Array result;
	result.resize(local.local_visibility.size() * 4);
	if (!local.local_visibility.empty()) {
		memcpy(result.ptrw(), local.local_visibility.data(), local.local_visibility.size() * sizeof(lrt::SH4));
	}
	return result;
}

PackedInt32Array LRTVolume::read_local_record_indices() const {
	PackedInt32Array result;
	result.resize(local.local_record_indices.size());
	if (!local.local_record_indices.empty()) {
		memcpy(result.ptrw(), local.local_record_indices.data(), local.local_record_indices.size() * sizeof(uint32_t));
	}
	return result;
}

Dictionary LRTVolume::sample_geometry(const Vector3 &p_point) const {
	const lrt::Vec3 point(p_point.x, p_point.y, p_point.z);
	for (const lrt::GeometryInstance &primitive : primitives) {
		if (primitive.precomputed) {
			if (!primitive.occupies_probe(point)) {
				continue;
			}
			const lrt::TransferSample transfer = primitive.precomputed->sample(point);
			Dictionary result;
			result["occupied"] = true;
			result["representation"] = "precomputed";
			result["local_visibility"] = Vector4(transfer.visibility[0], transfer.visibility[1], transfer.visibility[2], transfer.visibility[3]);
			PackedFloat32Array reflection;
			reflection.resize(48);
			memcpy(reflection.ptrw(), transfer.reflection.data(), 48 * sizeof(float));
			result["reflection"] = reflection;
			PackedFloat32Array emission;
			emission.resize(12);
			memcpy(emission.ptrw(), transfer.emission.data(), 12 * sizeof(float));
			result["emission_sh"] = emission;
			return result;
		}
		const lrt::SurfaceSample sample = primitive.sample(point);
		if (!sample.valid) {
			continue;
		}
		Dictionary result;
		result["occupied"] = true;
		result["normal"] = Vector3(sample.normal.x, sample.normal.y, sample.normal.z);
		result["albedo"] = Vector3(sample.color.x, sample.color.y, sample.color.z);
		result["emission"] = Vector3(sample.emission.x, sample.emission.y, sample.emission.z);
		return result;
	}
	return Dictionary();
}

void LRTVolume::clear_shared_sdf_cache() {
	lrt::clear_shared_asset_fields();
}

Dictionary LRTVolume::get_stats() const {
	Dictionary result;
	result["sky_task_submissions"] = int64_t(sky_task_submissions.load());
	result["iteration"] = iteration;
	result["propagation_passes"] = iteration;
	result["sky_visibility_rounds"] = int64_t(sky_visibility_rounds.load());
	result["sky_projection_dispatches"] = int64_t(sky_projection_dispatches.load());
	result["sky_iterations_remaining"] = sky_visibility_iterations_remaining.load();
	result["sky_geometry_version"] = int64_t(sky_geometry_version.load());
	result["sky_published_version"] = int64_t(sky_published_version.load());
	result["sky_publications"] = int64_t(sky_publications.load());
	result["pending_step_iterations"] = pending_step_iterations.load();
	result["injection_pending"] = injection_pending.load();
	result["last_gpu_ms"] = last_gpu_ms.load();
	result["last_gpu_work_items"] = last_gpu_pass_work_items[GPU_TIMING_PROPAGATE].load();
	result["last_gpu_batch_version"] = int64_t(last_gpu_pass_batch_version[GPU_TIMING_PROPAGATE].load());
	result["last_cpu_submit_ms"] = last_cpu_submit_ms.load();
	result["gpu_timing_readback_ms"] = last_gpu_timing_readback_ms.load();
	result["last_cpu_wait_ms"] = last_cpu_wait_ms.load();
	result["last_readback_ms"] = last_readback_ms;
	result["diagnostic_readbacks"] = diagnostic_readbacks;
	result["rendering_device"] = "main";
	result["solid"] = local.solid_count;
	result["surface"] = local.surface_count;
	result["count"] = grid.count;
	result["matrix_encoding"] = "rgb_ltm_localvis_sh4_f32_26";
	result["local_record_cpu_bytes"] = int64_t(vector_bytes(local.local_records));
	result["local_record_index_cpu_bytes"] = int64_t(vector_bytes(local.local_record_indices));
	result["local_record_allocator_cpu_bytes"] = int64_t(vector_bytes(local.local_record_owners) + vector_bytes(local.free_local_records));
	result["local_record_count"] = int64_t(local.local_records.size() - local.free_local_records.size());
	result["local_record_slots"] = int64_t(local.local_records.size());
	result["local_record_capacity"] = local_record_capacity.load();
	result["local_record_upload_cpu_bytes"] = int64_t(vector_bytes(local.gpu_local_records));
	result["geometry_dirty_trunks"] = local.geometry_dirty_trunks;
	result["reflectance_dirty_trunks"] = local.reflectance_dirty_trunks;
	result["emission_dirty_trunks"] = local.emission_dirty_trunks;
	result["occupancy_probes_updated"] = local.occupancy_probes_updated;
	result["geometry_records_built"] = local.geometry_records_built;
	result["reflectance_records_updated"] = local.reflectance_records_updated;
	result["emission_records_updated"] = local.emission_records_updated;
	result["surface_workspaces_built"] = local.surface_workspaces_built;
	result["surface_workspace_instances"] = local.surface_workspace_instances;
	result["surface_workspace_payload_bytes"] = local.surface_workspace_payload_bytes;
	result["spatial_index_payload_bytes"] = int64_t(spatial_index_payload_bytes.load());
	result["spatial_index_registrations"] = int64_t(spatial_index_registrations.load());
	result["spatial_index_candidate_references"] = int64_t(spatial_index_candidate_references.load());
	result["spatial_index_overlap_tests"] = int64_t(spatial_index_overlap_tests.load());
	result["static_occupancy_built"] = static_occupancy_built.load();
	result["static_occupancy_reused"] = static_occupancy_reused.load();
	result["static_occupancy_candidates"] = int64_t(static_occupancy_candidates.load());
	result["static_occupancy_ms"] = static_occupancy_ms.load();
	result["backend"] = local_backend;
	result["buffers_ready"] = radiance_buffers[current][0].is_valid();
	result["gpu_bytes"] = _gpu_bytes();
	return result;
}

Dictionary LRTVolume::get_performance_stats() const {
	Dictionary result;
	{
		MutexLock lock(light_input_mutex);
		auto inputs_to_array = [](const std::vector<NativeLightInput> &p_inputs) {
			Array inputs;
			for (const NativeLightInput &input : p_inputs) {
				Dictionary entry;
				entry["instance_id"] = int64_t(input.instance_id);
				entry["oldest_input_usec"] = int64_t(input.oldest_usec);
				entry["newest_input_usec"] = int64_t(input.newest_usec);
				inputs.push_back(entry);
			}
			return inputs;
		};
		result["injected_light_inputs"] = inputs_to_array(injected_light_inputs);
		result["displayed_light_inputs"] = inputs_to_array(displayed_light_inputs);
		result["injected_light_source_version"] = int64_t(injected_light_source_version);
		result["displayed_light_source_version"] = int64_t(displayed_light_source_version);
		result["displayed_geometry_input_usec"] = int64_t(displayed_geometry_input_usec);
		result["displayed_geometry_local_version"] = int64_t(displayed_geometry_local_version);
		result["injected_light_submission_frame"] = int64_t(injected_light_submission_frame);
		result["displayed_light_submission_frame"] = int64_t(displayed_light_submission_frame);
	}
	static const char *pass_keys[GPU_TIMING_PASS_COUNT] = { "inject", "light_resolve", "propagate" };
	Dictionary gpu_ms;
	Dictionary render_thread_ms;
	Dictionary dispatches;
	Dictionary timestamp_samples;
	Dictionary work_items;
	Dictionary batch_versions;
	Dictionary local_versions;
	Dictionary source_versions;
	Dictionary submission_frames;
	Dictionary latest_batch_versions;
	Dictionary latest_local_versions;
	Dictionary latest_source_versions;
	Dictionary latest_submission_frames;
	Dictionary completed_timestamp_ranges;
	Dictionary timestamp_begin_matches;
	Dictionary timestamp_end_matches;
	Dictionary dropped_timestamp_ranges;
	for (int pass = 0; pass < GPU_TIMING_PASS_COUNT; pass++) {
		gpu_ms[pass_keys[pass]] = last_gpu_pass_ms[pass].load();
		render_thread_ms[pass_keys[pass]] = last_render_thread_pass_ms[pass].load();
		dispatches[pass_keys[pass]] = gpu_pass_dispatches[pass].load();
		timestamp_samples[pass_keys[pass]] = last_gpu_pass_samples[pass].load();
		work_items[pass_keys[pass]] = last_gpu_pass_work_items[pass].load();
		batch_versions[pass_keys[pass]] = int64_t(last_gpu_pass_batch_version[pass].load());
		local_versions[pass_keys[pass]] = int64_t(last_gpu_pass_local_version[pass].load());
		source_versions[pass_keys[pass]] = int64_t(last_gpu_pass_source_version[pass].load());
		submission_frames[pass_keys[pass]] = int64_t(last_gpu_pass_submission_frame[pass].load());
		latest_batch_versions[pass_keys[pass]] = int64_t(latest_gpu_pass_batch_version[pass].load());
		latest_local_versions[pass_keys[pass]] = int64_t(latest_gpu_pass_local_version[pass].load());
		latest_source_versions[pass_keys[pass]] = int64_t(latest_gpu_pass_source_version[pass].load());
		latest_submission_frames[pass_keys[pass]] = int64_t(latest_gpu_pass_submission_frame[pass].load());
		completed_timestamp_ranges[pass_keys[pass]] = completed_gpu_pass_ranges[pass].load();
		timestamp_begin_matches[pass_keys[pass]] = last_gpu_timestamp_begin_matches[pass].load();
		timestamp_end_matches[pass_keys[pass]] = last_gpu_timestamp_end_matches[pass].load();
		dropped_timestamp_ranges[pass_keys[pass]] = int64_t(dropped_gpu_timestamp_ranges[pass].load());
	}
	result["gpu_ms"] = gpu_ms;
	result["render_thread_ms"] = render_thread_ms;
	result["dispatches"] = dispatches;
	result["timestamp_samples"] = timestamp_samples;
	result["work_items"] = work_items;
	result["batch_versions"] = batch_versions;
	result["local_versions"] = local_versions;
	result["source_versions"] = source_versions;
	result["submission_frames"] = submission_frames;
	result["latest_batch_versions"] = latest_batch_versions;
	result["latest_local_versions"] = latest_local_versions;
	result["latest_source_versions"] = latest_source_versions;
	result["latest_submission_frames"] = latest_submission_frames;
	result["completed_timestamp_ranges"] = completed_timestamp_ranges;
	result["timestamp_begin_matches"] = timestamp_begin_matches;
	result["timestamp_end_matches"] = timestamp_end_matches;
	result["timestamp_frame"] = int64_t(last_gpu_timestamp_frame.load());
	result["dropped_timestamp_ranges"] = dropped_timestamp_ranges;
	result["local_field_version"] = int64_t(local_field_version.load());
	result["source_version"] = int64_t(source_version.load());
	result["source_last_probe_count"] = int64_t(source_last_probe_count.load());
	result["source_total_probe_count"] = int64_t(source_total_probe_count.load());

	result["local_patch_upload_bytes"] = int64_t(apply_local_patch_upload_bytes);
	result["full_upload_bytes"] = int64_t(apply_full_upload_bytes);
	result["propagation_passes"] = iteration;
	result["sky_visibility_rounds"] = int64_t(sky_visibility_rounds.load());
	result["sky_projection_dispatches"] = int64_t(sky_projection_dispatches.load());
	result["sky_iterations_remaining"] = sky_visibility_iterations_remaining.load();
	result["sky_geometry_version"] = int64_t(sky_geometry_version.load());
	result["sky_published_version"] = int64_t(sky_published_version.load());
	result["sky_publications"] = int64_t(sky_publications.load());
	result["last_cpu_submit_ms"] = last_cpu_submit_ms.load();
	result["last_cpu_wait_ms"] = last_cpu_wait_ms.load();
	result["diagnostic_readback_ms"] = last_readback_ms;
	result["diagnostic_readbacks"] = diagnostic_readbacks;
	result["sky_task_submissions"] = int64_t(sky_task_submissions.load());
	result["gpu_timestamp_scope"] = "most_recent_completed_render_frame";
	result["gpu_timestamps_enabled"] = true;
	result["frame_profiling_enabled"] = lrt_gpu_profiling_enabled.load();
	return result;
}

Dictionary LRTVolume::get_memory_stats() const {
	return _gpu_memory_breakdown();
}

void LRTVolume::refresh_performance_stats() {
	if (!device) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	// Explicit refresh is an on-demand read, so it is not subject to the frame throttle.
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_update_gpu_timing).bind(true));
	rendering_server->sync();
}

void LRTVolume::set_render_frame_profiling_enabled(bool p_enabled) {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	lrt_gpu_profiling_enabled.store(p_enabled);
	LRTRenderBridge::set_performance_profiling_enabled(p_enabled);
	rendering_server->set_frame_profiling_enabled(p_enabled);
}

Dictionary LRTVolume::get_render_frame_profile() const {
	Dictionary result;
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, result);
	Array areas;
	const Vector<RenderingServerTypes::FrameProfileArea> profile = rendering_server->get_frame_profile();
	areas.resize(profile.size());
	for (int index = 0; index < profile.size(); index++) {
		Dictionary area;
		area["name"] = profile[index].name;
		area["gpu_ms"] = profile[index].gpu_msec;
		area["cpu_ms"] = profile[index].cpu_msec;
		areas[index] = area;
	}
	result["frame"] = int64_t(rendering_server->get_frame_profile_frame());
	result["areas"] = areas;
	return result;
}

Dictionary LRTVolume::get_preparation_status() const {
	Dictionary result;
	result["debug_direct_shadow_bound"] = debug_direct_shadow_bound;
	result["directional_shadow"] = directional_shadows.get_stats();
	const int phase = preparation_phase.load();
	static const char *phase_names[] = { "idle", "assets", "local", "visibility", "display", "ready", "failed" };
	result["phase"] = phase >= 0 && phase < 7 ? phase_names[phase] : "unknown";
	result["completed"] = preparation_completed.load();
	result["total"] = preparation_total.load();
	result["error"] = preparation_error;
	return result;
}
