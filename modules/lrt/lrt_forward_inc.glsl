// Native Forward+ LRT diffuse receiver. Expensive local-field reconstruction is executed
// by screen gather; transparent surfaces sample their own positions.

struct LRTData {
	mat4 world_to_volume;
	vec4 volume_min;
	vec4 volume_max;
	vec4 grid_min_spacing;
	ivec4 grid_size_mode;
	vec4 atlas_flags;
};

layout(set = 1, binding = 39, std140) uniform LRTDataBlock {
	LRTData data;
}
lrt;

layout(set = 1, binding = 51, std430) restrict readonly buffer LRTVolumes {
	LRTData data[];
} lrt_volumes;

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 49) uniform texture2DArray lrt_screen_lighting;

vec4 lrt_screen_fetch(texture2DArray field, vec2 uv) {
	return textureLod(sampler2DArray(field, SAMPLER_LINEAR_CLAMP), vec3(uv, float(ViewIndex)), 0.0);
}

ivec2 lrt_screen_size() {
	return textureSize(sampler2DArray(lrt_screen_lighting, SAMPLER_NEAREST_CLAMP), 0).xy;
}
#else
layout(set = 1, binding = 49) uniform texture2D lrt_screen_lighting;

vec4 lrt_screen_fetch(texture2D field, vec2 uv) {
	return textureLod(sampler2D(field, SAMPLER_LINEAR_CLAMP), uv, 0.0);
}

ivec2 lrt_screen_size() {
	return textureSize(sampler2D(lrt_screen_lighting, SAMPLER_NEAREST_CLAMP), 0);
}
#endif

layout(set = 1, binding = 52, std430) restrict readonly buffer LRTFields {
	uvec4 data[];
} lrt_fields;

vec4 lrt_fetch(LRTData data, int field, ivec3 cell) {
	int probe = cell.x + data.grid_size_mode.x * (cell.z + data.grid_size_mode.z * cell.y);
	int packed_field = field > 4 ? field - 1 : field;
	return uintBitsToFloat(lrt_fields.data[(data.grid_size_mode.w + probe) * 7 + packed_field]);
}

#include "lrt_sampling_inc.glsl"

bool lrt_sample_surface(vec2 screen_uv, vec3 world_position, vec3 world_normal, bool transparent,
		out vec3 ambient_light, out float blend_weight) {
	ambient_light = vec3(0.0);
	blend_weight = 0.0;
	if (lrt.data.volume_min.w < 0.5) {
		return false;
	}
	if (!transparent) {
		vec4 lighting = lrt_screen_fetch(lrt_screen_lighting, screen_uv);
		blend_weight = lighting.a;
		if (blend_weight > 0.0) {
			ambient_light = lighting.rgb / blend_weight;
			return true;
		}
		return false;
	}
	for (int index = 0; index < int(lrt.data.volume_min.w); index++) {
		vec3 lighting;
		float weight;
		lrt_sample_native(lrt_volumes.data[index], world_position, world_normal, lighting, weight);
		ambient_light = lighting * weight + ambient_light * (1.0 - weight);
		blend_weight = weight + blend_weight * (1.0 - weight);
	}
	if (blend_weight <= 0.0) {
		return false;
	}
	ambient_light /= blend_weight;
	return true;
}
