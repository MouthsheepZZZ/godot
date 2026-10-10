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
layout(set = 0, binding = 6) uniform utexture2D lrt_receiver_links;
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
	if (field == 5) { return texelFetch(sampler2D(lrt_sky_r, nearest_sampler), coord, 0); }
	if (field == 6) { return texelFetch(sampler2D(lrt_sky_g, nearest_sampler), coord, 0); }
	if (field == 7) { return texelFetch(sampler2D(lrt_sky_b, nearest_sampler), coord, 0); }
	return vec4(0.0);
}

uint lrt_fetch_links(LRTData data, ivec3 cell) {
	ivec2 coord = ivec2(cell.x + cell.z * data.grid_size_mode.x, cell.y);
	return texelFetch(usampler2D(lrt_receiver_links, nearest_sampler), coord, 0).r;
}

#include "lrt_sampling_inc.glsl"

const float LRT_SURFACE_RECONSTRUCTION_COVERAGE = 0.2;
const float LRT_SURFACE_RECONSTRUCTION_BLEND = 0.45;

vec3 lrt_view_position(ivec2 coord, float depth) {
	vec2 ndc = 2.0 * (vec2(coord) + 0.5) / vec2(gather.screen_size.xy) - 1.0;
	vec4 position = gather.inv_projection * vec4(ndc, depth, 1.0);
	return position.xyz / position.w;
}

bool lrt_fill_surface_hole(vec3 world_position, vec3 world_normal, out vec3 lighting) {
	const int sample_count = 32;
	const float golden_angle = 2.39996323;
	float spacing = lrt.data.grid_min_spacing.w;
	vec3 local_position = (lrt.data.world_to_volume * vec4(world_position, 1.0)).xyz;
	vec3 local_normal = normalize(mat3(lrt.data.world_to_volume) * world_normal);
	vec3 axis = abs(local_normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(axis, local_normal));
	vec3 bitangent = cross(local_normal, tangent);
	mat4 volume_to_clip = inverse(gather.inv_projection) * inverse(gather.view_to_world) * inverse(lrt.data.world_to_volume);
	lighting = vec3(0.0);
	float total = 0.0;
	// A uniform disk on the receiver plane avoids screen-aligned rings and perspective distortion.
	for (int index = 0; index < sample_count; index++) {
		float radius = 1.5 * spacing * sqrt((float(index) + 0.5) / float(sample_count));
		float angle = float(index) * golden_angle;
		vec3 target = local_position + radius * (cos(angle) * tangent + sin(angle) * bitangent);
		vec4 clip = volume_to_clip * vec4(target, 1.0);
		if (clip.w <= 0.0) {
			continue;
		}
		ivec2 sample_coord = ivec2(floor((clip.xy / clip.w * 0.5 + 0.5) * vec2(gather.screen_size.xy)));
		if (any(lessThan(sample_coord, ivec2(0))) || any(greaterThanEqual(sample_coord, gather.screen_size.xy))) {
			continue;
		}
		vec3 packed_normal = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), sample_coord, 0).xyz;
		if (all(lessThan(abs(packed_normal), vec3(0.00001)))) {
			continue;
		}
		vec3 sample_normal = normalize(mat3(gather.view_to_world) * normalize(packed_normal * 2.0 - 1.0));
		if (dot(sample_normal, world_normal) < 0.98) {
			continue;
		}
		float sample_depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), sample_coord, 0).r;
		vec3 sample_position = (gather.view_to_world * vec4(lrt_view_position(sample_coord, sample_depth), 1.0)).xyz;
		vec3 delta = mat3(lrt.data.world_to_volume) * (sample_position - world_position);
		float distance_squared = dot(delta, delta) / (spacing * spacing);
		if (abs(dot(delta, local_normal)) > spacing * 0.05 || distance_squared > 2.25) {
			continue;
		}
		vec3 sample_light;
		float sample_blend;
		float sample_coverage;
		if (!lrt_sample_native(lrt.data, sample_position, world_normal, sample_light, sample_blend, sample_coverage)) {
			continue;
		}
		float support = smoothstep(0.0, LRT_SURFACE_RECONSTRUCTION_COVERAGE, sample_coverage);
		float edge = 1.0 - distance_squared / 2.25;
		float weight = sample_blend * support * exp(-distance_squared / 1.125) * edge * edge;
		lighting += sample_light * weight;
		total += weight;
	}
	if (total <= 0.0) {
		return false;
	}
	lighting /= total;
	return true;
}

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
	vec3 view_position = lrt_view_position(full_coord, depth);
	vec3 view_normal = normalize(packed_normal.xyz * 2.0 - 1.0);
	vec3 world_position = (gather.view_to_world * vec4(view_position, 1.0)).xyz;
	vec3 world_normal = normalize(mat3(gather.view_to_world) * view_normal);
	vec3 ambient_light;
	float blend_weight;
	float reconstruction_coverage;
	lrt_sample_native(lrt.data, world_position, world_normal, ambient_light, blend_weight, reconstruction_coverage);
	if (reconstruction_coverage < LRT_SURFACE_RECONSTRUCTION_BLEND && blend_weight > 0.0 && lrt.data.atlas_flags.z >= 0.5) {
		vec3 surface_lighting;
		if (lrt_fill_surface_hole(world_position, world_normal, surface_lighting)) {
			float support = smoothstep(0.0, LRT_SURFACE_RECONSTRUCTION_BLEND, reconstruction_coverage);
			ambient_light = mix(surface_lighting, ambient_light, support);
		}
	}
	vec4 lower = composition.first_volume.x != 0 ? vec4(0.0) : imageLoad(gather_lighting, gather_coord);
	vec4 composed = vec4(ambient_light * blend_weight, blend_weight) + lower * (1.0 - blend_weight);
	imageStore(gather_lighting, gather_coord, composed);
	imageStore(gather_geometry, gather_coord, vec4(view_normal, view_position.z));
}
