/**************************************************************************/
/*  lrt_volume_3d.h                                                       */
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

#include "lrt_volume.h"

#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/rid.h"
#include "scene/3d/visual_instance_3d.h"

#include <atomic>
#include <map>
#include <set>

class Camera3D;
class Environment;
class Image;
class ImageTexture;
class Light3D;
class Material;
class Mesh;
class MeshInstance3D;
class Shader;
class ShaderMaterial;
class SubViewport;
class World3D;

// LRTVolume3D is the editor-facing volume node: it owns one LRTVolume (the migrated
// solver), reads its inputs from the scene (MeshInstance3D receivers, Light3D lights, the
// WorldEnvironment sky), schedules the CPU bake on a worker thread and shows the migrated
// result on the scene's own standard materials. Everything the prototype exposes as an
// input or as a display choice is a property of this node, so a scene only needs the node
// itself: opening the scene in the editor previews the same field the running game uses.
//
// Frame convention: the grid, local fields, propagation history, receivers and lights live
// in the volume's local space. A rigid transform of the volume therefore only changes the
// world-to-volume display transform; it does not rebuild the field or discard history.
// Scaling the volume itself, including inherited scale, is invalid because size is the sole
// authority for the probe region.
//
// It derives from VisualInstance3D, exactly like ReflectionProbe and VoxelGI, so the editor
// treats the volume the same way there: the gizmo draws and drags the box, and focusing the
// node frames it through get_aabb().
class LRTVolume3D : public VisualInstance3D {
	GDCLASS(LRTVolume3D, VisualInstance3D);

public:
	enum GeometryBackend {
		BACKEND_SDF,
		BACKEND_ANALYTIC,
	};

	enum VisibilityMode {
		VISIBILITY_SH,
		VISIBILITY_MASK,
	};

	enum PropagationSampling {
		PROPAGATION_FULL_26,
		PROPAGATION_FOUR_POINT_DITHERED,
	};

private:
	// One receiving surface instance. Its authored overlay remains untouched; Forward+ samples
	// LRT through an internal renderer flag on the instance.
	struct Receiver {
		ObjectID instance_id;
		Ref<Material> authored_overlay;
		Vector3 albedo;
		uint64_t mesh_content_signature = 0;
		uint64_t material_revision_signature = 0;
		uint64_t material_signature = 0;
		String material_error;
		bool contributes = true;
	};

	struct MeshCaptureCache {
		uint64_t key = 0;
		uint64_t content_signature = 0;
		std::shared_ptr<const std::vector<lrt::MeshTriangle>> triangles;
		std::shared_ptr<const lrt::MaterialCapture> material;
		uint64_t material_signature = 0;
	};

	struct LightEntry {
		ObjectID light_id;
		// The user's own visibility, told apart from the display modes that switch lights
		// off: only a visibility this node did not write feeds the solver.
		bool visible = true;
		bool written_visible = true;
	};

	struct NativeLightCapture {
		int light_snapshot_index = -1;
		int light_slot = -1;
		int target_buffer = 0;
		bool active = false;
		ObjectID source_id;
		String source_name;
		String source_type;
		Transform3D source_transform;
		Transform3D source_inverse_transform;
		Vector2 source_area_size;
		double source_range = 0.0;
		double capture_range = 0.0;
		uint32_t source_cull_mask = 0;
		uint32_t source_shadow_caster_mask = 0;
		bool directional = false;
		bool area = false;
		bool shadow_enabled = false;
		Light3D *clone = nullptr;
		SubViewport *viewport = nullptr;
		Camera3D *camera = nullptr;
		MeshInstance3D *receiver_proxy = nullptr;
		int receiver_offset = 0;
		int receiver_count = 0;
		bool processed = false;
	};

	struct NativeLightSnapshot {
		int light_slot = -1;
		int target_buffer = 0;
		ObjectID source_id;
		String source_name;
		String source_type;
		Transform3D source_transform;
		Vector2 source_area_size;
		double source_range = 0.0;
		uint32_t source_cull_mask = 0;
		uint32_t source_shadow_caster_mask = 0;
		bool directional = false;
		bool area = false;
		bool shadow_enabled = false;
		Light3D *clone = nullptr;
		int request_end = 0;
		int request_cursor = 0;
	};

	struct NativeShadowCasterSnapshot {
		Ref<Mesh> mesh;
		Ref<Material> material_override;
		Vector<Ref<Material>> surface_materials;
		Transform3D transform;
		uint32_t layer_mask = 0;
	};

	struct NativeLightCaptureRequest {
		int light_snapshot_index = -1;
		int receiver_offset = 0;
		int receiver_count = 0;
	};

	struct NativeReceiverMeshSpec {
		uint32_t light_cull_mask = 0;
		bool directional = false;
		int directional_batch_pages = 40;
	};

	// Worker side of one build: only plain data crosses the thread boundary.
	struct BuildJob {
		std::atomic<bool> done{ false };
		bool analytic = false;
		int generation = 0;
		uint32_t reasons = 0;
		uint64_t queued_usec = 0;
		uint64_t done_usec = 0;
		uint64_t cache_fingerprint = 0;
		double geometry_input_ms = 0.0;
		LRTVolume::LocalBakeResult result;
		Transform3D capture_volume_to_world;
		std::vector<NativeReceiverMeshSpec> receiver_mesh_specs;
		std::map<uint64_t, Ref<Mesh>> receiver_meshes;
	};

	enum RebuildReason {
		REBUILD_REASON_NONE = 0,
		REBUILD_REASON_CONFIGURATION = 1 << 0,
		REBUILD_REASON_GEOMETRY = 1 << 1,
		REBUILD_REASON_MATERIAL = 1 << 2,
		REBUILD_REASON_FORCED = 1 << 3,
	};

	// --- Configuration (inspector properties).
	double spacing = 0.25;
	Vector3 volume_size = Vector3(6, 4, 6);
	int geometry_backend = BACKEND_SDF;
	int visibility_mode = VISIBILITY_SH;
	// Zero inherits the project default; positive values override every contributing mesh.
	int mesh_sdf_resolution = 0;
	bool multi_bounce = true;
	bool paused = false;
	int iterations_per_frame = 2;
	double update_budget_ms = 0.5;
	int propagation_sampling = PROPAGATION_FULL_26;
	bool blur_sampling = true;
	bool editor_preview = true;
	double blend_distance = 0.5;
	Vector3 applied_volume_size = volume_size;
	double applied_spacing = spacing;
	bool has_applied_configuration = false;
	bool editor_build_dirty = true;
	bool editor_rebuild_requested = false;
	uint64_t serialized_build_cache_fingerprint = 0;
	uint64_t applied_build_cache_fingerprint = 0;
	uint64_t pending_apply_cache_fingerprint = 0;
	uint64_t last_cache_lookup_fingerprint = 0;
	bool build_data_missing = false;

	// --- Runtime state.
	Ref<LRTVolume> solver;
	std::vector<Receiver> receivers;
	std::set<ObjectID> stale_mesh_content_receivers;
	std::vector<LightEntry> lights;
	Ref<Environment> environment;
	PackedVector4Array sky_radiance;
	PackedVector3Array sky_samples;
	Array light_inputs;
	Array native_light_field_inputs;
	Array active_native_light_capture_inputs;
	bool display_active = false;
	uint64_t environment_key = 0;
	PackedVector4Array cached_sky_radiance;
	Ref<Image> cached_sky_panorama;
	bool environment_cache_valid = false;
	Ref<Environment> pending_environment_capture;
	Ref<Image> pending_environment_panorama;
	Mutex environment_capture_mutex;
	std::atomic<uint64_t> environment_capture_generation{ 0 };
	std::atomic<bool> environment_capture_ready{ false };
	uint64_t pending_environment_key = 0;
	int pending_environment_ready_frame = 0;
	bool environment_capture_pending = false;
	bool environment_capture_submitted = false;
	int external_gi_environment_state = -1;
	uint64_t geometry_signature = 0;
	uint64_t material_state_signature = 0;
	bool has_geometry_signature = false;
	bool has_material_state_signature = false;
	std::vector<Vector3> box_min_local;
	std::vector<Vector3> box_max_local;
	String error_message;
	Dictionary build_stats;
	std::map<ObjectID, MeshCaptureCache> mesh_capture_cache;
	static std::map<uint64_t, MeshCaptureCache> shared_mesh_capture_cache;
	int geometry_builds = 0;
	int source_injections = 0;
	int dropped_builds = 0;
	int cancelled_builds = 0;
	bool building = false;
	bool local_apply_pending = false;
	// Set while an editor gizmo drags the volume box: changes are collected and one bake runs
	// when the drag ends, instead of restarting the background bake on every mouse move.
	bool rebuild_suppressed = false;
	bool rebuild_pending = false;
	uint32_t pending_rebuild_reasons = REBUILD_REASON_NONE;
	uint32_t active_rebuild_reasons = REBUILD_REASON_NONE;
	uint32_t applied_rebuild_reasons = REBUILD_REASON_NONE;

	BuildJob *job = nullptr;
	WorkerThreadPool::TaskID task_id = 0;
	int generation = 0;
	int applied_generation = 0;
	// Prototype src/lab.js temporal policy: a rebuild keeps the propagated field when the grid,
	// the backend and the geometry root are unchanged, and clears it otherwise.
	uint64_t pending_operator_key = 0;
	uint64_t applied_operator_key = 0;
	bool has_applied_operator_key = false;
	bool pending_preserve_history = false;
	LRTVolume::LocalBakeResult pending_apply_result;
	int pending_apply_generation = 0;
	uint32_t pending_apply_reasons = REBUILD_REASON_NONE;

	Ref<ShaderMaterial> native_capture_material;
	SubViewport *sky_viewport = nullptr;
	Node *light_capture_host = nullptr;
	std::vector<NativeLightCapture> native_light_captures;
	std::vector<NativeLightSnapshot> native_light_snapshots;
	std::vector<NativeShadowCasterSnapshot> native_shadow_caster_snapshots;
	std::vector<NativeLightCaptureRequest> native_light_capture_requests;
	std::vector<MeshInstance3D *> shadow_caster_clones;
	std::map<uint64_t, Ref<Mesh>> native_receiver_mesh_cache;
	std::map<uint64_t, Ref<Mesh>> pending_receiver_mesh_cache;
	// Released incrementally on the main thread after a receiver-layout switch; live capture
	// proxies retain any meshes they still use, without racing the final Resource unreference.
	std::map<uint64_t, Ref<Mesh>> retired_receiver_mesh_cache;
	Transform3D native_receiver_mesh_transform;
	Transform3D pending_receiver_mesh_transform;
	bool has_native_receiver_mesh_transform = false;
	uint64_t native_light_field_set_signature = 0;
	Array native_light_diagnostics;
	Transform3D native_capture_volume_to_world;
	uint64_t shadow_capture_signature = 0;
	uint64_t active_shadow_capture_signature = 0;
	uint64_t shadow_capture_resource_signature = 0;
	uint64_t active_shadow_capture_resource_signature = 0;
	uint64_t shadow_capture_graph_signature = 0;
	uint64_t active_shadow_capture_graph_signature = 0;
	uint64_t shadow_signature_refresh_frame = UINT64_MAX;
	bool has_shadow_capture_signature = false;
	bool native_capture_pending = false;
	bool native_capture_queued = false;
	bool native_source_ready = false;
	uint64_t active_native_light_set_signature = 0;
	int native_capture_wait_frames = 0;
	int native_capture_settle_frames = 0;
	int native_capture_page_count = 0;
	int native_capture_concurrent_batches_per_light = 2;
	int native_capture_directional_batch_pages = 40;
	int native_capture_last_forced_draws = 0;
	double native_capture_last_ms = 0.0;
	int native_capture_count = 0;
	int native_capture_shadowed_count = 0;
	int native_shadow_caster_instance_count = 0;
	int native_capture_updates = 0;
	int native_capture_last_frame_pages = 0;
	int native_capture_peak_frame_pages = 0;
	int native_capture_gpu_resolves = 0;
	uint64_t native_capture_active_started_usec = 0;
	uint64_t native_capture_queued_usec = 0;
	uint64_t deferred_receiver_capture_frame = UINT64_MAX;
	double native_capture_last_latency_ms = 0.0;
	bool transform_valid = true;
	bool display_collection_dirty = true;
	bool has_display_transform = false;
	Transform3D display_transform;
	uint64_t scheduler_frame = 0;
	uint64_t pending_build_queued_usec = 0;
	uint64_t active_build_queued_usec = 0;
	double last_build_latency_ms = 0.0;
	double last_frame_work_ms = 0.0;
	double peak_frame_work_ms = 0.0;
	double last_collect_geometry_ms = 0.0;
	double last_collect_lights_ms = 0.0;
	double last_environment_ms = 0.0;
	double last_geometry_signature_ms = 0.0;
	double last_material_signature_ms = 0.0;
	double last_shadow_signature_ms = 0.0;
	double last_capture_mesh_prepare_ms = 0.0;
	double last_capture_mesh_submit_ms = 0.0;
	double last_sky_input_ms = 0.0;
	double last_build_poll_ms = 0.0;
	double last_apply_begin_ms = 0.0;
	double last_apply_finish_ms = 0.0;
	double last_build_publish_ms = 0.0;
	double last_native_input_ms = 0.0;
	double last_native_capture_poll_ms = 0.0;
	double last_display_update_ms = 0.0;
	double last_propagation_schedule_ms = 0.0;
	int last_frame_propagation_iterations = 0;

	void _collect_geometry();
	void _collect_lights();
	static Ref<Material> _surface_material(MeshInstance3D *p_instance, int p_surface);
	static Vector3 _material_albedo(const Ref<Material> &p_material);
	static Vector3 _material_emission(const Ref<Material> &p_material);
	static Vector3 _surface_albedo(MeshInstance3D *p_instance);
	static String _material_support_error(const Ref<Material> &p_material);
	uint64_t _material_resource_signature(const Ref<Material> &p_material) const;
	uint64_t _material_content_signature(const Ref<Material> &p_material) const;
	uint64_t _material_signature(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay,
			std::map<ObjectID, uint64_t> &r_material_signatures) const;
	uint64_t _material_revision_signature(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay) const;
	bool _capture_mesh(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay,
			const Transform3D &p_transform, int p_resolution,
			LRTVolume::MeshInstance &r_mesh, String &r_error) const;
	int _effective_sdf_resolution(MeshInstance3D *p_instance) const;
	Node *_scene_tree_root() const;
	bool _has_valid_volume_transform() const;
	bool _intersects_volume(MeshInstance3D *p_instance) const;
	uint64_t _geometry_signature() const;
	uint64_t _material_state_signature() const;
	uint64_t _build_cache_fingerprint() const;
	Array _mapped_lights() const;
	static bool _light_inputs_equal(const Array &p_left, const Array &p_right);
	static bool _light_capture_input_equal(const Dictionary &p_left, const Dictionary &p_right);
	static bool _light_capture_inputs_equal(const Array &p_left, const Array &p_right);
	Vector3 _light_photometric_scale(Light3D *p_light) const;
	void _apply_native_light_photometry(bool p_count_invalidation = true);
	uint64_t _shadow_inputs_signature(uint64_t *r_resource_signature = nullptr) const;
	uint64_t _native_capture_graph_signature() const;
	void _queue_native_light_capture(bool p_receiver_layout_changed = false, bool p_count_invalidation = true);
	void _rebuild_native_light_capture();
	bool _restart_native_light_capture(bool p_refresh_resources);
	bool _poll_native_light_capture();
	bool _complete_native_light_capture();
	void _finish_native_light_capture();
	void _clear_native_light_capture_batch();
	void _clear_native_light_capture();
	Ref<Mesh> _make_receiver_capture_mesh(uint32_t p_light_cull_mask,
			const Dictionary &p_capture_data, int p_receiver_offset, int p_receiver_count,
			int p_width, int p_height);
	static uint64_t _receiver_capture_mesh_key(uint32_t p_light_cull_mask, int p_receiver_offset,
			int p_receiver_count, int p_width, int p_height);
	static Ref<Mesh> _create_receiver_capture_mesh(uint32_t p_light_cull_mask, const Transform3D &p_volume_to_world,
			const Dictionary &p_capture_data, int p_receiver_offset, int p_receiver_count,
			int p_width, int p_height, double *r_prepare_ms = nullptr, double *r_submit_ms = nullptr);
	static void _prepare_receiver_capture_meshes(BuildJob &p_job, const Dictionary &p_capture_data);
	Light3D *_make_capture_light(Light3D *p_source, int p_index) const;
	void _update_capture_light(Light3D *p_clone, Light3D *p_source, int p_index) const;
	Ref<ShaderMaterial> _capture_material();
	static bool _is_axis_aligned(const Basis &p_basis);
	bool _build_geometry_inputs(std::vector<LRTVolume::BoxInstance> &r_boxes, std::vector<LRTVolume::MeshInstance> &r_meshes,
			bool p_validate_mesh_content, String &r_error);
	void _queue_build(uint32_t p_reasons);
	void _start_build();
	void _poll_build();
	void _finish_build_apply(Dictionary p_applied);
	bool _try_load_editor_cache(uint64_t p_fingerprint);
	// One frame of the node's logic: input refresh, finished-bake processing, propagation.
	void _refresh_frame();
	void _cancel_build();
	void _request_rebuild(uint32_t p_reasons = REBUILD_REASON_CONFIGURATION);
	void _update_display_parameters();
	void _clear_native_receiver();
	void _apply_display();
	PackedVector4Array _environment_radiance();
	PackedVector3Array _environment_samples();
	void _refresh_environment_cache();
	uint64_t _environment_key() const;
	void _refresh_environment();
	void _render_environment(const Ref<Environment> &p_environment);
	void _read_environment_panorama_render_thread(const Ref<Environment> &p_environment, uint64_t p_generation);
	bool _is_external_gi_active() const;
	bool _is_active() const;
	int _convergence_iterations() const;
	Vector3 _effective_volume_size() const;
	double _effective_spacing() const;
	void _inject_sources(bool p_restart = true, bool p_count = true);

	static void _bake_task(void *p_userdata);

protected:
	static void _bind_methods();
	void _notification(int p_what);
	AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

public:
	LRTVolume3D();
	~LRTVolume3D();
	static void clear_shared_mesh_capture_cache();

	void set_enabled(bool p_enabled);
	bool is_enabled() const;
	void set_spacing(double p_spacing);
	double get_spacing() const;
	void set_volume_size(const Vector3 &p_size);
	Vector3 get_volume_size() const;
	void set_geometry_backend(int p_backend);
	int get_geometry_backend() const;
	void set_visibility_mode(int p_mode);
	int get_visibility_mode() const;
	void set_mesh_sdf_resolution(int p_resolution);
	int get_mesh_sdf_resolution() const;
	void set_instance_sdf_resolution(MeshInstance3D *p_instance, int p_resolution);
	int get_instance_sdf_resolution(MeshInstance3D *p_instance) const;
	void set_multi_bounce(bool p_enabled);
	bool is_multi_bounce() const;
	void set_paused(bool p_paused);
	bool is_paused() const;
	void set_iterations_per_frame(int p_iterations);
	int get_iterations_per_frame() const;
	void set_update_budget_ms(double p_budget_ms);
	double get_update_budget_ms() const;
	void set_propagation_sampling(int p_sampling);
	int get_propagation_sampling() const;
	void set_blur_sampling(bool p_enabled);
	bool is_blur_sampling() const;
	void set_editor_preview(bool p_enabled);
	bool is_editor_preview() const;
	void set_external_gi_enabled(bool p_enabled);
	bool is_external_gi_enabled() const;
	void set_display_blend_enabled(bool p_enabled);
	bool is_display_blend_enabled() const;
	void set_blend_distance(double p_distance);
	double get_blend_distance() const;
	void set_build_cache_fingerprint(int64_t p_fingerprint);
	int64_t get_build_cache_fingerprint() const;

	void rebuild();
	void poll();
	void set_rebuild_suppressed(bool p_suppressed);
	bool is_rebuild_suppressed() const;
	PackedStringArray get_volume_warnings() const;
	void step(int p_iterations = 1);
	void step_update();
	void reset_field();
	String get_editor_build_state() const;
	String get_editor_build_tooltip() const;
	String get_instance_sdf_status(MeshInstance3D *p_instance) const;
	bool is_building() const;
	String get_error_message() const;
	Dictionary get_build_stats() const;
	Dictionary get_preparation_status() const;
	Dictionary get_collection_stats() const;
	int get_geometry_builds() const;
	int get_source_injections() const;
	int get_dropped_builds() const;
	int get_cancelled_builds() const;
	Ref<LRTVolume> get_solver() const;
	PackedVector4Array get_sky_radiance() const;
};

VARIANT_ENUM_CAST(LRTVolume3D::GeometryBackend);
VARIANT_ENUM_CAST(LRTVolume3D::VisibilityMode);
VARIANT_ENUM_CAST(LRTVolume3D::PropagationSampling);
