#[compute]
#version 450

#VERSION_DEFINES

#define MAX_VIEWS 2
#include "../../servers/rendering/renderer_rd/shaders/oct_inc.glsl"
#include "../../servers/rendering/renderer_rd/shaders/scene_data_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, std140) uniform Scene {
	SceneData data;
}
scene;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(push_constant, std430) uniform Params {
	float distance;
	float thickness;
	uint block_skipping;
	uint divisor;
	uint view;
	uint views;
	uint lights;
	uint pad;
}
params;

mat4 get_projection() {
	return params.views > 1u ? scene.data.projection_matrix_view[params.view] : scene.data.projection_matrix;
}
mat4 get_inverse_projection() {
	return params.views > 1u ? scene.data.inv_projection_matrix_view[params.view] : scene.data.inv_projection_matrix;
}
vec3 position_at(ivec2 pixel) {
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(textureSize(depth_texture, 0));
	vec4 position = get_inverse_projection() * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return position.xyz / position.w;
}

#ifdef MODE_BLOCKS
layout(rg32f, set = 1, binding = 0) uniform restrict writeonly image2D blocks;
shared vec2 ranges[64];
void main() {
	ivec2 size = textureSize(depth_texture, 0);
	ivec2 origin = ivec2(gl_WorkGroupID.xy) * 8 - 1;
	vec2 bounds = vec2(1e20, -1e20);
	for (int y = int(gl_LocalInvocationID.y); y < 10; y += 8) {
		for (int x = int(gl_LocalInvocationID.x); x < 10; x += 8) {
			ivec2 pixel = clamp(origin + ivec2(x, y), ivec2(0), size - 1);
			if (texelFetch(depth_texture, pixel, 0).r > 0.0) {
				float z = position_at(pixel).z;
				bounds = vec2(min(bounds.x, z), max(bounds.y, z));
			}
		}
	}
	uint lane = gl_LocalInvocationIndex;
	ranges[lane] = bounds;
	barrier();
	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (lane < stride) {
			ranges[lane] = vec2(min(ranges[lane].x, ranges[lane + stride].x), max(ranges[lane].y, ranges[lane + stride].y));
		}
		barrier();
	}
	if (lane == 0u) {
		imageStore(blocks, ivec2(gl_WorkGroupID.xy), vec4(ranges[0], 0.0, 0.0));
	}
}
#endif

#ifdef MODE_SURFACE
layout(rgba32f, set = 1, binding = 0) uniform restrict writeonly image2D receivers;
layout(rg16f, set = 1, binding = 1) uniform restrict writeonly image2D normals;
void main() {
	ivec2 output_pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(output_pixel, imageSize(receivers)))) {
		return;
	}
	ivec2 size = textureSize(depth_texture, 0);
	ivec2 pixel = output_pixel * int(params.divisor);
	float nearest = 0.0;
	ivec2 selected = pixel;
	for (int y = 0; y < int(params.divisor); y++) {
		for (int x = 0; x < int(params.divisor); x++) {
			ivec2 candidate = min(pixel + ivec2(x, y), size - 1);
			float depth = texelFetch(depth_texture, candidate, 0).r;
			if (depth > nearest) {
				nearest = depth;
				selected = candidate;
			}
		}
	}
	if (nearest == 0.0) {
		imageStore(receivers, output_pixel, vec4(0.0));
		imageStore(normals, output_pixel, vec4(0.5));
		return;
	}
	vec3 position = position_at(selected);
	// Choose the continuous neighbor on each axis rather than differentiating across silhouettes.
	vec3 l = position - position_at(max(selected - ivec2(1, 0), ivec2(0)));
	vec3 r = position_at(min(selected + ivec2(1, 0), size - 1)) - position;
	vec3 u = position - position_at(max(selected - ivec2(0, 1), ivec2(0)));
	vec3 d = position_at(min(selected + ivec2(0, 1), size - 1)) - position;
	vec3 dx = abs(l.z) < abs(r.z) ? l : r;
	vec3 dy = abs(u.z) < abs(d.z) ? u : d;
	vec3 normal = cross(dx, dy);
	// At a one-pixel isolated surface there is no reconstructible tangent plane.
	normal = dot(normal, normal) > 1e-20 ? normalize(normal) : normalize(-position);
	if (dot(normal, -position) < 0.0) {
		normal = -normal;
	}
	imageStore(receivers, output_pixel, vec4(position, 1.0));
	imageStore(normals, output_pixel, vec4(vec3_to_oct(normal), 0.0, 0.0));
}
#endif

#ifdef MODE_TRACE
layout(set = 0, binding = 2) uniform sampler2D block_texture;
layout(set = 0, binding = 3) uniform sampler2D receiver_texture;
layout(set = 0, binding = 4) uniform sampler2D normal_texture;
struct Light {
	vec4 position_range;
	vec4 direction_type;
	vec4 cone;
};
layout(set = 0, binding = 5, std430) readonly buffer Lights {
	Light data[];
}
lights;
layout(r8, set = 1, binding = 0) uniform restrict writeonly image2DArray shadows;

#include "shadow_trace_inc.glsl"

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, textureSize(receiver_texture, 0)))) {
		return;
	}
	uint light_index = gl_GlobalInvocationID.z;
	Light light = lights.data[light_index];
	vec4 receiver = texelFetch(receiver_texture, pixel, 0);
	float visibility = 1.0;
	if (receiver.w > 0.0) {
		vec3 normal = oct_to_vec3(texelFetch(normal_texture, pixel, 0).rg * 2.0 - 1.0);
		vec3 eye_offset = params.views > 1u ? scene.data.eye_offset[params.view].xyz : vec3(0.0);
		vec3 delta = light.position_range.xyz - eye_offset - receiver.xyz;
		float distance = light.direction_type.w == 0.0 ? params.distance : length(delta);
		vec3 direction = light.direction_type.w == 0.0 ? light.direction_type.xyz : delta / max(distance, 1e-8);
		bool in_range = light.direction_type.w == 0.0 || distance < light.position_range.w;
		bool in_cone = light.direction_type.w != 2.0 || dot(-direction, light.direction_type.xyz) > light.cone.x;
		if (in_range && in_cone) {
			visibility = trace_shadow(receiver.xyz, normal, direction, distance);
		}
	}
	imageStore(shadows, ivec3(pixel, light_index * params.views + params.view), vec4(visibility));
}
#endif
