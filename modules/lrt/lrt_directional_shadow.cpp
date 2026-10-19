/**************************************************************************/
/*  lrt_directional_shadow.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be        */
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


#include "lrt_directional_shadow.h"

#include "lrt_render_bridge.h"

#include "core/math/geometry_3d.h"
#include "core/os/os.h"

#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"

LRTDirectionalShadow::View LRTDirectionalShadow::make_view(const AABB &p_bounds, const Vector3 &p_direction,
		const Vector<AABB> &p_casters, float p_padding) {
	const Vector3 up = Math::abs(p_direction.y) > 0.99f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
	const Basis basis = Basis::looking_at(p_direction, up);
	const Basis inverse = basis.transposed();
	Vector3 low;
	Vector3 high;
	for (int corner = 0; corner < 8; corner++) {
		const Vector3 point = inverse.xform(p_bounds.get_endpoint(corner));
		low = corner == 0 ? point : low.min(point);
		high = corner == 0 ? point : high.max(point);
	}
	// Fit lateral coverage to receivers; include off-volume casters along the light rays.
	for (const AABB &box : p_casters) {
		for (int corner = 0; corner < 8; corner++) {
			const float depth = inverse.xform(box.get_endpoint(corner)).z;
			low.z = MIN(low.z, depth);
			high.z = MAX(high.z, depth);
		}
	}
	low -= Vector3(p_padding, p_padding, p_padding);
	high += Vector3(p_padding, p_padding, p_padding);
	View result;
	result.camera = Transform3D(basis, basis.get_column(2) * high.z);
	result.projection.set_orthogonal(low.x, high.x, low.y, high.y, 0.0, high.z - low.z);
	Projection correction;
	correction.set_depth_correction(true, true, true);
	result.clip = correction * result.projection * Projection(result.camera.affine_inverse());
	Projection texture_bias;
	texture_bias.columns[0][0] = 0.5f;
	texture_bias.columns[1][1] = 0.5f;
	texture_bias.columns[3][0] = 0.5f;
	texture_bias.columns[3][1] = 0.5f;
	result.sample = texture_bias * result.clip;
	result.depth_span = high.z - low.z;
	return result;
}

Error LRTDirectionalShadow::_collect_casters(const Light &p_light, const View &p_receivers,
		const Transform3D &p_volume_to_world, Vector<AABB> &r_bounds) {
	RendererSceneCull *scene = RendererSceneCull::singleton;
	ERR_FAIL_NULL_V(scene, ERR_UNCONFIGURED);
	scene->update_dirty_instances();
	RendererSceneCull::Instance *light = scene->instance_owner.get_or_null(p_light.id);
	ERR_FAIL_NULL_V(light, ERR_DOES_NOT_EXIST);
	ERR_FAIL_NULL_V(light->scenario, ERR_UNCONFIGURED);

	const Transform3D world_to_volume = p_volume_to_world.affine_inverse();
	Vector<Plane> planes = p_receivers.projection.get_projection_planes(p_volume_to_world * p_receivers.camera);
	// Like the native directional culler, extrude the near plane toward the light.
	planes.write[Projection::PLANE_NEAR].d += 1e6f;
	const Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(planes.ptr(), planes.size());
	instances.clear();
	struct Cull {
		PagedArray<RenderGeometryInstance *> *instances;
		Vector<AABB> *bounds;
		uint32_t mask;
		Transform3D world_to_volume;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			auto *instance = static_cast<RendererSceneCull::Instance *>(p_data);
			if (!instance->visible || !(instance->layer_mask & mask) ||
					!((1 << instance->base_type) & RSE::INSTANCE_GEOMETRY_MASK) ||
					instance->cast_shadows == RSE::SHADOW_CASTING_SETTING_OFF) {
				return false;
			}
			auto *geometry = static_cast<RendererSceneCull::InstanceGeometryData *>(instance->base_data);
			if (!geometry || !geometry->can_cast_shadows || !geometry->geometry_instance) {
				return false;
			}
			if (instance->mesh_instance.is_valid()) {
				RendererRD::MeshStorage::get_singleton()->mesh_instance_check_for_update(instance->mesh_instance);
			}
			instances->push_back(geometry->geometry_instance);
			bounds->push_back(world_to_volume.xform(instance->transformed_aabb));
			return false;
		}
	} cull{ &instances, &r_bounds, p_light.caster_mask, world_to_volume };
	light->scenario->indexers[RendererSceneCull::Scenario::INDEXER_GEOMETRY].convex_query(
			planes.ptr(), planes.size(), points.ptr(), points.size(), cull);
	RendererRD::MeshStorage::get_singleton()->update_mesh_instances();
	return OK;
}

void LRTDirectionalShadow::_free_texture(RenderingDevice *p_device) {
	for (RID framebuffer : framebuffers) {
		p_device->free_rid(framebuffer);
	}
	for (RID layer : layer_textures) {
		p_device->free_rid(layer);
	}
	if (texture.is_valid()) {
		p_device->free_rid(texture);
	}
	framebuffers.clear();
	layer_textures.clear();
	texture = RID();
	texture_bytes.store(0);
}

Error LRTDirectionalShadow::update(RenderingDevice *p_device, const AABB &p_bounds, const std::vector<Light> &p_lights,
		int p_resolution, float p_padding, const Transform3D &p_volume_to_world, bool p_profile) {
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	if (p_profile && timestamp_frame != p_device->get_captured_timestamps_frame()) {
		timestamp_frame = p_device->get_captured_timestamps_frame();
		uint64_t begin = UINT64_MAX;
		for (uint32_t index = 0; index < p_device->get_captured_timestamps_count(); index++) {
			const String name = p_device->get_captured_timestamp_name(index);
			if (name == timestamp_begin) {
				begin = p_device->get_captured_timestamp_gpu_time(index);
			} else if (name == timestamp_end && begin != UINT64_MAX) {
				const uint64_t end = p_device->get_captured_timestamp_gpu_time(index);
				last_gpu_ms.store(double(end - begin) / 1000000.0);
				gpu_samples.fetch_add(1);
			}
		}
	}
	const int size = p_lights.empty() ? 1 : p_resolution;
	const int layers = MAX(1, int(p_lights.size()));
	const bool recreate = texture.is_null() || resolution != size || layer_textures.size() != layers;
	if (recreate) {
		_free_texture(p_device);
		RD::TextureFormat format;
		format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		format.format = RD::DATA_FORMAT_D32_SFLOAT;
		format.width = size;
		format.height = size;
		format.array_layers = layers;
		format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		texture = p_device->texture_create(format, RD::TextureView());
		ERR_FAIL_COND_V(texture.is_null(), ERR_CANT_CREATE);
		const uint64_t bytes = uint64_t(size) * size * layers * sizeof(float);
		for (int layer = 0; layer < layers; layer++) {
			RID slice = p_device->texture_create_shared_from_slice(RD::TextureView(), texture, layer, 0);
			ERR_FAIL_COND_V(slice.is_null(), ERR_CANT_CREATE);
			layer_textures.push_back(slice);
			RID framebuffer = p_device->framebuffer_create(Vector<RID>({ slice }));
			ERR_FAIL_COND_V(framebuffer.is_null(), ERR_CANT_CREATE);
			framebuffers.push_back(framebuffer);
		}
		timestamp_begin = vformat("LRT Directional Shadow %d Begin", texture.get_id());
		timestamp_end = vformat("LRT Directional Shadow %d End", texture.get_id());
		resolution = size;
		texture_bytes.store(bytes);
	}
	const bool bounds_changed = bounds != p_bounds || padding != p_padding;
	views.resize(p_lights.size());
	uint64_t instance_count = 0;
	bool rendered = false;
	for (int layer = 0; layer < int(p_lights.size()); layer++) {
		const Light &light = p_lights[size_t(layer)];
		const bool light_changed = size_t(layer) >= lights.size() || lights[size_t(layer)].id != light.id ||
				lights[size_t(layer)].direction != light.direction || lights[size_t(layer)].caster_mask != light.caster_mask ||
				lights[size_t(layer)].signature != light.signature || lights[size_t(layer)].reverse_cull_face != light.reverse_cull_face;
		if (!recreate && !bounds_changed && !light_changed) {
			continue;
		}
		Vector<AABB> caster_bounds;
		const View receiver_view = make_view(p_bounds, light.direction, {}, p_padding);
		ERR_FAIL_COND_V(_collect_casters(light, receiver_view, p_volume_to_world, caster_bounds) != OK, ERR_CANT_ACQUIRE_RESOURCE);
		views[size_t(layer)] = make_view(p_bounds, light.direction, caster_bounds, p_padding);
		if (p_profile && !rendered) {
			p_device->capture_timestamp(timestamp_begin);
		}
		const View &view = views[size_t(layer)];
		const Error error = RendererSceneRenderImplementation::RenderForwardClustered::get_singleton()->render_shadow_depth(
				framebuffers[layer], p_volume_to_world * view.camera, view.projection, instances,
				LRTRenderBridge::get_native_lighting_camera(), light.reverse_cull_face);
		ERR_FAIL_COND_V(error != OK, error);
		instance_count += instances.size();
		updates.fetch_add(1);
		rendered = true;
	}
	if (p_profile && rendered) {
		p_device->capture_timestamp(timestamp_end);
	}
	padding = p_padding;
	bounds = p_bounds;
	lights = p_lights;
	light_count.store(p_lights.size());
	map_resolution.store(size);
	if (rendered || recreate) {
		revision.fetch_add(1);
		rendered_instances.store(instance_count);
		last_render_thread_ms.store(double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0);
	}
	return OK;
}

void LRTDirectionalShadow::clear(RenderingDevice *p_device) {
	_free_texture(p_device);
	instances.reset();
	lights.clear();
	views.clear();
	light_count.store(0);
	map_resolution.store(0);
}

Dictionary LRTDirectionalShadow::get_stats() const {
	Dictionary result;
	result["backend"] = "native_shadow_depth";
	result["resolution"] = map_resolution.load();
	result["light_count"] = light_count.load();
	result["updates"] = updates.load();
	result["revision"] = revision.load();
	result["rendered_instances"] = rendered_instances.load();
	result["texture_bytes"] = texture_bytes.load();
	result["last_render_thread_ms"] = last_render_thread_ms.load();
	result["last_gpu_ms"] = last_gpu_ms.load();
	result["gpu_samples"] = gpu_samples.load();
	return result;
}
