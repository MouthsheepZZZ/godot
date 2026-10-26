#[compute]
#version 450

#VERSION_DEFINES
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#define LRT_DATA_SET 0
#define LRT_METADATA_BINDING 0
#define LRT_SAMPLES_BINDING 1
#include "lrt_data_inc.glsl"
#include "lrt_sample_inc.glsl"

layout(set = 0, binding = 2) uniform sampler2D depth_texture;
layout(set = 0, binding = 3) uniform sampler2D normal_texture;
layout(set = 0, binding = 4, rgba16f) uniform image2DArray low_lighting;
layout(set = 0, binding = 5, rgba32f) uniform image2DArray low_guide;
layout(set = 0, binding = 6, rgba16f) uniform writeonly image2DArray full_lighting;
layout(set = 0, binding = 7, rgba32f) uniform writeonly image2DArray full_guide;
layout(set = 0, binding = 8, std140) uniform Parameters {
	mat4 inv_projection;
	mat4 to_world;
	ivec4 full_size_view;
	ivec2 low_size;
	float exposure;
	uint padding;
}
params;

vec4 surface(ivec2 pixel) {
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec3 normal = texelFetch(normal_texture, pixel, 0).xyz * 2.0 - 1.0;
	if (depth == 0.0) {
		return vec4(0.0);
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.full_size_view.xy);
	vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 world = (params.to_world * vec4(view.xyz / view.w, 1.0)).xyz;
	normal = normalize(mat3(params.to_world) * normal);
	uvec3 packed = uvec3(round(clamp(normal * 0.5 + 0.5, 0.0, 1.0) * 255.0));
	return vec4(world, uintBitsToFloat(0x3f000000u | packed.x | (packed.y << 8) | (packed.z << 16)));
}
vec3 guide_normal(vec4 guide) {
	uint p = floatBitsToUint(guide.w);
	return normalize(vec3(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u) / 127.5 - 1.0);
}
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	int layer = params.full_size_view.z;
#ifdef MODE_GATHER
	if (any(greaterThanEqual(pixel, params.low_size.xy))) {
		return;
	}
	// Both attributes come from the same actual full-resolution pixel.
	ivec2 representative = min(ivec2((vec2(pixel) + 0.5) * vec2(params.full_size_view.xy) / vec2(params.low_size.xy)), params.full_size_view.xy - 1);
	vec4 guide = surface(representative);
	vec4 lighting = guide.w != 0.0 ? lrt_sample(guide.xyz, guide_normal(guide)) : vec4(0.0);
	lighting.rgb *= params.exposure;
	imageStore(low_lighting, ivec3(pixel, layer), lighting);
	imageStore(low_guide, ivec3(pixel, layer), guide);
#else
	if (any(greaterThanEqual(pixel, params.full_size_view.xy))) {
		return;
	}
	vec4 guide = surface(pixel);
	vec4 lighting = vec4(0.0);
	if (guide.w != 0.0) {
		vec3 n = guide_normal(guide);
		vec2 coordinate = (vec2(pixel) + 0.5) * vec2(params.low_size.xy) / vec2(params.full_size_view.xy) - 0.5;
		ivec2 cell = ivec2(floor(coordinate));
		vec2 f = fract(coordinate);
		float total = 0.0;
		float depth = texelFetch(depth_texture, pixel, 0).r;
		vec2 ndc = (vec2(pixel) + 0.5) / vec2(params.full_size_view.xy) * 2.0 - 1.0;
		vec4 center = params.inv_projection * vec4(ndc, depth, 1.0);
		vec4 horizontal = params.inv_projection * vec4(ndc + vec2(4.0 / float(params.low_size.x), 0.0), depth, 1.0);
		vec4 vertical = params.inv_projection * vec4(ndc + vec2(0.0, 4.0 / float(params.low_size.y)), depth, 1.0);
		float radius = max(0.001, max(length(horizontal.xyz / horizontal.w - center.xyz / center.w), length(vertical.xyz / vertical.w - center.xyz / center.w)));
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				ivec3 q = ivec3(clamp(cell + ivec2(x, y), ivec2(0), params.low_size.xy - 1), layer);
				vec4 candidate = imageLoad(low_guide, q);
				if (candidate.w == 0.0) {
					continue;
				}
				vec3 delta = candidate.xyz - guide.xyz;
				float plane_distance = abs(dot(delta, n));
				float normal_weight = pow(max(dot(guide_normal(candidate), n), 0.0), 32.0);
				vec2 bilinear = mix(vec2(1.0) - f, f, vec2(x, y));
				float weight = bilinear.x * bilinear.y * normal_weight * exp(-plane_distance * plane_distance / (radius * radius));
				lighting += imageLoad(low_lighting, q) * weight;
				total += weight;
			}
		}
		if (total > 0.0001) {
			lighting /= total;
		} else {
			// No matching sample is an explicitly supported direct-sampling case.
			lighting = lrt_sample(guide.xyz, n);
			lighting.rgb *= params.exposure;
		}
	}
	imageStore(full_lighting, ivec3(pixel, layer), lighting);
	imageStore(full_guide, ivec3(pixel, layer), guide);
#endif
}
