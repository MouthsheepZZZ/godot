/**************************************************************************/
/*  lrt_display_shaders.h                                                 */
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

#pragma once

// One pixel per LRT surface receiver. UV stores the pixel's clip-space corner. Mesh NORMAL keeps
// the receiver transport direction so Godot applies its normal orientation consistently. CUSTOM0
// independently carries the geometric normal required by Forward+ shadow bias.
static const char *lrt_light_capture_shader_source = R"LRT(
shader_type spatial;
render_mode cull_disabled, depth_test_disabled, depth_draw_never, ambient_light_disabled, fog_disabled, specular_disabled;

varying vec3 surface_normal;
varying vec3 transport_normal;

void vertex() {
	surface_normal = normalize(MODELVIEW_NORMAL_MATRIX * CUSTOM0.xyz);
	POSITION = vec4(UV, 0.0, 1.0);
}

void fragment() {
	transport_normal = NORMAL;
	NORMAL = surface_normal;
	ALBEDO = vec3(1.0);
	ROUGHNESS = 1.0;
	METALLIC = 0.0;
}

void light() {
	float cosine = max(dot(transport_normal, LIGHT), 0.0);
	DIFFUSE_LIGHT += ATTENUATION * LIGHT_COLOR * LIGHT_AREA_DIFFUSE_MULTIPLIER * (cosine / PI);
}
)LRT";
