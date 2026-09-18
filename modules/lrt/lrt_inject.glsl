#[compute]

#version 450

// LRT first-bounce injection. Direct light is exclusively the coherent native Forward+
// receiver capture from R7; runtime analytic lights and BVH shadow traces no longer exist.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
} params;

layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer {
	vec4 data[];
}
material;

layout(set = 0, binding = 5, std430) restrict readonly buffer ReceiverBuffer {
	vec4 data[];
}
receivers;

layout(set = 0, binding = 6, std430) restrict buffer SourceRBuffer {
	vec4 data[];
}
source_r;

layout(set = 0, binding = 7, std430) restrict buffer SourceGBuffer {
	vec4 data[];
}
source_g;

layout(set = 0, binding = 8, std430) restrict buffer SourceBBuffer {
	vec4 data[];
}
source_b;

layout(set = 0, binding = 20, std430) restrict readonly buffer ReceiverEmissionBuffer {
	vec4 data[];
}
receiver_emission;

// RGB contains the native diffuse response at each receiver, including N.L / PI, light
// attenuation, projector/area response and raster shadowing.
layout(set = 0, binding = 21, std430) restrict buffer ReceiverLightingBuffer {
	vec4 data[];
}
receiver_lighting;

layout(set = 0, binding = 22, std430) restrict readonly buffer NativeLightUnitBufferA {
	vec4 data[];
}
native_light_units_a;

layout(set = 0, binding = 23, std430) restrict readonly buffer NativeLightUnitBufferB {
	vec4 data[];
}
native_light_units_b;

// Three vec4 values per light: RGB scale + cull-mask bits, current buffer/blend/enabled,
// then volume-local positional influence sphere (negative radius means directional/global).
layout(set = 0, binding = 24, std430) restrict readonly buffer NativeLightStateBuffer {
	vec4 data[];
}
native_light_states;

// The light loop walks one whole unit-field region per light. Running it receiver-major keeps
// consecutive invocations on consecutive unit-field entries, so each light streams instead of
// gathering across every light's region for one receiver.
layout(push_constant, std430) uniform PushArgs {
	int mode; // 0 source only, 1 receiver lighting, 2 source from precomputed lighting
	int pad0;
	int pad1;
	int pad2;
}
push;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float W = 4.0 * PI / 26.0;

const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

vec4 P(vec3 direction) {
	return vec4(C0, (C1 / 3.0) * direction);
}

vec3 native_receiver_lighting(int receiver_index, vec3 receiver_position, uint receiver_layer_mask) {
	vec3 result = vec3(0.0);
	for (int light_index = 0; light_index < params.counts.y; light_index++) {
		vec4 scale = native_light_states.data[light_index * 4];
		vec4 state = native_light_states.data[light_index * 4 + 1];
		if (state.z < 0.5) {
			continue;
		}
		vec4 influence = native_light_states.data[light_index * 4 + 2];
		if ((floatBitsToUint(scale.w) & receiver_layer_mask) == 0u ||
				(influence.w >= 0.0 && dot(receiver_position - influence.xyz, receiver_position - influence.xyz) > influence.w * influence.w)) {
			continue;
		}
		int field_index = light_index * params.counts.x + receiver_index;
		vec3 unit_field;
		if (state.y <= 0.0) {
			unit_field = state.x < 0.5 ? native_light_units_a.data[field_index].rgb : native_light_units_b.data[field_index].rgb;
		} else {
			vec3 field_a = native_light_units_a.data[field_index].rgb;
			vec3 field_b = native_light_units_b.data[field_index].rgb;
			vec3 current = state.x < 0.5 ? field_a : field_b;
			vec3 target = state.x < 0.5 ? field_b : field_a;
			unit_field = mix(current, target, state.y);
		}
		result += unit_field * scale.rgb;
	}
	return result;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (push.mode == 1) {
		if (index >= uint(params.counts.x)) {
			return;
		}
		uint receiver_layer_mask = floatBitsToUint(receivers.data[index * 3u + 1u].w);
		vec3 receiver_lighting_value = native_receiver_lighting(int(index), receivers.data[index * 3u].xyz, receiver_layer_mask);
		receiver_lighting.data[index] = vec4(receiver_lighting_value, 0.0);
		return;
	}
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	source_r.data[index] = vec4(0.0);
	source_g.data[index] = vec4(0.0);
	source_b.data[index] = vec4(0.0);
	if (material.data[index].a > 0.5) {
		return;
	}
	vec4 header = material.data[index];
	for (int receiver_index = 0; receiver_index < int(header.g); receiver_index++) {
		int base = int(header.r) + receiver_index * 3;
		vec4 receiver_data = receivers.data[base];
		vec3 albedo = receivers.data[base + 2].rgb;
		vec3 direction = normalize(vec3(OFFSETS[int(receiver_data.w)]));
		vec4 projected = W * P(direction);
		vec3 emission = receiver_emission.data[base / 3].rgb;
		int packed_receiver_index = base / 3;
		vec3 lighting = vec3(0.0);
		if (push.mode == 2) {
			lighting = receiver_lighting.data[packed_receiver_index].rgb;
		} else if (params.flags.w > 1.5) {
			uint receiver_layer_mask = floatBitsToUint(receivers.data[base + 1].w);
			lighting = native_receiver_lighting(packed_receiver_index, receiver_data.xyz, receiver_layer_mask);
			receiver_lighting.data[packed_receiver_index] = vec4(lighting, 0.0);
		} else if (params.flags.w > 0.5) {
			lighting = receiver_lighting.data[packed_receiver_index].rgb;
		}
		vec3 reflected = albedo * lighting;
		vec3 source = emission + reflected;
		source_r.data[index] += projected * source.r;
		source_g.data[index] += projected * source.g;
		source_b.data[index] += projected * source.b;
	}
}
