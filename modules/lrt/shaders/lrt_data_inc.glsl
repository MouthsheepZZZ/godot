struct LRTVolume {
	mat4 to_local;
	uvec4 grid_geometry;
	vec4 size_spacing;
	vec4 display;
	uvec4 history;
};
layout(set = 1, binding = 39, std430) restrict readonly buffer LRTMetadata {
	uint count;
	uint padding[3];
	vec4 sky[3];
	LRTVolume volumes[];
}
lrt;
layout(set = 1, binding = 40, std430) restrict readonly buffer LRTSamples {
	uint words[];
}
lrt_samples;
