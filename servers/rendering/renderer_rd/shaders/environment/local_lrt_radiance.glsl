#[compute]

#version 450

#VERSION_DEFINES

#define SH_Y00 0.28209479177387814

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer LocalVisibility {
	vec4 values[];
}
local_visibility;

layout(set = 0, binding = 1, std430) restrict readonly buffer LocalTransfer {
	vec4 values[];
}
local_transfer;

layout(set = 0, binding = 2, std430) restrict readonly buffer GlobalVisibility {
	vec4 values[];
}
global_visibility;

layout(set = 0, binding = 3, std430) restrict readonly buffer Injection {
	vec4 values[];
}
injection;

layout(set = 0, binding = 4, std430) restrict readonly buffer EmissiveInjection {
	vec4 values[];
}
emissive_injection;

layout(set = 0, binding = 5, std430) restrict readonly buffer DirectRadianceInput {
	vec4 values[];
}
direct_radiance_input;

layout(set = 0, binding = 6, std430) restrict writeonly buffer DirectRadianceOutput {
	vec4 values[];
}
direct_radiance_output;

layout(set = 0, binding = 7, std430) restrict readonly buffer IndirectRadianceInput {
	vec4 values[];
}
indirect_radiance_input;

layout(set = 0, binding = 8, std430) restrict writeonly buffer IndirectRadianceOutput {
	vec4 values[];
}
indirect_radiance_output;

layout(push_constant, std430) uniform Params {
	ivec3 resolution;
	int probe_count;
	vec3 probe_spacing;
	float decay_per_meter;
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

vec4 transform_transfer(int index, int channel, vec4 value) {
	int row_offset = index * 12 + channel * 4;
	return vec4(
			dot(local_transfer.values[row_offset], value),
			dot(local_transfer.values[row_offset + 1], value),
			dot(local_transfer.values[row_offset + 2], value),
			dot(local_transfer.values[row_offset + 3], value));
}

void main() {
	int index = int(gl_GlobalInvocationID.x);
	if (index >= params.probe_count) {
		return;
	}

	int plane_size = params.resolution.x * params.resolution.y;
	ivec3 position;
	position.z = index / plane_size;
	int plane_index = index - position.z * plane_size;
	position.y = plane_index / params.resolution.x;
	position.x = plane_index - position.y * params.resolution.x;

	vec4 local = local_visibility.values[index];
	float transmission = local.x * SH_Y00;
	for (int channel = 0; channel < 3; channel++) {
		int value_index = index * 3 + channel;
		vec4 emitted = emissive_injection.values[value_index];
		vec4 analytic = injection.values[value_index] - emitted;
		if (transmission <= 0.0) {
			direct_radiance_output.values[value_index] = vec4(0.0);
			indirect_radiance_output.values[value_index] = emitted;
			continue;
		}

		vec4 direct_incoming = vec4(0.0);
		vec4 indirect_incoming = vec4(0.0);
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					ivec3 offset = ivec3(x, y, z);
					if (all(equal(offset, ivec3(0)))) {
						continue;
					}
					ivec3 neighbor_position = position + offset;
					if (!is_valid_position(neighbor_position)) {
						continue;
					}

					int neighbor_index = probe_index(neighbor_position);
					int neighbor_value = neighbor_index * 3 + channel;
					float distance_decay = pow(params.decay_per_meter, length(vec3(offset) * params.probe_spacing));
					float weight = neighbor_weight(offset) * distance_decay;
					direct_incoming += triple_product(direct_radiance_input.values[neighbor_value], global_visibility.values[neighbor_index]) * weight;
					indirect_incoming += triple_product(indirect_radiance_input.values[neighbor_value], global_visibility.values[neighbor_index]) * weight;
				}
			}
		}

		direct_incoming = triple_product(direct_incoming, local);
		indirect_incoming = triple_product(indirect_incoming, local);
		direct_radiance_output.values[value_index] = analytic + direct_incoming * transmission;
		indirect_radiance_output.values[value_index] = emitted + indirect_incoming * transmission + transform_transfer(index, channel, direct_incoming + indirect_incoming);
	}
}
