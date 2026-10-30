#[compute]
#version 450
#VERSION_DEFINES
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define LRT_DATA_SET 0
#define LRT_METADATA_BINDING 4
#define LRT_SAMPLES_BINDING 5
#include "lrt_data_inc.glsl"
#include "lrt_sample_inc.glsl"

// Modes follow ViewportDebugDraw's nine contiguous LRT entries.
const uint INDIRECT = 0u;
const uint PROBE_SAMPLING = 1u;
const uint PROBES = 2u;
const uint LOCAL_VIS = 3u;
const uint GLOBAL_VIS = 4u;
const uint EDGE_VIS = 5u;
const uint STATIC_LTM = 6u;
const uint COLOR_SDF = 7u;
const uint TRUNKS = 8u;
const uint BLENDING = 9u;

layout(set = 0, binding = 0, std430) readonly buffer Geometry {
	uint words[];
}
geometry;
layout(set = 0, binding = 1, std430) readonly buffer History {
	vec4 sh[];
}
history;
layout(set = 0, binding = 2, std430) readonly buffer Diagnostics {
	uint words[];
}
diagnostic;
layout(set = 0, binding = 3, std430) readonly buffer SDF {
	uint words[];
}
sdf;
layout(set = 0, binding = 6, std140) uniform Parameters {
	mat4 inv_projection;
	mat4 camera_to_world;
	mat4 world_to_local;
	mat4 local_to_world;
	vec4 size_spacing;
	uvec4 grid_mode;
	vec4 sdf_min_scale;
	vec4 sdf_size_offset;
	uvec4 sdf_grid_color;
	uvec4 screen_pass;
	vec4 old_position;
	vec4 old_size;
	vec4 new_position;
	vec4 new_size;
}
params;
layout(set = 0, binding = 7, rgba16f) uniform image2DArray output_image;
layout(set = 0, binding = 8) uniform sampler2DArray gather_lighting;
layout(set = 0, binding = 9) uniform sampler2D depth_texture;
layout(set = 0, binding = 12) uniform sampler2D normal_texture;
layout(set = 0, binding = 10, r32f) uniform image2DArray output_depth;
layout(set = 0, binding = 11) uniform usampler2D surface_profile_texture;
layout(set = 0, binding = 13) uniform sampler2DArray gather_guide;
layout(set = 0, binding = 14) uniform usampler2DArray gather_profile;

vec3 guide_normal(vec4 guide) {
	uint p = floatBitsToUint(guide.w);
	return normalize(vec3(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u) / 127.5 - 1.0);
}
vec3 display_light(vec3 color) {
	color = max(color, vec3(0.0));
	return pow(color / (1.0 + color), vec3(1.0 / 2.2));
}
bool bounds_hit(vec3 origin, vec3 ray, vec3 minimum, vec3 maximum, out float begin, out float end) {
	vec3 inverse_ray = 1.0 / mix(ray, vec3(1e-30), equal(ray, vec3(0.0)));
	vec3 a = (minimum - origin) * inverse_ray;
	vec3 b = (maximum - origin) * inverse_ray;
	begin = max(0.0, max(min(a.x, b.x), max(min(a.y, b.y), min(a.z, b.z))));
	end = min(max(a.x, b.x), min(max(a.y, b.y), max(a.z, b.z)));
	return end >= begin;
}
float ray_sphere(vec3 origin, vec3 ray, vec3 center, float radius) {
	vec3 relative = origin - center;
	float b = dot(relative, ray);
	float discriminant = b * b - dot(relative, relative) + radius * radius;
	if (discriminant < 0.0) {
		return -1.0;
	}
	float t = -b - sqrt(discriminant);
	return t >= 0.0 ? t : -b + sqrt(discriminant);
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
float wire_box(vec3 origin, vec3 ray, vec3 minimum, vec3 size, float radius) {
	if (any(lessThanEqual(size, vec3(0.0)))) {
		return -1.0;
	}
	float closest = 1e30;
	for (int axis = 0; axis < 3; axis++) {
		int u = (axis + 1) % 3;
		int v = (axis + 2) % 3;
		for (int corner = 0; corner < 4; corner++) {
			vec3 a = minimum;
			a[u] += float(corner & 1) * size[u];
			a[v] += float(corner >> 1) * size[v];
			vec3 b = a;
			b[axis] += size[axis];
			float t = ray_line(origin, ray, a, b, radius);
			if (t >= 0.0) {
				closest = min(closest, t);
			}
		}
	}
	return closest < 1e30 ? closest : -1.0;
}
vec3 edge_color(uint count) {
	if (count == 0u) {
		return vec3(1.0, 0.15, 0.05);
	}
	if (count == 1u) {
		return vec3(1.0, 0.5, 0.05);
	}
	return count == 2u ? vec3(1.0, 0.9, 0.1) : vec3(0.1, 0.85, 1.0);
}
vec3 trunk_color(uint status) {
	if (status == 1u) {
		return vec3(1.0, 0.8, 0.05);
	}
	if (status == 2u) {
		return vec3(0.15, 0.5, 1.0);
	}
	return status == 3u ? vec3(0.1, 1.0, 0.35) : vec3(0.18);
}

bool probe_color(uint probe, vec3 n, out vec3 color) {
	uint mode = params.grid_mode.w;
	uint base = probe * 18u;
	uint flags = geometry.words[base + 8u];
	if ((flags & 0x01000000u) != 0u) {
		color = vec3(1.0, 0.0, 1.0);
		return mode == PROBES;
	}
	if (mode == LOCAL_VIS || mode == GLOBAL_VIS) {
		vec4 v = mode == LOCAL_VIS ? vec4(unpackHalf2x16(geometry.words[base + 9u]), unpackHalf2x16(geometry.words[base + 10u])) : history.sh[3u * params.grid_mode.x * params.grid_mode.y * params.grid_mode.z + probe];
		color = vec3(clamp(dot(lrt_constrain_visibility(v), vec4(LRT_C0, LRT_C1 * n)), 0.0, 1.0));
		return true;
	}
	mat4 transfer;
	vec3 albedo;
	if (mode == STATIC_LTM) {
		for (uint col = 0u; col < 4u; col++) {
			transfer[col] = vec4(unpackHalf2x16(geometry.words[base + col * 2u]), unpackHalf2x16(geometry.words[base + col * 2u + 1u]));
		}
		albedo = vec3(flags & 255u, (flags >> 8) & 255u, (flags >> 16) & 255u) / 255.0;
	}
	vec3 result;
	float energy = 0.0;
	for (uint c = 0u; c < 3u; c++) {
		vec4 light;
		if (mode == STATIC_LTM) {
			light = lrt_filter_radiance(albedo[c] * transfer * vec4(1.0 / LRT_C0, 0.0, 0.0, 0.0));
		} else {
			light = history.sh[c * params.grid_mode.x * params.grid_mode.y * params.grid_mode.z + probe];
		}
		energy += light.x;
		result[c] = max(0.0, dot(light, vec4(LRT_C0, -(2.0 / 3.0) * LRT_C1 * n)));
	}
	color = display_light(result * (mode == STATIC_LTM ? 1.0 : params.new_size.w));
	return mode != STATIC_LTM || energy > 0.0;
}

float sdf_distance(vec3 p, out uint nearest) {
	vec3 position = clamp((p - params.sdf_min_scale.xyz) / params.sdf_size_offset.xyz, 0.0, 1.0) * vec3(params.sdf_grid_color.xyz - 1u);
	ivec3 cell = clamp(ivec3(floor(position)), ivec3(0), ivec3(params.sdf_grid_color.xyz) - 2);
	vec3 f = clamp(position - vec3(cell), 0.0, 1.0);
	float result = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				uvec3 q = uvec3(cell + ivec3(x, y, z));
				uint at = (q.z * params.sdf_grid_color.y + q.y) * params.sdf_grid_color.x + q.x;
				uint bits = (sdf.words[uint(params.sdf_size_offset.w) + at / 2u] >> ((at % 2u) * 16u)) & 65535u;
				int value = int(bits << 16u) >> 16;
				vec3 w = mix(vec3(1.0) - f, f, vec3(x, y, z));
				result += float(value) / 32767.0 * params.sdf_min_scale.w * w.x * w.y * w.z;
			}
		}
	}
	uvec3 closest = uvec3(round(position));
	nearest = (closest.z * params.sdf_grid_color.y + closest.y) * params.sdf_grid_color.x + closest.x;
	return result;
}

vec3 surface_world(vec2 uv, float depth) {
	vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return (params.camera_to_world * vec4(view.xyz / view.w, 1.0)).xyz;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(params.screen_pass.xy)))) {
		return;
	}
	ivec3 output_pixel = ivec3(pixel, int(params.screen_pass.z));
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_pass.xy);
	uint mode = params.grid_mode.w;
	// Surface diagnostics validate the actual full-resolution receiver as well.
	// A nearby Gather representative may belong to a different receiving group.
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec4 guide = vec4(0.0);
	if (depth != 0.0) {
		vec3 world = surface_world(uv, depth);
		vec3 normal = normalize(mat3(params.camera_to_world) * (texelFetch(normal_texture, pixel, 0).xyz * 2.0 - 1.0));
		uvec3 packed = uvec3(round(clamp(normal * 0.5 + 0.5, 0.0, 1.0) * 255.0));
		guide = vec4(world, uintBitsToFloat(0x3f000000u | packed.x | (packed.y << 8) | (packed.z << 16)));
	}
	if (mode == INDIRECT || mode == PROBE_SAMPLING || mode == BLENDING) {
		uint profile_id = texelFetch(surface_profile_texture, pixel, 0).r;
		vec4 lighting = vec4(0.0);
		if (guide.w != 0.0) {
			bool reuse = false;
			if (mode == INDIRECT) {
				ivec3 representative = ivec3(ivec2(uv * vec2(textureSize(gather_lighting, 0).xy)), output_pixel.z);
				vec4 cached_guide = texelFetch(gather_guide, representative, 0);
				uint cached_profile = texelFetch(gather_profile, representative, 0).r;
				vec2 step_uv = 1.0 / vec2(params.screen_pass.xy);
				float footprint = max(0.001, length(surface_world(uv + vec2(step_uv.x, 0.0), depth) - guide.xyz) + length(surface_world(uv + vec2(0.0, step_uv.y), depth) - guide.xyz));
				reuse = cached_guide.w != 0.0 && cached_profile == profile_id && length(guide.xyz - cached_guide.xyz) <= footprint * 2.0 && dot(guide_normal(guide), guide_normal(cached_guide)) > 0.95;
				if (reuse) {
					lighting = texelFetch(gather_lighting, representative, 0);
				}
			}
			if (!reuse) {
				lighting = lrt_sample_observed(guide.xyz, guide_normal(guide), profile_id, true, mode == BLENDING);
				lighting.rgb *= params.new_size.w;
			}
		}
		vec3 color = display_light(lighting.rgb);
		if (mode == BLENDING) {
			color = guide.w != 0.0 && profile_id == 0xffffffffu ? vec3(0.5) : lighting.rgb / params.new_size.w;
		}
		imageStore(output_image, output_pixel, vec4(color, 1.0));
		return;
	}
	vec4 near_p = params.inv_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec4 far_p = params.inv_projection * vec4(uv * 2.0 - 1.0, 0.00001, 1.0);
	vec3 world_origin = (params.camera_to_world * vec4(near_p.xyz / near_p.w, 1.0)).xyz;
	vec3 world_end = (params.camera_to_world * vec4(far_p.xyz / far_p.w, 1.0)).xyz;
	vec3 world_ray = normalize(world_end - world_origin);
	float surface_depth = guide.w != 0.0 ? max(0.0, dot(guide.xyz - world_origin, world_ray)) : 1e30;
	vec3 color;
	float closest;
	if (params.screen_pass.w != 0u) {
		color = guide.w != 0.0 ? vec3(0.065 + 0.075 * abs(dot(guide_normal(guide), world_ray))) : vec3(0.025);
		closest = 1e30;
	} else {
		color = imageLoad(output_image, output_pixel).rgb;
		closest = imageLoad(output_depth, output_pixel).r;
	}
	vec3 origin = (params.world_to_local * vec4(world_origin, 1.0)).xyz;
	vec3 local_ray = mat3(params.world_to_local) * world_ray;
	float ray_scale = length(local_ray);
	vec3 ray = local_ray / ray_scale;
	float hit = 1e30;
	vec3 hit_color = color;
	float begin, end;
	if (mode == COLOR_SDF && all(greaterThan(params.sdf_grid_color.xyz, uvec3(1u)))) {
		vec3 minimum = params.sdf_min_scale.xyz;
		if (bounds_hit(origin, ray, minimum, minimum + params.sdf_size_offset.xyz, begin, end)) {
			vec3 cell_size = params.sdf_size_offset.xyz / vec3(params.sdf_grid_color.xyz - 1u);
			float step_size = min(cell_size.x, min(cell_size.y, cell_size.z)) * 0.25;
			for (float t = begin; t <= end; t += step_size) {
				uint at;
				if (sdf_distance(origin + ray * t, at) > 0.0) {
					continue;
				}
				float low = max(begin, t - step_size);
				float high = t;
				for (int i = 0; i < 6; i++) {
					float mid = (low + high) * 0.5;
					if (sdf_distance(origin + ray * mid, at) > 0.0) {
						low = mid;
					} else {
						high = mid;
					}
				}
				vec3 p = origin + ray * high;
				vec3 gradient;
				for (int axis = 0; axis < 3; axis++) {
					vec3 delta = vec3(0.0);
					delta[axis] = cell_size[axis] * 0.5;
					gradient[axis] = (sdf_distance(p + delta, at) - sdf_distance(p - delta, at)) / cell_size[axis];
				}
				sdf_distance(p, at);
				uint rgba = sdf.words[params.sdf_grid_color.w + at];
				vec3 material = vec3(rgba & 255u, (rgba >> 8u) & 255u, (rgba >> 16u) & 255u) / 255.0;
				vec3 normal = length(gradient) > 0.0 ? normalize(gradient) : -ray;
				float shading = 0.3 + 0.7 * max(dot(normal, -ray), 0.0);
				hit_color = pow(material * shading, vec3(1.0 / 2.2));
				hit = high / ray_scale;
				break;
			}
		}
	} else if (mode != COLOR_SDF && all(greaterThan(params.grid_mode.xyz, uvec3(1u)))) {
		float spacing = params.size_spacing.w;
		vec3 probe_min = -params.size_spacing.xyz * 0.5;
		vec3 minimum = probe_min - vec3(spacing * 0.5);
		vec3 maximum = probe_min + (vec3(params.grid_mode.xyz) - 0.5) * spacing;
		if (bounds_hit(origin, ray, minimum, maximum, begin, end)) {
			float cell_size = spacing * (mode == TRUNKS ? 8.0 : 1.0);
			ivec3 grid = mode == TRUNKS ? ivec3((params.grid_mode.xyz + 7u) / 8u) : ivec3(params.grid_mode.xyz);
			ivec3 cell = clamp(ivec3(floor((origin + ray * (begin + spacing * 0.0001) - minimum) / cell_size)), ivec3(0), grid - 1);
			ivec3 step_direction = ivec3(sign(ray));
			vec3 next = mix((minimum + (vec3(cell) + step(vec3(0.0), ray)) * cell_size - origin) / ray, vec3(1e30), equal(ray, vec3(0.0)));
			vec3 delta = abs(vec3(cell_size) / ray);
			for (int visited = 0; visited < grid.x + grid.y + grid.z; visited++) {
				if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, grid))) {
					break;
				}
				vec3 center = probe_min + vec3(cell) * spacing;
				float candidate = -1.0;
				vec3 candidate_color;
				uint probe = (uint(cell.z) * params.grid_mode.y + uint(cell.y)) * params.grid_mode.x + uint(cell.x);
				if (mode == TRUNKS) {
					vec3 box_min = minimum + vec3(cell) * cell_size;
					vec3 box_size = min(vec3(cell_size), maximum - box_min);
					candidate = wire_box(origin, ray, box_min, box_size, spacing * 0.012);
					uint index = (uint(cell.z) * uint(grid.y) + uint(cell.y)) * uint(grid.x) + uint(cell.x);
					candidate_color = trunk_color(diagnostic.words[index]);
				} else if (mode == EDGE_VIS) {
					uint direction = 0u;
					for (int z = -1; z <= 1; z++) {
						for (int y = -1; y <= 1; y++) {
							for (int x = -1; x <= 1; x++) {
								if (x == 0 && y == 0 && z == 0) {
									continue;
								}
								uint mask = (geometry.words[probe * 18u + 11u + direction / 4u] >> ((direction % 4u) * 8u)) & 15u;
								direction++;
								ivec3 neighbor = cell + ivec3(x, y, z);
								if (mask == 15u || any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, grid))) {
									continue;
								}
								uint neighbor_probe = (uint(neighbor.z) * params.grid_mode.y + uint(neighbor.y)) * params.grid_mode.x + uint(neighbor.x);
								if ((geometry.words[probe * 18u + 8u] & geometry.words[neighbor_probe * 18u + 8u] & 0x01000000u) != 0u) {
									continue;
								}
								// Each endpoint owns its half of the shared undirected edge.
								float t = ray_line(origin, ray, center, center + vec3(x, y, z) * spacing * 0.5, spacing * 0.008);
								if (t >= 0.0 && (candidate < 0.0 || t < candidate)) {
									candidate = t;
									candidate_color = edge_color(uint(bitCount(mask)));
								}
							}
						}
					}
				} else {
					float radius = spacing * (mode == PROBES ? 0.08 : 0.12);
					candidate = ray_sphere(origin, ray, center, radius);
					if (candidate >= 0.0) {
						vec3 normal = normalize(origin + ray * candidate - center);
						if (!probe_color(probe, normal, candidate_color)) {
							candidate = -1.0;
						} else if (mode == PROBES && max(candidate_color.r, max(candidate_color.g, candidate_color.b)) < 0.02 && dot(normal, -ray) < 0.5) {
							candidate_color = vec3(0.12);
						}
					}
				}
				float world_t = candidate / ray_scale;
				bool occluded = world_t > surface_depth + spacing * 0.01 / ray_scale;
				if (candidate >= 0.0 && world_t < hit && (!occluded || mode == EDGE_VIS || mode == TRUNKS)) {
					bool dashed = fract(candidate / spacing * 6.0) < 0.5;
					if (!occluded || dashed) {
						hit = world_t;
						hit_color = candidate_color * (occluded ? 0.2 : 1.0);
					}
				}
				int axis = next.x < next.y ? 0 : 1;
				if (next.z < next[axis]) {
					axis = 2;
				}
				if (next[axis] > end || next[axis] / ray_scale > hit) {
					break;
				}
				cell[axis] += step_direction[axis];
				next[axis] += delta[axis];
			}
		}
		if (mode == PROBES) {
			float border = wire_box(origin, ray, probe_min, params.size_spacing.xyz, spacing * 0.008);
			if (border >= 0.0 && border / ray_scale < min(hit, surface_depth)) {
				hit = border / ray_scale;
				hit_color = vec3(0.12, 0.4, 0.5);
			}
		}
		if (mode == TRUNKS) {
			float old_hit = wire_box(origin, ray, params.old_position.xyz, params.old_size.xyz, spacing * 0.018);
			if (old_hit >= 0.0 && old_hit / ray_scale < hit && fract(old_hit / spacing * 6.0) < 0.5) {
				hit = old_hit / ray_scale;
				hit_color = vec3(0.8, 0.65, 1.0);
			}
			float new_hit = wire_box(origin, ray, params.new_position.xyz, params.new_size.xyz, spacing * 0.018);
			if (new_hit >= 0.0 && new_hit / ray_scale < hit) {
				hit = new_hit / ray_scale;
				hit_color = vec3(0.8, 0.65, 1.0);
			}
		}
	}
	if (hit < closest) {
		closest = hit;
		color = hit_color;
	}
	// Trunk diagnostics overlay the rendered scene, including native editor gizmos.
	float alpha = mode == TRUNKS && closest >= 1e30 ? 0.0 : 1.0;
	imageStore(output_image, output_pixel, vec4(alpha > 0.0 ? color : vec3(0.0), alpha));
	imageStore(output_depth, output_pixel, vec4(closest));
}
