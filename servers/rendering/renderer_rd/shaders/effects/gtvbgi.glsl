// GT-VBGI by Mirko Salm (TinyTexel), https://www.shadertoy.com/view/lfdBWn.
// Adapted from the author's CC0 1.0 licensed implementation.
// Uses the reference default approximate cosine slice sampling (mode 3).

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(set = 0, binding = 1) uniform sampler2D source_normal;
layout(set = 0, binding = 2) uniform sampler2D source_color;
#ifdef GTVBGI_UPSAMPLE
layout(set = 0, binding = 11) uniform sampler2D direct_color;
#endif
#ifdef GTVBGI_TEMPORAL
layout(set = 0, binding = 3) uniform sampler2D previous_geometry;
layout(rgba32f, set = 0, binding = 6) uniform restrict writeonly image2D current_geometry;
layout(set = 0, binding = 7) uniform sampler2D previous_history;
layout(set = 0, binding = 10) uniform sampler2D source_velocity;
layout(set = 0, binding = 12) uniform sampler2D raw_samples;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D current_history;
#elif !defined(GTVBGI_FILTER)
layout(set = 0, binding = 9) uniform sampler2D blue_noise;
#endif
layout(set = 0, binding = 4, std140) uniform ProjectionConstants {
	mat4 projection;
	mat4 inverse_projection;
	mat4 reprojection;
	mat4 previous_view_transform;
}
matrices;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D dest_image;
layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	ivec2 effect_size;
	float radius;
	float thickness;
	float intensity;
	float ao_intensity;
	uint slice_count;
	uint sample_count;
	uint frame;
	uint history_valid;
	float sharpness;
	uint orthogonal;
	uint size_divisor;
}
params;

const float PI = 3.141592653589793;

ivec2 effect_to_screen(ivec2 pixel) {
	int divisor = int(params.size_divisor);
	return min(pixel * divisor + ivec2(divisor / 2), params.screen_size - 1);
}

vec3 view_position(vec2 uv, float depth) {
	vec4 p = matrices.inverse_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return p.xyz / p.w;
}

vec3 load_normal(ivec2 pixel) {
	return normalize(texelFetch(source_normal, pixel, 0).xyz * 2.0 - 1.0);
}

#if !defined(GTVBGI_FILTER) && !defined(GTVBGI_TEMPORAL)
vec4 mix_blue_noise_bits(vec4 noise) {
	const vec4 weights = exp2(-vec4(0.0, 8.0, 16.0, 24.0));
	const vec4 normalized_weights = weights / dot(weights, vec4(1.0));
	return vec4(dot(noise.xyzw, normalized_weights), dot(noise.yzwx, normalized_weights), dot(noise.zwxy, normalized_weights), dot(noise.wxyz, normalized_weights));
}

vec4 random_numbers(ivec2 pixel, uint slice) {
	uint n = params.frame * params.slice_count + slice;
	vec4 noise = mix_blue_noise_bits(texelFetch(blue_noise, pixel & ivec2(63), 0));
	// Reference golden-ratio value jitter stratifies slice directions over time.
	noise.x = float(uint(noise.x * 4294967295.0) + 2654435769u * n) * (1.0 / 4294967296.0);
	uvec2 offset = uvec2(3242174889u, 2447445413u) * n;
	noise.zw = mix_blue_noise_bits(texelFetch(blue_noise, ivec2((uvec2(pixel) + offset) & uvec2(63u)), 0)).zw;
	return noise;
}

// Reference SampleSliceDir, with high-quality approximate importance sampling.
vec2 sample_slice_direction(vec3 normal, float random_value) {
	float angle = random_value * PI;
	vec2 direction = vec2(cos(angle), sin(angle));
	float projected_length = length(normal.xy);
	if (projected_length == 0.0) {
		return direction;
	}
	direction *= dot(direction, normal.xy) < 0.0 ? -1.0 : 1.0;
	vec2 n = normal.xy / projected_length;
	float x = direction.x * n.y - direction.y * n.x;
	float stretch = projected_length + (projected_length - projected_length * projected_length) * 0.15;
	float y = acos(clamp(x * sin(stretch * PI * 0.5), -1.0, 1.0)) / (PI * 0.5);
	vec2 sample_direction;
	sample_direction.y = (1.0 - y) / stretch;
	sample_direction.x = sqrt(clamp(1.0 - sample_direction.y * sample_direction.y, 0.0, 1.0));
	return vec2(sample_direction.x * n.x - sample_direction.y * n.y, sample_direction.y * n.x + sample_direction.x * n.y);
}

vec3 rotate_by_quaternion(vec3 vector, vec4 quaternion) {
	return vector + 2.0 * cross(quaternion.xyz, cross(quaternion.xyz, vector) + quaternion.w * vector);
}

// Integrates abs(sin(view angle)) * cos(normal angle) within a slice.
float slice_cdf(float x, float normal_angle, vec3 coefficients) {
	if (x <= 0.0 || x >= 1.0) {
		return x;
	}
	float phi = x * PI - PI * 0.5;
	return (coefficients.x + coefficients.y * (cos(normal_angle - 2.0 * phi) + coefficients.z * phi));
}

uint interval_mask(vec2 interval) {
	uvec2 bounds = uvec2(floor(clamp(interval, 0.0, 1.0) * 32.0));
	uint lower = bounds.x < 32u ? 0xffffffffu << bounds.x : 0u;
	uint upper = bounds.y > 0u ? 0xffffffffu >> (32u - bounds.y) : 0u;
	return lower & upper;
}

vec4 gather(ivec2 pixel, vec3 position, vec3 normal) {
	vec3 view = params.orthogonal != 0u ? vec3(0.0, 0.0, 1.0) : -normalize(position);
	position += normal * (1.0 / 1024.0);
	vec4 center_clip = matrices.projection * vec4(position, 1.0);
	vec2 ray_start = (center_clip.xy / center_clip.w * 0.5 + 0.5) * vec2(params.screen_size);
	vec3 irradiance = vec3(0.0);
	float visibility = 0.0;
	// Rotate +Z onto the view vector to construct the reference view-vector space.
	vec4 rotation = normalize(vec4(-view.y, view.x, 0.0, 1.0 + view.z));
	vec3 normal_in_view = rotate_by_quaternion(normal, rotation * vec4(-1.0, -1.0, -1.0, 1.0));
	for (uint slice = 0u; slice < params.slice_count; slice++) {
		vec4 rnd = random_numbers(pixel, slice);
		vec2 direction = sample_slice_direction(normal_in_view, rnd.x);
		vec3 sample_direction = rotate_by_quaternion(vec3(direction, 0.0), rotation);
		vec4 direction_clip = matrices.projection * vec4(sample_direction, 0.0);
		vec2 screen_direction = (direction_clip.xy * center_clip.w - center_clip.xy * direction_clip.w) / (center_clip.w * center_clip.w) * 0.5 * vec2(params.screen_size);
		float pixels_per_meter = length(screen_direction);
		float pixel_radius = min(512.0, params.radius * pixels_per_meter);
		if (pixel_radius <= 1.0) {
			visibility += 1.0;
			continue;
		}
		screen_direction /= pixels_per_meter;
		vec3 slice_normal = cross(view, sample_direction);
		vec3 projected_normal = normal - slice_normal * dot(normal, slice_normal);
		float projected_length = length(projected_normal);
		if (projected_length < 0.00001) {
			visibility += 1.0;
			continue;
		}
		float normal_cosine = clamp(dot(projected_normal, view) / projected_length, -1.0, 1.0);
		vec3 tangent = cross(slice_normal, projected_normal);
		float normal_angle = (dot(view, tangent) < 0.0 ? -1.0 : 1.0) * acos(normal_cosine);
		float angle_offset = normal_angle / PI + 0.5;
		float normal_sine = sin(normal_angle);
		float cdf_normalization = 1.0 / (4.0 * (normal_cosine + normal_angle * normal_sine));
		uint occupied = 0u;
		vec3 slice_irradiance = vec3(0.0);
		float step_scale = pow(pixel_radius, 1.0 / float(params.sample_count));
		for (int side = 0; side < 2; side++) {
			if (occupied == 0xffffffffu) {
				break;
			}
			float sign_direction = side == 0 ? -1.0 : 1.0;
			float cdf_base = side == 0 ? normal_cosine + PI * normal_sine : 3.0 * normal_cosine + (4.0 * normal_angle + PI) * normal_sine;
			vec3 cdf_coefficients = vec3(cdf_base * cdf_normalization, -sign_direction * cdf_normalization, 2.0 * normal_sine);
			float distance_pixels = pow(step_scale, side == 0 ? rnd.z : 1.0 - rnd.z);
			for (uint step = 0u; step < params.sample_count; step++) {
				vec2 sample_pixel = ray_start + screen_direction * sign_direction * distance_pixels;
				distance_pixels *= step_scale;
				if (any(lessThan(sample_pixel, vec2(0.0))) || any(greaterThanEqual(sample_pixel, vec2(params.screen_size)))) {
					break;
				}
				ivec2 sample_index = ivec2(sample_pixel);
				vec2 sample_uv = (vec2(sample_index) + 0.5) / vec2(params.screen_size);
				float depth = texelFetch(source_depth, sample_index, 0).r;
				if (depth == 0.0) {
					continue;
				}
				vec3 sample_position = view_position(sample_uv, depth);
				vec3 front = sample_position - position;
				if (dot(front, front) > params.radius * params.radius || dot(front, front) < 0.0000001) {
					continue;
				}
				vec3 back_position = params.orthogonal != 0u ? sample_position - view * params.thickness : sample_position * (1.0 + params.thickness / -sample_position.z);
				vec3 back = back_position - position;
				vec2 angles = acos(clamp(vec2(dot(normalize(front), view), dot(normalize(back), view)), -1.0, 1.0)) * sign_direction;
				vec2 horizon = clamp(angles / PI + angle_offset, 0.0, 1.0);
				if (side == 0) {
					horizon = horizon.yx;
				}
				horizon.x = slice_cdf(horizon.x, normal_angle, cdf_coefficients);
				horizon.y = slice_cdf(horizon.y, normal_angle, cdf_coefficients);
				uint sample_mask = interval_mask(horizon + rnd.w / 32.0);
				uint visible_mask = sample_mask & ~occupied;
				if (visible_mask != 0u) {
// The reference default does not reject back-facing radiance.
#ifdef GTVBGI_UPSAMPLE
					slice_irradiance += texelFetch(direct_color, sample_index, 0).rgb * (float(bitCount(visible_mask)) / 32.0);
#else
					slice_irradiance += texelFetch(source_color, sample_index, 0).rgb * (float(bitCount(visible_mask)) / 32.0);
#endif
				}
				occupied |= sample_mask;
				if (occupied == 0xffffffffu) {
					break;
				}
			}
		}
		visibility += 1.0 - float(bitCount(occupied)) / 32.0;
		irradiance += slice_irradiance;
	}
	float inverse_count = 1.0 / float(params.slice_count);
	return vec4(irradiance * inverse_count, clamp(visibility * inverse_count, 0.0, 1.0));
}
#endif

#ifdef GTVBGI_TEMPORAL
vec2 encode_normal(vec3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(n.xy, vec2(0.0)));
}

vec3 decode_normal(vec2 encoded) {
	vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-n.z, 0.0);
	n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
	return normalize(n);
}

void accumulate(ivec2 pixel, vec2 uv, float depth) {
	vec4 result = texelFetch(source_color, pixel, 0);
	vec3 position = view_position(uv, depth);
	vec3 normal = load_normal(pixel);
	float count = 1.0;
	vec4 previous_clip = matrices.reprojection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 previous_ndc = previous_clip.xyz / previous_clip.w;
	vec2 previous_uv = uv + texelFetch(source_velocity, pixel, 0).xy;
	if (params.history_valid != 0u && previous_clip.w > 0.0 && all(greaterThanEqual(previous_uv, vec2(0.0))) && all(lessThan(previous_uv, vec2(1.0))) && previous_ndc.z >= 0.0 && previous_ndc.z <= 1.0) {
		vec4 previous_position = matrices.previous_view_transform * vec4(position, 1.0);
		float expected_depth = -previous_position.z / previous_position.w;
		vec4 geometry = textureLod(previous_geometry, previous_uv, 0.0);
		mat3 previous_view_rotation = mat3(matrices.previous_view_transform);
		vec3 expected_normal = normalize(previous_view_rotation * normal);
		if (geometry.x > 0.0 && abs(geometry.x - expected_depth) <= max(0.01, expected_depth * 0.01) && dot(expected_normal, decode_normal(geometry.zw)) > 0.95) {
			vec4 history = textureLod(previous_history, previous_uv, 0.0);
			// Bound reprojected history by this frame's local distribution to reject stale lighting.
			vec4 minimum = result;
			vec4 maximum = result;
			// Full-resolution neighbors also retain the sampling range of independently evaluated thin surfaces.
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					ivec2 neighbor = clamp(pixel + ivec2(x, y), ivec2(0), params.screen_size - 1);
					vec4 value = texelFetch(source_color, neighbor, 0);
					minimum = min(minimum, value);
					maximum = max(maximum, value);
				}
			}

			// Use the unfiltered sampling distribution; interpolated values underestimate noise variance.
			ivec2 effect_pixel = pixel / int(params.size_divisor);
			float plane_scale = mix(0.1, 0.005, params.sharpness) * max(-position.z, 0.01);
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					ivec2 neighbor = clamp(effect_pixel + ivec2(x, y), ivec2(0), params.effect_size - 1);
					ivec2 screen_pixel = effect_to_screen(neighbor);
					float neighbor_depth = texelFetch(source_depth, screen_pixel, 0).r;
					if (neighbor_depth == 0.0 || dot(normal, load_normal(screen_pixel)) < 0.9) {
						continue;
					}
					vec3 neighbor_position = view_position((vec2(screen_pixel) + 0.5) / vec2(params.screen_size), neighbor_depth);
					if (abs(dot(neighbor_position - position, normal)) > plane_scale * 2.0) {
						continue;
					}
					vec4 value = texelFetch(raw_samples, neighbor, 0);
					minimum = min(minimum, value);
					maximum = max(maximum, value);
				}
			}
			vec4 clipped = clamp(history, minimum, maximum);
			float relative_change = length(history.rgb - clipped.rgb) / max(length(result.rgb), 0.001);
			float visibility_change = abs(history.a - clipped.a);
			count = min(geometry.y + 1.0, 32.0);
			count = mix(count, 1.0, clamp(max(relative_change, visibility_change * 4.0), 0.0, 1.0));
			// Same running-average equation as the reference, with a bounded game history.
			result = mix(clipped, result, 1.0 / count);
		}
	}
	imageStore(current_geometry, pixel, vec4(-position.z, count, encode_normal(normal)));
	imageStore(current_history, pixel, result);
	imageStore(dest_image, pixel, result);
}
#endif

#ifdef GTVBGI_FILTER
const uint FILTER_TILE_SIZE = 12u;
shared vec4 filter_positions[144];
shared vec3 filter_normals[144];
shared vec4 filter_colors[144];

void cache_filter_tile() {
	ivec2 origin = ivec2(gl_WorkGroupID.xy * 8u) - ivec2(2);
	for (uint index = gl_LocalInvocationIndex; index < 144u; index += 64u) {
		ivec2 pixel = origin + ivec2(index % FILTER_TILE_SIZE, index / FILTER_TILE_SIZE);
		filter_positions[index] = vec4(0.0);
		if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, params.screen_size))) {
			continue;
		}
		ivec2 screen_pixel = pixel;
		float depth = texelFetch(source_depth, screen_pixel, 0).r;
		if (depth == 0.0) {
			continue;
		}
		vec2 uv = (vec2(screen_pixel) + 0.5) / vec2(params.screen_size);
		filter_positions[index] = vec4(view_position(uv, depth), 1.0);
		filter_normals[index] = load_normal(screen_pixel);
		filter_colors[index] = texelFetch(source_color, pixel, 0);
	}
	barrier();
}
#endif

#ifdef GTVBGI_UPSAMPLE
vec4 upsample(ivec2 pixel, vec3 position, vec3 normal) {
	vec2 coordinate = (vec2(pixel) - vec2(1.0)) * 0.5;
	ivec2 base = ivec2(floor(coordinate));
	vec2 fraction = fract(coordinate);
	vec4 result = vec4(0.0);
	float total_weight = 0.0;
	float plane_scale = mix(0.1, 0.005, params.sharpness) * max(-position.z, 0.01);
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			ivec2 neighbor = clamp(base + ivec2(x, y), ivec2(0), params.effect_size - 1);
			ivec2 screen_pixel = effect_to_screen(neighbor);
			float depth = texelFetch(source_depth, screen_pixel, 0).r;
			if (depth == 0.0) {
				continue;
			}
			vec3 sample_position = view_position((vec2(screen_pixel) + 0.5) / vec2(params.screen_size), depth);
			float plane_distance = abs(dot(sample_position - position, normal));
			float normal_cosine = dot(normal, load_normal(screen_pixel));
			if (plane_distance > plane_scale * 2.0 || normal_cosine < 0.9) {
				continue;
			}
			vec2 linear_weights = mix(vec2(1.0) - fraction, fraction, bvec2(x != 0, y != 0));
			float weight = linear_weights.x * linear_weights.y * exp(-plane_distance / plane_scale);
			weight *= pow(max(0.0, normal_cosine), mix(2.0, 32.0, params.sharpness));
			result += texelFetch(source_color, neighbor, 0) * weight;
			total_weight += weight;
		}
	}
	if (total_weight > 0.0) {
		return result / total_weight;
	}
	// Evaluate uncovered surfaces at full resolution rather than importing another surface's lighting.
	return gather(pixel, position, normal);
}
#endif

void main() {
#ifdef GTVBGI_FILTER
	// Every lane, including out-of-bounds lanes, must participate in the tile barrier.
	cache_filter_tile();
#endif
	ivec2 output_pixel = ivec2(gl_GlobalInvocationID.xy);
#if defined(GTVBGI_TEMPORAL) || defined(GTVBGI_UPSAMPLE) || defined(GTVBGI_FILTER)
	ivec2 pixel = output_pixel;
	ivec2 work_size = params.screen_size;
#else
	ivec2 pixel = effect_to_screen(output_pixel);
	ivec2 work_size = params.effect_size;
#endif
	if (any(greaterThanEqual(output_pixel, work_size))) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	float depth = texelFetch(source_depth, pixel, 0).r;
	if (depth == 0.0) {
		imageStore(dest_image, output_pixel, vec4(0.0, 0.0, 0.0, 1.0));
#ifdef GTVBGI_TEMPORAL
		imageStore(current_geometry, pixel, vec4(0.0));
		imageStore(current_history, pixel, vec4(0.0, 0.0, 0.0, 1.0));
#endif
		return;
	}
#if defined(GTVBGI_TEMPORAL)
	accumulate(pixel, uv, depth);
#elif defined(GTVBGI_UPSAMPLE)
	imageStore(dest_image, output_pixel, upsample(pixel, view_position(uv, depth), load_normal(pixel)));
#elif !defined(GTVBGI_FILTER)
	imageStore(dest_image, output_pixel, gather(pixel, view_position(uv, depth), load_normal(pixel)));
#else
	// The same 5x5 bilateral filter, with each tile's data reconstructed only once.
	int center = int(gl_LocalInvocationID.x + 2u + (gl_LocalInvocationID.y + 2u) * FILTER_TILE_SIZE);
	vec3 position = filter_positions[center].xyz;
	vec3 normal = filter_normals[center];
	vec4 result = vec4(0.0);
	float total_weight = 0.0;
	float depth_scale = mix(0.1, 0.005, params.sharpness) * max(-position.z, 0.01);
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			int neighbor = center + x + y * int(FILTER_TILE_SIZE);
			if (filter_positions[neighbor].w == 0.0) {
				continue;
			}
			float weight = exp(-float(x * x + y * y) * 0.25 - abs(dot(filter_positions[neighbor].xyz - position, normal)) / depth_scale);
			weight *= pow(max(0.0, dot(normal, filter_normals[neighbor])), mix(2.0, 32.0, params.sharpness));
			result += filter_colors[neighbor] * weight;
			total_weight += weight;
		}
	}
	result /= total_weight;
	float visibility = params.ao_intensity == 0.0 ? 1.0 : pow(clamp(result.a, 0.0, 1.0), params.ao_intensity);
	imageStore(dest_image, output_pixel, vec4(result.rgb * params.intensity, visibility));
#endif
}
