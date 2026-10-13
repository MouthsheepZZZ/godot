#[compute]

#version 450

// Reconstructs a display field without feeding it back into transport.

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

layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer {
	vec4 data[];
} transport_material;

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

layout(set = 0, binding = 31, std430) restrict buffer DisplayProbeSupport {
	float data[];
} display_probe_support;

const float DISPLAY_EXTENSION_RADIUS = 0.5;
const float DISPLAY_SMOOTH_RADIUS = 0.25;

ivec3 probe_cell(int index) {
	return ivec3(index % params.grid_size.x, index / (params.grid_size.x * params.grid_size.z),
			(index / params.grid_size.x) % params.grid_size.z);
}

int probe_index(ivec3 cell) {
	return cell.x + params.grid_size.x * (cell.z + params.grid_size.z * cell.y);
}

ivec2 atlas_coord_of(int index) {
	int width = params.grid_size.x * params.grid_size.z;
	return ivec2(index % width, index / width);
}

struct DisplayLight {
	vec4 red;
	vec4 green;
	vec4 blue;
	vec4 sky_red;
	vec4 sky_green;
	vec4 sky_blue;
};

// The same normalized convolution reconstructs original and occupied display samples.
// Read only original transport samples; reconstructed values never feed back.
DisplayLight reconstruct_display_light(int index, out float support) {
	DisplayLight light = DisplayLight(vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
	ivec3 center = probe_cell(index);
	int radius = int(floor(DISPLAY_EXTENSION_RADIUS / params.grid_min.w));
	float total = 0.0;
	for (int z = -radius; z <= radius; z++) {
		for (int y = -radius; y <= radius; y++) {
			for (int x = -radius; x <= radius; x++) {
				ivec3 cell = center + ivec3(x, y, z);
				float distance_squared = dot(vec3(x, y, z), vec3(x, y, z)) * params.grid_min.w * params.grid_min.w;
				if (distance_squared > DISPLAY_EXTENSION_RADIUS * DISPLAY_EXTENSION_RADIUS ||
						any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, params.grid_size.xyz))) {
					continue;
				}
				int candidate = probe_index(cell);
				if (transport_material.data[candidate].a > 0.5) {
					continue;
				}
				float weight = exp(-0.5 * distance_squared / (DISPLAY_SMOOTH_RADIUS * DISPLAY_SMOOTH_RADIUS));
				if ((push_constant.write_mask & 1u) != 0u) {
					light.red += weight * radiance_r.data[candidate];
					light.green += weight * radiance_g.data[candidate];
					light.blue += weight * radiance_b.data[candidate];
				}
				if ((push_constant.write_mask & 4u) != 0u) {
					light.sky_red += weight * sky_r.data[candidate];
					light.sky_green += weight * sky_g.data[candidate];
					light.sky_blue += weight * sky_b.data[candidate];
				}
				total += weight;
			}
		}
	}
	support = total > 0.0 ? 1.0 : 0.0;
	if (total > 0.0) {
		light.red /= total;
		light.green /= total;
		light.blue /= total;
		light.sky_red /= total;
		light.sky_green /= total;
		light.sky_blue /= total;
	}
	return light;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	ivec2 atlas_coord = atlas_coord_of(int(index));
	float support = 0.0;
	DisplayLight light;
	if ((push_constant.write_mask & 5u) != 0u) {
		light = reconstruct_display_light(int(index), support);
	}
	if ((push_constant.write_mask & 1u) != 0u) {
		vec4 material = imageLoad(material_atlas, atlas_coord);
		float previous_support = max(display_probe_support.data[index], 0.0);
		display_probe_support.data[index] = support;
		// Display support is independent of the unchanged transport occupancy in material.a.
		material.b = support;
		imageStore(material_atlas, atlas_coord, material);
		vec4 red = light.red;
		vec4 green = light.green;
		vec4 blue = light.blue;
		// Initialize newly covered samples directly; absent history is not black light.
		if (support > 0.0 && previous_support > 0.0 && push_constant.radiance_weight < 1.0) {
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
		imageStore(sky_r_atlas, atlas_coord, light.sky_red);
		imageStore(sky_g_atlas, atlas_coord, light.sky_green);
		imageStore(sky_b_atlas, atlas_coord, light.sky_blue);
	}
}
