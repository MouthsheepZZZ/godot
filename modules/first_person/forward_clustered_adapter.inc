// Kept outside the upstream renderer to keep render-list coupling in one place.
void RenderForwardClustered::_render_first_person_depth(const RenderDataRD *p_render_data, const RendererRD::MaterialStorage::Samplers &p_samplers, uint32_t p_uniform_buffer_index) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null()) {
		return;
	}
	if (p_render_data->scene_data->first_person.y <= 0.0f) {
		rb->clear_context(SNAME("first_person"));
		rb->clear_context(SNAME("first_person_shadow"));
		return;
	}

	RenderList &list = render_list[RENDER_LIST_FIRST_PERSON];
	list.clear();
	for (GeometryInstanceSurfaceDataCache *surface : render_list[RENDER_LIST_OPAQUE].elements) {
		if (surface->owner->flags_cache & FirstPerson::INSTANCE_FLAG) {
			list.add_element(surface);
		}
	}

	// A separate depth attachment isolates casters without duplicating materials or lighting.
	RID depth = rb->create_texture(SNAME("first_person"), SNAME("depth"), RD::DATA_FORMAT_D32_SFLOAT,
			RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT,
			RD::TEXTURE_SAMPLES_1);
	RID framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multiview(rb->get_view_count(), depth);
	_fill_instance_data(RENDER_LIST_FIRST_PERSON);
	RID uniforms = _setup_render_pass_uniform_set(RENDER_LIST_FIRST_PERSON, nullptr, rb->get_view_count() > 1, RID(), p_samplers, p_uniform_buffer_index);
	RenderListParameters params(list.elements.ptr(), list.element_info.ptr(), list.elements.size(),
			p_render_data->scene_data->cam_transform.basis.determinant() < 0, PASS_MODE_DEPTH, 0, true,
			false, uniforms, false, Vector2(), p_render_data->scene_data->lod_distance_multiplier,
			p_render_data->scene_data->screen_mesh_lod_threshold, rb->get_view_count());
	RD::get_singleton()->draw_command_begin_label("First Person Depth");
	_render_list_with_draw_list(&params, framebuffer, RD::DRAW_CLEAR_ALL);
	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::_render_first_person_shadows(const RenderDataRD *p_render_data, uint32_t p_uniform_buffer_index) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || p_render_data->scene_data->first_person.y <= 0.0f) {
		return;
	}
	AABB bounds;
	bool has_bounds = false;
	Transform3D to_camera = p_render_data->scene_data->cam_transform.affine_inverse();
	Vector4 fp = p_render_data->scene_data->first_person;
	to_camera = Transform3D(Basis::from_scale(Vector3(fp.x * fp.y, fp.x * fp.y, fp.y)), Vector3()) * to_camera;
	for (GeometryInstanceSurfaceDataCache *surface : render_list[RENDER_LIST_FIRST_PERSON].elements) {
		AABB surface_bounds = to_camera.xform(surface->owner->transformed_aabb);
		bounds = has_bounds ? bounds.merge(surface_bounds) : surface_bounds;
		has_bounds = true;
	}
	if (!has_bounds) {
		rb->clear_context(SNAME("first_person_shadow"));
		return;
	}
	RD::get_singleton()->draw_command_begin_label("First Person Shadows");
	first_person_shadows->render(rb, p_render_data->scene_data, scene_state.uniform_buffers[p_uniform_buffer_index], p_render_data->directional_light_count, bounds);
	RD::get_singleton()->draw_command_end_label();
}
