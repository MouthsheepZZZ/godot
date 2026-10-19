#[compute]

#version 450

// Fixed six-face RGB SH4 transport. Local OpenMask is the only link gate.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int reserved;
	int reserved2;
	int pad;
	int external_gi_enabled;
}
push_constant;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size; // xyz probe counts, w probe count
	vec4 grid_min; // xyz origin, w probe spacing
	ivec4 counts; // z direction count
	vec4 flags; // reserved
} params;

layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer {
	vec4 data[];
}
material;

layout(set = 0, binding = 2, std430) restrict readonly buffer LinksBuffer {
	uint data[];
}
links;

layout(set = 0, binding = 3, std430) restrict readonly buffer MatrixBuffer {
	vec4 data[];
}
matrices;

layout(set = 0, binding = 4, std430) restrict readonly buffer RecordIndexBuffer {
	uint data[];
}
record_indices;

layout(set = 0, binding = 6, std430) restrict readonly buffer SourceRBuffer {
	vec4 data[];
}
source_r;

layout(set = 0, binding = 7, std430) restrict readonly buffer SourceGBuffer {
	vec4 data[];
}
source_g;

layout(set = 0, binding = 8, std430) restrict readonly buffer SourceBBuffer {
	vec4 data[];
}
source_b;

layout(set = 0, binding = 9, std430) restrict readonly buffer RadianceInRBuffer {
	vec4 data[];
}
radiance_in_r;

layout(set = 0, binding = 10, std430) restrict readonly buffer RadianceInGBuffer {
	vec4 data[];
}
radiance_in_g;

layout(set = 0, binding = 11, std430) restrict readonly buffer RadianceInBBuffer {
	vec4 data[];
}
radiance_in_b;

layout(set = 0, binding = 13, std430) restrict writeonly buffer RadianceOutRBuffer {
	vec4 data[];
}
radiance_out_r;

layout(set = 0, binding = 14, std430) restrict writeonly buffer RadianceOutGBuffer {
	vec4 data[];
}
radiance_out_g;

layout(set = 0, binding = 15, std430) restrict writeonly buffer RadianceOutBBuffer {
	vec4 data[];
}
radiance_out_b;

layout(set = 0, binding = 22, std430) restrict readonly buffer Boundary { vec4 data[]; } boundary;

%LRT_BOUNDARY%

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float FACE_WEIGHT = 4.0 * PI / 6.0;

// Substituted from lrt::directions() so the CPU local field and the GPU passes always agree.
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);
// Generated from the same six directions used to calibrate LocalRecord k.
const int FACE_DIRECTIONS[6] = int[6](%LRT_FACE_DIRECTIONS%);

vec4 Y(vec3 d) {
	return vec4(C0, C1 * d);
}

vec4 P(vec3 d) {
	return vec4(C0, (C1 / 3.0) * d);
}

ivec3 decode_coord(uint p_index) {
	int width = params.grid_size.x * params.grid_size.z;
	int index = int(p_index);
	int y = index / width;
	int rest = index % width;
	return ivec3(rest % params.grid_size.x, y, rest / params.grid_size.x);
}

uint probe_index(ivec3 p) {
	int width = params.grid_size.x * params.grid_size.z;
	return uint(p.x + p.z * params.grid_size.x + p.y * width);
}

bool outside(ivec3 p) {
	return any(lessThan(p, ivec3(0))) || any(greaterThanEqual(p, params.grid_size.xyz));
}

vec4 transfer(vec4 incoming, uint index, int channel) {
	uint slot = record_indices.data[index];
	if (slot == 0xffffffffu) {
		return vec4(0.0);
	}
	vec4 result;
	for (int row = 0; row < 4; row++) {
		result[row] = dot(matrices.data[slot * 8u + uint(row)], incoming);
	}
	uint packed_rho = floatBitsToUint(matrices.data[slot * 8u + 4u].x);
	return result * unpackUnorm4x8(packed_rho)[channel];
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	radiance_out_r.data[index] = vec4(0.0);
	radiance_out_g.data[index] = vec4(0.0);
	radiance_out_b.data[index] = vec4(0.0);
	if (material.data[index].a > 0.5) {
		return;
	}
	ivec3 p = decode_coord(index);
	uint mask = links.data[index];
	vec4 incoming_r = vec4(0.0);
	vec4 incoming_g = vec4(0.0);
	vec4 incoming_b = vec4(0.0);
	for (int sample_index = 0; sample_index < 6; sample_index++) {
		int j = FACE_DIRECTIONS[sample_index];
		bool link_open = (mask & (1u << uint(j))) != 0u;
		if (!link_open) {
			continue;
		}
		ivec3 q = p + OFFSETS[j];
		vec3 direction = vec3(OFFSETS[j]);
		vec4 b = Y(direction);
		vec4 projected = P(direction);
		if (outside(q)) {
			if (push_constant.external_gi_enabled != 0) {
				vec3 radiance = boundary.data[boundary_index(p, OFFSETS[j], params.grid_size.xyz)].rgb;
				incoming_r += FACE_WEIGHT * projected * radiance.r;
				incoming_g += FACE_WEIGHT * projected * radiance.g;
				incoming_b += FACE_WEIGHT * projected * radiance.b;
			}
		} else {
			uint qi = probe_index(q);
			incoming_r += FACE_WEIGHT * projected * dot(radiance_in_r.data[qi], b);
			incoming_g += FACE_WEIGHT * projected * dot(radiance_in_g.data[qi], b);
			incoming_b += FACE_WEIGHT * projected * dot(radiance_in_b.data[qi], b);
		}
	}
	uint slot = record_indices.data[index];
	float k = slot == 0xffffffffu ? 1.0 : matrices.data[slot * 8u + 4u].y;
	vec4 out_r = k * (incoming_r + transfer(incoming_r, index, 0));
	vec4 out_g = k * (incoming_g + transfer(incoming_g, index, 1));
	vec4 out_b = k * (incoming_b + transfer(incoming_b, index, 2));
	if (slot != 0xffffffffu) {
		out_r += source_r.data[slot];
		out_g += source_g.data[slot];
		out_b += source_b.data[slot];
	}
	radiance_out_r.data[index] = out_r;
	radiance_out_g.data[index] = out_g;
	radiance_out_b.data[index] = out_b;
}
