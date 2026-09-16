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
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
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
	static constexpr int INITIAL_NATIVE_LIGHT_CAPACITY = 8;

	struct NativeLightResolve {
		RID texture;
		Transform3D volume_to_source;
		Vector2 area_half_size;
		double source_range = 0.0;
		double capture_range = 0.0;
		int receiver_offset = 0;
		int receiver_count = 0;
		int image_width = 0;
		int image_height = 0;
		int light_slot = 0;
		int target_buffer = 0;
		bool directional = false;
		bool area = false;
	};

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
		std::shared_ptr<const std::vector<lrt::MeshTriangle>> triangles;
		std::shared_ptr<const lrt::MaterialCapture> material;
		uint64_t material_signature = 0;
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
		int receiver_count = 0;
		int receiver_layout_capacity = 0;
		bool receiver_layout_compacted = false;
		int trunks = 0;
		int dirty_trunks = 0;
		int primitive_ltm_cache_hits = 0;
		int primitive_ltm_cache_misses = 0;
		int primitive_ltm_overlap_fallbacks = 0;
		uint64_t primitive_ltm_cache_bytes = 0;
		uint64_t primitive_ltm_cache_entries = 0;
		int mismatches = 0;
		int mesh_volumes = 0;
		double build_ms = 0.0;
		// Phase breakdown of build_ms, kept because N5 tunes these separately.
		double assets_ms = 0.0;
		double signature_ms = 0.0;
		double topology_ms = 0.0;
		double cache_read_ms = 0.0;
		double voxelize_ms = 0.0;
		double flood_fill_ms = 0.0;
		double distance_ms = 0.0;
		double cache_write_ms = 0.0;
		double instance_field_ms = 0.0;
		double local_ms = 0.0;
		double visibility_ms = 0.0;
		double receiver_capture_ms = 0.0;
		double queue_wait_ms = 0.0;
		double worker_total_ms = 0.0;
		double publish_delay_ms = 0.0;
		double geometry_input_ms = 0.0;
		double receiver_mesh_ms = 0.0;
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
		uint64_t input_bytes = 0;
		uint64_t active_cpu_bytes = 0;
		uint64_t staged_cpu_bytes = 0;
		uint64_t cpu_peak_bytes = 0;
		uint64_t sdf_scratch_peak_bytes = 0;
		uint64_t sdf_samples = 0;
		uint64_t largest_sdf_samples = 0;
		int largest_sdf_triangles = 0;
		double longest_asset_bake_ms = 0.0;
		std::vector<int> sdf_resolutions;
	};

private:
	struct alignas(16) LocalPatchData {
		uint32_t header[4] = {}; // probe, link mask, receiver patch start, receiver patch count
		float material[4] = {};
		float local_visibility[4] = {};
		float matrices[5][4] = {};
	};
	struct alignas(16) ReceiverPatchData {
		float receiver[3][4] = {};
		float emission[4] = {};
	};
	struct ReceiverCopyRange {
		uint32_t old_vector_start = 0;
		uint32_t new_vector_start = 0;
		uint32_t vector_count = 0;
	};

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
	uint64_t input_bytes = 0;
	uint64_t active_cpu_bytes = 0;
	uint64_t staged_cpu_bytes = 0;
	uint64_t cpu_peak_bytes = 0;
	uint64_t sdf_scratch_peak_bytes = 0;
	double signature_ms = 0.0;
	double topology_ms = 0.0;
	double cache_read_ms = 0.0;
	double voxelize_ms = 0.0;
	double flood_fill_ms = 0.0;
	double distance_ms = 0.0;
	double cache_write_ms = 0.0;
	double instance_field_ms = 0.0;
	uint64_t sdf_samples = 0;
	uint64_t largest_sdf_samples = 0;
	int largest_sdf_triangles = 0;
	double longest_asset_bake_ms = 0.0;
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
	// The next worker bake reuses the allocations retired by the previous apply. Keeping them out
	// of main-thread destruction removes allocator stalls without retaining a third live field.
	lrt::LocalField recycled_local;
	lrt::LocalCache recycled_cache;
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
	mutable Dictionary receiver_capture_data_cache;
	Dictionary staged_receiver_capture_data;
	Dictionary retired_receiver_capture_data;
	mutable bool receiver_capture_data_dirty = true;
	bool has_receiver_lighting = false;
	bool native_light_fields_enabled = false;
	int native_light_count = 0;
	struct NativeLightState {
		Vector3 scale;
		Vector3 influence_origin;
		float influence_radius = -1.0f;
		uint32_t cull_mask = UINT32_MAX;
		int current_buffer = 0;
		int target_buffer = 1;
		float blend = 0.0f;
		int blend_frames = 0;
		bool enabled = false;
	};
	std::vector<NativeLightState> native_light_states;
	int native_light_capacity = INITIAL_NATIVE_LIGHT_CAPACITY;
	// Directional environment radiance in the volume-local SH2 basis, one vec4 per RGB
	// channel. Kept for diagnostics; transport uses exact samples at the 26 lattice directions
	// so an occluded sky direction cannot leak its color through another opening.
	Vector4 sky_radiance[3];
	PackedVector3Array sky_samples;
	bool multi_bounce = true;
	bool sh_visibility = true;
	int propagation_sampling = 1;
	bool configured = false;
	bool has_local = false;
	std::atomic<bool> local_debug_textures_enabled{ false };
	bool local_debug_textures_dirty = false;
	std::atomic<bool> debug_textures_full_size{ false };

	RenderingDevice *device = nullptr;
	RID shader_inject;
	RID shader_light_resolve;
	RID shader_propagate;
	RID shader_sky_project;
	RID shader_display;
	RID shader_local_patch;
	RID pipeline_inject;
	RID pipeline_light_resolve;
	RID pipeline_propagate;
	RID pipeline_sky_project;
	RID pipeline_display;
	RID pipeline_local_patch;
	RID params_buffer;
	RID material_buffer;
	RID links_buffer;
	RID matrix_buffer;
	RID local_visibility_buffer;
	// Incremental edits upload into this inactive bank. Propagation keeps reading the active bank
	// until all four geometry buffers are coherent and the descriptor sets switch atomically.
	RID staged_material_buffer;
	RID staged_links_buffer;
	RID staged_matrix_buffer;
	RID staged_local_visibility_buffer;
	RID receiver_buffer;
	RID receiver_emission_buffer;
	RID staged_receiver_buffer;
	RID staged_receiver_emission_buffer;
	RID receiver_lighting_buffer;
	RID native_light_unit_buffers[2];
	RID native_light_state_buffer;
	RID native_light_sampler;
	RID local_patch_buffer;
	RID receiver_patch_buffer;
	size_t receiver_capacity = 0;
	RID source_buffers[3];
	// Incoming radiance sampled from the renderer's HDDAGI field at the six volume faces.
	// The renderer writes these buffers; propagation only reads them.
	RID external_gi_buffers[3];
	RID radiance_buffers[2][3];
	// Rolling four-neighbor contributions for the three paper dither phases. Updating one slot
	// per pass keeps a complete twelve-edge estimate available without reading 26 neighbors.
	RID radiance_phase_buffers[3][3];
	// Exact 384-direction sky projection produced beside directional visibility. Display reads
	// these SH4 buffers directly instead of repeating the projection for every sampled atlas.
	RID sky_buffers[3];
	// One bit stores each binary cubemap-quadrature visibility sample. This geometry-only field
	// lets a rotating HDR sky update immediately without spending float-buffer bandwidth.
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
	RID diagnostic_sdf_texture_rid;
	RID diagnostic_albedo_texture_rid;
	RID diagnostic_emission_texture_rid;
	RID diagnostic_dirty_texture_rid;
	RID uniform_set_inject;
	RID staged_uniform_set_inject;
	RID uniform_set_propagate[2];
	RID staged_uniform_set_propagate[2];
	RID uniform_set_sky_project[2];
	RID uniform_set_display[2];
	RID uniform_set_local_patch;
	RID staged_uniform_set_local_patch;
	int current = 0;
	int iteration = 0;
	int sky_visibility_iterations_remaining = 0;
	int sky_visibility_word_offset = 0;
	std::atomic<bool> sky_projection_dirty{ false };
	std::atomic<int> pending_step_iterations{ 0 };
	std::atomic<bool> injection_pending{ false };
	std::atomic<bool> injection_dirty{ false };
	std::atomic<bool> native_resolve_pending{ false };
	Mutex native_resolve_mutex;
	std::vector<NativeLightResolve> pending_native_resolves;
	Mutex params_mutex;
	std::atomic<double> last_gpu_ms{ 0.0 };
	std::atomic<double> last_cpu_submit_ms{ 0.0 };
	std::atomic<double> last_cpu_wait_ms{ 0.0 };
	enum GpuTimingPass {
		GPU_TIMING_INJECT,
		GPU_TIMING_LIGHT_RESOLVE,
		GPU_TIMING_PROPAGATE,
		GPU_TIMING_DISPLAY,
		GPU_TIMING_PASS_COUNT,
	};
	std::atomic<double> last_gpu_pass_ms[GPU_TIMING_PASS_COUNT]{};
	std::atomic<double> last_render_thread_pass_ms[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> gpu_pass_dispatches[GPU_TIMING_PASS_COUNT]{};
	std::atomic<int> last_gpu_pass_samples[GPU_TIMING_PASS_COUNT]{};
	std::atomic<int> last_gpu_pass_work_items[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> last_gpu_pass_batch_version[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> last_gpu_pass_local_version[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> last_gpu_pass_source_version[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> last_gpu_pass_submission_frame[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> latest_gpu_pass_batch_version[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> latest_gpu_pass_local_version[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> latest_gpu_pass_source_version[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> latest_gpu_pass_submission_frame[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> local_field_version{ 0 };
	std::atomic<uint64_t> source_version{ 0 };
	std::atomic<uint64_t> display_version{ 0 };
	std::atomic<uint64_t> display_radiance_bytes{ 0 };
	std::atomic<uint64_t> display_visibility_bytes{ 0 };
	std::atomic<uint64_t> display_source_bytes{ 0 };
	std::atomic<uint64_t> display_sky_bytes{ 0 };
	bool display_source_dirty = true;
	bool display_sky_dirty = true;
	std::atomic<uint64_t> completed_gpu_pass_ranges[GPU_TIMING_PASS_COUNT]{};
	std::atomic<int> last_gpu_timestamp_begin_matches[GPU_TIMING_PASS_COUNT]{};
	std::atomic<int> last_gpu_timestamp_end_matches[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> last_gpu_timestamp_frame{ 0 };
	bool gpu_timestamp_pending[GPU_TIMING_PASS_COUNT]{};
	int gpu_timestamp_pending_work_items[GPU_TIMING_PASS_COUNT]{};
	uint64_t gpu_timestamp_pending_batch_version[GPU_TIMING_PASS_COUNT]{};
	uint64_t gpu_timestamp_pending_local_version[GPU_TIMING_PASS_COUNT]{};
	uint64_t gpu_timestamp_pending_source_version[GPU_TIMING_PASS_COUNT]{};
	uint64_t gpu_timestamp_pending_submission_frame[GPU_TIMING_PASS_COUNT]{};
	uint64_t gpu_timestamp_pending_result_frame[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> dropped_gpu_timestamp_ranges[GPU_TIMING_PASS_COUNT]{};
	uint64_t last_completed_gpu_timestamp_end[GPU_TIMING_PASS_COUNT]{};
	double last_readback_ms = 0.0;
	uint64_t diagnostic_readbacks = 0;
	String timestamp_begin_names[GPU_TIMING_PASS_COUNT];
	String timestamp_end_names[GPU_TIMING_PASS_COUNT];

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
	Ref<LRTDisplayTexture> diagnostic_sdf_texture;
	Ref<LRTDisplayTexture> diagnostic_albedo_texture;
	Ref<LRTDisplayTexture> diagnostic_emission_texture;
	Ref<LRTDisplayTexture> diagnostic_dirty_texture;

	Error _ensure_device();
	Error _create_shaders();
	Error _create_buffers();
	// Grid-sized buffers (including the radiance/visibility history) and content-sized ones are
	// split, because a geometry edit keeps the first set and only replaces the second.
	Error _create_grid_buffers();
	Error _create_content_buffers();
	Error _create_display_textures();
	Error _create_debug_textures(bool p_full_size);
	RID _create_display_texture(int p_width, int p_height, const std::vector<float> *p_values, Ref<LRTDisplayTexture> &r_texture);
	RID _create_links_texture();
	void _free_debug_textures();
	void _set_local_debug_textures_enabled_render_thread(bool p_enabled);
	Error _create_display_uniform_sets();
	void _free_display_uniform_sets();
	void _free_content_buffers();
	void _free_uniform_sets();
	void _clear_changed_occupancy(const std::vector<int> &p_probes);
	Error _create_uniform_sets();
	void _free_gpu_resources();
	bool _upload_params();
	void _upload_local_buffers();
	bool _upload_local_buffer_chunk();
	void _upload_local_textures();
	void _sync_display();
	void _update_gpu_timing();
	bool _begin_gpu_timestamp(GpuTimingPass p_pass, int p_work_items = 1, uint64_t p_batch_version = 0);
	void _end_gpu_timestamp(GpuTimingPass p_pass, bool p_active);
	void _inject_render_thread();
	void _resolve_native_lights_render_thread();
	void _reset_native_light_buffers_render_thread();
	void _resize_native_light_buffers_render_thread(int p_capacity);
	void _begin_native_light_capture_render_thread(int p_slot, int p_target_buffer);
	void _read_receiver_lighting_render_thread();
	void _step_render_thread(int p_iterations, int p_start_iteration, int p_sampling, bool p_update_sky_visibility);
	void _reset_render_thread();
	void _apply_render_thread(bool p_preserve_history);
	void _submit_apply_chunk();
	void _queue_apply_chunk();
	void _read_back_render_thread();
	void _prepare_shared_gpu_resources_render_thread();
	void _free_render_thread();
	Error readback_error = OK;
	Error apply_error = OK;
	std::atomic<bool> apply_done{ false };
	std::atomic<bool> apply_needs_submit{ false };
	std::atomic<bool> apply_propagation_safe{ false };
	bool apply_pending = false;
	bool apply_preserve_history = false;
	int apply_buffer_stage = 0;
	size_t apply_buffer_offset = 0;
	size_t apply_receiver_copy_range = 0;
	bool apply_sparse_patch = false;
	bool apply_grid_bank_switch = false;
	bool apply_links_changed = true;
	bool local_grid_banks_synchronized = false;
	uint64_t apply_started_usec = 0;
	double apply_submit_ms = 0.0;
	double apply_resources_ms = 0.0;
	double apply_buffer_upload_ms = 0.0;
	double apply_texture_upload_ms = 0.0;
	double apply_finalize_ms = 0.0;
	uint64_t apply_local_patch_upload_bytes = 0;
	uint64_t apply_receiver_patch_upload_bytes = 0;
	uint64_t apply_receiver_copy_bytes = 0;
	uint64_t apply_full_upload_bytes = 0;
	uint64_t receiver_layout_full_rebuilds = 0;
	uint64_t receiver_layout_compactions = 0;
	std::vector<int> pending_changed_probes;
	std::vector<LocalPatchData> staged_local_patches;
	std::vector<ReceiverPatchData> staged_receiver_patches;
	std::vector<ReceiverCopyRange> staged_receiver_copy_ranges;
	bool staged_receiver_copy_valid = false;
	bool _build_primitives(const String &p_backend, int p_threads, std::vector<lrt::SdfPrimitive> &r_primitives,
			std::vector<lrt::Box> &r_boxes);
	LocalBakeResult _bake_local_field_data(bool p_analytic);
	int _mesh_instance_count() const;
	uint64_t _input_bytes() const;
	static Dictionary _make_receiver_capture_data(const lrt::LocalField &p_local);
	static uint64_t _receiver_capture_data_bytes(const Dictionary &p_capture_data);
	uint64_t _active_cpu_bytes() const;
	uint64_t _staged_cpu_bytes() const;
	uint64_t _gpu_bytes() const;
	Dictionary _gpu_memory_breakdown() const;

protected:
	static void _bind_methods();

public:
	LRTVolume();
	~LRTVolume();
	static void free_shared_gpu_resources();
	void prepare_shared_gpu_resources();

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
	PackedVector3Array get_receiver_lighting();
	void reset_native_lights(int p_count);
	void set_native_light_scale(int p_slot, const Vector3 &p_scale);
	void set_native_light_influence(int p_slot, const Vector3 &p_origin, float p_radius, uint32_t p_cull_mask);
	int begin_native_light_capture(int p_slot);
	void resolve_native_light_capture(const NativeLightResolve &p_resolve);
	void commit_native_light_capture(int p_slot, int p_blend_frames);
	bool advance_native_light_blends();
	bool has_native_light_blends() const;
	bool is_native_light_resolve_pending() const;
	void set_sky(const Vector3 &p_sky);
	void set_sky_radiance(const PackedVector4Array &p_radiance);
	PackedVector4Array get_sky_radiance() const;
	void set_sky_samples(const PackedVector3Array &p_samples);
	PackedVector3Array get_sky_samples() const;
	void set_multi_bounce(bool p_enabled);
	void set_sh_visibility(bool p_enabled);
	void set_propagation_sampling(int p_sampling);
	int get_propagation_sampling() const;
	bool set_local_debug_textures_enabled(bool p_enabled);
	Ref<Image> read_environment_panorama(const Ref<Environment> &p_environment, const Vector2i &p_size);
	Vector3 read_environment_radiance(const Ref<Environment> &p_environment, const Vector2i &p_size);
	PackedVector4Array read_environment_radiance_sh(const Ref<Environment> &p_environment, const Vector2i &p_size, const Basis &p_sky_to_local);
	PackedVector4Array project_panorama_radiance_sh(const Ref<Image> &p_panorama, const Basis &p_sky_to_local) const;
	PackedVector3Array sample_panorama_radiance(const Ref<Image> &p_panorama, const Basis &p_local_to_sky) const;

	void request_cancel();
	void clear_cancel();
	bool is_cancel_requested() const;
	bool has_local_field() const;
	// CPU-only half of the bake; safe to call on a worker thread. apply_local_field() is the
	// blocking compatibility wrapper; LRTVolume3D uses begin/finish to upload asynchronously.
	LocalBakeResult bake_local_field_data(bool p_analytic);
	bool load_local_field_cache(uint64_t p_fingerprint, LocalBakeResult &r_result);
	bool store_local_field_cache(uint64_t p_fingerprint) const;
	Dictionary bake_local_field(const String &p_backend);
	// p_preserve_history keeps the propagated field across a geometry edit and clears only the
	// probes whose solid/air occupancy changed (prototype src/lab.js clearChangedOccupancy).
	Dictionary apply_local_field(bool p_preserve_history = false);
	bool begin_apply_local_field(bool p_preserve_history = false);
	bool is_apply_pending() const;
	bool can_step_while_applying() const;
	Dictionary finish_apply_local_field(bool p_wait = false);
	Dictionary build_local_field(const String &p_backend);
	void inject();
	bool is_injection_pending() const;
	void step(int p_iterations);
	void step_radiance_only(int p_iterations);
	int get_pending_step_iterations() const;
	double measure_step_gpu_completion_ms(int p_iterations);
	void reset();
	int get_iteration() const;
	Dictionary get_grid() const;
	Dictionary get_external_gi_buffers() const;
	Dictionary get_debug_resources() const;

	void refresh_display();
	Ref<Texture2D> get_texture(const String &p_name) const;
	PackedFloat32Array read_field(const String &p_name) const;
	PackedInt32Array read_links() const;
	Dictionary get_receiver_capture_data() const;
	Dictionary get_staged_receiver_capture_data() const;
	Dictionary sample_geometry(const Vector3 &p_point) const;
	Dictionary get_stats() const;
	double get_scheduler_gpu_ms() const { return last_gpu_ms.load(); }
	int get_scheduler_gpu_work_items() const { return last_gpu_pass_work_items[GPU_TIMING_PROPAGATE].load(); }
	Dictionary get_performance_stats() const;
	Dictionary get_memory_stats() const;
	void refresh_performance_stats();
	void set_render_frame_profiling_enabled(bool p_enabled);
	Dictionary get_render_frame_profile() const;
	Dictionary get_preparation_status() const;
	static void clear_shared_sdf_cache();
	static void clear_shared_primitive_ltm_cache();
};
