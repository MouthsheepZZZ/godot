// Baked and local native GI override LRT. SDFGI remains the base ambient.
bool lrt_is_overridden_by_native_gi(uint instance, vec3 camera_position, vec3 camera_normal) {
	uint flags = instances.data[instance].flags;
	if ((flags & (INSTANCE_FLAGS_USE_LIGHTMAP | INSTANCE_FLAGS_USE_LIGHTMAP_CAPTURE)) != 0u) {
		return true;
	}
	if ((flags & INSTANCE_FLAGS_USE_VOXEL_GI) != 0u) {
		for (uint slot = 0u; slot < 2u; slot++) {
			uint index = (instances.data[instance].gi_offset >> (slot * 16u)) & 0xFFFFu;
			if (index == 0xFFFFu) {
				continue;
			}
			vec3 position = (voxel_gi_instances.data[index].xform * vec4(camera_position, 1.0)).xyz;
			vec3 normal = normalize(mat3(voxel_gi_instances.data[index].xform) * camera_normal);
			position += normal * voxel_gi_instances.data[index].normal_bias;
			if (all(greaterThan(position, vec3(0.0))) && all(lessThan(position, voxel_gi_instances.data[index].bounds))) {
				return true;
			}
		}
	}
	return false;
}
