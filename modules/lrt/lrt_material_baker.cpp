/**************************************************************************/
/*  lrt_material_baker.cpp                                                */
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

#include "lrt_material_baker.h"

#include "lrt_geometry_binding.h"
#include "lrt_renderer.h"

#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/os/thread.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#endif

struct LRTMaterialBaker::Request {
	Ref<ArrayMesh> mesh;
	Ref<Material> material;
	Vector<Ref<Material>> passes;
	Vector<bool> opaque;
	bool classify = true;
	RID instance;
	Transform3D transform;
	Transform3D camera;
	Projection projection;
	bool orthogonal = false;
	uint32_t camera_layers = UINT32_MAX;
	Vector<uint8_t> vertices;
	Size2i size;
	Surface *output = nullptr;
	Error error = ERR_UNAVAILABLE;
};

namespace {
Color bilinear(const Ref<Image> &p_image, const Vector2 &p_uv) {
	const Vector2 pixel = p_uv * Vector2(p_image->get_width(), p_image->get_height()) - Vector2(0.5, 0.5);
	const Vector2i cell(Math::floor(pixel.x), Math::floor(pixel.y));
	const Vector2 fraction = pixel - Vector2(cell);
	const int x0 = CLAMP(cell.x, 0, p_image->get_width() - 1);
	const int x1 = CLAMP(cell.x + 1, 0, p_image->get_width() - 1);
	const int y0 = CLAMP(cell.y, 0, p_image->get_height() - 1);
	const int y1 = CLAMP(cell.y + 1, 0, p_image->get_height() - 1);
	return p_image->get_pixel(x0, y0).lerp(p_image->get_pixel(x1, y0), fraction.x).lerp(p_image->get_pixel(x0, y1).lerp(p_image->get_pixel(x1, y1), fraction.x), fraction.y);
}
} // namespace

void LRTMaterialBaker::Surface::sample(const Vector2 &p_uv, Color &r_albedo, Color &r_emission) const {
	r_albedo = bilinear(albedo, p_uv);
	r_albedo.a = 1;
	r_emission = has_emission ? bilinear(emission, p_uv) : Color(0, 0, 0);
}

Error LRTMaterialBaker::bake(GeometryInstance3D *p_instance, int p_surface, float p_texel_size, Surface &r_surface, int p_instance_index) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(p_texel_size <= 0 || !Math::is_finite(p_texel_size), ERR_INVALID_PARAMETER);
	const Ref<Mesh> source = LRTGeometryBinding::get_mesh(p_instance);
	Ref<SurfaceTool> tool;
	tool.instantiate();
	tool->create_from(source, p_surface);
	tool->deindex();
	Request request;
	request.material = LRTGeometryBinding::get_material(p_instance, p_surface);
	if (request.material.is_null()) {
		Ref<StandardMaterial3D> material;
		material.instantiate();
		request.material = material;
	}
	for (Ref<Material> material = request.material; material.is_valid(); material = material->get_next_pass()) {
		request.passes.push_back(material);
	}
	for (Ref<Material> material = p_instance->get_material_overlay(); material.is_valid(); material = material->get_next_pass()) {
		request.passes.push_back(material);
	}
	RenderingServer *server = RenderingServer::get_singleton();
	server->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_bake).bind(int64_t(&request)));
	server->sync();
	ERR_FAIL_COND_V_MSG(request.error != OK, request.error, "LRT material baking requires valid Forward+ spatial shaders.");
	request.material.unref();
	for (int pass = request.passes.size() - 1; pass >= 0; pass--) {
		if (!request.opaque[pass]) {
			continue;
		}
		Ref<Material> material = request.passes[pass]->duplicate();
		material->set_next_pass(request.material);
		request.material = material;
	}
	if (request.material.is_null()) {
		r_surface.transparent = true;
		return OK;
	}
	request.classify = false;
	request.error = ERR_UNAVAILABLE;
	tool->set_material(request.material);
	request.mesh = tool->commit();
	request.instance = p_instance->get_instance();
	request.transform = (p_instance->is_inside_tree() ? p_instance->get_global_transform() : p_instance->get_transform()) * LRTGeometryBinding::get_instance_transform(p_instance, p_instance_index);
	if (p_instance->is_inside_tree()) {
		Camera3D *camera = p_instance->get_viewport()->get_camera_3d();
		if (camera) {
			request.camera = camera->get_global_transform();
			request.camera_layers = camera->get_cull_mask();
			request.projection = camera->get_camera_projection();
			request.orthogonal = camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
		}
	}
	const Array arrays = request.mesh->surface_get_arrays(0);
	const Vector<Vector3> positions = arrays[Mesh::ARRAY_VERTEX];
	const Vector<int> indices = source->surface_get_arrays(p_surface)[Mesh::ARRAY_INDEX];
	ERR_FAIL_COND_V(positions.is_empty() || positions.size() % 3 != 0, ERR_INVALID_DATA);
	r_surface.atlas_uv.resize(positions.size());

	// Pack independent triangles by surface-space extent, at four texels per
	// eventual geometry sample cell. Original UV2 and custom channels are untouched.
	struct Tile {
		Vector2 corners[3];
		Vector2i size;
	};
	Vector<Tile> tiles;
	double area = 0;
	int largest = 1;
	for (int face = 0; face < positions.size(); face += 3) {
		int start = 0;
		for (int edge = 1; edge < 3; edge++) {
			if (positions[face + edge].distance_squared_to(positions[face + (edge + 1) % 3]) >
					positions[face + start].distance_squared_to(positions[face + (start + 1) % 3])) {
				start = edge;
			}
		}
		const int second = (start + 1) % 3;
		const int third = (start + 2) % 3;
		const Vector3 u = positions[face + second] - positions[face + start];
		const Vector3 v = positions[face + third] - positions[face + start];
		const real_t length = u.length();
		// Degenerate source triangles still need material vertex capture: the shader
		// may deform them. Give them a minimum tile; geometry filtering runs later.
		const real_t projected = length > 0 ? v.dot(u) / length / length : 0;
		const real_t height = length > 0 ? u.cross(v).length() / length : 0;
		Tile tile;
		tile.corners[start] = Vector2();
		const Vector2 extent(MAX(2.0f, float(length / p_texel_size)), MAX(2.0f, float(height / p_texel_size)));
		tile.corners[second] = Vector2(extent.x, 0);
		tile.corners[third] = Vector2(projected * extent.x, extent.y);
		tile.size = Vector2i(Math::ceil(extent.x), Math::ceil(extent.y)) + Vector2i(6, 6);
		largest = MAX(largest, MAX(tile.size.x, tile.size.y));
		area += double(tile.size.x) * tile.size.y;
		tiles.push_back(tile);
	}
	ERR_FAIL_COND_V_MSG(largest > 8192 || area > 8192.0 * 8192.0, ERR_OUT_OF_MEMORY, "LRT material atlas exceeds 8192 texels; split the mesh surface.");
	const int target_width = MAX(largest, int(Math::ceil(Math::sqrt(area))));
	const int width = MIN(8192, Math::nearest_power_of_2_templated(target_width));
	Vector2i cursor;
	int row_height = 0;
	for (int face = 0; face < tiles.size(); face++) {
		const Tile &tile = tiles[face];
		if (cursor.x + tile.size.x > width) {
			cursor.x = 0;
			cursor.y += row_height;
			row_height = 0;
		}
		for (int corner = 0; corner < 3; corner++) {
			r_surface.atlas_uv.write[face * 3 + corner] = Vector2(cursor + Vector2i(3, 3)) + tile.corners[corner];
		}
		cursor.x += tile.size.x;
		row_height = MAX(row_height, tile.size.y);
	}
	request.size = Size2i(width, cursor.y + row_height);
	ERR_FAIL_COND_V_MSG(request.size.y > 8192, ERR_OUT_OF_MEMORY, "LRT material atlas packing exceeds 8192 texels; split the mesh surface.");
	ERR_FAIL_COND_V(request.vertices.resize(48 + positions.size() * 32) != OK, ERR_OUT_OF_MEMORY);
	request.vertices.fill(0);
	encode_float(1, request.vertices.ptrw());
	encode_uint32(p_instance_index, request.vertices.ptrw() + 4);
	Color instance_color(1, 1, 1, 1);
	Color instance_custom(0, 0, 0, 0);
	if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance)) {
		const Ref<MultiMesh> data = multi->get_multimesh();
		if (data->is_using_colors()) {
			instance_color = data->get_instance_color(p_instance_index);
		}
		if (data->is_using_custom_data()) {
			instance_custom = data->get_instance_custom_data(p_instance_index);
		}
	}
	for (int channel = 0; channel < 4; channel++) {
		encode_float(instance_color[channel], request.vertices.ptrw() + 16 + channel * 4);
		encode_float(instance_custom[channel], request.vertices.ptrw() + 32 + channel * 4);
	}
	for (int index = 0; index < positions.size(); index++) {
		r_surface.atlas_uv.write[index] /= Vector2(request.size);
		uint8_t *record = request.vertices.ptrw() + 48 + index * 32;
		encode_float(r_surface.atlas_uv[index].x, record);
		encode_float(r_surface.atlas_uv[index].y, record + 4);
		encode_uint32(indices.is_empty() ? index : indices[index], record + 8);
	}
	request.output = &r_surface;
	server->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_bake).bind(int64_t(&request)));
	server->sync();
	ERR_FAIL_COND_V_MSG(request.error != OK, request.error, "LRT native material baking failed.");
	return OK;
}

void LRTMaterialBaker::_bake(int64_t p_request) {
	Request &request = *reinterpret_cast<Request *>(p_request);
#ifdef RD_ENABLED
	using namespace RendererSceneRenderImplementation;
	RenderForwardClustered *renderer = RenderForwardClustered::get_singleton();
	ERR_FAIL_NULL(renderer);
	RD *rd = RD::get_singleton();
	RSG::utilities->update_dirty_resources();
	if (request.classify) {
		for (const Ref<Material> &material : request.passes) {
			auto *shader = static_cast<SceneShaderForwardClustered::ShaderData *>(RendererRD::MaterialStorage::get_singleton()->material_get_shader_data(material->get_rid()));
			ERR_FAIL_COND(!shader || !shader->is_valid());
			// Cutout/refraction/blended passes receive GI without becoming occluders.
			request.opaque.push_back(!(shader->uses_alpha || shader->uses_discard || shader->uses_blend_alpha || shader->uses_screen_texture || shader->uses_depth_texture || shader->uses_normal_texture));
		}
		request.error = OK;
		return;
	}
	auto *cull = static_cast<RendererSceneCull *>(RSG::scene);
	cull->update_dirty_instances();
	RSG::utilities->update_dirty_resources(); // Includes newly allocated instance uniforms.
	auto *instance = cull->instance_owner.get_or_null(request.instance);
	struct Resources {
		RD *rd = nullptr;
		RenderForwardClustered *renderer = nullptr;
		RenderGeometryInstance *geometry = nullptr;
		Vector<RID> rids;
		~Resources() {
			LRTRenderer::get_singleton()->set_material_bake_buffer(RID());
			for (int index = rids.size() - 1; index >= 0; index--) {
				rd->free_rid(rids[index]);
			}
			if (geometry) {
				renderer->geometry_instance_free(geometry);
			}
		}
	} resources;
	resources.rd = rd;
	resources.renderer = renderer;
	RenderGeometryInstance *geometry = renderer->geometry_instance_create(request.mesh->get_rid());
	ERR_FAIL_NULL(geometry);
	resources.geometry = geometry;
	geometry->set_transform(request.transform, request.mesh->get_aabb(), request.transform.xform(request.mesh->get_aabb()));
	if (instance) {
		geometry->set_instance_shader_uniforms_offset(instance->instance_uniforms.location());
	}
	PagedArrayPool<RenderGeometryInstance *> pool;
	PagedArray<RenderGeometryInstance *> instances;
	instances.set_page_pool(&pool);
	instances.push_back(geometry);

	Vector<RID> textures;
	for (int attachment = 0; attachment < 6; attachment++) {
		RD::TextureFormat format;
		format.width = request.size.x;
		format.height = request.size.y;
		format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		format.format = attachment == 0 || attachment == 3 ? RD::DATA_FORMAT_R32G32B32A32_SFLOAT : RD::DATA_FORMAT_R8G8B8A8_UNORM;
		if (attachment == 4) {
			format.format = RD::DATA_FORMAT_R32_SFLOAT;
		} else if (attachment == 5) {
			format.format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D32_SFLOAT, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D32_SFLOAT : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
			format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		RID texture = rd->texture_create(format, RD::TextureView());
		ERR_FAIL_COND(texture.is_null());
		textures.push_back(texture);
		resources.rids.push_back(texture);
	}
	RID framebuffer = rd->framebuffer_create(textures);
	ERR_FAIL_COND(framebuffer.is_null());
	resources.rids.push_back(framebuffer);
	RID buffer = rd->storage_buffer_create(request.vertices.size(), request.vertices);
	ERR_FAIL_COND(buffer.is_null());
	resources.rids.push_back(buffer);
	LRTRenderer::get_singleton()->set_material_bake_buffer(buffer);

	// The same shaders, uniforms, instance uniforms, transforms and texture samplers
	// as normal Forward+ rendering, with an independent rasterization coordinate.
	RenderSceneDataRD scene;
	scene.cam_transform = request.camera;
	scene.main_cam_transform = request.camera;
	scene.cam_projection = request.projection;
	scene.cam_orthogonal = request.orthogonal;
	scene.camera_visible_layers = request.camera_layers;
	scene.view_projection[0] = request.projection;
	scene.emissive_exposure_normalization = 1;
	scene.time = renderer->time;
	scene.time_step = renderer->time_step;
	RenderDataRD data;
	data.scene_data = &scene;
	data.instances = &instances;
	data.cluster_size = 1;
	data.cluster_max_elements = 32;
	renderer->scene_shader.enable_material_shader_group();
	renderer->_update_render_base_uniform_set();
	const uint32_t ubo = renderer->_setup_environment(&data, true, request.size, request.size, Color());
	const auto list = RenderForwardClustered::RENDER_LIST_SECONDARY;
	const auto pass = RenderForwardClustered::PASS_MODE_DEPTH_MATERIAL;
	renderer->_fill_render_list(list, &data, pass);
	renderer->render_list[list].sort_by_key();
	renderer->_fill_instance_data(list);
	RID uniforms = renderer->_setup_render_pass_uniform_set(list, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), ubo);
	ERR_FAIL_COND(uniforms.is_null());
	auto &render_list = renderer->render_list[list];
	ERR_FAIL_COND(render_list.elements.is_empty());
	RenderForwardClustered::RenderListParameters params(render_list.elements.ptr(), render_list.element_info.ptr(), render_list.elements.size(), true, pass, 0, true, false, uniforms, true);
	Vector<Color> clear = { Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0) };
	const RD::DrawListID draw = rd->draw_list_begin(framebuffer, RD::DRAW_CLEAR_ALL, clear, 0);
	// Match the native UV2 baker's one-texel conservative border, isolated by
	// three-texel gutters. The final filled triangles overwrite the interior.
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			params.uv_offset = Vector2(float(x) / request.size.x, float(y) / request.size.y);
			renderer->_render_list(draw, rd->framebuffer_get_format(framebuffer), &params, 0, params.element_count);
		}
	}
	params.force_wireframe = false;
	params.uv_offset = Vector2();
	renderer->_render_list(draw, rd->framebuffer_get_format(framebuffer), &params, 0, params.element_count);
	rd->draw_list_end();
	request.output->albedo = Image::create_from_data(request.size.x, request.size.y, false, Image::FORMAT_RGBAF, rd->texture_get_data(textures[0], 0));
	request.output->emission = Image::create_from_data(request.size.x, request.size.y, false, Image::FORMAT_RGBAF, rd->texture_get_data(textures[3], 0));
	const Vector<uint8_t> vertices = rd->buffer_get_data(buffer);
	ERR_FAIL_COND(vertices.size() != request.vertices.size() || request.output->albedo.is_null() || request.output->emission.is_null());
	request.output->positions.resize((vertices.size() - 48) / 32);
	for (int index = 0; index < request.output->positions.size(); index++) {
		const uint8_t *record = vertices.ptr() + 64 + index * 32;
		if (decode_float(record + 12) != 1) {
			request.error = ERR_CANT_CREATE;
			ERR_FAIL_MSG(vformat("LRT material vertex capture missed vertex %d.", index));
		}
		request.output->positions.write[index] = Vector3(decode_float(record), decode_float(record + 4), decode_float(record + 8));
	}
	for (int face = 0; face < request.output->atlas_uv.size(); face += 3) {
		const Vector2 center = (request.output->atlas_uv[face] + request.output->atlas_uv[face + 1] + request.output->atlas_uv[face + 2]) / 3 * Vector2(request.size);
		if (request.output->albedo->get_pixel(int(center.x), int(center.y)).a <= 0) {
			request.error = ERR_CANT_CREATE;
			ERR_FAIL_MSG(vformat("LRT material atlas did not cover triangle %d.", face / 3));
		}
	}
	const Ref<Image> emission = request.output->emission;
	for (int y = 0; y < request.size.y; y++) {
		for (int x = 0; x < request.size.x; x++) {
			const Color color = request.output->albedo->get_pixel(x, y);
			const Color light = emission->get_pixel(x, y);
			if (!Math::is_finite(color.r) || !Math::is_finite(color.g) || !Math::is_finite(color.b) ||
					!Math::is_finite(light.r) || !Math::is_finite(light.g) || !Math::is_finite(light.b)) {
				request.error = ERR_INVALID_DATA;
			}
			request.output->has_emission |= light.r != 0 || light.g != 0 || light.b != 0;
		}
	}
	if (!request.output->has_emission) {
		request.output->emission.unref();
	}
	instances.clear();
	if (request.error == ERR_UNAVAILABLE) {
		request.error = OK;
	}
#endif
}
