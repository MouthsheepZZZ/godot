#[compute]

#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int probe_count;
	int project_radiance;
	int pad1;
	int pad2;
}
push_constant;

struct ParamsData {
	ivec4 grid_size;
	vec4 grid_min_spacing;
	ivec4 counts;
	vec4 flags;
	vec4 sky_link_radiance[3][26];
};

layout(set = 0, binding = 0, std140) uniform ParamsBuffer {
	ParamsData params;
};

layout(set = 0, binding = 17, std430) restrict readonly buffer DirectionalVisibilityBuffer {
	uint data[];
}
directional_visibility;

layout(set = 0, binding = 18, std430) restrict writeonly buffer DirectionalVisibilityMirrorBuffer {
	uint data[];
}
directional_visibility_mirror;

layout(set = 0, binding = 19, std430) restrict writeonly buffer SkyOutRBuffer {
	vec4 data[];
}
sky_out_r;

layout(set = 0, binding = 20, std430) restrict writeonly buffer SkyOutGBuffer {
	vec4 data[];
}
sky_out_g;

layout(set = 0, binding = 21, std430) restrict writeonly buffer SkyOutBBuffer {
	vec4 data[];
}
sky_out_b;

const float C0 = 0.2820947918;
const float C1 = 0.4886025119;

vec4 project_non_negative(vec4 value) {
	float dc = C0 * value.x;
	float amplitude = C1 * length(value.yzw);
	if (amplitude <= dc) {
		return value;
	}
	if (dc <= 0.0) {
		return vec4(0.0);
	}
	return vec4(value.x, value.yzw * (dc / amplitude));
}

void main() {
	uint probe = gl_GlobalInvocationID.x;
	if (probe >= uint(push_constant.probe_count)) {
		return;
	}
	vec4 projected_r = vec4(0.0);
	vec4 projected_g = vec4(0.0);
	vec4 projected_b = vec4(0.0);
	uint packed = directional_visibility.data[probe];
	directional_visibility_mirror.data[probe] = packed;
	if (push_constant.project_radiance == 0) {
		return;
	}
	while (packed != 0u) {
		int link = findLSB(packed);
		projected_r += params.sky_link_radiance[0][link];
		projected_g += params.sky_link_radiance[1][link];
		projected_b += params.sky_link_radiance[2][link];
		packed &= packed - 1u;
	}
	projected_r = project_non_negative(projected_r);
	projected_g = project_non_negative(projected_g);
	projected_b = project_non_negative(projected_b);
	sky_out_r.data[probe] = projected_r;
	sky_out_g.data[probe] = projected_g;
	sky_out_b.data[probe] = projected_b;
}
