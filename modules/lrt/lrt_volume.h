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
#include "lrt_directional_shadow.h"

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
	ObjectID render_owner;
	GDCLASS(LRTVolume, RefCounted);
	friend class LRTVolume3DTestAccess;

public:
	static constexpr int SKY_DIRECTION_COUNT = 384;
	enum DebugField : uint32_t {
		DEBUG_SOURCE = 1u << 0,
		DEBUG_SKY_VISIBILITY = 1u << 1,
		DEBUG_COARSE_GEOMETRY = 1u << 4,
		DEBUG_DIRTY = 1u << 7,
	};

	// Native per-light parameters sampled at Probe centers during Source recomposition.
	struct NativeLightResolve {
		RID scene_light_instance;
		uint64_t source_signature = 0;
		Transform3D volume_to_source;
		Vector3 light_position;
		Vector3 light_direction;
		Vector2 area_half_size;
		double source_range = 0.0;
		double attenuation = 2.0;
		double shadow_bias = 0.0;
		double spot_cos_angle = 0.0;
		double spot_cone_attenuation = 1.0;
		float area_max_mipmap = 0.0f;
		int light_slot = 0;
		Transform3D volume_to_world;
		int direct_kind = 0;
		uint32_t cull_mask = 0xFFFFFu;
		bool directional = false;
		bool area = false;
		bool area_normalize_energy = false;
		bool shadow_enabled = false;
		// The light owns a projector texture. Its atlas rect is only readable on the render thread.
		bool projector_requested = false;
		bool area_texture_requested = false;
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
		int sdf_resolution = 24;
		lrt::Vec3 color;
		lrt::Vec3 emission;
		uint8_t emission_sides = lrt::EMISSION_FRONT;
		lrt::PrimitiveTransform transform;
		bool axis_aligned = true;
		uint32_t layer_mask = 1;
	};

	// One instance of a mesh asset: the shared local triangle soup plus its world transform.
	struct MeshInstance {
		lrt::PrimitiveTransform transform;
		lrt::PrimitiveTransform material_transform;
		bool instanced = false;
		bool raster_geometry = false;
		uint64_t local_capture_key = 0;
		std::shared_ptr<const lrt::RasterGeometryCapture> raster_capture;
		std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
		// Effective longest-axis resolution after project and instance overrides.
		int sdf_resolution = 128;
		std::shared_ptr<const std::vector<lrt::MeshTriangle>> triangles;
		std::shared_ptr<const lrt::MaterialCapture> material;
		// Retained only until an asynchronous material capture has been decoded.
		std::shared_ptr<const lrt::MaterialCapture> previous_material;
		lrt::LocalInputSignatures capture_input_signatures;
		uint64_t material_signature = 0;
		uint32_t layer_mask = 1;
	};

	// Plain-data result of the CPU half of the bake, so the whole bake can run on a worker
	// thread without touching engine objects.
	struct LocalBakeResult {
		bool ok = false;
		bool cache_loaded = false;
		bool cancelled = false;
		// Every Trunk still holds the previous field: the bake returned before allocating or
		// clearing anything and the caller must not publish a new local field.
		bool unchanged = false;
		bool needs_axis_aligned = false;
		int solid = 0;
		int surface = 0;
		int trunks = 0;
		int dirty_trunks = 0;
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
		double sdf_setup_ms = 0.0;
		double sdf_sample_ms = 0.0;
		double sdf_transfer_ms = 0.0;
		double sdf_merge_ms = 0.0;
		double local_pack_ms = 0.0;
		double queue_wait_ms = 0.0;
		double capture_decode_ms = 0.0;
		double worker_total_ms = 0.0;
		double publish_delay_ms = 0.0;
		double geometry_input_ms = 0.0;
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
		uint32_t header[4] = {}; // probe, link mask, reserved, reserved
		float material[4] = {};
		float records[8][4] = {};
		uint32_t record_slot = lrt::LocalField::NO_RECORD;
	};
	static_assert(sizeof(LocalPatchData) == 176);
	static_assert(offsetof(LocalPatchData, records) == 32);
	static_assert(offsetof(LocalPatchData, record_slot) == 160);
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
	std::vector<lrt::GeometryInstance> primitives;
	mutable Mutex debug_geometry_mutex;
	uint64_t debug_geometry_version = 0;
	uint64_t debug_geometry_uploaded_version = UINT64_MAX;
	struct DebugGeometryAsset {
		RID buffer;
		uint32_t cells = 0;
	};
	using DebugGeometryKey = std::pair<std::shared_ptr<const lrt::CoarseGeometryField>, std::shared_ptr<const lrt::SurfaceMaterialField>>;
	std::map<DebugGeometryKey, DebugGeometryAsset> debug_geometry_assets;
	Array debug_geometry_draws;
	std::atomic<uint64_t> debug_geometry_buffer_bytes{ 0 };
	// CPU result of the last bake, waiting for apply_local_field() to upload it.
	lrt::LocalField staged_local;
	std::vector<lrt::GeometryInstance> staged_primitives;
	// Incremental state of the applied field and of the bake being staged: trunk signatures plus
	// per-probe samples, so an edit only re-solves the trunks it touched.
	lrt::LocalCache local_cache;
	lrt::LocalCache staged_cache;
	// Only the bake worker mutates the spatial index. Diagnostics read its atomic counters.
	lrt::TrunkIndex spatial_index;
	std::atomic<uint64_t> spatial_index_payload_bytes{ 0 };
	std::atomic<uint64_t> spatial_index_registrations{ 0 };
	std::atomic<uint64_t> spatial_index_candidate_references{ 0 };
	std::atomic<uint64_t> spatial_index_overlap_tests{ 0 };
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

	int native_light_count = 0;
	struct NativeLightState {
		uint64_t instance_id = 0;
		uint64_t input_usec = 0;
		Vector3 scale;
		Vector3 influence_origin;
		float influence_radius = -1.0f;
		uint32_t cull_mask = UINT32_MAX;
		bool enabled = false;
		bool has_input = false;
		int atlas_wait_frames = 0;
		NativeLightResolve input;
	};
	std::vector<NativeLightState> native_light_states;
	struct NativeLightInput {
		uint64_t instance_id = 0;
		uint64_t oldest_usec = 0;
		uint64_t newest_usec = 0;
	};
	// Provenance follows the buffers actually consumed by injection and propagation.
	std::vector<NativeLightInput> injected_light_inputs;
	std::vector<NativeLightInput> propagated_light_inputs;
	std::vector<NativeLightInput> displayed_light_inputs;
	uint64_t injected_light_source_version = 0;
	uint64_t propagated_light_source_version = 0;
	uint64_t displayed_light_source_version = 0;
	uint64_t staged_geometry_input_usec = 0;
	std::atomic<uint64_t> local_geometry_input_usec{ 0 };
	uint64_t propagated_geometry_input_usec = 0;
	uint64_t propagated_geometry_local_version = 0;
	uint64_t displayed_geometry_input_usec = 0;
	uint64_t displayed_geometry_local_version = 0;
	uint64_t injected_light_submission_frame = 0;
	uint64_t displayed_light_submission_frame = 0;
	mutable Mutex light_input_mutex;
	int native_light_capacity = 1;
	std::atomic<int> allocated_native_light_capacity{ 0 };
	// Low-order RGB sky radiance and equal-weight wide projections at the 26 lattice directions.
	Vector4 sky_radiance[3];
	float sky_link_radiance[3][lrt::DIRECTION_COUNT][4] = {};
	bool configured = false;
	bool has_local = false;
	std::atomic<uint32_t> requested_debug_fields{ 0 };
	std::atomic<bool> local_debug_textures_dirty{ false };
	std::atomic<uint32_t> allocated_debug_fields{ 0 };
	std::atomic<bool> external_gi_enabled{ false };
	std::atomic<bool> receive_sky{ true };
	std::atomic<bool> external_gi_buffers_full_size{ false };

	RenderingDevice *device = nullptr;
	RID shader_inject;
	RID shader_propagate;
	RID shader_sky_project;
	RID shader_sky_solve;
	RID shader_local_patch;
	RID pipeline_inject;
	RID pipeline_propagate;
	RID pipeline_sky_project;
	RID pipeline_sky_solve;
	RID pipeline_local_patch;
	RID params_buffer;
	Vector<uint8_t> uploaded_params_data;
	RID material_buffer;
	RID links_buffer;
	RID matrix_buffer;
	// Incremental edits upload into this inactive bank. Propagation keeps reading the active bank
	// until all four geometry buffers are coherent and the descriptor sets switch atomically.
	RID staged_material_buffer;
	RID staged_links_buffer;
	RID staged_matrix_buffer;
	RID record_index_buffer;
	RID staged_record_index_buffer;
	std::atomic<uint32_t> local_record_capacity{ 0 };
	bool native_light_inputs_waiting = false;
	RID native_light_trunk_buffer;
	std::atomic<uint64_t> native_light_trunk_bytes{ 0 };
	RID native_light_textures[4];
	RID native_light_state_buffer;
	LRTDirectionalShadow directional_shadows;
	RID bound_directional_shadow;
	Vector<uint8_t> uploaded_native_light_state_data;
	struct SourceRegion {
		int32_t minimum[4] = {};
		int32_t size[4] = {};
	};
	struct SourceLightSnapshot {
		Vector<uint8_t> parameters;
		Vector3 origin;
		float radius = -1.0f;
		uint64_t signature = 0;
		std::array<RID, 4> textures;
	};
	// Render-thread input snapshots contain no per-light spatial results.
	std::map<RID, SourceLightSnapshot> source_light_snapshots;
	Vector3i source_dirty_minimum;
	Vector3i source_dirty_maximum;
	bool source_full_dirty = true;
	SourceRegion debug_source_region;
	uint64_t debug_local_frame = UINT64_MAX;
	uint64_t debug_source_frame = UINT64_MAX;
	uint64_t debug_rgb_frame = UINT64_MAX;
	uint64_t debug_sky_frame = UINT64_MAX;
	std::atomic<uint64_t> source_last_probe_count{ 0 };
	std::atomic<uint64_t> source_total_probe_count{ 0 };
	RID native_light_sampler;
	RID native_light_linear_sampler;
	RID native_light_dummy_texture;
	bool debug_direct_shadow_bound = false;
	std::vector<NativeLightResolve> queued_direct_resolves;
	RID local_patch_buffer;
	RID source_buffers[3];
	// LRT-owned RGB + coverage per outgoing face connection; no interior samples.
	RID external_gi_buffer;
	RID radiance_buffers[2][3];
	// RGB SH4 SkyIncoming integrated from the 26-direction published visibility field.
	RID sky_buffers[3];
	// Geometry-only uint26 work buffers; the completed result publishes independently.
	RID directional_visibility_buffers[2];
	RID published_sky_visibility_buffer;
	RID sky_visibility_sh_buffer;
	RID diagnostic_dirty_texture_rid;
	RID uniform_set_inject;
	RID staged_uniform_set_inject;
	RID uniform_set_propagate[2];
	RID staged_uniform_set_propagate[2];
	RID uniform_set_sky_project;
	RID uniform_set_sky_solve[2];
	RID staged_uniform_set_sky_solve[2];
	RID uniform_set_local_patch;
	RID staged_uniform_set_local_patch;
	int current = 0;
	int iteration = 0;
	int sky_current = 0;
	uint64_t sky_last_frame = UINT64_MAX;
	uint64_t sky_last_queued_frame = UINT64_MAX;
	std::atomic<uint64_t> sky_task_submissions{ 0 };
	std::atomic<int> sky_visibility_iterations_remaining{ 0 };
	std::atomic<uint64_t> sky_geometry_version{ 0 };
	std::atomic<uint64_t> sky_published_version{ 0 };
	std::atomic<uint64_t> sky_publications{ 0 };
	std::atomic<uint64_t> sky_visibility_rounds{ 0 };
	std::atomic<uint64_t> sky_projection_dispatches{ 0 };
	std::atomic<bool> sky_projection_dirty{ false };
	std::atomic<int> pending_step_iterations{ 0 };
	std::atomic<bool> injection_pending{ false };
	std::atomic<bool> injection_dirty{ false };
	std::atomic<bool> native_resolve_pending{ false };
	String native_resolve_error;
	mutable Mutex native_resolve_mutex;
	std::vector<NativeLightResolve> pending_native_resolves;
	Mutex params_mutex;
	std::atomic<double> last_gpu_ms{ 0.0 };
	std::atomic<double> last_cpu_submit_ms{ 0.0 };
	std::atomic<double> last_cpu_wait_ms{ 0.0 };
	enum GpuTimingPass {
		GPU_TIMING_INJECT,
		GPU_TIMING_LIGHT_RESOLVE,
		GPU_TIMING_PROPAGATE,
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
	std::atomic<uint64_t> completed_gpu_pass_ranges[GPU_TIMING_PASS_COUNT]{};
	std::atomic<int> last_gpu_timestamp_begin_matches[GPU_TIMING_PASS_COUNT]{};
	std::atomic<int> last_gpu_timestamp_end_matches[GPU_TIMING_PASS_COUNT]{};
	std::atomic<uint64_t> last_gpu_timestamp_frame{ 0 };
	std::atomic<double> last_gpu_timing_readback_ms{ 0.0 };
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
	std::vector<uint32_t> sky_visibility_cpu;
	std::vector<float> source_cpu[3];
	std::vector<float> external_gi_cpu;
	Ref<LRTDisplayTexture> diagnostic_dirty_texture;

	Error _ensure_device();
	Error _create_shaders();
	Error _create_buffers();
	// Grid-sized buffers (including the radiance/visibility history) and content-sized ones are
	// split, because a geometry edit keeps the first set and only replaces the second.
	Error _create_grid_buffers();
	Error _grow_local_record_buffers();
	Error _create_content_buffers();
	Error _create_debug_textures(uint32_t p_fields);
	RID _create_diagnostic_texture(int p_width, int p_height, const std::vector<float> *p_values, Ref<LRTDisplayTexture> &r_texture);
	void _publish_primitives();
	void _free_debug_geometry_resources();
	void _set_debug_fields_render_thread(uint32_t p_fields);
	void _set_external_gi_enabled_render_thread(bool p_enabled);
	void _free_content_buffers();
	void _free_uniform_sets();
	void _clear_changed_occupancy(const std::vector<int> &p_probes);
	Error _create_uniform_sets();
	void _free_gpu_resources();
	void _include_source_probes(const Vector3i &p_minimum, const Vector3i &p_maximum);
	void _include_source_light(const SourceLightSnapshot &p_light);
	SourceRegion _consume_source_region();
	bool _upload_params(std::vector<NativeLightInput> *r_light_inputs = nullptr, bool p_upload_lights = true, SourceRegion *r_source_region = nullptr);
	bool _upload_native_light_state_buffer(bool *r_changed = nullptr);
	Error _create_inject_uniform_sets();
	void _upload_local_buffers();
	void _clear_source_buffers();
	void _upload_staged_local_buffers();
	void _upload_local_textures();
	void _refresh_debug_textures();
	void _update_gpu_timing(bool p_force = false);
	uint64_t gpu_timing_processed_frame = UINT64_MAX;
	bool _begin_gpu_timestamp(GpuTimingPass p_pass, int p_work_items = 1, uint64_t p_batch_version = 0);
	void _end_gpu_timestamp(GpuTimingPass p_pass, bool p_active);
	void _inject_render_thread();
	void _resolve_native_lights_render_thread();
	void _resize_native_light_buffers_render_thread(int p_capacity);
	void _resolve_native_light_field(const NativeLightResolve &p_resolve);
	void _step_render_thread(int p_iterations, int p_start_iteration);
	void _advance_sky_render_thread(uint64_t p_frame);
	void _restart_sky_visibility();
	void _set_receive_sky_render_thread();
	bool _project_sky_render_thread(bool p_visibility);
	void _reset_render_thread();
	void _apply_render_thread(bool p_preserve_history);
	void _submit_apply();
	void _read_back_render_thread();
	void _prepare_shared_gpu_resources_render_thread();
	void _free_render_thread();
	Error readback_error = OK;
	Error apply_error = OK;
	std::atomic<bool> apply_done{ false };
	std::atomic<bool> apply_propagation_safe{ false };
	bool apply_pending = false;
	bool apply_preserve_history = false;
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
	uint64_t apply_full_upload_bytes = 0;
	std::vector<int> pending_changed_probes;
	std::vector<LocalPatchData> staged_local_patches;
	bool _build_primitives(const String &p_backend, int p_threads, std::vector<lrt::GeometryInstance> &r_primitives,
			std::vector<lrt::Box> &r_boxes);
	LocalBakeResult _bake_local_field_data(bool p_analytic);
	int _mesh_instance_count() const;
	uint64_t _input_bytes() const;
	Dictionary _local_field_report() const;
	uint64_t _active_cpu_bytes() const;
	uint64_t _staged_cpu_bytes() const;
	uint64_t _gpu_bytes(Dictionary *r_breakdown = nullptr) const;
	Dictionary _gpu_memory_breakdown() const;
	uint64_t _estimate_field_gpu_bytes(const lrt::LocalField &p_field, bool p_preserve_history) const;

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
	void set_mesh_instances(const std::vector<MeshInstance> &p_meshes, bool p_cull_to_volume = true);
	void set_mesh_sdf_resolution(int p_resolution);

	// Non-spatial light parameters; Source is the only cached direct-light field.
	bool reset_native_lights(int p_count);
	void set_native_light_scale(int p_slot, const Vector3 &p_scale);
	void set_native_light_influence(int p_slot, const Vector3 &p_origin, float p_radius, uint32_t p_cull_mask);
	// Native light parameters are handed over after the owning view's shadow writes.
	void queue_direct_native_light_resolve(const NativeLightResolve &p_resolve);
	void commit_queued_direct_resolves();
	void commit_native_light_input(int p_slot, uint64_t p_instance_id, uint64_t p_input_usec);
	void refresh_native_source_for_view();
	bool is_native_light_resolve_pending() const;
	String get_native_light_resolve_error();
	void set_receive_sky(bool p_enabled);
	bool is_receiving_sky() const { return receive_sky.load(); }
	void advance_sky();
	PackedInt32Array read_sky_visibility() const;
	void set_sky(const Vector3 &p_sky);
	void set_sky_radiance(const PackedVector4Array &p_radiance);
	PackedVector4Array get_sky_radiance() const;
	bool set_debug_fields(uint32_t p_fields);
	void set_external_gi_enabled(bool p_enabled);
	bool has_external_gi_buffers() const;
	Ref<Image> read_environment_panorama(const Ref<Environment> &p_environment, const Vector2i &p_size);
	Vector3 read_environment_radiance(const Ref<Environment> &p_environment, const Vector2i &p_size);
	PackedVector4Array read_environment_radiance_sh(const Ref<Environment> &p_environment, const Vector2i &p_size, const Basis &p_sky_to_local);
	PackedVector4Array project_panorama_radiance_sh(const Ref<Image> &p_panorama, const Basis &p_sky_to_local) const;

	void request_cancel();
	void clear_cancel();
	bool is_cancel_requested() const;
	bool has_local_field() const;
	// CPU-only half of the bake; safe to call on a worker thread. apply_local_field() is the
	// blocking compatibility wrapper; LRTVolume3D uses begin/finish to upload asynchronously.
	LocalBakeResult bake_local_field_data(bool p_analytic, bool p_prepare_cached_assets = false);
	void finish_cached_asset_preparation();
	bool load_local_field_cache(uint64_t p_fingerprint, LocalBakeResult &r_result);
	bool store_local_field_cache(uint64_t p_fingerprint, bool p_staged = false, const PackedStringArray &p_extra_dependencies = PackedStringArray()) const;
	PackedStringArray store_prepared_dependencies(bool p_staged = true) const;
	static Dictionary get_cached_export_dependencies(uint64_t p_fingerprint);
	Dictionary bake_local_field(const String &p_backend);
	// p_preserve_history keeps the propagated field across a geometry edit and clears only the
	// probes whose solid/air occupancy changed (prototype src/lab.js clearChangedOccupancy).
	Dictionary apply_local_field(bool p_preserve_history = false);
	bool begin_apply_local_field(bool p_preserve_history = false);
	uint64_t estimate_staged_gpu_bytes(bool p_preserve_history) const;
	// Native node snapshots are timestamped before baking; publish only with the GPU field.
	void set_geometry_snapshot_input_usec(uint64_t p_usec) { staged_geometry_input_usec = p_usec; }
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
	RID get_external_gi_buffer() const { return external_gi_buffer; }
	Dictionary get_debug_resources() const;
	// Render-thread only: bind the production bank selected by the latest dispatch.
	RID get_radiance_buffer(int p_channel) const { return radiance_buffers[current][p_channel]; }
	RID get_sky_visibility_sh_buffer() const { return sky_visibility_sh_buffer; }
	RID get_sky_visibility_mask_buffer() const { return published_sky_visibility_buffer; }
	RID get_source_buffer(int p_channel) const { return source_buffers[p_channel]; }
	RID get_material_buffer() const { return material_buffer; }
	RID get_links_buffer() const { return links_buffer; }
	RID get_record_index_buffer() const { return record_index_buffer; }
	void mark_gathered();
	uint32_t get_debug_update_activity() const;
	AABB get_debug_source_region() const;
	Array get_geometry_debug_resources();
	uint64_t get_local_field_version() const { return local_field_version.load(); }
	uint64_t get_source_version() const { return source_version.load(); }

	void set_render_owner(ObjectID p_owner) { render_owner = p_owner; }
	void refresh_display();
	Ref<Texture2D> get_texture(const String &p_name) const;
	PackedFloat32Array read_field(const String &p_name) const;
	PackedInt32Array read_links() const;
	PackedInt32Array read_local_record_indices() const;
	// Field-level report for a build that found nothing to solve, so the node can publish the same
	// statistics an upload would have produced, refreshing only diagnostic presentation.
	Dictionary describe_unchanged_local_field();
	Dictionary sample_geometry(const Vector3 &p_point) const;
	Dictionary get_stats() const;
	Dictionary get_performance_stats() const;
	Dictionary get_memory_stats() const;
	void refresh_performance_stats();
	void set_render_frame_profiling_enabled(bool p_enabled);
	Dictionary get_render_frame_profile() const;
	Dictionary get_preparation_status() const;
	static void clear_shared_sdf_cache();
};
