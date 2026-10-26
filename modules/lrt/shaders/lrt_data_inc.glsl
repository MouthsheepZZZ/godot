struct LRTVolume {
	mat4 to_local;
	uvec4 grid_geometry;
	vec4 size_spacing;
	vec4 display;
	uvec4 history;
};
#ifndef LRT_DATA_SET
#define LRT_DATA_SET 1
#define LRT_METADATA_BINDING 39
#define LRT_SAMPLES_BINDING 40
#endif
layout(set = LRT_DATA_SET, binding = LRT_METADATA_BINDING, std430) restrict readonly buffer LRTMetadata {
	uint count;
	uint padding[3];
	vec4 sky[3];
	LRTVolume volumes[];
}
lrt;
layout(set = LRT_DATA_SET, binding = LRT_SAMPLES_BINDING, std430) restrict readonly buffer LRTSamples {
	uint words[];
}
lrt_samples;
