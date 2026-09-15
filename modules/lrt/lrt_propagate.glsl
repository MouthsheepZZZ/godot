#[compute]

#version 450

// Local Radiance Transfer adjacency propagation (RGB SH4 + global visibility A/B).
// Ported from the prototype's src/shaders.js "propagation" pass (G:\lrt_external_test @ 524a290).
// Only the data layout changed: the prototype's 2D atlas textures became storage buffers.
// PDF pp.30-31 specifies the SH product, not this complete operator composition; the
// prototype's directional gather, local visibility triple product and H + T*H + Q
// combination are kept as the accepted baseline (see the migration plan's PDF table).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int sampling;
	int iteration;
	int update_sky_visibility;
	int sky_word_start;
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

layout(set = 0, binding = 4, std430) restrict readonly buffer LocalVisibilityBuffer {
	vec4 data[];
}
local_visibility;

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

layout(set = 0, binding = 25, std430) restrict readonly buffer SkyInRBuffer {
	vec4 data[];
}
sky_in_r;

layout(set = 0, binding = 26, std430) restrict readonly buffer SkyInGBuffer {
	vec4 data[];
}
sky_in_g;

layout(set = 0, binding = 27, std430) restrict readonly buffer SkyInBBuffer {
	vec4 data[];
}
sky_in_b;

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
const int SKY_DIRECTION_COUNT = %LRT_SKY_DIRECTION_COUNT%;
const int SKY_DIRECTION_WORDS = %LRT_SKY_DIRECTION_WORDS%;

// Substituted from lrt::directions() so the CPU local field and the GPU passes always agree.
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);
// Each digital path is composed only from existing 26-neighbor links, so higher angular
// resolution adds no geometry Trace.
const ivec4 SKY_PATH_A[SKY_DIRECTION_COUNT] = ivec4[SKY_DIRECTION_COUNT](%LRT_SKY_PATH_A%);
const ivec4 SKY_PATH_B[SKY_DIRECTION_COUNT] = ivec4[SKY_DIRECTION_COUNT](%LRT_SKY_PATH_B%);

vec4 Y(vec3 d) {
	return vec4(C0, C1 * d);
}

vec4 P(vec3 d) {
	return vec4(C0, (C1 / 3.0) * d);
}

// src/core.js shTripleProduct / src/shaders.js shTripleProduct.
vec4 sh_triple_product(vec4 a, vec4 b) {
	return C0 * vec4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
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

int sky_path_link(int direction_index, int step) {
	return step < 4 ? SKY_PATH_A[direction_index][step] : SKY_PATH_B[direction_index][step - 4];
}

float propagate_sky_visibility(ivec3 p, int direction_index) {
	ivec3 cursor = p;
	for (int step = 0; step < 8; step++) {
		int link_index = sky_path_link(direction_index, step);
		uint cursor_index = probe_index(cursor);
		if ((links.data[cursor_index] & (1u << uint(link_index))) == 0u) {
			return 0.0;
		}
		cursor += OFFSETS[link_index];
		if (outside(cursor)) {
			return 1.0;
		}
	}
	uint source_index = probe_index(cursor);
	uint packed = directional_visibility_in.data[source_index * SKY_DIRECTION_WORDS + direction_index / 32];
	return float((packed >> uint(direction_index % 32)) & 1u);
}

vec4 transfer(vec4 incoming, uint index, int channel) {
	vec4 result;
	int count = params.grid_size.w;
	for (int row = 0; row < 4; row++) {
		result[row] = dot(matrices.data[(channel * 4 + row) * count + int(index)], incoming);
	}
	return result;
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

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	radiance_out_r.data[index] = vec4(0.0);
	radiance_out_g.data[index] = vec4(0.0);
	radiance_out_b.data[index] = vec4(0.0);
	visibility_out.data[index] = vec4(0.0);
	if (material.data[index].a > 0.5) {
		sky_out_r.data[index] = vec4(0.0);
		sky_out_g.data[index] = vec4(0.0);
		sky_out_b.data[index] = vec4(0.0);
		if (push_constant.update_sky_visibility != 0) {
			for (int word = 0; word < SKY_DIRECTION_WORDS; word++) {
				bool update_word = word >= push_constant.sky_word_start &&
						word < push_constant.sky_word_start + SKY_DIRECTION_WORDS / 2;
				directional_visibility_out.data[index * SKY_DIRECTION_WORDS + word] = update_word ? 0u :
						directional_visibility_in.data[index * SKY_DIRECTION_WORDS + word];
			}
		}
		return;
	}
	ivec3 p = decode_coord(index);
	uint mask = links.data[index];
	vec4 incoming_r = vec4(0.0);
	vec4 incoming_g = vec4(0.0);
	vec4 incoming_b = vec4(0.0);
	vec4 incoming_v = vec4(0.0);
	int sample_count = push_constant.sampling == 0 ? 26 : 4;
	uint dither = index * 747796405u + uint(push_constant.iteration % 3) * 2891336453u;
	float gather_weight = push_constant.sampling == 0 ? W : W * 6.5;
	for (int sample_index = 0; sample_index < sample_count; sample_index++) {
		// The paper does not publish its four coordinates or dither. This experimental path uses
		// four distinct, spatially dithered strata and preserves the full-26 estimator's weight.
		int j = push_constant.sampling == 0 ? sample_index : int((dither + uint(sample_index * 7)) % 26u);
		bool link_open = (mask & (1u << uint(j))) != 0u;
		if (params.flags.y < 0.5 && !link_open) {
			continue;
		}
		ivec3 q = p + OFFSETS[j];
		vec3 direction = normalize(vec3(OFFSETS[j]));
		vec4 b = Y(direction);
		// SH multiplication requires orthonormal coefficients, without prefiltering.
		vec4 projected = params.flags.y > 0.5 ? b : P(direction);
		if (outside(q)) {
			incoming_r += gather_weight * projected * max(dot(external_r.data[index], b), 0.0);
			incoming_g += gather_weight * projected * max(dot(external_g.data[index], b), 0.0);
			incoming_b += gather_weight * projected * max(dot(external_b.data[index], b), 0.0);
			incoming_v += gather_weight * b;
		} else {
			uint qi = probe_index(q);
			incoming_r += gather_weight * projected * dot(radiance_in_r.data[qi], b);
			incoming_g += gather_weight * projected * dot(radiance_in_g.data[qi], b);
			incoming_b += gather_weight * projected * dot(radiance_in_b.data[qi], b);
			incoming_v += gather_weight * b * dot(visibility_in.data[qi], b);
		}
	}
	if (params.flags.y > 0.5) {
		vec4 local_v = local_visibility.data[index];
		incoming_r = sh_triple_product(incoming_r, local_v);
		incoming_g = sh_triple_product(incoming_g, local_v);
		incoming_b = sh_triple_product(incoming_b, local_v);
		incoming_v = sh_triple_product(incoming_v, local_v);
	}
	vec4 out_v = bounded_visibility(incoming_v);
	visibility_out.data[index] = out_v;
	if (push_constant.update_sky_visibility != 0) {
		for (int word = 0; word < SKY_DIRECTION_WORDS; word++) {
			bool update_word = word >= push_constant.sky_word_start &&
					word < push_constant.sky_word_start + SKY_DIRECTION_WORDS / 2;
			uint packed_visibility = update_word ? 0u : directional_visibility_in.data[index * SKY_DIRECTION_WORDS + word];
			if (!update_word) {
				directional_visibility_out.data[index * SKY_DIRECTION_WORDS + word] = packed_visibility;
				continue;
			}
			for (int component = 0; component < 32; component++) {
				int direction_index = word * 32 + component;
				if (direction_index >= SKY_DIRECTION_COUNT) {
					break;
				}
				if (propagate_sky_visibility(p, direction_index) > 0.5) {
					packed_visibility |= 1u << uint(component);
				}
			}
			directional_visibility_out.data[index * SKY_DIRECTION_WORDS + word] = packed_visibility;
		}
	}
	vec4 transported_sky_r = sky_in_r.data[index];
	vec4 transported_sky_g = sky_in_g.data[index];
	vec4 transported_sky_b = sky_in_b.data[index];
	sky_out_r.data[index] = transported_sky_r;
	sky_out_g.data[index] = transported_sky_g;
	sky_out_b.data[index] = transported_sky_b;

	// The colored sky field was masked by exact open links before entering this probe. Only its
	// reflected term enters radiance history; direct sky remains separate in the base pass.
	vec4 reflected_r = transfer(transported_sky_r, index, 0);
	vec4 reflected_g = transfer(transported_sky_g, index, 1);
	vec4 reflected_b = transfer(transported_sky_b, index, 2);
	if (params.flags.x > 0.5) {
		reflected_r += transfer(incoming_r, index, 0);
		reflected_g += transfer(incoming_g, index, 1);
		reflected_b += transfer(incoming_b, index, 2);
	}
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
