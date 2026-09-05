#[compute]

#version 450

#VERSION_DEFINES

#define HDDAGI_MAX_CASCADES 8

const float SH_PI = 3.14159265358979323846;
const float SH_Y00 = 0.28209479177387814;
const float SH_Y1 = 0.4886025119029199;

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer DynamicGIBoundary {
	vec4 values[];
}
dynamic_gi_boundary;

layout(set = 0, binding = 1) uniform texture2DArray hddagi_lightprobe_diffuse;
layout(set = 0, binding = 2) uniform texture3D hddagi_occlusion[2];
layout(set = 0, binding = 3) uniform sampler linear_sampler;

struct ProbeCascadeData {
	vec3 position;
	float to_probe;

	ivec3 region_world_offset;
	float to_cell;

	vec3 pad;
	float exposure_normalization;

	uvec4 pad2;
};

layout(set = 0, binding = 4, std140) uniform HDDAGI {
	ivec3 grid_size;
	uint max_cascades;

	float normal_bias;
	float energy;
	float y_mult;
	float reflection_bias;

	ivec3 probe_axis_size;
	float esm_strength;

	uvec4 pad3;

	ProbeCascadeData cascades[HDDAGI_MAX_CASCADES];
}
hddagi;

layout(push_constant, std430) uniform Params {
	ivec3 resolution;
	int probe_count;

	vec3 size;
	int lightprobe_oct_size;

	vec4 xform_x;
	vec4 xform_y;
	vec4 xform_z;
	vec4 xform_origin;

	vec3 camera_origin;
	float pad;
}
params;

ivec3 probe_position(int index) {
	int plane = params.resolution.x * params.resolution.y;
	int z = index / plane;
	int plane_index = index - z * plane;
	int y = plane_index / params.resolution.x;
	int x = plane_index - y * params.resolution.x;
	return ivec3(x, y, z);
}

vec3 boundary_outward_normal(ivec3 position) {
	vec3 normal = vec3(0.0);
	if (position.x == 0) {
		normal.x -= 1.0;
	} else if (position.x == params.resolution.x - 1) {
		normal.x += 1.0;
	}
	if (position.y == 0) {
		normal.y -= 1.0;
	} else if (position.y == params.resolution.y - 1) {
		normal.y += 1.0;
	}
	if (position.z == 0) {
		normal.z -= 1.0;
	} else if (position.z == params.resolution.z - 1) {
		normal.z += 1.0;
	}
	return length(normal) > 0.0 ? normalize(normal) : vec3(0.0);
}

vec3 xform_point(vec3 local_position) {
	return params.xform_x.xyz * local_position.x + params.xform_y.xyz * local_position.y + params.xform_z.xyz * local_position.z + params.xform_origin.xyz;
}

vec2 octahedron_wrap(vec2 value) {
	vec2 sign_value;
	sign_value.x = value.x >= 0.0 ? 1.0 : -1.0;
	sign_value.y = value.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(value.yx)) * sign_value;
}

vec2 octahedron_encode(vec3 normal) {
	normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
	normal.xy = normal.z >= 0.0 ? normal.xy : octahedron_wrap(normal.xy);
	return normal.xy * 0.5 + 0.5;
}

ivec3 positive_mod(ivec3 value, ivec3 divisor) {
	return mix(value % divisor, divisor - ((abs(value) - ivec3(1)) % divisor) - 1, lessThan(sign(value), ivec3(0)));
}

ivec2 probe_to_tex(ivec3 local_probe, int cascade) {
	ivec3 cell = positive_mod(hddagi.cascades[cascade].region_world_offset + local_probe, hddagi.probe_axis_size);
	return cell.xy + ivec2(0, cell.z * hddagi.probe_axis_size.y);
}

vec3 sample_cascade_diffuse(int cascade, vec3 cascade_position, vec3 sample_normal) {
	vec3 biased_position = cascade_position + sample_normal;
	ivec3 cell_position = ivec3(biased_position);
	ivec3 probe_cell_size = hddagi.grid_size / (hddagi.probe_axis_size - ivec3(1));
	ivec3 base_probe = cell_position / probe_cell_size;

	ivec3 occlusion_position = cell_position;
	vec3 position_fraction = biased_position - vec3(cell_position);
	occlusion_position = (occlusion_position + hddagi.cascades[cascade].region_world_offset * probe_cell_size) & (hddagi.grid_size - ivec3(1));
	occlusion_position.y += (hddagi.grid_size.y + 2) * cascade;
	occlusion_position += ivec3(1);
	ivec3 occlusion_size = hddagi.grid_size + ivec3(2);
	occlusion_size.y *= int(hddagi.max_cascades);
	vec3 occlusion_uv = (vec3(occlusion_position) + position_fraction) / vec3(occlusion_size);

	vec4 occlusion_0 = texture(sampler3D(hddagi_occlusion[0], linear_sampler), occlusion_uv);
	vec4 occlusion_1 = texture(sampler3D(hddagi_occlusion[1], linear_sampler), occlusion_uv);
	float occlusion_weights[8] = float[](occlusion_0.x, occlusion_0.y, occlusion_0.z, occlusion_0.w, occlusion_1.x, occlusion_1.y, occlusion_1.z, occlusion_1.w);

	vec2 texture_to_uv = 1.0 / vec2(
			(params.lightprobe_oct_size + 2) * hddagi.probe_axis_size.x,
			(params.lightprobe_oct_size + 2) * hddagi.probe_axis_size.y * hddagi.probe_axis_size.z);
	vec2 light_uv = octahedron_encode(sample_normal) * float(params.lightprobe_oct_size);
	vec3 diffuse = vec3(0.0);
	float total_weight = 0.0;

	for (int i = 0; i < 8; i++) {
		ivec3 probe = base_probe + ((ivec3(i) >> ivec3(0, 1, 2)) & ivec3(1));
		vec3 probe_position_cells = vec3(probe * probe_cell_size);
		vec3 probe_to_position = biased_position - probe_position_cells;
		ivec3 probe_occlusion = (hddagi.cascades[cascade].region_world_offset + probe) & ivec3(1);
		uint weight_index = uint(probe_occlusion.x) | (uint(probe_occlusion.y) << 1) | (uint(probe_occlusion.z) << 2);
		vec3 trilinear = vec3(1.0) - abs(probe_to_position / vec3(probe_cell_size));
		float weight = max(0.2, occlusion_weights[weight_index]) * trilinear.x * trilinear.y * trilinear.z;

		ivec2 texture_position = probe_to_tex(probe, cascade);
		vec2 base_uv = vec2(texture_position * (params.lightprobe_oct_size + 2) + ivec2(1));
		vec2 uv = (base_uv + light_uv) * texture_to_uv;
		diffuse += texture(sampler2DArray(hddagi_lightprobe_diffuse, linear_sampler), vec3(uv, float(cascade))).rgb * weight;
		total_weight += weight;
	}

	return total_weight > 0.0 ? diffuse / total_weight : vec3(0.0);
}

vec3 sample_hddagi_diffuse(vec3 camera_relative_position, vec3 sample_normal) {
	int cascade = HDDAGI_MAX_CASCADES;
	vec3 cascade_position = vec3(0.0);
	for (int i = 0; i < int(hddagi.max_cascades); i++) {
		vec3 candidate = (camera_relative_position - hddagi.cascades[i].position) * hddagi.cascades[i].to_cell;
		if (all(greaterThanEqual(candidate, vec3(0.0))) && all(lessThan(candidate, vec3(hddagi.grid_size)))) {
			cascade = i;
			cascade_position = candidate;
			break;
		}
	}
	if (cascade >= int(hddagi.max_cascades)) {
		return vec3(0.0);
	}

	vec3 diffuse = sample_cascade_diffuse(cascade, cascade_position, sample_normal);
	vec3 blend_from = (vec3(hddagi.probe_axis_size) - 1.0) * 0.5;
	vec3 inner_position = camera_relative_position * hddagi.cascades[cascade].to_probe;
	vec3 inner_distance = blend_from - abs(inner_position);
	float blend = clamp(1.0 - smoothstep(0.5, 2.5, min(inner_distance.x, min(inner_distance.y, inner_distance.z))), 0.0, 1.0);
	if (blend > 0.0) {
		if (cascade == int(hddagi.max_cascades) - 1) {
			diffuse *= 1.0 - blend;
		} else {
			vec3 next_position = (camera_relative_position - hddagi.cascades[cascade + 1].position) * hddagi.cascades[cascade + 1].to_cell;
			diffuse = mix(diffuse, sample_cascade_diffuse(cascade + 1, next_position, sample_normal), blend);
		}
	}
	return diffuse * hddagi.energy;
}

void main() {
	int index = int(gl_GlobalInvocationID.x);
	if (index >= params.probe_count) {
		return;
	}

	int output_index = index * 3;
	ivec3 grid_position = probe_position(index);
	vec3 local_outward = boundary_outward_normal(grid_position);
	if (all(equal(local_outward, vec3(0.0)))) {
		dynamic_gi_boundary.values[output_index] = vec4(0.0);
		dynamic_gi_boundary.values[output_index + 1] = vec4(0.0);
		dynamic_gi_boundary.values[output_index + 2] = vec4(0.0);
		return;
	}

	vec3 spacing = params.size / vec3(params.resolution - ivec3(1));
	vec3 local_position = vec3(grid_position) * spacing - params.size * 0.5;
	vec3 world_position = xform_point(local_position);
	mat3 world_basis = mat3(params.xform_x.xyz, params.xform_y.xyz, params.xform_z.xyz);
	vec3 world_outward = normalize(transpose(inverse(world_basis)) * local_outward);
	vec3 camera_relative_position = world_position - params.camera_origin;
	camera_relative_position.y *= hddagi.y_mult;
	world_outward.y *= hddagi.y_mult;
	world_outward = normalize(world_outward);

	vec3 diffuse = sample_hddagi_diffuse(camera_relative_position, world_outward);
	vec4 hemisphere_sh = vec4(2.0 * SH_PI * SH_Y00, SH_PI * SH_Y1 * local_outward);
	dynamic_gi_boundary.values[output_index] = hemisphere_sh * diffuse.r;
	dynamic_gi_boundary.values[output_index + 1] = hemisphere_sh * diffuse.g;
	dynamic_gi_boundary.values[output_index + 2] = hemisphere_sh * diffuse.b;
}
