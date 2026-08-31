#[compute]

#version 450

#VERSION_DEFINES

#include "../oct_inc.glsl"

#define SH_Y00 0.28209479177387814
#define SH_Y1 0.4886025119029199
#define SH_FOUR_PI 12.566370614359172

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

#ifdef USE_OCTMAP_ARRAY
layout(set = 0, binding = 0) uniform sampler2DArray sky_irradiance;
#else
layout(set = 0, binding = 0) uniform sampler2D sky_irradiance;
#endif

layout(set = 0, binding = 1, std430) restrict readonly buffer EnvironmentData {
	vec4 values[4];
}
environment_data;

layout(set = 0, binding = 2, std430) restrict writeonly buffer EnvironmentSH {
	vec4 values[3];
}
environment_sh;

vec4 sh_basis(vec3 direction) {
	vec3 n = normalize(direction);
	return vec4(SH_Y00, SH_Y1 * n.x, SH_Y1 * n.y, SH_Y1 * n.z);
}

float neighbor_weight(ivec3 offset) {
	const float normalization = 6.0 + 12.0 / sqrt(2.0) + 8.0 / sqrt(3.0);
	return (1.0 / length(vec3(offset))) / normalization;
}

void main() {
	vec3 ambient_color = environment_data.values[0].rgb;
	float sky_mix = environment_data.values[0].a;
	mat3 world_to_sky = mat3(
			environment_data.values[1].xyz,
			environment_data.values[2].xyz,
			environment_data.values[3].xyz);
	float sky_energy = environment_data.values[1].a;
	vec2 border_size = vec2(environment_data.values[2].a, environment_data.values[3].a);

	vec4 result_r = vec4(0.0);
	vec4 result_g = vec4(0.0);
	vec4 result_b = vec4(0.0);
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 offset = ivec3(x, y, z);
				if (all(equal(offset, ivec3(0)))) {
					continue;
				}
				vec3 world_direction = normalize(vec3(offset));
				vec3 sky_direction = normalize(world_to_sky * world_direction);
#ifdef USE_OCTMAP_ARRAY
				vec3 sky_color = textureLod(sky_irradiance, vec3(vec3_to_oct_with_border(sky_direction, border_size), 0.0), 2.0).rgb * sky_energy;
#else
				vec3 sky_color = textureLod(sky_irradiance, vec3_to_oct_with_border(sky_direction, border_size), 2.0).rgb * sky_energy;
#endif
				vec3 radiance = mix(ambient_color, sky_color, sky_mix);
				vec4 weighted_basis = sh_basis(world_direction) * (SH_FOUR_PI * neighbor_weight(offset));
				result_r += weighted_basis * radiance.r;
				result_g += weighted_basis * radiance.g;
				result_b += weighted_basis * radiance.b;
			}
		}
	}
	environment_sh.values[0] = result_r;
	environment_sh.values[1] = result_g;
	environment_sh.values[2] = result_b;
}
