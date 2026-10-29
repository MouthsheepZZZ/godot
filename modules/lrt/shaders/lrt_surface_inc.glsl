#include "lrt_sample_inc.glsl"

vec3 lrt_ambient(vec3 world, vec3 world_normal, vec2 uv, vec3 original, float normal_bias) {
	if (lrt.count == 0u) {
		return original;
	}
	ivec3 pixel = ivec3(ivec2(uv * vec2(textureSize(sampler2DArray(lrt_gather, SAMPLER_NEAREST_CLAMP), 0).xy)), ViewIndex);
	vec4 guide = texelFetch(sampler2DArray(lrt_gather_guide, SAMPLER_NEAREST_CLAMP), pixel, 0);
	uint packed_normal = floatBitsToUint(guide.w);
	vec3 normal = normalize(vec3(packed_normal & 255u, (packed_normal >> 8) & 255u, (packed_normal >> 16) & 255u) / 127.5 - 1.0);
	float footprint = max(0.001, length(dFdx(world)) + length(dFdy(world)));
	float bias = texelFetch(sampler2DArray(lrt_gather_bias, SAMPLER_NEAREST_CLAMP), pixel, 0).r;
	float surface_bias = unpackHalf2x16(packHalf2x16(vec2(normal_bias, 0.0))).x;
	vec4 lighting;
	// Transparent and unmatched pixels use the same formal direct-Probe path.
	if (packed_normal != 0u && bias == surface_bias && length(world - guide.xyz) <= footprint * 2.0 && dot(normal, world_normal) > 0.95) {
		lighting = texelFetch(sampler2DArray(lrt_gather, SAMPLER_NEAREST_CLAMP), pixel, 0);
	} else {
		lighting = lrt_sample(world, world_normal, normal_bias);
		lighting.rgb *= scene_data_block.data.emissive_exposure_normalization;
	}
	return original * (1.0 - lighting.a) + lighting.rgb;
}
