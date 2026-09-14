/**************************************************************************/
/*  lrt_render_bridge.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/object/object_id.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Render-thread state shared by the scene node and Forward+. R10 still owns one production
// volume; the array/priority representation is introduced with R11.
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
		int mode = 0;
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
		RID external_gi_r;
		RID external_gi_g;
		RID external_gi_b;
		uint64_t revision = 0;
	};

	static void set_state(const Dictionary &p_state);
	static void clear(ObjectID p_owner);
	static const State &get_state();
	static void capture_external_gi(RID p_environment, RID p_hddagi_ubo, RID p_diffuse,
			RID p_occlusion_0, RID p_occlusion_1, const Vector3 &p_camera_origin);
	static uint64_t get_external_gi_capture_count(ObjectID p_owner);
	static bool is_external_gi_capture_valid(ObjectID p_owner);
	static void free_external_gi_resources();
};
