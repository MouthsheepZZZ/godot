#include "lrt_sh_inc.glsl"

vec4 lrt_read_sh(uint offset) {
	return uintBitsToFloat(uvec4(lrt_samples.words[offset], lrt_samples.words[offset + 1], lrt_samples.words[offset + 2], lrt_samples.words[offset + 3]));
}
vec4 lrt_sample(vec3 world, vec3 world_normal) {
	vec4 result = vec4(0.0);
	for (uint index = 0; index < lrt.count; index++) {
		LRTVolume volume = lrt.volumes[index];
		vec3 p = (volume.to_local * vec4(world, 1.0)).xyz;
		vec3 distance_to_edge = volume.size_spacing.xyz * 0.5 - abs(p);
		if (any(lessThan(distance_to_edge, vec3(0.0)))) {
			continue;
		}
		vec3 position = (p + volume.size_spacing.xyz * 0.5) / volume.size_spacing.w;
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
					uint flags = lrt_samples.words[volume.grid_geometry.w + probe * 18 + 8];
					if ((flags & 0x01000000u) != 0) {
						continue;
					}
					vec3 weight = mix(vec3(1.0) - fraction, fraction, vec3(x, y, z));
					float w = weight.x * weight.y * weight.z;
					uint start = volume.history.x + probe * 16;
					for (uint c = 0; c < 3; c++) {
						light[c] += lrt_read_sh(start + c * 4) * w;
					}
					a += lrt_read_sh(start + 12) * w;
					total += w;
				}
			}
		}
		if (total <= 0.0) {
			continue;
		}
		a /= total;
		mat3 rotation = mat3(normalize(volume.to_local[0].xyz), normalize(volume.to_local[1].xyz), normalize(volume.to_local[2].xyz));
		vec3 normal = normalize(rotation * world_normal);
		vec4 diffuse = vec4(LRT_C0, -(2.0 / 3.0) * LRT_C1 * normal);
		vec3 irradiance;
		for (uint c = 0; c < 3; c++) {
			vec4 sky = vec4(lrt.sky[c].x, rotation * lrt.sky[c].yzw);
			vec4 b = lrt_flip(lrt_product(sky, a));
			irradiance[c] = max(0.0, dot(light[c] / total + b, diffuse));
		}
		float edge = min(distance_to_edge.x, min(distance_to_edge.y, distance_to_edge.z));
		float blend_width = volume.display.y * volume.size_spacing.w;
		float coverage = blend_width > 0.0 ? smoothstep(0.0, blend_width, edge) : 1.0;
		result = mix(result, vec4(irradiance * volume.display.x, 1.0), coverage);
	}
	return result;
}
