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
#include "core/templates/rid.h"
#include "scene/3d/visual_instance_3d.h"

#include <atomic>

class CanvasLayer;
class ColorRect;
class Environment;
class ImageTexture;
class Light3D;
class Material;
class Mesh;
class MeshInstance3D;
class Shader;
class ShaderMaterial;
class SubViewport;

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

	// Same observe modes the prototype's display offers; the last three are probe slices.
	enum ObserveMode {
		OBSERVE_FULL,
		OBSERVE_DIRECT,
		OBSERVE_INDIRECT,
		OBSERVE_SKY_VISIBILITY,
		OBSERVE_SLICE_RADIANCE,
		OBSERVE_SLICE_SKY_VISIBILITY,
		OBSERVE_SLICE_MATRIX,
	};

private:
	// One receiving surface instance. The node keeps the authored overlay so it can put it
	// back when LRT stops showing.
	struct Receiver {
		MeshInstance3D *instance = nullptr;
		Ref<ShaderMaterial> overlay;
		Ref<Material> authored_overlay;
		Vector3 albedo;
		uint64_t material_signature = 0;
		String material_error;
		bool contributes = true;
	};

	struct LightEntry {
		Light3D *light = nullptr;
		// The user's own visibility, told apart from the display modes that switch lights
		// off: only a visibility this node did not write feeds the solver.
		bool visible = true;
		bool written_visible = true;
	};

	// Worker side of one build: only plain data crosses the thread boundary.
	struct BuildJob {
		std::atomic<bool> done{ false };
		bool analytic = false;
		int generation = 0;
		LRTVolume::LocalBakeResult result;
	};

	// --- Configuration (inspector properties).
	bool enabled = true;
	double spacing = 0.25;
	Vector3 volume_size = Vector3(6, 4, 6);
	int geometry_backend = BACKEND_SDF;
	int visibility_mode = VISIBILITY_SH;
	// Zero inherits the project default; positive values override every contributing mesh.
	int mesh_sdf_resolution = 0;
	bool multi_bounce = true;
	bool paused = true;
	int iterations_per_frame = 2;
	int observe_mode = OBSERVE_FULL;
	double exposure = 1.1;
	double slice_height = 1.0;
	bool blur_sampling = true;
	bool editor_preview = true;
	bool prototype_tonemap = true;

	// --- Runtime state.
	Ref<LRTVolume> solver;
	std::vector<Receiver> receivers;
	std::vector<LightEntry> lights;
	Ref<Environment> environment;
	Vector3 sky;
	Array light_inputs;
	bool display_active = false;
	bool tonemap_saved = false;
	int saved_tonemap_mode = 0;
	double saved_tonemap_white = 1.0;
	double saved_tonemap_exposure = 1.0;
	uint64_t environment_key = 0;
	Vector3 cached_sky;
	uint64_t geometry_signature = 0;
	bool has_signature = false;
	std::vector<Vector3> box_min_local;
	std::vector<Vector3> box_max_local;
	String error_message;
	Dictionary build_stats;
	int geometry_builds = 0;
	int source_injections = 0;
	int dropped_builds = 0;
	int cancelled_builds = 0;
	bool building = false;
	// Set while an editor gizmo drags the volume box: changes are collected and one bake runs
	// when the drag ends, instead of restarting the background bake on every mouse move.
	bool rebuild_suppressed = false;
	bool rebuild_pending = false;

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

	Ref<Shader> receive_shader;
	Ref<Shader> slice_shader;
	// Node children the volume creates for its own display: never owned by the edited
	// scene, so saving the scene stores the volume node alone.
	CanvasLayer *slice_layer = nullptr;
	ColorRect *slice_rect = nullptr;
	SubViewport *sky_viewport = nullptr;
	Ref<ImageTexture> mesh_node_texture;
	Ref<ImageTexture> mesh_triangle_texture;
	Ref<ImageTexture> mesh_material_texture;
	Ref<ImageTexture> mesh_atlas_texture;
	int mesh_node_count = 0;
	bool transform_valid = true;
	bool display_collection_dirty = true;
	bool has_display_transform = false;
	Transform3D display_transform;

	void _collect_geometry();
	void _collect_lights();
	Ref<Shader> _receive_shader();
	Ref<Shader> _slice_shader();
	static Ref<Material> _surface_material(MeshInstance3D *p_instance, int p_surface);
	static Vector3 _material_albedo(const Ref<Material> &p_material);
	static Vector3 _material_emission(const Ref<Material> &p_material);
	static Vector3 _surface_albedo(MeshInstance3D *p_instance);
	static float _surface_metallic(MeshInstance3D *p_instance);
	static String _material_support_error(const Ref<Material> &p_material);
	uint64_t _material_signature(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay) const;
	bool _capture_mesh(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay,
			const Transform3D &p_transform, int p_resolution,
			LRTVolume::MeshInstance &r_mesh, String &r_error) const;
	int _effective_sdf_resolution(MeshInstance3D *p_instance) const;
	Node *_scene_tree_root() const;
	bool _has_valid_volume_transform() const;
	bool _intersects_volume(MeshInstance3D *p_instance) const;
	uint64_t _geometry_signature() const;
	Array _mapped_lights() const;
	static bool _light_inputs_equal(const Array &p_left, const Array &p_right);
	static bool _is_axis_aligned(const Basis &p_basis);
	bool _build_geometry_inputs(std::vector<LRTVolume::BoxInstance> &r_boxes, std::vector<LRTVolume::MeshInstance> &r_meshes, String &r_error);
	void _start_build();
	void _poll_build();
	// One frame of the node's logic: input refresh, finished-bake processing, propagation.
	void _refresh_frame();
	void _cancel_build();
	void _request_rebuild();
	void _ensure_display_resources();
	void _update_display_parameters();
	void _apply_display();
	void _apply_environment(bool p_active);
	void _restore_authored_environment();
	Vector3 _environment_radiance();
	uint64_t _environment_key() const;
	void _refresh_environment();
	void _render_environment();
	bool _is_slice_mode() const;
	bool _is_active() const;
	void _inject_sources(bool p_restart = true);

	static void _bake_task(void *p_userdata);

protected:
	static void _bind_methods();
	void _notification(int p_what);
	AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

public:
	LRTVolume3D();
	~LRTVolume3D();

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
	void set_observe_mode(int p_mode);
	int get_observe_mode() const;
	void set_exposure(double p_exposure);
	double get_exposure() const;
	void set_slice_height(double p_height);
	double get_slice_height() const;
	void set_blur_sampling(bool p_enabled);
	bool is_blur_sampling() const;
	void set_editor_preview(bool p_enabled);
	bool is_editor_preview() const;
	void set_prototype_tonemap(bool p_enabled);
	bool is_prototype_tonemap() const;

	void rebuild();
	void poll();
	void set_rebuild_suppressed(bool p_suppressed);
	bool is_rebuild_suppressed() const;
	PackedStringArray get_volume_warnings() const;
	void step(int p_iterations = 1);
	void reset_field();
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
};

VARIANT_ENUM_CAST(LRTVolume3D::GeometryBackend);
VARIANT_ENUM_CAST(LRTVolume3D::VisibilityMode);
VARIANT_ENUM_CAST(LRTVolume3D::ObserveMode);
