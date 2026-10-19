/**************************************************************************/
/*  lrt_directional_shadow.h                                              */
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


#pragma once

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/paged_array.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"

#include <atomic>
#include <vector>

class RenderingDevice;
class RenderGeometryInstance;

// Volume-owned native shadow depths. No extra viewports or per-material copies.
class LRTDirectionalShadow {
public:
	struct Light {
		RID id;
		Vector3 direction;
		uint32_t caster_mask = UINT32_MAX;
		uint64_t signature = 0;
		bool reverse_cull_face = false;
	};
	struct View {
		Projection clip;
		Projection sample;
		Transform3D camera;
		Projection projection;
		float depth_span = 1.0f;
	};
	static View make_view(const AABB &p_bounds, const Vector3 &p_direction, const Vector<AABB> &p_casters, float p_padding);
	Error update(RenderingDevice *p_device, const AABB &p_bounds, const std::vector<Light> &p_lights,
			int p_resolution, float p_padding, const Transform3D &p_volume_to_world, bool p_profile = false);
	void clear(RenderingDevice *p_device);
	RID get_texture() const { return texture; }
	const View &get_view(int p_layer) const { return views[size_t(p_layer)]; }
	uint64_t get_revision() const { return revision.load(); }
	uint64_t get_bytes() const { return texture_bytes.load(); }
	Dictionary get_stats() const;

	LRTDirectionalShadow() { instances.set_page_pool(&instance_pool); }

private:
	std::vector<Light> lights;
	std::vector<View> views;
	PagedArrayPool<RenderGeometryInstance *> instance_pool;
	PagedArray<RenderGeometryInstance *> instances;
	Vector<RID> layer_textures;
	Vector<RID> framebuffers;
	RID texture;
	AABB bounds;
	int resolution = 0;
	float padding = -1.0f;
	std::atomic<uint64_t> revision{ 0 };
	std::atomic<uint64_t> texture_bytes{ 0 };
	std::atomic<uint64_t> updates{ 0 };
	std::atomic<uint64_t> rendered_instances{ 0 };
	std::atomic<int> light_count{ 0 };
	std::atomic<int> map_resolution{ 0 };
	std::atomic<double> last_render_thread_ms{ 0.0 };
	std::atomic<double> last_gpu_ms{ -1.0 };
	std::atomic<uint64_t> gpu_samples{ 0 };
	String timestamp_begin;
	String timestamp_end;
	uint64_t timestamp_frame = UINT64_MAX;

	Error _collect_casters(const Light &p_light, const View &p_receivers, const Transform3D &p_volume_to_world, Vector<AABB> &r_bounds);
	void _free_texture(RenderingDevice *p_device);
};
