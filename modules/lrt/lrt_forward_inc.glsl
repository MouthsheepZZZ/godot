// Opaque Forward+ receivers sample the two premultiplied low-resolution outputs.
layout(set = 1, binding = 39, std140) uniform LRTForwardData {
	ivec4 flags; // enabled, lighting debug, reserved, reserved
} lrt;

bool lrt_is_enabled() {
	return lrt.flags.x != 0;
}

bool lrt_is_lighting_debug() {
	return lrt.flags.y != 0;
}

vec3 lrt_compose_indirect(vec3 native_indirect, vec3 native_sky, vec3 gi, float coverage, float sky_visibility, float ao, vec3 albedo) {
	return native_indirect * (1.0 - coverage) + (gi + native_sky * sky_visibility) * ao * albedo;
}

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
	// The field is unexposed; native GI and sky are already exposure-normalized.
	gi = lighting.rgb * scene_data_block.data.emissive_exposure_normalization;
	coverage = lighting.a;
	sky_visibility = lrt_screen_fetch(lrt_screen_sky_visibility, screen_uv).r;
	return coverage > 0.0;
}

vec3 lrt_debug_lighting(vec3 lighting, float coverage) {
	return lighting + vec3(0.0, 0.3, 0.5) * (4.0 * coverage * (1.0 - coverage));
}
