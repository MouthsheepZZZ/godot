#[compute]

#version 450

#VERSION_DEFINES

#define SH_Y00 0.28209479177387814
#define SH_Y1 0.4886025119029199
#define SH_TAU 6.283185307179586
#define LIGHT_DIRECTIONAL 1
#define LIGHT_OMNI 2
#define LIGHT_SPOT 3

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
	vec2 texel = vec2(1.0 / float(max(params.directional_shadow_resolution, 1)));
	vec2 offsets[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
	float vis = 0.0;
	for (int i = 0; i < 4; i++) {
		float occluder = textureLod(shadow_map, uv + offsets[i] * texel, 0.0).r;
		vis += (ndc.z + params.directional_shadow_bias) >= occluder ? 1.0 : 0.0;
	}
	return vis * 0.25;
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

void main() {
	int index = int(gl_GlobalInvocationID.x);
	if (index >= params.probe_count) {
		return;
	}

	int out_index = index * 3;
	if (inside_solid.values[index] != 0) {
		injection.values[out_index] = vec4(0.0);
		injection.values[out_index + 1] = vec4(0.0);
		injection.values[out_index + 2] = vec4(0.0);
		shadow_visibility.values[index] = 0.0;
		return;
	}

	vec3 spacing = params.size / vec3(params.resolution - ivec3(1));
	vec3 local_position = vec3(probe_position(index)) * spacing - params.size * 0.5;
	vec3 world_position = xform_point(local_position);

	vec4 acc_r = vec4(0.0);
	vec4 acc_g = vec4(0.0);
	vec4 acc_b = vec4(0.0);
	float visibility = 1.0;

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
			float cone_cosine = dot(normalize(spot_direction), light_to_probe_dir);
			if (cone_cosine <= cone_limit) {
				continue;
			}
			float range_attenuation = pow(1.0 - distance / range, 2.0);
			float cone_attenuation = pow((cone_cosine - cone_limit) / (1.0 - cone_limit), 2.0);
			add_light(acc_r, acc_g, acc_b, to_local_dir(-light_to_probe), color, energy * range_attenuation * cone_attenuation);
		}
	}

	injection.values[out_index] = acc_r;
	injection.values[out_index + 1] = acc_g;
	injection.values[out_index + 2] = acc_b;
	shadow_visibility.values[index] = visibility;
}
