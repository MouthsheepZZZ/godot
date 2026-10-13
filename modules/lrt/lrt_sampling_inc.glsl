// Shared world-space LRT reconstruction for screen and transparent receivers.
const float LRT_C0 = 0.2820947918;
const float LRT_C1 = 0.4886025119;
const float LRT_PI = 3.14159265358979323846;
const float LRT_SURFACE_BIAS = 0.2;
const float LRT_FRONTFACE_BLEND_WIDTH = 0.2;

vec4 lrt_cosine_kernel(vec3 normal) {
	return vec4(LRT_PI * LRT_C0, (2.0 * LRT_PI / 3.0) * LRT_C1 * normal);
}

bool lrt_outside(LRTData data, ivec3 cell) {
	return any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, data.grid_size_mode.xyz));
}

bool lrt_sample_native(LRTData data, vec3 world_position, vec3 world_normal, out vec3 ambient_light, out float blend_weight) {
	ambient_light = vec3(0.0);
	blend_weight = 0.0;
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
	vec3 surface_grid_position = (position - data.grid_min_spacing.xyz) / spacing - 0.5;
	// Keep forward support when the surface coincides with a probe plane.
	vec3 grid_position = surface_grid_position + normal * LRT_SURFACE_BIAS;
	bool interpolate = data.atlas_flags.z >= 0.5;
	ivec3 low = ivec3(floor(grid_position));
	vec3 fraction = fract(grid_position);
	vec4 red = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 blue = vec4(0.0);
	vec4 sky_red = vec4(0.0);
	vec4 sky_green = vec4(0.0);
	vec4 sky_blue = vec4(0.0);
	float total = 0.0;
	float nearest_distance = 1e30;
	for (int index = 0; index < 8; index++) {
		ivec3 corner = ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		ivec3 cell = low + corner;
		if (lrt_outside(data, cell)) {
			continue;
		}
		vec4 material = lrt_fetch(data, 3, cell);
		if (material.b <= 0.0) {
			continue;
		}
		float weight;
		if (interpolate) {
			vec3 axis_weight = mix(vec3(1.0) - fraction, fraction, vec3(corner));
			float surface_distance = dot(vec3(cell) - surface_grid_position, normal);
			float side_weight = smoothstep(0.0, LRT_FRONTFACE_BLEND_WIDTH, surface_distance);
			// Forward solid probes retain their zero radiance and normalization weight.
			weight = axis_weight.x * axis_weight.y * axis_weight.z * material.b * side_weight;
			if (weight <= 0.0) {
				continue;
			}
		} else {
			float distance = dot(vec3(cell) - grid_position, vec3(cell) - grid_position);
			if (distance >= nearest_distance) {
				continue;
			}
			nearest_distance = distance;
			red = green = blue = sky_red = sky_green = sky_blue = vec4(0.0);
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
		blend_weight = 0.0;
		return false;
	}
	vec4 kernel = lrt_cosine_kernel(normal) / total;
	vec3 irradiance = max(vec3(dot(red, kernel), dot(green, kernel), dot(blue, kernel)), vec3(0.0));
	vec3 direct_sky = max(vec3(dot(sky_red, kernel), dot(sky_green, kernel), dot(sky_blue, kernel)) / LRT_PI, vec3(0.0));
	ambient_light = irradiance / LRT_PI + direct_sky;
	return true;
}
