/**************************************************************************/
/*  lrt_render_bridge.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

#include "lrt_volume.h"

#include "core/math/transform_3d.h"
#include "core/math/projection.h"
#include "core/math/rect2.h"
#include "core/math/vector2.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/object/object_id.h"
#include "core/templates/paged_array.h"
#include "core/templates/rid.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server_enums.h"

// Per-Volume render state, ordered GI composition and read-only native lighting inputs.
class LRTRenderBridge {
public:
	enum ExternalGIStatus {
		EXTERNAL_GI_UNAVAILABLE,
		EXTERNAL_GI_READY,
		EXTERNAL_GI_FAILED,
	};
	static ExternalGIStatus get_external_gi_status(ObjectID p_owner);
	struct State {
		ObjectID owner;
		RID scenario;
		int priority = 0;
		PackedInt32Array tree_order;
		Transform3D world_to_volume;
		Vector3 volume_min;
		Vector3 volume_max;
		Vector3 grid_min;
		Vector3i grid_size;
		Vector2 atlas_size;
		RID environment;
		float spacing = 0.25f;
		float blend_distance = 0.0f;
		bool external_gi_enabled = false;
		bool enabled = false;
		uint64_t local_field_version = 0;
		RID diagnostic_dirty;
		RID external_gi;
		Ref<LRTVolume> solver;
		uint64_t revision = 0;
	};


	static void set_state(const Dictionary &p_state);
	static void clear(ObjectID p_owner);
	static const State &get_state();
	static const Vector<State> &get_states();
	static void set_view_scenario(RID p_scenario);
	static bool is_current_volume(ObjectID p_owner);
	static bool has_visible_volume(const Projection &p_projection, const Transform3D &p_camera_transform);
	static void _debug_draw(const State &state, RID p_framebuffer, const Projection &p_camera_with_transform, RSE::ViewportDebugDraw p_mode);
	static void _debug_draw_geometry(const State &state, RID p_framebuffer, const Projection &p_camera_with_transform);
	static void debug_draw(RID p_framebuffer, const Projection &p_camera_with_transform, RSE::ViewportDebugDraw p_mode);
	static void _capture_external_gi(const State &state, RID p_environment, const Ref<RenderSceneBuffersRD> &p_buffers);
	static void capture_external_gi(RID p_environment, const Ref<RenderSceneBuffersRD> &p_buffers);
	static bool gather_screen(RID p_depth, RID p_normal_roughness, RID p_lighting_output, RID p_sky_output,
			const Size2i &p_full_size, const Size2i &p_gather_size,
			const Projection &p_projection, const Transform3D &p_camera_transform);
	static bool _gather_volume(const State &state, RID p_lrt_ubo,
			RID p_depth, RID p_normal_roughness, RID p_lighting_output, RID p_sky_output,
			const Size2i &p_full_size, const Size2i &p_gather_size, const Projection &p_projection, const Transform3D &p_camera_transform);
	static uint64_t get_external_gi_capture_count(ObjectID p_owner);
	static bool is_external_gi_capture_valid(ObjectID p_owner);
	static Dictionary get_performance_stats(ObjectID p_owner);
	static Dictionary get_gather_resources(ObjectID p_owner);
	static bool ensure_screen_gather_buffers(RenderSceneBuffersRD *p_buffers, uint64_t &r_bytes);
	static void release_screen_gather_buffers(RenderSceneBuffersRD *p_buffers, uint64_t &r_bytes);
	static RID get_forward_uniform_buffer(bool p_enabled, RSE::ViewportDebugDraw p_mode);
	static void set_performance_profiling_enabled(bool p_enabled);
	static void free_external_gi_resources();

	static void defer_native_light_resolve(const Callable &p_callable);
	static void flush_deferred_light_resolves(const Transform3D &p_camera_transform);
	static bool is_native_lighting_ready(ObjectID p_owner);
	static Transform3D get_native_lighting_camera();
	static Transform3D get_volume_to_world(ObjectID p_owner);

	struct PositionalShadowSample {
		RID texture;
		Rect2 atlas_rect;
		Vector2 flip_offset;
		Projection shadow_camera;
		float shadow_bias = 0.0f;
		bool valid = false;
		bool omni = true;
		bool area = false;
	};

	static void reset_positional_shadow_atlas();
	static void bind_positional_shadow_atlas(RID p_atlas);
	static RID get_native_light_instance(RID p_scene_light_instance);
	static PositionalShadowSample get_positional_shadow_sample(RID p_scene_light_instance);

	struct AreaLightAtlasSample {
		RID texture;
		Rect2 projector_rect;
		float max_mipmap = 0.0f;
		bool valid = false;
	};

	static AreaLightAtlasSample get_area_light_atlas_sample(RID p_scene_light_instance);

	// A spot or omni projector lives in the renderer's decal atlas. The rect is packed exactly the
	// way the clustered shader expects it for each light type, so LRT can reproduce the native
	// sampling instead of rendering the light through a capture viewport.
	struct LightProjectorSample {
		RID texture;
		Vector4 rect;
		bool valid = false;
	};

	static LightProjectorSample get_light_projector_sample(RID p_scene_light_instance);

	static uint64_t get_deferred_resolve_flushes();
};
