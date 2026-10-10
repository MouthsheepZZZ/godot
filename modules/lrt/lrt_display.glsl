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
layout(rgba32f, set = 0, binding = 30) uniform restrict image2D material_atlas;

// Negative support marks occupied probes across local-field texture uploads.
layout(set = 0, binding = 31, std430) restrict buffer DisplayProbeSupport {
	float data[];
} display_probe_support;

vec4 lrt_blend_display_history(vec4 history, vec4 current, float previous_support, float support) {
	if (support <= 0.0) {
		return vec4(0.0);
	}
	return mix(history * previous_support, current, push_constant.radiance_weight) / support;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	int width = params.grid_size.x * params.grid_size.z;
	ivec2 atlas_coord = ivec2(int(index) % width, int(index) / width);
	if ((push_constant.write_mask & 1u) != 0u) {
		vec4 material = imageLoad(material_atlas, atlas_coord);
		bool solid = material.a > 0.5;
		float previous_support = max(display_probe_support.data[index], 0.0);
		float support = solid ? -1.0 : mix(previous_support, 1.0, push_constant.radiance_weight);
		display_probe_support.data[index] = support;
		// Newly opened probes recover support at the same rate as their displayed light.
		material.b = max(support, 0.0);
		imageStore(material_atlas, atlas_coord, material);
		vec4 red = solid ? vec4(0.0) : radiance_r.data[index];
		vec4 green = solid ? vec4(0.0) : radiance_g.data[index];
		vec4 blue = solid ? vec4(0.0) : radiance_b.data[index];
		// Occupied probes have no valid history. Normalize partial history so clearing
		// an occupied probe does not bias newly displayed light toward black.
		if (!solid && push_constant.radiance_weight < 1.0) {
			red = lrt_blend_display_history(imageLoad(radiance_r_atlas, atlas_coord), red, previous_support, support);
			green = lrt_blend_display_history(imageLoad(radiance_g_atlas, atlas_coord), green, previous_support, support);
			blue = lrt_blend_display_history(imageLoad(radiance_b_atlas, atlas_coord), blue, previous_support, support);
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
