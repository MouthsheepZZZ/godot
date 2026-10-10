#[compute]
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform texture2D field_0;
layout(set = 0, binding = 1) uniform texture2D field_1;
layout(set = 0, binding = 2) uniform texture2D field_2;
layout(set = 0, binding = 3) uniform texture2D field_3;
layout(set = 0, binding = 4) uniform utexture2D field_4;
layout(set = 0, binding = 5) uniform texture2D field_5;
layout(set = 0, binding = 6) uniform texture2D field_6;
layout(set = 0, binding = 7) uniform texture2D field_7;
layout(set = 0, binding = 8) uniform sampler nearest_sampler;
layout(set = 0, binding = 9, std430) restrict writeonly buffer Fields { uvec4 data[]; } fields;
layout(push_constant, std430) uniform Params { ivec4 layout_data; } params;
void main() {
	int probe = int(gl_GlobalInvocationID.x);
	if (probe >= params.layout_data.z) { return; }
	ivec2 coord = ivec2(probe % params.layout_data.y, probe / params.layout_data.y);
	int destination = (params.layout_data.x + probe) * 8;
	fields.data[destination + 0] = floatBitsToUint(texelFetch(sampler2D(field_0, nearest_sampler), coord, 0));
	fields.data[destination + 1] = floatBitsToUint(texelFetch(sampler2D(field_1, nearest_sampler), coord, 0));
	fields.data[destination + 2] = floatBitsToUint(texelFetch(sampler2D(field_2, nearest_sampler), coord, 0));
	fields.data[destination + 3] = floatBitsToUint(texelFetch(sampler2D(field_3, nearest_sampler), coord, 0));
	fields.data[destination + 4] = texelFetch(usampler2D(field_4, nearest_sampler), coord, 0);
	fields.data[destination + 5] = floatBitsToUint(texelFetch(sampler2D(field_5, nearest_sampler), coord, 0));
	fields.data[destination + 6] = floatBitsToUint(texelFetch(sampler2D(field_6, nearest_sampler), coord, 0));
	fields.data[destination + 7] = floatBitsToUint(texelFetch(sampler2D(field_7, nearest_sampler), coord, 0));
}
