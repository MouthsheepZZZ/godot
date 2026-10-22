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
namespace lrt {
struct TransferInstanceBake;
}
class Mesh;
class Shader;
class ShaderMaterial;
class SubViewport;
class World3D;

// LRTVolume3D is the editor-facing volume node: it owns one LRTVolume (the migrated
// solver), reads its inputs from the scene (geometry contributors, Light3D lights, the
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
	friend class LRTVolume3DTestAccess;

private:
	using GeometryKey = std::pair<RID, int>;

	// One scene geometry input. Receiving LRT is handled independently by Forward+.
	struct Receiver {
		ObjectID instance_id;
		RID render_instance;
		Color instance_color = Color(1, 1, 1, 1);
		bool uses_instance_color = false;
		std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
		GeometryKey get_key() const { return { render_instance, uses_instance_color ? int(Variant(instance_color).hash()) : 0 }; }
		RID multimesh;
		Ref<Mesh> mesh;
		Vector<Ref<Material>> materials;
		Transform3D transform;
		uint32_t layer_mask = 1;
		int sdf_resolution = 0;
		int representation = 0;
		uint64_t transfer_configuration = 0;
		bool gi_enabled = true;
		bool static_gi = true;
		Ref<Material> authored_overlay;
		Vector3 albedo;
		uint64_t mesh_content_signature = 0;
		uint64_t instances_signature = 0;
		uint64_t material_revision_signature = 0;
		uint64_t material_signature = 0;
		String material_error;
		Vector<int> contributing_surfaces;
		bool contributes = true;
	};

	struct MeshCaptureData {
		std::shared_ptr<const lrt::RasterGeometryCapture> raster_capture;
		std::shared_ptr<const std::vector<lrt::MeshTriangle>> triangles;
		std::shared_ptr<const lrt::MaterialCapture> material;
		uint64_t shared_signature = 0;
		uint64_t content_signature = 0;
		uint64_t material_signature = 0;
	};

	struct MeshCaptureCache {
		std::shared_ptr<const MeshCaptureData> data;
		Receiver source;
		uint64_t key = 0;
	};

	struct ResourceDependency {
		Ref<Resource> resource;
		Vector<StringName> global_uniform_names;
		uint64_t global_uniform_revision = UINT64_MAX;
		uint64_t revision = 0;
		int sdf_resolution = 0;
		AABB geometry_bounds;
		double geometry_cell_size = 0.0;
		bool used = false;
	};

	struct LightEntry {
		ObjectID light_id;
		bool visible = true;
	};

	// One light that the direct resolve published in the current batch.
	struct NativeLightSnapshot {
		uint64_t input_usec = 0;
		int light_slot = -1;
		ObjectID source_id;
		String source_name;
		String source_type;
		bool shadow_enabled = false;
	};

	// Worker side of one build: only plain data crosses the thread boundary.
	struct PendingMaterialCapture {
		GeometryKey geometry_key;
		uint64_t cache_key = 0;
		uint64_t shared_cache_key = 0;
		int mesh_index = -1;
		Vector3i size;
		Vector<Ref<Material>> materials;
		Ref<Mesh> mesh;
		Dictionary images;
		bool ready = false;
	};
	std::map<uint64_t, PendingMaterialCapture> pending_material_captures;
	uint64_t material_capture_id = 0;
	uint64_t voxel_captures = 0;
	uint64_t voxel_capture_reuses = 0;
	uint64_t voxel_cache_loads = 0;
	void _material_capture_ready(const Dictionary &p_images, uint64_t p_id);
	void _launch_bake();

	struct BuildJob {
		std::atomic<bool> done{ false };
		bool prepare_cached_assets = false;
		int generation = 0;
		uint32_t reasons = 0;
		uint64_t queued_usec = 0;
		uint64_t done_usec = 0;
		uint64_t cache_fingerprint = 0;
		double geometry_input_ms = 0.0;
		LRTVolume::LocalBakeResult result;
		std::vector<LRTVolume::MeshInstance> meshes;
		std::vector<PendingMaterialCapture> captures;
		String capture_error;
	};

	enum RebuildReason {
		REBUILD_REASON_NONE = 0,
		REBUILD_REASON_CONFIGURATION = 1 << 0,
		REBUILD_REASON_GEOMETRY = 1 << 1,
		REBUILD_REASON_MATERIAL = 1 << 2,
		REBUILD_REASON_FORCED = 1 << 3,
		REBUILD_REASON_CACHE_ASSETS = 1 << 4,
	};

	// --- Configuration (inspector properties).
	double spacing = 0.5;
	Vector3 volume_size = Vector3(6, 4, 6);
	// Zero inherits the project default; positive values override every contributing mesh.
	bool editor_preview = true;
	double blend_distance = 0.5;
	int priority = 0;
	Vector3 applied_volume_size = volume_size;
	double applied_spacing = spacing;
	bool has_applied_configuration = false;
	bool editor_build_dirty = true;
	bool editor_rebuild_requested = false;
	uint64_t serialized_build_cache_fingerprint = 0;
	Dictionary serialized_export_data;
	uint64_t applied_build_cache_fingerprint = 0;
	uint64_t pending_apply_cache_fingerprint = 0;
	uint64_t last_cache_lookup_fingerprint = 0;
	bool build_data_missing = false;

	// --- Runtime state.
	Ref<LRTVolume> solver;
	std::vector<Receiver> receivers;
	std::vector<LightEntry> lights;
	std::vector<ObjectID> geometry_candidates;
	std::vector<ObjectID> gridmap_candidates;
	std::vector<ObjectID> light_candidates;
	bool scene_candidates_dirty = true;
	Ref<Environment> environment;
	PackedVector4Array sky_radiance;
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
	bool external_gi_enabled = true;
	bool receive_sky = true;
	int external_gi_status = -1;
	uint64_t geometry_signature = 0;
	uint64_t material_state_signature = 0;
	bool has_geometry_signature = false;
	bool has_material_state_signature = false;
	String error_message;
	Dictionary build_stats;
	std::map<GeometryKey, MeshCaptureCache> mesh_capture_cache;
	std::map<ObjectID, ResourceDependency> resource_dependencies;
	// Shadow casters and light textures have a separate collection lifetime from GI receivers.
	std::map<ObjectID, ResourceDependency> shadow_resource_dependencies;
	// Capture records are owned on the main thread; workers receive only their immutable arrays.
	static std::map<uint64_t, std::weak_ptr<const MeshCaptureData>> shared_mesh_capture_cache;
	// Main-thread registry for resource accounting.
	static std::map<ObjectID, LRTVolume3D *> active_volumes;
	uint64_t geometry_update_count = 0;
	double last_geometry_update_ms = 0.0;
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
	bool pending_preserve_history = false;
	LRTVolume::LocalBakeResult pending_apply_result;
	int pending_apply_generation = 0;
	uint32_t pending_apply_reasons = REBUILD_REASON_NONE;

	SubViewport *sky_viewport = nullptr;
	std::vector<NativeLightSnapshot> native_light_snapshots;
	uint64_t native_light_field_set_signature = 0;
	Array native_light_diagnostics;
	uint64_t shadow_capture_signature = 0;
	uint64_t shadow_geometry_signature = 0;
	uint64_t light_photometry_signature = 0;
	uint64_t active_shadow_capture_signature = 0;
	uint64_t shadow_capture_resource_signature = 0;
	uint64_t active_shadow_capture_resource_signature = 0;
	uint64_t shadow_signature_refresh_frame = UINT64_MAX;
	bool has_shadow_capture_signature = false;
	bool has_light_photometry_signature = false;
	bool native_capture_pending = false;
	bool native_capture_queued = false;
	bool native_source_ready = false;
	uint64_t active_native_light_set_signature = 0;
	int native_capture_count = 0;
	int native_capture_shadowed_count = 0;
	int native_capture_updates = 0;
	int native_capture_gpu_resolves = 0;
	uint64_t native_capture_active_started_usec = 0;
	uint64_t native_capture_queued_usec = 0;
	// Set when a receiver-layout change must re-resolve the unit fields on a later frame.
	uint64_t deferred_receiver_unit_field_frame = UINT64_MAX;
	double native_capture_last_latency_ms = 0.0;
	bool transform_valid = true;
	bool display_collection_dirty = true;
	bool has_display_transform = false;
	Transform3D display_transform;
	uint64_t scheduler_frame = 0;
	uint64_t build_start_frame = 0;
	uint64_t build_done_frame = 0;
	uint64_t build_apply_frame = 0;
	uint64_t pending_build_queued_usec = 0;
	uint64_t active_build_queued_usec = 0;
	double last_build_latency_ms = 0.0;
	double last_frame_work_ms = 0.0;
	double peak_frame_work_ms = 0.0;
	double last_collect_geometry_ms = 0.0;
	double last_geometry_input_ms = 0.0;
	double last_collect_lights_ms = 0.0;
	double last_environment_ms = 0.0;
	double last_geometry_signature_ms = 0.0;
	double last_material_signature_ms = 0.0;
	double last_shadow_signature_ms = 0.0;
	double last_sky_input_ms = 0.0;
	double last_build_poll_ms = 0.0;
	double last_apply_begin_ms = 0.0;
	double last_apply_finish_ms = 0.0;
	double last_build_publish_ms = 0.0;
	double last_native_input_ms = 0.0;
	double last_native_resolve_poll_ms = 0.0;
	double last_display_update_ms = 0.0;
	double last_propagation_schedule_ms = 0.0;
	int last_frame_propagation_iterations = 0;
	int propagation_iterations_remaining = 0;
	int propagation_iterations_after_change = 0;
	uint64_t propagation_local_version = 0;
	uint64_t propagation_source_version = 0;
	uint64_t propagation_boundary_version = 0;
	bool propagation_boundary_valid = false;
	uint64_t propagation_last_frame = UINT64_MAX;

	void _collect_geometry(bool p_all_inputs = false);
	void _collect_lights();
	void _mark_scene_candidates_dirty();
	void _refresh_scene_candidates();
	static Ref<Material> _surface_material(GeometryInstance3D *p_instance, int p_surface);
	static Vector3 _material_albedo(const Ref<Material> &p_material);
	static Vector3 _surface_albedo(GeometryInstance3D *p_instance);
	static String _material_support_error(const Ref<Material> &p_material);
	uint64_t _resource_dependency_revision(const Ref<Resource> &p_resource, bool p_shadow = false);
	void _resource_dependency_changed(ObjectID p_id);
	void _shadow_resource_dependency_changed(ObjectID p_id);
	void _release_resource_dependencies(bool p_all, bool p_shadow = false);
	uint64_t _material_resource_signature(const Ref<Material> &p_material, bool p_shadow = false);
	static uint64_t _material_content_signature(const Ref<Material> &p_material, bool p_persistent = true, uint8_t p_component = lrt::LOCAL_ALL);
	static uint64_t _transfer_instance_signature(GeometryInstance3D *p_instance, const std::vector<Color> &p_colors);
	static String _transfer_material_error(const Ref<Material> &p_material);
	static Ref<Material> _transfer_capture_material(const Ref<Material> &p_material, const Ref<Material> &p_overlay = Ref<Material>());
	static std::vector<Color> _transfer_instance_colors(GeometryInstance3D *p_instance);
	uint64_t _material_signature(const Receiver &p_receiver,
			std::map<ObjectID, uint64_t> &r_material_signatures);
	uint64_t _material_revision_signature(const Receiver &p_receiver);
	bool _material_inputs_unchanged();
	bool _capture_mesh(const Receiver &p_receiver,
			int p_resolution, const Vector<int> &p_surfaces,
			LRTVolume::MeshInstance &r_mesh, String &r_error, bool p_async, Dictionary &r_images);
	static bool _decode_material_capture(Dictionary &p_images, const Vector3i &p_requested_size,
			LRTVolume::MeshInstance &r_mesh, String &r_error);
	int _effective_sdf_resolution(const Ref<Mesh> &p_mesh, const Object *p_instance = nullptr);
	Node *_scene_tree_root() const;
	bool _has_valid_volume_transform() const;
	bool _intersects_volume(GeometryInstance3D *p_instance);
	uint64_t _geometry_signature() const;
	uint64_t _material_state_signature() const;
	uint64_t _build_cache_fingerprint();
	Array _mapped_lights() const;
	// One light-input snapshot per frame refresh: the capture scheduler and the injection scheduler
	// both read it, and each entry is a multi-key dictionary.
	Array _cached_mapped_lights();
	Array mapped_lights_cache;
	bool mapped_lights_cache_valid = false;
	static bool _light_inputs_equal(const Array &p_left, const Array &p_right);
	static bool _light_capture_input_equal(const Dictionary &p_left, const Dictionary &p_right);
	static bool _light_capture_inputs_equal(const Array &p_left, const Array &p_right);
	Vector3 _light_photometric_scale(Light3D *p_light) const;
	void _apply_native_light_photometry(bool p_count_invalidation = true);
	uint64_t _shadow_inputs_signature(uint64_t *r_resource_signature = nullptr);
	std::map<ObjectID, uint32_t> _shadow_geometry_candidates() const;
	bool _shadow_bounds_intersect(const AABB &p_bounds, uint32_t p_layers) const;
	uint64_t _light_photometry_signature() const;
	uint64_t _native_capture_graph_signature() const;
	void _queue_native_light_capture(bool p_local_field_changed = false, bool p_count_invalidation = true);
	void _rebuild_native_light_capture();
	int _schedule_native_light_capture_batches();
	bool _restart_native_light_capture(bool p_refresh_resources);
	bool _poll_native_light_capture();
	bool _complete_native_light_resolve();
	void _finish_native_light_capture();
	std::vector<Receiver> _local_voxel_inputs(String &r_error, bool p_all_inputs);
	bool _build_geometry_inputs(std::vector<LRTVolume::MeshInstance> &r_meshes,
			bool p_validate_mesh_content, String &r_error, bool p_async = true, bool p_all_inputs = false);
	void _queue_build(uint32_t p_reasons);
	void _start_build();
	void _poll_build();
	void _finish_build_apply(Dictionary p_applied);
	bool _try_load_build_cache(uint64_t p_fingerprint);
	// One frame of the node's logic: input refresh, finished-bake processing, propagation.
	void _refresh_frame();
	void _cancel_build();
	void _request_rebuild(uint32_t p_reasons = REBUILD_REASON_CONFIGURATION);
	void _update_display_parameters();
	void _clear_native_receiver();
	void _apply_display();
	PackedVector4Array _environment_radiance();
	void _refresh_environment_cache();
	uint64_t _environment_key() const;
	void _refresh_environment();
	void _render_environment(const Ref<Environment> &p_environment);
	void _read_environment_panorama_render_thread(const Ref<Environment> &p_environment, uint64_t p_generation);
	bool _is_external_gi_active() const;
	bool _is_active() const;
	void _refresh_propagation_budget();
	int _propagation_iterations_per_frame() const;
	Vector3 _effective_volume_size() const;
	double _effective_spacing() const;
	void _inject_sources(bool p_count = true);

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
	static std::shared_ptr<const MeshCaptureData> _share_mesh_capture(uint64_t p_key, MeshCaptureData p_data);
	static Dictionary prepare_mesh_sdf(const Ref<Mesh> &p_mesh, int p_resolution = 0);
	static Dictionary prepare_transfer_mesh(const Ref<Mesh> &p_mesh, const TypedArray<Material> &p_materials,
			double p_support_radius = 1.0, double p_sample_spacing = 0.25, int p_angular_samples = 256, const Color &p_instance_color = Color(1, 1, 1, 1));
	static Dictionary prepare_transfer_instance(Node3D *p_instance);
	static Dictionary get_transfer_instance_status(Node3D *p_instance);
	static bool prepare_transfer_instance_inputs(Node3D *p_instance, lrt::TransferInstanceBake &r_input, String &r_error);
#ifdef TOOLS_ENABLED
	Dictionary prepare_export_data();
#endif

	void set_probe_spacing(double p_spacing);
	double get_probe_spacing() const;
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;
	void set_editor_preview(bool p_enabled);
	bool is_editor_preview() const;
	void set_receive_sky(bool p_enabled);
	bool is_receiving_sky() const;
	void set_external_gi_enabled(bool p_enabled);
	bool is_external_gi_enabled() const;
	void set_priority(int p_priority);
	int get_priority() const;
	void set_blend_distance(double p_distance);
	double get_blend_distance() const;
	void set_build_cache_fingerprint(int64_t p_fingerprint);
	int64_t get_build_cache_fingerprint() const;
	void set_build_cache_export_data(const Dictionary &p_data);
	Dictionary get_build_cache_export_data() const;

	void rebuild();
	void poll();
	void set_rebuild_suppressed(bool p_suppressed);
	bool is_rebuild_suppressed() const;
	PackedStringArray get_volume_warnings() const;
	String get_editor_build_state() const;
	String get_editor_build_tooltip() const;
	String get_instance_sdf_status(Node3D *p_instance) const;
	String get_instance_sdf_message(Node3D *p_instance) const;
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
