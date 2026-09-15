#[compute]

#version 450

// Resolves one native Forward+ receiver atlas directly into the inactive per-light
// irradiance field. The CPU never reads or decodes the capture texture.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D capture_texture;

layout(set = 0, binding = 1, std430) restrict readonly buffer ReceiverBuffer {
	vec4 data[];
}
receivers;

layout(set = 0, binding = 2, std430) restrict buffer NativeLightUnitBuffer {
	vec4 data[];
}
native_light_units;

layout(push_constant, std430) uniform Params {
	mat4 volume_to_source;
	vec4 ranges;
	ivec4 layout_data;
	ivec4 kind;
}
params;

float range_window(float distance_to_light, float light_range) {
	float normalized = distance_to_light / max(light_range, 1e-6);
	float normalized_squared = normalized * normalized;
	float window = max(1.0 - normalized_squared * normalized_squared, 0.0);
	return window * window;
}

void main() {
	int local_index = int(gl_GlobalInvocationID.x);
	if (local_index >= params.layout_data.y) {
		return;
	}
	int receiver_index = params.layout_data.x + local_index;
	int pixel_y = params.kind.w - 1 - local_index / params.layout_data.z;
	ivec2 pixel = ivec2(local_index % params.layout_data.z, pixel_y);
	vec3 value = texelFetch(capture_texture, pixel, 0).rgb;
	if (params.kind.y == 0) {
		int receiver_base = receiver_index * 3;
		vec3 receiver_position = receivers.data[receiver_base].xyz;
		vec3 surface_normal = receivers.data[receiver_base + 1].xyz;
		vec3 local_point = (params.volume_to_source * vec4(receiver_position + surface_normal * 0.001, 1.0)).xyz;
		float distance_to_light = length(local_point);
		if (params.kind.z != 0) {
			vec2 half_size = params.ranges.zw;
			vec3 closest_point = vec3(clamp(local_point.xy, -half_size, half_size), 0.0);
			distance_to_light = distance(local_point, closest_point);
		}
		float capture_window = range_window(distance_to_light, params.ranges.y);
		value *= range_window(distance_to_light, params.ranges.x) / max(capture_window, 1e-8);
	}
	int target_index = params.kind.x * params.layout_data.w + receiver_index;
	native_light_units.data[target_index] = vec4(value, 0.0);
}
