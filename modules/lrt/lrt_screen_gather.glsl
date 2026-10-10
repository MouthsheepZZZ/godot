#[compute]

#version 450

// Opaque receivers are sampled at their actual depth, including subpixel silhouettes.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct LRTData {
	mat4 world_to_volume;
	vec4 volume_min;
	vec4 volume_max;
	vec4 grid_min_spacing;
	ivec4 grid_size_mode;
	vec4 atlas_flags;
};

layout(set = 0, binding = 0, std140) uniform GatherDataBlock {
	mat4 inv_projection;
	mat4 view_to_world;
	ivec4 screen_size;
}
gather;

layout(set = 0, binding = 1, std140) uniform LRTDataBlock {
	LRTData data;
}
lrt;

layout(set = 0, binding = 2) uniform texture2D lrt_radiance_r;
layout(set = 0, binding = 3) uniform texture2D lrt_radiance_g;
layout(set = 0, binding = 4) uniform texture2D lrt_radiance_b;
layout(set = 0, binding = 5) uniform texture2D lrt_material;
layout(set = 0, binding = 6) uniform texture2D lrt_receiver_links;
layout(set = 0, binding = 7) uniform texture2D lrt_sky_r;
layout(set = 0, binding = 8) uniform texture2D lrt_sky_g;
layout(set = 0, binding = 9) uniform texture2D lrt_sky_b;
layout(set = 0, binding = 10) uniform texture2D depth_buffer;
layout(set = 0, binding = 11) uniform texture2D normal_roughness_buffer;
layout(rgba16f, set = 0, binding = 12) uniform restrict image2D gather_lighting;
layout(rgba16f, set = 0, binding = 13) uniform restrict writeonly image2D gather_geometry;
layout(set = 0, binding = 14) uniform sampler nearest_sampler;

layout(push_constant, std430) uniform Composition {
	ivec4 first_volume;
	ivec4 region;
} composition;

vec4 lrt_fetch(LRTData data, int field, ivec3 cell) {
	ivec2 coord = ivec2(cell.x + cell.z * data.grid_size_mode.x, cell.y);
	if (field == 0) { return texelFetch(sampler2D(lrt_radiance_r, nearest_sampler), coord, 0); }
	if (field == 1) { return texelFetch(sampler2D(lrt_radiance_g, nearest_sampler), coord, 0); }
	if (field == 2) { return texelFetch(sampler2D(lrt_radiance_b, nearest_sampler), coord, 0); }
	if (field == 3) { return texelFetch(sampler2D(lrt_material, nearest_sampler), coord, 0); }
	if (field == 4) { return texelFetch(sampler2D(lrt_receiver_links, nearest_sampler), coord, 0); }
	if (field == 5) { return texelFetch(sampler2D(lrt_sky_r, nearest_sampler), coord, 0); }
	if (field == 6) { return texelFetch(sampler2D(lrt_sky_g, nearest_sampler), coord, 0); }
	if (field == 7) { return texelFetch(sampler2D(lrt_sky_b, nearest_sampler), coord, 0); }
	return vec4(0.0);
}

#include "lrt_sampling_inc.glsl"

void main() {
	ivec2 region_coord = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(region_coord, composition.region.zw))) {
		return;
	}
	ivec2 gather_coord = region_coord + composition.region.xy;
	ivec2 full_coord = min(gather_coord, gather.screen_size.xy - ivec2(1));
	vec4 packed_normal = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), full_coord, 0);
	if (all(lessThan(abs(packed_normal.xyz), vec3(0.00001)))) {
		imageStore(gather_lighting, gather_coord, vec4(0.0));
		imageStore(gather_geometry, gather_coord, vec4(0.0));
		return;
	}
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), full_coord, 0).r;
	vec2 ndc_xy = (2.0 * (vec2(full_coord) + vec2(0.5)) / vec2(gather.screen_size.xy)) - 1.0;
	vec4 view_position_h = gather.inv_projection * vec4(ndc_xy, depth, 1.0);
	vec3 view_position = view_position_h.xyz / view_position_h.w;
	vec3 view_normal = normalize(packed_normal.xyz * 2.0 - 1.0);
	vec3 world_position = (gather.view_to_world * vec4(view_position, 1.0)).xyz;
	vec3 world_normal = normalize(mat3(gather.view_to_world) * view_normal);
	vec3 ambient_light;
	float blend_weight;
	lrt_sample_native(lrt.data, world_position, world_normal, ambient_light, blend_weight);
	vec4 lower = composition.first_volume.x != 0 ? vec4(0.0) : imageLoad(gather_lighting, gather_coord);
	vec4 composed = vec4(ambient_light * blend_weight, blend_weight) + lower * (1.0 - blend_weight);
	imageStore(gather_lighting, gather_coord, composed);
	imageStore(gather_geometry, gather_coord, vec4(view_normal, view_position.z));
}
