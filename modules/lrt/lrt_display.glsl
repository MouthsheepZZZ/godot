#[compute]

#version 450

// Copies the current solver buffers into sampled atlas textures on the main
// RenderingDevice. This is a GPU-only display handoff: normal rendering never
// reads the light field back to the CPU.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
	vec4 sky_color;
	vec4 light_position[8];
	vec4 light_direction[8];
	vec4 light_color[8];
	vec4 light_data[8];
	vec4 light_spot[8];
	vec4 box_min[16];
	vec4 box_max[16];
	vec4 box_color[16];
} params;

layout(set = 0, binding = 6, std430) restrict readonly buffer SourceRBuffer {
	vec4 data[];
} source_r;

layout(set = 0, binding = 7, std430) restrict readonly buffer SourceGBuffer {
	vec4 data[];
} source_g;

layout(set = 0, binding = 8, std430) restrict readonly buffer SourceBBuffer {
	vec4 data[];
} source_b;

layout(set = 0, binding = 9, std430) restrict readonly buffer RadianceRBuffer {
	vec4 data[];
} radiance_r;

layout(set = 0, binding = 10, std430) restrict readonly buffer RadianceGBuffer {
	vec4 data[];
} radiance_g;

layout(set = 0, binding = 11, std430) restrict readonly buffer RadianceBBuffer {
	vec4 data[];
} radiance_b;

layout(set = 0, binding = 12, std430) restrict readonly buffer VisibilityBuffer {
	vec4 data[];
} visibility;

layout(rgba32f, set = 0, binding = 20) uniform restrict writeonly image2D radiance_r_atlas;
layout(rgba32f, set = 0, binding = 21) uniform restrict writeonly image2D radiance_g_atlas;
layout(rgba32f, set = 0, binding = 22) uniform restrict writeonly image2D radiance_b_atlas;
layout(rgba32f, set = 0, binding = 23) uniform restrict writeonly image2D visibility_atlas;
layout(rgba32f, set = 0, binding = 24) uniform restrict writeonly image2D source_r_atlas;
layout(rgba32f, set = 0, binding = 25) uniform restrict writeonly image2D source_g_atlas;
layout(rgba32f, set = 0, binding = 26) uniform restrict writeonly image2D source_b_atlas;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	int width = params.grid_size.x * params.grid_size.z;
	ivec2 atlas_coord = ivec2(int(index) % width, int(index) / width);
	imageStore(radiance_r_atlas, atlas_coord, radiance_r.data[index]);
	imageStore(radiance_g_atlas, atlas_coord, radiance_g.data[index]);
	imageStore(radiance_b_atlas, atlas_coord, radiance_b.data[index]);
	imageStore(visibility_atlas, atlas_coord, visibility.data[index]);
	imageStore(source_r_atlas, atlas_coord, source_r.data[index]);
	imageStore(source_g_atlas, atlas_coord, source_g.data[index]);
	imageStore(source_b_atlas, atlas_coord, source_b.data[index]);
}
