#[compute]
#version 450
#VERSION_DEFINES
#include "lrt_sh_inc.glsl"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) restrict readonly buffer Geometry {
	uint words[];
}
geometry;
layout(set = 0, binding = 1, std430) restrict readonly buffer Emission {
	vec4 sh[];
}
emission;
struct ProbeState {
	vec4 light[3];
	vec4 visibility;
};
layout(set = 0, binding = 2, std430) restrict readonly buffer History {
	ProbeState probes[];
}
history;
layout(set = 0, binding = 3, std430) restrict writeonly buffer Sources {
	vec4 sh[];
}
sources;
struct Light {
	vec4 position_type;
	vec4 direction_range;
	vec4 color_attenuation;
	vec4 spot_shadow;
	mat4 shadow_matrix[4];
	vec4 shadow_rect[4];
	vec4 bias;
	vec4 omni_offset;
};
layout(set = 0, binding = 4, std430) restrict readonly buffer Lights {
	Light data[];
}
lights;
layout(set = 0, binding = 5, std430) restrict readonly buffer Sky {
	vec4 sh[3];
}
sky;
layout(set = 0, binding = 6) uniform sampler2DShadow shadow_atlas;
layout(set = 0, binding = 7) uniform sampler2DShadow directional_atlas;
layout(set = 0, binding = 8, std140) uniform Volume {
	mat4 to_world;
	mat4 to_local;
	vec4 size_spacing;
	uvec4 grid_lights;
}
volume;
layout(push_constant, std430) uniform Params {
	uint offset;
	uint end;
	uint padding[2];
}
params;

float visibility(Light light, vec3 world, vec4 a, mat3 rotation) {
	if (light.spot_shadow.z == 0.0) {
		return 1.0;
	}
	if (light.position_type.w == 0.0) {
		for (int split = 0; split < int(light.spot_shadow.z); split++) {
			// Directional bias is a world-space distance toward the light, as in Forward+.
			vec3 biased_world = world - light.direction_range.xyz * light.bias[split];
			vec4 projected = light.shadow_matrix[split] * vec4(biased_world, 1.0);
			vec3 q = projected.xyz / projected.w;
			vec4 rect = light.shadow_rect[split];
			if (all(greaterThanEqual(q.xy, rect.xy)) && all(lessThanEqual(q.xy, rect.xy + rect.zw)) && q.z >= 0.0 && q.z <= 1.0) {
				return mix(1.0, textureLod(directional_atlas, q, 0.0), light.spot_shadow.w);
			}
		}
		vec3 toward_light = normalize(rotation * -light.direction_range.xyz);
		return mix(1.0, clamp(dot(a, vec4(LRT_C0, LRT_C1 * toward_light)), 0.0, 1.0), light.spot_shadow.w);
	}
	if (light.position_type.w == 1.0) {
		vec3 p = (light.shadow_matrix[0] * vec4(world, 1.0)).xyz;
		float distance_to_light = length(p);
		vec3 d = p / max(distance_to_light, 0.0001);
		vec4 rect = light.shadow_rect[0];
		if (d.z >= 0.0) {
			rect.xy += light.omni_offset.xy;
		}
		vec2 uv = rect.xy + (d.xy / (1.0 + abs(d.z)) * 0.5 + 0.5) * rect.zw;
		float depth = 1.0 - (distance_to_light - light.bias.x) / light.direction_range.w;
		return mix(1.0, textureLod(shadow_atlas, vec3(uv, depth), 0.0), light.spot_shadow.w);
	}
	vec4 projected = light.shadow_matrix[0] * vec4(world, 1.0);
	// Spot bias is applied in homogeneous shadow space before perspective division.
	projected.z += light.bias.x;
	vec3 q = projected.xyz / projected.w;
	if (projected.w <= 0.0 || any(lessThan(q.xy, vec2(0.0))) || any(greaterThan(q.xy, vec2(1.0))) || q.z < 0.0 || q.z > 1.0) {
		return 0.0;
	}
	vec4 rect = light.shadow_rect[0];
	return mix(1.0, textureLod(shadow_atlas, vec3(rect.xy + q.xy * rect.zw, q.z), 0.0), light.spot_shadow.w);
}
void main() {
	uint probe = params.offset + gl_GlobalInvocationID.x;
	if (probe >= params.end) {
		return;
	}
	uint base = probe * 18;
	uint flags = geometry.words[base + 8];
	if ((flags & 0x01000000u) != 0) {
		for (uint c = 0; c < 3; c++) {
			sources.sh[probe * 3 + c] = vec4(0.0);
		}
		return;
	}
	uvec3 grid = volume.grid_lights.xyz;
	uvec3 coordinate = uvec3(probe % grid.x, (probe / grid.x) % grid.y, probe / (grid.x * grid.y));
	vec3 local = -volume.size_spacing.xyz * 0.5 + vec3(coordinate) * volume.size_spacing.w;
	vec3 world = (volume.to_world * vec4(local, 1.0)).xyz;
	mat3 rotation = mat3(normalize(volume.to_local[0].xyz), normalize(volume.to_local[1].xyz), normalize(volume.to_local[2].xyz));
	vec4 a = lrt_constrain_visibility(history.probes[probe].visibility);
	vec4 incoming[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));
	for (uint i = 0; i < volume.grid_lights.w; i++) {
		Light light = lights.data[i];
		vec3 travel = light.direction_range.xyz;
		float attenuation = 1.0;
		if (light.position_type.w != 0.0) {
			travel = world - light.position_type.xyz;
			float distance_to_light = length(travel);
			if (distance_to_light >= light.direction_range.w) {
				continue;
			}
			travel /= max(distance_to_light, 0.0001);
			float nd = distance_to_light / light.direction_range.w;
			float window = max(1.0 - nd * nd * nd * nd, 0.0);
			attenuation = window * window * pow(max(distance_to_light, 0.0001), -light.color_attenuation.w);
			if (light.position_type.w == 2.0) {
				float cosine = max(dot(travel, light.direction_range.xyz), light.spot_shadow.x);
				float rim = max(0.0001, (1.0 - cosine) / (1.0 - light.spot_shadow.x));
				attenuation *= 1.0 - pow(rim, light.spot_shadow.y);
			}
		}
		attenuation *= visibility(light, world, a, rotation);
		vec4 basis = vec4(LRT_C0, LRT_C1 * (rotation * travel));
		for (uint c = 0; c < 3; c++) {
			incoming[c] += light.color_attenuation[c] * attenuation * basis;
		}
	}
	for (uint c = 0u; c < 3u; c++) {
		incoming[c] = lrt_constrain_light(incoming[c]);
	}
	mat4 transfer;
	for (uint col = 0; col < 4; col++) {
		transfer[col] = vec4(unpackHalf2x16(geometry.words[base + col * 2]), unpackHalf2x16(geometry.words[base + col * 2 + 1]));
	}
	vec3 color = vec3(float(flags & 255u), float((flags >> 8) & 255u), float((flags >> 16) & 255u)) / 255.0;
	for (uint c = 0; c < 3; c++) {
		vec4 local_sky = vec4(sky.sh[c].x, rotation * sky.sh[c].yzw);
		vec4 b = lrt_visible_sky(local_sky, a);
		vec4 reflected = lrt_reflect(color[c] * transfer, incoming[c] + b);
		sources.sh[probe * 3 + c] = reflected + lrt_constrain_diffuse(emission.sh[probe * 3 + c]);
	}
}
