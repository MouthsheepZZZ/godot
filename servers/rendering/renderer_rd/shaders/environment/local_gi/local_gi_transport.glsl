#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct BvhNode {
	vec3 bounds_min;
	int left;
	vec3 bounds_max;
	int right;
	int first_triangle;
	int triangle_count;
	int pad0;
	int pad1;
};

struct Triangle {
	vec4 v0;
	vec4 v1;
	vec4 v2;
	vec4 normal;
	vec4 albedo;
};

struct Light {
	vec4 position_type;
	vec4 direction_range;
	vec4 intensity_attenuation;
	vec4 spot;
};

struct Moment {
	float mean;
	float second;
	float pad0;
	float pad1;
};

struct Hit {
	bool valid;
	float distance;
	vec3 position;
	vec3 normal;
	vec3 albedo;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer StaticNodes { BvhNode data[]; } static_nodes;
layout(set = 0, binding = 1, std430) restrict readonly buffer StaticTriangles { Triangle data[]; } static_triangles;
layout(set = 0, binding = 2, std430) restrict readonly buffer DynamicNodes { BvhNode data[]; } dynamic_nodes;
layout(set = 0, binding = 3, std430) restrict readonly buffer DynamicTriangles { Triangle data[]; } dynamic_triangles;
layout(set = 0, binding = 4, std430) restrict readonly buffer ProbePositions { vec4 data[]; } probe_positions;
layout(set = 0, binding = 5, std430) restrict readonly buffer RayDirections { vec4 data[]; } ray_directions;
layout(set = 0, binding = 6, std430) restrict readonly buffer Lights { Light data[]; } lights;
layout(set = 0, binding = 7, std430) restrict readonly buffer HistoryIrradiance { vec4 data[]; } history_irradiance;
layout(set = 0, binding = 8, std430) restrict readonly buffer HistoryMoments { Moment data[]; } history_moments;
layout(set = 0, binding = 9, std430) restrict writeonly buffer SampleIrradiance { vec4 data[]; } sample_irradiance;
layout(set = 0, binding = 10, std430) restrict writeonly buffer EstimateIrradiance { vec4 data[]; } estimate_irradiance;
layout(set = 0, binding = 11, std430) restrict writeonly buffer RayRadiance { vec4 data[]; } ray_radiance;
layout(set = 0, binding = 12, std430) restrict buffer SampleMoments { Moment data[]; } sample_moments;
layout(set = 0, binding = 13, std430) restrict writeonly buffer EstimateMoments { Moment data[]; } estimate_moments;
layout(set = 0, binding = 14, std430) restrict writeonly buffer ProbeActive { uint data[]; } probe_active;

layout(push_constant, std430) uniform Params {
	int static_node_count;
	int dynamic_node_count;
	int probe_count;
	int rays_per_probe;
	int light_count;
	int resolution_x;
	int resolution_y;
	int resolution_z;
	float volume_size_x;
	float volume_size_y;
	float volume_size_z;
	float visibility_bias;
	float temporal_hysteresis;
	int temporal_cursor;
	int update_count;
	uint history_valid;
	uint multi_bounce;
	float far_distance;
	float pad0;
} params;

const float PI = 3.14159265358979323846;
const float TRI_T_MIN = 0.00001;
const int STACK_SIZE = 64;

bool update_ray_slab(float p_origin, float p_direction, float p_minimum, float p_maximum, inout float r_minimum_distance, inout float r_maximum_distance) {
	if (p_direction == 0.0) {
		return p_origin >= p_minimum && p_origin <= p_maximum;
	}
	float first = (p_minimum - p_origin) / p_direction;
	float second = (p_maximum - p_origin) / p_direction;
	if (first > second) {
		float swap_value = first;
		first = second;
		second = swap_value;
	}
	r_minimum_distance = max(r_minimum_distance, first);
	r_maximum_distance = min(r_maximum_distance, second);
	return r_maximum_distance >= 0.0 && r_minimum_distance <= r_maximum_distance;
}

bool aabb_intersects_ray(vec3 p_origin, vec3 p_direction, vec3 p_minimum, vec3 p_maximum) {
	float minimum_distance = -1.0e20;
	float maximum_distance = 1.0e20;
	return update_ray_slab(p_origin.x, p_direction.x, p_minimum.x, p_maximum.x, minimum_distance, maximum_distance) &&
			update_ray_slab(p_origin.y, p_direction.y, p_minimum.y, p_maximum.y, minimum_distance, maximum_distance) &&
			update_ray_slab(p_origin.z, p_direction.z, p_minimum.z, p_maximum.z, minimum_distance, maximum_distance);
}

bool ray_intersects_triangle(vec3 p_origin, vec3 p_direction, Triangle p_triangle, out vec3 r_position) {
	vec3 edge1 = p_triangle.v1.xyz - p_triangle.v0.xyz;
	vec3 edge2 = p_triangle.v2.xyz - p_triangle.v0.xyz;
	vec3 h = cross(p_direction, edge2);
	float determinant = dot(edge1, h);
	if (abs(determinant) < 1e-6) {
		return false;
	}
	float inverse = 1.0 / determinant;
	vec3 s = p_origin - p_triangle.v0.xyz;
	float u = inverse * dot(s, h);
	if (u < 0.0 || u > 1.0) {
		return false;
	}
	vec3 q = cross(s, edge1);
	float v = inverse * dot(p_direction, q);
	if (v < 0.0 || u + v > 1.0) {
		return false;
	}
	float distance = inverse * dot(edge2, q);
	if (distance <= TRI_T_MIN) {
		return false;
	}
	r_position = p_origin + p_direction * distance;
	return true;
}

Hit empty_hit() {
	Hit hit;
	hit.valid = false;
	hit.distance = 0.0;
	hit.position = vec3(0.0);
	hit.normal = vec3(0.0);
	hit.albedo = vec3(1.0);
	return hit;
}

Hit trace_tree(vec3 p_origin, vec3 p_direction, bool p_dynamic) {
	Hit result = empty_hit();
	int node_count = p_dynamic ? params.dynamic_node_count : params.static_node_count;
	if (node_count <= 0) {
		return result;
	}

	float closest_distance = 1.0e20;
	int closest_triangle = -1;
	vec3 closest_position = vec3(0.0);
	int stack[STACK_SIZE];
	int stack_size = 0;
	stack[stack_size++] = 0;
	while (stack_size > 0) {
		int node_index = stack[--stack_size];
		BvhNode node = p_dynamic ? dynamic_nodes.data[node_index] : static_nodes.data[node_index];
		if (!aabb_intersects_ray(p_origin, p_direction, node.bounds_min, node.bounds_max)) {
			continue;
		}
		if (node.triangle_count > 0) {
			for (int i = 0; i < node.triangle_count; i++) {
				int triangle_index = node.first_triangle + i;
				Triangle triangle = p_dynamic ? dynamic_triangles.data[triangle_index] : static_triangles.data[triangle_index];
				vec3 position;
				if (!ray_intersects_triangle(p_origin, p_direction, triangle, position)) {
					continue;
				}
				float distance = length(position - p_origin);
				if (distance < closest_distance) {
					closest_distance = distance;
					closest_triangle = triangle_index;
					closest_position = position;
				}
			}
			continue;
		}
		if (node.right >= 0 && stack_size < STACK_SIZE) {
			stack[stack_size++] = node.right;
		}
		if (node.left >= 0 && stack_size < STACK_SIZE) {
			stack[stack_size++] = node.left;
		}
	}
	if (closest_triangle < 0) {
		return result;
	}
	Triangle closest = p_dynamic ? dynamic_triangles.data[closest_triangle] : static_triangles.data[closest_triangle];
	result.valid = true;
	result.distance = closest_distance;
	result.position = closest_position;
	result.normal = closest.normal.xyz;
	result.albedo = closest.albedo.rgb;
	return result;
}

Hit trace_scene(vec3 p_origin, vec3 p_direction) {
	Hit static_hit = trace_tree(p_origin, p_direction, false);
	Hit dynamic_hit = trace_tree(p_origin, p_direction, true);
	if (static_hit.valid && dynamic_hit.valid) {
		return static_hit.distance <= dynamic_hit.distance ? static_hit : dynamic_hit;
	}
	return static_hit.valid ? static_hit : dynamic_hit;
}

float omni_attenuation(float p_distance, float p_range, float p_decay) {
	float range_value = max(p_range, 0.0001);
	float normalized_distance = p_distance / range_value;
	normalized_distance *= normalized_distance;
	normalized_distance *= normalized_distance;
	normalized_distance = max(1.0 - normalized_distance, 0.0);
	normalized_distance *= normalized_distance;
	return normalized_distance * pow(max(p_distance, 0.0001), -p_decay);
}

vec3 direct_irradiance(Hit p_hit, vec3 p_ray_direction) {
	vec3 normal = normalize(p_hit.normal);
	if (dot(normal, -p_ray_direction) < 0.0) {
		normal = -normal;
	}
	vec3 total = vec3(0.0);
	vec3 shadow_origin = p_hit.position + normal * 0.002;
	for (int i = 0; i < params.light_count; i++) {
		Light light = lights.data[i];
		int light_type = int(light.position_type.w + 0.5);
		vec3 to_light;
		float distance;
		float attenuation = 1.0;
		if (light_type == 0) {
			to_light = normalize(-light.direction_range.xyz);
			distance = light.direction_range.w;
		} else {
			to_light = light.position_type.xyz - p_hit.position;
			distance = length(to_light);
			if (distance > light.direction_range.w || distance < 1e-5) {
				continue;
			}
			to_light /= distance;
			attenuation = omni_attenuation(distance, light.direction_range.w, light.intensity_attenuation.w);
			if (light_type == 2) {
				float cosine = max(dot(-to_light, light.direction_range.xyz), light.spot.x);
				float rim = max(1e-4, (1.0 - cosine) / max(1.0 - light.spot.x, 1e-4));
				attenuation *= 1.0 - pow(rim, light.spot.y);
			}
		}
		float n_dot_l = dot(normal, to_light);
		if (n_dot_l <= 0.0 || attenuation <= 0.0) {
			continue;
		}
		Hit shadow = trace_scene(shadow_origin, to_light);
		if (shadow.valid && shadow.distance + 0.002 < distance) {
			continue;
		}
		total += light.intensity_attenuation.rgb * (attenuation * n_dot_l);
	}
	return total;
}

int cell_to_index(ivec3 p_cell) {
	ivec3 resolution = ivec3(params.resolution_x, params.resolution_y, params.resolution_z);
	if (any(lessThan(p_cell, ivec3(0))) || any(greaterThanEqual(p_cell, resolution))) {
		return -1;
	}
	return (p_cell.x * resolution.y + p_cell.y) * resolution.z + p_cell.z;
}

int nearest_direction_index(vec3 p_direction) {
	float length_squared = dot(p_direction, p_direction);
	if (length_squared < 1e-12) {
		return 0;
	}
	vec3 direction = p_direction * inversesqrt(length_squared);
	int best = 0;
	float best_dot = dot(ray_directions.data[0].xyz, direction);
	for (int i = 1; i < params.rays_per_probe; i++) {
		float candidate = dot(ray_directions.data[i].xyz, direction);
		if (candidate > best_dot) {
			best_dot = candidate;
			best = i;
		}
	}
	return best;
}

float chebyshev_visibility(float p_distance, float p_mean, float p_second) {
	float mean_value = max(p_mean, 0.0);
	float biased_mean = mean_value + max(params.visibility_bias, 0.0);
	if (p_distance <= biased_mean) {
		return 1.0;
	}
	float variance = max(p_second - mean_value * mean_value, 0.0);
	float delta = p_distance - biased_mean;
	float denominator = variance + delta * delta;
	return denominator > 0.0 ? clamp(variance / denominator, 0.0, 1.0) : 0.0;
}

bool probe_is_embedded(vec3 p_position, bool p_dynamic);

vec3 sample_previous_irradiance(vec3 p_position, vec3 p_normal) {
	ivec3 resolution = ivec3(params.resolution_x, params.resolution_y, params.resolution_z);
	vec3 volume_size = vec3(params.volume_size_x, params.volume_size_y, params.volume_size_z);
	vec3 counts = vec3(resolution);
	vec3 cell_size = volume_size / counts;
	vec3 grid = (p_position + volume_size * 0.5) / cell_size - vec3(0.5);
	vec3 clamped_grid = clamp(grid, vec3(0.0), vec3(resolution - ivec3(1)));
	ivec3 base = min(ivec3(floor(clamped_grid)), resolution - ivec3(2));
	vec3 fraction = clamp(clamped_grid - vec3(base), vec3(0.0), vec3(1.0));
	vec3 normal = normalize(p_normal);
	vec3 accumulation = vec3(0.0);
	float weight_sum = 0.0;
	for (int x = 0; x < 2; x++) {
		for (int y = 0; y < 2; y++) {
			for (int z = 0; z < 2; z++) {
				ivec3 cell = base + ivec3(x, y, z);
				int index = cell_to_index(cell);
				if (index < 0) {
					continue;
				}
				vec3 probe_position = probe_positions.data[index].xyz;
				if (probe_is_embedded(probe_position, false) || probe_is_embedded(probe_position, true)) {
					continue;
				}
				vec3 to_probe = probe_position - p_position;
				float distance = length(to_probe);
				float normal_weight = distance < 1e-6 ? 1.0 : max(dot(to_probe / distance, normal), 0.0);
				int direction_index = nearest_direction_index(p_position - probe_position);
				Moment moment = history_moments.data[index * params.rays_per_probe + direction_index];
				float visibility = distance < 1e-6 ? 1.0 : chebyshev_visibility(distance, moment.mean, moment.second);
				vec3 axis_weight = mix(vec3(1.0) - fraction, fraction, vec3(x, y, z));
				float weight = axis_weight.x * axis_weight.y * axis_weight.z * normal_weight * visibility;
				accumulation += history_irradiance.data[index].rgb * weight;
				weight_sum += weight;
			}
		}
	}
	return weight_sum > 1e-8 ? accumulation / weight_sum : vec3(0.0);
}

int probe_inside_hit(vec3 p_position, vec3 p_direction, bool p_dynamic) {
	Hit hit = trace_tree(p_position, p_direction, p_dynamic);
	return hit.valid && dot(hit.normal, hit.normal) >= 1e-12 && dot(p_direction, normalize(hit.normal)) < 0.0 ? 1 : 0;
}

bool probe_is_embedded(vec3 p_position, bool p_dynamic) {
	int inside_hits = probe_inside_hit(p_position, vec3(1.0, 0.0, 0.0), p_dynamic);
	inside_hits += probe_inside_hit(p_position, vec3(-1.0, 0.0, 0.0), p_dynamic);
	inside_hits += probe_inside_hit(p_position, vec3(0.0, 1.0, 0.0), p_dynamic);
	inside_hits += probe_inside_hit(p_position, vec3(0.0, -1.0, 0.0), p_dynamic);
	inside_hits += probe_inside_hit(p_position, vec3(0.0, 0.0, 1.0), p_dynamic);
	inside_hits += probe_inside_hit(p_position, vec3(0.0, 0.0, -1.0), p_dynamic);
	return inside_hits > 3;
}

bool selected_for_temporal_update(int p_index) {
	if (params.update_count <= 0) {
		return false;
	}
	int relative = p_index - params.temporal_cursor;
	if (relative < 0) {
		relative += params.probe_count;
	}
	return relative < params.update_count;
}

void main() {
	int probe_index = int(gl_GlobalInvocationID.x);
	if (probe_index >= params.probe_count) {
		return;
	}

	vec3 probe_position = probe_positions.data[probe_index].xyz;
	bool probe_enabled = !probe_is_embedded(probe_position, false) && !probe_is_embedded(probe_position, true);
	probe_active.data[probe_index] = probe_enabled ? 1u : 0u;
	vec3 spherical_irradiance = vec3(0.0);
	float solid_angle = 4.0 * PI / float(params.rays_per_probe);
	for (int ray_index = 0; ray_index < params.rays_per_probe; ray_index++) {
		int output_index = probe_index * params.rays_per_probe + ray_index;
		vec3 direction = normalize(ray_directions.data[ray_index].xyz);
		Hit hit = trace_scene(probe_position, direction);
		vec3 incoming = vec3(0.0);
		float distance = params.far_distance;
		if (hit.valid) {
			distance = hit.distance;
			vec3 direct = direct_irradiance(hit, direction);
			incoming = hit.albedo * direct / PI;
			if (params.multi_bounce != 0u && params.history_valid != 0u) {
				vec3 normal = normalize(hit.normal);
				if (dot(normal, -direction) < 0.0) {
					normal = -normal;
				}
				incoming += hit.albedo * sample_previous_irradiance(hit.position, normal) / (4.0 * PI);
			}
		}
		ray_radiance.data[output_index] = vec4(incoming, 1.0);
		sample_moments.data[output_index].mean = distance;
		sample_moments.data[output_index].second = distance * distance;
		sample_moments.data[output_index].pad0 = 0.0;
		sample_moments.data[output_index].pad1 = 0.0;
		spherical_irradiance += incoming * solid_angle;
	}

	vec4 sample_value = vec4(spherical_irradiance, 1.0);
	sample_irradiance.data[probe_index] = sample_value;
	bool initialize_history = params.history_valid == 0u;
	bool update_history = initialize_history || selected_for_temporal_update(probe_index);
	if (!probe_enabled) {
		estimate_irradiance.data[probe_index] = vec4(0.0, 0.0, 0.0, 1.0);
	} else if (initialize_history) {
		estimate_irradiance.data[probe_index] = sample_value;
	} else if (update_history) {
		estimate_irradiance.data[probe_index] = vec4(mix(history_irradiance.data[probe_index].rgb, sample_value.rgb, 1.0 - params.temporal_hysteresis), 1.0);
	} else {
		estimate_irradiance.data[probe_index] = history_irradiance.data[probe_index];
	}

	for (int ray_index = 0; ray_index < params.rays_per_probe; ray_index++) {
		int output_index = probe_index * params.rays_per_probe + ray_index;
		Moment current = sample_moments.data[output_index];
		Moment estimate = current;
		if (!initialize_history && update_history) {
			Moment previous = history_moments.data[output_index];
			estimate.mean = mix(previous.mean, current.mean, 1.0 - params.temporal_hysteresis);
			estimate.second = mix(previous.second, current.second, 1.0 - params.temporal_hysteresis);
		} else if (!initialize_history) {
			estimate = history_moments.data[output_index];
		}
		estimate_moments.data[output_index] = estimate;
	}
}
