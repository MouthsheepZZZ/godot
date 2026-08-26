/**************************************************************************/
/*  local_gi_gpu_tracer.cpp                                               */
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

#include "local_gi_gpu_tracer.h"

#include <cstring>

#ifdef RD_ENABLED

#include "local_gi_bvh_trace.glsl.gen.h"

#include "servers/rendering/rendering_context_driver.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif
#if defined(D3D12_ENABLED)
#include "drivers/d3d12/rendering_context_driver_d3d12.h"
#endif
#if defined(METAL_ENABLED)
#include "drivers/metal/rendering_context_driver_metal.h"
#endif

namespace {

void store_vec3(float *p_dst, const Vector3 &p_value) {
	p_dst[0] = (float)p_value.x;
	p_dst[1] = (float)p_value.y;
	p_dst[2] = (float)p_value.z;
}

Vector3 load_vec3(const float *p_src) {
	return Vector3(p_src[0], p_src[1], p_src[2]);
}

} // namespace

RenderingDevice *LocalGIGpuTracer::shared_rd = nullptr;
RenderingContextDriver *LocalGIGpuTracer::shared_rcd = nullptr;
RID LocalGIGpuTracer::shared_shader;
RID LocalGIGpuTracer::shared_pipeline;
bool LocalGIGpuTracer::shared_owns_context = false;
bool LocalGIGpuTracer::shared_init_failed = false;

void LocalGIGpuTracer::pack_triangles(const LocalGIBVH &p_bvh, Vector<LocalGIGpuTriangle> &r_triangles) {
	const Vector<LocalGITriangle> &src = p_bvh.get_triangles();
	r_triangles.resize(src.size());
	for (int i = 0; i < src.size(); i++) {
		LocalGIGpuTriangle packed;
		store_vec3(packed.v0, src[i].v0);
		store_vec3(packed.v1, src[i].v1);
		store_vec3(packed.v2, src[i].v2);
		store_vec3(packed.normal, src[i].normal);
		packed.index = src[i].index;
		r_triangles.write[i] = packed;
	}
}

void LocalGIGpuTracer::pack_nodes(const LocalGIBVH &p_bvh, Vector<LocalGIGpuNode> &r_nodes) {
	const Vector<LocalGIBVHNode> &src = p_bvh.get_nodes();
	r_nodes.resize(src.size());
	for (int i = 0; i < src.size(); i++) {
		LocalGIGpuNode packed;
		store_vec3(packed.bounds_min, src[i].bounds_min);
		store_vec3(packed.bounds_max, src[i].bounds_max);
		packed.left = src[i].left;
		packed.right = src[i].right;
		packed.first_triangle = src[i].first_triangle;
		packed.triangle_count = src[i].triangle_count;
		r_nodes.write[i] = packed;
	}
}

LocalGICPUGPUCompareResult LocalGIGpuTracer::compare_hits(const Vector<LocalGIRayHit> &p_cpu, const Vector<LocalGIRayHit> &p_gpu) {
	LocalGICPUGPUCompareResult result;
	result.ray_count = MIN(p_cpu.size(), p_gpu.size());

	for (int i = 0; i < result.ray_count; i++) {
		const LocalGIRayHit &cpu = p_cpu[i];
		const LocalGIRayHit &gpu = p_gpu[i];
		if (cpu.hit != gpu.hit) {
			result.hit_mismatch++;
			continue;
		}
		if (!cpu.hit) {
			continue;
		}

		const real_t distance_error = Math::abs(cpu.distance - gpu.distance);
		result.max_distance_error = MAX(result.max_distance_error, distance_error);

		const real_t position_error = cpu.position.distance_to(gpu.position);
		const real_t normal_dot = CLAMP(cpu.normal.dot(gpu.normal), (real_t)-1, (real_t)1);
		const real_t normal_error = (real_t)1 - normal_dot;
		result.max_normal_error = MAX(result.max_normal_error, normal_error);

		if (cpu.triangle_index != gpu.triangle_index) {
			result.identity_mismatch++;
		}
		if (distance_error > DISTANCE_TOLERANCE || position_error > DISTANCE_TOLERANCE) {
			result.nearest_mismatch++;
		}
		if (cpu.triangle_index == gpu.triangle_index && normal_error > NORMAL_ERROR_TOLERANCE) {
			result.nearest_mismatch++;
		}
	}

	result.passed = result.hit_mismatch == 0 &&
			result.nearest_mismatch == 0 &&
			result.max_distance_error <= DISTANCE_TOLERANCE;
	return result;
}

void LocalGIGpuTracer::_free_rid(RID &r_rid) {
	if (shared_rd != nullptr && r_rid.is_valid()) {
		shared_rd->free_rid(r_rid);
	}
	r_rid = RID();
}

void LocalGIGpuTracer::_free_bvh_buffers() {
	_free_rid(bvh_uniform_set);
	_free_rid(static_nodes_buffer);
	_free_rid(static_triangles_buffer);
	_free_rid(dynamic_nodes_buffer);
	_free_rid(dynamic_triangles_buffer);
	static_node_count = 0;
	dynamic_node_count = 0;
	uploaded = false;
}

RID LocalGIGpuTracer::_create_storage_buffer(const void *p_data, uint32_t p_bytes) {
	const uint32_t size_bytes = MAX(p_bytes, 16u);
	Vector<uint8_t> bytes;
	bytes.resize(size_bytes);
	memset(bytes.ptrw(), 0, size_bytes);
	if (p_data != nullptr && p_bytes > 0) {
		memcpy(bytes.ptrw(), p_data, p_bytes);
	}
	return shared_rd->storage_buffer_create(size_bytes, bytes);
}

bool LocalGIGpuTracer::_ensure_shared() {
	if (shared_pipeline.is_valid() && shared_rd != nullptr) {
		return true;
	}
	if (shared_init_failed) {
		return false;
	}

	if (shared_rd == nullptr && RenderingServer::get_singleton() != nullptr) {
		shared_rd = RenderingServer::get_singleton()->create_local_rendering_device();
	}

	if (shared_rd == nullptr) {
		Error err = OK;
#if defined(METAL_ENABLED)
		if (shared_rcd == nullptr) {
			shared_rcd = memnew(RenderingContextDriverMetal);
			shared_rd = memnew(RenderingDevice);
			shared_owns_context = true;
		}
#endif
#if defined(VULKAN_ENABLED)
		if (shared_rcd == nullptr) {
			shared_rcd = memnew(RenderingContextDriverVulkan);
			shared_rd = memnew(RenderingDevice);
			shared_owns_context = true;
		}
#endif
#if defined(D3D12_ENABLED)
		if (shared_rcd == nullptr) {
			shared_rcd = memnew(RenderingContextDriverD3D12);
			shared_rd = memnew(RenderingDevice);
			shared_owns_context = true;
		}
#endif
		if (shared_rcd == nullptr || shared_rd == nullptr) {
			shared_init_failed = true;
			return false;
		}

		err = shared_rcd->initialize();
		if (err == OK) {
			err = shared_rd->initialize(shared_rcd);
		}
		if (err != OK) {
			memdelete(shared_rd);
			shared_rd = nullptr;
			if (shared_owns_context) {
				memdelete(shared_rcd);
			}
			shared_rcd = nullptr;
			shared_owns_context = false;
			shared_init_failed = true;
			return false;
		}
	}

	Ref<RDShaderFile> shader_file;
	shader_file.instantiate();
	const Error err = shader_file->parse_versions_from_text(local_gi_bvh_trace_shader_glsl);
	if (err != OK) {
		shader_file->print_errors("local_gi_bvh_trace");
		shared_init_failed = true;
		return false;
	}

	shared_shader = shared_rd->shader_create_from_spirv(shader_file->get_spirv_stages("trace"));
	if (shared_shader.is_null()) {
		shared_init_failed = true;
		return false;
	}

	shared_pipeline = shared_rd->compute_pipeline_create(shared_shader);
	if (shared_pipeline.is_null()) {
		shared_init_failed = true;
		return false;
	}
	return true;
}

bool LocalGIGpuTracer::ensure_available() {
	return _ensure_shared();
}

bool LocalGIGpuTracer::upload(const LocalGIBVH &p_static_bvh, const LocalGIBVH &p_dynamic_bvh) {
	if (!_ensure_shared()) {
		return false;
	}

	_free_bvh_buffers();

	Vector<LocalGIGpuNode> static_nodes;
	Vector<LocalGIGpuTriangle> static_triangles;
	Vector<LocalGIGpuNode> dynamic_nodes;
	Vector<LocalGIGpuTriangle> dynamic_triangles;
	pack_nodes(p_static_bvh, static_nodes);
	pack_triangles(p_static_bvh, static_triangles);
	pack_nodes(p_dynamic_bvh, dynamic_nodes);
	pack_triangles(p_dynamic_bvh, dynamic_triangles);

	static_node_count = static_nodes.size();
	dynamic_node_count = dynamic_nodes.size();

	static_nodes_buffer = _create_storage_buffer(static_nodes.ptr(), static_nodes.size() * sizeof(LocalGIGpuNode));
	static_triangles_buffer = _create_storage_buffer(static_triangles.ptr(), static_triangles.size() * sizeof(LocalGIGpuTriangle));
	dynamic_nodes_buffer = _create_storage_buffer(dynamic_nodes.ptr(), dynamic_nodes.size() * sizeof(LocalGIGpuNode));
	dynamic_triangles_buffer = _create_storage_buffer(dynamic_triangles.ptr(), dynamic_triangles.size() * sizeof(LocalGIGpuTriangle));

	if (static_nodes_buffer.is_null() || static_triangles_buffer.is_null() || dynamic_nodes_buffer.is_null() || dynamic_triangles_buffer.is_null()) {
		_free_bvh_buffers();
		return false;
	}

	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, static_nodes_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, static_triangles_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, dynamic_nodes_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, dynamic_triangles_buffer));
	bvh_uniform_set = shared_rd->uniform_set_create(uniforms, shared_shader, 0);
	if (bvh_uniform_set.is_null()) {
		_free_bvh_buffers();
		return false;
	}

	uploaded = true;
	return true;
}

bool LocalGIGpuTracer::trace(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions, Vector<LocalGIRayHit> &r_hits) {
	r_hits.clear();
	ERR_FAIL_COND_V(p_origins.size() != p_directions.size(), false);
	if (p_origins.is_empty()) {
		return true;
	}
	if (!uploaded && !upload(LocalGIBVH(), LocalGIBVH())) {
		return false;
	}

	Vector<LocalGIGpuRay> gpu_rays;
	gpu_rays.resize(p_origins.size());
	for (int i = 0; i < p_origins.size(); i++) {
		LocalGIGpuRay ray;
		store_vec3(ray.origin, p_origins[i]);
		store_vec3(ray.direction, p_directions[i]);
		gpu_rays.write[i] = ray;
	}

	RID ray_buffer = _create_storage_buffer(gpu_rays.ptr(), gpu_rays.size() * sizeof(LocalGIGpuRay));
	RID hit_buffer = _create_storage_buffer(nullptr, gpu_rays.size() * sizeof(LocalGIGpuHit));
	if (ray_buffer.is_null() || hit_buffer.is_null()) {
		_free_rid(ray_buffer);
		_free_rid(hit_buffer);
		return false;
	}

	Vector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, ray_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, hit_buffer));
	RID ray_uniform_set = shared_rd->uniform_set_create(uniforms, shared_shader, 1);
	if (ray_uniform_set.is_null()) {
		_free_rid(ray_buffer);
		_free_rid(hit_buffer);
		return false;
	}

	struct PushConstant {
		int32_t static_node_count = 0;
		int32_t dynamic_node_count = 0;
		int32_t ray_count = 0;
		int32_t pad = 0;
	} push;
	push.static_node_count = static_node_count;
	push.dynamic_node_count = dynamic_node_count;
	push.ray_count = gpu_rays.size();

	const uint32_t groups = (gpu_rays.size() + 63) / 64;
	RD::ComputeListID list = shared_rd->compute_list_begin();
	shared_rd->compute_list_bind_compute_pipeline(list, shared_pipeline);
	shared_rd->compute_list_bind_uniform_set(list, bvh_uniform_set, 0);
	shared_rd->compute_list_bind_uniform_set(list, ray_uniform_set, 1);
	shared_rd->compute_list_set_push_constant(list, &push, sizeof(PushConstant));
	shared_rd->compute_list_dispatch(list, groups, 1, 1);
	shared_rd->compute_list_end();
	shared_rd->submit();
	shared_rd->sync();

	const Vector<uint8_t> hit_bytes = shared_rd->buffer_get_data(hit_buffer);
	_free_rid(ray_uniform_set);
	_free_rid(ray_buffer);
	_free_rid(hit_buffer);

	ERR_FAIL_COND_V(hit_bytes.size() < (int)(gpu_rays.size() * sizeof(LocalGIGpuHit)), false);
	const LocalGIGpuHit *gpu_hits = reinterpret_cast<const LocalGIGpuHit *>(hit_bytes.ptr());
	r_hits.resize(gpu_rays.size());
	for (int i = 0; i < gpu_rays.size(); i++) {
		LocalGIRayHit hit;
		hit.hit = gpu_hits[i].hit != 0;
		hit.distance = gpu_hits[i].distance;
		hit.position = load_vec3(gpu_hits[i].position);
		hit.normal = load_vec3(gpu_hits[i].normal);
		hit.triangle_index = gpu_hits[i].triangle_index;
		r_hits.write[i] = hit;
	}
	return true;
}

LocalGIGpuTracer::~LocalGIGpuTracer() {
	_free_bvh_buffers();
}

#else // RD_ENABLED

void LocalGIGpuTracer::pack_triangles(const LocalGIBVH &p_bvh, Vector<LocalGIGpuTriangle> &r_triangles) {
	r_triangles.clear();
}

void LocalGIGpuTracer::pack_nodes(const LocalGIBVH &p_bvh, Vector<LocalGIGpuNode> &r_nodes) {
	r_nodes.clear();
}

LocalGICPUGPUCompareResult LocalGIGpuTracer::compare_hits(const Vector<LocalGIRayHit> &p_cpu, const Vector<LocalGIRayHit> &p_gpu) {
	LocalGICPUGPUCompareResult result;
	result.ray_count = MIN(p_cpu.size(), p_gpu.size());
	return result;
}

bool LocalGIGpuTracer::ensure_available() {
	return false;
}

bool LocalGIGpuTracer::upload(const LocalGIBVH &p_static_bvh, const LocalGIBVH &p_dynamic_bvh) {
	return false;
}

bool LocalGIGpuTracer::trace(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions, Vector<LocalGIRayHit> &r_hits) {
	r_hits.clear();
	return false;
}

LocalGIGpuTracer::~LocalGIGpuTracer() {
}

#endif // RD_ENABLED
