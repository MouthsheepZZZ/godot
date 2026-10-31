#include "lrt_sh_inc.glsl"

vec4 lrt_read_sh(uint offset) {
	return uintBitsToFloat(uvec4(lrt_samples.words[offset], lrt_samples.words[offset + 1], lrt_samples.words[offset + 2], lrt_samples.words[offset + 3]));
}

float lrt_coverage(LRTVolume volume, vec3 world) {
	vec3 p = (volume.to_local * vec4(world, 1.0)).xyz;
	vec3 distance_to_edge = volume.size_spacing.xyz * 0.5 - abs(p);
	if (any(lessThan(distance_to_edge, vec3(0.0)))) {
		return 0.0;
	}
	float edge = min(distance_to_edge.x, min(distance_to_edge.y, distance_to_edge.z));
	float width = volume.display.y * length(volume.to_local[0].xyz);
	return width > 0.0 ? smoothstep(0.0, width, edge) : 1.0;
}

vec4 lrt_sample_grid(LRTVolume volume, vec3 world, vec3 world_normal, float normal_bias) {
	vec3 sample_world = world + normalize(world_normal) * normal_bias;
	vec3 sample_local = (volume.to_local * vec4(sample_world, 1.0)).xyz;
	vec3 position = (sample_local + volume.size_spacing.xyz * 0.5) / volume.size_spacing.w;
	ivec3 cell = clamp(ivec3(floor(position)), ivec3(0), ivec3(volume.grid_geometry.xyz) - 2);
	vec3 fraction = clamp(position - vec3(cell), vec3(0.0), vec3(1.0));
	vec4 light[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));
	vec4 a = vec4(0.0);
	float total = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				uvec3 coordinate = uvec3(cell + ivec3(x, y, z));
				uint probe = (coordinate.z * volume.grid_geometry.y + coordinate.y) * volume.grid_geometry.x + coordinate.x;
				uint flags = lrt_samples.words[volume.grid_geometry.w + probe];
				if ((flags & 0x01000000u) != 0u) {
					continue;
				}
				vec3 weight = mix(vec3(1.0) - fraction, fraction, vec3(x, y, z));
				float w = weight.x * weight.y * weight.z;
				uint offset = volume.history.x + probe * 16;
				for (uint c = 0; c < 3; c++) {
					light[c] += lrt_read_sh(offset + c * 4) * w;
				}
				a += lrt_read_sh(offset + 12) * w;
				total += w;
			}
		}
	}
	if (total <= 0.0) {
		return vec4(0.0);
	}
	a = lrt_constrain_visibility(a / total);
	mat3 rotation = mat3(normalize(volume.to_local[0].xyz), normalize(volume.to_local[1].xyz), normalize(volume.to_local[2].xyz));
	vec3 normal = normalize(rotation * world_normal);
	vec4 diffuse = vec4(LRT_C0, -(2.0 / 3.0) * LRT_C1 * normal);
	vec3 irradiance;
	for (uint c = 0; c < 3; c++) {
		vec4 sky = vec4(lrt.sky[c].x, rotation * lrt.sky[c].yzw);
		vec4 b = lrt_constrain_diffuse(lrt_visible_sky(sky, a));
		irradiance[c] = max(0.0, dot(lrt_constrain_diffuse(light[c] / total) + b, diffuse));
	}
	return vec4(irradiance, 1.0);
}

vec3 lrt_source_color(uint index) {
	uint parent = lrt.volumes[index].history.z;
	uint seed = lrt.volumes[parent == 0u ? index : parent - 1u].history.w;
	vec3 color = 0.25 + 0.75 * fract(vec3(seed & 255u, (seed >> 8u) & 255u, (seed >> 16u) & 255u) / 255.0);
	return color * (parent == 0u ? 1.0 : 0.55 + 0.1 * float((index - parent) % 4u));
}

vec4 lrt_sample_observed(vec3 world, vec3 world_normal, uint profile_id, bool observed, bool diagnostic) {
	if (profile_id >= lrt.profile_count) {
		return vec4(0.0); // Includes native-GI suppression (UINT32_MAX).
	}
	uint profile_offset = profile_id * 4u;
	float normal_bias = uintBitsToFloat(lrt_samples.words[profile_offset]);
	uvec2 scope = uvec2(lrt_samples.words[profile_offset + 1u], lrt_samples.words[profile_offset + 2u]);
	vec4 result = vec4(0.0);
	for (uint index = 0; index < lrt.count; index++) {
		LRTVolume parent = lrt.volumes[index];
		if ((observed && parent.display.z == 0.0) || (parent.history.z != 0u && !(observed && parent.display.w > 0.0))) {
			continue;
		}
		// Detail metadata carries its parent's identity, including isolated views.
		// Reject ineligible groups before coverage tests or any Probe reads.
		if (any(notEqual(scope, uvec2(0u))) && parent.receiver.z == 0u && any(notEqual(scope, parent.receiver.xy))) {
			continue;
		}
		float coverage = lrt_coverage(parent, world);
		if (coverage <= 0.0) {
			continue;
		}
		vec4 group = lrt_sample_grid(parent, world, world_normal, normal_bias);
		if (diagnostic) {
			group.rgb = lrt_source_color(index) * group.a;
		}
		for (uint detail_index = index + 1u; detail_index < lrt.count; detail_index++) {
			LRTVolume detail = lrt.volumes[detail_index];
			if (detail.history.z != index + 1u || (observed && parent.display.w > 0.0)) {
				break;
			}
			float weight = lrt_coverage(detail, world);
			if (weight <= 0.0) {
				continue;
			}
			vec4 sample_value = lrt_sample_grid(detail, world, world_normal, normal_bias);
			if (sample_value.a > 0.0) {
				if (diagnostic) {
					sample_value.rgb = lrt_source_color(detail_index);
				}
				group = mix(group, sample_value, weight);
			}
		}
		group *= coverage;
		if (!diagnostic) {
			group.rgb *= parent.display.x;
		}
		result = result * (1.0 - group.a) + group;
	}
	return result;
}

vec4 lrt_sample(vec3 world, vec3 world_normal, uint profile_id) {
	return lrt_sample_observed(world, world_normal, profile_id, false, false);
}
