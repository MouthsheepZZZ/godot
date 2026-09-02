#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 8

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D depth_buffer;
layout(set = 0, binding = 1) uniform texture2D normal_roughness_buffer;
layout(set = 0, binding = 2) uniform sampler linear_sampler;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D gather_buffer;
layout(r16f, set = 0, binding = 4) uniform restrict writeonly image2D gather_weight_buffer;

layout(set = 0, binding = 5, std140) uniform GatherSceneData {
	mat4 inv_projection;
	mat4 camera_transform;
	ivec2 screen_size;
	ivec2 gather_size;
}
gather_scene;

struct LocalLRTVolumeData {
	mat4 world_to_local;
	vec3 size;
	float edge_blend_distance;
	ivec3 resolution;
	uint enabled;
	vec4 energy_pad;
};

#define LOCAL_LRT_MAX_SURFACE_VOLUMES 8

layout(set = 0, binding = 6, std140) uniform LocalLRTDataBlock {
	LocalLRTVolumeData volume0;
	LocalLRTVolumeData volume1;
	LocalLRTVolumeData volume2;
	LocalLRTVolumeData volume3;
	LocalLRTVolumeData volume4;
	LocalLRTVolumeData volume5;
	LocalLRTVolumeData volume6;
	LocalLRTVolumeData volume7;
}
local_lrt_data;

#define LOCAL_LRT_DECLARE_VOLUME_BUFFERS(N, DIRECT_BINDING, INDIRECT_BINDING, VIS_BINDING, SOLID_BINDING) \
	layout(set = 0, binding = DIRECT_BINDING, std430) restrict readonly buffer LocalLRTDirectRadiance##N { \
		vec4 values[];                                                                                \
	}                                                                                                 \
	local_lrt_direct_radiance_##N;                                                                    \
	layout(set = 0, binding = INDIRECT_BINDING, std430) restrict readonly buffer LocalLRTIndirectRadiance##N { \
		vec4 values[];                                                                                \
	}                                                                                                 \
	local_lrt_indirect_radiance_##N;                                                                  \
	layout(set = 0, binding = VIS_BINDING, std430) restrict readonly buffer LocalLRTGlobalVisibility##N { \
		vec4 values[];                                                                                \
	}                                                                                                 \
	local_lrt_visibility_##N;                                                                         \
	layout(set = 0, binding = SOLID_BINDING, std430) restrict readonly buffer LocalLRTInsideSolid##N { \
		uint values[];                                                                                \
	}                                                                                                 \
	local_lrt_inside_solid_##N;

LOCAL_LRT_DECLARE_VOLUME_BUFFERS(0, 7, 8, 9, 10)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(1, 11, 12, 13, 14)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(2, 15, 16, 17, 18)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(3, 19, 20, 21, 22)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(4, 23, 24, 25, 26)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(5, 27, 28, 29, 30)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(6, 31, 32, 33, 34)
LOCAL_LRT_DECLARE_VOLUME_BUFFERS(7, 35, 36, 37, 38)

#define LOCAL_LRT_ONLY
#include "../scene_forward_gi_inc.glsl"

void main() {
	ivec2 gather_position = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(gather_position, gather_scene.gather_size))) {
		return;
	}

	ivec2 screen_position = min(gather_position * 2 + ivec2(1), gather_scene.screen_size - ivec2(1));
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), screen_position, 0).r;
	vec4 normal_roughness = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), screen_position, 0);
	if (depth <= 0.0 || dot(normal_roughness.xyz, normal_roughness.xyz) <= 0.000001) {
		imageStore(gather_buffer, gather_position, vec4(0.0, 0.0, 0.0, 1.0));
		imageStore(gather_weight_buffer, gather_position, vec4(0.0));
		return;
	}

	vec2 uv = (vec2(screen_position) + vec2(0.5)) / vec2(gather_scene.screen_size);
	vec4 view_position = gather_scene.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	view_position /= view_position.w;
	vec3 world_position = (gather_scene.camera_transform * vec4(view_position.xyz, 1.0)).xyz;
	vec3 view_normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
	vec3 world_normal = normalize(mat3(gather_scene.camera_transform) * view_normal);

	float sky_visibility;
	vec4 local_lrt = local_lrt_compute(world_position, world_normal, sky_visibility);
	imageStore(gather_buffer, gather_position, vec4(local_lrt.rgb, sky_visibility));
	imageStore(gather_weight_buffer, gather_position, vec4(local_lrt.a));
}
