#[compute]

#version 450

#VERSION_DEFINES

#define SH_Y00 0.28209479177387814
#define SH_Y1 0.4886025119029199
#define SH_FOUR_PI 12.566370614359172

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer LocalVisibility {
	vec4 values[];
}
local_visibility;

layout(set = 0, binding = 1, std430) restrict readonly buffer LocalTransfer {
#if defined(LOCAL_TRANSFER_RGB_FP16) || defined(LOCAL_TRANSFER_LUMINANCE_FP32_TINT) || defined(LOCAL_TRANSFER_LUMINANCE_FP16_TINT)
	uint values[];
#else
	vec4 values[];
#endif
}
local_transfer;

layout(set = 0, binding = 2, std430) restrict readonly buffer LocalTransportVisibility {
	vec4 values[];
}
local_transport_visibility;

layout(set = 0, binding = 3, std430) restrict readonly buffer Injection {
	vec4 values[];
}
injection;

layout(set = 0, binding = 4, std430) restrict readonly buffer MeshLight {
	vec4 values[];
}
mesh_light;

layout(set = 0, binding = 5, std430) restrict readonly buffer RadianceInput {
	vec4 values[];
}
radiance_input;

layout(set = 0, binding = 6, std430) restrict writeonly buffer RadianceOutput {
	vec4 values[];
}
radiance_output;

layout(set = 0, binding = 7, std430) restrict readonly buffer InsideSolid {
	uint values[];
}
inside_solid;

layout(set = 0, binding = 8, std430) restrict readonly buffer EnvironmentInjection {
	vec4 values[];
}
environment_injection;

layout(push_constant, std430) uniform Params {
	ivec3 resolution;
	int probe_count;
	vec3 probe_spacing;
	float decay_per_meter;
	int probe_offset;
	int neighbor_pattern;
	int pattern_phase;
}
params;

int probe_index(ivec3 position) {
	return position.x + params.resolution.x * (position.y + params.resolution.y * position.z);
}

bool is_valid_position(ivec3 position) {
	return all(greaterThanEqual(position, ivec3(0))) && all(lessThan(position, params.resolution));
}

float neighbor_weight(ivec3 offset) {
	const float normalization = 6.0 + 12.0 / sqrt(2.0) + 8.0 / sqrt(3.0);
	return (1.0 / length(vec3(offset))) / normalization;
}

vec4 triple_product(vec4 a, vec4 b) {
	return vec4(
			dot(a, b),
			a.x * b.y + b.x * a.y,
			a.x * b.z + b.x * a.z,
			a.x * b.w + b.x * a.w) * SH_Y00;
}

vec4 antipodal(vec4 value) {
	return vec4(value.x, -value.yzw);
}

vec4 sh_basis(vec3 direction) {
	vec3 n = normalize(direction);
	return vec4(SH_Y00, SH_Y1 * n.x, SH_Y1 * n.y, SH_Y1 * n.z);
}

vec4 positive_product(vec4 a, vec4 b) {
	if (all(equal(a, vec4(0.0)))) {
		return vec4(0.0);
	}
	vec4 result = vec4(0.0);
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 offset = ivec3(x, y, z);
				if (all(equal(offset, ivec3(0)))) {
					continue;
				}
				vec3 direction = normalize(vec3(offset));
				vec4 basis = sh_basis(direction);
				float value = max(dot(a, basis), 0.0) * max(dot(b, basis), 0.0);
				result += basis * (value * SH_FOUR_PI * neighbor_weight(offset));
			}
		}
	}
	return result;
}

uint spatial_dither(ivec3 position) {
	uvec3 value = uvec3(position) * uvec3(73856093u, 19349663u, 83492791u);
	return (value.x ^ value.y ^ value.z) % 3u;
}

ivec3 edge_neighbor_offset(int pattern, int first_sign, int second_sign) {
	if (pattern == 0) {
		return ivec3(first_sign, second_sign, 0);
	}
	if (pattern == 1) {
		return ivec3(first_sign, 0, second_sign);
	}
	return ivec3(0, first_sign, second_sign);
}

vec4 gather_neighbor(int channel, ivec3 position, ivec3 offset, float weight) {
	ivec3 neighbor_position = position + offset;
	if (!is_valid_position(neighbor_position)) {
		return vec4(0.0);
	}
	int neighbor_index = probe_index(neighbor_position);
	int neighbor_value = neighbor_index * 3 + channel;
	float distance_decay = pow(params.decay_per_meter, length(vec3(offset) * params.probe_spacing));
	vec4 transport_visibility = antipodal(local_transport_visibility.values[neighbor_index]);
	vec4 visible_radiance = triple_product(radiance_input.values[neighbor_value], transport_visibility);
	vec3 direction = normalize(vec3(offset));
	vec4 basis = sh_basis(direction);
	float directional_radiance = max(dot(visible_radiance, basis), 0.0);
	return basis * (directional_radiance * SH_FOUR_PI * weight * distance_decay);
}

vec4 transform_transfer(int index, int channel, vec4 value) {
#ifdef LOCAL_TRANSFER_RGB_FP16
	int row_offset = index * 24 + channel * 8;
	vec4 transformed = vec4(0.0);
	for (int row = 0; row < 4; row++) {
		vec4 matrix_row = vec4(unpackHalf2x16(local_transfer.values[row_offset]), unpackHalf2x16(local_transfer.values[row_offset + 1]));
		transformed[row] = dot(matrix_row, value);
		row_offset += 2;
	}
	return transformed;
#elif defined(LOCAL_TRANSFER_LUMINANCE_FP32_TINT)
	int transfer_offset = index * 17;
	vec4 transformed = vec4(0.0);
	for (int row = 0; row < 4; row++) {
		int row_offset = transfer_offset + row * 4;
		vec4 matrix_row = vec4(
				uintBitsToFloat(local_transfer.values[row_offset]),
				uintBitsToFloat(local_transfer.values[row_offset + 1]),
				uintBitsToFloat(local_transfer.values[row_offset + 2]),
				uintBitsToFloat(local_transfer.values[row_offset + 3]));
		transformed[row] = dot(matrix_row, value);
	}
	vec3 tint = unpackUnorm4x8(local_transfer.values[transfer_offset + 16]).rgb;
	return transformed * tint[channel];
#elif defined(LOCAL_TRANSFER_LUMINANCE_FP16_TINT)
	int transfer_offset = index * 9;
	int row_offset = transfer_offset;
	vec4 transformed = vec4(0.0);
	for (int row = 0; row < 4; row++) {
		vec4 matrix_row = vec4(unpackHalf2x16(local_transfer.values[row_offset]), unpackHalf2x16(local_transfer.values[row_offset + 1]));
		transformed[row] = dot(matrix_row, value);
		row_offset += 2;
	}
	vec3 tint = unpackUnorm4x8(local_transfer.values[transfer_offset + 8]).rgb;
	return transformed * tint[channel];
#else
	int row_offset = index * 12 + channel * 4;
	return vec4(
			dot(local_transfer.values[row_offset], value),
			dot(local_transfer.values[row_offset + 1], value),
			dot(local_transfer.values[row_offset + 2], value),
			dot(local_transfer.values[row_offset + 3], value));
#endif
}

void main() {
	int index = params.probe_offset + int(gl_GlobalInvocationID.x);
	if (index >= params.probe_count) {
		return;
	}

	int plane_size = params.resolution.x * params.resolution.y;
	ivec3 position;
	position.z = index / plane_size;
	int plane_index = index - position.z * plane_size;
	position.y = plane_index / params.resolution.x;
	position.x = plane_index - position.y * params.resolution.x;

	if (inside_solid.values[index] != 0u) {
		for (int channel = 0; channel < 3; channel++) {
			radiance_output.values[index * 3 + channel] = vec4(0.0);
		}
		return;
	}

	vec4 local = local_visibility.values[index];
	for (int channel = 0; channel < 3; channel++) {
		int value_index = index * 3 + channel;
		vec4 analytic = injection.values[value_index];

		vec4 gathered = vec4(0.0);
		if (params.neighbor_pattern == 1) {
			int pattern = int((uint(params.pattern_phase) + spatial_dither(position)) % 3u);
			for (int first_sign = -1; first_sign <= 1; first_sign += 2) {
				for (int second_sign = -1; second_sign <= 1; second_sign += 2) {
					gathered += gather_neighbor(channel, position, edge_neighbor_offset(pattern, first_sign, second_sign), 0.25);
				}
			}
		} else {
			for (int z = -1; z <= 1; z++) {
				for (int y = -1; y <= 1; y++) {
					for (int x = -1; x <= 1; x++) {
						ivec3 offset = ivec3(x, y, z);
						if (!all(equal(offset, ivec3(0)))) {
							gathered += gather_neighbor(channel, position, offset, neighbor_weight(offset));
						}
					}
				}
			}
		}

		vec4 filtered_gathered = triple_product(gathered, local);
		vec4 filtered_incoming = positive_product(mesh_light.values[value_index], local) + triple_product(analytic + gathered, local);
		vec4 global_incoming = environment_injection.values[value_index];
		vec4 propagated = filtered_gathered + transform_transfer(index, channel, filtered_incoming + global_incoming);
		radiance_output.values[value_index] = params.neighbor_pattern == 1 ? mix(radiance_input.values[value_index], propagated, 1.0 / 3.0) : propagated;
	}
}
