#[compute]
#version 450
#VERSION_DEFINES
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define LRT_DATA_SET 0
#define LRT_METADATA_BINDING 7
#define LRT_SAMPLES_BINDING 8
#include "lrt_data_inc.glsl"
#include "lrt_sample_inc.glsl"
layout(set = 0, binding = 0, std430) readonly buffer Geometry {
	uint words[];
}
geometry;
layout(set = 0, binding = 1, std430) readonly buffer Emission {
	vec4 sh[];
}
emission;
struct ProbeState {
	vec4 light[3];
	vec4 visibility;
};
layout(set = 0, binding = 2, std430) readonly buffer History {
	ProbeState probes[];
}
history;
layout(set = 0, binding = 3, std430) readonly buffer Sources {
	vec4 sh[];
}
sources;
layout(set = 0, binding = 4, std430) readonly buffer Direct {
	vec4 sh[];
}
direct_light;
layout(set = 0, binding = 5, std430) readonly buffer Diagnostics {
	uint words[];
}
diagnostic;
layout(set = 0, binding = 6, std430) readonly buffer SDF {
	uint words[];
}
sdf;
layout(set = 0, binding = 9, std140) uniform Parameters {
	mat4 inv_projection;
	mat4 camera_to_world;
	mat4 volume_to_world;
	mat4 world_to_sdf;
	vec4 size_spacing;
	uvec4 grid_mode;
	ivec4 probe_axis;
	vec4 direction_slice;
	vec4 sdf_position_scale;
	vec4 sdf_size;
	uvec4 sdf_grid_color;
	uvec4 screen_view_samples;
	vec4 old_position;
	vec4 old_size;
	vec4 new_position;
	vec4 new_size;
}
params;
layout(set = 0, binding = 10, rgba16f) uniform writeonly image2DArray output_image;
layout(set = 0, binding = 11) uniform sampler2DArray gather_lighting;
layout(set = 0, binding = 12) uniform sampler2DArray gather_guide;
layout(set = 0, binding = 13) uniform sampler2DArray gather_low;

vec3 normal_from_guide(vec4 guide) {
	uint p = floatBitsToUint(guide.w);
	return normalize(vec3(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u) / 127.5 - 1.0);
}
vec3 display_light(vec3 color) {
	color = max(color, vec3(0.0));
	return pow(color / (1.0 + color), vec3(1.0 / 2.2));
}
vec3 palette(uint index) {
	return 0.35 + 0.65 * fract(vec3(0.37, 0.61, 0.83) * float(index + 1));
}
bool bounds_hit(vec3 origin, vec3 ray, vec3 minimum, vec3 maximum, out float near_t, out float far_t) {
	vec3 inverse_ray = 1.0 / ray;
	vec3 a = (minimum - origin) * inverse_ray;
	vec3 b = (maximum - origin) * inverse_ray;
	near_t = max(0.0, max(min(a.x, b.x), max(min(a.y, b.y), min(a.z, b.z))));
	far_t = min(max(a.x, b.x), min(max(a.y, b.y), max(a.z, b.z)));
	return far_t >= near_t;
}
float sdf_distance(vec3 p, out uint nearest) {
	vec3 position = clamp((p - params.sdf_position_scale.xyz) / params.sdf_size.xyz, 0.0, 1.0) * vec3(params.sdf_grid_color.xyz - 1u);
	ivec3 cell = clamp(ivec3(floor(position)), ivec3(0), ivec3(params.sdf_grid_color.xyz) - 2);
	vec3 f = clamp(position - vec3(cell), 0.0, 1.0);
	float result = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				uvec3 q = uvec3(cell + ivec3(x, y, z));
				uint at = (q.z * params.sdf_grid_color.y + q.y) * params.sdf_grid_color.x + q.x;
				uint bits = (sdf.words[at / 2] >> ((at % 2) * 16)) & 65535u;
				int value = int(bits << 16) >> 16;
				vec3 w = mix(vec3(1.0) - f, f, vec3(x, y, z));
				result += float(value) / 32767.0 * params.sdf_position_scale.w * w.x * w.y * w.z;
			}
		}
	}
	uvec3 closest = uvec3(round(position));
	nearest = (closest.z * params.sdf_grid_color.y + closest.y) * params.sdf_grid_color.x + closest.x;
	return result;
}
vec3 probe_color(uint probe, vec3 n) {
	uint mode = params.grid_mode.w;
	uint base = probe * 18;
	uint flags = geometry.words[base + 8];
	if (mode == 0u) {
		return (flags & 0x01000000u) != 0u ? vec3(1.0, 0.1, 0.15) : vec3(0.1, 0.9, 0.5);
	}
	vec4 sh = vec4(LRT_C0, LRT_C1 * n);
	if (mode == 4u || mode == 5u) {
		vec4 v = mode == 4u ? vec4(unpackHalf2x16(geometry.words[base + 9]), unpackHalf2x16(geometry.words[base + 10])) : history.probes[probe].visibility;
		return vec3(clamp(dot(v, sh), 0.0, 1.0));
	}
	if (mode == 11u) {
		vec3 direction = normalize(params.direction_slice.xyz);
		float best = -2.0;
		uint selected = 0u;
		uint at = 0u;
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					if (x == 0 && y == 0 && z == 0) {
						continue;
					}
					float angle = dot(normalize(vec3(x, y, z)), direction);
					if (angle > best) {
						best = angle;
						selected = at;
					}
					at++;
				}
			}
		}
		uint mask = (geometry.words[base + 11 + selected / 4] >> ((selected % 4) * 8)) & 15u;
		return mix(vec3(0.9, 0.1, 0.1), vec3(0.1, 1.0, 0.25), float(bitCount(mask)) * 0.25);
	}
	if (mode == 12u) {
		uvec3 p = uvec3(probe % params.grid_mode.x, (probe / params.grid_mode.x) % params.grid_mode.y, probe / (params.grid_mode.x * params.grid_mode.y)) / 8u;
		uvec3 grid = (params.grid_mode.xyz + 7u) / 8u;
		uint status = diagnostic.words[(p.z * grid.y + p.y) * grid.x + p.x];
		if (status == 1u) {
			return vec3(1.0, 0.8, 0.05);
		}
		if (status == 2u) {
			return vec3(0.1, 0.4, 1.0);
		}
		if (status == 3u) {
			return vec3(0.1, 1.0, 0.35);
		}
		return palette((p.z * grid.y + p.y) * grid.x + p.x) * 0.45;
	}
	if (mode == 19u) {
		vec4 status = direct_light.sh[probe * 4 + 3];
		return status.z > 0.0 ? mix(vec3(1.0, 0.1, 0.05), vec3(0.1, 1.0, 0.3), status.x / status.z) : vec3(0.6, 0.6, 0.15);
	}
	vec3 result;
	mat4 transfer;
	if (mode == 3u) {
		for (uint col = 0; col < 4; col++) {
			transfer[col] = vec4(unpackHalf2x16(geometry.words[base + col * 2]), unpackHalf2x16(geometry.words[base + col * 2 + 1]));
		}
	}
	mat3 world_to_volume = transpose(mat3(normalize(params.volume_to_world[0].xyz), normalize(params.volume_to_world[1].xyz), normalize(params.volume_to_world[2].xyz)));
	for (uint c = 0u; c < 3u; c++) {
		vec4 light;
		if (mode == 3u) {
			vec3 color = vec3(flags & 255u, (flags >> 8) & 255u, (flags >> 16) & 255u) / 255.0;
			light = color[c] * (transfer * vec4(LRT_C0, LRT_C1 * normalize(params.direction_slice.xyz)));
		} else if (mode == 6u) {
			light = direct_light.sh[probe * 4 + c];
		} else if (mode == 8u) {
			light = sources.sh[probe * 3 + c];
		} else if (mode == 7u) {
			light = lrt_flip(lrt_product(vec4(lrt.sky[c].x, world_to_volume * lrt.sky[c].yzw), history.probes[probe].visibility));
		} else {
			light = history.probes[probe].light[c];
		}
		result[c] = max(0.0, dot(light, vec4(LRT_C0, -(2.0 / 3.0) * LRT_C1 * n)));
	}
	return display_light(result);
}

vec4 diagnostic_vec4(uint offset) {
	return uintBitsToFloat(uvec4(diagnostic.words[offset], diagnostic.words[offset + 1], diagnostic.words[offset + 2], diagnostic.words[offset + 3]));
}
float ray_sphere(vec3 origin, vec3 ray, vec3 center, float radius) {
	vec3 relative = origin - center;
	float b = dot(relative, ray);
	float discriminant = b * b - dot(relative, relative) + radius * radius;
	return discriminant >= 0.0 ? max(0.0, -b - sqrt(discriminant)) : -1.0;
}
float ray_line(vec3 origin, vec3 ray, vec3 a, vec3 b, float radius) {
	vec3 segment = b - a;
	vec3 relative = origin - a;
	float c = dot(segment, segment);
	float along = dot(ray, segment);
	float denominator = c - along * along;
	float u = denominator > 0.000001 ? clamp((dot(segment, relative) - along * dot(ray, relative)) / denominator, 0.0, 1.0) : 0.0;
	float t = max(0.0, dot(a + segment * u - origin, ray));
	vec3 delta = origin + ray * t - (a + segment * u);
	return dot(delta, delta) <= radius * radius ? t : -1.0;
}
bool wire_box(vec3 origin, vec3 ray, vec3 minimum, vec3 size, float radius) {
	if (any(lessThanEqual(size, vec3(0.0)))) {
		return false;
	}
	float near_t, far_t;
	if (!bounds_hit(origin, ray, minimum, minimum + size, near_t, far_t)) {
		return false;
	}
	vec3 p = origin + ray * near_t;
	vec3 distance = min(abs(p - minimum), abs(p - (minimum + size)));
	return int(distance.x < radius) + int(distance.y < radius) + int(distance.z < radius) >= 2;
}
vec3 sample_markers(vec3 origin, vec3 ray, bool edges) {
	uint base = uint(params.sdf_size.w);
	float closest = 1e30;
	vec3 color = vec3(0.025);
	if (edges) {
		float best = -2.0;
		uint selected = 0u;
		uint d = 0u;
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					if (x == 0 && y == 0 && z == 0) {
						continue;
					}
					float alignment = dot(normalize(vec3(x, y, z)), normalize(params.direction_slice.xyz));
					if (alignment > best) {
						best = alignment;
						selected = d;
					}
					d++;
				}
			}
		}
		base += params.screen_view_samples.w * 8u + selected * 32u;
		for (uint line = 0u; line < 4u; line++) {
			vec4 a = diagnostic_vec4(base + line * 8u);
			vec4 b = diagnostic_vec4(base + line * 8u + 4u);
			float t = ray_line(origin, ray, a.xyz, b.xyz, params.size_spacing.w * 0.009);
			if (t >= 0.0 && t < closest) {
				closest = t;
				color = a.w > 0.0 ? vec3(0.1, 1.0, 0.2) : vec3(1.0, 0.1, 0.05);
			}
		}
	} else {
		vec3 center = -params.size_spacing.xyz * 0.5 + vec3(params.probe_axis.xyz) * params.size_spacing.w;
		for (uint d = 0u; d < params.screen_view_samples.w; d++) {
			vec4 sample_data = diagnostic_vec4(base + d * 8u);
			vec4 albedo = diagnostic_vec4(base + d * 8u + 4u);
			float sphere = ray_sphere(origin, ray, sample_data.xyz, params.size_spacing.w * 0.11);
			float segment = ray_line(origin, ray, center, sample_data.xyz, params.size_spacing.w * 0.004);
			float t = sphere >= 0.0 ? sphere : segment;
			if (t >= 0.0 && t < closest) {
				closest = t;
				color = sample_data.w > 0.0 ? pow(max(albedo.rgb, vec3(0.0)), vec3(1.0 / 2.2)) : vec3(0.1, 0.4, 1.0);
				if (albedo.w > 0.0) {
					color = palette(uint(albedo.w) - 1u);
				}
			}
		}
	}
	return color;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(params.screen_view_samples.xy)))) {
		return;
	}
	uint mode = params.grid_mode.w;
	int view = int(params.screen_view_samples.z);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_view_samples.xy);
	vec3 color = vec3(0.025);
	if (mode >= 10u && mode != 11u && mode != 12u && mode != 19u) {
		vec4 guide = textureLod(gather_guide, vec3(uv, view), 0.0);
		vec4 lighting = textureLod(gather_lighting, vec3(uv, view), 0.0);
		if (mode == 15u) {
			lighting = textureLod(gather_low, vec3(uv, view), 0.0);
		}
		if (mode == 13u) {
			color = guide.w == 0.0 ? vec3(0.0) : vec3(length(guide.xyz - params.camera_to_world[3].xyz) / max(params.size_spacing.x, 1.0));
		} else if (mode == 14u) {
			color = guide.w == 0.0 ? vec3(0.0) : normal_from_guide(guide) * 0.5 + 0.5;
		} else if (mode == 17u) {
			color = vec3(lighting.a);
		} else {
			if ((mode == 10u || (mode == 18u && uv.x < 0.5)) && guide.w != 0.0) {
				lighting = lrt_sample(guide.xyz, normal_from_guide(guide));
				lighting.rgb *= params.new_size.w;
			}
			color = display_light(lighting.rgb);
		}
	} else {
		vec4 near_p = params.inv_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
		vec4 far_p = params.inv_projection * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
		vec3 world_origin = (params.camera_to_world * vec4(near_p.xyz / near_p.w, 1.0)).xyz;
		vec3 world_end = (params.camera_to_world * vec4(far_p.xyz / far_p.w, 1.0)).xyz;
		mat4 world_to_local = mode == 2u ? params.world_to_sdf : inverse(params.volume_to_world);
		vec3 origin = (world_to_local * vec4(world_origin, 1.0)).xyz;
		vec3 ray = normalize(mat3(world_to_local) * (world_end - world_origin));
		vec3 minimum = mode == 2u ? params.sdf_position_scale.xyz : -params.size_spacing.xyz * 0.5;
		vec3 maximum = mode == 2u ? minimum + params.sdf_size.xyz : params.size_spacing.xyz * 0.5;
		float begin, end;
		if (mode == 1u || (mode == 11u && params.screen_view_samples.w == 26u)) {
			color = sample_markers(origin, ray, mode == 11u);
		} else if (bounds_hit(origin, ray, minimum, maximum, begin, end)) {
			if (mode == 2u && all(greaterThan(params.sdf_grid_color.xyz, uvec3(1u)))) {
				if (params.probe_axis.w >= 0) {
					int axis = params.probe_axis.w;
					float cell_size = params.sdf_size[axis] / float(params.sdf_grid_color[axis] - 1u);
					float plane = minimum[axis] + params.direction_slice.w * cell_size;
					float t = (plane - origin[axis]) / ray[axis];
					if (t >= begin && t <= end) {
						uint at;
						float distance = sdf_distance(origin + ray * t, at);
						uint rgba = sdf.words[params.sdf_grid_color.w + at];
						vec3 material = pow(vec3(rgba & 255u, (rgba >> 8) & 255u, (rgba >> 16) & 255u) / 255.0, vec3(1.0 / 2.2));
						color = distance <= 0.0 ? material : vec3(0.1, 0.25, 0.9) * min(distance / cell_size, 1.0);
						if (abs(distance) < cell_size * 0.08) {
							color = vec3(1.0);
						}
					}
					imageStore(output_image, ivec3(pixel, view), vec4(color, 1.0));
					return;
				}
				float step_size = min(params.sdf_size.x / float(params.sdf_grid_color.x - 1u), min(params.sdf_size.y / float(params.sdf_grid_color.y - 1u), params.sdf_size.z / float(params.sdf_grid_color.z - 1u))) * 0.25;
				for (float t = begin; t <= end; t += step_size) {
					uint at;
					float distance = sdf_distance(origin + ray * t, at);
					if (distance <= 0.0) {
						uint rgba = sdf.words[params.sdf_grid_color.w + at];
						color = pow(vec3(rgba & 255u, (rgba >> 8) & 255u, (rgba >> 16) & 255u) / 255.0, vec3(1.0 / 2.2));
						break;
					}
				}
			} else if (mode != 2u && all(greaterThan(params.grid_mode.xyz, uvec3(1u)))) {
				float spacing = params.size_spacing.w;
				if (params.probe_axis.w >= 0) {
					int axis = params.probe_axis.w;
					float plane = minimum[axis] + params.direction_slice.w * spacing;
					float t = (plane - origin[axis]) / ray[axis];
					begin = max(begin, t - spacing * 0.2);
					end = min(end, t + spacing * 0.2);
				}
				for (float t = begin; t <= end; t += spacing * 0.15) {
					vec3 p = origin + ray * t;
					ivec3 coordinate = ivec3(round((p - minimum) / spacing));
					if (any(lessThan(coordinate, ivec3(0))) || any(greaterThanEqual(coordinate, ivec3(params.grid_mode.xyz)))) {
						continue;
					}
					if (all(greaterThanEqual(params.probe_axis.xyz, ivec3(0))) && any(notEqual(coordinate, params.probe_axis.xyz))) {
						continue;
					}
					vec3 relative = p - (minimum + vec3(coordinate) * spacing);
					if (dot(relative, relative) <= spacing * spacing * 0.04) {
						uint probe = (uint(coordinate.z) * params.grid_mode.y + uint(coordinate.y)) * params.grid_mode.x + uint(coordinate.x);
						color = probe_color(probe, normalize(relative));
						break;
					}
				}
			}
		}
		if (mode == 12u) {
			if (wire_box(origin, ray, params.old_position.xyz, params.old_size.xyz, params.size_spacing.w * 0.02)) {
				color = vec3(1.0, 0.1, 0.1);
			}
			if (wire_box(origin, ray, params.new_position.xyz, params.new_size.xyz, params.size_spacing.w * 0.02)) {
				color = vec3(0.1, 1.0, 0.2);
			}
		}
	}
	imageStore(output_image, ivec3(pixel, view), vec4(color, 1.0));
}
