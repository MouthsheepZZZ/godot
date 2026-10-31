/**************************************************************************/
/*  lrt_geometry_upload.glsl                                                         */
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
#include "lrt_sh_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Updates {
	uint words[];
}
updates;

layout(set = 0, binding = 1, std430) restrict buffer Geometry {
	uint words[];
}
geometry;

layout(set = 0, binding = 2, std430) restrict writeonly buffer Emission {
	float coefficients[];
}
emission;

layout(set = 0, binding = 3, std430) restrict writeonly buffer History0 {
	vec4 sh[];
}
history0;

layout(set = 0, binding = 4, std430) restrict writeonly buffer History1 {
	vec4 sh[];
}
history1;

layout(push_constant, std430) uniform Params {
	uint offset;
	uint end;
	uint reset;
	uint has_emission;
	uvec4 grid;
}
params;

void write_edge(uint probe, uint direction, uint mask) {
	uint word = probe * 15 + 11 + direction / 8;
	uint shift = (direction % 8) * 4;
	atomicAnd(geometry.words[word], ~(15u << shift));
	atomicOr(geometry.words[word], mask << shift);
}

void main() {
	uint update = params.offset + gl_GlobalInvocationID.x;
	if (update >= params.end) {
		return;
	}
	// Record: probe index, 15 geometry words, 12 FP32 emission coefficients.
	uint base = update * 28;
	uint probe = updates.words[base];
	// Full resets have no initialized geometry to read.
	bool reset_history = params.reset != 0;
	if (!reset_history) {
		reset_history = (geometry.words[probe * 15 + 8] & 0x01000000u) != 0;
	}
	// Non-owning trunks never overwrite an edge published by its canonical owner.
	// Atomic nibble updates let different edges share a packed word safely.
	for (uint i = 0; i < 11; i++) {
		geometry.words[probe * 15 + i] = updates.words[base + 1 + i];
	}
	ivec3 coordinate = ivec3(probe % params.grid.x, (probe / params.grid.x) % params.grid.y, probe / (params.grid.x * params.grid.y));
	for (uint direction = 0; direction < 26; direction++) {
		uint cell = direction < 13 ? direction : direction + 1;
		ivec3 delta = ivec3(int(cell % 3) - 1, int((cell / 3) % 3) - 1, int(cell / 9) - 1);
		ivec3 adjacent = coordinate + delta;
		bool in_grid = all(greaterThanEqual(adjacent, ivec3(0))) && all(lessThan(adjacent, ivec3(params.grid.xyz)));
		if (direction < 13 && in_grid) {
			continue;
		}
		uint mask = (updates.words[base + 12 + direction / 8] >> ((direction % 8) * 4)) & 15u;
		write_edge(probe, direction, mask);
		if (in_grid) {
			uint neighbor = uint((adjacent.z * int(params.grid.y) + adjacent.y) * int(params.grid.x) + adjacent.x);
			write_edge(neighbor, 25 - direction, mask);
		}
	}
	if (params.has_emission != 0u) {
		for (uint i = 0; i < 12; i++) {
			emission.coefficients[probe * 12 + i] = uintBitsToFloat(updates.words[base + 16 + i]);
		}
	}
	vec4 visibility = lrt_constrain_visibility(vec4(unpackHalf2x16(updates.words[base + 10]), unpackHalf2x16(updates.words[base + 11])));
	bool inside = (updates.words[base + 9] & 0x01000000u) != 0;
	if (inside) {
		visibility = vec4(0.0);
	}
	geometry.words[probe * 15 + 9] = packHalf2x16(visibility.xy);
	geometry.words[probe * 15 + 10] = packHalf2x16(visibility.zw);
	// Keep valid air-probe histories as the initial state for the new geometry.
	if (!reset_history && !inside) {
		return;
	}
	uint count = params.grid.x * params.grid.y * params.grid.z;
	for (uint i = 0; i < 3; i++) {
		history0.sh[i * count + probe] = vec4(0.0);
		history1.sh[i * count + probe] = vec4(0.0);
	}
	history0.sh[3 * count + probe] = visibility;
	history1.sh[3 * count + probe] = visibility;
}
