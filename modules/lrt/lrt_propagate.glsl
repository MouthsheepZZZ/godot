#[compute]

#version 450

// Local Radiance Transfer adjacency propagation (RGB SH4 + global visibility A/B).
// Ported from the prototype's src/shaders.js "propagation" pass (G:\lrt_external_test @ 524a290).
// Apply visibility to each lattice direction before SH projection. Multiplying by
// truncated SH visibility after gathering can cancel light with negative visibility.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int sampling;
	int iteration;
	int update_sky_visibility;
	int external_gi_enabled;
}
push_constant;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size; // xyz probe counts, w probe count
	vec4 grid_min; // xyz origin, w probe spacing
	ivec4 counts; // z direction count
	vec4 flags; // x multi bounce, y SH visibility, z color SDF, w native receiver lighting
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

layout(set = 0, binding = 12, std430) restrict readonly buffer VisibilityInBuffer {
	vec4 data[];
}
visibility_in;

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

layout(set = 0, binding = 16, std430) restrict writeonly buffer VisibilityOutBuffer {
	vec4 data[];
}
visibility_out;

layout(set = 0, binding = 17, std430) restrict readonly buffer DirectionalVisibilityInBuffer {
	uint data[];
}
directional_visibility_in;

layout(set = 0, binding = 18, std430) restrict writeonly buffer DirectionalVisibilityOutBuffer {
	uint data[];
}
directional_visibility_out;

layout(set = 0, binding = 25, std430) restrict readonly buffer SkyInRBuffer { vec4 data[]; } sky_in_r;
layout(set = 0, binding = 26, std430) restrict readonly buffer SkyInGBuffer { vec4 data[]; } sky_in_g;
layout(set = 0, binding = 27, std430) restrict readonly buffer SkyInBBuffer { vec4 data[]; } sky_in_b;

layout(set = 0, binding = 28, std430) restrict buffer Phase0RBuffer { vec4 data[]; } phase_0_r;
layout(set = 0, binding = 29, std430) restrict buffer Phase0GBuffer { vec4 data[]; } phase_0_g;
layout(set = 0, binding = 30, std430) restrict buffer Phase0BBuffer { vec4 data[]; } phase_0_b;
layout(set = 0, binding = 31, std430) restrict buffer Phase1RBuffer { vec4 data[]; } phase_1_r;
layout(set = 0, binding = 32, std430) restrict buffer Phase1GBuffer { vec4 data[]; } phase_1_g;
layout(set = 0, binding = 33, std430) restrict buffer Phase1BBuffer { vec4 data[]; } phase_1_b;
layout(set = 0, binding = 34, std430) restrict buffer Phase2RBuffer { vec4 data[]; } phase_2_r;
layout(set = 0, binding = 35, std430) restrict buffer Phase2GBuffer { vec4 data[]; } phase_2_g;
layout(set = 0, binding = 36, std430) restrict buffer Phase2BBuffer { vec4 data[]; } phase_2_b;

layout(set = 0, binding = 22, std430) restrict readonly buffer ExternalRBuffer {
	vec4 data[];
} external_r;

layout(set = 0, binding = 23, std430) restrict readonly buffer ExternalGBuffer {
	vec4 data[];
} external_g;

layout(set = 0, binding = 24, std430) restrict readonly buffer ExternalBBuffer {
	vec4 data[];
} external_b;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float W = 4.0 * PI / 26.0;
const float FOUR_POINT_PHASE_WEIGHT = PI / 3.0;

// Substituted from lrt::directions() so the CPU local field and the GPU passes always agree.
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);
// The paper specifies four edge neighbours over a three-frame dither cycle but does not
// publish coordinates. This project design uses the twelve cube-edge directions, grouped by
// their zero axis. A stable per-probe phase offset prevents a whole lattice plane from choosing
// the same tetrahedral projection in one pass.
const ivec4 EDGE_PHASES[3] = ivec4[3](
		ivec4(9, 11, 14, 16),
		ivec4(3, 5, 20, 22),
		ivec4(1, 7, 18, 24));

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

uint spatial_dither(ivec3 p) {
	uvec3 value = uvec3(p) * uvec3(73856093u, 19349663u, 83492791u);
	return (value.x ^ value.y ^ value.z) % 3u;
}

vec4 transfer(vec4 incoming, uint index, int channel) {
	vec4 result;
	int count = params.grid_size.w;
	for (int row = 0; row < 4; row++) {
		result[row] = dot(matrices.data[row * count + int(index)], incoming);
	}
	return result * matrices.data[4 * count + int(index)][channel];
}

vec4 bounded_visibility(vec4 value) {
	value.x = clamp(value.x, 0.0, 1.0 / C0);
	float limit = min(value.x * C0, 1.0 - value.x * C0) / C1;
	float amplitude = length(value.yzw);
	if (amplitude > limit) {
		value.yzw *= limit / amplitude;
	}
	return value;
}

// An SH4 field reconstructs as non-negative in every direction exactly when C0*a0 >= C1*|a.yzw|.
// Keep DC and shrink l=1 onto that boundary: exact for l <= 1, rotation invariant, idempotent,
// and it only modifies probes whose directional field is negative somewhere.
// Original fix for the negative SH transport that cancelled the positive source (N5-R0a).
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

vec4 phase_read(int phase, int channel, uint index) {
	if (phase == 0) {
		return channel == 0 ? phase_0_r.data[index] : (channel == 1 ? phase_0_g.data[index] : phase_0_b.data[index]);
	}
	if (phase == 1) {
		return channel == 0 ? phase_1_r.data[index] : (channel == 1 ? phase_1_g.data[index] : phase_1_b.data[index]);
	}
	return channel == 0 ? phase_2_r.data[index] : (channel == 1 ? phase_2_g.data[index] : phase_2_b.data[index]);
}

void phase_write(int phase, uint index, vec4 red, vec4 green, vec4 blue) {
	if (phase == 0) {
		phase_0_r.data[index] = red;
		phase_0_g.data[index] = green;
		phase_0_b.data[index] = blue;
	} else if (phase == 1) {
		phase_1_r.data[index] = red;
		phase_1_g.data[index] = green;
		phase_1_b.data[index] = blue;
	} else {
		phase_2_r.data[index] = red;
		phase_2_g.data[index] = green;
		phase_2_b.data[index] = blue;
	}
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	radiance_out_r.data[index] = vec4(0.0);
	radiance_out_g.data[index] = vec4(0.0);
	radiance_out_b.data[index] = vec4(0.0);
	if (params.counts.w != 0) {
		visibility_out.data[index] = vec4(0.0);
	}
	if (material.data[index].a > 0.5) {
		if (push_constant.sampling != 0) {
			phase_write(0, index, vec4(0.0), vec4(0.0), vec4(0.0));
			phase_write(1, index, vec4(0.0), vec4(0.0), vec4(0.0));
			phase_write(2, index, vec4(0.0), vec4(0.0), vec4(0.0));
		}
		if (push_constant.update_sky_visibility != 0) {
			directional_visibility_out.data[index] = 0u;
		}
		return;
	}
	ivec3 p = decode_coord(index);
	uint mask = links.data[index];
	vec4 incoming_r = vec4(0.0);
	vec4 incoming_g = vec4(0.0);
	vec4 incoming_b = vec4(0.0);
	vec4 incoming_v = vec4(0.0);
	bool update_global_visibility = params.counts.w != 0 &&
			(push_constant.sampling == 0 || (push_constant.iteration % 3) == 2);
	int sample_count = push_constant.sampling == 0 ? 26 : 4;
	int edge_phase = int((uint(push_constant.iteration % 3) + spatial_dither(p)) % 3u);
	float gather_weight = push_constant.sampling == 0 ? W : FOUR_POINT_PHASE_WEIGHT;
	for (int sample_index = 0; sample_index < sample_count; sample_index++) {
		int j = push_constant.sampling == 0 ? sample_index : EDGE_PHASES[edge_phase][sample_index];
		bool link_open = (mask & (1u << uint(j))) != 0u;
		if (!link_open) {
			continue;
		}
		ivec3 q = p + OFFSETS[j];
		vec3 direction = normalize(vec3(OFFSETS[j]));
		vec4 b = Y(direction);
		// SH transport uses the orthonormal basis; the other mode retains its prefilter.
		vec4 projected = params.flags.y > 0.5 ? b : P(direction);
		if (outside(q)) {
			if (push_constant.external_gi_enabled != 0) {
				incoming_r += gather_weight * projected * max(dot(external_r.data[index], b), 0.0);
				incoming_g += gather_weight * projected * max(dot(external_g.data[index], b), 0.0);
				incoming_b += gather_weight * projected * max(dot(external_b.data[index], b), 0.0);
			}
			if (update_global_visibility && push_constant.sampling == 0) {
				incoming_v += gather_weight * b;
			}
		} else {
			uint qi = probe_index(q);
			incoming_r += gather_weight * projected * dot(radiance_in_r.data[qi], b);
			incoming_g += gather_weight * projected * dot(radiance_in_g.data[qi], b);
			incoming_b += gather_weight * projected * dot(radiance_in_b.data[qi], b);
			if (update_global_visibility && push_constant.sampling == 0) {
				incoming_v += gather_weight * b * dot(visibility_in.data[qi], b);
			}
		}
	}
	if (push_constant.sampling != 0) {
		phase_write(edge_phase, index, incoming_r, incoming_g, incoming_b);
		for (int phase = 0; phase < 3; phase++) {
			if (phase == edge_phase) {
				continue;
			}
			incoming_r += phase_read(phase, 0, index);
			incoming_g += phase_read(phase, 1, index);
			incoming_b += phase_read(phase, 2, index);
		}
		// Global Visibility is a geometry-only field with no published four-point closure. Run its
		// accepted Full26 operator once per complete dither cycle and carry it between phases.
		for (int j = 0; update_global_visibility && j < 26; j++) {
			bool link_open = (mask & (1u << uint(j))) != 0u;
			if (!link_open) {
				continue;
			}
			ivec3 q = p + OFFSETS[j];
			vec3 direction = normalize(vec3(OFFSETS[j]));
			vec4 b = Y(direction);
			if (outside(q)) {
				incoming_v += W * b;
			} else {
				uint qi = probe_index(q);
				incoming_v += W * b * dot(visibility_in.data[qi], b);
			}
		}
	}
	if (params.counts.w != 0) {
		visibility_out.data[index] = update_global_visibility ? bounded_visibility(incoming_v) : visibility_in.data[index];
	}
	if (push_constant.update_sky_visibility != 0) {
		uint packed_visibility = 0u;
		for (int link = 0; link < 26; link++) {
			uint bit = 1u << uint(link);
			if ((mask & bit) == 0u) {
				continue;
			}
			ivec3 source = p + OFFSETS[link];
			if (outside(source)) {
				packed_visibility |= bit;
			} else {
				packed_visibility |= directional_visibility_in.data[probe_index(source)] & bit;
			}
		}
		directional_visibility_out.data[index] = packed_visibility;
	}
	// The colored sky field was masked by exact open links before entering this probe. Only its
	// reflected term enters radiance history; direct sky remains separate in the base pass.
	vec4 reflected_r = transfer(sky_in_r.data[index] + (params.flags.x > 0.5 ? incoming_r : vec4(0.0)), index, 0);
	vec4 reflected_g = transfer(sky_in_g.data[index] + (params.flags.x > 0.5 ? incoming_g : vec4(0.0)), index, 1);
	vec4 reflected_b = transfer(sky_in_b.data[index] + (params.flags.x > 0.5 ? incoming_b : vec4(0.0)), index, 2);
	vec4 out_r = incoming_r + reflected_r + source_r.data[index];
	vec4 out_g = incoming_g + reflected_g + source_g.data[index];
	vec4 out_b = incoming_b + reflected_b + source_b.data[index];
	// Only the SH transport gathers with the signed Y basis, so only it needs the fix. The
	// binary-mask path keeps its accepted behaviour until it is measured on its own.
	if (params.flags.y > 0.5) {
		out_r = project_non_negative(out_r);
		out_g = project_non_negative(out_g);
		out_b = project_non_negative(out_b);
	}
	radiance_out_r.data[index] = out_r;
	radiance_out_g.data[index] = out_g;
	radiance_out_b.data[index] = out_b;
}
