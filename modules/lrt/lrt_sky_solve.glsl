#[compute]

#version 450

// Geometry-only SH4 visibility recurrence, independent of RGB reflection and emission.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
} params;
layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer { vec4 data[]; } material;
layout(set = 0, binding = 2, std430) restrict readonly buffer LocalVisibility { vec4 data[]; } local_visibility;
layout(set = 0, binding = 17, std430) restrict readonly buffer VisibilityIn { vec4 data[]; } visibility_in;
layout(set = 0, binding = 18, std430) restrict writeonly buffer VisibilityOut { vec4 data[]; } visibility_out;

const float C0 = 0.2820947918;
const float WEIGHT = 1.0 / 26.0;
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

vec4 product(vec4 a, vec4 b) {
	return C0 * vec4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	vec4 incoming = vec4(0.0);
	if (material.data[index].a <= 0.5) {
		int width = params.grid_size.x * params.grid_size.z;
		int rest = int(index) % width;
		ivec3 p = ivec3(rest % params.grid_size.x, int(index) / width, rest / params.grid_size.x);
		for (int j = 0; j < 26; j++) {
			ivec3 q = p + OFFSETS[j];
			vec4 value = vec4(1.0 / C0, 0.0, 0.0, 0.0);
			if (all(greaterThanEqual(q, ivec3(0))) && all(lessThan(q, params.grid_size.xyz))) {
				uint neighbor = uint(q.x + q.z * params.grid_size.x + q.y * width);
				vec4 reverse_visibility = local_visibility.data[neighbor] * vec4(1.0, -1.0, -1.0, -1.0);
				value = product(visibility_in.data[neighbor], reverse_visibility);
			}
			incoming += WEIGHT * value;
		}
	}
	visibility_out.data[index] = product(incoming, local_visibility.data[index]);
}
