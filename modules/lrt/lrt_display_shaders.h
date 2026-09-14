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

// Probe slice display: one horizontal probe layer, drawn over the viewport.
static const char *lrt_slice_shader_source = R"LRT(
// Draws one horizontal probe layer exactly like the prototype's slice modes:
// u spans x, v spans z, solid probes show as their stored material color.
shader_type canvas_item;
render_mode unshaded;

uniform vec3 grid_min;
uniform vec3 grid_size;
uniform float spacing;
uniform vec2 atlas_size;
uniform vec2 matrix_atlas_size;
uniform float slice_height = 1.0;
uniform float exposure = 1.1;
uniform int mode = %LRT_SLICE_RADIANCE%;

uniform sampler2D radiance_r : filter_nearest, repeat_disable;
uniform sampler2D radiance_g : filter_nearest, repeat_disable;
uniform sampler2D radiance_b : filter_nearest, repeat_disable;
uniform sampler2D visibility_field : filter_nearest, repeat_disable;
uniform sampler2D material_field : filter_nearest, repeat_disable;
uniform sampler2D matrix_field : filter_nearest, repeat_disable;
uniform sampler2D source_r : filter_nearest, repeat_disable;
uniform sampler2D source_g : filter_nearest, repeat_disable;
uniform sampler2D source_b : filter_nearest, repeat_disable;
uniform sampler2D local_visibility_field : filter_nearest, repeat_disable;
uniform sampler2D diagnostic_sdf_field : filter_nearest, repeat_disable;
uniform sampler2D diagnostic_albedo_field : filter_nearest, repeat_disable;
uniform sampler2D diagnostic_emission_field : filter_nearest, repeat_disable;
uniform sampler2D diagnostic_dirty_field : filter_nearest, repeat_disable;

const float C0 = 0.2820947918;

vec4 fetch_atlas(sampler2D field, ivec3 cell) {
	vec2 coordinate = (vec2(float(cell.x + cell.z * int(grid_size.x)), float(cell.y)) + 0.5) / atlas_size;
	return texture(field, coordinate);
}

vec3 tone_map_linear(vec3 color) {
	color *= exposure;
	return max(color / (1.0 + color), vec3(0.0));
}

void fragment() {
	vec2 plane = UV * vec2(grid_size.x, grid_size.z);
	ivec3 cell = ivec3(int(plane.x), int((slice_height - grid_min.y) / spacing), int(plane.y));
	cell = clamp(cell, ivec3(0), ivec3(int(grid_size.x) - 1, int(grid_size.y) - 1, int(grid_size.z) - 1));
	vec3 color;
	if (mode == %LRT_SLICE_RADIANCE%) {
		color = tone_map_linear(C0 * vec3(fetch_atlas(radiance_r, cell).x, fetch_atlas(radiance_g, cell).x, fetch_atlas(radiance_b, cell).x));
	} else if (mode == %LRT_SLICE_SKY_VISIBILITY%) {
		color = vec3(fetch_atlas(visibility_field, cell).x * C0);
	} else if (mode == %LRT_SLICE_MATRIX%) {
		// transfer(vec4(1,0,0,0), cell, channel).x is matrix row 0, column 0.
		float column = float(cell.x + cell.z * int(grid_size.x)) + 0.5;
		float width_ratio = 1.0 / matrix_atlas_size.x;
		float red = texture(matrix_field, vec2(column * width_ratio, (float(cell.y) + 0.5) / matrix_atlas_size.y)).x;
		float green = texture(matrix_field, vec2(column * width_ratio, (float(cell.y) + float(grid_size.y) * 4.0 + 0.5) / matrix_atlas_size.y)).x;
		float blue = texture(matrix_field, vec2(column * width_ratio, (float(cell.y) + float(grid_size.y) * 8.0 + 0.5) / matrix_atlas_size.y)).x;
		color = vec3(red, green, blue);
	} else if (mode == %LRT_SLICE_SOURCE%) {
		color = tone_map_linear(C0 * vec3(fetch_atlas(source_r, cell).x, fetch_atlas(source_g, cell).x, fetch_atlas(source_b, cell).x));
	} else if (mode == %LRT_SLICE_LOCAL_VISIBILITY%) {
		color = vec3(clamp(fetch_atlas(local_visibility_field, cell).x * C0, 0.0, 1.0));
	} else if (mode == %LRT_SLICE_SDF%) {
		vec4 sample_value = fetch_atlas(diagnostic_sdf_field, cell);
		float signed_distance = sample_value.r / max(spacing * 4.0, 0.0001);
		color = sample_value.a < 0.5 ? vec3(0.0) :
				(signed_distance < 0.0 ? vec3(0.8, 0.12, 0.08) : vec3(0.08, 0.25, 0.8)) *
				(1.0 - 0.75 * clamp(abs(signed_distance), 0.0, 1.0));
	} else if (mode == %LRT_SLICE_ALBEDO%) {
		vec4 sample_value = fetch_atlas(diagnostic_albedo_field, cell);
		color = sample_value.a < 0.5 ? vec3(0.0) : sample_value.rgb;
	} else if (mode == %LRT_SLICE_EMISSION%) {
		vec4 sample_value = fetch_atlas(diagnostic_emission_field, cell);
		color = sample_value.a < 0.5 ? vec3(0.0) : tone_map_linear(sample_value.rgb);
	} else if (mode == %LRT_SLICE_DIRTY_TRUNKS%) {
		color = fetch_atlas(diagnostic_dirty_field, cell).r > 0.5 ? vec3(1.0, 0.32, 0.04) : vec3(0.035);
	} else {
		color = vec3(0.0);
	}
	vec4 material = fetch_atlas(material_field, cell);
	if (material.a > 0.5 && mode <= %LRT_SLICE_MATRIX%) {
		color = material.rgb * 0.16 + vec3(0.04);
	}
	if (min(fract(plane.x), fract(plane.y)) < 0.025) {
		color *= 0.5;
	}
	COLOR = vec4(color, 1.0);
}
)LRT";
