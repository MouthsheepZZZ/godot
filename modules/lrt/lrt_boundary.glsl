#[compute]

#version 450

// LRT-owned six-face HDDAGI adapter: native E/PI, with per-connection coverage.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct ProbeCascadeData {
	vec3 position;
	float to_probe;
	ivec3 region_world_offset;
	float to_cell;
	uvec3 pad;
	float exposure_normalization;
	uvec4 pad2;
};

layout(set = 0, binding = 0, std140) uniform HDDAGIData {
	ivec3 grid_size;
	uint max_cascades;
	float normal_bias;
	float energy;
	float y_mult;
	float reflection_bias;
	ivec3 probe_axis_size;
	float esm_strength;
	uvec4 pad3;
	ProbeCascadeData cascades[8];
} hddagi;

layout(set = 0, binding = 1) uniform texture2DArray lightprobe_diffuse;
layout(set = 0, binding = 2) uniform texture3D hddagi_occlusion[2];
layout(set = 0, binding = 3) uniform sampler linear_sampler;

layout(set = 0, binding = 4, std430) restrict writeonly buffer Boundary { vec4 data[]; } boundary;
layout(set = 0, binding = 8, std140) uniform ExposureNormalization { vec4 inverse_exposure[8]; } exposure;

%LRT_BOUNDARY%

layout(push_constant, std430) uniform Params {
	mat4 volume_to_world;
	ivec4 grid_size;
	vec4 grid_min_spacing;
	vec4 camera_origin;
} params;

const int PROBE_CELLS = 8;
const int LIGHTPROBE_OCT_SIZE = 6;

vec2 octahedron_wrap(vec2 value) {
	vec2 sign_value = mix(vec2(-1.0), vec2(1.0), greaterThanEqual(value, vec2(0.0)));
	return (1.0 - abs(value.yx)) * sign_value;
}

vec2 octahedron_encode(vec3 normal) {
	normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
	normal.xy = normal.z >= 0.0 ? normal.xy : octahedron_wrap(normal.xy);
	return normal.xy * 0.5 + 0.5;
}

ivec3 positive_mod(ivec3 value, ivec3 divisor) {
	return mix(value % divisor, divisor - ((abs(value) - ivec3(1)) % divisor) - 1,
			lessThan(sign(value), ivec3(0)));
}

ivec2 probe_to_tex(ivec3 local_probe, int cascade) {
	ivec3 cell = positive_mod(hddagi.cascades[cascade].region_world_offset + local_probe,
			hddagi.probe_axis_size);
	return cell.xy + ivec2(0, cell.z * hddagi.probe_axis_size.y);
}

vec3 sample_cascade(int cascade, vec3 cascade_position, vec3 normal) {
	vec3 position = cascade_position + normal;
	ivec3 position_i = ivec3(position);
	ivec3 base_probe = position_i / PROBE_CELLS;

	ivec3 occ_position = position_i;
	vec3 position_fraction = position - vec3(position_i);
	occ_position = (occ_position + hddagi.cascades[cascade].region_world_offset * PROBE_CELLS) &
			(hddagi.grid_size - 1);
	occ_position.y += (hddagi.grid_size.y + 2) * cascade;
	occ_position += ivec3(1);
	ivec3 occ_size = hddagi.grid_size + ivec3(2);
	occ_size.y *= int(hddagi.max_cascades);
	vec3 occ_uv = (vec3(occ_position) + position_fraction) / vec3(occ_size);
	vec4 occ_0 = texture(sampler3D(hddagi_occlusion[0], linear_sampler), occ_uv);
	vec4 occ_1 = texture(sampler3D(hddagi_occlusion[1], linear_sampler), occ_uv);
	float occ_weights[8] = float[](occ_0.x, occ_0.y, occ_0.z, occ_0.w,
			occ_1.x, occ_1.y, occ_1.z, occ_1.w);

	vec2 texel_size = 1.0 / vec2((LIGHTPROBE_OCT_SIZE + 2) * hddagi.probe_axis_size.x,
			(LIGHTPROBE_OCT_SIZE + 2) * hddagi.probe_axis_size.y * hddagi.probe_axis_size.z);
	vec2 direction_uv = octahedron_encode(normal) * float(LIGHTPROBE_OCT_SIZE);
	vec3 light = vec3(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 8; i++) {
		ivec3 probe = base_probe + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1));
		vec3 probe_to_position = position - vec3(probe * PROBE_CELLS);
		ivec3 probe_occ = (hddagi.cascades[cascade].region_world_offset + probe) & ivec3(1);
		uint weight_index = uint(probe_occ.x) | (uint(probe_occ.y) << 1u) | (uint(probe_occ.z) << 2u);
		vec3 trilinear = vec3(1.0) - abs(probe_to_position / float(PROBE_CELLS));
		float weight = max(0.2, occ_weights[weight_index]) * trilinear.x * trilinear.y * trilinear.z;
		ivec2 tex_position = probe_to_tex(probe, cascade);
		vec2 base_uv = vec2(tex_position * (LIGHTPROBE_OCT_SIZE + 2) + ivec2(1));
		vec2 uv = (base_uv + direction_uv) * texel_size;
		light += texture(sampler2DArray(lightprobe_diffuse, linear_sampler), vec3(uv, float(cascade))).rgb * weight;
		weight_sum += weight;
	}
	if (weight_sum <= 0.0) {
		return vec3(0.0);
	}
	return light * exposure.inverse_exposure[cascade].x / weight_sum;
}

vec3 sample_hddagi(vec3 camera_position, vec3 normal, out bool valid) {
	valid = false;
	camera_position.y *= hddagi.y_mult;
	normal.y *= hddagi.y_mult;
	normal = normalize(normal);
	int cascade = -1;
	vec3 cascade_position = vec3(0.0);
	for (int i = 0; i < int(hddagi.max_cascades); i++) {
		cascade_position = (camera_position - hddagi.cascades[i].position) * hddagi.cascades[i].to_cell;
		if (all(greaterThanEqual(cascade_position, vec3(0.0))) &&
				all(lessThan(cascade_position, vec3(hddagi.grid_size)))) {
			cascade = i;
			break;
		}
	}
	if (cascade < 0) {
		return vec3(0.0);
	}
	valid = true;
	vec3 diffuse = sample_cascade(cascade, cascade_position, normal);
	vec3 blend_from = (vec3(hddagi.probe_axis_size) - 1.0) * 0.5;
	vec3 inner_position = camera_position * hddagi.cascades[cascade].to_probe;
	vec3 inner_distance = blend_from - abs(inner_position);
	float blend = clamp(1.0 - smoothstep(0.5, 2.5,
			min(inner_distance.x, min(inner_distance.y, inner_distance.z))), 0.0, 1.0);
	if (blend > 0.0) {
		if (cascade == int(hddagi.max_cascades) - 1) {
			diffuse *= 1.0 - blend;
		} else {
			vec3 next_position = (camera_position - hddagi.cascades[cascade + 1].position) *
					hddagi.cascades[cascade + 1].to_cell;
			diffuse = mix(diffuse, sample_cascade(cascade + 1, next_position, normal), blend);
		}
	}
	return diffuse * hddagi.energy;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	ivec3 cell;
	ivec3 offset;
	boundary_connection(int(index), params.grid_size.xyz, cell, offset);
	vec3 local_position = params.grid_min_spacing.xyz +
			(vec3(cell + offset) + 0.5) * params.grid_min_spacing.w;
	vec3 world_position = (params.volume_to_world * vec4(local_position, 1.0)).xyz;
	vec3 world_direction = normalize(mat3(params.volume_to_world) * vec3(offset));
	bool covered;
	// Native diffuse already stores the normalized cosine integral E/PI.
	vec3 diffuse = sample_hddagi(world_position - params.camera_origin.xyz, world_direction, covered);
	boundary.data[index] = vec4(diffuse, covered ? 1.0 : 0.0);
}
