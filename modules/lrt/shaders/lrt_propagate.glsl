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

layout(push_constant, std430) uniform Params {
	uvec3 grid;
	uint offset;
	uint end;
	uint phase; // 0: A, 1: L.
	float relaxation;
	uint padding;
}
params;

const float C0 = 0.28209479177387814;
const float C1 = 0.4886025119029199;

vec4 sh_product(vec4 a, vec4 b) {
	return C0 * vec4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
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
				vec4 kernel = vec4(1.0 / (26.0 * C0), -direction / (26.0 * C1));
				ivec3 q = ivec3(position) + delta;
				bool in_grid = all(greaterThanEqual(q, ivec3(0))) && all(lessThan(q, ivec3(params.grid)));
				uint adjacent = in_grid ? (uint(q.z) * params.grid.y + uint(q.y)) * params.grid.x + uint(q.x) : 0;
				if (params.phase == 0) {
					kernel.yzw = -kernel.yzw;
					// Until nested-volume inputs are wired, volumes have open boundaries.
					vec4 visibility = in_grid ? source.probes[adjacent].visibility : vec4(1.0 / C0, 0.0, 0.0, 0.0);
					aggregate[0] += edge * sh_product(kernel, visibility);
				} else if (in_grid) {
					// Open boundaries supply L=0.
					for (uint channel = 0; channel < 3; channel++) {
						aggregate[channel] += edge * sh_product(kernel, source.probes[adjacent].light[channel]);
					}
				}
			}
		}
	}
	vec4 local_vis = vec4(unpackHalf2x16(geometry.words[base + 9]), unpackHalf2x16(geometry.words[base + 10]));
	if (params.phase == 0) {
		destination.probes[probe].visibility = mix(source.probes[probe].visibility, sh_product(local_vis, aggregate[0]), params.relaxation);
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
		vec4 incoming = aggregate[channel];
		vec4 raw_light = emission.coefficients[probe * 3 + channel] + sh_product(local_vis, incoming) + color[channel] * (transfer * incoming);
		destination.probes[probe].light[channel] = mix(source.probes[probe].light[channel], raw_light, params.relaxation);
	}
	destination.probes[probe].visibility = source.probes[probe].visibility;
}
