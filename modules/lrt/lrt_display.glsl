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
	vec4 sky_samples[%LRT_SKY_DIRECTION_COUNT%];
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

layout(set = 0, binding = 17, std430) restrict readonly buffer DirectionalVisibilityBuffer {
	vec4 data[];
} directional_visibility;

layout(rgba32f, set = 0, binding = 20) uniform restrict writeonly image2D radiance_r_atlas;
layout(rgba32f, set = 0, binding = 21) uniform restrict writeonly image2D radiance_g_atlas;
layout(rgba32f, set = 0, binding = 22) uniform restrict writeonly image2D radiance_b_atlas;
layout(rgba32f, set = 0, binding = 23) uniform restrict writeonly image2D visibility_atlas;
layout(rgba32f, set = 0, binding = 24) uniform restrict writeonly image2D source_r_atlas;
layout(rgba32f, set = 0, binding = 25) uniform restrict writeonly image2D source_g_atlas;
layout(rgba32f, set = 0, binding = 26) uniform restrict writeonly image2D source_b_atlas;
layout(rgba32f, set = 0, binding = 27) uniform restrict writeonly image2D sky_r_atlas;
layout(rgba32f, set = 0, binding = 28) uniform restrict writeonly image2D sky_g_atlas;
layout(rgba32f, set = 0, binding = 29) uniform restrict writeonly image2D sky_b_atlas;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const int SKY_DIRECTION_COUNT = %LRT_SKY_DIRECTION_COUNT%;
const int SKY_DIRECTION_LANES = %LRT_SKY_DIRECTION_LANES%;

const vec4 SKY_DIRECTIONS[SKY_DIRECTION_COUNT] = vec4[SKY_DIRECTION_COUNT](%LRT_SKY_DIRECTIONS%);

vec4 P(vec3 direction) {
	return vec4(C0, (C1 / 3.0) * direction);
}

vec4 project_non_negative(vec4 value) {
	float dc = C0 * value.x;
	float amplitude = C1 * length(value.yzw);
	if (amplitude <= dc) {
		return value;
	}
	if (dc <= 0.0) {
		return vec4(0.0);
	}
	return vec4(value.x, value.yzw * (dc / amplitude));
}

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
	vec4 sky_r = vec4(0.0);
	vec4 sky_g = vec4(0.0);
	vec4 sky_b = vec4(0.0);
	for (int direction_index = 0; direction_index < SKY_DIRECTION_COUNT; direction_index++) {
		float visible = directional_visibility.data[index * SKY_DIRECTION_LANES + direction_index / 4][direction_index % 4];
		vec4 direction = SKY_DIRECTIONS[direction_index];
		vec4 projected = direction.w * P(direction.xyz) * visible;
		sky_r += projected * params.sky_samples[direction_index].r;
		sky_g += projected * params.sky_samples[direction_index].g;
		sky_b += projected * params.sky_samples[direction_index].b;
	}
	imageStore(sky_r_atlas, atlas_coord, project_non_negative(sky_r));
	imageStore(sky_g_atlas, atlas_coord, project_non_negative(sky_g));
	imageStore(sky_b_atlas, atlas_coord, project_non_negative(sky_b));
}
