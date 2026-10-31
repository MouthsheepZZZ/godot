int lrt_instance_index() {
#ifdef MODE_RENDER_MATERIAL
	return lrt_material_bake.control.x != 0.0 ? floatBitsToInt(lrt_material_bake.control.y) : INSTANCE_INDEX;
#else
	return INSTANCE_INDEX;
#endif
}

int lrt_vertex_index() {
#ifdef MODE_RENDER_MATERIAL
	return lrt_material_bake.control.x != 0.0 ? floatBitsToInt(lrt_material_bake.vertices[gl_VertexIndex].atlas_id.z) : gl_VertexIndex;
#else
	return gl_VertexIndex;
#endif
}
