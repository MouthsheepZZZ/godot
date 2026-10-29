/**************************************************************************/
/*  lrt_propagate.glsl                                                         */
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

#[compute]

#version 450

#VERSION_DEFINES
#extension GL_EXT_control_flow_attributes : require

#ifdef PHASE_A
const uint LRT_PHASE = 0u;
#else
const uint LRT_PHASE = 1u;
#endif

layout(local_size_x = 8, local_size_y = 4, local_size_z = LRT_WORKGROUP_DEPTH) in;

const uvec3 LRT_HALO_SIZE = gl_WorkGroupSize + uvec3(2);
const uint LRT_HALO_PLANE = LRT_HALO_SIZE.x * LRT_HALO_SIZE.y;
const uint LRT_HALO_COUNT = LRT_HALO_PLANE * LRT_HALO_SIZE.z;
const uint LRT_GROUP_SIZE = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z;
#ifdef PHASE_A
shared vec4 tile_visibility[LRT_HALO_COUNT];
#else
shared vec4 tile_light[3 * LRT_HALO_COUNT];
#endif

layout(set = 0, binding = 0, std430) restrict readonly buffer Geometry {
	uint words[];
}
geometry;

layout(set = 0, binding = 1, std430) restrict readonly buffer Emission {
	vec4 coefficients[];
}
emission;

// Both bindings are read/write so ping-pong iterations share one compute list.
// Only the current phase's planes are written; the other field remains intact.
layout(set = 0, binding = 2, std430) buffer Source {
	vec4 sh[];
}
source;

layout(set = 0, binding = 3, std430) buffer Destination {
	vec4 sh[];
}
destination;

#define LRT_SAMPLES_WRITABLE
#define LRT_DATA_SET 0
#define LRT_METADATA_BINDING 4
#define LRT_SAMPLES_BINDING 5
#include "lrt_data_inc.glsl"
#include "lrt_sh_inc.glsl"

layout(push_constant, std430) uniform Params {
	uvec3 grid;
	uint offset; // First workgroup in the flattened tile grid.
	uint end;
	uint other_in_source; // The other field occupies the source or destination history.
	uint volume_index;
	uint publish_samples;
	mat4 to_world;
	vec4 size_spacing;
}
params;

// Boundary interpolation includes Inside zero states and ignores display parameters.
void boundary(ivec3 coordinate, out vec4 light[3], out vec4 visibility) {
	for (uint c = 0; c < 3; c++) {
		light[c] = vec4(0.0);
	}
	visibility = vec4(1.0 / LRT_C0, 0.0, 0.0, 0.0);
	vec3 local = vec3(coordinate) * params.size_spacing.w - params.size_spacing.xyz * 0.5;
	vec3 world = (params.to_world * vec4(local, 1.0)).xyz;
	for (int index = int(params.volume_index) - 1; index >= 0; index--) {
		LRTVolume volume = lrt.volumes[index];
		if (int(volume.history.y) >= int(lrt.volumes[params.volume_index].history.y)) {
			continue;
		}
		vec3 p = (volume.to_local * vec4(world, 1.0)).xyz;
		if (any(greaterThan(abs(p), volume.size_spacing.xyz * 0.5))) {
			continue;
		}
		vec3 grid_position = (p + volume.size_spacing.xyz * 0.5) / volume.size_spacing.w;
		ivec3 cell = clamp(ivec3(floor(grid_position)), ivec3(0), ivec3(volume.grid_geometry.xyz) - 2);
		vec3 f = clamp(grid_position - vec3(cell), vec3(0.0), vec3(1.0));
		visibility = vec4(0.0);
		for (int z = 0; z < 2; z++) {
			for (int y = 0; y < 2; y++) {
				for (int x = 0; x < 2; x++) {
					uvec3 q = uvec3(cell + ivec3(x, y, z));
					uint probe = (q.z * volume.grid_geometry.y + q.y) * volume.grid_geometry.x + q.x;
					vec3 weights = mix(vec3(1.0) - f, f, vec3(x, y, z));
					float weight = weights.x * weights.y * weights.z;
					for (uint c = (LRT_PHASE == 0u ? 3u : 0u); c < (LRT_PHASE == 0u ? 4u : 3u); c++) {
						uint offset = volume.history.x + probe * 16 + c * 4;
						vec4 sh = uintBitsToFloat(uvec4(lrt_samples.words[offset], lrt_samples.words[offset + 1], lrt_samples.words[offset + 2], lrt_samples.words[offset + 3]));
						if (c < 3) {
							light[c] += sh * weight;
						} else {
							visibility += sh * weight;
						}
					}
				}
			}
		}
		mat3 world_to_source = mat3(normalize(volume.to_local[0].xyz), normalize(volume.to_local[1].xyz), normalize(volume.to_local[2].xyz));
		mat3 current_to_world = mat3(normalize(params.to_world[0].xyz), normalize(params.to_world[1].xyz), normalize(params.to_world[2].xyz));
		mat3 rotation = transpose(current_to_world) * transpose(world_to_source);
		for (uint c = 0; c < 3; c++) {
			light[c].yzw = rotation * light[c].yzw;
		}
		visibility.yzw = rotation * visibility.yzw;
		return;
	}
}

void publish_sh(uint probe, uint channel, vec4 sh) {
	if (params.publish_samples == 0u) {
		return;
	}
	uint offset = lrt.volumes[params.volume_index].history.x + probe * 16 + channel * 4;
	uvec4 words = floatBitsToUint(sh);
	for (uint i = 0; i < 4; i++) {
		lrt_samples.words[offset + i] = words[i];
	}
}

void main() {
	uint tile = params.offset + gl_WorkGroupID.x;
	if (tile >= params.end) {
		return;
	}
	uint count = params.grid.x * params.grid.y * params.grid.z;
	uvec3 tile_grid = (params.grid + gl_WorkGroupSize - uvec3(1)) / gl_WorkGroupSize;
	uvec3 tile_position = uvec3(tile % tile_grid.x, (tile / tile_grid.x) % tile_grid.y, tile / (tile_grid.x * tile_grid.y)) * gl_WorkGroupSize;
	// All lanes, including padded and Inside probes, participate before the barrier.
	for (uint index = gl_LocalInvocationIndex; index < LRT_HALO_COUNT; index += LRT_GROUP_SIZE) {
		ivec3 q = ivec3(tile_position) + ivec3(index % LRT_HALO_SIZE.x, (index / LRT_HALO_SIZE.x) % LRT_HALO_SIZE.y, index / LRT_HALO_PLANE) - ivec3(1);
		bool in_grid = all(greaterThanEqual(q, ivec3(0))) && all(lessThan(q, ivec3(params.grid)));
		vec4 light[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));
		vec4 visibility = vec4(0.0);
		if (in_grid) {
			uint adjacent = (uint(q.z) * params.grid.y + uint(q.y)) * params.grid.x + uint(q.x);
#ifdef PHASE_A
			visibility = source.sh[3 * count + adjacent];
#else
			[[unroll]] for (uint channel = 0; channel < 3; channel++) {
				light[channel] = source.sh[channel * count + adjacent];
			}
#endif
		} else if (all(lessThanEqual(q, ivec3(params.grid)))) {
			boundary(q, light, visibility);
		}
#ifdef PHASE_A
		tile_visibility[index] = lrt_constrain_visibility(visibility);
#else
		[[unroll]] for (uint channel = 0; channel < 3; channel++) {
			tile_light[channel * LRT_HALO_COUNT + index] = light[channel];
		}
#endif
	}
	barrier();
	uvec3 position = tile_position + gl_LocalInvocationID;
	if (any(greaterThanEqual(position, params.grid))) {
		return;
	}
	uint probe = (position.z * params.grid.y + position.y) * params.grid.x + position.x;
	uint center = ((gl_LocalInvocationID.z + 1) * LRT_HALO_SIZE.y + gl_LocalInvocationID.y + 1) * LRT_HALO_SIZE.x + gl_LocalInvocationID.x + 1;
	uint base = probe * 18;
	uint color_flags = geometry.words[base + 8];
	if (params.publish_samples != 0u) {
		lrt_samples.words[lrt.volumes[params.volume_index].grid_geometry.w + probe] = color_flags;
		// Refresh both fields even when sampling storage was rebuilt or geometry changed.
		for (uint channel = (LRT_PHASE == 0u ? 0u : 3u); channel < (LRT_PHASE == 0u ? 3u : 4u); channel++) {
			uint index = channel * count + probe;
			publish_sh(probe, channel, params.other_in_source != 0u ? source.sh[index] : destination.sh[index]);
		}
	}
	if ((color_flags & 0x01000000u) != 0) {
		if (LRT_PHASE == 0u) {
			destination.sh[3 * count + probe] = vec4(0.0);
			publish_sh(probe, 3u, vec4(0.0));
		} else {
			for (uint channel = 0; channel < 3; channel++) {
				destination.sh[channel * count + probe] = vec4(0.0);
				publish_sh(probe, channel, vec4(0.0));
			}
		}
		return;
	}

	vec4 aggregate[3];
	for (uint channel = 0; channel < 3; channel++) {
		aggregate[channel] = vec4(0.0);
	}
	vec4 open_transport = vec4(0.0);
	uint neighbor = 0;
	[[unroll]] for (int z = -1; z <= 1; z++) {
		[[unroll]] for (int y = -1; y <= 1; y++) {
			[[unroll]] for (int x = -1; x <= 1; x++) {
				ivec3 delta = ivec3(x, y, z);
				if (all(equal(delta, ivec3(0)))) {
					continue;
				}
				uint mask_word = geometry.words[base + 11 + neighbor / 4];
				uint mask = (mask_word >> ((neighbor % 4) * 8)) & 15u;
				neighbor++;
				float edge = float(bitCount(mask)) * 0.25;
				if (edge == 0.0) {
					continue;
				}
				vec3 direction = normalize(vec3(delta));
				vec4 kernel = vec4(1.0 / (26.0 * LRT_C0), -direction / (26.0 * LRT_C1));
				uint adjacent = uint(int(center) + z * int(LRT_HALO_PLANE) + y * int(LRT_HALO_SIZE.x) + x);
				if (LRT_PHASE == 0u) {
					kernel.yzw = -kernel.yzw;
#ifdef PHASE_A
					aggregate[0] += edge * lrt_constrain_visibility(lrt_product(kernel, tile_visibility[adjacent]));
#endif
				} else {
					open_transport += edge * kernel;
#ifdef PHASE_L
					[[unroll]] for (uint channel = 0; channel < 3; channel++) {
						aggregate[channel] += edge * lrt_product(kernel, tile_light[channel * LRT_HALO_COUNT + adjacent]);
					}
#endif
				}
			}
		}
	}
	vec4 local_vis = vec4(unpackHalf2x16(geometry.words[base + 9]), unpackHalf2x16(geometry.words[base + 10]));
	if (LRT_PHASE == 0u) {
		vec4 visibility = lrt_constrain_visibility(lrt_product(lrt_constrain_visibility(local_vis), lrt_constrain_visibility(aggregate[0])));
		destination.sh[3 * count + probe] = visibility;
		publish_sh(probe, 3u, visibility);
		return;
	}
#ifdef PHASE_L
	mat4 transfer;
	[[unroll]] for (uint column = 0; column < 4; column++) {
		transfer[column] = vec4(unpackHalf2x16(geometry.words[base + column * 2]), unpackHalf2x16(geometry.words[base + column * 2 + 1]));
	}
	vec3 color = vec3(float(color_flags & 255u), float((color_flags >> 8) & 255u), float((color_flags >> 16) & 255u)) / 255.0;
	[[unroll]] for (uint channel = 0; channel < 3; channel++) {
		// Match the reflected outgoing moments to K without scattering free-space transport.
		vec4 reflected = lrt_filter_radiance(color[channel] * transfer * tile_light[channel * LRT_HALO_COUNT + center]);
		// Bound the response to unit radiance, reconciling center-ray T with four-ray Edge.
		// This geometry-only bound never increases transport or scales the injected source.
		vec4 unit_response = open_transport + lrt_filter_radiance(color[channel] * transfer[0] / LRT_C0);
		float peak_response = LRT_C0 * unit_response.x + (2.0 / 3.0) * LRT_C1 * length(unit_response.yzw);
		float transport_scale = 1.0 / max(1.0, peak_response);
		vec4 raw_light = lrt_constrain_diffuse(emission.coefficients[probe * 3 + channel] + transport_scale * (aggregate[channel] + reflected));
		destination.sh[channel * count + probe] = raw_light;
		publish_sh(probe, channel, raw_light);
	}
#endif
}
