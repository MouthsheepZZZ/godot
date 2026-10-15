#[versions]

prepare = "#define MODE_PREPARE";
filter = "";

#[compute]

#version 450

#VERSION_DEFINES

// Normalized convolution fills occupied display probes without changing transport.
// Interpolate weighted SH and support separately, then normalize at the receiver.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(push_constant, std430) uniform Params {
	ivec4 grid_axis;
	vec4 world_step;
}
params;

#ifdef MODE_PREPARE
layout(set = 0, binding = 0) uniform texture2D source_r;
layout(set = 0, binding = 1) uniform texture2D source_g;
layout(set = 0, binding = 2) uniform texture2D source_b;
layout(set = 0, binding = 3) uniform texture2D source_sky_r;
layout(set = 0, binding = 4) uniform texture2D source_sky_g;
layout(set = 0, binding = 5) uniform texture2D source_sky_b;
layout(set = 0, binding = 6) uniform texture2D source_material;
#else
layout(set = 0, binding = 0) uniform texture3D source_r;
layout(set = 0, binding = 1) uniform texture3D source_g;
layout(set = 0, binding = 2) uniform texture3D source_b;
layout(set = 0, binding = 3) uniform texture3D source_support;
#endif
layout(set = 0, binding = 7) uniform sampler nearest_sampler;
layout(rgba32f, set = 0, binding = 8) uniform restrict writeonly image3D output_r;
layout(rgba32f, set = 0, binding = 9) uniform restrict writeonly image3D output_g;
layout(rgba32f, set = 0, binding = 10) uniform restrict writeonly image3D output_b;
layout(r32f, set = 0, binding = 11) uniform restrict writeonly image3D output_support;

const float RADIUS = 0.3;
const float SIGMA = 0.1;

void main() {
	ivec3 cell = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(cell, params.grid_axis.xyz))) {
		return;
	}
	int axis = params.grid_axis.w;
	float step_size = params.world_step[axis];
	int radius = int(floor(RADIUS / step_size));
	vec4 red = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 blue = vec4(0.0);
	float support = 0.0;
	for (int offset = -radius; offset <= radius; offset++) {
		ivec3 neighbor = cell;
		neighbor[axis] += offset;
		if (neighbor[axis] < 0 || neighbor[axis] >= params.grid_axis[axis]) {
			continue;
		}
		float distance = float(offset) * step_size / SIGMA;
		float weight = exp(-0.5 * distance * distance);
#ifdef MODE_PREPARE
		ivec2 atlas = ivec2(neighbor.x + neighbor.z * params.grid_axis.x, neighbor.y);
		vec4 material = texelFetch(sampler2D(source_material, nearest_sampler), atlas, 0);
		if (material.a > 0.5 || material.b <= 0.0) {
			continue;
		}
		red += weight * (texelFetch(sampler2D(source_r, nearest_sampler), atlas, 0) + texelFetch(sampler2D(source_sky_r, nearest_sampler), atlas, 0));
		green += weight * (texelFetch(sampler2D(source_g, nearest_sampler), atlas, 0) + texelFetch(sampler2D(source_sky_g, nearest_sampler), atlas, 0));
		blue += weight * (texelFetch(sampler2D(source_b, nearest_sampler), atlas, 0) + texelFetch(sampler2D(source_sky_b, nearest_sampler), atlas, 0));
		support += weight;
#else
		red += weight * texelFetch(sampler3D(source_r, nearest_sampler), neighbor, 0);
		green += weight * texelFetch(sampler3D(source_g, nearest_sampler), neighbor, 0);
		blue += weight * texelFetch(sampler3D(source_b, nearest_sampler), neighbor, 0);
		support += weight * texelFetch(sampler3D(source_support, nearest_sampler), neighbor, 0).r;
#endif
	}
	imageStore(output_r, cell, red);
	imageStore(output_g, cell, green);
	imageStore(output_b, cell, blue);
	imageStore(output_support, cell, vec4(support));
}
