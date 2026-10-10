#[compute]

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform TemporalData {
	mat4 inv_projection;
	mat4 current_to_previous_view;
	mat4 previous_projection;
	vec4 settings; // current frame weight, history valid, unused
} temporal;

layout(rgba16f, set = 0, binding = 1) uniform restrict image2D lighting;
layout(rgba16f, set = 0, binding = 2) uniform restrict readonly image2D geometry;
layout(set = 0, binding = 3) uniform texture2D depth_buffer;
layout(set = 0, binding = 4) uniform texture2D history_lighting;
layout(set = 0, binding = 5) uniform texture2D history_geometry;
layout(set = 0, binding = 6) uniform sampler nearest_sampler;

void main() {
	ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(lighting);
	if (any(greaterThanEqual(coord, size))) {
		return;
	}
	vec4 current = imageLoad(lighting, coord);
	vec4 surface = imageLoad(geometry, coord);
	if (temporal.settings.y < 0.5 || current.a <= 0.0 || dot(surface.xyz, surface.xyz) < 0.5) {
		return;
	}
	float depth = texelFetch(sampler2D(depth_buffer, nearest_sampler), coord, 0).r;
	vec2 ndc = 2.0 * (vec2(coord) + 0.5) / vec2(size) - 1.0;
	vec4 view_position = temporal.inv_projection * vec4(ndc, depth, 1.0);
	view_position /= view_position.w;
	vec4 previous_position = temporal.current_to_previous_view * view_position;
	vec4 previous_clip = temporal.previous_projection * previous_position;
	if (previous_clip.w <= 0.0) {
		return;
	}
	vec2 previous_pixel = (previous_clip.xy / previous_clip.w * 0.5 + 0.5) * vec2(size) - 0.5;
	ivec2 low = ivec2(floor(previous_pixel));
	vec2 fraction = fract(previous_pixel);
	vec3 previous_normal = normalize(mat3(temporal.current_to_previous_view) * surface.xyz);
	vec3 history = vec3(0.0);
	float total = 0.0;
	// Validate each bilinear tap before mixing across silhouettes or depth edges.
	for (int index = 0; index < 4; index++) {
		ivec2 corner = ivec2(index & 1, (index >> 1) & 1);
		ivec2 sample_coord = low + corner;
		if (any(lessThan(sample_coord, ivec2(0))) || any(greaterThanEqual(sample_coord, size))) {
			continue;
		}
		vec4 previous_surface = texelFetch(sampler2D(history_geometry, nearest_sampler), sample_coord, 0);
		if (dot(previous_surface.xyz, previous_normal) < 0.98 ||
				abs(previous_surface.w - previous_position.z) > 0.01 + abs(previous_position.z) * 0.002) {
			continue;
		}
		vec4 previous_light = texelFetch(sampler2D(history_lighting, nearest_sampler), sample_coord, 0);
		if (previous_light.a <= 0.0) {
			continue;
		}
		vec2 corner_weight = mix(vec2(1.0) - fraction, fraction, vec2(corner));
		float weight = corner_weight.x * corner_weight.y;
		history += previous_light.rgb / previous_light.a * weight;
		total += weight;
	}
	if (total <= 0.0) {
		return;
	}
	// Keep volume coverage current; only the surface's GI changes over time.
	vec3 filtered = mix(history / total, current.rgb / current.a, temporal.settings.x);
	imageStore(lighting, coord, vec4(filtered * current.a, current.a));
}
