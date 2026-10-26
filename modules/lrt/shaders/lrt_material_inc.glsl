// Material baking keeps UV/UV2/custom attributes intact and supplies a separate
// triangle atlas. Records also preserve original VERTEX_ID after deindexing.
struct LRTBakeVertex {
	vec4 atlas_id;
	vec4 position;
};
layout(set = 1, binding = 43, std430) buffer LRTMaterialBake {
	vec4 control;
	vec4 instance_color;
	vec4 instance_custom;
	LRTBakeVertex vertices[];
}
lrt_material_bake;
