// Native Forward+ LRT diffuse receiver. It reconstructs the local field from probe data only:
// no triangle, BVH, SDF ray march, depth buffer, screen gather, or user material overlay.

#define INSTANCE_FLAGS_USE_LRT (1 << 0)

struct LRTData {
	mat4 world_to_volume;
	vec4 volume_min;
	vec4 volume_max;
	vec4 grid_min_spacing;
	ivec4 grid_size_mode;
	vec4 atlas_flags;
	vec4 sky_color;
};

layout(set = 1, binding = 39, std140) uniform LRTDataBlock {
	LRTData data;
}
lrt;

layout(set = 1, binding = 40) uniform texture2D lrt_radiance_r;
layout(set = 1, binding = 41) uniform texture2D lrt_radiance_g;
layout(set = 1, binding = 42) uniform texture2D lrt_radiance_b;
layout(set = 1, binding = 43) uniform texture2D lrt_visibility;
layout(set = 1, binding = 44) uniform texture2D lrt_material;
layout(set = 1, binding = 45) uniform texture2D lrt_links;

const float LRT_C0 = 0.2820947918;
const float LRT_C1 = 0.4886025119;

vec4 lrt_basis(vec3 direction) {
	return vec4(LRT_C0, LRT_C1 * direction);
}

vec4 lrt_cosine_kernel(vec3 normal) {
	return vec4(M_PI * LRT_C0, (2.0 * M_PI / 3.0) * LRT_C1 * normal);
}

bool lrt_outside(ivec3 cell) {
	return any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, lrt.data.grid_size_mode.xyz));
}

ivec2 lrt_atlas_coord(ivec3 cell) {
	return ivec2(cell.x + cell.z * lrt.data.grid_size_mode.x, cell.y);
}

vec4 lrt_fetch(texture2D field, ivec3 cell) {
	return texelFetch(sampler2D(field, SAMPLER_NEAREST_CLAMP), lrt_atlas_coord(cell), 0);
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
	if (lrt_outside(target) || any(greaterThan(abs(offset), ivec3(1)))) {
		return 0.0;
	}
	int packed_index = (offset.z + 1) * 9 + (offset.y + 1) * 3 + offset.x + 1;
	int direction_index = packed_index < 13 ? packed_index : packed_index - 1;
	uint links = direction_index < 13 ? uint(packed_links.r + 0.5) : uint(packed_links.g + 0.5);
	uint bit = 1u << uint(direction_index % 13);
	return (links & bit) != 0u ? 1.0 : 0.0;
}

// Interpolating the exact one-edge classifications of the eight surrounding receiver cells
// keeps the filter continuous as the receiver crosses a probe-cell boundary. It neither traces
// scene geometry nor projects the binary mask into ringing low-order SH.
float lrt_local_connection(ivec3 cell, vec3 receiver_grid_position) {
	ivec3 low = ivec3(floor(receiver_grid_position));
	vec3 fraction = fract(receiver_grid_position);
	vec2 packed_links = texelFetch(sampler2D(lrt_links, SAMPLER_NEAREST_CLAMP), lrt_atlas_coord(cell), 0).rg;
	float connection = 0.0;
	float valid_weight = 0.0;
	for (int index = 0; index < 8; index++) {
		ivec3 corner = ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		vec3 corner_weight = mix(vec3(1.0) - fraction, fraction, vec3(corner));
		float weight = corner_weight.x * corner_weight.y * corner_weight.z;
		ivec3 target = low + corner;
		if (lrt_outside(target) || lrt_fetch(lrt_material, target).a > 0.5) {
			continue;
		}
		valid_weight += weight;
		connection += weight * lrt_link_open(cell, target, packed_links);
	}
	return valid_weight > 0.00001 ? connection / valid_weight : 0.0;
}

bool lrt_sample_native(vec3 world_position, vec3 world_normal, out vec3 diffuse_irradiance, out float sky_visibility) {
	diffuse_irradiance = vec3(0.0);
	sky_visibility = 0.0;
	if (lrt.data.volume_min.w < 0.5) {
		return false;
	}
	vec3 position = (lrt.data.world_to_volume * vec4(world_position, 1.0)).xyz;
	if (any(lessThan(position, lrt.data.volume_min.xyz)) || any(greaterThan(position, lrt.data.volume_max.xyz))) {
		return false;
	}
	vec3 normal = normalize(mat3(lrt.data.world_to_volume) * world_normal);
	float spacing = lrt.data.grid_min_spacing.w;
	vec3 receiver_position = position + normal * spacing * 0.55;
	vec3 grid_position = (receiver_position - lrt.data.grid_min_spacing.xyz) / spacing - 0.5;
	ivec3 base = ivec3(floor(grid_position + 0.5)) - ivec3(1);
	vec4 red = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 blue = vec4(0.0);
	vec4 visible = vec4(0.0);
	float total = 0.0;
	float nearest_distance = 1e30;
	for (int index = 0; index < 27; index++) {
		ivec3 cell = base + ivec3(index % 3, (index / 3) % 3, index / 9);
		if (lrt_outside(cell) || lrt_fetch(lrt_material, cell).a > 0.5) {
			continue;
		}
		vec3 probe_delta = lrt_probe_position(cell) - position;
		if (dot(probe_delta, normal) < 0.0) {
			continue;
		}
		vec3 weight_delta = vec3(cell) - grid_position;
		float weight = lrt_reconstruction_weight(weight_delta.x) *
				lrt_reconstruction_weight(weight_delta.y) *
				lrt_reconstruction_weight(weight_delta.z);
		if (weight <= 0.0) {
			continue;
		}
		float connection = lrt_local_connection(cell, grid_position);
		if (connection <= 0.02) {
			continue;
		}
		weight *= connection;
		float sample_distance = dot(weight_delta, weight_delta);
		if (lrt.data.atlas_flags.z < 0.5 && sample_distance >= nearest_distance) {
			continue;
		}
		if (lrt.data.atlas_flags.z < 0.5) {
			nearest_distance = sample_distance;
			red = vec4(0.0);
			green = vec4(0.0);
			blue = vec4(0.0);
			visible = vec4(0.0);
			total = 0.0;
			weight = 1.0;
		}
		red += weight * lrt_fetch(lrt_radiance_r, cell);
		green += weight * lrt_fetch(lrt_radiance_g, cell);
		blue += weight * lrt_fetch(lrt_radiance_b, cell);
		visible += weight * lrt_fetch(lrt_visibility, cell);
		total += weight;
	}
	if (total <= 0.0) {
		return true;
	}
	vec4 kernel = lrt_cosine_kernel(normal) / total;
	diffuse_irradiance = max(vec3(dot(red, kernel), dot(green, kernel), dot(blue, kernel)), vec3(0.0));
	sky_visibility = clamp(dot(visible, kernel) / M_PI, 0.0, 1.0);
	return true;
}
