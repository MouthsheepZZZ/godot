/**************************************************************************/
/*  local_gi_volume_3d.h                                                  */
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

#include "core/variant/variant.h"
#include "scene/3d/local_gi/local_gi_bvh.h"
#include "scene/3d/local_gi/local_gi_direct_light.h"
#include "scene/3d/local_gi/local_gi_gpu_tracer.h"
#include "scene/3d/local_gi/local_gi_probe_grid.h"
#include "scene/3d/local_gi/local_gi_probe_sample.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/3d/visual_instance_3d.h"

class ImmediateMesh;
class StandardMaterial3D;

class LocalGIVolume3D : public VisualInstance3D {
	GDCLASS(LocalGIVolume3D, VisualInstance3D);

public:
	enum DebugMode {
		DEBUG_DISABLED,
		DEBUG_LOCAL_GEOMETRY,
		DEBUG_STATIC_BVH_HIT,
		DEBUG_DYNAMIC_BVH_HIT,
		DEBUG_RAY_HIT_MISS,
		DEBUG_HIT_NORMAL,
		DEBUG_HIT_DISTANCE,
		DEBUG_PROBE_POSITIONS,
		DEBUG_SELECTED_PROBE_RAYS,
		DEBUG_RAW_PROBE_RADIANCE,
		DEBUG_PROBE_IRRADIANCE,
		DEBUG_VISIBILITY,
		DEBUG_PROBE_WEIGHTS,
		DEBUG_GLOBAL_INDIRECT_CACHE,
		DEBUG_FINAL_LOCAL_GI,
		DEBUG_GLOBAL_GI,
		DEBUG_FINAL_SELECTED_GI,
		DEBUG_PROBE_CLASSIFICATION,
		DEBUG_MAX,
	};

private:
	Vector3 size = Vector3(4, 4, 4);
	float probe_spacing = 0.5;
	int rays_per_probe = 64;
	float update_fraction = 1.0;
	float temporal_hysteresis = 0.9;
	bool multi_bounce_enabled = false;
	DebugMode debug_mode = DEBUG_DISABLED;
	LocalGIBVH static_bvh;
	LocalGIBVH dynamic_bvh;
	Vector<LocalGIContributorKey> dynamic_snapshot;
	AABB dynamic_snapshot_bounds;
	bool dynamic_has_snapshot = false;
	uint32_t dynamic_rebuild_count = 0;
	Vector<LocalGIContributorKey> static_snapshot;
	AABB static_snapshot_bounds;
	bool static_has_snapshot = false;
	uint32_t static_rebuild_count = 0;
	LocalGIGpuTracer gpu_tracer;
	bool gpu_dirty = true;
	LocalGIProbeGrid probe_grid;
	bool probes_dirty = true;
	int debug_selected_probe = -1;
	Vector<LocalGIRayHit> probe_ray_hits;
	bool probe_rays_traced = false;
	Vector<LocalGIDirectLight> collected_lights;
	Vector<Color> probe_irradiances;
	Vector<Color> probe_irradiance_samples;
	Vector<Color> probe_ray_radiances;
	Vector<float> probe_ray_distance_mean;
	Vector<float> probe_ray_distance_mean_samples;
	Vector<float> probe_ray_distance_second_moment;
	Vector<float> probe_ray_distance_second_moment_samples;
	bool one_bounce_ready = false;
	bool temporal_history_valid = false;
	int temporal_probe_cursor = 0;
	Vector<uint8_t> probe_active;
	bool probes_classified = false;
	Ref<ImmediateMesh> debug_mesh;
	Ref<StandardMaterial3D> debug_material;

	Node *_resolve_from_node(Node *p_from_node) const;
	void _collect_dynamic_keys(Node *p_from_node, Vector<LocalGIContributorKey> &r_keys) const;
	void _collect_static_keys(Node *p_from_node, Vector<LocalGIContributorKey> &r_keys) const;
	void _mark_gpu_dirty();
	void _mark_dynamic_gpu_dirty();
	void _mark_probes_dirty();
	void _mark_one_bounce_dirty();
	void _copy_samples_to_estimate();
	void _ensure_probes();
	void _ensure_classified();
	void _set_editor_preview_enabled(bool p_enabled);
	void _editor_scene_changed(Node *p_node);
	void _queue_editor_preview_tick();
	void _editor_preview_tick();
	bool _refresh_contributors_if_dirty();
	bool editor_preview_queued = false;
	Color _evaluate_outgoing_radiance(const LocalGIRayHit &p_hit, const Vector3 &p_direction) const;
	Color _evaluate_previous_indirect_radiance(const LocalGIRayHit &p_hit, const Vector3 &p_direction) const;
	float _visibility_bias() const;
	int _resolved_selected_probe() const;
	void _update_debug_mesh();
	void _draw_probe_debug_mesh();
	void _draw_shading_debug_mesh();
	void _set_debug_mesh_visible(bool p_visible);
	Dictionary _sample_shading_bind(const Vector3 &p_position, const Vector3 &p_normal) const;
	bool updating_debug_mesh = false;
	Dictionary _hit_to_dictionary(const LocalGIRayHit &p_hit) const;
	Dictionary _intersect_static_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) const;
	Dictionary _intersect_dynamic_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) const;
	Dictionary _intersect_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) const;
	Dictionary _intersect_gpu_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction);
	Dictionary _compare_cpu_gpu_rays_bind(const PackedVector3Array &p_origins, const PackedVector3Array &p_directions);
	TypedArray<Dictionary> _intersect_gpu_rays_bind(const PackedVector3Array &p_origins, const PackedVector3Array &p_directions);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const;

	void set_rays_per_probe(int p_rays);
	int get_rays_per_probe() const;

	void set_update_fraction(float p_fraction);
	float get_update_fraction() const;

	void set_temporal_hysteresis(float p_hysteresis);
	float get_temporal_hysteresis() const;

	void set_multi_bounce_enabled(bool p_enabled);
	bool is_multi_bounce_enabled() const;

	void set_debug_mode(DebugMode p_mode);
	DebugMode get_debug_mode() const;

	void bake(Node *p_from_node = nullptr);
	int get_static_rebuild_count() const;
	int get_baked_triangle_count() const;
	bool is_static_dirty(Node *p_from_node = nullptr) const;
	bool intersect_static_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const;
	const LocalGIBVH &get_static_bvh() const { return static_bvh; }

	bool is_dynamic_dirty(Node *p_from_node = nullptr) const;
	bool update_dynamic(Node *p_from_node = nullptr);
	int get_dynamic_rebuild_count() const;
	int get_dynamic_triangle_count() const;
	int get_dynamic_contributor_count() const;
	bool intersect_dynamic_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const;
	bool intersect_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const;
	const LocalGIBVH &get_dynamic_bvh() const { return dynamic_bvh; }

	bool is_gpu_available();
	bool upload_gpu();
	bool intersect_gpu_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit);
	bool intersect_gpu_rays(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions, Vector<LocalGIRayHit> &r_hits);
	LocalGICPUGPUCompareResult compare_cpu_gpu_rays(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions);
	void collect_debug_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const;

	void set_debug_selected_probe(int p_index);
	int get_debug_selected_probe() const;
	void build_probes();
	int get_probe_count() const;
	Vector3i get_probe_resolution() const;
	int get_probe_ray_budget() const;
	Vector3 get_probe_position(int p_index) const;
	PackedVector3Array get_probe_positions() const;
	PackedVector3Array get_probe_directions() const;
	void collect_probe_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const;
	void collect_selected_probe_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const;
	bool trace_probe_rays();
	const LocalGIProbeGrid &get_probe_grid() const { return probe_grid; }

	bool compute_one_bounce(Node *p_from_node = nullptr);
	bool update_temporal();
	bool step_temporal(Node *p_from_node = nullptr);
	void reset_temporal_history();
	bool has_temporal_history() const { return temporal_history_valid; }
	void classify_probes();
	bool is_probe_active(int p_index) const;
	int get_active_probe_count() const;
	PackedByteArray get_probe_active_states() const;
	int get_collected_light_count() const;
	bool has_one_bounce() const { return one_bounce_ready; }
	bool probe_irradiance_is_finite() const;
	Color get_probe_irradiance(int p_index) const;
	PackedColorArray get_probe_irradiances() const;
	Color get_mean_probe_irradiance() const;
	Color get_probe_irradiance_sample(int p_index) const;
	Color get_mean_probe_irradiance_sample() const;
	Color get_probe_ray_radiance(int p_probe_index, int p_ray_index) const;
	float get_probe_ray_distance_mean(int p_probe_index, int p_ray_index) const;
	float get_probe_ray_distance_second_moment(int p_probe_index, int p_ray_index) const;

	LocalGIShadingSample sample_shading(const Vector3 &p_local_position, const Vector3 &p_local_normal) const;
	Color sample_indirect_irradiance(const Vector3 &p_local_position, const Vector3 &p_local_normal) const;
	Color sample_indirect_radiance(const Vector3 &p_local_position, const Vector3 &p_local_normal, const Color &p_albedo) const;

	virtual AABB get_aabb() const override;

	LocalGIVolume3D();
	~LocalGIVolume3D();
};

VARIANT_ENUM_CAST(LocalGIVolume3D::DebugMode)
