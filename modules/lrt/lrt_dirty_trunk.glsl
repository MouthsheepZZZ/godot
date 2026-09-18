#[compute]

#version 450

// O5 experiment: rebuild the complete local data of dirty 8^3 Trunks from packed per-object
// Color SDF data. CPU work is limited to dirty-Trunk/candidate collection and asset packing.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

const float PI = 3.141592653589793;
const float C0 = 0.2820947917738781;
const float C1 = 0.4886025119029199;
const float WEIGHT = 4.0 * PI / 26.0;
const int DIRECTION_COUNT = 26;

const ivec3 DIRECTIONS[DIRECTION_COUNT] = ivec3[DIRECTION_COUNT](
	%LRT_DIRECTION_OFFSETS%
);

layout(push_constant, std430) uniform PushConstant {
	vec4 grid_min_spacing;
	ivec4 grid_size_count;
	ivec4 trunk_size_count;
	int dirty_probe_count;
	int pad0;
	int pad1;
	int pad2;
}
push_constant;

struct DirtyProbeInput {
	uvec4 data; // probe index, unused
};

struct PrimitiveData {
	vec4 origin_pad;
	vec4 inverse_x;
	vec4 inverse_y;
	vec4 inverse_z;
	vec4 geometry_min_cell;
	vec4 distance_scale_pad;
	uvec4 geometry_size_distance_offset;
	uvec4 color_size_albedo_offset;
	uvec4 emission_offset_flags;
};

struct ProbeOutput {
	uvec4 header; // probe index, propagation links, receiver links, receiver count
	vec4 material; // receiver vector offset is unused in the scratch result; A is solid
	vec4 local_visibility;
	vec4 matrices[5];
};

struct ReceiverOutput {
	vec4 receiver[3];
	vec4 emission;
};

struct SdfSample {
	float distance;
	vec3 normal;
	vec3 color;
	vec3 emission;
	uint layer_mask;
	bool valid;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer DirtyProbeBuffer {
	DirtyProbeInput data[];
}
dirty_probes;

layout(set = 0, binding = 1, std430) restrict readonly buffer CandidateBuffer {
	uint data[];
}
candidates;

layout(set = 0, binding = 2, std430) restrict readonly buffer TrunkRangeBuffer {
	uvec4 data[]; // candidate start, candidate count, unused
}
trunk_ranges;

layout(set = 0, binding = 3, std430) restrict readonly buffer PrimitiveBuffer {
	PrimitiveData data[];
}
primitives;

layout(set = 0, binding = 4, std430) restrict readonly buffer DistanceBuffer {
	uint data[];
}
distances;

layout(set = 0, binding = 5, std430) restrict readonly buffer AlbedoBuffer {
	uint data[];
}
albedos;

layout(set = 0, binding = 6, std430) restrict readonly buffer EmissionBuffer {
	vec4 data[];
}
emissions;

layout(set = 0, binding = 7, std430) restrict writeonly buffer ProbeOutputBuffer {
	ProbeOutput data[];
}
probe_outputs;

layout(set = 0, binding = 8, std430) restrict writeonly buffer ReceiverOutputBuffer {
	ReceiverOutput data[];
}
receiver_outputs;

vec3 probe_point(ivec3 coordinate) {
	return push_constant.grid_min_spacing.xyz + (vec3(coordinate) + vec3(0.5)) * push_constant.grid_min_spacing.w;
}

ivec3 probe_coordinate(uint probe_index) {
	int x = int(probe_index) % push_constant.grid_size_count.x;
	int z = (int(probe_index) / push_constant.grid_size_count.x) % push_constant.grid_size_count.z;
	int y = int(probe_index) / (push_constant.grid_size_count.x * push_constant.grid_size_count.z);
	return ivec3(x, y, z);
}

bool inside_grid(ivec3 coordinate) {
	return all(greaterThanEqual(coordinate, ivec3(0))) &&
			all(lessThan(coordinate, push_constant.grid_size_count.xyz));
}

uvec2 candidate_range(ivec3 coordinate) {
	ivec3 trunk_coordinate = coordinate / 8;
	int trunk_index = trunk_coordinate.x + push_constant.trunk_size_count.x *
			(trunk_coordinate.y + push_constant.trunk_size_count.y * trunk_coordinate.z);
	return trunk_ranges.data[trunk_index].xy;
}

int distance_value(uint offset, uint index) {
	uint packed = distances.data[(offset + index) >> 1u];
	uint bits = (((offset + index) & 1u) == 0u) ? (packed & 0xffffu) : (packed >> 16u);
	return bits >= 0x8000u ? int(bits) - 0x10000 : int(bits);
}

vec3 unpack_albedo(uint packed) {
	return vec3(float(packed & 0xffu), float((packed >> 8u) & 0xffu),
			float((packed >> 16u) & 0xffu)) / 255.0;
}

SdfSample sample_primitive(uint primitive_index, vec3 world_point) {
	PrimitiveData primitive = primitives.data[primitive_index];
	if (primitive.emission_offset_flags.z == 0u) {
		SdfSample invalid;
		invalid.valid = false;
		return invalid;
	}
	vec3 delta = world_point - primitive.origin_pad.xyz;
	vec3 local = vec3(dot(delta, primitive.inverse_x.xyz), dot(delta, primitive.inverse_y.xyz),
			dot(delta, primitive.inverse_z.xyz));
	ivec3 geometry_size = ivec3(primitive.geometry_size_distance_offset.xyz);
	vec3 coordinate = (local - primitive.geometry_min_cell.xyz) / primitive.geometry_min_cell.w;
	vec3 g = clamp(coordinate, vec3(0.0), vec3(geometry_size - ivec3(1)));
	ivec3 base = min(geometry_size - ivec3(2), ivec3(floor(g)));
	vec3 f = g - vec3(base);
	float distance = 0.0;
	vec3 normal = vec3(0.0);
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				ivec3 corner = ivec3(x, y, z);
				vec3 w = mix(vec3(1.0) - f, f, vec3(corner));
				uint index = uint(base.x + x + geometry_size.x * (base.y + y + geometry_size.y * (base.z + z)));
				float d = float(distance_value(primitive.geometry_size_distance_offset.w, index)) * primitive.distance_scale_pad.x;
				distance += w.x * w.y * w.z * d;
				normal.x += d * (x != 0 ? 1.0 : -1.0) * w.y * w.z / primitive.geometry_min_cell.w;
				normal.y += d * (y != 0 ? 1.0 : -1.0) * w.z * w.x / primitive.geometry_min_cell.w;
				normal.z += d * (z != 0 ? 1.0 : -1.0) * w.x * w.y / primitive.geometry_min_cell.w;
			}
		}
	}
	ivec3 color_size = ivec3(primitive.color_size_albedo_offset.xyz);
	vec3 cg = g / vec3(geometry_size - ivec3(1)) * vec3(color_size - ivec3(1));
	ivec3 color_base = min(color_size - ivec3(2), ivec3(floor(cg)));
	vec3 cf = cg - vec3(color_base);
	vec3 color = vec3(0.0);
	vec3 emission = vec3(0.0);
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				ivec3 corner = ivec3(x, y, z);
				vec3 w = mix(vec3(1.0) - cf, cf, vec3(corner));
				float weight = w.x * w.y * w.z;
				uint index = uint(color_base.x + x + color_size.x * (color_base.y + y + color_size.y * (color_base.z + z)));
				color += weight * unpack_albedo(albedos.data[primitive.color_size_albedo_offset.w + index]);
				if (primitive.emission_offset_flags.y != 0u) {
					emission += weight * emissions.data[primitive.emission_offset_flags.x + index].xyz;
				}
			}
		}
	}
	vec3 outside = (coordinate - g) * primitive.geometry_min_cell.w;
	float outside_length = length(outside);
	if (outside_length > 0.0) {
		distance = max(0.0, distance) + outside_length;
		normal = outside / outside_length;
	} else if (dot(normal, normal) > 0.0) {
		normal = normalize(normal);
	}
	vec3 transformed_gradient = primitive.inverse_x.xyz * normal.x +
			primitive.inverse_y.xyz * normal.y + primitive.inverse_z.xyz * normal.z;
	float gradient_length = length(transformed_gradient);
	if (gradient_length > 1e-6) {
		distance /= gradient_length;
		normal = transformed_gradient / gradient_length;
	}
	SdfSample result;
	result.distance = distance;
	result.normal = normal;
	result.color = color;
	result.emission = emission;
	result.layer_mask = primitive.emission_offset_flags.w;
	result.valid = true;
	return result;
}

SdfSample sample_nearest(vec3 point, uint candidate_start, uint candidate_count) {
	SdfSample result;
	result.valid = false;
	for (uint i = 0u; i < candidate_count; i++) {
		SdfSample value = sample_primitive(candidates.data[candidate_start + i], point);
		if (value.valid && (!result.valid || value.distance < result.distance)) {
			result = value;
		}
	}
	return result;
}

bool endpoint_blocks(vec3 point, SdfSample sample_value, vec3 other, float spacing) {
	if (!sample_value.valid || sample_value.distance < 0.0 || dot(sample_value.normal, sample_value.normal) <= 1e-6) {
		return false;
	}
	vec3 surface_point = point - sample_value.normal * sample_value.distance;
	return dot(other - surface_point, sample_value.normal) < -spacing * 1e-4;
}

vec4 sh_basis(vec3 direction) {
	return vec4(C0, C1 * direction.x, C1 * direction.y, C1 * direction.z);
}

vec4 positive_basis(vec3 direction) {
	return vec4(C0, C1 * direction.x / 3.0, C1 * direction.y / 3.0, C1 * direction.z / 3.0);
}

vec4 cosine_basis(vec3 normal) {
	return vec4(PI * C0, 2.0 * PI / 3.0 * C1 * normal.x,
			2.0 * PI / 3.0 * C1 * normal.y, 2.0 * PI / 3.0 * C1 * normal.z);
}

void main() {
	uint dirty_index = gl_GlobalInvocationID.x;
	if (dirty_index >= uint(push_constant.dirty_probe_count)) {
		return;
	}
	DirtyProbeInput input_data = dirty_probes.data[dirty_index];
	uint probe_index = input_data.data.x;
	ivec3 coordinate = probe_coordinate(probe_index);
	uvec2 origin_range = candidate_range(coordinate);
	vec3 origin = probe_point(coordinate);
	float spacing = push_constant.grid_min_spacing.w;
	SdfSample origin_sample = sample_nearest(origin, origin_range.x, origin_range.y);
	ProbeOutput output_data;
	output_data.header = uvec4(probe_index, 0u, 0u, 0u);
	output_data.material = vec4(0.0);
	output_data.local_visibility = vec4(0.0);
	for (int i = 0; i < 5; i++) {
		output_data.matrices[i] = vec4(0.0);
	}
	if (origin_sample.valid && origin_sample.distance < 0.0) {
		output_data.material = vec4(0.5, 0.5, 0.5, 1.0);
		probe_outputs.data[dirty_index] = output_data;
		return;
	}
	float matrix_values[48];
	for (int i = 0; i < 48; i++) {
		matrix_values[i] = 0.0;
	}
	uint receiver_count = 0u;
	for (int direction_index = 0; direction_index < DIRECTION_COUNT; direction_index++) {
		ivec3 target_coordinate = coordinate + DIRECTIONS[direction_index];
		vec3 target_point = probe_point(target_coordinate);
		bool target_inside = inside_grid(target_coordinate);
		uvec2 target_range = target_inside ? candidate_range(target_coordinate) : origin_range;
		SdfSample target_sample = sample_nearest(target_point, target_range.x, target_range.y);
		if (target_inside && (!target_sample.valid || target_sample.distance >= 0.0) &&
				!endpoint_blocks(origin, origin_sample, target_point, spacing) &&
				!endpoint_blocks(target_point, target_sample, origin, spacing)) {
			output_data.header.z |= 1u << uint(direction_index);
		}
		if (!target_sample.valid || target_sample.distance > spacing * 0.5) {
			output_data.header.y |= 1u << uint(direction_index);
			continue;
		}
		vec3 direction = normalize(vec3(DIRECTIONS[direction_index]));
		vec4 outgoing = positive_basis(direction);
		vec4 incident = cosine_basis(-direction);
		for (int channel = 0; channel < 3; channel++) {
			float factor = (4.0 / 26.0) * target_sample.color[channel];
			for (int row = 0; row < 4; row++) {
				for (int column = 0; column < 4; column++) {
					int matrix_index = (channel * 4 + row) * 4 + column;
					matrix_values[matrix_index] += factor * outgoing[row] * incident[column];
				}
			}
		}
		vec3 receiver_position = target_point - target_sample.distance * target_sample.normal;
		float facing = dot(target_sample.normal, origin - receiver_position) < 0.0 ? -1.0 : 1.0;
		ReceiverOutput receiver_data;
		receiver_data.receiver[0] = vec4(receiver_position, float(direction_index));
		receiver_data.receiver[1] = vec4(target_sample.normal * facing, uintBitsToFloat(target_sample.layer_mask));
		receiver_data.receiver[2] = vec4(target_sample.color, 0.0);
		receiver_data.emission = vec4(target_sample.emission, 0.0);
		receiver_outputs.data[dirty_index * uint(DIRECTION_COUNT) + receiver_count] = receiver_data;
		receiver_count++;
	}
	output_data.header.w = receiver_count;
	output_data.material.y = float(receiver_count);
	for (int direction_index = 0; direction_index < DIRECTION_COUNT; direction_index++) {
		if ((output_data.header.y & (1u << uint(direction_index))) != 0u) {
			output_data.local_visibility += WEIGHT * sh_basis(normalize(vec3(DIRECTIONS[direction_index])));
		}
	}
	float luminance[16];
	float denominator = 0.0;
	for (int element = 0; element < 16; element++) {
		luminance[element] = 0.2126 * matrix_values[element] +
				0.7152 * matrix_values[16 + element] + 0.0722 * matrix_values[32 + element];
		denominator += luminance[element] * luminance[element];
		output_data.matrices[element / 4][element % 4] = luminance[element];
	}
	vec3 tint = vec3(0.0);
	if (denominator > 1e-20) {
		for (int channel = 0; channel < 3; channel++) {
			float numerator = 0.0;
			for (int element = 0; element < 16; element++) {
				numerator += matrix_values[channel * 16 + element] * luminance[element];
			}
			tint[channel] = max(0.0, numerator / denominator);
		}
		float encoded_luminance = dot(tint, vec3(0.2126, 0.7152, 0.0722));
		if (encoded_luminance > 1e-12) {
			tint /= encoded_luminance;
		}
	}
	output_data.matrices[4] = vec4(tint, 0.0);
	probe_outputs.data[dirty_index] = output_data;
}
