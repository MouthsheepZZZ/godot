#[compute]

#version 450

#VERSION_DEFINES

#define SH_Y00 0.28209479177387814
#define SH_Y1 0.4886025119029199
#define SH_TAU 6.283185307179586
#define SH_FOUR_PI 12.566370614359172
#define LIGHT_DIRECTIONAL 1
#define LIGHT_OMNI 2
#define LIGHT_SPOT 3
#define LIGHT_AREA 4
#define AREA_SHADOW_SAMPLE_COUNT 16

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer AnalyticLights {
	vec4 values[];
}
analytic_lights;

layout(set = 0, binding = 1, std430) restrict readonly buffer InsideSolid {
	uint values[];
}
inside_solid;

layout(set = 0, binding = 2, std430) restrict buffer Injection {
	vec4 values[];
}
injection;

layout(set = 0, binding = 3, std430) restrict buffer ShadowVisibility {
	float values[];
}
shadow_visibility;

layout(set = 0, binding = 4) uniform sampler2D shadow_map;

layout(set = 0, binding = 5, std430) restrict readonly buffer ShadowMatrix {
	vec4 columns[4];
}
shadow_matrix;

layout(set = 0, binding = 6) uniform sampler2D positional_shadow_atlas;

layout(set = 0, binding = 7, std430) restrict readonly buffer GlobalVisibility {
	vec4 values[];
}
global_visibility;

layout(set = 0, binding = 8, std430) restrict readonly buffer EnvironmentSH {
	vec4 values[3];
}
environment_sh;

layout(set = 0, binding = 9, std430) restrict buffer EnvironmentInjection {
	vec4 values[];
}
environment_injection;

layout(set = 0, binding = 10, std430) restrict readonly buffer LocalVisibility {
	vec4 values[];
}
local_visibility;

layout(set = 0, binding = 11, std430) restrict readonly buffer LocalTransfer {
#if defined(LOCAL_TRANSFER_RGB_FP16) || defined(LOCAL_TRANSFER_LUMINANCE_FP32_TINT) || defined(LOCAL_TRANSFER_LUMINANCE_FP16_TINT)
	uint values[];
#else
	vec4 values[];
#endif
}
local_transfer;

layout(set = 0, binding = 12, std430) restrict readonly buffer MeshLight {
	vec4 values[];
}
mesh_light;

layout(push_constant, std430) uniform Params {
	ivec3 resolution;
	int probe_count;
	vec3 size;
	int light_count;
	vec4 xform_x;
	vec4 xform_y;
	vec4 xform_z;
	vec4 xform_origin;
	float directional_shadow_bias;
	int directional_shadow_enabled;
	int directional_shadow_resolution;
	int positional_shadow_resolution;
	int probe_offset;
	int dispatch_probe_count;
}
params;

ivec3 probe_position(int index) {
	int plane = params.resolution.x * params.resolution.y;
	int z = index / plane;
	int plane_index = index - z * plane;
	int y = plane_index / params.resolution.x;
	int x = plane_index - y * params.resolution.x;
	return ivec3(x, y, z);
}

vec3 xform_point(vec3 local_position) {
	return params.xform_x.xyz * local_position.x + params.xform_y.xyz * local_position.y + params.xform_z.xyz * local_position.z + params.xform_origin.xyz;
}

vec3 to_local_dir(vec3 world_direction) {
	return vec3(dot(params.xform_x.xyz, world_direction), dot(params.xform_y.xyz, world_direction), dot(params.xform_z.xyz, world_direction));
}

vec4 encode_direction(vec3 direction, float energy) {
	vec3 n = normalize(direction);
	return vec4(SH_Y00, SH_Y1 * n.x, SH_Y1 * n.y, SH_Y1 * n.z) * (energy * SH_TAU);
}

vec4 triple_product(vec4 a, vec4 b) {
	return vec4(
			dot(a, b),
			a.x * b.y + b.x * a.y,
			a.x * b.z + b.x * a.z,
			a.x * b.w + b.x * a.w) * SH_Y00;
}

vec4 sh_basis(vec3 direction) {
	vec3 n = normalize(direction);
	return vec4(SH_Y00, SH_Y1 * n.x, SH_Y1 * n.y, SH_Y1 * n.z);
}

float neighbor_weight(ivec3 offset) {
	float inverse_distance = inversesqrt(float(dot(offset, offset)));
	return inverse_distance / 19.104083527755577;
}

vec4 positive_product(vec4 a, vec4 b) {
	if (all(equal(a, vec4(0.0)))) {
		return vec4(0.0);
	}
	vec4 result = vec4(0.0);
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 offset = ivec3(x, y, z);
				if (all(equal(offset, ivec3(0)))) {
					continue;
				}
				vec4 basis = sh_basis(normalize(vec3(offset)));
				float value = max(dot(a, basis), 0.0) * max(dot(b, basis), 0.0);
				result += basis * (value * SH_FOUR_PI * neighbor_weight(offset));
			}
		}
	}
	return result;
}

vec4 transform_transfer(int index, int channel, vec4 value) {
#ifdef LOCAL_TRANSFER_RGB_FP16
	int row_offset = index * 24 + channel * 8;
	vec4 row_0 = vec4(unpackHalf2x16(local_transfer.values[row_offset]), unpackHalf2x16(local_transfer.values[row_offset + 1]));
	vec4 row_1 = vec4(unpackHalf2x16(local_transfer.values[row_offset + 2]), unpackHalf2x16(local_transfer.values[row_offset + 3]));
	vec4 row_2 = vec4(unpackHalf2x16(local_transfer.values[row_offset + 4]), unpackHalf2x16(local_transfer.values[row_offset + 5]));
	vec4 row_3 = vec4(unpackHalf2x16(local_transfer.values[row_offset + 6]), unpackHalf2x16(local_transfer.values[row_offset + 7]));
	return vec4(dot(row_0, value), dot(row_1, value), dot(row_2, value), dot(row_3, value));
#elif defined(LOCAL_TRANSFER_LUMINANCE_FP32_TINT)
	int transfer_offset = index * 17;
	vec4 row_0 = vec4(uintBitsToFloat(local_transfer.values[transfer_offset]), uintBitsToFloat(local_transfer.values[transfer_offset + 1]), uintBitsToFloat(local_transfer.values[transfer_offset + 2]), uintBitsToFloat(local_transfer.values[transfer_offset + 3]));
	vec4 row_1 = vec4(uintBitsToFloat(local_transfer.values[transfer_offset + 4]), uintBitsToFloat(local_transfer.values[transfer_offset + 5]), uintBitsToFloat(local_transfer.values[transfer_offset + 6]), uintBitsToFloat(local_transfer.values[transfer_offset + 7]));
	vec4 row_2 = vec4(uintBitsToFloat(local_transfer.values[transfer_offset + 8]), uintBitsToFloat(local_transfer.values[transfer_offset + 9]), uintBitsToFloat(local_transfer.values[transfer_offset + 10]), uintBitsToFloat(local_transfer.values[transfer_offset + 11]));
	vec4 row_3 = vec4(uintBitsToFloat(local_transfer.values[transfer_offset + 12]), uintBitsToFloat(local_transfer.values[transfer_offset + 13]), uintBitsToFloat(local_transfer.values[transfer_offset + 14]), uintBitsToFloat(local_transfer.values[transfer_offset + 15]));
	vec4 transformed = vec4(dot(row_0, value), dot(row_1, value), dot(row_2, value), dot(row_3, value));
	vec3 tint = unpackUnorm4x8(local_transfer.values[transfer_offset + 16]).rgb;
	float tint_component = channel == 0 ? tint.r : (channel == 1 ? tint.g : tint.b);
	return transformed * tint_component;
#elif defined(LOCAL_TRANSFER_LUMINANCE_FP16_TINT)
	int transfer_offset = index * 9;
	int row_offset = transfer_offset;
	vec4 row_0 = vec4(unpackHalf2x16(local_transfer.values[row_offset]), unpackHalf2x16(local_transfer.values[row_offset + 1]));
	vec4 row_1 = vec4(unpackHalf2x16(local_transfer.values[row_offset + 2]), unpackHalf2x16(local_transfer.values[row_offset + 3]));
	vec4 row_2 = vec4(unpackHalf2x16(local_transfer.values[row_offset + 4]), unpackHalf2x16(local_transfer.values[row_offset + 5]));
	vec4 row_3 = vec4(unpackHalf2x16(local_transfer.values[row_offset + 6]), unpackHalf2x16(local_transfer.values[row_offset + 7]));
	vec4 transformed = vec4(dot(row_0, value), dot(row_1, value), dot(row_2, value), dot(row_3, value));
	vec3 tint = unpackUnorm4x8(local_transfer.values[transfer_offset + 8]).rgb;
	float tint_component = channel == 0 ? tint.r : (channel == 1 ? tint.g : tint.b);
	return transformed * tint_component;
#else
	int row_offset = index * 12 + channel * 4;
	return vec4(
			dot(local_transfer.values[row_offset], value),
			dot(local_transfer.values[row_offset + 1], value),
			dot(local_transfer.values[row_offset + 2], value),
			dot(local_transfer.values[row_offset + 3], value));
#endif
}

vec4 world_sh_to_local(vec4 world_sh) {
	vec3 world_directional = world_sh.yzw;
	return vec4(
			world_sh.x,
			dot(params.xform_x.xyz, world_directional),
			dot(params.xform_y.xyz, world_directional),
			dot(params.xform_z.xyz, world_directional));
}

float triangle_solid_angle(vec3 a, vec3 b, vec3 c) {
	return 2.0 * atan(dot(a, cross(b, c)), 1.0 + dot(a, b) + dot(b, c) + dot(c, a));
}

vec3 spherical_edge_moment(vec3 a, vec3 b) {
	vec3 edge_cross = cross(a, b);
	float cross_length = length(edge_cross);
	if (cross_length <= 1e-12) {
		return vec3(0.0);
	}
	float edge_angle = atan(cross_length, dot(a, b));
	return edge_cross * (0.5 * edge_angle / cross_length);
}

vec4 encode_spherical_quad(vec3 direction_0, vec3 direction_1, vec3 direction_2, vec3 direction_3) {
	vec3 d0 = normalize(direction_0);
	vec3 d1 = normalize(direction_1);
	vec3 d2 = normalize(direction_2);
	vec3 d3 = normalize(direction_3);
	float solid_angle = triangle_solid_angle(d0, d1, d2) + triangle_solid_angle(d0, d2, d3);
	vec3 first_moment = spherical_edge_moment(d0, d1) + spherical_edge_moment(d1, d2) + spherical_edge_moment(d2, d3) + spherical_edge_moment(d3, d0);
	if (solid_angle < 0.0) {
		solid_angle = -solid_angle;
		first_moment = -first_moment;
	}
	return vec4(SH_Y00 * solid_angle, SH_Y1 * first_moment);
}

void add_light(inout vec4 r, inout vec4 g, inout vec4 b, vec3 local_direction, vec3 color, float energy) {
	if (energy <= 0.0 || dot(local_direction, local_direction) < 1e-12) {
		return;
	}
	vec4 encoded = encode_direction(local_direction, energy);
	r += encoded * color.r;
	g += encoded * color.g;
	b += encoded * color.b;
}

float sample_directional_shadow(vec3 world_position) {
	if (params.directional_shadow_enabled == 0) {
		return 1.0;
	}
	mat4 view_proj = mat4(shadow_matrix.columns[0], shadow_matrix.columns[1], shadow_matrix.columns[2], shadow_matrix.columns[3]);
	vec4 clip = view_proj * vec4(world_position, 1.0);
	if (abs(clip.w) < 1e-12) {
		return 1.0;
	}
	vec3 ndc = clip.xyz / clip.w;
	vec2 uv = ndc.xy * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
		return 1.0;
	}
	ivec2 shadow_size = textureSize(shadow_map, 0);
	vec2 texel_position = uv * vec2(shadow_size) - vec2(0.5);
	ivec2 base = ivec2(floor(texel_position));
	vec2 fraction = fract(texel_position);
	float vis = 0.0;
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			ivec2 sample_position = clamp(base + ivec2(x, y), ivec2(0), shadow_size - ivec2(1));
			float occluder = texelFetch(shadow_map, sample_position, 0).r;
			float weight_x = x == 0 ? 1.0 - fraction.x : fraction.x;
			float weight_y = y == 0 ? 1.0 - fraction.y : fraction.y;
			vis += ((ndc.z + params.directional_shadow_bias) >= occluder ? 1.0 : 0.0) * weight_x * weight_y;
		}
	}
	return vis;
}

float local_light_attenuation(float distance, float range, float decay) {
	float normalized_distance = distance / range;
	normalized_distance *= normalized_distance;
	normalized_distance *= normalized_distance;
	float window = max(1.0 - normalized_distance, 0.0);
	window *= window;
	return window * pow(max(distance, 0.0001), -decay);
}

float sample_omni_shadow(vec3 world_position, vec3 light_position, vec3 axis_x, vec3 axis_y, vec3 axis_z, vec4 atlas_rect, vec2 hemisphere_offset, float range, float bias) {
	vec3 relative = world_position - light_position;
	vec3 light_local = vec3(dot(axis_x, relative), dot(axis_y, relative), dot(axis_z, relative));
	float distance = length(light_local);
	if (distance < 1e-12 || range <= 0.0 || atlas_rect.z <= 0.0 || atlas_rect.w <= 0.0) {
		return 1.0;
	}

	vec3 shadow_direction = light_local / distance;
	vec2 paraboloid = shadow_direction.xy / (1.0 + abs(shadow_direction.z));
	vec4 sample_rect = atlas_rect;
	vec2 cross_hemisphere_offset = hemisphere_offset;
	if (shadow_direction.z >= 0.0) {
		sample_rect.xy += hemisphere_offset;
		cross_hemisphere_offset *= -1.0;
	}

	float atlas_texel = 1.0 / float(max(params.positional_shadow_resolution, 1));
	sample_rect.xy += vec2(atlas_texel);
	sample_rect.zw -= vec2(atlas_texel * 2.0);
	vec2 offset_scale = vec2(atlas_texel * 2.0) / sample_rect.zw;
	vec2 offsets[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
	float probe_depth = 1.0 - (distance - bias) / range;
	float visibility = 0.0;
	for (int i = 0; i < 4; i++) {
		vec2 sample_coord = paraboloid + offsets[i] * offset_scale;
		float radius_squared = dot(sample_coord, sample_coord);
		bool crosses_hemisphere = radius_squared > 1.0;
		if (crosses_hemisphere) {
			float radius = sqrt(radius_squared);
			sample_coord *= 2.0 / radius - 1.0;
		}
		sample_coord = sample_coord * 0.5 + 0.5;
		sample_coord = sample_rect.xy + sample_coord * sample_rect.zw;
		if (crosses_hemisphere) {
			sample_coord += cross_hemisphere_offset;
		}
		float occluder_depth = textureLod(positional_shadow_atlas, sample_coord, 0.0).r;
		visibility += probe_depth >= occluder_depth ? 1.0 : 0.0;
	}
	return visibility * 0.25;
}

float sample_spot_shadow(vec3 world_position, vec3 light_position, vec3 axis_x, vec3 axis_y, vec3 light_direction, vec4 atlas_rect, float range, float cone_limit, float bias, float pcf_scale) {
	vec3 relative = world_position - light_position;
	float axial_distance = dot(light_direction, relative);
	float z_near = min(0.025, range);
	if (axial_distance <= z_near || axial_distance >= range || atlas_rect.z <= 0.0 || atlas_rect.w <= 0.0) {
		return 1.0;
	}
	float tan_angle = sqrt(max(1.0 - cone_limit * cone_limit, 0.0)) / cone_limit;
	vec2 uv = vec2(dot(axis_x, relative), dot(axis_y, relative)) / (axial_distance * tan_angle) * 0.5 + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
		return 1.0;
	}
	float atlas_texel = 1.0 / float(max(params.positional_shadow_resolution, 1));
	vec4 sample_rect = atlas_rect;
	sample_rect.xy += vec2(atlas_texel);
	sample_rect.zw -= vec2(atlas_texel * 2.0);
	vec2 atlas_uv = sample_rect.xy + uv * sample_rect.zw;
	vec2 clamp_max = sample_rect.xy + sample_rect.zw;
	vec2 offsets[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
	float receiver_depth = z_near * (range - axial_distance) / (axial_distance * (range - z_near)) + bias / axial_distance;
	float visibility = 0.0;
	for (int i = 0; i < 4; i++) {
		vec2 sample_uv = clamp(atlas_uv + offsets[i] * atlas_texel * max(pcf_scale, 1.0), sample_rect.xy, clamp_max);
		float occluder_depth = textureLod(positional_shadow_atlas, sample_uv, 0.0).r;
		visibility += receiver_depth >= occluder_depth ? 1.0 : 0.0;
	}
	return visibility * 0.25;
}

vec2 area_shadow_uv(vec3 light_local, vec4 atlas_rect) {
	vec3 direction = normalize(light_local);
	vec2 paraboloid = direction.xy / (1.0 + abs(direction.z));
	return atlas_rect.xy + (paraboloid * 0.5 + 0.5) * atlas_rect.zw;
}

float sample_area_shadow(vec3 world_position, vec3 light_position, vec3 area_width, vec3 area_height, vec3 light_direction, vec4 atlas_rect, float range, float bias, float soft_size, float blur) {
	float width_length = length(area_width);
	float height_length = length(area_height);
	float center_range = range + 0.5 * length(vec2(width_length, height_length));
	if (center_range <= 0.0 || width_length <= 0.0 || height_length <= 0.0 || atlas_rect.z <= 0.0 || atlas_rect.w <= 0.0) {
		return 1.0;
	}
	vec3 relative = world_position - light_position;
	vec3 light_local = vec3(dot(relative, area_width / width_length), dot(relative, area_height / height_length), dot(relative, -normalize(light_direction)));
	float shadow_length = length(light_local);
	if (shadow_length < 1e-12) {
		return 1.0;
	}
	float atlas_texel = 1.0 / float(max(params.positional_shadow_resolution, 1));
	vec4 sample_rect = atlas_rect;
	sample_rect.xy += vec2(atlas_texel);
	sample_rect.zw -= vec2(atlas_texel * 2.0);
	float receiver_depth = 1.0 - shadow_length / center_range;
	vec2 kernel[AREA_SHADOW_SAMPLE_COUNT] = vec2[](
			vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
			vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
			vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
			vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
			vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
			vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
			vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
			vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));
	vec3 shadow_direction = light_local / shadow_length;
	vec3 helper = abs(shadow_direction.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(helper, shadow_direction));
	vec3 bitangent = normalize(cross(tangent, shadow_direction));
	if (soft_size > 0.0 && blur > 0.0) {
		vec3 blocker_tangent = tangent * soft_size * blur;
		vec3 blocker_bitangent = bitangent * soft_size * blur;
		float blocker_count = 0.0;
		float blocker_depth = 0.0;
		for (int i = 0; i < AREA_SHADOW_SAMPLE_COUNT; i++) {
			vec3 sample_position = light_local + blocker_tangent * kernel[i].x + blocker_bitangent * kernel[i].y;
			float depth = textureLod(positional_shadow_atlas, area_shadow_uv(sample_position, sample_rect), 0.0).r;
			if (depth > receiver_depth) {
				blocker_depth += depth;
				blocker_count += 1.0;
			}
		}
		if (blocker_count == 0.0) {
			return 1.0;
		}
		blocker_depth /= blocker_count;
		float penumbra = (blocker_depth - receiver_depth) / max(1.0 - blocker_depth, 1e-5);
		vec3 filter_tangent = blocker_tangent * penumbra;
		vec3 filter_bitangent = blocker_bitangent * penumbra;
		float compare_depth = receiver_depth + bias / center_range;
		float visibility = 0.0;
		for (int i = 0; i < AREA_SHADOW_SAMPLE_COUNT; i++) {
			vec3 sample_position = light_local + filter_tangent * kernel[i].x + filter_bitangent * kernel[i].y;
			float depth = textureLod(positional_shadow_atlas, area_shadow_uv(sample_position, sample_rect), 0.0).r;
			visibility += compare_depth >= depth ? 1.0 : 0.0;
		}
		return visibility / float(AREA_SHADOW_SAMPLE_COUNT);
	}
	vec2 offsets[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
	vec2 offset_scale = vec2(atlas_texel * 2.0 * max(blur, 1.0)) / sample_rect.zw;
	vec2 projected = shadow_direction.xy / (1.0 + abs(shadow_direction.z));
	float compare_depth = receiver_depth + bias / center_range;
	float visibility = 0.0;
	for (int i = 0; i < 4; i++) {
		vec2 uv = sample_rect.xy + ((projected + offsets[i] * offset_scale) * 0.5 + 0.5) * sample_rect.zw;
		float depth = textureLod(positional_shadow_atlas, uv, 0.0).r;
		visibility += compare_depth >= depth ? 1.0 : 0.0;
	}
	return visibility * 0.25;
}

void main() {
	int dispatch_index = int(gl_GlobalInvocationID.x);
	if (dispatch_index >= params.dispatch_probe_count) {
		return;
	}
	int index = params.probe_offset + dispatch_index;
	if (index >= params.probe_count) {
		return;
	}

	int out_index = index * 3;
	if (inside_solid.values[index] != 0) {
		injection.values[out_index] = vec4(0.0);
		injection.values[out_index + 1] = vec4(0.0);
		injection.values[out_index + 2] = vec4(0.0);
		shadow_visibility.values[index] = 0.0;
		environment_injection.values[out_index] = vec4(0.0);
		environment_injection.values[out_index + 1] = vec4(0.0);
		environment_injection.values[out_index + 2] = vec4(0.0);
		return;
	}

	vec3 spacing = params.size / vec3(params.resolution - ivec3(1));
	vec3 local_position = vec3(probe_position(index)) * spacing - params.size * 0.5;
	vec3 world_position = xform_point(local_position);

	vec4 acc_r = vec4(0.0);
	vec4 acc_g = vec4(0.0);
	vec4 acc_b = vec4(0.0);
	float visibility = 1.0;
	vec4 sky_visibility = global_visibility.values[index];
	environment_injection.values[out_index] = triple_product(world_sh_to_local(environment_sh.values[0]), sky_visibility);
	environment_injection.values[out_index + 1] = triple_product(world_sh_to_local(environment_sh.values[1]), sky_visibility);
	environment_injection.values[out_index + 2] = triple_product(world_sh_to_local(environment_sh.values[2]), sky_visibility);

	for (int light = 0; light < params.light_count; light++) {
		int base = light * 9;
		vec4 packed = analytic_lights.values[base];
		int type = int(packed.x);
		float energy = packed.y;
		float range = packed.z;
		float cone_limit = packed.w;
		vec3 color = analytic_lights.values[base + 1].xyz;
		float shadow_flag = analytic_lights.values[base + 1].w;
		vec4 vector_and_attenuation = analytic_lights.values[base + 2];
		vec4 direction_and_bias = analytic_lights.values[base + 3];
		vec3 vector = vector_and_attenuation.xyz;
		float attenuation_decay = vector_and_attenuation.w;
		vec3 spot_direction = direction_and_bias.xyz;
		float shadow_bias = direction_and_bias.w;
		vec3 shadow_axis_x = analytic_lights.values[base + 4].xyz;
		vec3 shadow_axis_y = analytic_lights.values[base + 5].xyz;
		vec3 shadow_axis_z = analytic_lights.values[base + 6].xyz;
		vec4 shadow_rect = analytic_lights.values[base + 7];
		vec4 shadow_options = analytic_lights.values[base + 8];

		if (type == LIGHT_DIRECTIONAL) {
			float shadow = 1.0;
			if (shadow_flag > 0.5) {
				shadow = sample_directional_shadow(world_position);
				visibility = min(visibility, shadow);
			}
			// Godot directional energy is diffuse radiance. The SH encoder uses
			// TAU, so halve it to inject the equivalent PI-scaled irradiance.
			add_light(acc_r, acc_g, acc_b, to_local_dir(vector), color, energy * shadow * 0.5);
		} else if (type == LIGHT_OMNI) {
			vec3 to_light = vector - world_position;
			float distance = length(to_light);
			if (distance >= range || distance < 1e-12) {
				continue;
			}
			float attenuation = local_light_attenuation(distance, range, attenuation_decay);
			float shadow = 1.0;
			if (shadow_flag > 0.5) {
				shadow = sample_omni_shadow(world_position, vector, shadow_axis_x, shadow_axis_y, shadow_axis_z, shadow_rect, shadow_options.xy, range, shadow_bias);
				shadow = mix(1.0, shadow, shadow_options.z);
				visibility = min(visibility, shadow);
			}
			add_light(acc_r, acc_g, acc_b, to_local_dir(to_light), color, energy * attenuation * shadow * 0.5);
		} else if (type == LIGHT_SPOT) {
			vec3 light_to_probe = world_position - vector;
			float distance = length(light_to_probe);
			if (distance >= range || distance < 1e-12) {
				continue;
			}
			vec3 light_to_probe_dir = light_to_probe / distance;
			float cone_cosine = max(dot(normalize(spot_direction), light_to_probe_dir), cone_limit);
			float spot_rim = max(1e-4, (1.0 - cone_cosine) / (1.0 - cone_limit));
			float cone_attenuation = 1.0 - pow(spot_rim, direction_and_bias.w);
			float attenuation = local_light_attenuation(distance, range, attenuation_decay) * cone_attenuation;
			float shadow = 1.0;
			if (shadow_flag > 0.5) {
				shadow = sample_spot_shadow(world_position, vector, shadow_axis_x, shadow_axis_y, normalize(spot_direction), shadow_rect, range, cone_limit, analytic_lights.values[base + 4].w, analytic_lights.values[base + 5].w);
				shadow = mix(1.0, shadow, shadow_options.z);
				visibility = min(visibility, shadow);
			}
			add_light(acc_r, acc_g, acc_b, to_local_dir(-light_to_probe), color, energy * attenuation * shadow * 0.5);
		} else if (type == LIGHT_AREA) {
			vec3 area_width = shadow_axis_x;
			vec3 area_height = shadow_axis_y;
			float width_length = length(area_width);
			float height_length = length(area_height);
			float area = width_length * height_length;
			if (area <= 1e-12 || range <= 0.0 || dot(spot_direction, spot_direction) < 1e-12) {
				continue;
			}
			vec3 area_direction = normalize(spot_direction);
			vec3 light_to_probe = world_position - vector;
			if (dot(area_direction, light_to_probe) <= 0.0) {
				continue;
			}
			vec3 width_direction = area_width / width_length;
			vec3 height_direction = area_height / height_length;
			float closest_x = clamp(dot(light_to_probe, width_direction), -width_length * 0.5, width_length * 0.5);
			float closest_y = clamp(dot(light_to_probe, height_direction), -height_length * 0.5, height_length * 0.5);
			vec3 closest_point = vector + width_direction * closest_x + height_direction * closest_y;
			float closest_distance = distance(world_position, closest_point);
			if (closest_distance >= range) {
				continue;
			}
			float attenuation = local_light_attenuation(closest_distance, range, attenuation_decay) * closest_distance * closest_distance;
			float shadow = 1.0;
			if (shadow_flag > 0.5) {
				shadow = sample_area_shadow(world_position, vector, area_width, area_height, area_direction, shadow_rect, range, shadow_bias, analytic_lights.values[base + 4].w, analytic_lights.values[base + 5].w);
				shadow = mix(1.0, shadow, shadow_options.z);
				visibility = min(visibility, shadow);
			}
			float energy_scale = cone_limit > 0.5 ? 1.0 / area : 1.0;
			vec4 encoded = encode_spherical_quad(
					vector - area_width * 0.5 - area_height * 0.5 - world_position,
					vector + area_width * 0.5 - area_height * 0.5 - world_position,
					vector + area_width * 0.5 + area_height * 0.5 - world_position,
					vector - area_width * 0.5 + area_height * 0.5 - world_position) *
					(energy * attenuation * energy_scale * shadow * 0.5 * SH_TAU);
			encoded.yzw = to_local_dir(encoded.yzw);
			acc_r += encoded * color.r;
			acc_g += encoded * color.g;
			acc_b += encoded * color.b;
		}
	}

	vec4 local = local_visibility.values[index];
	vec4 incoming_r = positive_product(mesh_light.values[out_index], local) + triple_product(acc_r, local) + environment_injection.values[out_index];
	vec4 incoming_g = positive_product(mesh_light.values[out_index + 1], local) + triple_product(acc_g, local) + environment_injection.values[out_index + 1];
	vec4 incoming_b = positive_product(mesh_light.values[out_index + 2], local) + triple_product(acc_b, local) + environment_injection.values[out_index + 2];
	injection.values[out_index] = transform_transfer(index, 0, incoming_r);
	injection.values[out_index + 1] = transform_transfer(index, 1, incoming_g);
	injection.values[out_index + 2] = transform_transfer(index, 2, incoming_b);
	shadow_visibility.values[index] = visibility;
}
