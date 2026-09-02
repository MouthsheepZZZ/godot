#[compute]

#version 450

#VERSION_DEFINES

#define SH_Y00 0.28209479177387814
#define FULLY_VISIBLE_X (1.0 / SH_Y00)

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer LocalVisibility {
	vec4 values[];
}
local_visibility;

layout(set = 0, binding = 1, std430) restrict readonly buffer GlobalVisibilityInput {
	vec4 values[];
}
global_visibility_input;

layout(set = 0, binding = 2, std430) restrict writeonly buffer GlobalVisibilityOutput {
	vec4 values[];
}
global_visibility_output;

layout(push_constant, std430) uniform Params {
	ivec3 resolution;
	int probe_count;
	int probe_offset;
	int dispatch_probe_count;
}
params;

int probe_index(ivec3 position) {
	return position.x + params.resolution.x * (position.y + params.resolution.y * position.z);
}

bool is_valid_position(ivec3 position) {
	return all(greaterThanEqual(position, ivec3(0))) && all(lessThan(position, params.resolution));
}

float neighbor_weight(ivec3 offset) {
	const float normalization = 6.0 + 12.0 / sqrt(2.0) + 8.0 / sqrt(3.0);
	return (1.0 / length(vec3(offset))) / normalization;
}

vec4 triple_product(vec4 a, vec4 b) {
	return vec4(
			dot(a, b),
			a.x * b.y + b.x * a.y,
			a.x * b.z + b.x * a.z,
			a.x * b.w + b.x * a.w) * SH_Y00;
}

void main() {
	int index = params.probe_offset + int(gl_GlobalInvocationID.x);
	if (index >= params.probe_count || index >= params.probe_offset + params.dispatch_probe_count) {
		return;
	}

	int plane_size = params.resolution.x * params.resolution.y;
	ivec3 position;
	position.z = index / plane_size;
	int plane_index = index - position.z * plane_size;
	position.y = plane_index / params.resolution.x;
	position.x = plane_index - position.y * params.resolution.x;

	vec4 gathered = vec4(0.0);
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 offset = ivec3(x, y, z);
				if (all(equal(offset, ivec3(0)))) {
					continue;
				}
				ivec3 neighbor_position = position + offset;
				vec4 neighbor_visibility = is_valid_position(neighbor_position) ? global_visibility_input.values[probe_index(neighbor_position)] : vec4(FULLY_VISIBLE_X, 0.0, 0.0, 0.0);
				gathered += neighbor_visibility * neighbor_weight(offset);
			}
		}
	}
	global_visibility_output.values[index] = triple_product(gathered, local_visibility.values[index]);
}
