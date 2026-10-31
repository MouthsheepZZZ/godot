/**************************************************************************/
/*  lrt_render_adapter.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "lrt_render_adapter.h"

#ifdef RD_ENABLED
#include "lrt_renderer.h"

#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

namespace LRTRenderAdapter {

void append_uniforms(LocalVector<RD::Uniform> &r_uniforms, const RenderDataRD *p_render_data) {
	LRTRenderer *renderer = LRTRenderer::get_singleton();
	if (!renderer) {
		return;
	}
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 39, { renderer->get_draw_metadata() }));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 40, { renderer->get_draw_samples() }));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 41, { renderer->get_gather_texture(p_render_data, "lighting") }));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 42, { renderer->get_gather_texture(p_render_data, "guide") }));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 43, { renderer->get_material_bake_buffer() }));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 44, { renderer->get_gather_texture(p_render_data, "profile") }));
}

void render(RenderDataRD *p_render_data, const RID *p_normals) {
	LRTRenderer *renderer = LRTRenderer::get_singleton();
	if (!renderer) {
		return;
	}
	if (p_render_data->reflection_probe.is_null()) {
		renderer->finish_positional_shadows();
	}
	renderer->render(p_render_data);
	renderer->gather(p_render_data, p_normals);
}

void draw_debug(const RenderDataRD *p_render_data) {
	LRTRenderer *renderer = LRTRenderer::get_singleton();
	if (!renderer) {
		return;
	}
	RID debug = renderer->debug_render(p_render_data);
	if (debug.is_null()) {
		return;
	}
	const Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	RID source = rb->get_view_count() > 1 ? debug : rb->get_texture_slice("lrt_debug", "output", 0, 0);
	const bool overlay = RendererSceneRenderRD::get_singleton()->get_debug_draw_mode() == RSE::VIEWPORT_DEBUG_DRAW_LRT_TRUNKS;
	RID framebuffer = RendererRD::TextureStorage::get_singleton()->render_target_get_rd_framebuffer(rb->get_render_target());
	RendererRD::CopyEffects::get_singleton()->copy_to_fb_rect(source, framebuffer, Rect2i(Point2i(), rb->get_target_size()), false, false, false, false, RID(), rb->get_view_count() > 1, false, false, false, Rect2(), 1.0f, true, overlay);
}

} // namespace LRTRenderAdapter
#endif
