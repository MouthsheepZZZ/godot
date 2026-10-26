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

layout(set = 0, binding = 1, std430) restrict writeonly buffer Geometry {
	uint words[];
}
geometry;

layout(set = 0, binding = 2, std430) restrict writeonly buffer Emission {
	float coefficients[];
}
emission;

struct ProbeState {
	vec4 light[3];
	vec4 visibility;
};

layout(set = 0, binding = 3, std430) restrict writeonly buffer History0 {
	ProbeState probes[];
}
history0;

layout(set = 0, binding = 4, std430) restrict writeonly buffer History1 {
	ProbeState probes[];
}
history1;

layout(push_constant, std430) uniform Params {
	uint offset;
	uint end;
	uvec2 padding;
}
params;

void main() {
	uint update = params.offset + gl_GlobalInvocationID.x;
	if (update >= params.end) {
		return;
	}
	// Record: probe index, 18 geometry words, 12 FP32 emission coefficients.
	uint base = update * 31;
	uint probe = updates.words[base];
	for (uint i = 0; i < 18; i++) {
		geometry.words[probe * 18 + i] = updates.words[base + 1 + i];
	}
	for (uint i = 0; i < 12; i++) {
		emission.coefficients[probe * 12 + i] = uintBitsToFloat(updates.words[base + 19 + i]);
	}
	vec4 visibility = lrt_constrain_visibility(vec4(unpackHalf2x16(updates.words[base + 10]), unpackHalf2x16(updates.words[base + 11])));
	bool inside = (updates.words[base + 9] & 0x01000000u) != 0;
	if (inside) {
		visibility = vec4(0.0);
	}
	geometry.words[probe * 18 + 9] = packHalf2x16(visibility.xy);
	geometry.words[probe * 18 + 10] = packHalf2x16(visibility.zw);
	for (uint i = 0; i < 3; i++) {
		history0.probes[probe].light[i] = vec4(0.0);
		history1.probes[probe].light[i] = vec4(0.0);
	}
	history0.probes[probe].visibility = visibility;
	history1.probes[probe].visibility = visibility;
}
