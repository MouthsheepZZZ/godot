/**************************************************************************/
/*  lrt_volume.h                                                          */
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

#include "lrt_core.h"

#include "core/math/basis.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

#include <atomic>
#include <map>

class Environment;
class Image;
class RenderingDevice;
class Texture2D;
class LRTDisplayTexture;

// N1 driver for the Local Radiance Transfer core.
//
// The numeric algorithm lives in lrt_core.cpp (local field) and the two compute
// shaders (injection, propagation). This class only adapts platform APIs and data
// layout: analytic box inputs from the scene and main RenderingDevice resources. Production
// display stays on the GPU; CPU-visible copies are populated only by explicit diagnostics.
class LRTVolume : public RefCounted {
	GDCLASS(LRTVolume, RefCounted);

public:
	static constexpr int SKY_DIRECTION_COUNT = 384;

	// Engine-facing geometry inputs. The GDScript setters below build the same records
	// from plain dictionaries; LRTVolume3D builds them straight from MeshInstance3D.
	struct BoxInstance {
		// Analytic backend: the world axis-aligned box (prototype geometry-query.js boxes).
		lrt::Vec3 world_min;
		lrt::Vec3 world_max;
		// SDF backend: the box baked in the instance frame (prototype bakeBoxSDF) plus the
		// world transform that places it (prototype PrimitiveGI).
		lrt::Vec3 local_extent;
		lrt::Vec3 color;
		lrt::Vec3 emission;
		lrt::PrimitiveTransform transform;
		bool axis_aligned = true;
		uint32_t layer_mask = 1;
	};

	// One instance of a mesh asset: the shared local triangle soup plus its world transform.
	struct MeshInstance {
		lrt::PrimitiveTransform transform;
		// Effective longest-axis resolution after project and instance overrides.
		int sdf_resolution = 128;
		std::vector<lrt::MeshTriangle> triangles;
		std::shared_ptr<const lrt::MaterialCapture> material;
		uint32_t layer_mask = 1;
	};

	// Plain-data result of the CPU half of the bake, so the whole bake can run on a worker
	// thread without touching engine objects.
	struct LocalBakeResult {
		bool ok = false;
		bool cancelled = false;
		bool needs_axis_aligned = false;
		int solid = 0;
		int surface = 0;
		int receivers = 0;
		int trunks = 0;
		int dirty_trunks = 0;
		int mismatches = 0;
		int mesh_volumes = 0;
		double build_ms = 0.0;
		// Phase breakdown of build_ms, kept because N5 tunes these separately.
		double assets_ms = 0.0;
		double local_ms = 0.0;
		double visibility_ms = 0.0;
		double display_ms = 0.0;
		// Derived-cache accounting: how many assets came from disk and how many were baked.
		int assets_loaded = 0;
		int assets_baked = 0;
		int assets_memory = 0;
		int assets_requested = 0;
		int assets_prepared = 0;
		int closed_mesh_assets = 0;
		int open_mesh_assets = 0;
		int surface_voxels = 0;
		uint64_t sdf_ray_queries = 0;
		int preparation_error = lrt::MESH_SDF_BAKE_OK;
		int sdf_specs = 0;
		int sdf_instance_references = 0;
		uint64_t sdf_bytes = 0;
		uint64_t instance_field_bytes = 0;
		std::vector<int> sdf_resolutions;
	};

private:
	lrt::Grid grid;
	std::vector<BoxInstance> box_instances;
	std::vector<MeshInstance> mesh_instances;
	// Counters of the bake currently running, reported through LocalBakeResult.
	int assets_loaded = 0;
	int assets_baked = 0;
	int assets_memory = 0;
	int assets_requested = 0;
	int assets_prepared = 0;
	int closed_mesh_assets = 0;
	int open_mesh_assets = 0;
	int surface_voxels = 0;
	uint64_t sdf_ray_queries = 0;
	int preparation_error = lrt::MESH_SDF_BAKE_OK;
	int sdf_specs = 0;
	int sdf_instance_references = 0;
	uint64_t sdf_bytes = 0;
	uint64_t instance_field_bytes = 0;
	std::vector<int> sdf_resolutions;
	lrt::LocalField local;
	std::vector<lrt::SdfPrimitive> primitives;
	// CPU result of the last bake, waiting for apply_local_field() to upload it.
	lrt::LocalField staged_local;
	std::vector<lrt::SdfPrimitive> staged_primitives;
	// Incremental state of the applied field and of the bake being staged: trunk signatures plus
	// per-probe samples, so an edit only re-solves the trunks it touched.
	lrt::LocalCache local_cache;
	lrt::LocalCache staged_cache;
	bool has_staged = false;
	std::atomic<bool> cancel_flag{ false };
	// Live worker progress. Phase: 0 idle, 1 assets, 2 local field, 3 visibility, 4 display,
	// 5 ready, 6 failed. The counters count unique active mesh specifications.
	std::atomic<int> preparation_phase{ 0 };
	std::atomic<int> preparation_total{ 0 };
	std::atomic<int> preparation_completed{ 0 };
	String local_backend = "sdf";
	int mesh_sdf_resolution = 128;
	// The grid and backend of the field that is currently on the GPU. The prototype's temporal
	// policy compares exactly these to decide whether a rebuild may keep the propagated field.
	lrt::Grid applied_grid;
	String applied_backend;
	bool has_applied_grid = false;

	std::vector<float> receiver_lighting;
	bool has_receiver_lighting = false;
	// Directional environment radiance in the volume-local SH2 basis, one vec4 per RGB
	// channel. Kept for diagnostics; transport uses exact samples at the 26 lattice directions
	// so an occluded sky direction cannot leak its color through another opening.
	Vector4 sky_radiance[3];
	PackedVector3Array sky_samples;
	bool multi_bounce = true;
	bool sh_visibility = true;
	bool configured = false;
	bool has_local = false;

	RenderingDevice *device = nullptr;
	RID shader_inject;
	RID shader_propagate;
	RID shader_display;
	RID pipeline_inject;
	RID pipeline_propagate;
	RID pipeline_display;
	RID params_buffer;
	RID material_buffer;
	RID links_buffer;
	RID matrix_buffer;
	RID local_visibility_buffer;
	RID receiver_buffer;
	RID receiver_emission_buffer;
	RID receiver_lighting_buffer;
	RID source_buffers[3];
	// Incoming radiance sampled from the renderer's HDDAGI field at the six volume faces.
	// The renderer writes these buffers; propagation only reads them.
	RID external_gi_buffers[3];
	RID radiance_buffers[2][3];
	// Vec4 lanes store one scalar visibility value for each cubemap quadrature direction.
	// This geometry-only field lets a rotating HDR sky update immediately.
	RID directional_visibility_buffers[2];
	RID visibility_buffers[2];
	RID field_texture_rids[3];
	RID sky_texture_rids[3];
	RID visibility_texture_rid;
	RID source_texture_rids[3];
	RID material_texture_rid;
	RID matrix_texture_rid;
	RID local_visibility_texture_rid;
	RID links_texture_rid;
	RID uniform_set_inject;
	RID uniform_set_propagate[2];
	RID uniform_set_display[2];
	int current = 0;
	int iteration = 0;
	double last_gpu_ms = 0.0;
	double last_cpu_submit_ms = 0.0;
	double last_cpu_wait_ms = 0.0;
	double last_readback_ms = 0.0;
	uint64_t diagnostic_readbacks = 0;
	String timestamp_begin_name;
	String timestamp_end_name;

	// CPU-visible copies of the production fields (always the current A/B buffer).
	std::vector<float> radiance_cpu[3];
	std::vector<float> sky_cpu[3];
	std::vector<float> visibility_cpu;
	std::vector<float> source_cpu[3];
	std::vector<float> external_gi_cpu[3];
	Ref<LRTDisplayTexture> field_textures[3];
	Ref<LRTDisplayTexture> sky_textures[3];
	Ref<LRTDisplayTexture> visibility_texture;
	Ref<LRTDisplayTexture> source_textures[3];
	Ref<LRTDisplayTexture> material_texture;
	Ref<LRTDisplayTexture> matrix_texture;
	Ref<LRTDisplayTexture> local_visibility_texture;
	Ref<LRTDisplayTexture> links_texture;

	Error _ensure_device();
	Error _create_shaders();
	Error _create_buffers();
	// Grid-sized buffers (including the radiance/visibility history) and content-sized ones are
	// split, because a geometry edit keeps the first set and only replaces the second.
	Error _create_grid_buffers();
	Error _create_content_buffers();
	Error _create_display_textures();
	RID _create_display_texture(int p_width, int p_height, const std::vector<float> *p_values, Ref<LRTDisplayTexture> &r_texture);
	RID _create_links_texture();
	void _free_content_buffers();
	void _free_uniform_sets();
	void _clear_changed_occupancy(const std::vector<int> &p_probes);
	Error _create_uniform_sets();
	void _free_gpu_resources();
	bool _upload_params();
	void _upload_local_buffers();
	void _sync_display();
	void _update_gpu_timing();
	void _inject_render_thread();
	void _step_render_thread(int p_iterations);
	void _reset_render_thread();
	void _apply_render_thread(bool p_preserve_history);
	void _read_back_render_thread();
	void _free_render_thread();
	Error readback_error = OK;
	Error apply_error = OK;
	std::vector<int> pending_changed_probes;
	bool _build_primitives(const String &p_backend, int p_threads, std::vector<lrt::SdfPrimitive> &r_primitives,
			std::vector<lrt::Box> &r_boxes);
	LocalBakeResult _bake_local_field_data(bool p_analytic);
	int _mesh_instance_count() const;

protected:
	static void _bind_methods();

public:
	LRTVolume();
	~LRTVolume();

	void configure(double p_spacing);
	void configure_with_bounds(double p_spacing, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max);
	void configure_sized(double p_spacing, const Vector3 &p_min, const Vector3 &p_size);
	void configure_sized_with_bounds(double p_spacing, const Vector3 &p_min, const Vector3 &p_size, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max);
	void set_boxes(const Array &p_boxes);
	void set_meshes(const Array &p_meshes);
	void set_box_instances(const std::vector<BoxInstance> &p_boxes);
	void set_mesh_instances(const std::vector<MeshInstance> &p_meshes);
	void set_mesh_sdf_resolution(int p_resolution);
	void set_receiver_lighting(const PackedVector3Array &p_lighting);
	PackedVector3Array get_receiver_lighting() const;
	void set_sky(const Vector3 &p_sky);
	void set_sky_radiance(const PackedVector4Array &p_radiance);
	PackedVector4Array get_sky_radiance() const;
	void set_sky_samples(const PackedVector3Array &p_samples);
	PackedVector3Array get_sky_samples() const;
	void set_multi_bounce(bool p_enabled);
	void set_sh_visibility(bool p_enabled);
	Ref<Image> read_environment_panorama(const Ref<Environment> &p_environment, const Vector2i &p_size);
	Vector3 read_environment_radiance(const Ref<Environment> &p_environment, const Vector2i &p_size);
	PackedVector4Array read_environment_radiance_sh(const Ref<Environment> &p_environment, const Vector2i &p_size, const Basis &p_sky_to_local);
	PackedVector4Array project_panorama_radiance_sh(const Ref<Image> &p_panorama, const Basis &p_sky_to_local) const;
	PackedVector3Array sample_panorama_radiance(const Ref<Image> &p_panorama, const Basis &p_local_to_sky) const;

	void request_cancel();
	void clear_cancel();
	bool is_cancel_requested() const;
	bool has_local_field() const;
	// CPU-only half of the bake; safe to call on a worker thread. apply_local_field() must
	// run on the main thread afterwards to upload the staged result.
	LocalBakeResult bake_local_field_data(bool p_analytic);
	Dictionary bake_local_field(const String &p_backend);
	// p_preserve_history keeps the propagated field across a geometry edit and clears only the
	// probes whose solid/air occupancy changed (prototype src/lab.js clearChangedOccupancy).
	Dictionary apply_local_field(bool p_preserve_history = false);
	Dictionary build_local_field(const String &p_backend);
	void inject();
	void step(int p_iterations);
	void reset();
	int get_iteration() const;
	Dictionary get_grid() const;
	Dictionary get_external_gi_buffers() const;

	void refresh_display();
	Ref<Texture2D> get_texture(const String &p_name) const;
	PackedFloat32Array read_field(const String &p_name) const;
	PackedInt32Array read_links() const;
	Dictionary get_receiver_capture_data() const;
	Dictionary sample_geometry(const Vector3 &p_point) const;
	Dictionary get_stats() const;
	Dictionary get_preparation_status() const;
	static void clear_shared_sdf_cache();
};
