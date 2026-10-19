#[compute]

#version 450

// Applies dirty 8^3 Trunks to both local-field banks; total Source is invalidated once.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	int patch_count;
	int write_source;
	int pad;
	int pad2;
}
push_constant;

struct PatchData {
	uvec4 header; // probe index, link mask, reserved, reserved
	vec4 material;
	vec4 records[8];
	uint record_slot;
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

layout(set = 0, binding = 10, std430) restrict writeonly buffer SourceRed {
	vec4 data[];
} source_r;
layout(set = 0, binding = 11, std430) restrict writeonly buffer SourceGreen {
	vec4 data[];
} source_g;
layout(set = 0, binding = 12, std430) restrict writeonly buffer SourceBlue {
	vec4 data[];
} source_b;

layout(set = 0, binding = 9, std430) restrict writeonly buffer RecordIndexBuffer {
	uint data[];
}
record_indices;

void main() {
	uint patch_index = gl_GlobalInvocationID.x;
	if (patch_index >= uint(push_constant.patch_count)) {
		return;
	}
	PatchData local_data = patches.data[patch_index];
	uint probe_index = local_data.header.x;
	material.data[probe_index] = local_data.material;
	links.data[probe_index] = local_data.header.y;
	record_indices.data[probe_index] = local_data.record_slot;
	if (local_data.record_slot != 0xffffffffu) {
		for (int row = 0; row < 8; row++) {
			matrices.data[local_data.record_slot * 8u + uint(row)] = local_data.records[row];
		}
		// Source is shared by both banks. Invalidate a reused slot once, before recomposition.
		if (push_constant.write_source != 0) {
			source_r.data[local_data.record_slot] = vec4(0.0);
			source_g.data[local_data.record_slot] = vec4(0.0);
			source_b.data[local_data.record_slot] = vec4(0.0);
		}
	}
}
