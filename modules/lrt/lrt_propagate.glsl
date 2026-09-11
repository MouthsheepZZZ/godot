#[compute]

#version 450

// Local Radiance Transfer adjacency propagation (RGB SH4 + global visibility A/B).
// Ported from the prototype's src/shaders.js "propagation" pass (G:\lrt_external_test @ 524a290).
// Only the data layout changed: the prototype's 2D atlas textures became storage buffers.
// PDF pp.30-31 specifies the SH product, not this complete operator composition; the
// prototype's directional gather, local visibility triple product and H + T*H + Q
// combination are kept as the accepted baseline (see the migration plan's PDF table).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size; // xyz probe counts, w probe count
	vec4 grid_min; // xyz origin, w probe spacing
	ivec4 counts; // x light count, y box count, z direction count
	vec4 flags; // x multi bounce, y SH visibility, z color SDF, w unused
	vec4 sky_color; // environment radiance outside the grid
	vec4 light_position[8];
	vec4 light_direction[8];
	vec4 light_color[8];
	vec4 light_data[8]; // intensity, type, 1 / range, attenuation
	vec4 light_spot[8]; // cos spot angle, spot attenuation, casts shadow, unused
	vec4 box_min[16];
	vec4 box_max[16];
	vec4 box_color[16];
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

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float W = 4.0 * PI / 26.0;

// Substituted from lrt::directions() so the CPU local field and the GPU passes always agree.
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

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
		return;
	}
	ivec3 p = decode_coord(index);
	uint mask = links.data[index];
	vec4 incoming_r = vec4(0.0);
	vec4 incoming_g = vec4(0.0);
	vec4 incoming_b = vec4(0.0);
	vec4 incoming_v = vec4(0.0);
	for (int j = 0; j < 26; j++) {
		if (params.flags.y < 0.5 && (mask & (1u << uint(j))) == 0u) {
			continue;
		}
		ivec3 q = p + OFFSETS[j];
		vec4 b = Y(normalize(vec3(OFFSETS[j])));
		// SH multiplication requires orthonormal coefficients, without prefiltering.
		vec4 projected = params.flags.y > 0.5 ? b : P(normalize(vec3(OFFSETS[j])));
		if (outside(q)) {
			incoming_v += W * b;
			continue;
		}
		uint qi = probe_index(q);
		incoming_r += W * projected * dot(radiance_in_r.data[qi], b);
		incoming_g += W * projected * dot(radiance_in_g.data[qi], b);
		incoming_b += W * projected * dot(radiance_in_b.data[qi], b);
		incoming_v += W * b * dot(visibility_in.data[qi], b);
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

	// The prototype multiplies the *unbounded* gathered visibility by the sky radiance, so
	// the sky bounce keeps the raw directional response while the stored field is bounded.
	vec4 reflected_r = transfer(incoming_v * params.sky_color.r, index, 0);
	vec4 reflected_g = transfer(incoming_v * params.sky_color.g, index, 1);
	vec4 reflected_b = transfer(incoming_v * params.sky_color.b, index, 2);
	if (params.flags.x > 0.5) {
		reflected_r += transfer(incoming_r, index, 0);
		reflected_g += transfer(incoming_g, index, 1);
		reflected_b += transfer(incoming_b, index, 2);
	}
	radiance_out_r.data[index] = incoming_r + reflected_r + source_r.data[index];
	radiance_out_g.data[index] = incoming_g + reflected_g + source_g.data[index];
	radiance_out_b.data[index] = incoming_b + reflected_b + source_b.data[index];
}
