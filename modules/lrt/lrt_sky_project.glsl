#[compute]

#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int probe_count;
	int pad0;
	int pad1;
	int pad2;
}
push_constant;

struct ParamsData {
	ivec4 grid_size;
	vec4 grid_min_spacing;
	vec4 flags;
	vec4 sky_samples[%LRT_SKY_DIRECTION_COUNT%];
};

layout(set = 0, binding = 0, std140) uniform ParamsBuffer {
	ParamsData params;
};

layout(set = 0, binding = 17, std430) restrict readonly buffer DirectionalVisibilityBuffer {
	uint data[];
}
directional_visibility;

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
const int SKY_DIRECTION_COUNT = %LRT_SKY_DIRECTION_COUNT%;
const int SKY_DIRECTION_WORDS = %LRT_SKY_DIRECTION_WORDS%;
const vec4 SKY_DIRECTIONS[SKY_DIRECTION_COUNT] = vec4[SKY_DIRECTION_COUNT](%LRT_SKY_DIRECTIONS%);

vec4 P(vec3 direction) {
	return vec4(C0, (C1 / 3.0) * direction);
}

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
	for (int word = 0; word < SKY_DIRECTION_WORDS; word++) {
		uint packed = directional_visibility.data[probe * SKY_DIRECTION_WORDS + word];
		for (int component = 0; component < 32; component++) {
			int direction_index = word * 32 + component;
			if (direction_index >= SKY_DIRECTION_COUNT) {
				break;
			}
			float visibility = float((packed >> uint(component)) & 1u);
			vec4 direction = SKY_DIRECTIONS[direction_index];
			vec4 projected = direction.w * P(direction.xyz) * visibility;
			projected_r += projected * params.sky_samples[direction_index].r;
			projected_g += projected * params.sky_samples[direction_index].g;
			projected_b += projected * params.sky_samples[direction_index].b;
		}
	}
	sky_out_r.data[probe] = project_non_negative(projected_r);
	sky_out_g.data[probe] = project_non_negative(projected_g);
	sky_out_b.data[probe] = project_non_negative(projected_b);
}
