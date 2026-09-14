#[compute]

#version 450

// LRT first-bounce injection. Direct light is exclusively the coherent native Forward+
// receiver capture from R7; runtime analytic lights and BVH shadow traces no longer exist.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
} params;

layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer {
	vec4 data[];
}
material;

layout(set = 0, binding = 5, std430) restrict readonly buffer ReceiverBuffer {
	vec4 data[];
}
receivers;

layout(set = 0, binding = 6, std430) restrict buffer SourceRBuffer {
	vec4 data[];
}
source_r;

layout(set = 0, binding = 7, std430) restrict buffer SourceGBuffer {
	vec4 data[];
}
source_g;

layout(set = 0, binding = 8, std430) restrict buffer SourceBBuffer {
	vec4 data[];
}
source_b;

layout(set = 0, binding = 20, std430) restrict readonly buffer ReceiverEmissionBuffer {
	vec4 data[];
}
receiver_emission;

// RGB contains the native diffuse response at each receiver, including N.L / PI, light
// attenuation, projector/area response and raster shadowing.
layout(set = 0, binding = 21, std430) restrict readonly buffer ReceiverLightingBuffer {
	vec4 data[];
}
receiver_lighting;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float W = 4.0 * PI / 26.0;

const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

vec4 P(vec3 direction) {
	return vec4(C0, (C1 / 3.0) * direction);
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	source_r.data[index] = vec4(0.0);
	source_g.data[index] = vec4(0.0);
	source_b.data[index] = vec4(0.0);
	if (material.data[index].a > 0.5) {
		return;
	}
	vec4 header = material.data[index];
	for (int receiver_index = 0; receiver_index < int(header.g); receiver_index++) {
		int base = int(header.r) + receiver_index * 3;
		vec4 receiver_data = receivers.data[base];
		vec3 albedo = receivers.data[base + 2].rgb;
		vec3 direction = normalize(vec3(OFFSETS[int(receiver_data.w)]));
		vec4 projected = W * P(direction);
		vec3 emission = receiver_emission.data[base / 3].rgb;
		vec3 reflected = params.flags.w > 0.5 ? albedo * receiver_lighting.data[base / 3].rgb : vec3(0.0);
		vec3 source = emission + reflected;
		source_r.data[index] += projected * source.r;
		source_g.data[index] += projected * source.g;
		source_b.data[index] += projected * source.b;
	}
}
