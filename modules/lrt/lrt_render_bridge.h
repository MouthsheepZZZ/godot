/**************************************************************************/
/*  lrt_render_bridge.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#pragma once

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
#include "servers/rendering/rendering_server_enums.h"

// Render-thread state shared by the scene node and Forward+. The single production volume
// remains authoritative through R11; array/priority composition is introduced in N5-M1.
class LRTRenderBridge {
public:
	struct State {
		ObjectID owner;
		Transform3D world_to_volume;
		// Authored transform of the first shadow-casting directional light. The scene cull
		// instance transform is not reliable for directional lights, so the Volume shadow
		// camera is oriented from here.
		Transform3D directional_light_transform;
		bool has_directional_light = false;
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
		RID receiver_links;
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
		bool volume_shadow_requested = false;
	};

	static const int VOLUME_SHADOW_PASS = 16;
	static const int VOLUME_SHADOW_SIZE = 1024;

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

	static bool is_volume_shadow_requested();
	static void reset_volume_shadow_pass();
	static void set_volume_shadow_camera(RID p_light_instance, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, const Projection &p_shadow_matrix, bool p_use_pancake, bool p_reverse_cull);
	static bool get_volume_shadow_camera(RID &r_light_instance, Projection &r_projection, Transform3D &r_transform, float &r_zfar, bool &r_use_pancake, bool &r_reverse_cull);
	static RID ensure_volume_shadow_framebuffer();
	static RID get_volume_shadow_texture();
	static bool is_volume_shadow_valid();
	static Projection get_volume_shadow_matrix();
	static void mark_volume_shadow_rendered(uint32_t p_instance_count);
	static bool begin_volume_shadow_gpu_timing();
	static void end_volume_shadow_gpu_timing(bool p_active, double p_cpu_ms);
	static void defer_native_light_resolve(const Callable &p_callable);
	static void flush_deferred_light_resolves();

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
	static void bind_positional_shadow_atlas(RID p_atlas, const PagedArray<RID> *p_lights);
	static void register_positional_light(RID p_light, RID p_instance);
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

	struct VolumeShadowStats {
		uint32_t width = 0;
		uint32_t height = 0;
		float min_value = 1.0f;
		float max_value = 0.0f;
		float center_value = 0.0f;
		uint32_t min_x = 0;
		uint32_t min_y = 0;
		uint32_t max_x = 0;
		uint32_t max_y = 0;
		uint32_t written_pixels = 0;
		bool valid = false;
	};

	// Render-thread readback of the Volume directional shadow depth. Diagnostic only.
	static void read_volume_shadow_depth();
	static VolumeShadowStats get_last_volume_shadow_stats();
	static void set_volume_shadow_light_transform(const Transform3D &p_transform);
	static void set_volume_shadow_camera_debug(float p_radius, float p_pancake);
	// Marks the shadow map stale so a pending direct resolve waits for this frame's raster pass
	// instead of sampling the previous frame's map.
	static void invalidate_volume_shadow_frame();
	static uint64_t get_deferred_resolve_flushes();
	static void count_volume_positional_redraw();
	static uint64_t get_volume_positional_redraws();
	static void count_camera_positional_redraw();
	static uint64_t get_camera_positional_redraws();
	static void count_omni_positional_redraw();
	static uint64_t get_omni_positional_redraws();
	static void count_omni_shadow_caster(uint32_t p_instances);
	static uint32_t get_omni_shadow_caster_max();
	static void record_omni_dp_debug(const Vector3 &p_origin, uint32_t p_points);
	static Vector3 get_omni_dp_origin();
	static uint32_t get_omni_dp_points();
};
