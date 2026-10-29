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
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2DArray lighting_image;
layout(set = 0, binding = 5, rgba32f) uniform writeonly image2DArray guide_image;
layout(set = 0, binding = 6, r16f) uniform writeonly image2DArray bias_image;
layout(set = 0, binding = 7, std140) uniform Parameters {
	mat4 inv_projection;
	mat4 to_world;
	ivec4 full_size_view;
	ivec2 gather_size;
	float exposure;
	uint padding;
}
params;
layout(set = 0, binding = 8) uniform sampler2D surface_bias_texture;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.gather_size))) {
		return;
	}
	ivec3 output_pixel = ivec3(pixel, params.full_size_view.z);
	// Position, normal and bias come from the same actual full-resolution pixel.
	ivec2 representative = min(ivec2((vec2(pixel) + 0.5) * vec2(params.full_size_view.xy) / vec2(params.gather_size)), params.full_size_view.xy - 1);
	float depth = texelFetch(depth_texture, representative, 0).r;
	vec4 guide = vec4(0.0);
	vec4 lighting = vec4(0.0);
	float bias = texelFetch(surface_bias_texture, representative, 0).r;
	if (depth != 0.0) {
		vec2 uv = (vec2(representative) + 0.5) / vec2(params.full_size_view.xy);
		vec4 view = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
		vec3 world = (params.to_world * vec4(view.xyz / view.w, 1.0)).xyz;
		vec3 normal = normalize(mat3(params.to_world) * (texelFetch(normal_texture, representative, 0).xyz * 2.0 - 1.0));
		uvec3 packed = uvec3(round(clamp(normal * 0.5 + 0.5, 0.0, 1.0) * 255.0));
		guide = vec4(world, uintBitsToFloat(0x3f000000u | packed.x | (packed.y << 8) | (packed.z << 16)));
		// Use the same quantized normal as the surface and debug sampling paths.
		normal = normalize(vec3(packed) / 127.5 - 1.0);
		lighting = lrt_sample(world, normal, bias);
		lighting.rgb *= params.exposure;
	}
	imageStore(lighting_image, output_pixel, lighting);
	imageStore(guide_image, output_pixel, guide);
	imageStore(bias_image, output_pixel, vec4(bias));
}
