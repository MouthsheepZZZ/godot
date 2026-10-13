#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FilterDataBlock {
	mat4 inv_projection;
	mat4 projection;
	ivec4 screen_size;
}
gather;

layout(set = 0, binding = 1) uniform texture2D lighting_input;
layout(set = 0, binding = 2) uniform texture2D depth_buffer;
layout(set = 0, binding = 3) uniform texture2D normal_buffer;
layout(set = 0, binding = 4) uniform sampler nearest_sampler;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D lighting_output;

layout(push_constant, std430) uniform FilterPass {
	ivec4 axis_resolve;
}
filter_pass;

// Surface-space support radius in scene units, independent of screen resolution.
const float FILTER_RADIUS = 0.3;
const int FILTER_HALF_TAPS = 6;

ivec2 full_coord(ivec2 coord) {
	return min(ivec2((vec2(coord) + 0.5) * vec2(gather.screen_size.xy) /
					   vec2(gather.screen_size.zw)),
			gather.screen_size.xy - ivec2(1));
}

vec3 view_position(ivec2 coord) {
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), coord, 0).r;
	vec2 ndc = 2.0 * (vec2(coord) + 0.5) / vec2(gather.screen_size.xy) - 1.0;
	vec4 position = gather.inv_projection * vec4(ndc, depth, 1.0);
	return position.xyz / position.w;
}

vec3 packed_normal(ivec2 coord) {
	return texelFetch(sampler2D(normal_buffer, nearest_sampler), coord, 0).xyz;
}

float geometry_weight(vec3 position, vec3 normal, vec3 neighbor_position, vec3 neighbor_normal, float tolerance) {
	vec3 delta = neighbor_position - position;
	float plane_distance = max(abs(dot(delta, normal)), abs(dot(delta, neighbor_normal)));
	float normal_weight = pow(max(dot(normal, neighbor_normal), 0.0), 8.0);
	return exp(-0.5 * pow(plane_distance / tolerance, 2.0)) * normal_weight;
}

vec3 surface_tangent(vec3 normal) {
	vec3 reference = abs(normal.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	return normalize(reference - normal * dot(reference, normal));
}

void main() {
	bool resolve = filter_pass.axis_resolve.z != 0;
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 output_size = resolve ? gather.screen_size.xy : gather.screen_size.zw;
	if (any(greaterThanEqual(coord, output_size))) {
		return;
	}
	ivec2 center_full = resolve ? coord : full_coord(coord);
	vec3 center_packed = packed_normal(center_full);
	if (all(lessThan(abs(center_packed), vec3(0.00001)))) {
		imageStore(lighting_output, coord, vec4(0.0));
		return;
	}
	vec3 position = view_position(center_full);
	vec3 normal = normalize(center_packed * 2.0 - 1.0);
	// One gather texel in view space; support perspective and orthographic cameras.
	float projection_scale = abs(gather.inv_projection[2][3]) > 0.00001 ? abs(position.z) : 1.0;
	vec2 texel_size = 2.0 * abs(vec2(gather.inv_projection[0][0], gather.inv_projection[1][1])) *
			projection_scale / vec2(gather.screen_size.zw);
	float tolerance = resolve ? max(max(texel_size.x, texel_size.y) * 0.5, 0.0001) : FILTER_RADIUS * 0.05;
	vec3 tangent = surface_tangent(normal);
	vec3 direction = filter_pass.axis_resolve.x != 0 ? tangent : cross(normal, tangent);
	vec4 total = vec4(0.0);
	float total_weight = 0.0;
	vec2 gather_position = (vec2(coord) + 0.5) * vec2(gather.screen_size.zw) / vec2(gather.screen_size.xy) - 0.5;
	ivec2 base = resolve ? ivec2(floor(gather_position + 0.5)) : coord;
	int count = resolve ? 9 : FILTER_HALF_TAPS * 2 + 1;
	for (int index = 0; index < count; index++) {
		ivec2 neighbor_coord;
		float surface_offset = 0.0;
		if (resolve) {
			neighbor_coord = base + ivec2(index % 3 - 1, index / 3 - 1);
		} else {
			surface_offset = float(index - FILTER_HALF_TAPS) * FILTER_RADIUS / float(FILTER_HALF_TAPS);
			vec3 sample_position = position + direction * surface_offset;
			vec4 clip = gather.projection * vec4(sample_position, 1.0);
			if (clip.w <= 0.0) {
				continue;
			}
			vec2 sample_coord = (clip.xy / clip.w * 0.5 + 0.5) * vec2(gather.screen_size.zw);
			neighbor_coord = ivec2(floor(sample_coord));
		}
		if (any(lessThan(neighbor_coord, ivec2(0))) || any(greaterThanEqual(neighbor_coord, gather.screen_size.zw))) {
			continue;
		}
		ivec2 neighbor_full = full_coord(neighbor_coord);
		vec3 neighbor_packed = packed_normal(neighbor_full);
		if (all(lessThan(abs(neighbor_packed), vec3(0.00001)))) {
			continue;
		}
		float spatial_weight;
		if (resolve) {
			vec2 tent = max(vec2(1.5) - abs(vec2(neighbor_coord) - gather_position), vec2(0.0));
			spatial_weight = tent.x * tent.y;
		} else {
			float normalized_offset = surface_offset / (FILTER_RADIUS * 0.5);
			spatial_weight = exp(-0.5 * normalized_offset * normalized_offset);
		}
		float weight = spatial_weight * geometry_weight(position, normal, view_position(neighbor_full), normalize(neighbor_packed * 2.0 - 1.0), tolerance);
		// Filter premultiplied RGB and coverage together, including zero-coverage surfaces.
		total += texelFetch(sampler2D(lighting_input, nearest_sampler), neighbor_coord, 0) * weight;
		total_weight += weight;
	}
	imageStore(lighting_output, coord, total_weight > 0.0 ? total / total_weight : vec4(0.0));
}
