/**************************************************************************/
/*  lrt_render_bridge.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/math/projection.h"
#include "core/math/vector2.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/object/object_id.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"

// Render-thread state shared by the scene node and Forward+. The single production volume
// remains authoritative through R11; array/priority composition is introduced in N5-M1.
class LRTRenderBridge {
public:
	struct State {
		ObjectID owner;
		Transform3D world_to_volume;
		Vector3 volume_min;
		Vector3 volume_max;
		Vector3 grid_min;
		Vector3i grid_size;
		Vector2 atlas_size;
		RID environment;
		float spacing = 0.25f;
		float blend_distance = 0.0f;
		bool blur_sampling = true;
		bool display_blend_enabled = true;
		bool external_gi_enabled = false;
		bool enabled = false;
		RID radiance_r;
		RID radiance_g;
		RID radiance_b;
		RID visibility;
		RID material;
		RID links;
		RID sky_r;
		RID sky_g;
		RID sky_b;
		RID source_r;
		RID source_g;
		RID source_b;
		RID local_visibility;
		RID matrices;
		RID diagnostic_sdf;
		RID diagnostic_albedo;
		RID diagnostic_emission;
		RID diagnostic_dirty;
		RID external_gi_r;
		RID external_gi_g;
		RID external_gi_b;
		RID receiver_buffer;
		int receiver_count = 0;
		uint64_t revision = 0;
	};

	static void set_state(const Dictionary &p_state);
	static void clear(ObjectID p_owner);
	static const State &get_state();
	static void debug_draw(RID p_framebuffer, const Projection &p_camera_with_transform, RSE::ViewportDebugDraw p_mode);
	static void capture_external_gi(RID p_environment, RID p_hddagi_ubo, RID p_diffuse,
			RID p_occlusion_0, RID p_occlusion_1, const Vector3 &p_camera_origin);
	static bool gather_screen(RID p_lrt_ubo, RID p_depth, RID p_normal_roughness,
			RID p_lighting_output, RID p_geometry_output, const Size2i &p_full_size,
			const Projection &p_projection, const Transform3D &p_camera_transform);
	static uint64_t get_external_gi_capture_count(ObjectID p_owner);
	static bool is_external_gi_capture_valid(ObjectID p_owner);
	static Dictionary get_performance_stats(ObjectID p_owner);
	static void set_performance_profiling_enabled(bool p_enabled);
	static void free_external_gi_resources();
};
