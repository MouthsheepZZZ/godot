#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform GatherDataBlock {
	mat4 inv_projection;
	mat4 view_to_world;
	ivec4 screen_size;
} gather;

layout(set = 0, binding = 1) uniform texture2D lighting_input;
layout(set = 0, binding = 2) uniform texture2D depth_buffer;
layout(set = 0, binding = 3) uniform sampler nearest_sampler;
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D lighting_output;

layout(push_constant, std430) uniform FilterDirection {
	ivec4 axis;
} filter_direction;

// Keep the display smoothing scale independent of probe spacing and camera distance.
const float FILTER_RADIUS = 0.2;

vec3 view_position(ivec2 coord) {
	ivec2 full_coord = min(ivec2((vec2(coord) + 0.5) * vec2(gather.screen_size.xy) /
			vec2(gather.screen_size.zw)), gather.screen_size.xy - ivec2(1));
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), full_coord, 0).r;
	vec2 ndc = 2.0 * (vec2(full_coord) + 0.5) / vec2(gather.screen_size.xy) - 1.0;
	vec4 position = gather.inv_projection * vec4(ndc, depth, 1.0);
	return position.xyz / position.w;
}

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(coord, gather.screen_size.zw))) {
		return;
	}
	vec4 center = texelFetch(sampler2D(lighting_input, nearest_sampler), coord, 0);
	if (center.a <= 0.0) {
		imageStore(lighting_output, coord, center);
		return;
	}
	vec3 position = view_position(coord);
	float projection_scale = abs(gather.inv_projection[2][3]) > 0.00001 ? abs(position.z) : 1.0;
	vec2 pixel_radius = FILTER_RADIUS * vec2(gather.screen_size.zw) * 0.5 /
			(vec2(abs(gather.inv_projection[0][0]), abs(gather.inv_projection[1][1])) * projection_scale);
	vec2 pixel_step = max(pixel_radius * 0.125, vec2(1.0)) * vec2(filter_direction.axis.xy);
	vec3 light = center.rgb / center.a;
	float total = 1.0;
	for (int index = -8; index <= 8; index++) {
		if (index == 0) {
			continue;
		}
		ivec2 neighbor_coord = coord + ivec2(round(pixel_step * float(index)));
		if (any(lessThan(neighbor_coord, ivec2(0))) || any(greaterThanEqual(neighbor_coord, gather.screen_size.zw))) {
			continue;
		}
		vec4 neighbor = texelFetch(sampler2D(lighting_input, nearest_sampler), neighbor_coord, 0);
		if (neighbor.a <= 0.0) {
			continue;
		}
		float distance = length(view_position(neighbor_coord) - position);
		// Position distance rejects separated surfaces without rejecting a connected wall corner.
		float gaussian = exp(-0.03125 * float(index * index));
		float weight = gaussian * (1.0 - smoothstep(FILTER_RADIUS, 2.0 * FILTER_RADIUS, distance)) * min(center.a, neighbor.a);
		light += neighbor.rgb / neighbor.a * weight;
		total += weight;
	}
	imageStore(lighting_output, coord, vec4(light / total * center.a, center.a));
}
