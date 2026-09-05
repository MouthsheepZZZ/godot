/**************************************************************************/
/*  local_lrt.h                                                           */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/math/color.h"
#include "core/math/plane.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_radiance.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_visibility.glsl.gen.h"

#include <cstdint>

class LocalLrtInjectionShaderRD;
class LocalLrtEnvironmentShaderRD;
class LocalLrtDynamicGiBoundaryShaderRD;

namespace RendererRD {

class LocalLRT {
	struct Volume {
		bool enabled = true;
		Vector3 size = Vector3(10.0, 10.0, 10.0);
		Vector3i resolution = Vector3i(11, 11, 11);
		Transform3D transform;
		int visibility_iterations = 4;
		int radiance_iterations = 4;
		int visibility_probe_budget = 0;
		int radiance_probe_budget = 0;
		int radiance_neighbor_pattern = 1;
		float energy = 1.0;
		int priority = 0;
		float edge_blend_distance = 1.0;

		Vector<Vector4> local_visibility;
		RID local_visibility_buffer;
		RID local_transfer_buffer;
		RID mesh_light_buffer;
		RID global_visibility_buffers[2];
		RID radiance_buffers[2];
		RID direct_radiance_buffers[3];
		RID environment_data_buffer;
		RID environment_sh_buffer;
		RID environment_injection_buffer;
		RID dynamic_gi_boundary_buffer;
		RID inside_solid_buffer;
		RID analytic_lights_buffer;
		uint32_t analytic_lights_buffer_bytes = 0;
		Vector<Vector4> analytic_lights;
		Vector<Vector4> requested_analytic_lights;
		Vector<Vector4> environment_data;
		RID environment_sky_texture;
		int environment_mode = 0;
		RID shadow_visibility_buffer;
		RID shadow_matrix_buffer;
		RID shadow_depth_texture;
		RID shadow_upload_texture;
		RID shadow_framebuffer;
		int shadow_resolution = 1;
		float shadow_bias = 0.001f;
		bool shadow_enabled = false;
		bool shadow_use_upload = false;
		Transform3D shadow_camera;
		Projection shadow_projection;
		uint64_t shadow_caster_revision = 0;
		bool shadow_raster_state_valid = false;
		Vector3 directional_shadow_right;
		bool directional_shadow_right_valid = false;
		RID positional_shadow_texture;
		int positional_shadow_resolution = 1;
		int visibility_steps_remaining = 0;
		int visibility_probe_offset = 0;
		int radiance_steps_remaining = 0;
		int radiance_probe_offset = 0;
		int radiance_pattern_phase = 0;
		bool global_visibility_is_a = true;
		bool radiance_is_a = true;
		int direct_current_index = 0;
		int direct_pinned_index = -1;
		uint64_t direct_revision = 0;
		uint64_t direct_buffer_revisions[3] = {};
		uint64_t propagation_direct_revision = 0;
		bool injection_dirty = true;
		bool dynamic_gi_boundary_enabled = false;
		bool dynamic_gi_boundary_requested = false;
		RID dynamic_gi_diffuse_texture;
		Vector<RID> dynamic_gi_occlusion_textures;
		RID dynamic_gi_ubo;
		Vector3 dynamic_gi_camera_origin;
		int dynamic_gi_lightprobe_oct_size = 0;
		RID requested_dynamic_gi_diffuse_texture;
		Vector<RID> requested_dynamic_gi_occlusion_textures;
		RID requested_dynamic_gi_ubo;
		Vector3 requested_dynamic_gi_camera_origin;
		int requested_dynamic_gi_lightprobe_oct_size = 0;
	};

	struct VisibilityPushConstant {
		int32_t resolution[3];
		int32_t probe_count;
		int32_t probe_offset;
		int32_t dispatch_probe_count;
	};

	struct DynamicGIBoundaryPushConstant {
		int32_t resolution[3];
		int32_t probe_count;
		float size[3];
		int32_t lightprobe_oct_size;
		float xform_x[4];
		float xform_y[4];
		float xform_z[4];
		float xform_origin[4];
		float camera_origin[3];
		float pad;
	};

	struct RadiancePushConstant {
		int32_t resolution[3];
		int32_t probe_count;
		float probe_spacing[3];
		float decay_per_meter;
		int32_t probe_offset;
		int32_t dispatch_probe_count;
		int32_t neighbor_pattern;
		int32_t pattern_phase;
	};

	struct InjectionPushConstant {
		int32_t resolution[3];
		int32_t probe_count;
		float size[3];
		int32_t light_count;
		float xform_x[4];
		float xform_y[4];
		float xform_z[4];
		float xform_origin[4];
		float directional_shadow_bias;
		int32_t directional_shadow_enabled;
		int32_t directional_shadow_resolution;
		int32_t positional_shadow_resolution;
		int32_t probe_offset;
		int32_t dispatch_probe_count;
		int32_t dynamic_gi_boundary_enabled;
		int32_t pad;
	};

	mutable RID_Owner<Volume, true> volume_owner;
	LocalLrtVisibilityShaderRD *visibility_shader = nullptr;
	RID visibility_shader_version;
	RID visibility_pipeline;
	bool visibility_shader_initialized = false;
	LocalLrtRadianceShaderRD *radiance_shader = nullptr;
	RID radiance_shader_version;
	RID radiance_pipeline;
	bool radiance_shader_initialized = false;
	LocalLrtInjectionShaderRD *injection_shader = nullptr;
	RID injection_shader_version;
	RID injection_pipeline;
	bool injection_shader_initialized = false;
	LocalLrtEnvironmentShaderRD *environment_shader = nullptr;
	RID environment_shader_version;
	RID environment_pipelines[2];
	bool environment_shader_initialized = false;
	LocalLrtDynamicGiBoundaryShaderRD *dynamic_gi_boundary_shader = nullptr;
	RID dynamic_gi_boundary_shader_version;
	RID dynamic_gi_boundary_pipeline;
	bool dynamic_gi_boundary_shader_initialized = false;
	int transfer_format = 3;
	RID default_shadow_texture;
	RID default_sky_textures[2];

	static constexpr int DIRECTIONAL_SHADOW_SIZE = 512;

	bool _ensure_visibility_shader();
	bool _ensure_radiance_shader();
	bool _ensure_injection_shader();
	bool _ensure_environment_shader();
	bool _ensure_dynamic_gi_boundary_shader();
	void _ensure_default_shadow_texture();
	void _ensure_default_sky_textures();
	void _ensure_shadow_visibility_buffer(Volume &r_volume);
	void _ensure_raster_shadow(Volume &r_volume);
	void _upload_shadow_matrix(Volume &r_volume, const Projection &p_view_proj);
	RID _shadow_sample_texture(const Volume &p_volume) const;
	void _free_gpu_resources(Volume &r_volume);
	RID _create_vector4_buffer(const Vector<Vector4> &p_values);
	void _update_vector4_buffer(RID p_buffer, const Vector<Vector4> &p_values);
	void _update_transfer_buffer(RID p_buffer, const Vector<Vector4> &p_values);
	void _reset_direct_radiance(Volume &r_volume);
	RID _create_transfer_buffer(const Vector<Vector4> &p_values);
	Vector<uint8_t> _pack_transfer(const Vector<Vector4> &p_values) const;
	int _transfer_uints_per_probe() const;
	RID _create_uint_buffer(const Vector<uint32_t> &p_values);
	RID _create_float_buffer(int p_value_count);
	Vector<Vector4> _read_vector4_buffer(RID p_buffer, int p_value_count) const;
	Vector<float> _read_float_buffer(RID p_buffer, int p_value_count) const;
	void _reset_visibility(Volume &r_volume);
	void _reset_radiance(Volume &r_volume);
	void _mark_visibility_dirty(Volume &r_volume);
	void _propagate_visibility(Volume &r_volume, int p_iterations);
	void _propagate_radiance(Volume &r_volume, int p_iterations);
	bool _volume_has_pending_work(const Volume &p_volume) const;
	void _update_environment_sh(Volume &r_volume, RID p_sky_texture, bool p_sky_texture_is_array, const Color &p_ambient_color, float p_sky_mix, float p_sky_energy, const Basis &p_sky_orientation, float p_sky_border_size);
	void _inject_analytic_lights(Volume &r_volume, const Vector<Vector4> &p_lights);
	void _update_dynamic_gi_boundary(Volume &r_volume);

public:
	struct SurfaceData {
		Transform3D world_to_local;
		Vector3 size;
		Vector3i resolution;
		float energy = 0.0f;
		float edge_blend_distance = 0.0f;
		RID global_visibility_buffer;
		RID direct_radiance_buffer;
		RID indirect_radiance_buffer;
		RID inside_solid_buffer;
	};

	RID volume_allocate();
	void volume_initialize(RID p_volume);
	void volume_free(RID p_volume);
	bool owns_volume(RID p_volume) const;

	static constexpr int MAX_SURFACE_VOLUMES = 8;

	void volume_set_enabled(RID p_volume, bool p_enabled);
	void volume_set_grid(RID p_volume, const Vector3 &p_size, const Vector3i &p_resolution);
	void volume_set_transform(RID p_volume, const Transform3D &p_transform);
	void volume_set_visibility_iterations(RID p_volume, int p_iterations);
	void volume_set_propagation_iterations(RID p_volume, int p_iterations);
	void volume_set_visibility_probe_budget(RID p_volume, int p_probe_budget);
	void volume_set_radiance_probe_budget(RID p_volume, int p_probe_budget);
	void volume_set_radiance_neighbor_pattern(RID p_volume, int p_pattern);
	void volume_set_energy(RID p_volume, float p_energy);
	void volume_set_priority(RID p_volume, int p_priority);
	void volume_set_edge_blend_distance(RID p_volume, float p_distance);
	void volume_set_static_data(RID p_volume, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light);
	void volume_update_static_data(RID p_volume, const Vector3i &p_begin, const Vector3i &p_size, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light, const Vector<int> &p_inside_solid);
	void volume_set_inside_solid(RID p_volume, const Vector<int> &p_inside_solid);
	void volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection);
	void volume_set_environment(RID p_volume, RID p_sky_texture, bool p_sky_texture_is_array, const Color &p_ambient_color, float p_sky_mix, float p_sky_energy, const Basis &p_sky_orientation, float p_sky_border_size);
	void volume_set_dynamic_gi_boundary(RID p_volume, bool p_enabled, RID p_diffuse_texture, const Vector<RID> &p_occlusion_textures, RID p_hddagi_ubo, const Vector3 &p_camera_origin, int p_lightprobe_oct_size);
	void volume_inject_analytic_lights(RID p_volume, const Vector<Vector4> &p_lights);
	void volume_set_directional_shadow(RID p_volume, const Vector<float> &p_depths, int p_size, const Transform3D &p_camera, const Projection &p_projection, float p_bias);
	RID volume_prepare_raster_shadow(RID p_volume, const Transform3D &p_camera, const Projection &p_projection, float p_bias, uint64_t p_caster_revision, bool p_cacheable);
	void volume_clear_directional_shadow(RID p_volume);
	Vector3 volume_get_directional_shadow_right(RID p_volume) const;
	void volume_set_directional_shadow_right(RID p_volume, const Vector3 &p_right);
	void volume_set_positional_shadow_atlas(RID p_volume, RID p_texture, int p_resolution);
	void volume_propagate_visibility(RID p_volume);
	void volume_propagate_radiance(RID p_volume);
	bool volume_has_pending_work(RID p_volume) const;

	RID get_first_enabled_volume() const;
	Vector<RID> get_enabled_volumes() const;
	Vector<RID> get_sorted_enabled_volumes() const;
	Vector<RID> get_camera_volumes(int p_max, const Plane *p_frustum_planes = nullptr, int p_plane_count = 0) const;
	AABB volume_get_world_aabb(RID p_volume) const;
	AABB volume_get_bounds(RID p_volume) const;
	Vector<Vector4> volume_get_global_visibility(RID p_volume) const;
	Vector<Vector4> volume_get_injection(RID p_volume) const;
	Vector<Vector4> volume_get_environment_injection(RID p_volume) const;
	Vector<Vector4> volume_get_dynamic_gi_boundary(RID p_volume) const;
	Vector<Vector4> volume_get_radiance(RID p_volume) const;
	Vector<float> volume_get_shadow_visibility(RID p_volume) const;
	bool volume_has_gpu_resources(RID p_volume) const;
	int get_surface_data(SurfaceData *r_data, int p_max, const Plane *p_frustum_planes = nullptr, int p_plane_count = 0) const;

	LocalLRT();
	~LocalLRT();
};

} // namespace RendererRD
