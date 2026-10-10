// Shared world-space LRT reconstruction for screen and transparent receivers.
const float LRT_C0 = 0.2820947918;
const float LRT_C1 = 0.4886025119;
const float LRT_PI = 3.14159265358979323846;

vec4 lrt_cosine_kernel(vec3 normal) {
	return vec4(LRT_PI * LRT_C0, (2.0 * LRT_PI / 3.0) * LRT_C1 * normal);
}

bool lrt_outside(LRTData data, ivec3 cell) {
	return any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, data.grid_size_mode.xyz));
}

ivec2 lrt_atlas_coord(LRTData data, ivec3 cell) {
	return ivec2(cell.x + cell.z * data.grid_size_mode.x, cell.y);
}

vec3 lrt_probe_position(LRTData data, ivec3 cell) {
	return data.grid_min_spacing.xyz + (vec3(cell) + 0.5) * data.grid_min_spacing.w;
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

float lrt_local_connection(LRTData data, ivec3 cell, ivec3 low, vec4 weights_0, vec4 weights_1,
		uint valid_mask, float valid_weight) {
	if (valid_weight <= 0.00001) {
		return 0.0;
	}
	vec2 packed_links = lrt_fetch(data, 4, cell).rg;
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

bool lrt_sample_native(LRTData data, vec3 world_position, vec3 world_normal, out vec3 ambient_light, out float blend_weight, out float reconstruction_coverage) {
	ambient_light = vec3(0.0);
	blend_weight = 0.0;
	reconstruction_coverage = 0.0;
	if (data.volume_min.w < 0.5) {
		return false;
	}
	vec3 position = (data.world_to_volume * vec4(world_position, 1.0)).xyz;
	if (any(lessThan(position, data.volume_min.xyz)) || any(greaterThan(position, data.volume_max.xyz))) {
		return false;
	}
	vec3 face_distance = min(position - data.volume_min.xyz, data.volume_max.xyz - position);
	float boundary_distance = min(face_distance.x, min(face_distance.y, face_distance.z));
	blend_weight = data.atlas_flags.w > 0.0 ? clamp(boundary_distance / data.atlas_flags.w, 0.0, 1.0) : 1.0;
	if (blend_weight <= 0.0) {
		return false;
	}
	vec3 normal = normalize(mat3(data.world_to_volume) * world_normal);
	float spacing = data.grid_min_spacing.w;
	vec3 receiver_position = position + normal * spacing * 0.55;
	vec3 grid_position = (receiver_position - data.grid_min_spacing.xyz) / spacing - 0.5;
	ivec3 connection_low = ivec3(floor(grid_position));
	vec3 connection_fraction = fract(grid_position);
	vec4 connection_weights_0 = vec4(0.0);
	vec4 connection_weights_1 = vec4(0.0);
	uint connection_valid_mask = 0u;
	float connection_valid_weight = 0.0;
	for (int index = 0; index < 8; index++) {
		ivec3 corner = ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		ivec3 target = connection_low + corner;
		if (lrt_outside(data, target)) {
			continue;
		}
		vec4 target_material = lrt_fetch(data, 3, target);
		if (target_material.a > 0.5 || target_material.b <= 0.0) {
			continue;
		}
		vec3 corner_weight = mix(vec3(1.0) - connection_fraction, connection_fraction, vec3(corner));
		float weight = corner_weight.x * corner_weight.y * corner_weight.z * target_material.b;
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
	ivec3 base = ivec3(floor(grid_position + 0.5)) - ivec3(1);
	for (int index = 0; index < 27; index++) {
		ivec3 cell = base + ivec3(index % 3, (index / 3) % 3, index / 9);
		if (lrt_outside(data, cell)) {
			continue;
		}
		vec4 material = lrt_fetch(data, 3, cell);
		if (material.a > 0.5 || material.b <= 0.0) {
			continue;
		}
		vec3 probe_delta = lrt_probe_position(data, cell) - position;
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
		float connection = lrt_local_connection(data, cell, connection_low, connection_weights_0,
				connection_weights_1, connection_valid_mask, connection_valid_weight);
		if (connection <= 0.0) {
			continue;
		}
		weight *= connection * material.b;
		float sample_distance = dot(weight_delta, weight_delta);
		if (data.atlas_flags.z < 0.5 && sample_distance >= nearest_distance) {
			continue;
		}
		if (data.atlas_flags.z < 0.5) {
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
		red += weight * lrt_fetch(data, 0, cell);
		green += weight * lrt_fetch(data, 1, cell);
		blue += weight * lrt_fetch(data, 2, cell);
		sky_red += weight * lrt_fetch(data, 5, cell);
		sky_green += weight * lrt_fetch(data, 6, cell);
		sky_blue += weight * lrt_fetch(data, 7, cell);
		total += weight;
	}
	if (total <= 0.0) {
		return false;
	}
	// Coverage describes available reconstruction support, not physical visibility.
	reconstruction_coverage = total * connection_valid_weight;
	vec4 kernel = lrt_cosine_kernel(normal) / total;
	vec3 irradiance = max(vec3(dot(red, kernel), dot(green, kernel), dot(blue, kernel)), vec3(0.0));
	vec3 direct_sky = max(vec3(dot(sky_red, kernel), dot(sky_green, kernel), dot(sky_blue, kernel)) / LRT_PI, vec3(0.0));
	ambient_light = irradiance / LRT_PI + direct_sky;
	return true;
}

