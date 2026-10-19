#[compute]

#version 450

// Geometry-only uint26 recurrence. RGB transport never reads these work banks.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
} params;

layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer { vec4 data[]; } material;
layout(set = 0, binding = 2, std430) restrict readonly buffer LinksBuffer { uint data[]; } links;
layout(set = 0, binding = 17, std430) restrict readonly buffer VisibilityIn { uint data[]; } visibility_in;
layout(set = 0, binding = 18, std430) restrict writeonly buffer VisibilityOut { uint data[]; } visibility_out;

const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	uint result = 0u;
	if (material.data[index].a <= 0.5) {
		int width = params.grid_size.x * params.grid_size.z;
		int rest = int(index) % width;
		ivec3 p = ivec3(rest % params.grid_size.x, int(index) / width, rest / params.grid_size.x);
		uint mask = links.data[index];
		while (mask != 0u) {
			int direction = findLSB(mask);
			uint bit = 1u << uint(direction);
			ivec3 q = p + OFFSETS[direction];
			if (any(lessThan(q, ivec3(0))) || any(greaterThanEqual(q, params.grid_size.xyz))) {
				result |= bit;
			} else {
				uint neighbor = uint(q.x + q.z * params.grid_size.x + q.y * width);
				result |= visibility_in.data[neighbor] & bit;
			}
			mask &= mask - 1u;
		}
	}
	visibility_out.data[index] = result;
}
