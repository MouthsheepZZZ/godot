#[compute]
#version 450
#VERSION_DEFINES
#include "../../../servers/rendering/renderer_rd/shaders/oct_inc.glsl"
#include "lrt_sh_inc.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D radiance;
layout(set = 0, binding = 1, std430) restrict writeonly buffer Sky {
	vec4 sh[3];
}
sky;
layout(push_constant, std430) uniform Params {
	mat4 orientation;
	vec4 ambient_energy;
	vec4 sky_energy; // rgb: radiance multiplier, w: sky mixture.
}
params;
void main() {
	vec4 result[3];
	for (uint c = 0; c < 3; c++) {
		result[c] = vec4(params.ambient_energy[c] * (1.0 - params.sky_energy.w) / LRT_C0, 0.0, 0.0, 0.0);
	}
	const uint SAMPLES = 256;
	for (uint i = 0; i < SAMPLES; i++) {
		float z = 1.0 - 2.0 * (float(i) + 0.5) / float(SAMPLES);
		float angle = float(i) * 2.399963229728653;
		vec3 direction = vec3(sqrt(1.0 - z * z) * cos(angle), z, sqrt(1.0 - z * z) * sin(angle));
		vec3 lookup = mat3(params.orientation) * direction;
		vec2 uv = vec3_to_oct_with_border(lookup, vec2(params.ambient_energy.w, 1.0 - 2.0 * params.ambient_energy.w));
		vec3 color = textureLod(radiance, uv, 0.0).rgb;
		color *= params.sky_energy.rgb * params.sky_energy.w;
		vec4 basis = vec4(LRT_C0, LRT_C1 * direction);
		for (uint c = 0; c < 3; c++) {
			result[c] += color[c] * basis * (12.566370614359172 / float(SAMPLES));
		}
	}
	for (uint c = 0; c < 3; c++) {
		sky.sh[c] = lrt_constrain_light(result[c]);
	}
}
