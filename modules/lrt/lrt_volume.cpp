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

#include "lrt_cache.h"
#include "lrt_display.glsl.gen.h"
#include "lrt_inject.glsl.gen.h"
#include "lrt_light_resolve.glsl.gen.h"
#include "lrt_local_patch.glsl.gen.h"
#include "lrt_propagate.glsl.gen.h"
#include "lrt_render_bridge.h"
#include "lrt_sky_project.glsl.gen.h"

#include "core/config/engine.h"
#include "core/io/image.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "scene/resources/environment.h"
#include "scene/resources/texture.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

#include <algorithm>
#include <array>
#include <climits>
#include <set>

namespace {

constexpr int WORKGROUP_SIZE = 64;
constexpr size_t APPLY_UPLOAD_CHUNK_BYTES = 512 * 1024;
constexpr size_t APPLY_COPY_CHUNK_BYTES = 288 * 1024;
constexpr int MAX_LOCAL_PATCH_PROBES = 8192;
constexpr int MAX_RECEIVER_PATCHES = 65536;
constexpr int PROBES_PER_LOCAL_TRUNK = 8 * 8 * 8;
constexpr int SKY_FACE_RESOLUTION = 8;
constexpr int SKY_DIRECTION_COUNT = LRTVolume::SKY_DIRECTION_COUNT;
static_assert(SKY_DIRECTION_COUNT == 6 * SKY_FACE_RESOLUTION * SKY_FACE_RESOLUTION);
constexpr int SKY_DIRECTION_WORDS = (SKY_DIRECTION_COUNT + 31) / 32;
constexpr int SKY_VISIBILITY_WORDS_PER_FRAME = SKY_DIRECTION_WORDS / 2;
static_assert(SKY_DIRECTION_WORDS % SKY_VISIBILITY_WORDS_PER_FRAME == 0);
constexpr int SKY_PATH_LENGTH = SKY_FACE_RESOLUTION;
constexpr double SH_C0 = 0.2820947918;
constexpr double SH_C1 = 0.4886025119;
// Mirrors the prototype's bakeBoxSDF() call, which always uses the default 24 for boxes.
constexpr int BOX_SDF_RESOLUTION = 24;
constexpr uint32_t LOCAL_CACHE_FORMAT_VERSION = 2;
constexpr char LOCAL_CACHE_MAGIC[8] = { 'L', 'R', 'T', 'L', 'O', 'C', '0', '1' };
std::atomic<bool> lrt_gpu_profiling_enabled{ false };

template <typename T>
void store_local_cache_values(const Ref<FileAccess> &p_file, const std::vector<T> &p_values) {
	p_file->store_64(uint64_t(p_values.size()));
	if (!p_values.empty()) {
		p_file->store_buffer(reinterpret_cast<const uint8_t *>(p_values.data()), uint64_t(p_values.size() * sizeof(T)));
	}
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
	std::array<Ref<RDShaderSPIRV>, 6> spirv;
	std::array<RID, 6> shaders;
	std::array<RID, 6> pipelines;
};

SharedShaderResources lrt_shared_shaders;

// std140 layout shared by both compute shaders.
struct ParamsData {
	int32_t grid_size[4] = { 0, 0, 0, 0 };
	float grid_min[4] = { 0, 0, 0, 0 };
	int32_t counts[4] = { 0, 0, 0, 0 };
	float flags[4] = { 0, 0, 0, 0 }; // x multi bounce, y SH visibility, z color SDF, w native receiver lighting
	float sky_samples[SKY_DIRECTION_COUNT][4] = {};
};

struct NativeLightStateData {
	float scale[4] = { 0, 0, 0, 0 };
	float state[4] = { 0, 0, 0, 0 };
	float influence[4] = { 0, 0, 0, -1 };
};

struct NativeLightResolvePushConstant {
	float volume_to_source[16] = {};
	float ranges[4] = {};
	int32_t layout[4] = {};
	int32_t kind[4] = {};
};

struct SkyDirection {
	Vector3 direction;
	Vector3i endpoint;
	float weight = 0.0f;
	int path[SKY_PATH_LENGTH] = {};
};

template <typename T>
uint64_t vector_bytes(const std::vector<T> &p_values) {
	return uint64_t(p_values.capacity()) * sizeof(T);
}

uint64_t local_field_bytes(const lrt::LocalField &p_field) {
	return vector_bytes(p_field.material) + vector_bytes(p_field.matrices) + vector_bytes(p_field.gpu_matrices) +
			vector_bytes(p_field.links) + vector_bytes(p_field.receiver_links) +
			vector_bytes(p_field.local_visibility) + vector_bytes(p_field.diagnostic_sdf) +
			vector_bytes(p_field.diagnostic_albedo) + vector_bytes(p_field.diagnostic_emission) +
			vector_bytes(p_field.diagnostic_dirty) + vector_bytes(p_field.receivers) +
			vector_bytes(p_field.receiver_emission) + vector_bytes(p_field.receiver_capacities) +
			vector_bytes(p_field.receiver_staging_offsets) + vector_bytes(p_field.receiver_free_ranges) +
			vector_bytes(p_field.changed_occupancy);
}

void build_gpu_luminance_matrices(lrt::LocalField &r_field, int p_probe_count) {
	static constexpr double LUMA[3] = { 0.2126, 0.7152, 0.0722 };
	r_field.gpu_matrices.assign(size_t(p_probe_count) * 20, 0.0f);
	double squared_error = 0.0;
	double squared_reference = 0.0;
	double max_abs_error = 0.0;
	for (int probe = 0; probe < p_probe_count; probe++) {
		double luminance[16] = {};
		for (int channel = 0; channel < 3; channel++) {
			for (int row = 0; row < 4; row++) {
				const float *source = r_field.matrices.data() + (size_t(channel * 4 + row) * p_probe_count + probe) * 4;
				for (int column = 0; column < 4; column++) {
					luminance[row * 4 + column] += LUMA[channel] * double(source[column]);
				}
			}
		}
		double denominator = 0.0;
		for (double value : luminance) {
			denominator += value * value;
		}
		double tint[3] = {};
		if (denominator > 1e-20) {
			for (int channel = 0; channel < 3; channel++) {
				double numerator = 0.0;
				for (int row = 0; row < 4; row++) {
					const float *source = r_field.matrices.data() + (size_t(channel * 4 + row) * p_probe_count + probe) * 4;
					for (int column = 0; column < 4; column++) {
						numerator += double(source[column]) * luminance[row * 4 + column];
					}
				}
				tint[channel] = MAX(0.0, numerator / denominator);
			}
			const double encoded_luminance = LUMA[0] * tint[0] + LUMA[1] * tint[1] + LUMA[2] * tint[2];
			if (encoded_luminance > 1e-12) {
				for (double &value : tint) {
					value /= encoded_luminance;
				}
			}
		}
		for (int row = 0; row < 4; row++) {
			float *target = r_field.gpu_matrices.data() + (size_t(row) * p_probe_count + probe) * 4;
			for (int column = 0; column < 4; column++) {
				target[column] = float(luminance[row * 4 + column]);
			}
		}
		float *target_tint = r_field.gpu_matrices.data() + (size_t(4) * p_probe_count + probe) * 4;
		target_tint[0] = float(tint[0]);
		target_tint[1] = float(tint[1]);
		target_tint[2] = float(tint[2]);
		for (int channel = 0; channel < 3; channel++) {
			for (int row = 0; row < 4; row++) {
				const float *source = r_field.matrices.data() + (size_t(channel * 4 + row) * p_probe_count + probe) * 4;
				for (int column = 0; column < 4; column++) {
					const double reference = source[column];
					const double error = luminance[row * 4 + column] * tint[channel] - reference;
					squared_error += error * error;
					squared_reference += reference * reference;
					max_abs_error = MAX(max_abs_error, Math::abs(error));
				}
			}
		}
	}
	r_field.matrix_compression_relative_rmse = squared_reference > 0.0 ? Math::sqrt(squared_error / squared_reference) : 0.0;
	r_field.matrix_compression_max_abs = max_abs_error;
}

void coalesce_receiver_free_ranges(std::vector<lrt::LocalField::ReceiverFreeRange> &r_ranges) {
	r_ranges.erase(std::remove_if(r_ranges.begin(), r_ranges.end(), [](const lrt::LocalField::ReceiverFreeRange &p_range) {
		return p_range.count == 0;
	}), r_ranges.end());
	std::sort(r_ranges.begin(), r_ranges.end(), [](const lrt::LocalField::ReceiverFreeRange &p_left,
			const lrt::LocalField::ReceiverFreeRange &p_right) {
		return p_left.start < p_right.start;
	});
	std::vector<lrt::LocalField::ReceiverFreeRange> merged;
	merged.reserve(r_ranges.size());
	for (const lrt::LocalField::ReceiverFreeRange &range : r_ranges) {
		if (!merged.empty() && merged.back().start + merged.back().count >= range.start) {
			const uint32_t end = MAX(merged.back().start + merged.back().count, range.start + range.count);
			merged.back().count = end - merged.back().start;
		} else {
			merged.push_back(range);
		}
	}
	r_ranges = std::move(merged);
}

bool allocate_receiver_free_range(std::vector<lrt::LocalField::ReceiverFreeRange> &r_ranges,
		uint32_t p_capacity, uint32_t &r_start) {
	size_t best = SIZE_MAX;
	for (size_t index = 0; index < r_ranges.size(); index++) {
		if (r_ranges[index].count >= p_capacity &&
				(best == SIZE_MAX || r_ranges[index].count < r_ranges[best].count)) {
			best = index;
		}
	}
	if (best == SIZE_MAX) {
		return false;
	}
	r_start = r_ranges[best].start;
	r_ranges[best].start += p_capacity;
	r_ranges[best].count -= p_capacity;
	if (r_ranges[best].count == 0) {
		r_ranges.erase(r_ranges.begin() + best);
	}
	return true;
}

void update_receiver_fragmentation(lrt::LocalField &r_field, int p_probe_count) {
	uint64_t active = 0;
	for (int probe = 0; probe < p_probe_count; probe++) {
		active += uint64_t(r_field.material[size_t(probe) * 4 + 1]);
	}
	const uint64_t permitted = MAX(active + 4096, (active * 3 + 1) / 2);
	r_field.receiver_layout_compaction_pending = uint64_t(r_field.receiver_layout_capacity) > permitted;
}

void stabilize_receiver_pages(lrt::LocalField &r_field, const lrt::LocalField *p_previous, int p_probe_count) {
	if (r_field.material.size() != size_t(p_probe_count) * 4) {
		return;
	}
	const bool can_reuse = p_previous != nullptr && p_previous->receiver_layout_stable && !r_field.receiver_layout_compacted &&
			p_previous->material.size() == r_field.material.size() &&
			p_previous->receiver_capacities.size() == size_t(p_probe_count);
	if (!can_reuse) {
		r_field.receiver_capacities.resize(size_t(p_probe_count));
		for (int probe = 0; probe < p_probe_count; probe++) {
			r_field.receiver_capacities[size_t(probe)] = uint16_t(r_field.material[size_t(probe) * 4 + 1]);
		}
		r_field.receiver_layout_stable = true;
		r_field.receiver_layout_capacity = uint32_t(r_field.receivers.size() / 12);
		r_field.receiver_free_ranges.clear();
		update_receiver_fragmentation(r_field, p_probe_count);
		return;
	}
	if (r_field.receiver_delta && r_field.receiver_staging_offsets.size() == size_t(p_probe_count)) {
		r_field.receiver_capacities = p_previous->receiver_capacities;
		r_field.receiver_free_ranges = p_previous->receiver_free_ranges;
		uint32_t layout_capacity = p_previous->receiver_layout_capacity > 0 ?
				p_previous->receiver_layout_capacity : uint32_t(p_previous->receivers.size() / 12);
		for (int probe = 0; probe < p_probe_count; probe++) {
			const size_t material_index = size_t(probe) * 4;
			if (r_field.diagnostic_dirty[material_index] < 0.5f) {
				continue;
			}
			const uint32_t receiver_count = uint32_t(r_field.material[material_index + 1]);
			const uint32_t old_capacity = p_previous->receiver_capacities[size_t(probe)];
			if (receiver_count > old_capacity && old_capacity > 0) {
				r_field.receiver_free_ranges.push_back({ uint32_t(p_previous->material[material_index]) / 3, old_capacity });
			}
		}
		coalesce_receiver_free_ranges(r_field.receiver_free_ranges);
		for (int probe = 0; probe < p_probe_count; probe++) {
			const size_t material_index = size_t(probe) * 4;
			if (r_field.diagnostic_dirty[material_index] < 0.5f) {
				continue;
			}
			const uint32_t receiver_count = uint32_t(r_field.material[material_index + 1]);
			uint32_t capacity = p_previous->receiver_capacities[size_t(probe)];
			uint32_t target_receiver_start = uint32_t(p_previous->material[material_index]) / 3;
			if (receiver_count > capacity) {
				capacity = MIN(uint32_t(26), MAX(receiver_count, receiver_count + MAX(uint32_t(2), receiver_count / 4)));
				if (!allocate_receiver_free_range(r_field.receiver_free_ranges, capacity, target_receiver_start)) {
					target_receiver_start = layout_capacity;
					layout_capacity += capacity;
				}
			}
			r_field.material[material_index] = float(target_receiver_start * 3);
			r_field.receiver_capacities[size_t(probe)] = uint16_t(capacity);
		}
		r_field.receiver_layout_capacity = layout_capacity;
		r_field.receiver_layout_stable = true;
		update_receiver_fragmentation(r_field, p_probe_count);
		return;
	}

	std::vector<float> compact_receivers = std::move(r_field.receivers);
	std::vector<float> compact_emission = std::move(r_field.receiver_emission);
	r_field.receivers = p_previous->receivers;
	r_field.receiver_emission = p_previous->receiver_emission;
	r_field.receiver_capacities = p_previous->receiver_capacities;
	r_field.receiver_free_ranges = p_previous->receiver_free_ranges;
	for (int probe = 0; probe < p_probe_count; probe++) {
		const size_t material_index = size_t(probe) * 4;
		if (r_field.diagnostic_dirty[material_index] < 0.5f) {
			r_field.material[material_index] = p_previous->material[material_index];
			continue;
		}
		const uint32_t source_vector_start = uint32_t(r_field.material[material_index]);
		const uint32_t receiver_count = uint32_t(r_field.material[material_index + 1]);
		const uint32_t previous_vector_start = uint32_t(p_previous->material[material_index]);
		uint32_t capacity = p_previous->receiver_capacities[size_t(probe)];
		uint32_t target_receiver_start = previous_vector_start / 3;
		if (receiver_count > capacity) {
			target_receiver_start = uint32_t(r_field.receivers.size() / 12);
			capacity = MIN(uint32_t(26), MAX(receiver_count, receiver_count + MAX(uint32_t(2), receiver_count / 4)));
			r_field.receivers.resize(r_field.receivers.size() + size_t(capacity) * 12, 0.0f);
			r_field.receiver_emission.resize(r_field.receiver_emission.size() + size_t(capacity) * 4, 0.0f);
		}
		const size_t target_receiver_float = size_t(target_receiver_start) * 12;
		const size_t target_emission_float = size_t(target_receiver_start) * 4;
		std::fill_n(r_field.receivers.data() + target_receiver_float, size_t(capacity) * 12, 0.0f);
		std::fill_n(r_field.receiver_emission.data() + target_emission_float, size_t(capacity) * 4, 0.0f);
		if (receiver_count > 0) {
			const size_t source_receiver = size_t(source_vector_start / 3) * 12;
			const size_t source_emission = size_t(source_vector_start / 3) * 4;
			memcpy(r_field.receivers.data() + target_receiver_float,
					compact_receivers.data() + source_receiver, size_t(receiver_count) * 12 * sizeof(float));
			memcpy(r_field.receiver_emission.data() + target_emission_float,
					compact_emission.data() + source_emission, size_t(receiver_count) * 4 * sizeof(float));
		}
		r_field.material[material_index] = float(target_receiver_start * 3);
		r_field.receiver_capacities[size_t(probe)] = uint16_t(capacity);
	}
	r_field.receiver_layout_stable = true;
	r_field.receiver_layout_capacity = uint32_t(r_field.receivers.size() / 12);
	update_receiver_fragmentation(r_field, p_probe_count);
}

void materialize_receiver_delta(lrt::LocalField &r_field, lrt::LocalField &r_previous, int p_probe_count) {
	if (!r_field.receiver_delta || r_field.receiver_staging_offsets.size() != size_t(p_probe_count)) {
		return;
	}
	std::vector<float> compact_receivers = std::move(r_field.receivers);
	std::vector<float> compact_emission = std::move(r_field.receiver_emission);
	r_field.receivers = std::move(r_previous.receivers);
	r_field.receiver_emission = std::move(r_previous.receiver_emission);
	r_field.receivers.resize(size_t(r_field.receiver_layout_capacity) * 12, 0.0f);
	r_field.receiver_emission.resize(size_t(r_field.receiver_layout_capacity) * 4, 0.0f);
	for (int probe = 0; probe < p_probe_count; probe++) {
		const size_t material_index = size_t(probe) * 4;
		if (r_field.diagnostic_dirty[material_index] < 0.5f) {
			continue;
		}
		const uint32_t receiver_count = uint32_t(r_field.material[material_index + 1]);
		const uint32_t capacity = r_field.receiver_capacities[size_t(probe)];
		const uint32_t target_start = uint32_t(r_field.material[material_index]) / 3;
		const uint32_t source_start = r_field.receiver_staging_offsets[size_t(probe)];
		std::fill_n(r_field.receivers.data() + size_t(target_start) * 12, size_t(capacity) * 12, 0.0f);
		std::fill_n(r_field.receiver_emission.data() + size_t(target_start) * 4, size_t(capacity) * 4, 0.0f);
		if (receiver_count > 0) {
			ERR_CONTINUE(source_start == UINT32_MAX || (size_t(source_start) + receiver_count) * 12 > compact_receivers.size() ||
					(size_t(source_start) + receiver_count) * 4 > compact_emission.size());
			memcpy(r_field.receivers.data() + size_t(target_start) * 12,
					compact_receivers.data() + size_t(source_start) * 12, size_t(receiver_count) * 12 * sizeof(float));
			memcpy(r_field.receiver_emission.data() + size_t(target_start) * 4,
					compact_emission.data() + size_t(source_start) * 4, size_t(receiver_count) * 4 * sizeof(float));
		}
	}
	r_field.receiver_delta = false;
	r_field.receiver_staging_offsets.clear();
}

uint64_t local_cache_bytes(const lrt::LocalCache &p_cache) {
	return vector_bytes(p_cache.trunk_signatures) + vector_bytes(p_cache.samples) + vector_bytes(p_cache.sampled);
}

uint64_t material_capture_bytes(const lrt::MaterialCapture &p_capture) {
	return vector_bytes(p_capture.albedo) + vector_bytes(p_capture.emission) + vector_bytes(p_capture.occupied);
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
					direction.endpoint = endpoint;
					const double u0 = double(x * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					const double v0 = double(y * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					const double u1 = double((x + 1) * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					const double v1 = double((y + 1) * 2 - SKY_FACE_RESOLUTION) / SKY_FACE_RESOLUTION;
					direction.weight = float(cube_texel_solid_angle(u0, v0, u1, v1));

					Vector3i previous;
					for (int step = 1; step <= SKY_PATH_LENGTH; step++) {
						const Vector3i current(
								round_ratio(endpoint.x * step, SKY_PATH_LENGTH),
								round_ratio(endpoint.y * step, SKY_PATH_LENGTH),
								round_ratio(endpoint.z * step, SKY_PATH_LENGTH));
						const int link_index = link_direction_index(current - previous);
						DEV_ASSERT(link_index >= 0);
						direction.path[step - 1] = link_index;
						previous = current;
					}
				}
			}
		}
		for (int index = 0; index < SKY_DIRECTION_COUNT; index++) {
			for (int opposite_index = index + 1; opposite_index < SKY_DIRECTION_COUNT; opposite_index++) {
				if (result[opposite_index].endpoint != -result[index].endpoint) {
					continue;
				}
				for (int step = 0; step < SKY_PATH_LENGTH; step++) {
					const int forward_link = result[index].path[SKY_PATH_LENGTH - step - 1];
					const lrt::Direction &forward_direction = lrt::directions()[forward_link];
					const int reverse_link = link_direction_index(Vector3i(
							-forward_direction.offset[0],
							-forward_direction.offset[1],
							-forward_direction.offset[2]));
					DEV_ASSERT(reverse_link >= 0);
					result[opposite_index].path[step] = reverse_link;
				}
				break;
			}
		}
		return result;
	}();
	return directions;
}

Vector3 sample_panorama_direction(const Ref<Image> &p_panorama, const Vector3 &p_direction) {
	const int width = p_panorama->get_width();
	const int height = p_panorama->get_height();
	if (width <= 0 || height <= 0) {
		return Vector3();
	}
	const Vector3 direction = p_direction.normalized();
	double phi = Math::atan2(-direction.x, -direction.z);
	if (phi < 0.0) {
		phi += Math::TAU;
	}
	const double theta = Math::acos(CLAMP(direction.y, -1.0, 1.0));
	const double pixel_x = phi * width / Math::TAU - 0.5;
	const double pixel_y = CLAMP(theta * height / Math::PI - 0.5, 0.0, double(height - 1));
	const int raw_x0 = int(Math::floor(pixel_x));
	const int x0 = ((raw_x0 % width) + width) % width;
	const int x1 = (x0 + 1) % width;
	const int y0 = int(Math::floor(pixel_y));
	const int y1 = MIN(y0 + 1, height - 1);
	const float blend_x = float(pixel_x - Math::floor(pixel_x));
	const float blend_y = float(pixel_y - Math::floor(pixel_y));
	const Color top = p_panorama->get_pixel(x0, y0).lerp(p_panorama->get_pixel(x1, y0), blend_x);
	const Color bottom = p_panorama->get_pixel(x0, y1).lerp(p_panorama->get_pixel(x1, y1), blend_x);
	const Color result = top.lerp(bottom, blend_y);
	return Vector3(result.r, result.g, result.b);
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

String sky_weighted_basis_initializer() {
	String text;
	const std::array<SkyDirection, SKY_DIRECTION_COUNT> &directions = sky_directions();
	for (int index = 0; index < SKY_DIRECTION_COUNT; index++) {
		if (index > 0) {
			text += ", ";
		}
		const SkyDirection &direction = directions[index];
		text += vformat("vec4(%.9f, %.9f, %.9f, %.9f)",
				direction.weight * SH_C0,
				direction.weight * (SH_C1 / 3.0) * direction.direction.x,
				direction.weight * (SH_C1 / 3.0) * direction.direction.y,
				direction.weight * (SH_C1 / 3.0) * direction.direction.z);
	}
	return text;
}

String sky_path_initializer(int p_first_step) {
	String text;
	const std::array<SkyDirection, SKY_DIRECTION_COUNT> &directions = sky_directions();
	for (int index = 0; index < SKY_DIRECTION_COUNT; index++) {
		if (index > 0) {
			text += ", ";
		}
		const int *path = directions[index].path + p_first_step;
		text += vformat("ivec4(%d, %d, %d, %d)", path[0], path[1], path[2], path[3]);
	}
	return text;
}

Vector<uint8_t> bytes_of(const void *p_data, size_t p_size) {
	Vector<uint8_t> bytes;
	bytes.resize(int64_t(p_size));
	memcpy(bytes.ptrw(), p_data, p_size);
	return bytes;
}

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
	sky_samples.resize(SKY_DIRECTION_COUNT);
	sky_samples.fill(Vector3());
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
	ClassDB::bind_method(D_METHOD("set_receiver_lighting", "lighting"), &LRTVolume::set_receiver_lighting);
	ClassDB::bind_method(D_METHOD("get_receiver_lighting"), &LRTVolume::get_receiver_lighting);
	ClassDB::bind_method(D_METHOD("set_sky", "sky"), &LRTVolume::set_sky);
	ClassDB::bind_method(D_METHOD("set_sky_radiance", "radiance"), &LRTVolume::set_sky_radiance);
	ClassDB::bind_method(D_METHOD("get_sky_radiance"), &LRTVolume::get_sky_radiance);
	ClassDB::bind_method(D_METHOD("set_sky_samples", "samples"), &LRTVolume::set_sky_samples);
	ClassDB::bind_method(D_METHOD("get_sky_samples"), &LRTVolume::get_sky_samples);
	ClassDB::bind_method(D_METHOD("read_environment_panorama", "environment", "size"), &LRTVolume::read_environment_panorama);
	ClassDB::bind_method(D_METHOD("read_environment_radiance", "environment", "size"), &LRTVolume::read_environment_radiance);
	ClassDB::bind_method(D_METHOD("read_environment_radiance_sh", "environment", "size", "sky_to_local"), &LRTVolume::read_environment_radiance_sh);
	ClassDB::bind_method(D_METHOD("project_panorama_radiance_sh", "panorama", "sky_to_local"), &LRTVolume::project_panorama_radiance_sh);
	ClassDB::bind_method(D_METHOD("sample_panorama_radiance", "panorama", "local_to_sky"), &LRTVolume::sample_panorama_radiance);
	ClassDB::bind_method(D_METHOD("set_multi_bounce", "enabled"), &LRTVolume::set_multi_bounce);
	ClassDB::bind_method(D_METHOD("set_sh_visibility", "enabled"), &LRTVolume::set_sh_visibility);
	ClassDB::bind_method(D_METHOD("set_propagation_sampling", "sampling"), &LRTVolume::set_propagation_sampling);
	ClassDB::bind_method(D_METHOD("get_propagation_sampling"), &LRTVolume::get_propagation_sampling);
	ClassDB::bind_method(D_METHOD("build_local_field", "backend"), &LRTVolume::build_local_field);
	ClassDB::bind_method(D_METHOD("bake_local_field", "backend"), &LRTVolume::bake_local_field);
	ClassDB::bind_method(D_METHOD("apply_local_field", "preserve_history"), &LRTVolume::apply_local_field, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("request_cancel"), &LRTVolume::request_cancel);
	ClassDB::bind_method(D_METHOD("clear_cancel"), &LRTVolume::clear_cancel);
	ClassDB::bind_method(D_METHOD("is_cancel_requested"), &LRTVolume::is_cancel_requested);
	ClassDB::bind_method(D_METHOD("has_local_field"), &LRTVolume::has_local_field);
	ClassDB::bind_method(D_METHOD("inject"), &LRTVolume::inject);
	ClassDB::bind_method(D_METHOD("is_injection_pending"), &LRTVolume::is_injection_pending);
	ClassDB::bind_method(D_METHOD("step", "iterations"), &LRTVolume::step);
	ClassDB::bind_method(D_METHOD("get_pending_step_iterations"), &LRTVolume::get_pending_step_iterations);
	ClassDB::bind_method(D_METHOD("measure_step_gpu_completion_ms", "iterations"), &LRTVolume::measure_step_gpu_completion_ms);
	ClassDB::bind_method(D_METHOD("reset"), &LRTVolume::reset);
	ClassDB::bind_method(D_METHOD("get_iteration"), &LRTVolume::get_iteration);
	ClassDB::bind_method(D_METHOD("get_grid"), &LRTVolume::get_grid);
	ClassDB::bind_method(D_METHOD("refresh_display"), &LRTVolume::refresh_display);
	ClassDB::bind_method(D_METHOD("get_texture", "name"), &LRTVolume::get_texture);
	ClassDB::bind_method(D_METHOD("read_field", "name"), &LRTVolume::read_field);
	ClassDB::bind_method(D_METHOD("read_links"), &LRTVolume::read_links);
	ClassDB::bind_method(D_METHOD("read_receiver_links"), &LRTVolume::read_receiver_links);
	ClassDB::bind_method(D_METHOD("get_receiver_capture_data"), &LRTVolume::get_receiver_capture_data);
	ClassDB::bind_method(D_METHOD("sample_geometry", "point"), &LRTVolume::sample_geometry);
	ClassDB::bind_method(D_METHOD("get_stats"), &LRTVolume::get_stats);
	ClassDB::bind_method(D_METHOD("get_performance_stats"), &LRTVolume::get_performance_stats);
	ClassDB::bind_method(D_METHOD("get_memory_stats"), &LRTVolume::get_memory_stats);
	ClassDB::bind_method(D_METHOD("refresh_performance_stats"), &LRTVolume::refresh_performance_stats);
	ClassDB::bind_method(D_METHOD("set_render_frame_profiling_enabled", "enabled"), &LRTVolume::set_render_frame_profiling_enabled);
	ClassDB::bind_method(D_METHOD("get_render_frame_profile"), &LRTVolume::get_render_frame_profile);
	ClassDB::bind_method(D_METHOD("get_preparation_status"), &LRTVolume::get_preparation_status);
	ClassDB::bind_static_method("LRTVolume", D_METHOD("clear_shared_sdf_cache"), &LRTVolume::clear_shared_sdf_cache);
	ClassDB::bind_static_method("LRTVolume", D_METHOD("clear_shared_primitive_ltm_cache"), &LRTVolume::clear_shared_primitive_ltm_cache);
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

void LRTVolume::set_mesh_instances(const std::vector<MeshInstance> &p_meshes) {
	mesh_instances = p_meshes;
	has_staged = false;
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

void LRTVolume::set_receiver_lighting(const PackedVector3Array &p_lighting) {
	const size_t receiver_count = local.receivers.size() / 12;
	ERR_FAIL_COND_MSG(size_t(p_lighting.size()) != receiver_count,
			vformat("LRT receiver lighting count mismatch: expected %d, got %d.", receiver_count, p_lighting.size()));
	MutexLock lock(params_mutex);
	receiver_lighting.resize(receiver_count * 4);
	for (size_t i = 0; i < receiver_count; i++) {
		const Vector3 value = p_lighting[int64_t(i)];
		receiver_lighting[i * 4 + 0] = value.x;
		receiver_lighting[i * 4 + 1] = value.y;
		receiver_lighting[i * 4 + 2] = value.z;
		receiver_lighting[i * 4 + 3] = 0.0f;
	}
	has_receiver_lighting = true;
	native_light_fields_enabled = false;
}

PackedVector3Array LRTVolume::get_receiver_lighting() {
	if (native_light_fields_enabled && has_local && receiver_lighting_buffer.is_valid()) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL_V(rendering_server, PackedVector3Array());
		rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_read_receiver_lighting_render_thread));
		rendering_server->sync();
	}
	PackedVector3Array result;
	const int receiver_count = int(receiver_lighting.size() / 4);
	result.resize(receiver_count);
	for (int i = 0; i < receiver_count; i++) {
		result.set(i, Vector3(receiver_lighting[i * 4 + 0], receiver_lighting[i * 4 + 1], receiver_lighting[i * 4 + 2]));
	}
	return result;
}

void LRTVolume::reset_native_lights(int p_count) {
	ERR_FAIL_COND(p_count < 0);
	const bool grow_buffers = p_count > native_light_capacity;
	{
		MutexLock lock(params_mutex);
		if (grow_buffers) {
			native_light_capacity = MAX(INITIAL_NATIVE_LIGHT_CAPACITY, p_count);
		}
		native_light_fields_enabled = true;
		native_light_count = p_count;
		has_receiver_lighting = false;
		receiver_lighting.clear();
		native_light_states.assign(size_t(native_light_capacity), NativeLightState());
		for (int i = 0; i < p_count; i++) {
			native_light_states[size_t(i)].enabled = true;
		}
	}
	if (has_local) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL(rendering_server);
		if (grow_buffers) {
			rendering_server->call_on_render_thread(
					callable_mp(this, &LRTVolume::_resize_native_light_buffers_render_thread).bind(native_light_capacity));
		}
		rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_reset_native_light_buffers_render_thread));
	}
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

int LRTVolume::begin_native_light_capture(int p_slot) {
	ERR_FAIL_INDEX_V(p_slot, native_light_count, 0);
	int target_buffer = 0;
	{
		MutexLock lock(params_mutex);
		NativeLightState &state = native_light_states[p_slot];
		if (state.blend_frames > 0) {
			state.current_buffer = state.target_buffer;
			state.blend = 0.0f;
			state.blend_frames = 0;
		}
		state.target_buffer = 1 - state.current_buffer;
		target_buffer = state.target_buffer;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, target_buffer);
	rendering_server->call_on_render_thread(
			callable_mp(this, &LRTVolume::_begin_native_light_capture_render_thread).bind(p_slot, target_buffer));
	return target_buffer;
}

void LRTVolume::resolve_native_light_capture(const NativeLightResolve &p_resolve) {
	bool schedule = false;
	{
		MutexLock lock(native_resolve_mutex);
		pending_native_resolves.push_back(p_resolve);
		if (!native_resolve_pending.exchange(true)) {
			schedule = true;
		}
	}
	if (schedule) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL(rendering_server);
		rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_resolve_native_lights_render_thread));
	}
}

void LRTVolume::commit_native_light_capture(int p_slot, int p_blend_frames) {
	ERR_FAIL_INDEX(p_slot, native_light_count);
	MutexLock lock(params_mutex);
	NativeLightState &state = native_light_states[p_slot];
	state.blend = 0.0f;
	state.blend_frames = MAX(1, p_blend_frames);
}

bool LRTVolume::advance_native_light_blends() {
	MutexLock lock(params_mutex);
	bool changed = false;
	for (int i = 0; i < native_light_count; i++) {
		NativeLightState &state = native_light_states[i];
		if (state.blend_frames <= 0) {
			continue;
		}
		state.blend += 1.0f / float(state.blend_frames);
		changed = true;
		if (state.blend >= 1.0f) {
			state.current_buffer = state.target_buffer;
			state.blend = 0.0f;
			state.blend_frames = 0;
		}
	}
	return changed;
}

bool LRTVolume::has_native_light_blends() const {
	for (int i = 0; i < native_light_count; i++) {
		if (native_light_states[i].blend_frames > 0) {
			return true;
		}
	}
	return false;
}

bool LRTVolume::is_native_light_resolve_pending() const {
	return native_resolve_pending.load();
}

void LRTVolume::set_sky(const Vector3 &p_sky) {
	sky_radiance[0] = Vector4(p_sky.x / SH_C0, 0.0, 0.0, 0.0);
	sky_radiance[1] = Vector4(p_sky.y / SH_C0, 0.0, 0.0, 0.0);
	sky_radiance[2] = Vector4(p_sky.z / SH_C0, 0.0, 0.0, 0.0);
	PackedVector3Array samples;
	samples.resize(SKY_DIRECTION_COUNT);
	samples.fill(p_sky);
	set_sky_samples(samples);
}

void LRTVolume::set_sky_radiance(const PackedVector4Array &p_radiance) {
	ERR_FAIL_COND_MSG(p_radiance.size() != 3, "LRT sky radiance requires exactly three RGB SH2 coefficient vectors.");
	for (int channel = 0; channel < 3; channel++) {
		sky_radiance[channel] = p_radiance[channel];
	}
	PackedVector3Array samples;
	samples.resize(SKY_DIRECTION_COUNT);
	const std::array<SkyDirection, SKY_DIRECTION_COUNT> &directions = sky_directions();
	for (int direction_index = 0; direction_index < SKY_DIRECTION_COUNT; direction_index++) {
		const Vector3 direction = directions[direction_index].direction;
		const Vector4 basis(SH_C0, SH_C1 * direction.x, SH_C1 * direction.y, SH_C1 * direction.z);
		samples.set(direction_index, Vector3(
				MAX(0.0, sky_radiance[0].dot(basis)),
				MAX(0.0, sky_radiance[1].dot(basis)),
				MAX(0.0, sky_radiance[2].dot(basis))));
	}
	set_sky_samples(samples);
}

PackedVector4Array LRTVolume::get_sky_radiance() const {
	PackedVector4Array result;
	result.resize(3);
	for (int channel = 0; channel < 3; channel++) {
		result.set(channel, sky_radiance[channel]);
	}
	return result;
}

void LRTVolume::set_sky_samples(const PackedVector3Array &p_samples) {
	ERR_FAIL_COND_MSG(p_samples.size() != SKY_DIRECTION_COUNT, vformat("LRT sky input requires one RGB sample for each of the %d sky directions.", SKY_DIRECTION_COUNT));
	MutexLock lock(params_mutex);
	bool changed = sky_samples.size() != p_samples.size();
	for (int index = 0; !changed && index < p_samples.size(); index++) {
		changed = sky_samples[index] != p_samples[index];
	}
	if (!changed) {
		return;
	}
	sky_samples = p_samples;
	sky_projection_dirty.store(true);
}

PackedVector3Array LRTVolume::get_sky_samples() const {
	return sky_samples;
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

PackedVector3Array LRTVolume::sample_panorama_radiance(const Ref<Image> &p_panorama, const Basis &p_local_to_sky) const {
	PackedVector3Array result;
	result.resize(SKY_DIRECTION_COUNT);
	result.fill(Vector3());
	if (p_panorama.is_null() || p_panorama->is_empty()) {
		return result;
	}
	const std::array<SkyDirection, SKY_DIRECTION_COUNT> &directions = sky_directions();
	for (int direction_index = 0; direction_index < SKY_DIRECTION_COUNT; direction_index++) {
		const Vector3 local_direction = directions[direction_index].direction;
		result.set(direction_index, sample_panorama_direction(p_panorama, p_local_to_sky.xform(local_direction)));
	}
	return result;
}

void LRTVolume::set_multi_bounce(bool p_enabled) {
	MutexLock lock(params_mutex);
	multi_bounce = p_enabled;
}

void LRTVolume::set_sh_visibility(bool p_enabled) {
	MutexLock lock(params_mutex);
	sh_visibility = p_enabled;
}

void LRTVolume::set_propagation_sampling(int p_sampling) {
	propagation_sampling = CLAMP(p_sampling, 0, 1);
}

int LRTVolume::get_propagation_sampling() const {
	return propagation_sampling;
}

bool LRTVolume::set_local_debug_textures_enabled(bool p_enabled) {
	if (local_debug_textures_enabled.load() == p_enabled) {
		return false;
	}
	local_debug_textures_enabled.store(p_enabled);
	if (!has_local || apply_pending || !device) {
		local_debug_textures_dirty = p_enabled;
		return true;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, true);
	rendering_server->call_on_render_thread(
			callable_mp(this, &LRTVolume::_set_local_debug_textures_enabled_render_thread).bind(p_enabled));
	return true;
}

Error LRTVolume::_ensure_device() {
	if (device) {
		return OK;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rendering_server, ERR_UNAVAILABLE);
	device = rendering_server->get_rendering_device();
	ERR_FAIL_NULL_V(device, ERR_UNAVAILABLE);
	static const char *pass_names[GPU_TIMING_PASS_COUNT] = { "Inject", "Light Resolve", "Propagate", "Display" };
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
		shader_light_resolve = lrt_shared_shaders.shaders[1];
		shader_propagate = lrt_shared_shaders.shaders[2];
		shader_sky_project = lrt_shared_shaders.shaders[3];
		shader_display = lrt_shared_shaders.shaders[4];
		shader_local_patch = lrt_shared_shaders.shaders[5];
		pipeline_inject = lrt_shared_shaders.pipelines[0];
		pipeline_light_resolve = lrt_shared_shaders.pipelines[1];
		pipeline_propagate = lrt_shared_shaders.pipelines[2];
		pipeline_sky_project = lrt_shared_shaders.pipelines[3];
		pipeline_display = lrt_shared_shaders.pipelines[4];
		pipeline_local_patch = lrt_shared_shaders.pipelines[5];
		return OK;
	}
	lrt_shared_shaders.device = device;
	const String offsets = direction_initializer();
	const String sky_weighted_basis_text = sky_weighted_basis_initializer();
	const String sky_path_a = sky_path_initializer(0);
	const String sky_path_b = sky_path_initializer(4);
	const String sky_direction_count = itos(SKY_DIRECTION_COUNT);
	const String sky_direction_words = itos(SKY_DIRECTION_WORDS);
	const String sources[6] = {
		String(lrt_inject_shader_glsl).replace("%LRT_DIRECTIONS%", offsets),
		String(lrt_light_resolve_shader_glsl),
		String(lrt_propagate_shader_glsl)
				.replace("%LRT_DIRECTIONS%", offsets)
				.replace("%LRT_SKY_PATH_A%", sky_path_a)
				.replace("%LRT_SKY_PATH_B%", sky_path_b)
				.replace("%LRT_SKY_DIRECTION_COUNT%", sky_direction_count)
				.replace("%LRT_SKY_DIRECTION_WORDS%", sky_direction_words),
		String(lrt_sky_project_shader_glsl)
				.replace("%LRT_SKY_WEIGHTED_BASIS%", sky_weighted_basis_text)
				.replace("%LRT_SKY_DIRECTION_COUNT%", sky_direction_count)
				.replace("%LRT_SKY_DIRECTION_WORDS%", sky_direction_words),
		String(lrt_display_shader_glsl)
				.replace("%LRT_SKY_DIRECTION_COUNT%", sky_direction_count),
		String(lrt_local_patch_shader_glsl),
	};
	const char *shader_names[6] = {
		"LRT injection shader",
		"LRT native light resolve shader",
		"LRT propagation shader",
		"LRT sky projection shader",
		"LRT display shader",
		"LRT local patch shader",
	};
	RID shaders[6] = { shader_inject, shader_light_resolve, shader_propagate, shader_sky_project, shader_display, shader_local_patch };
	for (int i = 0; i < 6; i++) {
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
	shader_light_resolve = shaders[1];
	shader_propagate = shaders[2];
	shader_sky_project = shaders[3];
	shader_display = shaders[4];
	shader_local_patch = shaders[5];
	pipeline_inject = device->compute_pipeline_create(shader_inject);
	pipeline_light_resolve = device->compute_pipeline_create(shader_light_resolve);
	pipeline_propagate = device->compute_pipeline_create(shader_propagate);
	pipeline_sky_project = device->compute_pipeline_create(shader_sky_project);
	pipeline_display = device->compute_pipeline_create(shader_display);
	pipeline_local_patch = device->compute_pipeline_create(shader_local_patch);
	ERR_FAIL_COND_V(pipeline_inject.is_null() || pipeline_light_resolve.is_null() || pipeline_propagate.is_null() ||
			pipeline_sky_project.is_null() || pipeline_display.is_null() || pipeline_local_patch.is_null(), ERR_CANT_CREATE);
	lrt_shared_shaders.pipelines = { pipeline_inject, pipeline_light_resolve, pipeline_propagate,
		pipeline_sky_project, pipeline_display, pipeline_local_patch };
	return OK;
}

Error LRTVolume::_create_buffers() {
	ERR_FAIL_COND_V(_create_grid_buffers() != OK, ERR_CANT_CREATE);
	return _create_content_buffers();
}

RID LRTVolume::_create_display_texture(int p_width, int p_height, const std::vector<float> *p_values, Ref<LRTDisplayTexture> &r_texture) {
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

RID LRTVolume::_create_links_texture(const std::vector<uint32_t> &p_links, Ref<LRTDisplayTexture> &r_texture) {
	std::vector<float> packed(p_links.size() * 4, 0.0f);
	for (size_t i = 0; i < p_links.size(); i++) {
		packed[i * 4] = float(p_links[i] & 0x1FFFu);
		packed[i * 4 + 1] = float((p_links[i] >> 13) & 0x1FFFu);
	}
	return _create_display_texture(grid.width, grid.height, &packed, r_texture);
}

Error LRTVolume::_create_display_textures() {
	for (int channel = 0; channel < 3; channel++) {
		field_texture_rids[channel] = _create_display_texture(grid.width, grid.height, nullptr, field_textures[channel]);
		sky_texture_rids[channel] = _create_display_texture(grid.width, grid.height, nullptr, sky_textures[channel]);
	}
	material_texture_rid = _create_display_texture(grid.width, grid.height, &local.material, material_texture);
	links_texture_rid = _create_links_texture(local.links, links_texture);
	receiver_links_texture_rid = _create_links_texture(local.receiver_links, receiver_links_texture);
	ERR_FAIL_COND_V(_create_debug_textures(local_debug_textures_enabled.load()) != OK, ERR_CANT_CREATE);
	for (int channel = 0; channel < 3; channel++) {
		ERR_FAIL_COND_V(field_texture_rids[channel].is_null() || sky_texture_rids[channel].is_null(), ERR_CANT_CREATE);
	}
	ERR_FAIL_COND_V(material_texture_rid.is_null() || links_texture_rid.is_null() || receiver_links_texture_rid.is_null(), ERR_CANT_CREATE);
	return OK;
}

Error LRTVolume::_create_debug_textures(bool p_full_size) {
	const int width = p_full_size ? grid.width : 1;
	const int height = p_full_size ? grid.height : 1;
	for (int channel = 0; channel < 3; channel++) {
		source_texture_rids[channel] = _create_display_texture(width, height, nullptr, source_textures[channel]);
	}
	visibility_texture_rid = _create_display_texture(width, height, nullptr, visibility_texture);
	local_visibility_texture_rid = _create_display_texture(width, height,
			p_full_size ? &local.local_visibility : nullptr, local_visibility_texture);
	matrix_texture_rid = _create_display_texture(width, p_full_size ? grid.height * 12 : 1,
			p_full_size ? &local.matrices : nullptr, matrix_texture);
	diagnostic_sdf_texture_rid = _create_display_texture(width, height,
			p_full_size ? &local.diagnostic_sdf : nullptr, diagnostic_sdf_texture);
	diagnostic_albedo_texture_rid = _create_display_texture(width, height,
			p_full_size ? &local.diagnostic_albedo : nullptr, diagnostic_albedo_texture);
	diagnostic_emission_texture_rid = _create_display_texture(width, height,
			p_full_size ? &local.diagnostic_emission : nullptr, diagnostic_emission_texture);
	diagnostic_dirty_texture_rid = _create_display_texture(width, height,
			p_full_size ? &local.diagnostic_dirty : nullptr, diagnostic_dirty_texture);
	debug_textures_full_size.store(p_full_size);
	for (int channel = 0; channel < 3; channel++) {
		ERR_FAIL_COND_V(source_texture_rids[channel].is_null(), ERR_CANT_CREATE);
	}
	ERR_FAIL_COND_V(visibility_texture_rid.is_null() || local_visibility_texture_rid.is_null() ||
					matrix_texture_rid.is_null() || diagnostic_sdf_texture_rid.is_null() ||
					diagnostic_albedo_texture_rid.is_null() || diagnostic_emission_texture_rid.is_null() ||
					diagnostic_dirty_texture_rid.is_null(), ERR_CANT_CREATE);
	return OK;
}

void LRTVolume::_free_debug_textures() {
	for (int channel = 0; channel < 3; channel++) {
		source_textures[channel].unref();
		source_texture_rids[channel] = RID();
	}
	visibility_texture.unref();
	matrix_texture.unref();
	local_visibility_texture.unref();
	diagnostic_sdf_texture.unref();
	diagnostic_albedo_texture.unref();
	diagnostic_emission_texture.unref();
	diagnostic_dirty_texture.unref();
	visibility_texture_rid = RID();
	matrix_texture_rid = RID();
	local_visibility_texture_rid = RID();
	diagnostic_sdf_texture_rid = RID();
	diagnostic_albedo_texture_rid = RID();
	diagnostic_emission_texture_rid = RID();
	diagnostic_dirty_texture_rid = RID();
	debug_textures_full_size.store(false);
}

void LRTVolume::_set_local_debug_textures_enabled_render_thread(bool p_enabled) {
	if (!device || !has_local || apply_pending || debug_textures_full_size.load() == p_enabled) {
		return;
	}
	_free_display_uniform_sets();
	_free_debug_textures();
	if (_create_debug_textures(p_enabled) != OK || _create_display_uniform_sets() != OK) {
		return;
	}
	if (p_enabled) {
		_upload_local_textures();
		display_source_dirty = true;
		_sync_display();
	} else {
		local_debug_textures_dirty = true;
	}
}

// Sized by the probe grid alone: these survive a geometry edit, which is what keeps the
// propagated field (radiance/visibility) alive across edits on the same grid.
Error LRTVolume::_create_grid_buffers() {
	local_grid_banks_synchronized = false;
	display_source_dirty = true;
	display_sky_dirty = true;
	const int count = grid.count;
	params_buffer = device->uniform_buffer_create(sizeof(ParamsData));
	material_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	links_buffer = device->storage_buffer_create(count * sizeof(uint32_t));
	matrix_buffer = device->storage_buffer_create(count * 20 * sizeof(float));
	local_visibility_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	staged_material_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	staged_links_buffer = device->storage_buffer_create(count * sizeof(uint32_t));
	staged_matrix_buffer = device->storage_buffer_create(count * 20 * sizeof(float));
	staged_local_visibility_buffer = device->storage_buffer_create(count * 4 * sizeof(float));
	local_patch_buffer = device->storage_buffer_create(MAX_LOCAL_PATCH_PROBES * sizeof(LocalPatchData));
	receiver_patch_buffer = device->storage_buffer_create(MAX_RECEIVER_PATCHES * sizeof(ReceiverPatchData));
	for (int i = 0; i < 3; i++) {
		source_buffers[i] = device->storage_buffer_create(count * 4 * sizeof(float));
		external_gi_buffers[i] = device->storage_buffer_create(count * 4 * sizeof(float));
		if (external_gi_buffers[i].is_valid()) {
			device->buffer_clear(external_gi_buffers[i], 0, count * 4 * sizeof(float));
		}
		for (int phase = 0; phase < 3; phase++) {
			radiance_phase_buffers[phase][i] = device->storage_buffer_create(count * 4 * sizeof(float));
			if (radiance_phase_buffers[phase][i].is_valid()) {
				device->buffer_clear(radiance_phase_buffers[phase][i], 0, count * 4 * sizeof(float));
			}
		}
	}
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			radiance_buffers[buffer][channel] = device->storage_buffer_create(count * 4 * sizeof(float));
		}
		directional_visibility_buffers[buffer] = device->storage_buffer_create(count * SKY_DIRECTION_WORDS * sizeof(uint32_t));
		visibility_buffers[buffer] = device->storage_buffer_create(count * 4 * sizeof(float));
	}
	for (int channel = 0; channel < 3; channel++) {
		sky_buffers[channel] = device->storage_buffer_create(count * 4 * sizeof(float));
	}
	ERR_FAIL_COND_V(params_buffer.is_null() || material_buffer.is_null() || links_buffer.is_null() ||
					matrix_buffer.is_null() || local_visibility_buffer.is_null() || staged_material_buffer.is_null() ||
					staged_links_buffer.is_null() || staged_matrix_buffer.is_null() || staged_local_visibility_buffer.is_null() ||
					local_patch_buffer.is_null() || receiver_patch_buffer.is_null() ||
					directional_visibility_buffers[0].is_null() || directional_visibility_buffers[1].is_null(),
			ERR_CANT_CREATE);
	return _create_display_textures();
}

// Receiver content uses modest spare capacity so ordinary local edits retain their buffers and
// descriptor sets. A larger field grows the whole group together because every light slot shares
// the receiver stride.
Error LRTVolume::_create_content_buffers() {
	const size_t receiver_count = local.receivers.size() / 12;
	receiver_capacity = receiver_count + MAX(size_t(1024), receiver_count / 64);
	const size_t receiver_bytes = MAX(size_t(16), receiver_capacity * 12 * sizeof(float));
	receiver_buffer = device->storage_buffer_create(uint32_t(receiver_bytes));
	staged_receiver_buffer = device->storage_buffer_create(uint32_t(receiver_bytes));
	const size_t emission_bytes = MAX(size_t(16), receiver_capacity * 4 * sizeof(float));
	receiver_emission_buffer = device->storage_buffer_create(uint32_t(emission_bytes));
	staged_receiver_emission_buffer = device->storage_buffer_create(uint32_t(emission_bytes));
	const size_t lighting_bytes = MAX(size_t(16), receiver_capacity * 4 * sizeof(float));
	receiver_lighting_buffer = device->storage_buffer_create(uint32_t(lighting_bytes));
	native_light_capacity = MAX(native_light_capacity, MAX(INITIAL_NATIVE_LIGHT_CAPACITY, native_light_count));
	native_light_states.resize(size_t(native_light_capacity));
	const size_t native_light_bytes = MAX(size_t(16), receiver_capacity * size_t(native_light_capacity) * 4 * sizeof(float));
	for (int buffer = 0; buffer < 2; buffer++) {
		native_light_unit_buffers[buffer] = device->storage_buffer_create(uint32_t(native_light_bytes));
		if (native_light_unit_buffers[buffer].is_valid()) {
			device->buffer_clear(native_light_unit_buffers[buffer], 0, native_light_bytes);
		}
	}
	native_light_state_buffer = device->storage_buffer_create(sizeof(NativeLightStateData) * native_light_capacity);
	native_light_sampler = device->sampler_create(RD::SamplerState());
	ERR_FAIL_COND_V(receiver_buffer.is_null() || staged_receiver_buffer.is_null() ||
			staged_receiver_emission_buffer.is_null() || receiver_emission_buffer.is_null() || receiver_lighting_buffer.is_null() ||
			native_light_unit_buffers[0].is_null() || native_light_unit_buffers[1].is_null() ||
			native_light_state_buffer.is_null() || native_light_sampler.is_null(),
			ERR_CANT_CREATE);
	return OK;
}

Error LRTVolume::_create_uniform_sets() {
	auto make_uniform = [](RD::UniformType p_type, uint32_t p_binding, RID p_id) {
		RD::Uniform uniform;
		uniform.uniform_type = p_type;
		uniform.binding = p_binding;
		uniform.append_id(p_id);
		return uniform;
	};
	for (int bank = 0; bank < 2; bank++) {
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1,
				bank == 0 ? material_buffer : staged_material_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5,
				bank == 0 ? receiver_buffer : staged_receiver_buffer));
		for (int i = 0; i < 3; i++) {
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, source_buffers[i]));
		}
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20,
				bank == 0 ? receiver_emission_buffer : staged_receiver_emission_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, receiver_lighting_buffer));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, native_light_unit_buffers[0]));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, native_light_unit_buffers[1]));
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, native_light_state_buffer));
		RID &set = bank == 0 ? uniform_set_inject : staged_uniform_set_inject;
		set = device->uniform_set_create(uniforms, shader_inject, 0);
		ERR_FAIL_COND_V(set.is_null(), ERR_CANT_CREATE);

		Vector<RD::Uniform> patch_uniforms;
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, local_patch_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1,
				bank == 0 ? material_buffer : staged_material_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2,
				bank == 0 ? links_buffer : staged_links_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3,
				bank == 0 ? matrix_buffer : staged_matrix_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4,
				bank == 0 ? local_visibility_buffer : staged_local_visibility_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, receiver_patch_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6,
				bank == 0 ? receiver_buffer : staged_receiver_buffer));
		patch_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7,
				bank == 0 ? receiver_emission_buffer : staged_receiver_emission_buffer));
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
				bank == 0 ? links_buffer : staged_links_buffer));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3,
				bank == 0 ? matrix_buffer : staged_matrix_buffer));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4,
				bank == 0 ? local_visibility_buffer : staged_local_visibility_buffer));
			for (int i = 0; i < 3; i++) {
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + i, source_buffers[i]));
			}
			for (int i = 0; i < 3; i++) {
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9 + i, radiance_buffers[buffer][i]));
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13 + i, radiance_buffers[1 - buffer][i]));
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22 + i, external_gi_buffers[i]));
				uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25 + i, sky_buffers[i]));
			}
			for (int phase = 0; phase < 3; phase++) {
				for (int channel = 0; channel < 3; channel++) {
					uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER,
							28 + phase * 3 + channel, radiance_phase_buffers[phase][channel]));
				}
			}
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, visibility_buffers[buffer]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, visibility_buffers[1 - buffer]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, directional_visibility_buffers[buffer]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, directional_visibility_buffers[1 - buffer]));
			RID &propagate_set = bank == 0 ? uniform_set_propagate[buffer] : staged_uniform_set_propagate[buffer];
			propagate_set = device->uniform_set_create(uniforms, shader_propagate, 0);
			ERR_FAIL_COND_V(propagate_set.is_null(), ERR_CANT_CREATE);

			if (bank != 0) {
				continue;
			}
			Vector<RD::Uniform> sky_uniforms;
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, directional_visibility_buffers[buffer]));
			sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, directional_visibility_buffers[1 - buffer]));
			for (int i = 0; i < 3; i++) {
				sky_uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19 + i, sky_buffers[i]));
			}
			uniform_set_sky_project[buffer] = device->uniform_set_create(sky_uniforms, shader_sky_project, 0);
			ERR_FAIL_COND_V(uniform_set_sky_project[buffer].is_null(), ERR_CANT_CREATE);

		}
	}
	return _create_display_uniform_sets();
}

Error LRTVolume::_create_display_uniform_sets() {
	auto make_uniform = [](RD::UniformType p_type, uint32_t p_binding, RID p_id) {
		RD::Uniform uniform;
		uniform.uniform_type = p_type;
		uniform.binding = p_binding;
		uniform.append_id(p_id);
		return uniform;
	};
	for (int buffer = 0; buffer < 2; buffer++) {
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, params_buffer));
		for (int channel = 0; channel < 3; channel++) {
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6 + channel, source_buffers[channel]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9 + channel, radiance_buffers[buffer][channel]));
		}
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, visibility_buffers[buffer]));
		for (int channel = 0; channel < 3; channel++) {
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17 + channel, sky_buffers[channel]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_IMAGE, 20 + channel, field_texture_rids[channel]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_IMAGE, 24 + channel, source_texture_rids[channel]));
			uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_IMAGE, 27 + channel, sky_texture_rids[channel]));
		}
		uniforms.push_back(make_uniform(RD::UNIFORM_TYPE_IMAGE, 23, visibility_texture_rid));
		uniform_set_display[buffer] = device->uniform_set_create(uniforms, shader_display, 0);
		ERR_FAIL_COND_V(uniform_set_display[buffer].is_null(), ERR_CANT_CREATE);
	}
	return OK;
}

void LRTVolume::_free_display_uniform_sets() {
	if (!device) {
		return;
	}
	for (RID &set : uniform_set_display) {
		if (set.is_valid()) {
			device->free_rid(set);
			set = RID();
		}
	}
}

void LRTVolume::_free_uniform_sets() {
	if (!device) {
		return;
	}
	RID sets[12] = { uniform_set_inject, staged_uniform_set_inject,
		uniform_set_propagate[0], uniform_set_propagate[1],
		staged_uniform_set_propagate[0], staged_uniform_set_propagate[1],
		uniform_set_sky_project[0], uniform_set_sky_project[1],
		uniform_set_display[0], uniform_set_display[1],
		uniform_set_local_patch, staged_uniform_set_local_patch };
	for (RID &set : sets) {
		if (set.is_valid()) {
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
	uniform_set_sky_project[0] = RID();
	uniform_set_sky_project[1] = RID();
	uniform_set_display[0] = RID();
	uniform_set_display[1] = RID();
	uniform_set_local_patch = RID();
	staged_uniform_set_local_patch = RID();
}

void LRTVolume::_free_content_buffers() {
	if (!device) {
		return;
	}
	RID content[9] = { receiver_buffer, staged_receiver_buffer,
		receiver_emission_buffer, staged_receiver_emission_buffer, receiver_lighting_buffer,
		native_light_unit_buffers[0], native_light_unit_buffers[1], native_light_state_buffer, native_light_sampler };
	receiver_buffer = RID();
	staged_receiver_buffer = RID();
	receiver_emission_buffer = RID();
	staged_receiver_emission_buffer = RID();
	receiver_lighting_buffer = RID();
	native_light_unit_buffers[0] = RID();
	native_light_unit_buffers[1] = RID();
	native_light_state_buffer = RID();
	native_light_sampler = RID();
	receiver_capacity = 0;
	for (const RID &buffer : content) {
		if (buffer.is_valid()) {
			device->free_rid(buffer);
		}
	}
}

void LRTVolume::_free_gpu_resources() {
	if (!device) {
		return;
	}
	_free_uniform_sets();
	_free_content_buffers();
	has_applied_grid = false;

	RID buffers[64];
	int buffer_count = 0;
	buffers[buffer_count++] = params_buffer;
	buffers[buffer_count++] = material_buffer;
	buffers[buffer_count++] = links_buffer;
	buffers[buffer_count++] = matrix_buffer;
	buffers[buffer_count++] = local_visibility_buffer;
	buffers[buffer_count++] = staged_material_buffer;
	buffers[buffer_count++] = staged_links_buffer;
	buffers[buffer_count++] = staged_matrix_buffer;
	buffers[buffer_count++] = staged_local_visibility_buffer;
	buffers[buffer_count++] = local_patch_buffer;
	buffers[buffer_count++] = receiver_patch_buffer;
	params_buffer = RID();
	material_buffer = RID();
	links_buffer = RID();
	matrix_buffer = RID();
	local_visibility_buffer = RID();
	staged_material_buffer = RID();
	staged_links_buffer = RID();
	staged_matrix_buffer = RID();
	staged_local_visibility_buffer = RID();
	local_patch_buffer = RID();
	receiver_patch_buffer = RID();
	for (int i = 0; i < 3; i++) {
		buffers[buffer_count++] = source_buffers[i];
		source_buffers[i] = RID();
		buffers[buffer_count++] = external_gi_buffers[i];
		external_gi_buffers[i] = RID();
		for (int phase = 0; phase < 3; phase++) {
			buffers[buffer_count++] = radiance_phase_buffers[phase][i];
			radiance_phase_buffers[phase][i] = RID();
		}
	}
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			buffers[buffer_count++] = radiance_buffers[buffer][channel];
			radiance_buffers[buffer][channel] = RID();
		}
		buffers[buffer_count++] = directional_visibility_buffers[buffer];
		directional_visibility_buffers[buffer] = RID();
		buffers[buffer_count++] = visibility_buffers[buffer];
		visibility_buffers[buffer] = RID();
	}
	for (int channel = 0; channel < 3; channel++) {
		buffers[buffer_count++] = sky_buffers[channel];
		sky_buffers[channel] = RID();
	}
	for (int i = 0; i < buffer_count; i++) {
		if (buffers[i].is_valid()) {
			device->free_rid(buffers[i]);
		}
	}
	for (int channel = 0; channel < 3; channel++) {
		field_textures[channel].unref();
		sky_textures[channel].unref();
		source_textures[channel].unref();
	}
	visibility_texture.unref();
	material_texture.unref();
	matrix_texture.unref();
	local_visibility_texture.unref();
	links_texture.unref();
	receiver_links_texture.unref();
	diagnostic_sdf_texture.unref();
	diagnostic_albedo_texture.unref();
	diagnostic_emission_texture.unref();
	diagnostic_dirty_texture.unref();
	for (int channel = 0; channel < 3; channel++) {
		field_texture_rids[channel] = RID();
		sky_texture_rids[channel] = RID();
		source_texture_rids[channel] = RID();
	}
	visibility_texture_rid = RID();
	material_texture_rid = RID();
	matrix_texture_rid = RID();
	local_visibility_texture_rid = RID();
	links_texture_rid = RID();
	receiver_links_texture_rid = RID();
	diagnostic_sdf_texture_rid = RID();
	diagnostic_albedo_texture_rid = RID();
	diagnostic_emission_texture_rid = RID();
	diagnostic_dirty_texture_rid = RID();
	local_grid_banks_synchronized = false;
	pipeline_inject = RID();
	pipeline_light_resolve = RID();
	pipeline_propagate = RID();
	pipeline_sky_project = RID();
	pipeline_display = RID();
	pipeline_local_patch = RID();
	shader_inject = RID();
	shader_light_resolve = RID();
	shader_propagate = RID();
	shader_sky_project = RID();
	shader_display = RID();
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

bool LRTVolume::_upload_params() {
	MutexLock lock(params_mutex);
	ParamsData params;
	params.grid_size[0] = grid.size[0];
	params.grid_size[1] = grid.size[1];
	params.grid_size[2] = grid.size[2];
	params.grid_size[3] = grid.count;
	params.grid_min[0] = float(grid.min.x);
	params.grid_min[1] = float(grid.min.y);
	params.grid_min[2] = float(grid.min.z);
	params.grid_min[3] = float(grid.spacing);
	params.counts[0] = int(local.receivers.size() / 12);
	params.counts[1] = native_light_count;
	params.counts[2] = SKY_DIRECTION_COUNT;
	params.flags[0] = multi_bounce ? 1.0f : 0.0f;
	params.flags[1] = sh_visibility ? 1.0f : 0.0f;
	params.flags[2] = local_backend == "sdf" ? 1.0f : 0.0f;
	params.flags[3] = native_light_fields_enabled ? 2.0f : (has_receiver_lighting ? 1.0f : 0.0f);
	for (int direction_index = 0; direction_index < sky_samples.size(); direction_index++) {
		const Vector3 sample = sky_samples[direction_index];
		params.sky_samples[direction_index][0] = sample.x;
		params.sky_samples[direction_index][1] = sample.y;
		params.sky_samples[direction_index][2] = sample.z;
	}
	if (!native_light_fields_enabled && receiver_lighting_buffer.is_valid() && !receiver_lighting.empty()) {
		device->buffer_update(receiver_lighting_buffer, 0, receiver_lighting.size() * sizeof(float), receiver_lighting.data());
	}
	if (native_light_state_buffer.is_valid()) {
		std::vector<NativeLightStateData> states;
		states.resize(size_t(native_light_capacity));
		for (int i = 0; i < native_light_capacity; i++) {
			const NativeLightState &source = native_light_states[size_t(i)];
			states[size_t(i)].scale[0] = source.scale.x;
			states[size_t(i)].scale[1] = source.scale.y;
			states[size_t(i)].scale[2] = source.scale.z;
			static_assert(sizeof(states[size_t(i)].scale[3]) == sizeof(source.cull_mask));
			memcpy(&states[size_t(i)].scale[3], &source.cull_mask, sizeof(source.cull_mask));
			states[size_t(i)].state[0] = float(source.current_buffer);
			states[size_t(i)].state[1] = source.blend;
			states[size_t(i)].state[2] = source.enabled ? 1.0f : 0.0f;
			states[size_t(i)].influence[0] = source.influence_origin.x;
			states[size_t(i)].influence[1] = source.influence_origin.y;
			states[size_t(i)].influence[2] = source.influence_origin.z;
			states[size_t(i)].influence[3] = source.influence_radius;
		}
		device->buffer_update(native_light_state_buffer, 0, states.size() * sizeof(NativeLightStateData), states.data());
	}
	return device->buffer_update(params_buffer, 0, sizeof(ParamsData), &params) == OK;
}

void LRTVolume::_upload_local_buffers() {
	if (local.material.empty()) {
		return;
	}
	const uint64_t buffers_started_usec = OS::get_singleton()->get_ticks_usec();
	device->buffer_update(material_buffer, 0, local.material.size() * sizeof(float), local.material.data());
	device->buffer_update(staged_material_buffer, 0, local.material.size() * sizeof(float), local.material.data());
	device->buffer_update(links_buffer, 0, local.links.size() * sizeof(uint32_t), local.links.data());
	device->buffer_update(staged_links_buffer, 0, local.links.size() * sizeof(uint32_t), local.links.data());
	device->buffer_update(matrix_buffer, 0, local.gpu_matrices.size() * sizeof(float), local.gpu_matrices.data());
	device->buffer_update(staged_matrix_buffer, 0, local.gpu_matrices.size() * sizeof(float), local.gpu_matrices.data());
	device->buffer_update(local_visibility_buffer, 0, local.local_visibility.size() * sizeof(float), local.local_visibility.data());
	device->buffer_update(staged_local_visibility_buffer, 0, local.local_visibility.size() * sizeof(float), local.local_visibility.data());
	if (!local.receivers.empty()) {
		device->buffer_update(receiver_buffer, 0, local.receivers.size() * sizeof(float), local.receivers.data());
		device->buffer_update(staged_receiver_buffer, 0, local.receivers.size() * sizeof(float), local.receivers.data());
	}
	if (!local.receiver_emission.empty()) {
		device->buffer_update(receiver_emission_buffer, 0, local.receiver_emission.size() * sizeof(float), local.receiver_emission.data());
		device->buffer_update(staged_receiver_emission_buffer, 0, local.receiver_emission.size() * sizeof(float), local.receiver_emission.data());
	}
	apply_full_upload_bytes += 2 * (local.material.size() * sizeof(float) + local.links.size() * sizeof(uint32_t) +
			local.gpu_matrices.size() * sizeof(float) + local.local_visibility.size() * sizeof(float) +
			local.receivers.size() * sizeof(float) + local.receiver_emission.size() * sizeof(float));
	local_grid_banks_synchronized = true;
	apply_buffer_upload_ms = double(OS::get_singleton()->get_ticks_usec() - buffers_started_usec) / 1000.0;
	const uint64_t textures_started_usec = OS::get_singleton()->get_ticks_usec();
	_upload_receiver_textures();
	if (!local_debug_textures_enabled.load()) {
		local_debug_textures_dirty = true;
		apply_texture_upload_ms = double(OS::get_singleton()->get_ticks_usec() - textures_started_usec) / 1000.0;
		return;
	}
	_upload_local_textures();
	apply_texture_upload_ms = double(OS::get_singleton()->get_ticks_usec() - textures_started_usec) / 1000.0;
}

void LRTVolume::_upload_receiver_textures() {
	device->texture_update(material_texture_rid, 0, bytes_of(local.material.data(), local.material.size() * sizeof(float)));
	std::vector<float> packed_links(local.receiver_links.size() * 4, 0.0f);
	for (size_t i = 0; i < local.receiver_links.size(); i++) {
		packed_links[i * 4] = float(local.receiver_links[i] & 0x1FFFu);
		packed_links[i * 4 + 1] = float((local.receiver_links[i] >> 13) & 0x1FFFu);
	}
	device->texture_update(receiver_links_texture_rid, 0, bytes_of(packed_links.data(), packed_links.size() * sizeof(float)));
}

bool LRTVolume::_upload_local_buffer_chunk() {
	if (apply_sparse_patch) {
		if (local.dirty_trunk_count == 0) {
			return true;
		}
		if (apply_buffer_stage == 0) {
			if (!staged_local_patches.empty()) {
				const uint32_t bytes = uint32_t(staged_local_patches.size() * sizeof(LocalPatchData));
				device->buffer_update(local_patch_buffer, 0, bytes, staged_local_patches.data());
				apply_local_patch_upload_bytes += bytes;
			}
			if (!staged_receiver_patches.empty()) {
				const uint32_t bytes = uint32_t(staged_receiver_patches.size() * sizeof(ReceiverPatchData));
				device->buffer_update(receiver_patch_buffer, 0, bytes, staged_receiver_patches.data());
				apply_receiver_patch_upload_bytes += bytes;
			}
			if (!local_grid_banks_synchronized) {
				device->buffer_copy(material_buffer, staged_material_buffer, 0, 0, uint32_t(local.material.size() * sizeof(float)));
				device->buffer_copy(links_buffer, staged_links_buffer, 0, 0, uint32_t(local.links.size() * sizeof(uint32_t)));
				device->buffer_copy(matrix_buffer, staged_matrix_buffer, 0, 0, uint32_t(local.gpu_matrices.size() * sizeof(float)));
				device->buffer_copy(local_visibility_buffer, staged_local_visibility_buffer, 0, 0,
						uint32_t(local.local_visibility.size() * sizeof(float)));
			}
			apply_buffer_stage = 4;
			if (local_grid_banks_synchronized && local.receiver_layout_stable) {
				apply_receiver_copy_range = staged_receiver_copy_ranges.size();
			}
		}
		size_t remaining_vectors = (APPLY_COPY_CHUNK_BYTES / (3 * 4 * sizeof(float))) * 3;
		while (apply_receiver_copy_range < staged_receiver_copy_ranges.size()) {
			const ReceiverCopyRange &range = staged_receiver_copy_ranges[apply_receiver_copy_range];
			const size_t vector_count = MIN(remaining_vectors, size_t(range.vector_count) - apply_buffer_offset);
			const size_t old_vector_start = size_t(range.old_vector_start) + apply_buffer_offset;
			const size_t new_vector_start = size_t(range.new_vector_start) + apply_buffer_offset;
			device->buffer_copy(receiver_buffer, staged_receiver_buffer,
					uint32_t(old_vector_start * 4 * sizeof(float)), uint32_t(new_vector_start * 4 * sizeof(float)),
					uint32_t(vector_count * 4 * sizeof(float)));
			device->buffer_copy(receiver_emission_buffer, staged_receiver_emission_buffer,
					uint32_t((old_vector_start / 3) * 4 * sizeof(float)), uint32_t((new_vector_start / 3) * 4 * sizeof(float)),
					uint32_t((vector_count / 3) * 4 * sizeof(float)));
			apply_receiver_copy_bytes += vector_count * 4 * sizeof(float) +
					(vector_count / 3) * 4 * sizeof(float);
			apply_buffer_offset += vector_count;
			if (apply_buffer_offset == range.vector_count) {
				apply_receiver_copy_range++;
				apply_buffer_offset = 0;
			}
			remaining_vectors -= vector_count;
			if (remaining_vectors == 0) {
				return false;
			}
		}
		apply_buffer_stage = 5;
		return true;
	}

	constexpr int stage_count = 6;
	while (apply_buffer_stage < stage_count) {
		RID buffer;
		const void *source = nullptr;
		size_t byte_count = 0;
		switch (apply_buffer_stage) {
				case 0:
					buffer = staged_material_buffer;
					source = local.material.data();
					byte_count = local.material.size() * sizeof(float);
					break;
				case 1:
					buffer = staged_links_buffer;
					source = local.links.data();
					byte_count = local.links.size() * sizeof(uint32_t);
					break;
			case 2:
				buffer = staged_matrix_buffer;
				source = local.gpu_matrices.data();
				byte_count = local.gpu_matrices.size() * sizeof(float);
					break;
				case 3:
					buffer = staged_local_visibility_buffer;
					source = local.local_visibility.data();
					byte_count = local.local_visibility.size() * sizeof(float);
					break;
				case 4:
					buffer = staged_receiver_buffer;
					source = local.receivers.data();
					byte_count = local.receivers.size() * sizeof(float);
					break;
				case 5:
					buffer = staged_receiver_emission_buffer;
					source = local.receiver_emission.data();
					byte_count = local.receiver_emission.size() * sizeof(float);
					break;
		}
		if (apply_buffer_offset >= byte_count) {
			apply_buffer_stage++;
			apply_buffer_offset = 0;
			continue;
		}
		const size_t chunk_bytes = MIN(APPLY_UPLOAD_CHUNK_BYTES, byte_count - apply_buffer_offset);
		const uint8_t *bytes = static_cast<const uint8_t *>(source);
		device->buffer_update(buffer, uint32_t(apply_buffer_offset), uint32_t(chunk_bytes), bytes + apply_buffer_offset);
		apply_full_upload_bytes += chunk_bytes;
		apply_buffer_offset += chunk_bytes;
		if (apply_buffer_offset == byte_count) {
			apply_buffer_stage++;
			apply_buffer_offset = 0;
		}
		return apply_buffer_stage >= stage_count;
	}
	return true;
}

void LRTVolume::_upload_local_textures() {
	device->texture_update(matrix_texture_rid, 0, bytes_of(local.matrices.data(), local.matrices.size() * sizeof(float)));
	device->texture_update(local_visibility_texture_rid, 0, bytes_of(local.local_visibility.data(), local.local_visibility.size() * sizeof(float)));
	if (!local.diagnostic_sdf.empty()) {
		device->texture_update(diagnostic_sdf_texture_rid, 0, bytes_of(local.diagnostic_sdf.data(), local.diagnostic_sdf.size() * sizeof(float)));
		device->texture_update(diagnostic_albedo_texture_rid, 0, bytes_of(local.diagnostic_albedo.data(), local.diagnostic_albedo.size() * sizeof(float)));
		device->texture_update(diagnostic_emission_texture_rid, 0, bytes_of(local.diagnostic_emission.data(), local.diagnostic_emission.size() * sizeof(float)));
		device->texture_update(diagnostic_dirty_texture_rid, 0, bytes_of(local.diagnostic_dirty.data(), local.diagnostic_dirty.size() * sizeof(float)));
	}
	std::vector<float> packed_links(local.links.size() * 4, 0.0f);
	for (size_t i = 0; i < local.links.size(); i++) {
		packed_links[i * 4] = float(local.links[i] & 0x1FFFu);
		packed_links[i * 4 + 1] = float((local.links[i] >> 13) & 0x1FFFu);
	}
	device->texture_update(links_texture_rid, 0, bytes_of(packed_links.data(), packed_links.size() * sizeof(float)));
	local_debug_textures_dirty = false;
}

// The bake already runs on a worker thread, so it spreads over the remaining cores; the cap
// keeps a 32-thread machine from oversubscribing memory bandwidth for no gain.
static int lrt_bake_thread_count() {
	return CLAMP(OS::get_singleton()->get_processor_count() - 1, 1, 16);
}

// Geometry distance fields are immutable and process-wide shared by content + precision.
// Material fields use a separate content cache, so recolouring never changes the SDF.
bool LRTVolume::_build_primitives(const String &p_backend, int p_threads, std::vector<lrt::SdfPrimitive> &r_primitives,
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
	std::set<const lrt::SdfInstanceField *> active_instance_fields;
	std::set<int> active_resolutions;
	auto record_primitive = [&](uint64_t p_signature, const std::shared_ptr<const lrt::SdfGeometryField> &p_geometry,
									const std::shared_ptr<const lrt::SdfInstanceField> &p_instance, const lrt::PrimitiveTransform &p_transform, uint32_t p_layer_mask) {
		if (active_specs.insert(p_signature).second) {
			sdf_bytes += lrt::asset_field_bytes(*p_geometry);
		}
		if (active_instance_fields.insert(p_instance.get()).second) {
			instance_field_bytes += uint64_t(sizeof(lrt::SdfInstanceField)) +
					uint64_t(p_instance->albedo.capacity()) + uint64_t(p_instance->emission.capacity() * sizeof(float));
		}
		const uint64_t material_signature = lrt::instance_field_signature(*p_instance);
		r_primitives.push_back(lrt::make_sdf_primitive(p_geometry, p_instance, p_transform,
				lrt::primitive_signature(p_signature, material_signature, p_transform), p_layer_mask,
				p_signature, material_signature));
	};
	for (const BoxInstance &box : box_instances) {
		const uint64_t field_signature = lrt::box_field_signature(box.local_extent, BOX_SDF_RESOLUTION);
		std::shared_ptr<const lrt::SdfGeometryField> geometry = lrt::find_shared_asset_field(field_signature);
		if (!geometry) {
			lrt::SdfGeometryField baked = lrt::bake_box_sdf(box.local_extent, BOX_SDF_RESOLUTION, &cancel_flag);
			if (baked.distance.empty()) {
				return false;
			}
			geometry = lrt::share_asset_field(field_signature, std::move(baked));
		}
		lrt::SdfInstanceField baked_instance = lrt::bake_constant_instance_field(*geometry, box.color, box.emission);
		const uint64_t instance_signature = lrt::instance_field_cache_signature(field_signature, lrt::instance_field_signature(baked_instance));
		std::shared_ptr<const lrt::SdfInstanceField> instance = lrt::find_shared_instance_field(instance_signature);
		if (!instance) {
			instance = lrt::share_instance_field(instance_signature, std::move(baked_instance));
		}
		record_primitive(field_signature, geometry, instance, box.transform, box.layer_mask);
	}

	// One job owns each unique geometry + precision pair. Instance material fields are handled
	// separately below, so repeated meshes never duplicate the geometry SDF.
	struct AssetJob {
		int instance = -1;
		uint64_t signature = 0;
		lrt::TriangleMesh mesh;
		lrt::MeshSdfBakeResult baked;
		std::shared_ptr<const lrt::SdfGeometryField> field;
		double bake_ms = 0.0;
	};
	std::vector<AssetJob> jobs;
	std::vector<int> job_of_instance(mesh_instances.size(), -1);
	std::map<uint64_t, int> job_by_signature;
	for (int i = 0; i < int(mesh_instances.size()); i++) {
		const MeshInstance &instance = mesh_instances[size_t(i)];
		if (!instance.triangles || instance.triangles->empty()) {
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
		lrt::SdfGeometryField loaded;
		if (lrt::load_asset_field(job.signature, loaded)) {
			job.field = lrt::share_asset_field(job.signature, std::move(loaded));
			assets_loaded++;
			preparation_completed.fetch_add(1);
		}
	}
	cache_read_ms += double(OS::get_singleton()->get_ticks_usec() - cache_read_begin) / 1000.0;
	// Per-asset parallelism avoids the long tail measured on heterogeneous carriage meshes: the
	// slowest job is not necessarily the one with the most samples, so a size threshold leaves
	// most workers idle. Jobs run serially and each exact nearest-distance sweep owns the complete
	// thread budget; the worker task itself participates in the sweep.
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
		sdf_scratch_peak_bytes = MAX(sdf_scratch_peak_bytes, jobs[size_t(job_index)].baked.sample_count * 16);
	}
	if (cancel_flag.load()) {
		return false;
	}
	const uint64_t cache_write_begin = OS::get_singleton()->get_ticks_usec();
	for (AssetJob &job : jobs) {
		if (job.field) {
			continue;
		}
		if (job.baked.error != lrt::MESH_SDF_BAKE_OK || job.baked.field.distance.empty()) {
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
		sdf_ray_queries += job.field->ray_queries;
		sdf_samples += job.field->distance.size();
		if (job.field->distance.size() > largest_sdf_samples) {
			largest_sdf_samples = job.field->distance.size();
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
		if (!instance.triangles || instance.triangles->empty()) {
			continue;
		}
		const int job_index = job_of_instance[size_t(i)];
		if (job_index < 0 || !jobs[size_t(job_index)].field) {
			return false;
		}
		AssetJob &job = jobs[size_t(job_index)];
		const uint64_t material_signature = lrt::instance_field_cache_signature(job.signature, instance.material_signature);
		std::shared_ptr<const lrt::SdfInstanceField> material = lrt::find_shared_instance_field(material_signature);
		if (!material) {
			if (job.mesh.triangles.empty()) {
				const uint64_t topology_begin = OS::get_singleton()->get_ticks_usec();
				job.mesh = lrt::build_triangle_mesh(*instance.triangles);
				topology_ms += double(OS::get_singleton()->get_ticks_usec() - topology_begin) / 1000.0;
			}
			const MeshInstance &representative = mesh_instances[size_t(job.instance)];
			lrt::TriangleMesh instance_mesh;
			const lrt::TriangleMesh *material_mesh = &job.mesh;
			if (instance.material_signature != representative.material_signature) {
				const uint64_t topology_begin = OS::get_singleton()->get_ticks_usec();
				instance_mesh = lrt::build_triangle_mesh(*instance.triangles);
				topology_ms += double(OS::get_singleton()->get_ticks_usec() - topology_begin) / 1000.0;
				material_mesh = &instance_mesh;
			}
			lrt::SdfInstanceField baked_material = lrt::bake_mesh_instance_field(*material_mesh, *job.field, instance.material.get(), &cancel_flag, p_threads);
			if (baked_material.albedo.empty()) {
				return false;
			}
			material = lrt::share_instance_field(material_signature, std::move(baked_material));
		}
		record_primitive(job.signature, job.field, material, instance.transform, instance.layer_mask);
	}
	instance_field_ms += double(OS::get_singleton()->get_ticks_usec() - instance_field_begin) / 1000.0;
	sdf_specs = int(active_specs.size());
	sdf_instance_references = int(r_primitives.size());
	sdf_resolutions.assign(active_resolutions.begin(), active_resolutions.end());
	return true;
}

int LRTVolume::_mesh_instance_count() const {
	int count = 0;
	for (const MeshInstance &instance : mesh_instances) {
		if (instance.triangles && !instance.triangles->empty()) {
			count++;
		}
	}
	return count;
}

uint64_t LRTVolume::_input_bytes() const {
	uint64_t bytes = vector_bytes(box_instances) + vector_bytes(mesh_instances);
	std::set<const std::vector<lrt::MeshTriangle> *> triangle_sets;
	std::set<const lrt::MaterialCapture *> material_sets;
	for (const MeshInstance &instance : mesh_instances) {
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
			vector_bytes(primitives) + vector_bytes(receiver_lighting) + vector_bytes(pending_changed_probes) +
			_receiver_capture_data_bytes(receiver_capture_data_cache) + sdf_bytes + instance_field_bytes;
}

uint64_t LRTVolume::_staged_cpu_bytes() const {
	return local_field_bytes(staged_local) + local_cache_bytes(staged_cache) +
			local_field_bytes(recycled_local) + local_cache_bytes(recycled_cache) +
			vector_bytes(staged_primitives) + _receiver_capture_data_bytes(staged_receiver_capture_data) +
			_receiver_capture_data_bytes(retired_receiver_capture_data) + vector_bytes(staged_local_patches) +
			vector_bytes(staged_receiver_patches) + vector_bytes(staged_receiver_copy_ranges);
}

uint64_t LRTVolume::_gpu_bytes() const {
	return uint64_t(_gpu_memory_breakdown().get("total_bytes", 0));
}

Dictionary LRTVolume::_gpu_memory_breakdown() const {
	Dictionary result;
	if (!has_local) {
		result["allocated"] = false;
		result["total_bytes"] = uint64_t(0);
		return result;
	}

	const uint64_t probe_count = uint64_t(grid.count);
	uint64_t receiver_count = 0;
	for (int probe = 0; probe < grid.count; probe++) {
		receiver_count += uint64_t(local.material[size_t(probe) * 4 + 1]);
	}
	const uint64_t receiver_layout_capacity = uint64_t(local.receivers.size() / 12);
	const uint64_t allocated_receiver_count = uint64_t(receiver_capacity);
	const uint64_t params_bytes = sizeof(ParamsData);
	const uint64_t material_bytes = probe_count * 4 * sizeof(float);
	const uint64_t links_bytes = probe_count * sizeof(uint32_t);
	const uint64_t matrix_bytes = probe_count * 20 * sizeof(float);
	const uint64_t local_visibility_bytes = probe_count * 4 * sizeof(float);
	const uint64_t staged_local_field_bytes = material_bytes + links_bytes + matrix_bytes + local_visibility_bytes;
	const uint64_t local_patch_bytes = MAX_LOCAL_PATCH_PROBES * sizeof(LocalPatchData);
	const uint64_t receiver_patch_bytes = MAX_RECEIVER_PATCHES * sizeof(ReceiverPatchData);
	const uint64_t source_bytes = probe_count * 3 * 4 * sizeof(float);
	const uint64_t external_gi_bytes = probe_count * 3 * 4 * sizeof(float);
	const uint64_t radiance_history_bytes = probe_count * 2 * 3 * 4 * sizeof(float);
	const uint64_t radiance_phase_bytes = probe_count * 3 * 3 * 4 * sizeof(float);
	const uint64_t sky_projection_bytes = probe_count * 3 * 4 * sizeof(float);
	const uint64_t directional_visibility_bytes = probe_count * 2 * SKY_DIRECTION_WORDS * sizeof(uint32_t);
	const uint64_t scalar_visibility_bytes = probe_count * 2 * 4 * sizeof(float);
	const uint64_t grid_storage_bytes = params_bytes + material_bytes + links_bytes + matrix_bytes +
			local_visibility_bytes + source_bytes + external_gi_bytes + radiance_history_bytes + radiance_phase_bytes +
			directional_visibility_bytes + scalar_visibility_bytes + sky_projection_bytes + staged_local_field_bytes +
			local_patch_bytes + receiver_patch_bytes;
	// Normal rendering consumes radiance, sky, material, propagation links and receiver links
	// (nine vec4 textures). The remaining 21 vec4 fields are full-sized only while a viewport
	// debug mode or explicit diagnostic readback is active; otherwise valid 1x1 storage-image
	// placeholders satisfy the display descriptor layout without reserving grid-sized memory.
	const uint64_t runtime_texture_bytes = probe_count * 9 * 4 * sizeof(float);
	const uint64_t diagnostic_texture_bytes = debug_textures_full_size.load() ?
			probe_count * 21 * 4 * sizeof(float) : uint64_t(21 * 4 * sizeof(float));
	const uint64_t receiver_geometry_bytes = MAX(uint64_t(16), allocated_receiver_count * 12 * sizeof(float));
	const uint64_t receiver_emission_bytes = MAX(uint64_t(16), allocated_receiver_count * 4 * sizeof(float));
	const uint64_t receiver_lighting_bytes = MAX(uint64_t(16), allocated_receiver_count * 4 * sizeof(float));
	const uint64_t native_light_field_bytes = 2 * MAX(uint64_t(16), allocated_receiver_count * uint64_t(native_light_capacity) * 4 * sizeof(float));
	const uint64_t native_light_state_bytes = sizeof(NativeLightStateData) * uint64_t(native_light_capacity);
	const uint64_t receiver_storage_bytes = 2 * (receiver_geometry_bytes + receiver_emission_bytes) + receiver_lighting_bytes;
	const uint64_t total_bytes = grid_storage_bytes + runtime_texture_bytes + diagnostic_texture_bytes +
			receiver_storage_bytes + native_light_field_bytes + native_light_state_bytes;

	result["allocated"] = true;
	result["probe_count"] = probe_count;
	result["receiver_count"] = receiver_count;
	result["receiver_layout_capacity"] = receiver_layout_capacity;
	result["receiver_layout_stable"] = local.receiver_layout_stable;
	result["receiver_capacity"] = allocated_receiver_count;
	result["native_light_capacity"] = native_light_capacity;
	result["params_bytes"] = params_bytes;
	result["material_bytes"] = material_bytes;
	result["links_bytes"] = links_bytes;
	result["matrix_bytes"] = matrix_bytes;
	result["local_visibility_bytes"] = local_visibility_bytes;
	result["staged_local_field_bytes"] = staged_local_field_bytes;
	result["local_patch_bytes"] = local_patch_bytes;
	result["receiver_patch_bytes"] = receiver_patch_bytes;
	result["source_bytes"] = source_bytes;
	result["external_gi_bytes"] = external_gi_bytes;
	result["radiance_history_bytes"] = radiance_history_bytes;
	result["radiance_phase_bytes"] = radiance_phase_bytes;
	result["sky_projection_bytes"] = sky_projection_bytes;
	result["directional_visibility_bytes"] = directional_visibility_bytes;
	result["directional_visibility_encoding"] = "binary_bitset";
	result["scalar_visibility_bytes"] = scalar_visibility_bytes;
	result["grid_storage_bytes"] = grid_storage_bytes;
	result["runtime_texture_bytes"] = runtime_texture_bytes;
	result["diagnostic_texture_bytes"] = diagnostic_texture_bytes;
	result["debug_textures_full_size"] = debug_textures_full_size.load();
	result["receiver_geometry_bytes"] = receiver_geometry_bytes;
	result["receiver_emission_bytes"] = receiver_emission_bytes;
	result["receiver_lighting_bytes"] = receiver_lighting_bytes;
	result["receiver_storage_bytes"] = receiver_storage_bytes;
	result["native_light_field_bytes"] = native_light_field_bytes;
	result["native_light_state_bytes"] = native_light_state_bytes;
	result["total_bytes"] = total_bytes;
	result["resource_object_overhead_included"] = false;
	return result;
}

// The CPU half of the bake: the part that dominates a cold carriage build. It only touches
// plain data, so LRTVolume3D can run it on a worker thread; every GPU call stays in
// apply_local_field() on the main thread.
LRTVolume::LocalBakeResult LRTVolume::bake_local_field_data(bool p_analytic) {
	LocalBakeResult result;
	// Releasing the previous receiver arrays can take a visible fraction of a frame. They are no
	// longer used after the last apply, so retire them here on the bake worker.
	retired_receiver_capture_data = Dictionary();
	if (!configured) {
		return result;
	}
	{
		MutexLock lock(params_mutex);
		local_backend = p_analytic ? "analytic" : "sdf";
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	const int full_bake_threads = lrt_bake_thread_count();
	// Cached incremental edits touch only a few trunks. Spawning the full bake fan-out for each
	// short pass creates more scheduler and memory-bandwidth contention than useful parallel work.
	// Keep initial/full builds wide, but leave the render and main threads ample headroom at runtime.
	const int threads = local_cache.local != nullptr ? 1 : full_bake_threads;
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
	sdf_ray_queries = 0;
	preparation_error = lrt::MESH_SDF_BAKE_OK;
	sdf_specs = 0;
	sdf_instance_references = 0;
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
	staged_receiver_capture_data.clear();
	std::vector<lrt::SdfPrimitive> bake_primitives;
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
	preparation_phase.store(2);
	if (p_analytic) {
		staged_local = lrt::build_local_data(grid, lrt::BoxQuery(analytic_boxes), &cancel_flag, threads);
	} else {
		// The incremental path lives in the SDF backend, exactly like the prototype's
		// src/sdf-local.js; the analytic backend has no dirty-region support there either.
		staged_cache = std::move(recycled_cache);
		staged_local = lrt::build_sdf_local_data(
				grid, bake_primitives, &cancel_flag, threads, &local_cache, &staged_cache, &recycled_local);
	}
	const uint64_t after_local = OS::get_singleton()->get_ticks_usec();
	if (cancel_flag.load()) {
		has_staged = false;
		staged_receiver_capture_data.clear();
		result.cancelled = true;
		preparation_phase.store(0);
		return result;
	}
	preparation_phase.store(3);
	lrt::build_local_visibility(staged_local, threads);
	stabilize_receiver_pages(staged_local, !p_analytic && local_cache.local != nullptr ? &local : nullptr, grid.count);
	build_gpu_luminance_matrices(staged_local, grid.count);
	staged_local_patches.clear();
	staged_receiver_patches.clear();
	bool sparse_patch_valid = staged_local.dirty_trunk_count <= MAX_LOCAL_PATCH_PROBES / PROBES_PER_LOCAL_TRUNK;
	if (staged_local.dirty_trunk_count > 0 && sparse_patch_valid) {
		staged_local_patches.reserve(MIN(grid.count, staged_local.dirty_trunk_count * PROBES_PER_LOCAL_TRUNK));
		for (int probe = 0; probe < grid.count; probe++) {
			if (staged_local.diagnostic_dirty[size_t(probe) * 4] < 0.5f) {
				continue;
			}
			LocalPatchData patch;
			patch.header[0] = uint32_t(probe);
			patch.header[1] = staged_local.links[size_t(probe)];
			patch.header[2] = uint32_t(staged_receiver_patches.size());
			patch.header[3] = uint32_t(staged_local.material[size_t(probe) * 4 + 1]);
			memcpy(patch.material, staged_local.material.data() + size_t(probe) * 4, sizeof(patch.material));
			memcpy(patch.local_visibility, staged_local.local_visibility.data() + size_t(probe) * 4, sizeof(patch.local_visibility));
			for (int matrix = 0; matrix < 5; matrix++) {
				memcpy(patch.matrices[matrix],
						staged_local.gpu_matrices.data() + (size_t(matrix) * grid.count + probe) * 4,
						sizeof(patch.matrices[matrix]));
			}
			if (patch.header[3] > 0) {
				const size_t receiver_start = size_t(patch.header[2]);
				const size_t local_receiver_start = staged_local.receiver_delta ?
						size_t(staged_local.receiver_staging_offsets[size_t(probe)]) : size_t(patch.material[0]) / 3;
				if ((local_receiver_start + patch.header[3]) * 12 > staged_local.receivers.size() ||
						(local_receiver_start + patch.header[3]) * 4 > staged_local.receiver_emission.size()) {
					print_error(vformat("Invalid LRT receiver patch: probe=%d start=%d count=%d receivers=%d emission=%d",
							probe, local_receiver_start, patch.header[3], staged_local.receivers.size(), staged_local.receiver_emission.size()));
					return result;
				}
				if (receiver_start + patch.header[3] > MAX_RECEIVER_PATCHES) {
					sparse_patch_valid = false;
					break;
				}
				staged_receiver_patches.resize(receiver_start + patch.header[3]);
				for (size_t receiver = 0; receiver < patch.header[3]; receiver++) {
					ReceiverPatchData &receiver_patch = staged_receiver_patches[receiver_start + receiver];
					memcpy(receiver_patch.receiver,
							staged_local.receivers.data() + (local_receiver_start + receiver) * 12,
							sizeof(receiver_patch.receiver));
					memcpy(receiver_patch.emission,
							staged_local.receiver_emission.data() + (local_receiver_start + receiver) * 4,
							sizeof(receiver_patch.emission));
				}
			}
			staged_local_patches.push_back(patch);
		}
	}
	if (!sparse_patch_valid) {
		staged_local_patches.clear();
		staged_receiver_patches.clear();
	}
	staged_receiver_copy_ranges.clear();
	staged_receiver_copy_valid = sparse_patch_valid && local.material.size() == staged_local.material.size();
	if (staged_receiver_copy_valid) {
		for (int probe = 0; probe < grid.count; probe++) {
			if (staged_local.diagnostic_dirty[size_t(probe) * 4] >= 0.5f) {
				continue;
			}
			const uint32_t old_count = uint32_t(local.material[size_t(probe) * 4 + 1]);
			const uint32_t new_count = uint32_t(staged_local.material[size_t(probe) * 4 + 1]);
			if (old_count != new_count) {
				staged_receiver_copy_valid = false;
				break;
			}
			if (new_count == 0) {
				continue;
			}
			const uint32_t old_start = uint32_t(local.material[size_t(probe) * 4]);
			const uint32_t new_start = uint32_t(staged_local.material[size_t(probe) * 4]);
			const uint32_t vector_count = new_count * 3;
			if (!staged_receiver_copy_ranges.empty()) {
				ReceiverCopyRange &range = staged_receiver_copy_ranges.back();
				if (range.old_vector_start + range.vector_count == old_start &&
						range.new_vector_start + range.vector_count == new_start) {
					range.vector_count += vector_count;
					continue;
				}
			}
			staged_receiver_copy_ranges.push_back({ old_start, new_start, vector_count });
		}
	}
	const uint64_t after_visibility = OS::get_singleton()->get_ticks_usec();
	staged_primitives = std::move(bake_primitives);
	if (staged_local.receiver_delta) {
		staged_receiver_capture_data.clear();
	} else {
		staged_receiver_capture_data = _make_receiver_capture_data(staged_local);
	}
	const uint64_t after_receiver_capture = OS::get_singleton()->get_ticks_usec();
	const uint64_t after_display = OS::get_singleton()->get_ticks_usec();
	has_staged = true;
	input_bytes = _input_bytes();
	active_cpu_bytes = _active_cpu_bytes();
	staged_cpu_bytes = _staged_cpu_bytes();
	cpu_peak_bytes = active_cpu_bytes + MAX(staged_cpu_bytes, sdf_scratch_peak_bytes);

	result.ok = true;
	result.solid = staged_local.solid_count;
	result.surface = staged_local.surface_count;
	for (int probe = 0; probe < grid.count; probe++) {
		result.receiver_count += int(staged_local.material[size_t(probe) * 4 + 1]);
	}
	result.receivers = result.receiver_count * 12;
	result.receiver_layout_capacity = int(staged_local.receiver_layout_capacity);
	result.receiver_layout_compacted = staged_local.receiver_layout_compacted;
	result.trunks = staged_local.trunk_count;
	result.dirty_trunks = staged_local.dirty_trunk_count;
	result.primitive_ltm_cache_hits = staged_local.primitive_ltm_cache_hits;
	result.primitive_ltm_cache_misses = staged_local.primitive_ltm_cache_misses;
	result.primitive_ltm_overlap_fallbacks = staged_local.primitive_ltm_overlap_fallbacks;
	result.primitive_ltm_cache_bytes = staged_local.primitive_ltm_cache_bytes;
	result.primitive_ltm_cache_entries = staged_local.primitive_ltm_cache_entries;
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
	result.sdf_ray_queries = sdf_ray_queries;
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
	result.visibility_ms = double(after_visibility - after_local) / 1000.0;
	result.receiver_capture_ms = double(after_receiver_capture - after_visibility) / 1000.0;
	result.display_ms = double(after_display - after_receiver_capture) / 1000.0;
	result.build_ms = double(after_display - start) / 1000.0;
	preparation_phase.store(5);
	return result;
}

bool LRTVolume::store_local_field_cache(uint64_t p_fingerprint) const {
	if (!has_local || p_fingerprint == 0) {
		return false;
	}
	const String path = local_cache_path(p_fingerprint);
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
	file->store_32(uint32_t(local.solid_count));
	file->store_32(uint32_t(local.surface_count));
	file->store_32(uint32_t(local.classification_mismatches));
	file->store_32(uint32_t(local.trunk_count));
	file->store_32(uint32_t(local.dirty_trunk_count));
	store_local_cache_values(file, local.material);
	store_local_cache_values(file, local.matrices);
	store_local_cache_values(file, local.links);
	store_local_cache_values(file, local.receiver_links);
	store_local_cache_values(file, local.local_visibility);
	store_local_cache_values(file, local.diagnostic_sdf);
	store_local_cache_values(file, local.diagnostic_albedo);
	store_local_cache_values(file, local.diagnostic_emission);
	store_local_cache_values(file, local.diagnostic_dirty);
	store_local_cache_values(file, local.receivers);
	store_local_cache_values(file, local.receiver_emission);
	const Error write_error = file->get_error();
	file.unref();
	if (write_error != OK) {
		return false;
	}
	Ref<DirAccess> directory = DirAccess::open(lrt::asset_cache_directory());
	return directory.is_valid() && directory->rename(temporary.get_file(), path.get_file()) == OK;
}

bool LRTVolume::load_local_field_cache(uint64_t p_fingerprint, LocalBakeResult &r_result) {
	if (p_fingerprint == 0) {
		return false;
	}
	Ref<FileAccess> file = FileAccess::open(local_cache_path(p_fingerprint), FileAccess::READ);
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
			!read_local_cache_values(file, cached.matrices, count * 48) || cached.matrices.size() != count * 48 ||
			!read_local_cache_values(file, cached.links, count) || cached.links.size() != count ||
			!read_local_cache_values(file, cached.receiver_links, count) || cached.receiver_links.size() != count ||
			!read_local_cache_values(file, cached.local_visibility, count * 4) || cached.local_visibility.size() != count * 4 ||
			!read_local_cache_values(file, cached.diagnostic_sdf, count * 4) || cached.diagnostic_sdf.size() != count * 4 ||
			!read_local_cache_values(file, cached.diagnostic_albedo, count * 4) || cached.diagnostic_albedo.size() != count * 4 ||
			!read_local_cache_values(file, cached.diagnostic_emission, count * 4) || cached.diagnostic_emission.size() != count * 4 ||
			!read_local_cache_values(file, cached.diagnostic_dirty, count * 4) || cached.diagnostic_dirty.size() != count * 4 ||
			!read_local_cache_values(file, cached.receivers, count * 26 * 12) || cached.receivers.size() % 12 != 0 ||
			!read_local_cache_values(file, cached.receiver_emission, count * 26 * 4) ||
			cached.receiver_emission.size() != cached.receivers.size() / 3 || file->get_error() != OK) {
		return false;
	}
	cached.changed_occupancy.clear();
	cached.changed_occupancy_valid = false;
	stabilize_receiver_pages(cached, nullptr, cached_grid.count);
	build_gpu_luminance_matrices(cached, cached_grid.count);
	grid = cached_grid;
	configured = true;
	local_backend = cached_backend;
	staged_local = std::move(cached);
	staged_cache = lrt::LocalCache();
	staged_receiver_capture_data = _make_receiver_capture_data(staged_local);
	staged_primitives.clear();
	staged_local_patches.clear();
	staged_receiver_patches.clear();
	staged_receiver_copy_ranges.clear();
	staged_receiver_copy_valid = false;
	has_staged = true;
	r_result = LocalBakeResult();
	r_result.ok = true;
	r_result.solid = staged_local.solid_count;
	r_result.surface = staged_local.surface_count;
	for (int probe = 0; probe < cached_grid.count; probe++) {
		r_result.receiver_count += int(staged_local.material[size_t(probe) * 4 + 1]);
	}
	r_result.receivers = r_result.receiver_count * 12;
	r_result.receiver_layout_capacity = int(staged_local.receiver_layout_capacity);
	r_result.receiver_layout_compacted = staged_local.receiver_layout_compacted;
	r_result.trunks = staged_local.trunk_count;
	r_result.dirty_trunks = staged_local.dirty_trunk_count;
	r_result.primitive_ltm_cache_hits = staged_local.primitive_ltm_cache_hits;
	r_result.primitive_ltm_cache_misses = staged_local.primitive_ltm_cache_misses;
	r_result.primitive_ltm_overlap_fallbacks = staged_local.primitive_ltm_overlap_fallbacks;
	r_result.primitive_ltm_cache_bytes = staged_local.primitive_ltm_cache_bytes;
	r_result.primitive_ltm_cache_entries = staged_local.primitive_ltm_cache_entries;
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
		result["preparation_error"] = baked.preparation_error;
		result["assets_requested"] = baked.assets_requested;
		result["assets_prepared"] = baked.assets_prepared;
		return result;
	}
	result["ok"] = true;
	result["backend"] = local_backend;
	result["solid"] = baked.solid;
	result["surface"] = baked.surface;
	result["receivers"] = baked.receivers;
	result["receiver_count"] = baked.receiver_count;
	result["receiver_layout_capacity"] = baked.receiver_layout_capacity;
	result["receiver_layout_compacted"] = baked.receiver_layout_compacted;
	result["trunks"] = baked.trunks;
	result["dirty_trunks"] = baked.dirty_trunks;
	result["primitive_ltm_cache_hits"] = baked.primitive_ltm_cache_hits;
	result["primitive_ltm_cache_misses"] = baked.primitive_ltm_cache_misses;
	result["primitive_ltm_overlap_fallbacks"] = baked.primitive_ltm_overlap_fallbacks;
	result["primitive_ltm_cache_bytes"] = int64_t(baked.primitive_ltm_cache_bytes);
	result["primitive_ltm_cache_entries"] = int64_t(baked.primitive_ltm_cache_entries);
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
	result["sdf_ray_queries"] = int64_t(baked.sdf_ray_queries);
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
	result["receiver_capture_ms"] = baked.receiver_capture_ms;
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
		const uint32_t directional_offset = uint32_t(p_probes[index]) * SKY_DIRECTION_WORDS * sizeof(uint32_t);
		const uint32_t directional_bytes = uint32_t(run_count * SKY_DIRECTION_WORDS * sizeof(uint32_t));
		for (int buffer = 0; buffer < 2; buffer++) {
			for (int channel = 0; channel < 3; channel++) {
				device->buffer_clear(radiance_buffers[buffer][channel], offset, bytes);
			}
			device->buffer_clear(directional_visibility_buffers[buffer], directional_offset, directional_bytes);
			device->buffer_clear(visibility_buffers[buffer], offset, bytes);
		}
		for (int channel = 0; channel < 3; channel++) {
			device->buffer_clear(sky_buffers[channel], offset, bytes);
		}
		for (int phase = 0; phase < 3; phase++) {
			for (int channel = 0; channel < 3; channel++) {
				device->buffer_clear(radiance_phase_buffers[phase][channel], offset, bytes);
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
	ERR_FAIL_COND_V(_ensure_device() != OK, false);

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
	if (staged_local.receiver_delta) {
		materialize_receiver_delta(staged_local, local, grid.count);
		staged_receiver_capture_data = _make_receiver_capture_data(staged_local);
	}
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
	recycled_local = std::move(local);
	local = std::move(staged_local);
	if (local.receiver_layout_compacted) {
		receiver_layout_compactions++;
	}
	primitives = std::move(staged_primitives);
	// Dictionary assignment is reference-counted. Detach the staging handle after the handoff so
	// the multi-megabyte packed arrays are neither copied nor cleared on the main thread.
	retired_receiver_capture_data = receiver_capture_data_cache;
	receiver_capture_data_cache = staged_receiver_capture_data;
	staged_receiver_capture_data = Dictionary();
	receiver_capture_data_dirty = false;
	{
		MutexLock lock(params_mutex);
		// Native-light injection writes this GPU buffer before it can be observed. Keeping a
		// receiver-capacity CPU mirror here only adds a multi-megabyte allocation to every
		// incremental apply; diagnostic readback sizes the mirror when it is actually requested.
		receiver_lighting.clear();
		has_receiver_lighting = false;
	}
	// The incremental cache and the field it describes must always switch together.
	recycled_cache = std::move(local_cache);
	local_cache = std::move(staged_cache);
	local_cache.local = &local;
	has_staged = false;
	pending_changed_probes = std::move(changed);
	// The bake worker already measured active + staged allocations before this move-only handoff;
	// recomputing the same peak here would rescan all mesh inputs on the main thread.
	apply_error = OK;
	apply_done.store(false);
	apply_needs_submit.store(false);
	apply_propagation_safe.store(false);
	apply_pending = true;
	apply_preserve_history = preserve;
	apply_sparse_patch = preserve && local.changed_occupancy_valid &&
			staged_local_patches.size() <= MAX_LOCAL_PATCH_PROBES &&
			staged_receiver_patches.size() <= MAX_RECEIVER_PATCHES && staged_receiver_copy_valid;
	apply_grid_bank_switch = preserve && (!apply_sparse_patch || local.dirty_trunk_count > 0);
	apply_links_changed = !preserve || local.links_changed;
	apply_buffer_stage = 0;
	apply_buffer_offset = 0;
	apply_receiver_copy_range = 0;
	apply_submit_ms = 0.0;
	apply_local_patch_upload_bytes = 0;
	apply_receiver_patch_upload_bytes = 0;
	apply_receiver_copy_bytes = 0;
	apply_full_upload_bytes = 0;
	_queue_apply_chunk();
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
	while (p_wait && apply_pending && !apply_done.load()) {
		RenderingServer::get_singleton()->sync();
		if (!apply_done.load() && apply_needs_submit.exchange(false)) {
			_queue_apply_chunk();
		}
	}
	if (!p_wait && apply_pending && !apply_done.load() && apply_needs_submit.exchange(false)) {
		_queue_apply_chunk();
	}
	if (!apply_pending || !apply_done.load()) {
		return result;
	}
	apply_pending = false;
	staged_local_patches.clear();
	staged_receiver_patches.clear();
	staged_receiver_copy_ranges.clear();
	ERR_FAIL_COND_V(apply_error != OK, result);
	has_local = true;
	applied_grid = grid;
	applied_backend = local_backend;
	has_applied_grid = true;
	if (!apply_preserve_history) {
		iteration = 0;
	}

	result["backend"] = local_backend;
	result["preserved_history"] = apply_preserve_history;
	result["cleared_probes"] = int(pending_changed_probes.size());
	result["solid"] = local.solid_count;
	result["surface"] = local.surface_count;
	result["receivers"] = int(local.receivers.size());
	uint64_t active_receiver_count = 0;
	for (int probe = 0; probe < grid.count; probe++) {
		active_receiver_count += uint64_t(local.material[size_t(probe) * 4 + 1]);
	}
	result["receiver_count"] = int64_t(active_receiver_count);
	result["receiver_layout_capacity"] = int64_t(local.receiver_layout_capacity);
	result["receiver_layout_compaction_pending"] = local.receiver_layout_compaction_pending;
	result["receiver_layout_compacted"] = local.receiver_layout_compacted;
	result["trunks"] = local.trunk_count;
	result["primitive_ltm_cache_hits"] = local.primitive_ltm_cache_hits;
	result["primitive_ltm_cache_misses"] = local.primitive_ltm_cache_misses;
	result["primitive_ltm_overlap_fallbacks"] = local.primitive_ltm_overlap_fallbacks;
	result["primitive_ltm_cache_bytes"] = int64_t(local.primitive_ltm_cache_bytes);
	result["primitive_ltm_cache_entries"] = int64_t(local.primitive_ltm_cache_entries);
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
	result["sdf_ray_queries"] = int64_t(sdf_ray_queries);
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
	result["upload_submit_ms"] = apply_submit_ms;
	result["upload_ms"] = double(OS::get_singleton()->get_ticks_usec() - apply_started_usec) / 1000.0;
	result["upload_resources_ms"] = apply_resources_ms;
	result["upload_buffers_ms"] = apply_buffer_upload_ms;
	result["upload_textures_ms"] = apply_texture_upload_ms;
	result["upload_finalize_ms"] = apply_finalize_ms;
	result["local_patch_upload_bytes"] = int64_t(apply_local_patch_upload_bytes);
	result["receiver_patch_upload_bytes"] = int64_t(apply_receiver_patch_upload_bytes);
	result["receiver_copy_bytes"] = int64_t(apply_receiver_copy_bytes);
	result["full_upload_bytes"] = int64_t(apply_full_upload_bytes);
	result["receiver_layout_full_rebuilds"] = int64_t(receiver_layout_full_rebuilds);
	result["receiver_layout_compactions"] = int64_t(receiver_layout_compactions);
	return result;
}

void LRTVolume::_apply_render_thread(bool p_preserve_history) {
	if (apply_buffer_stage == 0 && apply_buffer_offset == 0) {
		const uint64_t resources_started_usec = OS::get_singleton()->get_ticks_usec();
		apply_buffer_upload_ms = 0.0;
		apply_texture_upload_ms = 0.0;
		apply_finalize_ms = 0.0;
		bool recreate_uniform_sets = true;
		if (p_preserve_history) {
			// A local edit normally changes only buffer contents. Keep the allocated receiver capacity
			// and its descriptor sets until the edited field actually outgrows them.
			const size_t receiver_count = local.receivers.size() / 12;
			if (receiver_count <= receiver_capacity && receiver_buffer.is_valid() && staged_receiver_buffer.is_valid() &&
					receiver_emission_buffer.is_valid() && staged_receiver_emission_buffer.is_valid() &&
					receiver_lighting_buffer.is_valid() && native_light_unit_buffers[0].is_valid() &&
					native_light_unit_buffers[1].is_valid() && native_light_state_buffer.is_valid() && native_light_sampler.is_valid()) {
				recreate_uniform_sets = false;
			} else {
				// The active receiver bank cannot be the repack source after a capacity rebuild.
				apply_sparse_patch = false;
				apply_grid_bank_switch = true;
				receiver_layout_full_rebuilds++;
				_free_uniform_sets();
				_free_content_buffers();
				apply_error = _create_content_buffers();
			}
		} else {
			_free_gpu_resources();
			apply_error = _create_buffers();
		}
		if (apply_error != OK) {
			apply_done.store(true);
			return;
		}
		apply_error = _create_shaders();
		if (apply_error != OK) {
			apply_done.store(true);
			return;
		}
		if (recreate_uniform_sets) {
			apply_error = _create_uniform_sets();
			if (apply_error != OK) {
				apply_done.store(true);
				return;
			}
		}
		apply_propagation_safe.store(p_preserve_history && !recreate_uniform_sets);
		apply_resources_ms = double(OS::get_singleton()->get_ticks_usec() - resources_started_usec) / 1000.0;
	}
	const uint64_t buffers_started_usec = OS::get_singleton()->get_ticks_usec();
	if (p_preserve_history && !_upload_local_buffer_chunk()) {
		apply_buffer_upload_ms += double(OS::get_singleton()->get_ticks_usec() - buffers_started_usec) / 1000.0;
		apply_needs_submit.store(true);
		return;
	}
	if (p_preserve_history) {
		apply_buffer_upload_ms += double(OS::get_singleton()->get_ticks_usec() - buffers_started_usec) / 1000.0;
		const uint64_t textures_started_usec = OS::get_singleton()->get_ticks_usec();
		_upload_receiver_textures();
		if (local_debug_textures_enabled.load()) {
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
				int32_t probe_count;
				int32_t write_receivers;
				int32_t pad1;
			} push_constant = { int32_t(staged_local_patches.size()), grid.count, 1, 0 };
			RD::ComputeListID patch_list = device->compute_list_begin();
			device->compute_list_bind_compute_pipeline(patch_list, pipeline_local_patch);
			device->compute_list_bind_uniform_set(patch_list, staged_uniform_set_local_patch, 0);
			device->compute_list_set_push_constant(patch_list, &push_constant, sizeof(push_constant));
			device->compute_list_dispatch(patch_list,
					Math::division_round_up(uint32_t(staged_local_patches.size()), uint32_t(WORKGROUP_SIZE)), 1, 1);
			push_constant.write_receivers = local.receiver_layout_stable ? 1 : 0;
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
		SWAP(links_buffer, staged_links_buffer);
		SWAP(matrix_buffer, staged_matrix_buffer);
		SWAP(local_visibility_buffer, staged_local_visibility_buffer);
		SWAP(receiver_buffer, staged_receiver_buffer);
		SWAP(receiver_emission_buffer, staged_receiver_emission_buffer);
		SWAP(uniform_set_inject, staged_uniform_set_inject);
		SWAP(uniform_set_local_patch, staged_uniform_set_local_patch);
		for (int buffer = 0; buffer < 2; buffer++) {
			SWAP(uniform_set_propagate[buffer], staged_uniform_set_propagate[buffer]);
		}
	}
	_upload_params();
	local_field_version.fetch_add(1);
	if (p_preserve_history) {
		_clear_changed_occupancy(pending_changed_probes);
		if (apply_links_changed) {
			sky_projection_dirty.store(true);
			sky_visibility_iterations_remaining =
					(grid.size[0] + grid.size[1] + grid.size[2] + SKY_PATH_LENGTH - 1) / SKY_PATH_LENGTH + 1;
			sky_visibility_word_offset = 0;
		}
		_sync_display();
	} else {
		current = 0;
		_reset_render_thread();
	}
	apply_finalize_ms = double(OS::get_singleton()->get_ticks_usec() - finalize_started_usec) / 1000.0;
	apply_done.store(true);
}

void LRTVolume::_submit_apply_chunk() {
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

void LRTVolume::_queue_apply_chunk() {
	_submit_apply_chunk();
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
	while (true) {
		_update_gpu_timing();
		const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
		injection_dirty.store(false);
		_upload_params();
		const uint64_t batch_version = source_version.fetch_add(1) + 1;
		const bool timing_active = _begin_gpu_timestamp(GPU_TIMING_INJECT, 1, batch_version);
		RD::ComputeListID list = device->compute_list_begin();
		const bool project_sky = sky_projection_dirty.exchange(false);
		if (project_sky) {
			struct SkyPushConstant {
				int32_t mode;
				int32_t probe_count;
				int32_t pad0;
				int32_t pad1;
			} sky_push_constant = { grid.count, 0, 0, 0 };
			device->compute_list_bind_compute_pipeline(list, pipeline_sky_project);
			device->compute_list_bind_uniform_set(list, uniform_set_sky_project[current], 0);
			device->compute_list_set_push_constant(list, &sky_push_constant, sizeof(SkyPushConstant));
			device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
			device->compute_list_add_barrier(list);
			display_sky_dirty = true;
		}
		device->compute_list_bind_compute_pipeline(list, pipeline_inject);
		device->compute_list_bind_uniform_set(list, uniform_set_inject, 0);
		device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
		device->compute_list_end();
		display_source_dirty = true;
		_end_gpu_timestamp(GPU_TIMING_INJECT, timing_active);
		gpu_pass_dispatches[GPU_TIMING_INJECT].fetch_add(1);
		// A propagation batch queued by the same main-thread frame publishes both the new source
		// and its resulting radiance. Paused/single-step workflows have no such batch and still
		// receive an immediate diagnostic source snapshot.
		if (pending_step_iterations.load() == 0) {
			_sync_display();
		}
		last_render_thread_pass_ms[GPU_TIMING_INJECT].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
		injection_pending.store(false);
		if (!injection_dirty.exchange(false) || injection_pending.exchange(true)) {
			break;
		}
	}
}

void LRTVolume::_reset_native_light_buffers_render_thread() {
	const size_t bytes = MAX(size_t(16), (local.receivers.size() / 12) * size_t(native_light_capacity) * 4 * sizeof(float));
	for (RID buffer : native_light_unit_buffers) {
		if (buffer.is_valid()) {
			device->buffer_clear(buffer, 0, bytes);
		}
	}
}

void LRTVolume::_resize_native_light_buffers_render_thread(int p_capacity) {
	if (device == nullptr || !has_local || p_capacity <= 0) {
		return;
	}
	_free_uniform_sets();
	for (RID &buffer : native_light_unit_buffers) {
		if (buffer.is_valid()) {
			device->free_rid(buffer);
			buffer = RID();
		}
	}
	if (native_light_state_buffer.is_valid()) {
		device->free_rid(native_light_state_buffer);
		native_light_state_buffer = RID();
	}
	const size_t bytes = MAX(size_t(16), receiver_capacity * size_t(p_capacity) * 4 * sizeof(float));
	for (RID &buffer : native_light_unit_buffers) {
		buffer = device->storage_buffer_create(uint32_t(bytes));
		if (buffer.is_valid()) {
			device->buffer_clear(buffer, 0, bytes);
		}
	}
	native_light_state_buffer = device->storage_buffer_create(sizeof(NativeLightStateData) * p_capacity);
	ERR_FAIL_COND(native_light_unit_buffers[0].is_null() || native_light_unit_buffers[1].is_null() || native_light_state_buffer.is_null());
	const Error err = _create_uniform_sets();
	ERR_FAIL_COND(err != OK);
}

void LRTVolume::_begin_native_light_capture_render_thread(int p_slot, int p_target_buffer) {
	ERR_FAIL_INDEX(p_slot, native_light_capacity);
	ERR_FAIL_INDEX(p_target_buffer, 2);
	const size_t receiver_count = local.receivers.size() / 12;
	if (receiver_count == 0 || native_light_unit_buffers[p_target_buffer].is_null()) {
		return;
	}
	const size_t offset = size_t(p_slot) * receiver_count * 4 * sizeof(float);
	const size_t bytes = receiver_count * 4 * sizeof(float);
	device->buffer_clear(native_light_unit_buffers[p_target_buffer], offset, bytes);
}

void LRTVolume::_resolve_native_lights_render_thread() {
	while (true) {
		_update_gpu_timing();
		const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
		std::vector<NativeLightResolve> resolves;
		{
			MutexLock lock(native_resolve_mutex);
			if (pending_native_resolves.empty()) {
				native_resolve_pending.store(false);
				return;
			}
			resolves.swap(pending_native_resolves);
		}
		Vector<RID> uniform_sets;
		const bool timing_active = _begin_gpu_timestamp(GPU_TIMING_LIGHT_RESOLVE, int(resolves.size()));
		RD::ComputeListID list = device->compute_list_begin();
		for (const NativeLightResolve &resolve : resolves) {
			if (resolve.receiver_count <= 0 || resolve.texture.is_null() || resolve.target_buffer < 0 || resolve.target_buffer > 1) {
				continue;
			}
			RenderingServer *rendering_server = RenderingServer::get_singleton();
			const RID texture = rendering_server != nullptr ? rendering_server->texture_get_rd_texture(resolve.texture) : RID();
			if (texture.is_null()) {
				continue;
			}
			Vector<RD::Uniform> uniforms;
			RD::Uniform capture_uniform;
			capture_uniform.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			capture_uniform.binding = 0;
			capture_uniform.append_id(native_light_sampler);
			capture_uniform.append_id(texture);
			uniforms.push_back(capture_uniform);
			for (const Pair<uint32_t, RID> &binding : { Pair<uint32_t, RID>(1, receiver_buffer),
					 Pair<uint32_t, RID>(2, native_light_unit_buffers[resolve.target_buffer]) }) {
				RD::Uniform uniform;
				uniform.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
				uniform.binding = binding.first;
				uniform.append_id(binding.second);
				uniforms.push_back(uniform);
			}
			const RID uniform_set = device->uniform_set_create(uniforms, shader_light_resolve, 0);
			if (uniform_set.is_null()) {
				continue;
			}
			uniform_sets.push_back(uniform_set);
			NativeLightResolvePushConstant push_constant;
			for (int column = 0; column < 3; column++) {
				const Vector3 value = resolve.volume_to_source.basis.get_column(column);
				push_constant.volume_to_source[column * 4 + 0] = value.x;
				push_constant.volume_to_source[column * 4 + 1] = value.y;
				push_constant.volume_to_source[column * 4 + 2] = value.z;
			}
			push_constant.volume_to_source[15] = 1.0f;
			push_constant.volume_to_source[12] = resolve.volume_to_source.origin.x;
			push_constant.volume_to_source[13] = resolve.volume_to_source.origin.y;
			push_constant.volume_to_source[14] = resolve.volume_to_source.origin.z;
			push_constant.ranges[0] = float(resolve.source_range);
			push_constant.ranges[1] = float(resolve.capture_range);
			push_constant.ranges[2] = resolve.area_half_size.x;
			push_constant.ranges[3] = resolve.area_half_size.y;
			push_constant.layout[0] = resolve.receiver_offset;
			push_constant.layout[1] = resolve.receiver_count;
			push_constant.layout[2] = resolve.image_width;
			push_constant.layout[3] = int(local.receivers.size() / 12);
			push_constant.kind[0] = resolve.light_slot;
			push_constant.kind[1] = resolve.directional ? 1 : 0;
			push_constant.kind[2] = resolve.area ? 1 : 0;
			push_constant.kind[3] = resolve.image_height;
			device->compute_list_bind_compute_pipeline(list, pipeline_light_resolve);
			device->compute_list_bind_uniform_set(list, uniform_set, 0);
			device->compute_list_set_push_constant(list, &push_constant, sizeof(push_constant));
			device->compute_list_dispatch(list, Math::division_round_up(uint32_t(resolve.receiver_count), uint32_t(WORKGROUP_SIZE)), 1, 1);
		}
		device->compute_list_end();
		_end_gpu_timestamp(GPU_TIMING_LIGHT_RESOLVE, timing_active);
		gpu_pass_dispatches[GPU_TIMING_LIGHT_RESOLVE].fetch_add(1);
		for (RID uniform_set : uniform_sets) {
			device->free_rid(uniform_set);
		}
		last_render_thread_pass_ms[GPU_TIMING_LIGHT_RESOLVE].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
	}
}

void LRTVolume::_read_receiver_lighting_render_thread() {
	if (receiver_lighting_buffer.is_null()) {
		return;
	}
	const uint32_t byte_count = uint32_t((local.receivers.size() / 12) * 4 * sizeof(float));
	const Vector<uint8_t> data = device->buffer_get_data(receiver_lighting_buffer, 0, byte_count);
	const size_t float_count = data.size() / sizeof(float);
	receiver_lighting.resize(float_count);
	if (float_count > 0) {
		memcpy(receiver_lighting.data(), data.ptr(), float_count * sizeof(float));
	}
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
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_step_render_thread).bind(p_iterations, start_iteration, propagation_sampling, true));
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
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_step_render_thread).bind(p_iterations, start_iteration, propagation_sampling, false));
	const double total_ms = double(OS::get_singleton()->get_ticks_usec() - wait_start) / 1000.0;
	last_cpu_wait_ms.store(0.0);
	last_cpu_submit_ms.store(total_ms);
	iteration += p_iterations;
}

void LRTVolume::_step_render_thread(int p_iterations, int p_start_iteration, int p_sampling, bool p_update_sky_visibility) {
	_update_gpu_timing();
	_upload_params();
	const bool timing_active = _begin_gpu_timestamp(GPU_TIMING_PROPAGATE, p_iterations, uint64_t(p_start_iteration + p_iterations));
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	RD::ComputeListID list = device->compute_list_begin();
	const bool project_changed_sky = p_update_sky_visibility && sky_projection_dirty.exchange(false);
	struct SkyPushConstant {
		int32_t probe_count;
		int32_t pad0;
		int32_t pad1;
		int32_t pad2;
	} sky_push_constant = { grid.count, 0, 0, 0 };
	auto project_sky = [&]() {
		device->compute_list_bind_compute_pipeline(list, pipeline_sky_project);
		device->compute_list_bind_uniform_set(list, uniform_set_sky_project[current], 0);
		device->compute_list_set_push_constant(list, &sky_push_constant, sizeof(SkyPushConstant));
		device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
	};
	if (project_changed_sky) {
		project_sky();
		device->compute_list_add_barrier(list);
		display_sky_dirty = true;
	}
	for (int i = 0; i < p_iterations; i++) {
		struct PushConstant {
			int32_t sampling;
			int32_t iteration;
			int32_t update_sky_visibility;
			int32_t sky_word_start;
		};
		const bool update_sky_visibility = p_update_sky_visibility && sky_visibility_iterations_remaining > 0;
		PushConstant push_constant = { p_sampling, p_start_iteration + i, update_sky_visibility ? 1 : 0, sky_visibility_word_offset };
		device->compute_list_bind_compute_pipeline(list, pipeline_propagate);
		device->compute_list_bind_uniform_set(list, uniform_set_propagate[current], 0);
		device->compute_list_set_push_constant(list, &push_constant, sizeof(PushConstant));
		device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
		current = 1 - current;
		if (update_sky_visibility) {
			sky_visibility_word_offset += SKY_VISIBILITY_WORDS_PER_FRAME;
			if (sky_visibility_word_offset == SKY_DIRECTION_WORDS) {
				sky_visibility_word_offset = 0;
				sky_visibility_iterations_remaining--;
				device->compute_list_add_barrier(list);
				project_sky();
				display_sky_dirty = true;
			}
		}
		if (i + 1 < p_iterations) {
			device->compute_list_add_barrier(list);
		}
	}
	device->compute_list_end();
	_end_gpu_timestamp(GPU_TIMING_PROPAGATE, timing_active);
	gpu_pass_dispatches[GPU_TIMING_PROPAGATE].fetch_add(1);
	_sync_display();
	const double elapsed_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
	last_cpu_submit_ms.store(elapsed_ms);
	last_render_thread_pass_ms[GPU_TIMING_PROPAGATE].store(elapsed_ms);
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
			.bind(p_iterations, start_iteration, propagation_sampling, true));
	// The main RenderingDevice is submitted by RenderingServer, so calling RenderingDevice::sync()
	// on it is invalid. Force one diagnostic frame, then query the timestamps captured around the
	// propagation dispatch from the render thread. Production never enters this blocking path.
	rendering_server->draw(false, 0.0);
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_update_gpu_timing));
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
	const size_t bytes = size_t(grid.count) * 4 * sizeof(float);
	const size_t directional_bytes = size_t(grid.count) * SKY_DIRECTION_WORDS * sizeof(uint32_t);
	for (int buffer = 0; buffer < 2; buffer++) {
		for (int channel = 0; channel < 3; channel++) {
			device->buffer_clear(radiance_buffers[buffer][channel], 0, bytes);
		}
		device->buffer_clear(directional_visibility_buffers[buffer], 0, directional_bytes);
		device->buffer_clear(visibility_buffers[buffer], 0, bytes);
	}
	for (int channel = 0; channel < 3; channel++) {
		device->buffer_clear(sky_buffers[channel], 0, bytes);
	}
	for (int phase = 0; phase < 3; phase++) {
		for (int channel = 0; channel < 3; channel++) {
			device->buffer_clear(radiance_phase_buffers[phase][channel], 0, bytes);
		}
	}
	current = 0;
	// Each visibility round advances eight monotonic lattice steps. The L1 grid extent is a
	// conservative exact bound for every digital sky path; one extra round makes both A/B buffers
	// complete before the cheap copy-only steady state begins.
	sky_visibility_iterations_remaining =
			(grid.size[0] + grid.size[1] + grid.size[2] + SKY_PATH_LENGTH - 1) / SKY_PATH_LENGTH + 1;
	sky_visibility_word_offset = 0;
	sky_projection_dirty.store(false);
	_sync_display();
}

void LRTVolume::_sync_display() {
	const uint64_t cpu_start = OS::get_singleton()->get_ticks_usec();
	const uint64_t batch_version = display_version.fetch_add(1) + 1;
	const bool timing_active = _begin_gpu_timestamp(GPU_TIMING_DISPLAY, 1, batch_version);
	enum DisplayWriteMask {
		DISPLAY_WRITE_RADIANCE = 1,
		DISPLAY_WRITE_SOURCE = 2,
		DISPLAY_WRITE_SKY = 4,
		DISPLAY_WRITE_VISIBILITY = 8,
	};
	uint32_t write_mask = DISPLAY_WRITE_RADIANCE;
	if (local_debug_textures_enabled.load()) {
		write_mask |= DISPLAY_WRITE_VISIBILITY;
		display_visibility_bytes.fetch_add(uint64_t(grid.count) * 4 * sizeof(float));
	}
	if (local_debug_textures_enabled.load() && display_source_dirty) {
		write_mask |= DISPLAY_WRITE_SOURCE;
		display_source_dirty = false;
		display_source_bytes.fetch_add(uint64_t(grid.count) * 3 * 4 * sizeof(float));
	}
	if (display_sky_dirty) {
		write_mask |= DISPLAY_WRITE_SKY;
		display_sky_dirty = false;
		display_sky_bytes.fetch_add(uint64_t(grid.count) * 3 * 4 * sizeof(float));
	}
	display_radiance_bytes.fetch_add(uint64_t(grid.count) * 3 * 4 * sizeof(float));
	struct DisplayPushConstant {
		uint32_t write_mask;
		uint32_t padding[3];
	} push_constant = { write_mask, {} };
	RD::ComputeListID list = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(list, pipeline_display);
	device->compute_list_bind_uniform_set(list, uniform_set_display[current], 0);
	device->compute_list_set_push_constant(list, &push_constant, sizeof(push_constant));
	device->compute_list_dispatch(list, Math::division_round_up(uint32_t(grid.count), uint32_t(WORKGROUP_SIZE)), 1, 1);
	device->compute_list_end();
	_end_gpu_timestamp(GPU_TIMING_DISPLAY, timing_active);
	gpu_pass_dispatches[GPU_TIMING_DISPLAY].fetch_add(1);
	last_render_thread_pass_ms[GPU_TIMING_DISPLAY].store(double(OS::get_singleton()->get_ticks_usec() - cpu_start) / 1000.0);
}

void LRTVolume::_update_gpu_timing() {
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
			// missing range must not permanently disable production budget feedback.
			gpu_timestamp_pending[pass] = false;
			dropped_gpu_timestamp_ranges[pass].fetch_add(1);
		}
	}
	if (propagation_updated) {
		last_gpu_ms.store(totals_ms[GPU_TIMING_PROPAGATE]);
	}
}

bool LRTVolume::_begin_gpu_timestamp(GpuTimingPass p_pass, int p_work_items, uint64_t p_batch_version) {
	latest_gpu_pass_batch_version[p_pass].store(p_batch_version);
	latest_gpu_pass_local_version[p_pass].store(local_field_version.load());
	latest_gpu_pass_source_version[p_pass].store(source_version.load());
	latest_gpu_pass_submission_frame[p_pass].store(Engine::get_singleton()->get_frames_drawn());
	// Propagation timing is production scheduler feedback. The other ranges remain optional
	// diagnostics so enabling the benchmark cannot change the amount of propagation work.
	if ((p_pass != GPU_TIMING_PROPAGATE && !lrt_gpu_profiling_enabled.load()) || gpu_timestamp_pending[p_pass]) {
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

Dictionary LRTVolume::get_external_gi_buffers() const {
	Dictionary result;
	result["r"] = external_gi_buffers[0];
	result["g"] = external_gi_buffers[1];
	result["b"] = external_gi_buffers[2];
	return result;
}

Dictionary LRTVolume::get_debug_resources() const {
	Dictionary result;
	result["receiver_buffer"] = receiver_buffer;
	result["receiver_count"] = int(local.receivers.size() / 12);
	return result;
}

void LRTVolume::_read_back_render_thread() {
	readback_error = OK;
	const size_t bytes = size_t(grid.count) * 4 * sizeof(float);
	for (int channel = 0; channel < 3; channel++) {
		const Vector<uint8_t> data = device->texture_get_data(field_texture_rids[channel], 0);
		if (int64_t(data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		radiance_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(radiance_cpu[channel].data(), data.ptr(), bytes);
		const Vector<uint8_t> sky_data = device->texture_get_data(sky_texture_rids[channel], 0);
		if (int64_t(sky_data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		sky_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(sky_cpu[channel].data(), sky_data.ptr(), bytes);
	}
	const Vector<uint8_t> visibility_data = device->buffer_get_data(visibility_buffers[current]);
	if (int64_t(visibility_data.size()) != int64_t(bytes)) {
		readback_error = ERR_CANT_ACQUIRE_RESOURCE;
		return;
	}
	visibility_cpu.resize(size_t(grid.count) * 4);
	memcpy(visibility_cpu.data(), visibility_data.ptr(), bytes);
	for (int channel = 0; channel < 3; channel++) {
		const Vector<uint8_t> data = device->buffer_get_data(source_buffers[channel]);
		if (int64_t(data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		source_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(source_cpu[channel].data(), data.ptr(), bytes);
		const Vector<uint8_t> external_data = device->buffer_get_data(external_gi_buffers[channel]);
		if (int64_t(external_data.size()) != int64_t(bytes)) {
			readback_error = ERR_CANT_ACQUIRE_RESOURCE;
			return;
		}
		external_gi_cpu[channel].resize(size_t(grid.count) * 4);
		memcpy(external_gi_cpu[channel].data(), external_data.ptr(), bytes);
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
	if (p_name == "radiance_r") {
		return field_textures[0];
	}
	if (p_name == "radiance_g") {
		return field_textures[1];
	}
	if (p_name == "radiance_b") {
		return field_textures[2];
	}
	if (p_name == "sky_r") {
		return sky_textures[0];
	}
	if (p_name == "sky_g") {
		return sky_textures[1];
	}
	if (p_name == "sky_b") {
		return sky_textures[2];
	}
	if (p_name == "visibility") {
		return visibility_texture;
	}
	if (p_name == "source_r") {
		return source_textures[0];
	}
	if (p_name == "source_g") {
		return source_textures[1];
	}
	if (p_name == "source_b") {
		return source_textures[2];
	}
	if (p_name == "material") {
		return material_texture;
	}
	if (p_name == "matrices") {
		return matrix_texture;
	}
	if (p_name == "local_visibility") {
		return local_visibility_texture;
	}
	if (p_name == "links") {
		return links_texture;
	}
	if (p_name == "receiver_links") {
		return receiver_links_texture;
	}
	if (p_name == "diagnostic_sdf") {
		return diagnostic_sdf_texture;
	}
	if (p_name == "diagnostic_albedo") {
		return diagnostic_albedo_texture;
	}
	if (p_name == "diagnostic_emission") {
		return diagnostic_emission_texture;
	}
	if (p_name == "diagnostic_dirty") {
		return diagnostic_dirty_texture;
	}
	return Ref<Texture2D>();
}

PackedFloat32Array LRTVolume::read_field(const String &p_name) const {
	if (p_name == "material_canonical") {
		PackedFloat32Array result;
		result.resize(int64_t(local.material.size()));
		uint32_t receiver_vector_offset = 0;
		for (int probe = 0; probe < grid.count; probe++) {
			const size_t source = size_t(probe) * 4;
			for (int component = 0; component < 4; component++) {
				result.set(int64_t(source + component), local.material[source + component]);
			}
			result.set(int64_t(source), float(receiver_vector_offset));
			receiver_vector_offset += uint32_t(local.material[source + 1]) * 3;
		}
		return result;
	}
	if (p_name == "receivers_compact" || p_name == "receiver_emission_compact") {
		const int stride = p_name == "receivers_compact" ? 12 : 4;
		uint64_t active_count = 0;
		for (int probe = 0; probe < grid.count; probe++) {
			active_count += uint64_t(local.material[size_t(probe) * 4 + 1]);
		}
		PackedFloat32Array result;
		result.resize(int64_t(active_count * stride));
		int64_t target = 0;
		for (int probe = 0; probe < grid.count; probe++) {
			const size_t material_index = size_t(probe) * 4;
			const uint32_t count = uint32_t(local.material[material_index + 1]);
			const uint32_t receiver_start = uint32_t(local.material[material_index]) / 3;
			const std::vector<float> &source_values = p_name == "receivers_compact" ? local.receivers : local.receiver_emission;
			for (uint32_t receiver = 0; receiver < count; receiver++) {
				for (int component = 0; component < stride; component++) {
					result.set(target++, source_values[(size_t(receiver_start + receiver) * stride) + component]);
				}
			}
		}
		return result;
	}
	const std::vector<float> *values = nullptr;
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
	} else if (p_name == "external_r") {
		values = &external_gi_cpu[0];
	} else if (p_name == "external_g") {
		values = &external_gi_cpu[1];
	} else if (p_name == "external_b") {
		values = &external_gi_cpu[2];
	} else if (p_name == "material") {
		values = &local.material;
	} else if (p_name == "matrices") {
		values = &local.matrices;
	} else if (p_name == "local_visibility") {
		values = &local.local_visibility;
	} else if (p_name == "receivers") {
		values = &local.receivers;
	} else if (p_name == "receiver_emission") {
		values = &local.receiver_emission;
	} else if (p_name == "diagnostic_sdf") {
		values = &local.diagnostic_sdf;
	} else if (p_name == "diagnostic_albedo") {
		values = &local.diagnostic_albedo;
	} else if (p_name == "diagnostic_emission") {
		values = &local.diagnostic_emission;
	} else if (p_name == "diagnostic_dirty") {
		values = &local.diagnostic_dirty;
	}
	PackedFloat32Array result;
	if (!values || values->empty()) {
		return result;
	}
	result.resize(values->size());
	memcpy(result.ptrw(), values->data(), values->size() * sizeof(float));
	return result;
}

PackedInt32Array LRTVolume::read_links() const {
	PackedInt32Array result;
	if (local.links.empty()) {
		return result;
	}
	result.resize(local.links.size());
	int32_t *write = result.ptrw();
	for (size_t i = 0; i < local.links.size(); i++) {
		write[i] = int32_t(local.links[i]);
	}
	return result;
}

PackedInt32Array LRTVolume::read_receiver_links() const {
	PackedInt32Array result;
	if (local.receiver_links.empty()) {
		return result;
	}
	result.resize(local.receiver_links.size());
	int32_t *write = result.ptrw();
	for (size_t i = 0; i < local.receiver_links.size(); i++) {
		write[i] = int32_t(local.receiver_links[i]);
	}
	return result;
}

Dictionary LRTVolume::_make_receiver_capture_data(const lrt::LocalField &p_local) {
	Dictionary result;
	const size_t receiver_count = p_local.receivers.size() / 12;
	PackedVector3Array positions;
	PackedVector3Array normals;
	PackedVector3Array surface_normals;
	PackedInt32Array directions;
	PackedInt32Array layer_masks;
	positions.resize(receiver_count);
	normals.resize(receiver_count);
	surface_normals.resize(receiver_count);
	directions.resize(receiver_count);
	layer_masks.resize(receiver_count);
	for (size_t i = 0; i < receiver_count; i++) {
		const float *receiver = p_local.receivers.data() + i * 12;
		positions.set(int64_t(i), Vector3(receiver[0], receiver[1], receiver[2]));
		const int direction_index = int(receiver[3]);
		const lrt::Direction &direction = lrt::directions()[direction_index];
		normals.set(int64_t(i), -Vector3(direction.direction.x, direction.direction.y, direction.direction.z));
		surface_normals.set(int64_t(i), Vector3(receiver[4], receiver[5], receiver[6]));
		directions.set(int64_t(i), direction_index);
		uint32_t layer_mask;
		memcpy(&layer_mask, receiver + 7, sizeof(layer_mask));
		layer_masks.set(int64_t(i), int32_t(layer_mask));
	}
	result["positions"] = positions;
	result["normals"] = normals;
	result["surface_normals"] = surface_normals;
	result["directions"] = directions;
	result["layer_masks"] = layer_masks;
	return result;
}

uint64_t LRTVolume::_receiver_capture_data_bytes(const Dictionary &p_capture_data) {
	const PackedVector3Array positions = p_capture_data.get("positions", PackedVector3Array());
	return uint64_t(positions.size()) * (3 * sizeof(Vector3) + 2 * sizeof(int32_t));
}

Dictionary LRTVolume::get_receiver_capture_data() const {
	if (!receiver_capture_data_dirty) {
		return receiver_capture_data_cache;
	}
	receiver_capture_data_cache = _make_receiver_capture_data(local);
	receiver_capture_data_dirty = false;
	return receiver_capture_data_cache;
}

Dictionary LRTVolume::get_staged_receiver_capture_data() const {
	return staged_receiver_capture_data;
}

Dictionary LRTVolume::sample_geometry(const Vector3 &p_point) const {
	Dictionary result;
	bool found = false;
	lrt::ColorSdfSample nearest;
	const lrt::Vec3 point(p_point.x, p_point.y, p_point.z);
	for (const lrt::SdfPrimitive &primitive : primitives) {
		const lrt::ColorSdfSample sample = primitive.sample(point);
		if (!sample.valid || (found && sample.distance >= nearest.distance)) {
			continue;
		}
		nearest = sample;
		found = true;
	}
	if (!found) {
		return result;
	}
	result["distance"] = nearest.distance;
	result["normal"] = Vector3(nearest.normal.x, nearest.normal.y, nearest.normal.z);
	result["albedo"] = Vector3(nearest.color.x, nearest.color.y, nearest.color.z);
	result["emission"] = Vector3(nearest.emission.x, nearest.emission.y, nearest.emission.z);
	return result;
}

void LRTVolume::clear_shared_sdf_cache() {
	lrt::clear_shared_asset_fields();
}

void LRTVolume::clear_shared_primitive_ltm_cache() {
	lrt::clear_shared_primitive_ltm_cache();
}

Dictionary LRTVolume::get_stats() const {
	Dictionary result;
	result["iteration"] = iteration;
	result["propagation_passes"] = iteration;
	result["dither_phase"] = propagation_sampling == 1 ? iteration % 3 : 0;
	result["completed_dither_cycles"] = propagation_sampling == 1 ? iteration / 3 : 0;
	result["pending_step_iterations"] = pending_step_iterations.load();
	result["injection_pending"] = injection_pending.load();
	result["propagation_sampling"] = propagation_sampling;
	result["last_gpu_ms"] = last_gpu_ms.load();
	result["last_gpu_work_items"] = last_gpu_pass_work_items[GPU_TIMING_PROPAGATE].load();
	result["last_gpu_batch_version"] = int64_t(last_gpu_pass_batch_version[GPU_TIMING_PROPAGATE].load());
	result["last_cpu_submit_ms"] = last_cpu_submit_ms.load();
	result["last_cpu_wait_ms"] = last_cpu_wait_ms.load();
	result["last_readback_ms"] = last_readback_ms;
	result["diagnostic_readbacks"] = diagnostic_readbacks;
	result["rendering_device"] = "main";
	result["solid"] = local.solid_count;
	result["surface"] = local.surface_count;
	result["count"] = grid.count;
	result["matrix_encoding"] = "luminance_4x4_rgb_tint_f32";
	result["matrix_compression_relative_rmse"] = local.matrix_compression_relative_rmse;
	result["matrix_compression_max_abs"] = local.matrix_compression_max_abs;
	result["primitive_ltm_cache_hits"] = local.primitive_ltm_cache_hits;
	result["primitive_ltm_cache_misses"] = local.primitive_ltm_cache_misses;
	result["primitive_ltm_overlap_fallbacks"] = local.primitive_ltm_overlap_fallbacks;
	result["primitive_ltm_cache_bytes"] = int64_t(lrt::shared_primitive_ltm_cache_bytes());
	result["primitive_ltm_cache_entries"] = int64_t(lrt::shared_primitive_ltm_cache_entries());
	result["backend"] = local_backend;
	result["textures_ready"] = bool(field_textures[0].is_valid());
	result["gpu_bytes"] = _gpu_bytes();
	return result;
}

Dictionary LRTVolume::get_performance_stats() const {
	Dictionary result;
	static const char *pass_keys[GPU_TIMING_PASS_COUNT] = { "inject", "light_resolve", "propagate", "display" };
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
	result["display_version"] = int64_t(display_version.load());
	result["display_radiance_bytes"] = int64_t(display_radiance_bytes.load());
	result["display_visibility_bytes"] = int64_t(display_visibility_bytes.load());
	result["display_source_bytes"] = int64_t(display_source_bytes.load());
	result["display_sky_bytes"] = int64_t(display_sky_bytes.load());
	result["local_patch_upload_bytes"] = int64_t(apply_local_patch_upload_bytes);
	result["receiver_patch_upload_bytes"] = int64_t(apply_receiver_patch_upload_bytes);
	result["receiver_copy_bytes"] = int64_t(apply_receiver_copy_bytes);
	result["full_upload_bytes"] = int64_t(apply_full_upload_bytes);
	result["receiver_layout_full_rebuilds"] = int64_t(receiver_layout_full_rebuilds);
	result["receiver_layout_compactions"] = int64_t(receiver_layout_compactions);
	result["propagation_passes"] = iteration;
	result["dither_phase"] = propagation_sampling == 1 ? iteration % 3 : 0;
	result["completed_dither_cycles"] = propagation_sampling == 1 ? iteration / 3 : 0;
	result["last_cpu_submit_ms"] = last_cpu_submit_ms.load();
	result["last_cpu_wait_ms"] = last_cpu_wait_ms.load();
	result["diagnostic_readback_ms"] = last_readback_ms;
	result["diagnostic_readbacks"] = diagnostic_readbacks;
	result["gpu_timestamp_scope"] = "most_recent_completed_render_frame";
	result["gpu_timestamps_enabled"] = lrt_gpu_profiling_enabled.load();
	result["scheduler_propagation_timestamp_enabled"] = true;
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
	rendering_server->call_on_render_thread(callable_mp(this, &LRTVolume::_update_gpu_timing));
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
	const int phase = preparation_phase.load();
	static const char *phase_names[] = { "idle", "assets", "local", "visibility", "display", "ready", "failed" };
	result["phase"] = phase >= 0 && phase < 7 ? phase_names[phase] : "unknown";
	result["completed"] = preparation_completed.load();
	result["total"] = preparation_total.load();
	result["error"] = preparation_error;
	return result;
}
