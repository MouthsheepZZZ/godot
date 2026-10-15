#[versions]

nearest = "#define MODE_NEAREST";
linear = "";
cubic = "#define MODE_CUBIC";

#[compute]

#version 450

#VERSION_DEFINES

// Sample the world-space filtered field at every opaque receiver.

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

layout(set = 0, binding = 2) uniform texture3D lrt_radiance_r;
layout(set = 0, binding = 3) uniform texture3D lrt_radiance_g;
layout(set = 0, binding = 4) uniform texture3D lrt_radiance_b;
layout(set = 0, binding = 5) uniform texture3D lrt_support;
layout(set = 0, binding = 9) uniform texture2D depth_buffer;
layout(set = 0, binding = 10) uniform texture2D normal_roughness_buffer;
layout(rgba16f, set = 0, binding = 11) uniform restrict image2D gather_lighting;
layout(set = 0, binding = 13) uniform sampler linear_sampler;

layout(push_constant, std430) uniform Composition {
	ivec4 first_volume;
	ivec4 region;
}
composition;

vec4 lrt_reconstruct(vec3 position, vec3 normal) {
	vec3 size = vec3(lrt.data.grid_size_mode.xyz);
	vec3 grid_position = (position - lrt.data.grid_min_spacing.xyz) / lrt.data.grid_min_spacing.w - 0.5;
	vec4 kernel = vec4(0.2820947918, (2.0 / 3.0) * 0.4886025119 * normal);
#ifdef MODE_CUBIC
	vec3 base = floor(grid_position);
	vec3 fraction = fract(grid_position);
	vec3 inverse_fraction = 1.0 - fraction;
	vec3 w0 = inverse_fraction * inverse_fraction * inverse_fraction / 6.0;
	vec3 w1 = (3.0 * fraction * fraction * fraction - 6.0 * fraction * fraction + 4.0) / 6.0;
	vec3 w3 = fraction * fraction * fraction / 6.0;
	vec3 w2 = 1.0 - w0 - w1 - w3;
	vec3 low_weight = w0 + w1;
	vec3 high_weight = w2 + w3;
	// Positive cubic weights let each pair of taps use hardware linear interpolation.
	// Eight trilinear samples evaluate the full 4x4x4 B-spline without overshoot.
	vec3 low_uv = (base - 1.0 + w1 / low_weight + 0.5) / size;
	vec3 high_uv = (base + 1.0 + w3 / high_weight + 0.5) / size;
	vec4 result = vec4(0.0);
	for (int tap = 0; tap < 8; tap++) {
		vec3 corner = vec3(tap & 1, (tap >> 1) & 1, (tap >> 2) & 1);
		vec3 uv = mix(low_uv, high_uv, corner);
		vec3 axis_weight = mix(low_weight, high_weight, corner);
		float weight = axis_weight.x * axis_weight.y * axis_weight.z;
		vec3 lighting;
		lighting.r = dot(textureLod(sampler3D(lrt_radiance_r, linear_sampler), uv, 0.0), kernel);
		lighting.g = dot(textureLod(sampler3D(lrt_radiance_g, linear_sampler), uv, 0.0), kernel);
		lighting.b = dot(textureLod(sampler3D(lrt_radiance_b, linear_sampler), uv, 0.0), kernel);
		float support = textureLod(sampler3D(lrt_support, linear_sampler), uv, 0.0).r;
		result += weight * vec4(lighting, support);
	}
	return result;
#elif defined(MODE_NEAREST)
	ivec3 cell = clamp(ivec3(floor(grid_position + 0.5)), ivec3(0), lrt.data.grid_size_mode.xyz - 1);
	return vec4(
			dot(texelFetch(sampler3D(lrt_radiance_r, linear_sampler), cell, 0), kernel),
			dot(texelFetch(sampler3D(lrt_radiance_g, linear_sampler), cell, 0), kernel),
			dot(texelFetch(sampler3D(lrt_radiance_b, linear_sampler), cell, 0), kernel),
			texelFetch(sampler3D(lrt_support, linear_sampler), cell, 0).r);
#else
	vec3 uv = (grid_position + 0.5) / size;
	return vec4(
			dot(textureLod(sampler3D(lrt_radiance_r, linear_sampler), uv, 0.0), kernel),
			dot(textureLod(sampler3D(lrt_radiance_g, linear_sampler), uv, 0.0), kernel),
			dot(textureLod(sampler3D(lrt_radiance_b, linear_sampler), uv, 0.0), kernel),
			textureLod(sampler3D(lrt_support, linear_sampler), uv, 0.0).r);
#endif
}

vec4 lrt_sample(vec3 world_position, vec3 world_normal) {
	vec3 position = (lrt.data.world_to_volume * vec4(world_position, 1.0)).xyz;
	if (any(lessThan(position, lrt.data.volume_min.xyz)) || any(greaterThan(position, lrt.data.volume_max.xyz))) {
		return vec4(0.0);
	}
	vec3 face_distance = min(position - lrt.data.volume_min.xyz, lrt.data.volume_max.xyz - position);
	float boundary_distance = min(face_distance.x, min(face_distance.y, face_distance.z));
	float blend = lrt.data.atlas_flags.w > 0.0 ? clamp(boundary_distance / lrt.data.atlas_flags.w, 0.0, 1.0) : 1.0;
	vec3 normal = normalize(mat3(lrt.data.world_to_volume) * world_normal);
	vec4 reconstructed = lrt_reconstruct(position, normal);
	if (reconstructed.a <= 0.0) {
		return vec4(0.0);
	}
	// Normalize after reconstructing both SH and support with the same smooth kernel.
	vec3 lighting = max(reconstructed.rgb / reconstructed.a, vec3(0.0));
	return vec4(lighting * blend, blend);
}

vec3 lrt_view_position(ivec2 coord, float depth) {
	vec2 ndc = 2.0 * (vec2(coord) + 0.5) / vec2(gather.screen_size.xy) - 1.0;
	vec4 position = gather.inv_projection * vec4(ndc, depth, 1.0);
	return position.xyz / position.w;
}

void main() {
	ivec2 region_coord = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(region_coord, composition.region.zw))) {
		return;
	}
	ivec2 gather_coord = region_coord + composition.region.xy;
	ivec2 full_coord = gather_coord;
	vec4 packed_normal = texelFetch(sampler2D(normal_roughness_buffer, linear_sampler), full_coord, 0);
	if (all(lessThan(abs(packed_normal.xyz), vec3(0.00001)))) {
		imageStore(gather_lighting, gather_coord, vec4(0.0));
		return;
	}
	float depth = texelFetch(sampler2D(depth_buffer, linear_sampler), full_coord, 0).r;
	vec3 view_position = lrt_view_position(full_coord, depth);
	vec3 view_normal = normalize(packed_normal.xyz * 2.0 - 1.0);
	vec3 world_position = (gather.view_to_world * vec4(view_position, 1.0)).xyz;
	vec3 world_normal = normalize(mat3(gather.view_to_world) * view_normal);
	vec4 lighting = lrt_sample(world_position, world_normal);
	vec4 lower = composition.first_volume.x != 0 ? vec4(0.0) : imageLoad(gather_lighting, gather_coord);
	vec4 composed = lighting + lower * (1.0 - lighting.a);
	imageStore(gather_lighting, gather_coord, composed);
}
