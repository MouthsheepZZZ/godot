// Full-resolution DDA with conservative, halo-expanded 8x8 depth bounds.
float trace_shadow(vec3 position, vec3 normal, vec3 direction, float light_distance) {
	float ray_length = min(params.distance, light_distance);
	float thickness = params.thickness;
	float bias = thickness * 0.1;
	vec3 origin = position + normal * bias;
	mat4 projection = get_projection();
	mat4 inverse_projection = get_inverse_projection();
	if (origin.z >= -scene.data.z_near) {
		return 1.0;
	}
	if (direction.z > 0.0) {
		ray_length = min(ray_length, (-scene.data.z_near - origin.z) / direction.z);
	}
	vec3 endpoint = origin + direction * ray_length;
	vec4 start_clip = projection * vec4(origin, 1.0);
	vec4 end_clip = projection * vec4(endpoint, 1.0);
	vec2 size = vec2(textureSize(depth_texture, 0).xy);
	vec2 start_pixel = (start_clip.xy / start_clip.w * 0.5 + 0.5) * size;
	vec2 end_pixel = (end_clip.xy / end_clip.w * 0.5 + 0.5) * size;
	if (any(lessThan(start_pixel, vec2(0.0))) || any(greaterThanEqual(start_pixel, size))) {
		return 1.0;
	}

	// Clip the projected segment before choosing its pixel-sized DDA steps.
	vec2 delta_pixel = end_pixel - start_pixel;
	float end_fraction = 1.0;
	for (int axis = 0; axis < 2; axis++) {
		if (delta_pixel[axis] != 0.0) {
			float edge = delta_pixel[axis] > 0.0 ? size[axis] : 0.0;
			end_fraction = min(end_fraction, (edge - start_pixel[axis]) / delta_pixel[axis]);
		}
	}
	delta_pixel *= end_fraction;
	float start_k = 1.0 / start_clip.w;
	float end_k = mix(start_k, 1.0 / end_clip.w, end_fraction);
	float start_q = origin.z * start_k;
	float end_q = mix(start_q, endpoint.z / end_clip.w, end_fraction);
	int steps = max(1, int(ceil(max(abs(delta_pixel.x), abs(delta_pixel.y)))));
	float previous_z = origin.z;
	float occlusion = 0.0;
	vec2 pixel_step = delta_pixel / float(steps);
	int i = 0;
	while (i < steps) {
		vec2 pixel = start_pixel + pixel_step * (float(i) + 0.5);
		if (params.block_skipping != 0u) {
			ivec2 tile = clamp(ivec2(floor(pixel / 8.0)), ivec2(0), textureSize(block_texture, 0) - 1);
			vec2 edge = (vec2(tile) + step(vec2(0.0), pixel_step)) * 8.0;
			vec2 exits = vec2(1e20);
			for (int axis = 0; axis < 2; axis++) {
				if (pixel_step[axis] != 0.0) {
					exits[axis] = (edge[axis] - pixel[axis]) / pixel_step[axis];
				}
			}
			int advance = min(steps - i, max(1, int(ceil(min(min(exits.x, exits.y), float(steps - i))))));
			float exit_t = float(i + advance) / float(steps);
			float exit_z = mix(start_q, end_q, exit_t) / mix(start_k, end_k, exit_t);
			vec2 bounds = texelFetch(block_texture, tile, 0).rg;
			// Tiles include the complete 2x2 gather footprint, including a one-texel halo.
			// Only a disjoint depth interval is skipped; possible intersections stay exact.
			if (bounds.y <= min(previous_z, exit_z) + bias || bounds.x >= max(previous_z, exit_z) + thickness) {
				i += advance;
				previous_z = exit_z;
				continue;
			}
		}
		float t = float(i + 1) / float(steps);
		float ray_z = mix(start_q, end_q, t) / mix(start_k, end_k, t);
		float ray_min = min(previous_z, ray_z);
		float ray_max = max(previous_z, ray_z);
		previous_z = ray_z;
		i++;

		// Compare all four depths before filtering coverage. Interpolating depth itself
		// would invent occluders across silhouettes. Segment tests also cover thin hits
		// when a single projected pixel spans a large camera-space depth interval.
		vec4 depths = textureGather(depth_texture, pixel / size).wzxy;
		vec4 hit_z = (inverse_projection[2][2] * depths + inverse_projection[3][2]) /
				(inverse_projection[2][3] * depths + inverse_projection[3][3]);
		vec4 blocked = vec4(greaterThan(depths, vec4(0.0))) *
				vec4(greaterThan(hit_z, vec4(ray_min + bias))) *
				vec4(lessThan(hit_z, vec4(ray_max + thickness)));
		vec2 weight = fract(pixel - 0.5);
		float coverage = mix(mix(blocked.x, blocked.y, weight.x), mix(blocked.z, blocked.w, weight.x), weight.y);
		occlusion = max(occlusion, coverage);
		if (occlusion >= 1.0) {
			break;
		}
	}
	return 1.0 - occlusion;
}
