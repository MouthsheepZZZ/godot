#[compute]

#version 450

// Preserve all 26 directions during neighbor transport; project only at publication.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(push_constant, std430) uniform Publish { ivec4 flags; } publish;
layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
} params;
layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer { vec4 data[]; } material;
layout(set = 0, binding = 17, std430) restrict readonly buffer VisibilityIn { uint data[]; } visibility_in;
layout(set = 0, binding = 18, std430) restrict writeonly buffer VisibilityOut { uint data[]; } visibility_out;
layout(set = 0, binding = 19, std430) restrict writeonly buffer PublishedVisibility { vec4 data[]; } published_visibility;

const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float WEIGHT = 12.566370614359172 / 26.0;
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

uint blocked_connections(uint index) {
	uvec2 halves = uvec2(material.data[index].xz);
	return halves.x | (halves.y << 13u);
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	if (publish.flags.x != 0) {
		vec4 sh = vec4(0.0);
		uint mask = visibility_in.data[index];
		for (int j = 0; j < 26; j++) {
			if ((mask & (1u << uint(j))) != 0u) {
				sh += WEIGHT * vec4(C0, C1 * normalize(vec3(OFFSETS[j])));
			}
		}
		published_visibility.data[index] = sh;
		return;
	}
	uint result = 0u;
	if (material.data[index].a > 0.5) {
		visibility_out.data[index] = result;
		return;
	}
	int width = params.grid_size.x * params.grid_size.z;
	int rest = int(index) % width;
	ivec3 p = ivec3(rest % params.grid_size.x, int(index) / width, rest / params.grid_size.x);
	uint blocked = blocked_connections(index);
	for (int j = 0; j < 26; j++) {
		uint bit = 1u << uint(j);
		if ((blocked & bit) != 0u) {
			continue;
		}
		ivec3 q = p + OFFSETS[j];
		if (any(lessThan(q, ivec3(0))) || any(greaterThanEqual(q, params.grid_size.xyz))) {
			result |= bit;
			continue;
		}
		uint neighbor = uint(q.x + q.z * params.grid_size.x + q.y * width);
		if (material.data[neighbor].a > 0.5 || (blocked_connections(neighbor) & (1u << uint(25 - j))) != 0u) {
			continue;
		}
		result |= visibility_in.data[neighbor] & bit;
	}
	visibility_out.data[index] = result;
}
