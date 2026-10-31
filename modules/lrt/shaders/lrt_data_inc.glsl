struct LRTVolume {
	mat4 to_local;
	uvec4 grid_geometry;
	vec4 size_spacing;
	vec4 display;
	uvec4 history;
	uvec4 receiver; // Full group ObjectID, public-group flag, padding.
};
#ifndef LRT_DATA_SET
#define LRT_DATA_SET 1
#define LRT_METADATA_BINDING 39
#define LRT_SAMPLES_BINDING 40
#endif
layout(set = LRT_DATA_SET, binding = LRT_METADATA_BINDING, std430) restrict readonly buffer LRTMetadata {
	uint count;
	uint profile_count;
	uint padding[2];
	vec4 sky[3];
	LRTVolume volumes[];
}
lrt;
#ifdef LRT_SAMPLES_WRITABLE
layout(set = LRT_DATA_SET, binding = LRT_SAMPLES_BINDING, std430) restrict buffer LRTSamples {
#else
layout(set = LRT_DATA_SET, binding = LRT_SAMPLES_BINDING, std430) restrict readonly buffer LRTSamples {
#endif
	uint words[];
}
lrt_samples;
