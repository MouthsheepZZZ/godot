// Native Forward+ LRT diffuse receiver. Expensive local-field reconstruction is executed
// by the quarter-pixel screen gather; Basepass only performs edge-aware reconstruction.

#define INSTANCE_FLAGS_USE_LRT (1 << 0)

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

#ifdef USE_MULTIVIEW
layout(set = 1, binding = 49) uniform texture2DArray lrt_screen_lighting;
layout(set = 1, binding = 50) uniform texture2DArray lrt_screen_geometry;

vec4 lrt_screen_fetch(texture2DArray field, ivec2 coord) {
	return texelFetch(sampler2DArray(field, SAMPLER_NEAREST_CLAMP), ivec3(coord, ViewIndex), 0);
}

ivec2 lrt_screen_size() {
	return textureSize(sampler2DArray(lrt_screen_lighting, SAMPLER_NEAREST_CLAMP), 0).xy;
}
#else
layout(set = 1, binding = 49) uniform texture2D lrt_screen_lighting;
layout(set = 1, binding = 50) uniform texture2D lrt_screen_geometry;

vec4 lrt_screen_fetch(texture2D field, ivec2 coord) {
	return texelFetch(sampler2D(field, SAMPLER_NEAREST_CLAMP), coord, 0);
}

ivec2 lrt_screen_size() {
	return textureSize(sampler2D(lrt_screen_lighting, SAMPLER_NEAREST_CLAMP), 0);
}
#endif

bool lrt_sample_screen(vec2 fragment_coord, vec3 view_position, vec3 view_normal, vec3 world_position,
		out vec3 ambient_light, out float blend_weight) {
	ambient_light = vec3(0.0);
	blend_weight = 0.0;
	if (lrt.data.volume_min.w < 0.5) {
		return false;
	}
	vec3 position = (lrt.data.world_to_volume * vec4(world_position, 1.0)).xyz;
	if (any(lessThan(position, lrt.data.volume_min.xyz)) || any(greaterThan(position, lrt.data.volume_max.xyz))) {
		return false;
	}
	vec3 face_distance = min(position - lrt.data.volume_min.xyz, lrt.data.volume_max.xyz - position);
	float boundary_distance = min(face_distance.x, min(face_distance.y, face_distance.z));
	blend_weight = lrt.data.atlas_flags.w > 0.0 ? clamp(boundary_distance / lrt.data.atlas_flags.w, 0.0, 1.0) : 1.0;
	if (blend_weight <= 0.0) {
		return false;
	}
	ivec2 gather_size = lrt_screen_size();
	vec2 gather_position = fragment_coord * 0.5 - vec2(0.75);
	ivec2 gather_base = ivec2(floor(gather_position));
	vec2 gather_fraction = fract(gather_position);
	vec3 normal = normalize(view_normal);
	float total_weight = 0.0;
	float nearest_score = 1e30;
	vec4 nearest_lighting = vec4(0.0);
	for (int index = 0; index < 4; index++) {
		ivec2 corner = ivec2(index & 1, (index >> 1) & 1);
		ivec2 coord = clamp(gather_base + corner, ivec2(0), gather_size - ivec2(1));
		vec4 lighting = lrt_screen_fetch(lrt_screen_lighting, coord);
		if (lighting.a <= 0.0) {
			continue;
		}
		vec4 geometry = lrt_screen_fetch(lrt_screen_geometry, coord);
		float depth_scale = max(abs(view_position.z) * 0.02, 0.02);
		float depth_error = abs(geometry.w - view_position.z) / depth_scale;
		float normal_error = 1.0 - max(dot(normalize(geometry.xyz), normal), 0.0);
		float score = depth_error + normal_error * 4.0;
		if (score < nearest_score) {
			nearest_score = score;
			nearest_lighting = lighting;
		}
		vec2 linear_weight = mix(vec2(1.0) - gather_fraction, gather_fraction, vec2(corner));
		float weight = linear_weight.x * linear_weight.y * exp2(-depth_error * 4.0 - normal_error * 16.0);
		ambient_light += lighting.rgb * weight;
		total_weight += weight;
	}
	if (total_weight > 0.00001) {
		ambient_light /= total_weight;
		return true;
	}
	if (nearest_score < 8.0) {
		ambient_light = nearest_lighting.rgb;
		return true;
	}
	return false;
}
