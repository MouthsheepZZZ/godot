// Opaque Forward+ receivers sample the two premultiplied low-resolution outputs.
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
} lrt;

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 49) uniform texture2DArray lrt_screen_lighting;
layout(set = 1, binding = 51) uniform texture2DArray lrt_screen_sky_visibility;

vec4 lrt_screen_fetch(texture2DArray field, vec2 uv) {
	return textureLod(sampler2DArray(field, SAMPLER_LINEAR_CLAMP), vec3(uv, float(ViewIndex)), 0.0);
}
#else
layout(set = 1, binding = 49) uniform texture2D lrt_screen_lighting;
layout(set = 1, binding = 51) uniform texture2D lrt_screen_sky_visibility;

vec4 lrt_screen_fetch(texture2D field, vec2 uv) {
	return textureLod(sampler2D(field, SAMPLER_LINEAR_CLAMP), uv, 0.0);
}
#endif

bool lrt_sample_surface(vec2 screen_uv, out vec3 gi, out float coverage, out float sky_visibility) {
	vec4 lighting = lrt_screen_fetch(lrt_screen_lighting, screen_uv);
	gi = lighting.rgb;
	coverage = lighting.a;
	sky_visibility = lrt_screen_fetch(lrt_screen_sky_visibility, screen_uv).r;
	return coverage > 0.0;
}

vec3 lrt_debug_lighting(vec3 lighting, float coverage) {
	return lighting + vec3(0.0, 0.3, 0.5) * (4.0 * coverage * (1.0 - coverage));
}
