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

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Geometry {
	uint words[];
}
geometry;

layout(set = 0, binding = 1, std430) restrict readonly buffer Emission {
	vec4 coefficients[];
}
emission;

struct ProbeState {
	vec4 light[3];
	vec4 visibility;
};

layout(set = 0, binding = 2, std430) restrict readonly buffer Source {
	ProbeState probes[];
}
source;

layout(set = 0, binding = 3, std430) restrict writeonly buffer Destination {
	ProbeState probes[];
}
destination;

#define LRT_DATA_SET 0
#define LRT_METADATA_BINDING 4
#define LRT_SAMPLES_BINDING 5
#include "lrt_data_inc.glsl"
#include "lrt_sh_inc.glsl"

layout(push_constant, std430) uniform Params {
	uvec3 grid;
	uint offset;
	uint end;
	uint phase; // 0: A, 1: L.
	float relaxation;
	uint volume_index;
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
					for (uint c = 0; c < 4; c++) {
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

void main() {
	uint probe = params.offset + gl_GlobalInvocationID.x;
	if (probe >= params.end) {
		return;
	}
	uint base = probe * 18;
	uint color_flags = geometry.words[base + 8];
	if ((color_flags & 0x01000000u) != 0) {
		for (uint channel = 0; channel < 3; channel++) {
			destination.probes[probe].light[channel] = vec4(0.0);
		}
		destination.probes[probe].visibility = vec4(0.0);
		return;
	}

	uvec3 position = uvec3(probe % params.grid.x, (probe / params.grid.x) % params.grid.y, probe / (params.grid.x * params.grid.y));
	vec4 aggregate[3];
	for (uint channel = 0; channel < 3; channel++) {
		aggregate[channel] = vec4(0.0);
	}
	uint neighbor = 0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
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
				ivec3 q = ivec3(position) + delta;
				bool in_grid = all(greaterThanEqual(q, ivec3(0))) && all(lessThan(q, ivec3(params.grid)));
				uint adjacent = in_grid ? (uint(q.z) * params.grid.y + uint(q.y)) * params.grid.x + uint(q.x) : 0;
				vec4 outer_light[3];
				vec4 outer_visibility;
				if (!in_grid) {
					boundary(q, outer_light, outer_visibility);
				}
				if (params.phase == 0) {
					kernel.yzw = -kernel.yzw;
					vec4 visibility = in_grid ? source.probes[adjacent].visibility : outer_visibility;
					aggregate[0] += edge * lrt_constrain_visibility(lrt_product(kernel, lrt_constrain_visibility(visibility)));
				} else {
					for (uint channel = 0; channel < 3; channel++) {
						vec4 incoming = in_grid ? source.probes[adjacent].light[channel] : outer_light[channel];
						aggregate[channel] += edge * lrt_constrain_light(lrt_product(kernel, lrt_constrain_light(incoming)));
					}
				}
			}
		}
	}
	vec4 local_vis = vec4(unpackHalf2x16(geometry.words[base + 9]), unpackHalf2x16(geometry.words[base + 10]));
	if (params.phase == 0) {
		vec4 visibility = lrt_constrain_visibility(lrt_product(lrt_constrain_visibility(local_vis), lrt_constrain_visibility(aggregate[0])));
		destination.probes[probe].visibility = lrt_constrain_visibility(mix(source.probes[probe].visibility, visibility, params.relaxation));
		for (uint channel = 0; channel < 3; channel++) {
			destination.probes[probe].light[channel] = source.probes[probe].light[channel];
		}
		return;
	}
	mat4 transfer;
	for (uint column = 0; column < 4; column++) {
		transfer[column] = vec4(unpackHalf2x16(geometry.words[base + column * 2]), unpackHalf2x16(geometry.words[base + column * 2 + 1]));
	}
	vec3 color = vec3(float(color_flags & 255u), float((color_flags >> 8) & 255u), float((color_flags >> 16) & 255u)) / 255.0;
	for (uint channel = 0; channel < 3; channel++) {
		vec4 transmitted, reflected;
		lrt_transport(color[channel] * transfer, local_vis, aggregate[channel], transmitted, reflected);
		vec4 raw_light = lrt_constrain_light(emission.coefficients[probe * 3 + channel]) + transmitted + reflected;
		destination.probes[probe].light[channel] = lrt_constrain_light(mix(source.probes[probe].light[channel], raw_light, params.relaxation));
	}
	destination.probes[probe].visibility = source.probes[probe].visibility;
}
