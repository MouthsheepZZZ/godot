#[compute]

#version 450

// Presents the solver buffers without feeding temporal display history back into
// transport. Geometry and direct sky remain current; indirect radiance approaches
// the latest solution once per rendered frame, independently of bake publication.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform PushConstant {
	uint write_mask;
	float radiance_weight;
	uint pad1;
	uint pad2;
}
push_constant;

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

layout(set = 0, binding = 17, std430) restrict readonly buffer SkyRBuffer {
	vec4 data[];
} sky_r;

layout(set = 0, binding = 18, std430) restrict readonly buffer SkyGBuffer {
	vec4 data[];
} sky_g;

layout(set = 0, binding = 19, std430) restrict readonly buffer SkyBBuffer {
	vec4 data[];
} sky_b;

layout(rgba32f, set = 0, binding = 20) uniform restrict image2D radiance_r_atlas;
layout(rgba32f, set = 0, binding = 21) uniform restrict image2D radiance_g_atlas;
layout(rgba32f, set = 0, binding = 22) uniform restrict image2D radiance_b_atlas;
layout(rgba32f, set = 0, binding = 23) uniform restrict writeonly image2D visibility_atlas;
layout(rgba32f, set = 0, binding = 24) uniform restrict writeonly image2D source_r_atlas;
layout(rgba32f, set = 0, binding = 25) uniform restrict writeonly image2D source_g_atlas;
layout(rgba32f, set = 0, binding = 26) uniform restrict writeonly image2D source_b_atlas;
layout(rgba32f, set = 0, binding = 27) uniform restrict writeonly image2D sky_r_atlas;
layout(rgba32f, set = 0, binding = 28) uniform restrict writeonly image2D sky_g_atlas;
layout(rgba32f, set = 0, binding = 29) uniform restrict writeonly image2D sky_b_atlas;
layout(rgba32f, set = 0, binding = 30) uniform restrict readonly image2D material_atlas;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	int width = params.grid_size.x * params.grid_size.z;
	ivec2 atlas_coord = ivec2(int(index) % width, int(index) / width);
	if ((push_constant.write_mask & 1u) != 0u) {
		bool solid = imageLoad(material_atlas, atlas_coord).a > 0.5;
		vec4 red = solid ? vec4(0.0) : radiance_r.data[index];
		vec4 green = solid ? vec4(0.0) : radiance_g.data[index];
		vec4 blue = solid ? vec4(0.0) : radiance_b.data[index];
		// Occupied probes cannot retain light. A reset also overwrites history exactly.
		if (!solid && push_constant.radiance_weight < 1.0) {
			red = mix(imageLoad(radiance_r_atlas, atlas_coord), red, push_constant.radiance_weight);
			green = mix(imageLoad(radiance_g_atlas, atlas_coord), green, push_constant.radiance_weight);
			blue = mix(imageLoad(radiance_b_atlas, atlas_coord), blue, push_constant.radiance_weight);
		}
		imageStore(radiance_r_atlas, atlas_coord, red);
		imageStore(radiance_g_atlas, atlas_coord, green);
		imageStore(radiance_b_atlas, atlas_coord, blue);
	}
	if ((push_constant.write_mask & 8u) != 0u) {
		imageStore(visibility_atlas, atlas_coord, visibility.data[index]);
	}
	if ((push_constant.write_mask & 2u) != 0u) {
		imageStore(source_r_atlas, atlas_coord, source_r.data[index]);
		imageStore(source_g_atlas, atlas_coord, source_g.data[index]);
		imageStore(source_b_atlas, atlas_coord, source_b.data[index]);
	}
	if ((push_constant.write_mask & 4u) != 0u) {
		imageStore(sky_r_atlas, atlas_coord, sky_r.data[index]);
		imageStore(sky_g_atlas, atlas_coord, sky_g.data[index]);
		imageStore(sky_b_atlas, atlas_coord, sky_b.data[index]);
	}
}
