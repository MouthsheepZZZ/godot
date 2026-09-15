#[compute]

#version 450

// Applies the probes of dirty 8^3 trunks to one local-field buffer bank. The final apply patches
// both grid banks, while receiver data is written only to the newly active bank.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int patch_count;
	int probe_count;
	int write_receivers;
	int pad1;
}
push_constant;

struct PatchData {
	uvec4 header; // probe index, link mask, receiver patch start, receiver patch count
	vec4 material;
	vec4 local_visibility;
	vec4 matrices[12];
};

struct ReceiverPatchData {
	vec4 receiver[3];
	vec4 emission;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer PatchBuffer {
	PatchData data[];
}
patches;

layout(set = 0, binding = 1, std430) restrict writeonly buffer MaterialBuffer {
	vec4 data[];
}
material;

layout(set = 0, binding = 2, std430) restrict writeonly buffer LinksBuffer {
	uint data[];
}
links;

layout(set = 0, binding = 3, std430) restrict writeonly buffer MatrixBuffer {
	vec4 data[];
}
matrices;

layout(set = 0, binding = 4, std430) restrict writeonly buffer LocalVisibilityBuffer {
	vec4 data[];
}
local_visibility;

layout(set = 0, binding = 5, std430) restrict readonly buffer ReceiverPatchBuffer {
	ReceiverPatchData data[];
}
receiver_patches;

layout(set = 0, binding = 6, std430) restrict writeonly buffer ReceiverBuffer {
	vec4 data[];
}
receivers;

layout(set = 0, binding = 7, std430) restrict writeonly buffer ReceiverEmissionBuffer {
	vec4 data[];
}
receiver_emission;

void main() {
	uint patch_index = gl_GlobalInvocationID.x;
	if (patch_index >= uint(push_constant.patch_count)) {
		return;
	}
	PatchData local_data = patches.data[patch_index];
	uint probe_index = local_data.header.x;
	material.data[probe_index] = local_data.material;
	links.data[probe_index] = local_data.header.y;
	local_visibility.data[probe_index] = local_data.local_visibility;
	for (int matrix = 0; matrix < 12; matrix++) {
		matrices.data[matrix * push_constant.probe_count + int(probe_index)] = local_data.matrices[matrix];
	}
	uint receiver_vector_start = uint(local_data.material.x);
	uint emission_start = receiver_vector_start / 3u;
	if (push_constant.write_receivers == 0) {
		return;
	}
	for (uint receiver = 0; receiver < local_data.header.w; receiver++) {
		ReceiverPatchData receiver_data = receiver_patches.data[local_data.header.z + receiver];
		for (int vector_index = 0; vector_index < 3; vector_index++) {
			receivers.data[receiver_vector_start + receiver * 3u + uint(vector_index)] = receiver_data.receiver[vector_index];
		}
		receiver_emission.data[emission_start + receiver] = receiver_data.emission;
	}
}
