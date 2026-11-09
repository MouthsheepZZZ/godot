// Reconstruct per-light visibility, never mix shadows from different lights or views.
float first_person_shadow(vec3 position, vec3 normal, uint light_type, uint light_index) {
#if defined(SHADOWS_DISABLED)
	return 1.0;
#else
	if (implementation_data.first_person.y <= 0.0 || implementation_data.first_person.z <= 0.0 ||
			!bool(instances.data[instance_index_interp].flags & INSTANCE_FLAGS_FIRST_PERSON) ||
			light_index >= first_person_shadow_map.header.x) {
		return 1.0;
	}
	uint slot = first_person_shadow_map.indices[light_index][light_type];
	if (slot == 0u) {
		return 1.0;
	}
	uint layer = (slot - 1u) * first_person_shadow_map.header.z;
#ifdef USE_MULTIVIEW
	layer += ViewIndex;
#endif
	SceneData scene_data = scene_data_block.data;
	vec4 clip = projection_matrix * vec4(position, 1.0);
	vec2 full_size = vec2(textureSize(multiviewSampler(first_person_depth, SAMPLER_NEAREST_CLAMP), 0).xy);
	vec2 pixel = (clip.xy / clip.w * 0.5 + 0.5) * full_size / float(first_person_shadow_map.header.w) - 0.5;
	vec2 weight = fract(pixel);
	ivec2 base = ivec2(floor(pixel));
	ivec2 size = textureSize(multiviewSampler(first_person_receivers, SAMPLER_NEAREST_CLAMP), 0).xy;
	float total = 0.0;
	float visibility = 0.0;
	// Plane distance preserves sloped surfaces while rejecting depth discontinuities.
	float footprint = max(abs(position.z) * float(first_person_shadow_map.header.w) /
					(abs(projection_matrix[1][1]) * full_size.y),
			1e-7);
	for (int y = 0; y < 2; y++) {
		for (int x = 0; x < 2; x++) {
			ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), size - 1);
			vec2 uv = (vec2(tap) + 0.5) / vec2(size);
			vec4 receiver = textureLod(multiviewSampler(first_person_receivers, SAMPLER_NEAREST_CLAMP), multiview_uv(uv), 0.0);
			if (receiver.w == 0.0) {
				continue;
			}
			vec3 tap_normal = oct_to_vec3(textureLod(multiviewSampler(first_person_normals, SAMPLER_NEAREST_CLAMP), multiview_uv(uv), 0.0).rg * 2.0 - 1.0);
			float plane_distance = abs(dot(position - receiver.xyz, tap_normal));
			float bilateral = exp2(-plane_distance / footprint) * pow(max(dot(normal, tap_normal), 0.0), 8.0);
			float spatial = (x == 0 ? 1.0 - weight.x : weight.x) * (y == 0 ? 1.0 - weight.y : weight.y);
			float w = spatial * bilateral;
			visibility += texelFetch(sampler2DArray(first_person_masks, SAMPLER_NEAREST_CLAMP), ivec3(tap, layer), 0).r * w;
			total += w;
		}
	}
	// No receiver in the reconstruction footprint means this surface has no cached shadow.
	return total > 0.0 ? visibility / total : 1.0;
#endif
}
#define FIRST_PERSON_SHADOW(position, normal, type, index) first_person_shadow(position, vec3(normal), type, index)
