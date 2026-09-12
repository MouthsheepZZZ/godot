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

// Scene shaders the module uses to display the production LRT field. They are the ones
// validated in N2 (standard-material receiving) and N1 (probe slice); keeping them inside
// the module is what lets LRTVolume3D show the migrated result on its own, with no
// per-project display script.

// Standard-material receiver: the surface keeps its authored material and this overlay
// only adds the migrated diffuse term, sampled per fragment from the production field.
static const char *lrt_receive_shader_source = R"LRT(
// N2 standard-material receiver.
//
// The surface keeps its authored StandardMaterial3D, so the engine owns direct light,
// specular and the final tonemap. This material is assigned as GeometryInstance3D
// material_overlay and only *adds* the migrated LRT diffuse term, sampled per fragment
// from the production probe field:
//
//   full / indirect:  albedo * (indirect / PI + skyColor * visibleSky)
//   sky visibility:   visibleSky
//
// The prototype's display reconstruction (src/shaders.js "display") decides which probes
// contribute: 3x3x3 candidates, quadratic B-spline weights, solid/back-face rejection and
// an occlusion test against the same geometry that feeds the solver. Albedo is applied
// exactly once, here, and the LRT field itself is never pre-multiplied by the receiver.
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled;

uniform vec3 albedo = vec3(0.72, 0.72, 0.68);
uniform vec3 grid_min;
uniform vec3 grid_size;
uniform float spacing;
uniform vec2 atlas_size;
uniform int gather_count = 27;
uniform bool blur_sampling = true;

uniform sampler2D radiance_r : filter_nearest, repeat_disable;
uniform sampler2D radiance_g : filter_nearest, repeat_disable;
uniform sampler2D radiance_b : filter_nearest, repeat_disable;
uniform sampler2D visibility_field : filter_nearest, repeat_disable;
uniform sampler2D material_field : filter_nearest, repeat_disable;

uniform int box_count = 0;
uniform vec3 box_min[16];
uniform vec3 box_max[16];

// Mesh occlusion, same layout as the prototype's mesh-shader.js.
uniform int mesh_node_count = 0;
uniform sampler2D mesh_nodes : filter_nearest, repeat_disable;
uniform sampler2D mesh_triangles : filter_nearest, repeat_disable;
uniform sampler2D mesh_materials : filter_nearest, repeat_disable;
uniform sampler2D mesh_atlas : filter_nearest, repeat_disable;

uniform vec3 sky_color = vec3(0.0);
uniform int mode = 0;

varying vec3 world_position;
varying vec3 world_normal;

const float C0 = 0.2820947918;
const float C1 = 0.4886025119;

vec4 K(vec3 n) {
	return vec4(PI * C0, (2.0 * PI / 3.0) * C1 * n);
}

ivec3 grid_size_i() {
	return ivec3(grid_size);
}

bool outside(ivec3 p) {
	return any(lessThan(p, ivec3(0))) || any(greaterThanEqual(p, grid_size_i()));
}

vec2 atlas_coord(ivec3 p) {
	return (vec2(float(p.x + p.z * int(grid_size.x)), float(p.y)) + 0.5) / atlas_size;
}

vec4 fetch_field(sampler2D field, ivec3 p) {
	return texture(field, atlas_coord(p));
}

vec3 probe_position(ivec3 p) {
	return grid_min + (vec3(p) + 0.5) * spacing;
}

float hit_box(vec3 origin, vec3 direction, vec3 low, vec3 high, out vec3 normal) {
	float near_t = -1e20;
	float far_t = 1e20;
	vec3 near_normal = vec3(0.0);
	vec3 far_normal = vec3(0.0);
	for (int axis = 0; axis < 3; axis++) {
		if (abs(direction[axis]) < 1e-7) {
			if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
				return -1.0;
			}
			continue;
		}
		float a = (low[axis] - origin[axis]) / direction[axis];
		float b = (high[axis] - origin[axis]) / direction[axis];
		vec3 n = vec3(0.0);
		n[axis] = -sign(direction[axis]);
		if (min(a, b) > near_t) {
			near_t = min(a, b);
			near_normal = n;
		}
		if (max(a, b) < far_t) {
			far_t = max(a, b);
			far_normal = -n;
		}
	}
	if (far_t < max(near_t, 0.0)) {
		return -1.0;
	}
	normal = near_t > 0.0 ? near_normal : far_normal;
	return near_t > 0.0 ? near_t : far_t;
}

vec4 mesh_fetch(sampler2D image, int index) {
	return texelFetch(image, ivec2(index % 1024, index / 1024), 0);
}

bool mesh_bounds(vec3 origin, vec3 direction, vec3 low, vec3 high, float limit) {
	float near_t = 0.0;
	float far_t = limit;
	for (int axis = 0; axis < 3; axis++) {
		if (abs(direction[axis]) < 1e-8) {
			if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
				return false;
			}
		} else {
			float a = (low[axis] - origin[axis]) / direction[axis];
			float b = (high[axis] - origin[axis]) / direction[axis];
			near_t = max(near_t, min(a, b));
			far_t = min(far_t, max(a, b));
			if (far_t < near_t) {
				return false;
			}
		}
	}
	return true;
}

float mesh_wrap(float value, float wrap_mode) {
	if (wrap_mode == 0.0) {
		return clamp(value, 0.0, 1.0);
	}
	if (wrap_mode == 1.0) {
		return fract(value);
	}
	return 1.0 - abs(mod(value, 2.0) - 1.0);
}

// Preorder BVH with escape indices: no traversal stack and no visit limit.
float trace_mesh(vec3 origin, vec3 direction, float limit, bool any_hit, bool cull_back_faces,
		out vec3 normal, out vec3 color, out vec3 geometric_normal) {
	int node = 0;
	float nearest = limit;
	bool found = false;
	while (node < mesh_node_count) {
		vec4 low = mesh_fetch(mesh_nodes, node * 2);
		vec4 high = mesh_fetch(mesh_nodes, node * 2 + 1);
		if (!mesh_bounds(origin, direction, low.xyz, high.xyz, nearest)) {
			node = int(low.w);
			continue;
		}
		if (high.w < 0.0) {
			node++;
			continue;
		}
		int leaf_code = int(high.w);
		int start = leaf_code / 4;
		int count = leaf_code % 4 + 1;
		for (int i = 0; i < 4; i++) {
			if (i >= count) {
				break;
			}
			int base = (start + i) * 10;
			vec4 a = mesh_fetch(mesh_triangles, base);
			vec4 b = mesh_fetch(mesh_triangles, base + 1);
			vec4 c = mesh_fetch(mesh_triangles, base + 2);
			vec3 edge1 = b.xyz - a.xyz;
			vec3 edge2 = c.xyz - a.xyz;
			vec3 h = cross(direction, edge2);
			float det = dot(edge1, h);
			if (abs(det) < 1e-10) {
				continue;
			}
			vec3 delta = origin - a.xyz;
			float u = dot(delta, h) / det;
			if (u < 0.0 || u > 1.0) {
				continue;
			}
			vec3 q = cross(delta, edge1);
			float v = dot(direction, q) / det;
			if (v < 0.0 || u + v > 1.0) {
				continue;
			}
			float t = dot(edge2, q) / det;
			if (t < 0.000001 || t >= nearest) {
				continue;
			}
			int material_index = int(mesh_fetch(mesh_triangles, base + 9).x);
			vec4 rect = mesh_fetch(mesh_materials, material_index * 2);
			vec4 settings = mesh_fetch(mesh_materials, material_index * 2 + 1);
			if (cull_back_faces && settings.w == 0.0 && det < 0.0) {
				continue;
			}
			vec4 na = mesh_fetch(mesh_triangles, base + 3);
			vec4 nb = mesh_fetch(mesh_triangles, base + 4);
			vec4 nc = mesh_fetch(mesh_triangles, base + 5);
			vec2 texcoord = vec2(a.w, na.w) * (1.0 - u - v) + vec2(b.w, nb.w) * u + vec2(c.w, nc.w) * v;
			texcoord = vec2(mesh_wrap(texcoord.x, settings.x), mesh_wrap(texcoord.y, settings.y));
			vec4 albedo_value = mesh_fetch(mesh_triangles, base + 6) * (1.0 - u - v) + mesh_fetch(mesh_triangles, base + 7) * u + mesh_fetch(mesh_triangles, base + 8) * v;
			albedo_value *= textureLod(mesh_atlas, rect.xy + texcoord * rect.zw, 0.0);
			if (albedo_value.a < settings.z) {
				continue;
			}
			if (any_hit) {
				return t;
			}
			nearest = t;
			found = true;
			normal = normalize(na.xyz * (1.0 - u - v) + nb.xyz * u + nc.xyz * v);
			geometric_normal = normalize(cross(edge1, edge2));
			if (dot(geometric_normal, direction) > 0.0) {
				geometric_normal = -geometric_normal;
			}
			if (dot(normal, direction) > 0.0) {
				normal = -normal;
			}
			color = albedo_value.rgb;
		}
		node = int(low.w);
	}
	return found ? nearest : -1.0;
}

// Mirrors the prototype's shadow() helper: analytic boxes first, then the traced mesh.
float shadow_ray(vec3 p, vec3 direction, float distance_to_light) {
	for (int i = 0; i < 16; i++) {
		if (i >= box_count) {
			break;
		}
		vec3 normal;
		float t = hit_box(p, direction, box_min[i], box_max[i], normal);
		if (t > 0.0001 && t < distance_to_light) {
			return 0.0;
		}
	}
	if (mesh_node_count > 0) {
		vec3 normal;
		vec3 color;
		vec3 geometric_normal;
		if (trace_mesh(p, direction, distance_to_light, true, false, normal, color, geometric_normal) > 0.0) {
			return 0.0;
		}
	}
	return 1.0;
}

float reconstruction_weight(float value) {
	float d = abs(value);
	if (d < 0.5) {
		return 0.75 - d * d;
	}
	if (d < 1.5) {
		return 0.5 * (1.5 - d) * (1.5 - d);
	}
	return 0.0;
}

// Quadratic B-spline reconstruction over three probes per axis, including corners.
void sample_field(vec3 p, vec3 normal, vec3 surface_normal, out vec3 irradiance, out float sky_visibility) {
	vec3 g = (p + surface_normal * spacing * 0.55 - grid_min) / spacing - 0.5;
	ivec3 base = ivec3(floor(g + 0.5)) - ivec3(1);
	vec4 r = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 b = vec4(0.0);
	vec4 v = vec4(0.0);
	float total = 0.0;
	float nearest_sample = 1e30;
	for (int index = 0; index < 27; index++) {
		if (index >= gather_count) {
			break;
		}
		ivec3 q = base + ivec3(index % 3, (index / 3) % 3, index / 9);
		if (outside(q) || fetch_field(material_field, q).a > 0.5 || dot(probe_position(q) - p, surface_normal) < 0.0) {
			continue;
		}
		vec3 delta = vec3(q) - g;
		float w = reconstruction_weight(delta.x) * reconstruction_weight(delta.y) * reconstruction_weight(delta.z);
		if (w <= 0.0) {
			continue;
		}
		float sample_distance = dot(delta, delta);
		if (!blur_sampling && sample_distance >= nearest_sample) {
			continue;
		}
		vec3 ray = probe_position(q) - (p + surface_normal * 0.001);
		float distance = length(ray);
		if (shadow_ray(p + surface_normal * 0.001, ray / distance, distance) <= 0.0) {
			continue;
		}
		if (!blur_sampling) {
			nearest_sample = sample_distance;
			r = vec4(0.0);
			green = vec4(0.0);
			b = vec4(0.0);
			v = vec4(0.0);
			total = 0.0;
			w = 1.0;
		}
		r += w * fetch_field(radiance_r, q);
		green += w * fetch_field(radiance_g, q);
		b += w * fetch_field(radiance_b, q);
		v += w * fetch_field(visibility_field, q);
		total += w;
	}
	irradiance = vec3(0.0);
	sky_visibility = 0.0;
	if (total <= 0.0) {
		return;
	}
	vec4 k = K(normal) / total;
	irradiance = max(vec3(dot(r, k), dot(green, k), dot(b, k)), vec3(0.0));
	sky_visibility = clamp(dot(v, k) / PI, 0.0, 1.0);
}

void vertex() {
	world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
	vec3 normal = normalize(world_normal);
	vec3 p = world_position;
	vec3 indirect;
	float visible_sky;
	sample_field(p, normal, normal, indirect, visible_sky);
	if (mode == 3) {
		// Prototype mode 3 shows the sky visibility channel itself, without albedo.
		ALBEDO = vec3(visible_sky);
	} else {
		vec3 color = albedo * indirect / PI;
		if (mode == 0) {
			// The prototype's ambient sky term lives in the direct-facing color.
			color += albedo * sky_color * visible_sky;
		}
		ALBEDO = color;
	}
}
)LRT";

// Probe slice display: one horizontal probe layer, drawn over the viewport.
static const char *lrt_slice_shader_source = R"LRT(
// N1 probe slice display.
//
// Draws one horizontal probe layer exactly like the prototype's slice modes:
// u spans x, v spans z, solid probes show as their stored material color.
// Values are output linear; the engine's output conversion applies the sRGB encoding.
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

// PI comes from Godot's shader built-ins.
const float C0 = 0.2820947918;

vec4 fetch_atlas(sampler2D field, ivec3 cell) {
	vec2 coordinate = (vec2(float(cell.x + cell.z * int(grid_size.x)), float(cell.y)) + 0.5) / atlas_size;
	return texture(field, coordinate);
}

vec3 tone_map_linear(vec3 c) {
	c *= exposure;
	return max(c / (1.0 + c), vec3(0.0));
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
	} else {
		// transfer(vec4(1,0,0,0), cell, channel).x is the (row 0, column 0) element of each channel matrix.
		float column = float(cell.x + cell.z * int(grid_size.x)) + 0.5;
		float width_ratio = 1.0 / matrix_atlas_size.x;
		float red = texture(matrix_field, vec2(column * width_ratio, (float(cell.y) + 0.5) / matrix_atlas_size.y)).x;
		float green = texture(matrix_field, vec2(column * width_ratio, (float(cell.y) + float(grid_size.y) * 4.0 + 0.5) / matrix_atlas_size.y)).x;
		float blue = texture(matrix_field, vec2(column * width_ratio, (float(cell.y) + float(grid_size.y) * 8.0 + 0.5) / matrix_atlas_size.y)).x;
		color = vec3(red, green, blue);
	}
	vec4 material = fetch_atlas(material_field, cell);
	if (material.a > 0.5) {
		color = material.rgb * 0.16 + vec3(0.04);
	}
	if (min(fract(plane.x), fract(plane.y)) < 0.025) {
		color *= 0.5;
	}
	COLOR = vec4(color, 1.0);
}
)LRT";
