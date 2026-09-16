#[compute]

#version 450

// The paper specifies a low-resolution screen gather, normally at 25% pixel count.
// Godot uses half width and half height; the exact layout and edge-aware reconstruction
// are project design because the paper does not publish them.

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
layout(set = 0, binding = 6) uniform texture2D lrt_links;
layout(set = 0, binding = 7) uniform texture2D lrt_sky_r;
layout(set = 0, binding = 8) uniform texture2D lrt_sky_g;
layout(set = 0, binding = 9) uniform texture2D lrt_sky_b;
layout(set = 0, binding = 10) uniform texture2D depth_buffer;
layout(set = 0, binding = 11) uniform texture2D normal_roughness_buffer;
layout(rgba16f, set = 0, binding = 12) uniform restrict writeonly image2D gather_lighting;
layout(rgba16f, set = 0, binding = 13) uniform restrict writeonly image2D gather_geometry;
layout(set = 0, binding = 14) uniform sampler nearest_sampler;

const float LRT_C0 = 0.2820947918;
const float LRT_C1 = 0.4886025119;
const float LRT_PI = 3.14159265358979323846;

vec4 lrt_cosine_kernel(vec3 normal) {
	return vec4(LRT_PI * LRT_C0, (2.0 * LRT_PI / 3.0) * LRT_C1 * normal);
}

bool lrt_outside(ivec3 cell) {
	return any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, lrt.data.grid_size_mode.xyz));
}

ivec2 lrt_atlas_coord(ivec3 cell) {
	return ivec2(cell.x + cell.z * lrt.data.grid_size_mode.x, cell.y);
}

vec4 lrt_fetch(texture2D field, ivec3 cell) {
	return texelFetch(sampler2D(field, nearest_sampler), lrt_atlas_coord(cell), 0);
}

vec3 lrt_probe_position(ivec3 cell) {
	return lrt.data.grid_min_spacing.xyz + (vec3(cell) + 0.5) * lrt.data.grid_min_spacing.w;
}

float lrt_reconstruction_weight(float value) {
	float distance_value = abs(value);
	if (distance_value < 0.5) {
		return 0.75 - distance_value * distance_value;
	}
	if (distance_value < 1.5) {
		float edge = 1.5 - distance_value;
		return 0.5 * edge * edge;
	}
	return 0.0;
}

float lrt_link_open(ivec3 cell, ivec3 target, vec2 packed_links) {
	ivec3 offset = target - cell;
	if (all(equal(offset, ivec3(0)))) {
		return 1.0;
	}
	if (any(greaterThan(abs(offset), ivec3(1)))) {
		return 0.0;
	}
	int packed_index = (offset.z + 1) * 9 + (offset.y + 1) * 3 + offset.x + 1;
	int direction_index = packed_index < 13 ? packed_index : packed_index - 1;
	uint links = direction_index < 13 ? uint(packed_links.r + 0.5) : uint(packed_links.g + 0.5);
	uint bit = 1u << uint(direction_index % 13);
	return (links & bit) != 0u ? 1.0 : 0.0;
}

float lrt_local_connection(ivec3 cell, ivec3 low, vec4 weights_0, vec4 weights_1,
		uint valid_mask, float valid_weight) {
	if (valid_weight <= 0.00001) {
		return 0.0;
	}
	vec2 packed_links = texelFetch(sampler2D(lrt_links, nearest_sampler), lrt_atlas_coord(cell), 0).rg;
	float connection = 0.0;
	for (int index = 0; index < 8; index++) {
		if ((valid_mask & (1u << uint(index))) == 0u) {
			continue;
		}
		ivec3 corner = ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		float weight = index < 4 ? weights_0[index] : weights_1[index - 4];
		connection += weight * lrt_link_open(cell, low + corner, packed_links);
	}
	return connection / valid_weight;
}

bool lrt_sample_native(vec3 world_position, vec3 world_normal, out vec3 ambient_light, out float blend_weight) {
	ambient_light = vec3(0.0);
	blend_weight = 0.0;
	if (lrt.data.volume_min.w < 0.5) {
		return false;
	}
	vec3 position = (lrt.data.world_to_volume * vec4(world_position, 1.0)).xyz;
	if (any(lessThan(position, lrt.data.volume_min.xyz)) || any(greaterThan(position, lrt.data.volume_max.xyz))) {
		return false;
	}
	vec3 face_distance = min(position - lrt.data.volume_min.xyz, lrt.data.volume_max.xyz - position);
	float boundary_distance = min(face_distance.x, min(face_distance.y, face_distance.z));
	blend_weight = lrt.data.atlas_flags.w > 0.0 ? clamp(boundary_distance / lrt.data.atlas_flags.w, 0.0, 1.0) : 1.0;
	if (blend_weight <= 0.0) {
		return false;
	}
	vec3 normal = normalize(mat3(lrt.data.world_to_volume) * world_normal);
	float spacing = lrt.data.grid_min_spacing.w;
	vec3 receiver_position = position + normal * spacing * 0.55;
	vec3 grid_position = (receiver_position - lrt.data.grid_min_spacing.xyz) / spacing - 0.5;
	ivec3 connection_low = ivec3(floor(grid_position));
	vec3 connection_fraction = fract(grid_position);
	vec4 connection_weights_0 = vec4(0.0);
	vec4 connection_weights_1 = vec4(0.0);
	uint connection_valid_mask = 0u;
	float connection_valid_weight = 0.0;
	for (int index = 0; index < 8; index++) {
		ivec3 corner = ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		ivec3 target = connection_low + corner;
		if (lrt_outside(target) || lrt_fetch(lrt_material, target).a > 0.5) {
			continue;
		}
		vec3 corner_weight = mix(vec3(1.0) - connection_fraction, connection_fraction, vec3(corner));
		float weight = corner_weight.x * corner_weight.y * corner_weight.z;
		if (index < 4) {
			connection_weights_0[index] = weight;
		} else {
			connection_weights_1[index - 4] = weight;
		}
		connection_valid_weight += weight;
		connection_valid_mask |= 1u << uint(index);
	}
	vec4 red = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 blue = vec4(0.0);
	vec4 sky_red = vec4(0.0);
	vec4 sky_green = vec4(0.0);
	vec4 sky_blue = vec4(0.0);
	float total = 0.0;
	float nearest_distance = 1e30;
	for (int index = 0; index < 8; index++) {
		if ((connection_valid_mask & (1u << uint(index))) == 0u) {
			continue;
		}
		ivec3 corner = ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		ivec3 cell = connection_low + corner;
		vec3 probe_delta = lrt_probe_position(cell) - position;
		if (dot(probe_delta, normal) < 0.0) {
			continue;
		}
		float weight = index < 4 ? connection_weights_0[index] : connection_weights_1[index - 4];
		float connection = lrt_local_connection(cell, connection_low, connection_weights_0,
				connection_weights_1, connection_valid_mask, connection_valid_weight);
		if (connection <= 0.02) {
			continue;
		}
		weight *= connection;
		vec3 weight_delta = vec3(cell) - grid_position;
		float sample_distance = dot(weight_delta, weight_delta);
		if (lrt.data.atlas_flags.z < 0.5 && sample_distance >= nearest_distance) {
			continue;
		}
		if (lrt.data.atlas_flags.z < 0.5) {
			nearest_distance = sample_distance;
			red = vec4(0.0);
			green = vec4(0.0);
			blue = vec4(0.0);
			sky_red = vec4(0.0);
			sky_green = vec4(0.0);
			sky_blue = vec4(0.0);
			total = 0.0;
			weight = 1.0;
		}
		red += weight * lrt_fetch(lrt_radiance_r, cell);
		green += weight * lrt_fetch(lrt_radiance_g, cell);
		blue += weight * lrt_fetch(lrt_radiance_b, cell);
		sky_red += weight * lrt_fetch(lrt_sky_r, cell);
		sky_green += weight * lrt_fetch(lrt_sky_g, cell);
		sky_blue += weight * lrt_fetch(lrt_sky_b, cell);
		total += weight;
	}
	if (total <= 0.0) {
		return true;
	}
	vec4 kernel = lrt_cosine_kernel(normal) / total;
	vec3 irradiance = max(vec3(dot(red, kernel), dot(green, kernel), dot(blue, kernel)), vec3(0.0));
	vec3 direct_sky = max(vec3(dot(sky_red, kernel), dot(sky_green, kernel), dot(sky_blue, kernel)) / LRT_PI, vec3(0.0));
	ambient_light = irradiance / LRT_PI + direct_sky;
	return true;
}

void main() {
	ivec2 gather_coord = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(gather_coord, gather.screen_size.zw))) {
		return;
	}
	ivec2 full_coord = min(gather_coord * 2 + ivec2(1), gather.screen_size.xy - ivec2(1));
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
	lrt_sample_native(world_position, world_normal, ambient_light, blend_weight);
	imageStore(gather_lighting, gather_coord, vec4(ambient_light, blend_weight));
	imageStore(gather_geometry, gather_coord, vec4(view_normal, view_position.z));
}
