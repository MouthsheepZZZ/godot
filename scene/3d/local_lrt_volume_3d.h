/**************************************************************************/
/*  local_lrt_volume_3d.h                                                 */
/**************************************************************************/

#pragma once

#include "scene/3d/local_lrt_builder.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
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
		DEBUG_MODE_GEOMETRY_DISTANCE,
		DEBUG_MODE_GEOMETRY_COVERAGE,
		DEBUG_MODE_INSIDE_SOLID,
		DEBUG_MODE_DIRECTIONAL_SHADOW,
		DEBUG_MODE_OMNI_SHADOW,
		DEBUG_MODE_AREA_SHADOW,
		DEBUG_MODE_SPOT_SHADOW,
		DEBUG_MODE_SHADOWED_INJECTION,
		DEBUG_MODE_ENVIRONMENT_INJECTION,
	};

	enum RadianceNeighborPattern {
		RADIANCE_NEIGHBOR_PATTERN_REFERENCE_26,
		RADIANCE_NEIGHBOR_PATTERN_DITHERED_4,
	};

private:
	struct GeometrySourceState {
		ObjectID object_id;
		Transform3D object_to_volume;
		Ref<Mesh> mesh;
		Color albedo;
		Color emission;
		Color transfer_emission;
		LocalLRTColorSDF sdf;
		AABB influence_bounds;
		int gi_mode = 0;
		bool visible = false;
		bool sdf_ready = false;
		bool active = false;
	};

	RID volume;
	bool enabled = true;
	Vector3 size = Vector3(10.0, 10.0, 10.0);
	float probe_spacing = 1.0;
	float geometry_voxel_size = 0.125;
	int dynamic_update_probe_budget = 0;
	int visibility_iterations = 4;
	int propagation_iterations = 4;
	int visibility_probe_budget = 0;
	int radiance_probe_budget = 0;
	int injection_probe_budget = 16384;
	RadianceNeighborPattern radiance_neighbor_pattern = RADIANCE_NEIGHBOR_PATTERN_DITHERED_4;
	float energy = 1.0;
	int priority = 0;
	float edge_blend_distance = 1.0;
	bool debug_draw = false;
	DebugMode debug_mode = DEBUG_MODE_LOCAL_TRANSFER;
	float debug_probe_scale = 0.1;
	LocalLRTBuilder *builder = nullptr;
	Vector<Vector4> global_visibility;
	Vector<Vector4> injection;
	Vector<Vector4> analytic_lights;
	Vector<Vector4> shadowed_injection;
	Vector<Vector4> environment_injection;
	Vector<Vector4> radiance;
	Vector<float> shadow_visibility;
	Vector<GeometrySourceState> geometry_sources;
	int built_geometry_count = 0;
	int sdf_build_count = 0;
	int last_geometry_update_probe_count = 0;
	uint64_t last_geometry_update_usec = 0;
	uint64_t last_geometry_build_usec = 0;
	uint64_t last_geometry_pack_usec = 0;
	uint64_t last_geometry_upload_usec = 0;
	int last_geometry_update_frame_count = 0;
	uint64_t last_geometry_max_build_slice_usec = 0;
	bool geometry_update_pending = false;
	Vector<LocalLRTBuilder::TrunkRegion> pending_geometry_regions;
	int pending_geometry_region_index = 0;
	int pending_geometry_region_probe_index = 0;
	Vector3i pending_geometry_upload_begin;
	Vector3i pending_geometry_upload_end;
	int pending_geometry_probe_count = 0;
	int pending_geometry_update_frame_count = 0;
	uint64_t pending_geometry_source_usec = 0;
	uint64_t pending_geometry_build_usec = 0;
	uint64_t pending_geometry_max_build_slice_usec = 0;
	bool force_light_injection_update = false;
	bool gizmo_size_edit_active = false;
	MultiMeshInstance3D *debug_probe_instance = nullptr;
	Ref<MultiMesh> debug_probe_multimesh;

	Vector3i _calculate_resolution() const;
	bool _is_valid_probe_position(const Vector3i &p_grid_position) const;
	void _sync_grid();
	void _clear_built_data();
	AABB _get_collection_bounds() const;
	int _find_geometry_source(ObjectID p_object_id) const;
	bool _geometry_sdf_input_matches(const GeometrySourceState &p_a, const GeometrySourceState &p_b) const;
	bool _geometry_source_voxel_size_matches(const GeometrySourceState &p_state) const;
	bool _geometry_output_matches(const GeometrySourceState &p_a, const GeometrySourceState &p_b) const;
	AABB _get_source_influence_bounds(const LocalLRTColorSDF &p_sdf, const Transform3D &p_object_to_volume) const;
	void _collect_geometry_sources(Node *p_node, const Transform3D &p_world_to_volume, Vector<GeometrySourceState> &r_geometry);
	void _install_geometry_sources();
	bool _update_geometry_sources();
	bool _process_pending_geometry_update();
	void _upload_geometry_region(const Vector3i &p_begin, const Vector3i &p_end, uint64_t &r_pack_usec, uint64_t &r_upload_usec);
	void _collect_light_injection(Node *p_node, Vector<Vector4> &r_lights, bool p_inject_builder);
	void _sync_global_visibility_to_builder();
	void _ensure_debug_probe_instance();
	void _update_debug_probe_instances();
	Vector4 _get_probe_debug_injection(const Vector3i &p_position, int p_channel) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const;

	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;
	void begin_gizmo_size_edit();
	void end_gizmo_size_edit();

	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const;

	void set_geometry_voxel_size(float p_voxel_size);
	float get_geometry_voxel_size() const;
	void set_dynamic_update_probe_budget(int p_probe_budget);
	int get_dynamic_update_probe_budget() const;
	Vector3i get_resolution() const;
	Vector3 get_actual_probe_spacing() const;
	Vector3 get_probe_position(const Vector3i &p_grid_position) const;

	void set_visibility_iterations(int p_iterations);
	int get_visibility_iterations() const;
	void set_propagation_iterations(int p_iterations);
	int get_propagation_iterations() const;
	void set_visibility_probe_budget(int p_probe_budget);
	int get_visibility_probe_budget() const;
	void set_radiance_probe_budget(int p_probe_budget);
	int get_radiance_probe_budget() const;
	void set_injection_probe_budget(int p_probe_budget);
	int get_injection_probe_budget() const;
	void set_radiance_neighbor_pattern(RadianceNeighborPattern p_pattern);
	RadianceNeighborPattern get_radiance_neighbor_pattern() const;

	void set_energy(float p_energy);
	float get_energy() const;

	void set_priority(int p_priority);
	int get_priority() const;

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
	int get_sdf_build_count() const;
	int get_last_geometry_update_probe_count() const;
	uint64_t get_last_geometry_update_usec() const;
	uint64_t get_last_geometry_build_usec() const;
	uint64_t get_last_geometry_pack_usec() const;
	uint64_t get_last_geometry_upload_usec() const;
	int get_last_geometry_update_frame_count() const;
	uint64_t get_last_geometry_max_build_slice_usec() const;
	bool is_geometry_update_pending() const;
	bool is_probe_occupied(const Vector3i &p_grid_position) const;
	bool is_probe_inside_solid(const Vector3i &p_grid_position) const;
	real_t get_probe_signed_distance(const Vector3i &p_grid_position) const;
	real_t get_probe_coverage(const Vector3i &p_grid_position) const;
	Vector3 get_probe_surface_normal(const Vector3i &p_grid_position) const;
	Color get_probe_albedo(const Vector3i &p_grid_position) const;
	Color get_probe_emission(const Vector3i &p_grid_position) const;
	Vector4 get_probe_local_visibility(const Vector3i &p_grid_position) const;
	Color get_probe_transfer_color(const Vector3i &p_grid_position) const;
	Vector4 get_probe_global_visibility(const Vector3i &p_grid_position) const;
	Vector4 get_probe_injection(const Vector3i &p_grid_position, int p_channel) const;
	Vector4 get_probe_shadowed_injection(const Vector3i &p_grid_position, int p_channel) const;
	Vector4 get_probe_environment_injection(const Vector3i &p_grid_position, int p_channel) const;
	Color get_probe_injection_color(const Vector3i &p_grid_position) const;
	Vector4 get_probe_radiance(const Vector3i &p_grid_position, int p_channel) const;
	Color get_probe_radiance_color(const Vector3i &p_grid_position) const;
	real_t get_probe_shadow_visibility(const Vector3i &p_grid_position) const;
	bool has_gpu_data() const;
	void update_light_injection();
	void rebuild();

	LocalLRTVolume3D();
	~LocalLRTVolume3D();
};

VARIANT_ENUM_CAST(LocalLRTVolume3D::DebugMode);
VARIANT_ENUM_CAST(LocalLRTVolume3D::RadianceNeighborPattern);
