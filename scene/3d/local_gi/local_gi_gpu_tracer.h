/**************************************************************************/
/*  local_gi_gpu_tracer.h                                                 */
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

#pragma once

#include "core/templates/rid.h"
#include "scene/3d/local_gi/local_gi_bvh.h"

class RenderingContextDriver;
class RenderingDevice;

struct LocalGIGpuTriangle {
	float v0[3] = {};
	float pad0 = 0.0f;
	float v1[3] = {};
	float pad1 = 0.0f;
	float v2[3] = {};
	float pad2 = 0.0f;
	float normal[3] = {};
	int32_t index = -1;
};

struct LocalGIGpuNode {
	float bounds_min[3] = {};
	int32_t left = -1;
	float bounds_max[3] = {};
	int32_t right = -1;
	int32_t first_triangle = -1;
	int32_t triangle_count = 0;
	int32_t pad0 = 0;
	int32_t pad1 = 0;
};

struct LocalGIGpuRay {
	float origin[3] = {};
	float pad0 = 0.0f;
	float direction[3] = {};
	float pad1 = 0.0f;
};

struct LocalGIGpuHit {
	uint32_t hit = 0;
	float distance = 0.0f;
	int32_t triangle_index = -1;
	uint32_t pad0 = 0;
	float position[3] = {};
	float pad1 = 0.0f;
	float normal[3] = {};
	float pad2 = 0.0f;
};

struct LocalGICPUGPUCompareResult {
	int ray_count = 0;
	int hit_mismatch = 0;
	int nearest_mismatch = 0;
	int identity_mismatch = 0;
	real_t max_distance_error = 0;
	real_t max_normal_error = 0;
	bool passed = false;
};

class LocalGIGpuTracer {
	static RenderingDevice *shared_rd;
	static RenderingContextDriver *shared_rcd;
	static RID shared_shader;
	static RID shared_pipeline;
	static bool shared_owns_context;
	static bool shared_init_failed;

	RID static_nodes_buffer;
	RID static_triangles_buffer;
	RID dynamic_nodes_buffer;
	RID dynamic_triangles_buffer;
	RID bvh_uniform_set;
	int32_t static_node_count = 0;
	int32_t dynamic_node_count = 0;
	bool uploaded = false;

	void _free_rid(RID &r_rid);
	void _free_bvh_buffers();
	static bool _ensure_shared();
	RID _create_storage_buffer(const void *p_data, uint32_t p_bytes);

public:
	static constexpr real_t DISTANCE_TOLERANCE = 1e-3;
	static constexpr real_t NORMAL_ERROR_TOLERANCE = 2e-3;

	static void pack_triangles(const LocalGIBVH &p_bvh, Vector<LocalGIGpuTriangle> &r_triangles);
	static void pack_nodes(const LocalGIBVH &p_bvh, Vector<LocalGIGpuNode> &r_nodes);
	static LocalGICPUGPUCompareResult compare_hits(const Vector<LocalGIRayHit> &p_cpu, const Vector<LocalGIRayHit> &p_gpu);

	bool ensure_available();
	bool is_available() const { return shared_rd != nullptr && shared_pipeline.is_valid(); }
	bool upload(const LocalGIBVH &p_static_bvh, const LocalGIBVH &p_dynamic_bvh);
	bool is_uploaded() const { return uploaded; }
	bool trace(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions, Vector<LocalGIRayHit> &r_hits);

	LocalGIGpuTracer() = default;
	~LocalGIGpuTracer();
};
