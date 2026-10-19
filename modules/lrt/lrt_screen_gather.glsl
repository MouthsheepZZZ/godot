#[compute]

#version 450

// Read the latest production buffers; interpolation preserves all eight probe weights.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct LRTData {
	mat4 world_to_volume;
	vec4 volume_min;
	vec4 volume_max;
	vec4 grid_min_spacing;
	ivec4 grid_size_mode;
	vec4 atlas_flags;
};

layout(set = 0, binding = 0, std140) uniform GatherDataBlock {
	mat4 inv_projection;
	mat4 view_to_world;
	ivec4 screen_size;
} gather;
layout(set = 0, binding = 1, std140) uniform LRTDataBlock {
	LRTData data;
} lrt;
layout(set = 0, binding = 2, std430) restrict readonly buffer RadianceR { vec4 data[]; } radiance_r;
layout(set = 0, binding = 3, std430) restrict readonly buffer RadianceG { vec4 data[]; } radiance_g;
layout(set = 0, binding = 4, std430) restrict readonly buffer RadianceB { vec4 data[]; } radiance_b;
layout(set = 0, binding = 5, std430) restrict readonly buffer SkyVisibility { vec4 data[]; } sky_visibility;
layout(set = 0, binding = 6) uniform texture2D depth_buffer;
layout(set = 0, binding = 7) uniform texture2D normal_roughness_buffer;
layout(rgba16f, set = 0, binding = 8) uniform restrict image2D gather_lighting;
layout(r16f, set = 0, binding = 9) uniform restrict image2D gather_sky_visibility;
layout(set = 0, binding = 10) uniform sampler nearest_sampler;

layout(push_constant, std430) uniform Composition {
	ivec4 region;
	ivec4 flags;
} composition;

const float PI = 3.14159265358979323846;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;

void main() {
	ivec2 offset = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(offset, composition.region.zw))) {
		return;
	}
	ivec2 coord = offset + composition.region.xy;
	ivec2 full_coord = min(ivec2((vec2(coord) + 0.5) * vec2(gather.screen_size.xy) /
			vec2(gather.screen_size.zw)), gather.screen_size.xy - ivec2(1));
	vec4 packed_normal = texelFetch(sampler2D(normal_roughness_buffer, nearest_sampler), full_coord, 0);
	if (all(lessThan(abs(packed_normal.xyz), vec3(0.00001)))) {
		return;
	}
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), full_coord, 0).r;
	vec2 ndc = 2.0 * (vec2(full_coord) + 0.5) / vec2(gather.screen_size.xy) - 1.0;
	vec4 view_position = gather.inv_projection * vec4(ndc, depth, 1.0);
	vec3 world_position = (gather.view_to_world * vec4(view_position.xyz / view_position.w, 1.0)).xyz;
	vec3 position = (lrt.data.world_to_volume * vec4(world_position, 1.0)).xyz;
	if (any(lessThan(position, lrt.data.volume_min.xyz)) || any(greaterThan(position, lrt.data.volume_max.xyz))) {
		return;
	}
	vec3 face_distance = min(position - lrt.data.volume_min.xyz, lrt.data.volume_max.xyz - position);
	float boundary_distance = min(face_distance.x, min(face_distance.y, face_distance.z));
	float blend_distance = lrt.data.atlas_flags.w;
	float weight = blend_distance > 0.0 ? clamp(boundary_distance / blend_distance, 0.0, 1.0) : 1.0;
	if (weight <= 0.0) {
		return;
	}
	vec3 world_normal = normalize(mat3(gather.view_to_world) * (packed_normal.xyz * 2.0 - 1.0));
	vec3 normal = normalize(transpose(inverse(mat3(lrt.data.world_to_volume))) * world_normal);
	vec3 grid_position = (position - lrt.data.grid_min_spacing.xyz) / lrt.data.grid_min_spacing.w - 0.5;
	ivec3 low = ivec3(floor(grid_position));
	vec3 fraction = fract(grid_position);
	vec4 red = vec4(0.0);
	vec4 green = vec4(0.0);
	vec4 blue = vec4(0.0);
	vec4 visibility = vec4(0.0);
	for (int corner_index = 0; corner_index < 8; corner_index++) {
		ivec3 corner = ivec3(corner_index & 1, (corner_index >> 1) & 1, (corner_index >> 2) & 1);
		ivec3 cell = clamp(low + corner, ivec3(0), lrt.data.grid_size_mode.xyz - ivec3(1));
		vec3 axis_weight = mix(vec3(1.0) - fraction, fraction, vec3(corner));
		float probe_weight = axis_weight.x * axis_weight.y * axis_weight.z;
		int probe = cell.x + lrt.data.grid_size_mode.x * (cell.z + lrt.data.grid_size_mode.z * cell.y);
		red += radiance_r.data[probe] * probe_weight;
		green += radiance_g.data[probe] * probe_weight;
		blue += radiance_b.data[probe] * probe_weight;
		if (composition.flags.x != 0) {
			visibility += sky_visibility.data[probe] * probe_weight;
		}
	}
	vec4 kernel = vec4(PI * C0, (2.0 * PI / 3.0) * C1 * normal);
	vec3 gi = max(vec3(dot(red, kernel), dot(green, kernel), dot(blue, kernel)), vec3(0.0)) / PI;
	float sky = dot(visibility, kernel) / PI;
	vec4 lower = imageLoad(gather_lighting, coord);
	float lower_sky = imageLoad(gather_sky_visibility, coord).r;
	imageStore(gather_lighting, coord, vec4(gi * weight, weight) + lower * (1.0 - weight));
	imageStore(gather_sky_visibility, coord, vec4(sky * weight + lower_sky * (1.0 - weight), 0.0, 0.0, 0.0));
}
