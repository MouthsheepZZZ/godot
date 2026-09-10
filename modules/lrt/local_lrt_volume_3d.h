#pragma once

#include "scene/3d/node_3d.h"

class MeshInstance3D;
class MultiMeshInstance3D;
class ImageTexture3D;
class Light3D;

class LocalLRTVolume3D : public Node3D {
	GDCLASS(LocalLRTVolume3D, Node3D);

public:
	enum DebugMode {
		DEBUG_DISABLED,
		DEBUG_DISTANCE,
		DEBUG_INSIDE_OUTSIDE,
		DEBUG_SURFACE_COLOR,
	};

private:
	struct Grid {
		Vector3i size;
		Vector3 origin;
		float cell_size = 1.0f;
		Vector<uint8_t> surface;
		Vector<uint8_t> closed_surface;
		Vector<float> distance;
		Vector<float> surface_distance_squared;
		Vector<int32_t> nearest_surface;
		Vector<Vector3> surface_normal;
		Vector<uint64_t> surface_owner;
		Vector<Color> color;
		Vector<uint16_t> color_weight;
	};
	struct SH4 {
		Color red;
		Color green;
		Color blue;
	};
	struct TransportLink {
		float direction[3] = {};
		uint32_t source = UINT32_MAX;
	};

	Vector3 volume_size = Vector3(10, 10, 10);
	int sdf_resolution = 48;
	int color_resolution = 48;
	DebugMode debug_mode = DEBUG_DISTANCE;
	int debug_slice_axis = 2;
	float debug_slice_position = 0.5f;
	int max_debug_cells = 65536;
	String bake_status = "Not baked";
	Grid sdf_grid;
	Grid color_grid;
	Vector<Color> direct_outgoing;
	Vector<Color> propagation_albedo;
	Vector<Color> propagation_normal;
	Vector<TransportLink> transport_links;
	Vector<SH4> radiance_sh;
	Vector<Color> sky_visibility_sh;
	Ref<ImageTexture3D> irradiance_textures[3];
	Ref<ImageTexture3D> sky_visibility_texture;
	int propagation_iterations = 4;
	float bounce_feedback = 0.85f;
	float sky_energy = 0.0f;
	bool lighting_enabled = true;
	bool indirect_only = false;
	bool lighting_cleared = false;
	MultiMeshInstance3D *debug_instance = nullptr;

	static int _index(const Vector3i &p, const Vector3i &p_size);
	Grid _create_grid(int p_resolution) const;
	bool _collect_meshes(Node *p_node, Vector<MeshInstance3D *> &r_meshes) const;
	void _collect_lights(Node *p_node, Vector<Light3D *> &r_lights) const;
	bool _rasterize_mesh(MeshInstance3D *p_mesh_instance, Grid &r_sdf, Grid &r_color, String &r_error);
	void _compute_distance(Grid &r_grid);
	void _classify_inside(Grid &r_grid);
	void _publish_lighting();
	Error _run_gpu_propagation(int p_iterations);
	Error _upload_sh_textures();
	void _update_debug();
	void _clear_debug();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_volume_size(const Vector3 &p_size);
	Vector3 get_volume_size() const;
	void set_sdf_resolution(int p_resolution);
	int get_sdf_resolution() const;
	void set_color_resolution(int p_resolution);
	int get_color_resolution() const;
	void set_debug_mode(DebugMode p_mode);
	DebugMode get_debug_mode() const;
	void set_debug_slice_axis(int p_axis);
	int get_debug_slice_axis() const;
	void set_debug_slice_position(float p_position);
	float get_debug_slice_position() const;
	void set_max_debug_cells(int p_count);
	int get_max_debug_cells() const;
	String get_bake_status() const;
	Dictionary get_field_summary() const;
	Dictionary sample_nearest_surface(const Vector3 &p_local_position) const;
	float sample_sdf(const Vector3 &p_local_position) const;
	Color sample_surface_color(const Vector3 &p_local_position) const;
	Color sample_first_bounce(const Vector3 &p_local_position) const;
	float sample_sky_visibility(const Vector3 &p_local_position, const Vector3 &p_local_normal) const;
	Error bake();
	void clear();
	Error inject_first_bounce();
	Error propagate(int p_iterations = 1);
	void clear_lighting();
	void set_lighting_enabled(bool p_enabled);
	bool is_lighting_enabled() const;
	void set_indirect_only(bool p_enabled);
	bool is_indirect_only() const;
	void set_propagation_iterations(int p_iterations);
	int get_propagation_iterations() const;
	void set_bounce_feedback(float p_feedback);
	float get_bounce_feedback() const;
	void set_sky_energy(float p_energy);
	float get_sky_energy() const;

	PackedStringArray get_configuration_warnings() const override;
	~LocalLRTVolume3D();
};

VARIANT_ENUM_CAST(LocalLRTVolume3D::DebugMode);
