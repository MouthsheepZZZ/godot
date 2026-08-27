/**************************************************************************/
/*  local_lrt_volume_3d.h                                                 */
/**************************************************************************/

#pragma once

#include "scene/3d/node_3d.h"

class LocalLRTVolume3D : public Node3D {
	GDCLASS(LocalLRTVolume3D, Node3D);

	RID volume;
	bool enabled = true;
	Vector3 size = Vector3(10.0, 10.0, 10.0);
	float probe_spacing = 1.0;
	int propagation_iterations = 4;
	float energy = 1.0;
	float edge_blend_distance = 1.0;
	bool debug_draw = false;
	float debug_probe_scale = 0.1;

	Vector3i _calculate_resolution() const;
	void _sync_grid();

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

	void set_propagation_iterations(int p_iterations);
	int get_propagation_iterations() const;

	void set_energy(float p_energy);
	float get_energy() const;

	void set_edge_blend_distance(float p_distance);
	float get_edge_blend_distance() const;

	void set_debug_draw(bool p_enabled);
	bool is_debug_draw_enabled() const;

	void set_debug_probe_scale(float p_scale);
	float get_debug_probe_scale() const;

	AABB get_bounds() const;
	RID get_rid() const;
	void rebuild();

	LocalLRTVolume3D();
	~LocalLRTVolume3D();
};
