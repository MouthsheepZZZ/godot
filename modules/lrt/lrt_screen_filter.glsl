#[versions]

prepare = "#define MODE_PREPARE";
reduce = "#define MODE_REDUCE";
filter = "#define MODE_FILTER";
resolve = "#define MODE_RESOLVE";

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FilterDataBlock {
	mat4 inv_projection;
	mat4 projection;
	ivec4 screen_size;
} gather;

layout(set = 0, binding = 2) uniform texture2D depth_buffer;
layout(set = 0, binding = 3) uniform texture2D normal_buffer;
layout(set = 0, binding = 4) uniform sampler nearest_sampler;

#ifndef MODE_PREPARE
layout(set = 0, binding = 1) uniform texture2D lighting_input;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D lighting_output;
layout(set = 0, binding = 6) uniform texture2D geometry_input;
#endif

#if defined(MODE_PREPARE) || defined(MODE_REDUCE)
layout(rg32f, set = 0, binding = 7) uniform restrict writeonly image2D geometry_output;
#endif

#ifdef MODE_FILTER
layout(set = 0, binding = 8) uniform texture2D coarse_lighting;
layout(set = 0, binding = 9) uniform texture2D coarse_geometry;
layout(push_constant, std430) uniform FilterPass {
	ivec4 has_coarse;
} filter_pass;
#endif

const float FILTER_RADIUS = 0.3;
const float FILTER_SIGMA = 0.1;
const float GAUSSIAN_EXP2 = -0.7213475204444817;

vec3 view_position(ivec2 coord) {
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), coord, 0).r;
	vec2 ndc = 2.0 * (vec2(coord) + 0.5) / vec2(gather.screen_size.xy) - 1.0;
	vec4 position = gather.inv_projection * vec4(ndc, depth, 1.0);
	return position.xyz / position.w;
}

vec3 packed_normal(ivec2 coord) {
	return texelFetch(sampler2D(normal_buffer, nearest_sampler), coord, 0).xyz;
}

float plane_tolerance(vec3 position) {
	float projection_scale = abs(gather.inv_projection[2][3]) > 0.00001 ? abs(position.z) : 1.0;
	vec2 texel_size = 2.0 * abs(vec2(gather.inv_projection[0][0], gather.inv_projection[1][1])) *
			projection_scale / vec2(gather.screen_size.zw);
	return max(max(texel_size.x, texel_size.y) * 0.5, 0.0001);
}

float geometry_weight(vec3 delta, vec3 normal, vec3 neighbor_normal, float tolerance, float spatial_exponent) {
	float normal_weight = max(dot(normal, neighbor_normal), 0.0);
	float plane_distance = max(abs(dot(delta, normal)), abs(dot(delta, neighbor_normal))) / tolerance;
	normal_weight *= normal_weight;
	normal_weight *= normal_weight;
	normal_weight *= normal_weight;
	return exp2(GAUSSIAN_EXP2 * (spatial_exponent + plane_distance * plane_distance)) * normal_weight;
}

#ifndef MODE_PREPARE
bool read_geometry(texture2D field, ivec2 coord, out vec3 position, out vec3 normal) {
	vec2 full = texelFetch(sampler2D(field, nearest_sampler), coord, 0).xy;
	if (full.x < 0.0) {
		return false;
	}
	position = view_position(ivec2(full));
	normal = normalize(packed_normal(ivec2(full)) * 2.0 - 1.0);
	return true;
}
#endif

#if defined(MODE_FILTER) || defined(MODE_RESOLVE)
vec4 reconstruct(texture2D lighting, texture2D geometry, vec2 uv, vec3 position, vec3 normal, float tolerance, out float total_weight) {
	ivec2 size = textureSize(sampler2D(lighting, nearest_sampler), 0);
	vec2 sample_position = uv * vec2(size) - 0.5;
	ivec2 base = ivec2(floor(sample_position + 0.5));
	vec4 total = vec4(0.0);
	total_weight = 0.0;
	float spatial_total = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 neighbor = base + ivec2(x, y);
			if (any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size))) {
				continue;
			}
			vec3 neighbor_position;
			vec3 neighbor_normal;
			vec2 tent = max(vec2(1.5) - abs(vec2(neighbor) - sample_position), vec2(0.0));
			float spatial_weight = tent.x * tent.y;
			spatial_total += spatial_weight;
			if (!read_geometry(geometry, neighbor, neighbor_position, neighbor_normal)) {
				continue;
			}
			float weight = spatial_weight * geometry_weight(neighbor_position - position, normal, neighbor_normal, tolerance, 0.0);
			total += texelFetch(sampler2D(lighting, nearest_sampler), neighbor, 0) * weight;
			total_weight += weight;
		}
	}
	vec4 reconstructed = total_weight > 0.0 ? total / total_weight : vec4(0.0);
	total_weight = spatial_total > 0.0 ? clamp(total_weight / spatial_total, 0.0, 1.0) : 0.0;
	return reconstructed;
}
#endif

#ifdef MODE_FILTER
float projected_radius(vec3 position, ivec2 size) {
	vec4 clip = gather.projection * vec4(position, 1.0);
	vec3 denominator_axis = vec3(gather.projection[0].w, gather.projection[1].w, gather.projection[2].w);
	float denominator_squared = dot(denominator_axis, denominator_axis);
	if (clip.w <= FILTER_RADIUS * sqrt(denominator_squared)) {
		return float(max(size.x, size.y));
	}
	float radius_squared = FILTER_RADIUS * FILTER_RADIUS;
	float divisor = clip.w * clip.w - radius_squared * denominator_squared;
	float radius_pixels = 0.0;
	for (int axis = 0; axis < 2; axis++) {
		vec3 numerator_axis = vec3(gather.projection[0][axis], gather.projection[1][axis], gather.projection[2][axis]);
		float axes_product = dot(numerator_axis, denominator_axis);
		float midpoint = clip[axis] * clip.w - radius_squared * axes_product;
		vec3 tangent = clip.w * numerator_axis - clip[axis] * denominator_axis;
		float discriminant = dot(tangent, tangent) - radius_squared * (dot(numerator_axis, numerator_axis) * denominator_squared - axes_product * axes_product);
		float spread = FILTER_RADIUS * sqrt(max(discriminant, 0.0));
		vec2 limits = vec2(midpoint - spread, midpoint + spread) / divisor;
		radius_pixels = max(radius_pixels, max(abs(limits.x - clip[axis] / clip.w), abs(limits.y - clip[axis] / clip.w)) * 0.5 * float(size[axis]));
	}
	return radius_pixels;
}
#endif

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
#if defined(MODE_PREPARE)
	ivec2 size = imageSize(geometry_output);
#else
	ivec2 size = imageSize(lighting_output);
#endif
	if (any(greaterThanEqual(coord, size))) {
		return;
	}

#ifdef MODE_PREPARE
	ivec2 full = min(ivec2((vec2(coord) + 0.5) * vec2(gather.screen_size.xy) / vec2(size)), gather.screen_size.xy - 1);
	vec3 packed = packed_normal(full);
	bool valid = any(greaterThanEqual(abs(packed), vec3(0.00001)));
	imageStore(geometry_output, coord, vec4(valid ? vec2(full) : vec2(-1.0), 0.0, 0.0));
#endif

#ifdef MODE_REDUCE
	ivec2 input_size = textureSize(sampler2D(lighting_input, nearest_sampler), 0);
	vec2 footprint_min = vec2(coord) * vec2(input_size) / vec2(size);
	vec2 footprint_max = vec2(coord + 1) * vec2(input_size) / vec2(size);
	ivec2 base = ivec2(floor(footprint_min));
	vec2 target = (vec2(coord) + 0.5) * vec2(gather.screen_size.xy) / vec2(size) - 0.5;
	vec2 representative = vec2(-1.0);
	float closest = 1e30;
	// At odd dimensions, a parent footprint can overlap three children per axis.
	for (int y = 0; y < 3; y++) {
		for (int x = 0; x < 3; x++) {
			ivec2 child = base + ivec2(x, y);
			if (any(greaterThanEqual(child, input_size)) || any(greaterThanEqual(vec2(child), footprint_max))) {
				continue;
			}
			vec2 full = texelFetch(sampler2D(geometry_input, nearest_sampler), child, 0).xy;
			if (full.x < 0.0) {
				continue;
			}
			float distance_squared = dot(full - target, full - target);
			if (distance_squared < closest) {
				closest = distance_squared;
				representative = full;
			}
		}
	}
	imageStore(geometry_output, coord, vec4(representative, 0.0, 0.0));
	if (representative.x < 0.0) {
		imageStore(lighting_output, coord, vec4(0.0));
		return;
	}
	vec3 position = view_position(ivec2(representative));
	vec3 normal = normalize(packed_normal(ivec2(representative)) * 2.0 - 1.0);
	float tolerance = plane_tolerance(position);
	vec4 total = vec4(0.0);
	float total_weight = 0.0;
	for (int y = 0; y < 3; y++) {
		for (int x = 0; x < 3; x++) {
			ivec2 child = base + ivec2(x, y);
			if (any(greaterThanEqual(child, input_size)) || any(greaterThanEqual(vec2(child), footprint_max))) {
				continue;
			}
			vec3 neighbor_position;
			vec3 neighbor_normal;
			if (!read_geometry(geometry_input, child, neighbor_position, neighbor_normal)) {
				continue;
			}
			vec2 overlap = max(min(vec2(child + 1), footprint_max) - max(vec2(child), footprint_min), vec2(0.0));
			float weight = overlap.x * overlap.y * geometry_weight(neighbor_position - position, normal, neighbor_normal, tolerance, 0.0);
			total += texelFetch(sampler2D(lighting_input, nearest_sampler), child, 0) * weight;
			total_weight += weight;
		}
	}
	imageStore(lighting_output, coord, total / total_weight);
#endif

#ifdef MODE_FILTER
	vec3 position;
	vec3 normal;
	if (!read_geometry(geometry_input, coord, position, normal)) {
		imageStore(lighting_output, coord, vec4(0.0));
		return;
	}
	float tolerance = plane_tolerance(position);
	float coarse_mix = 0.0;
	vec4 coarse = vec4(0.0);
	if (filter_pass.has_coarse.x != 0) {
		coarse_mix = smoothstep(2.0, 3.0, projected_radius(position, size));
		if (coarse_mix > 0.0) {
			float coarse_weight;
			coarse = reconstruct(coarse_lighting, coarse_geometry, (vec2(coord) + 0.5) / vec2(size), position, normal, tolerance, coarse_weight);
			// Weak geometric support must not become full influence after normalization.
			coarse_mix *= coarse_weight;
		}
	}
	if (coarse_mix == 1.0) {
		imageStore(lighting_output, coord, coarse);
		return;
	}
	vec4 total = vec4(0.0);
	float total_weight = 0.0;
	for (int y = -3; y <= 3; y++) {
		for (int x = -3; x <= 3; x++) {
			ivec2 neighbor = coord + ivec2(x, y);
			if (any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size))) {
				continue;
			}
			vec3 neighbor_position;
			vec3 neighbor_normal;
			if (!read_geometry(geometry_input, neighbor, neighbor_position, neighbor_normal)) {
				continue;
			}
			vec3 delta = neighbor_position - position;
			float distance_squared = dot(delta, delta);
			if (distance_squared > FILTER_RADIUS * FILTER_RADIUS) {
				continue;
			}
			float weight = geometry_weight(delta, normal, neighbor_normal, tolerance, distance_squared / (FILTER_SIGMA * FILTER_SIGMA));
			total += texelFetch(sampler2D(lighting_input, nearest_sampler), neighbor, 0) * weight;
			total_weight += weight;
		}
	}
	imageStore(lighting_output, coord, mix(total / total_weight, coarse, coarse_mix));
#endif

#ifdef MODE_RESOLVE
	vec3 packed = packed_normal(coord);
	if (all(lessThan(abs(packed), vec3(0.00001)))) {
		imageStore(lighting_output, coord, vec4(0.0));
		return;
	}
	vec3 position = view_position(coord);
	vec3 normal = normalize(packed * 2.0 - 1.0);
	float weight;
	vec4 lighting = reconstruct(lighting_input, geometry_input, (vec2(coord) + 0.5) / vec2(size), position, normal, plane_tolerance(position), weight);
	imageStore(lighting_output, coord, lighting);
#endif
}
