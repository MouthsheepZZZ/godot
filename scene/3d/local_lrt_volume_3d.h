/**************************************************************************/
/*  local_lrt_volume_3d.h                                                 */
/**************************************************************************/

#pragma once

#include "scene/3d/local_lrt_builder.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;

class LocalLRTVolume3D : public Node3D {
	GDCLASS(LocalLRTVolume3D, Node3D);

public:
	enum DebugMode {
		DEBUG_MODE_OCCUPANCY,
		DEBUG_MODE_LOCAL_VISIBILITY,
		DEBUG_MODE_LOCAL_TRANSFER,
		DEBUG_MODE_GLOBAL_VISIBILITY,
		DEBUG_MODE_INJECTION,
		DEBUG_MODE_RADIANCE,
	};

private:
	RID volume;
	bool enabled = true;
	Vector3 size = Vector3(10.0, 10.0, 10.0);
	float probe_spacing = 1.0;
	int visibility_iterations = 4;
	int propagation_iterations = 4;
	float energy = 1.0;
	float edge_blend_distance = 1.0;
	bool debug_draw = false;
	DebugMode debug_mode = DEBUG_MODE_LOCAL_TRANSFER;
	float debug_probe_scale = 0.1;
	LocalLRTBuilder *builder = nullptr;
	Vector<Vector4> global_visibility;
	Vector<Vector4> injection;
	Vector<Vector4> emissive_injection;
	Vector<Vector4> radiance;
	int built_geometry_count = 0;
	MultiMeshInstance3D *debug_probe_instance = nullptr;
	Ref<MultiMesh> debug_probe_multimesh;

	Vector3i _calculate_resolution() const;
	bool _is_valid_probe_position(const Vector3i &p_grid_position) const;
	void _sync_grid();
	void _clear_built_data();
	void _collect_static_geometry(Node *p_node, const Transform3D &p_world_to_volume);
	void _collect_light_injection(Node *p_node);
	void _sync_global_visibility_to_builder();
	void _ensure_debug_probe_instance();
	void _update_debug_probe_instances();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const;

	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const;
	Vector3i get_resolution() const;
	Vector3 get_actual_probe_spacing() const;
	Vector3 get_probe_position(const Vector3i &p_grid_position) const;

	void set_visibility_iterations(int p_iterations);
	int get_visibility_iterations() const;
	void set_propagation_iterations(int p_iterations);
	int get_propagation_iterations() const;

	void set_energy(float p_energy);
	float get_energy() const;

	void set_edge_blend_distance(float p_distance);
	float get_edge_blend_distance() const;

	void set_debug_draw(bool p_enabled);
	bool is_debug_draw_enabled() const;

	void set_debug_mode(DebugMode p_mode);
	DebugMode get_debug_mode() const;

	void set_debug_probe_scale(float p_scale);
	float get_debug_probe_scale() const;

	AABB get_bounds() const;
	RID get_rid() const;
	bool has_built_data() const;
	int get_built_geometry_count() const;
	bool is_probe_occupied(const Vector3i &p_grid_position) const;
	Color get_probe_albedo(const Vector3i &p_grid_position) const;
	Color get_probe_emission(const Vector3i &p_grid_position) const;
	Vector4 get_probe_local_visibility(const Vector3i &p_grid_position) const;
	Color get_probe_transfer_color(const Vector3i &p_grid_position) const;
	Vector4 get_probe_global_visibility(const Vector3i &p_grid_position) const;
	Vector4 get_probe_injection(const Vector3i &p_grid_position, int p_channel) const;
	Color get_probe_injection_color(const Vector3i &p_grid_position) const;
	Vector4 get_probe_radiance(const Vector3i &p_grid_position, int p_channel) const;
	Color get_probe_radiance_color(const Vector3i &p_grid_position) const;
	bool has_gpu_data() const;
	void update_light_injection();
	void rebuild();

	LocalLRTVolume3D();
	~LocalLRTVolume3D();
};

VARIANT_ENUM_CAST(LocalLRTVolume3D::DebugMode);
