/**************************************************************************/
/*  local_lrt.h                                                           */
/**************************************************************************/

#pragma once

#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_radiance.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_lrt_visibility.glsl.gen.h"

#include <cstdint>

namespace RendererRD {

class LocalLRT {
	struct Volume {
		bool enabled = true;
		Vector3 size = Vector3(10.0, 10.0, 10.0);
		Vector3i resolution = Vector3i(11, 11, 11);
		Transform3D transform;
		int visibility_iterations = 4;
		int radiance_iterations = 4;
		float energy = 1.0;
		float edge_blend_distance = 1.0;

		Vector<Vector4> local_visibility;
		RID local_visibility_buffer;
		RID local_transfer_buffer;
		RID global_visibility_buffers[2];
		RID radiance_buffers[2];
		RID injection_buffer;
		RID emissive_injection_buffer;
		RID inside_solid_buffer;
		bool global_visibility_is_a = true;
		bool radiance_is_a = true;
	};

	struct VisibilityPushConstant {
		int32_t resolution[3];
		int32_t probe_count;
	};

	struct RadiancePushConstant {
		int32_t resolution[3];
		int32_t probe_count;
		float probe_spacing[3];
		float decay_per_meter;
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

	bool _ensure_visibility_shader();
	bool _ensure_radiance_shader();
	void _free_gpu_resources(Volume &r_volume);
	RID _create_vector4_buffer(const Vector<Vector4> &p_values);
	RID _create_uint_buffer(const Vector<uint32_t> &p_values);
	Vector<Vector4> _read_vector4_buffer(RID p_buffer, int p_value_count) const;
	void _reset_and_propagate_visibility(Volume &r_volume);
	void _propagate_radiance(Volume &r_volume, int p_iterations);

public:
	struct SurfaceData {
		Transform3D world_to_local;
		Vector3 size;
		Vector3i resolution;
		float energy = 0.0f;
		float edge_blend_distance = 0.0f;
		RID local_visibility_buffer;
		RID radiance_buffer;
		RID inside_solid_buffer;
	};

	RID volume_allocate();
	void volume_initialize(RID p_volume);
	void volume_free(RID p_volume);
	bool owns_volume(RID p_volume) const;

	void volume_set_enabled(RID p_volume, bool p_enabled);
	void volume_set_grid(RID p_volume, const Vector3 &p_size, const Vector3i &p_resolution);
	void volume_set_transform(RID p_volume, const Transform3D &p_transform);
	void volume_set_visibility_iterations(RID p_volume, int p_iterations);
	void volume_set_propagation_iterations(RID p_volume, int p_iterations);
	void volume_set_energy(RID p_volume, float p_energy);
	void volume_set_edge_blend_distance(RID p_volume, float p_distance);
	void volume_set_static_data(RID p_volume, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer);
	void volume_set_inside_solid(RID p_volume, const Vector<int> &p_inside_solid);
	void volume_set_injection(RID p_volume, const Vector<Vector4> &p_injection, const Vector<Vector4> &p_emissive_injection);
	void volume_propagate_radiance(RID p_volume);

	AABB volume_get_bounds(RID p_volume) const;
	Vector<Vector4> volume_get_global_visibility(RID p_volume) const;
	Vector<Vector4> volume_get_injection(RID p_volume) const;
	Vector<Vector4> volume_get_radiance(RID p_volume) const;
	bool volume_has_gpu_resources(RID p_volume) const;
	bool get_surface_data(SurfaceData &r_data) const;

	LocalLRT() = default;
	~LocalLRT();
};

} // namespace RendererRD
