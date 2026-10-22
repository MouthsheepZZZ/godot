#[compute]

#version 450

// Full 26-neighbor RGB SH4 transport with independent per-channel local operators.

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

layout(set = 0, binding = 2, std430) restrict readonly buffer LocalVisibilityBuffer {
	vec4 data[];
}
local_visibility;

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

const float C0 = 0.2820947918;

// Substituted from lrt::directions() so the CPU local field and the GPU passes always agree.
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

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

vec3 boundary_radiance(ivec3 p, ivec3 q) {
	vec3 result = vec3(0.0);
	float coverage = 0.0;
	for (int axis = 0; axis < 3; axis++) {
		if (q[axis] >= 0 && q[axis] < params.grid_size[axis]) {
			continue;
		}
		ivec3 offset = ivec3(0);
		offset[axis] = q[axis] < 0 ? -1 : 1;
		vec4 sample_value = boundary.data[boundary_index(p, offset, params.grid_size.xyz)];
		result += sample_value.rgb * sample_value.a;
		coverage += sample_value.a;
	}
	return coverage > 0.0 ? result / coverage : vec3(0.0);
}

vec4 product(vec4 a, vec4 b) {
	return C0 * vec4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
}

vec4 transfer(vec4 incoming, uint index, int channel) {
	uint slot = record_indices.data[index];
	if (slot == 0xffffffffu) {
		return vec4(0.0);
	}
	vec4 result;
	for (int row = 0; row < 4; row++) {
		result[row] = dot(matrices.data[slot * 16u + uint(channel * 4 + row)], incoming);
	}
	return result;
}

uint blocked_connections(uint index) {
	uvec2 halves = uvec2(material.data[index].xz);
	return halves.x | (halves.y << 13u);
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
	uint blocked = blocked_connections(index);
	int open_connections = 26;
	vec4 incoming_r = vec4(0.0);
	vec4 incoming_g = vec4(0.0);
	vec4 incoming_b = vec4(0.0);
	vec4 reflection_incoming_r = vec4(0.0);
	vec4 reflection_incoming_g = vec4(0.0);
	vec4 reflection_incoming_b = vec4(0.0);
	for (int j = 0; j < 26; j++) {
		if ((blocked & (1u << uint(j))) != 0u) {
			open_connections--;
			continue;
		}
		ivec3 q = p + OFFSETS[j];
		if (outside(q)) {
			if (push_constant.external_gi_enabled != 0) {
				vec3 radiance = boundary_radiance(p, q);
				// External irradiance/PI is an isotropic incident-radiance approximation.
				incoming_r.x += radiance.r / C0;
				incoming_g.x += radiance.g / C0;
				incoming_b.x += radiance.b / C0;
				// External samples have no local sender mask.
				reflection_incoming_r.x += radiance.r / C0;
				reflection_incoming_g.x += radiance.g / C0;
				reflection_incoming_b.x += radiance.b / C0;
			}
		} else {
			uint qi = probe_index(q);
			// Either endpoint can close the shared edge; never allow one-way transport.
			if (material.data[qi].a > 0.5 || (blocked_connections(qi) & (1u << uint(25 - j))) != 0u) {
				open_connections--;
				continue;
			}
			vec4 neighbor_r = radiance_in_r.data[qi];
			vec4 neighbor_g = radiance_in_g.data[qi];
			vec4 neighbor_b = radiance_in_b.data[qi];
			incoming_r += neighbor_r;
			incoming_g += neighbor_g;
			incoming_b += neighbor_b;
			// Reference p.20 masks each sender before gathering reflected light.
			vec4 sender_visibility = local_visibility.data[qi] * vec4(1.0, -1.0, -1.0, -1.0);
			reflection_incoming_r += product(neighbor_r, sender_visibility);
			reflection_incoming_g += product(neighbor_g, sender_visibility);
			reflection_incoming_b += product(neighbor_b, sender_visibility);
		}
	}
	// Closed links are not black samples. Open exterior links still carry weight,
	// allowing light to leave the volume even without an external GI input.
	float weight = 1.0 / float(max(open_connections, 1));
	incoming_r *= weight;
	incoming_g *= weight;
	incoming_b *= weight;
	uint slot = record_indices.data[index];
	vec4 visibility = local_visibility.data[index];
	// Reflection consumes the sender-masked incident field, not the receiver's
	// transmitted field: multiplying by receiver visibility here occludes it twice.
	reflection_incoming_r *= weight;
	reflection_incoming_g *= weight;
	reflection_incoming_b *= weight;
	vec4 out_r = product(incoming_r, visibility) + transfer(reflection_incoming_r, index, 0);
	vec4 out_g = product(incoming_g, visibility) + transfer(reflection_incoming_g, index, 1);
	vec4 out_b = product(incoming_b, visibility) + transfer(reflection_incoming_b, index, 2);
	if (slot != 0xffffffffu) {
		out_r += source_r.data[slot];
		out_g += source_g.data[slot];
		out_b += source_b.data[slot];
	}
	// Retain signed directional coefficients; only negative total light is invalid.
	radiance_out_r.data[index] = out_r.x > 0.0 ? out_r : vec4(0.0);
	radiance_out_g.data[index] = out_g.x > 0.0 ? out_g : vec4(0.0);
	radiance_out_b.data[index] = out_b.x > 0.0 ? out_b : vec4(0.0);
}
