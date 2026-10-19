#[compute]

#version 450

// Derive render coefficients only from the last complete uint26 publication.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int probe_count;
	int project_visibility;
	int pad1;
	int pad2;
} push_constant;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
	vec4 sky_link_radiance[3][26];
} params;

layout(set = 0, binding = 17, std430) restrict readonly buffer PublishedVisibility { uint data[]; } visibility;
layout(set = 0, binding = 18, std430) restrict writeonly buffer VisibilitySH { vec4 data[]; } visibility_sh;
layout(set = 0, binding = 19, std430) restrict writeonly buffer SkyR { vec4 data[]; } sky_r;
layout(set = 0, binding = 20, std430) restrict writeonly buffer SkyG { vec4 data[]; } sky_g;
layout(set = 0, binding = 21, std430) restrict writeonly buffer SkyB { vec4 data[]; } sky_b;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

void main() {
	uint probe = gl_GlobalInvocationID.x;
	if (probe >= uint(push_constant.probe_count)) {
		return;
	}
	vec4 h = vec4(0.0);
	vec4 red = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 blue = vec4(0.0);
	uint mask = visibility.data[probe];
	while (mask != 0u) {
		int direction = findLSB(mask);
		if (push_constant.project_visibility != 0) {
			h += (4.0 * PI / 26.0) * vec4(C0, (C1 / 3.0) * normalize(vec3(OFFSETS[direction])));
		}
		red += params.sky_link_radiance[0][direction];
		green += params.sky_link_radiance[1][direction];
		blue += params.sky_link_radiance[2][direction];
		mask &= mask - 1u;
	}
	if (push_constant.project_visibility != 0) {
		visibility_sh.data[probe] = h;
	}
	sky_r.data[probe] = red;
	sky_g.data[probe] = green;
	sky_b.data[probe] = blue;
}
