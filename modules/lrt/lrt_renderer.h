/**************************************************************************/
/*  lrt_renderer.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "lrt_geometry.h"

#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/hash_set.h"

class RenderDataRD;
class RendererSceneCull;

class LRTRenderer : public Object {
	GDCLASS(LRTRenderer, Object);

	static LRTRenderer *singleton;
	struct State;
	RID material_bake_buffer;
	State *state = nullptr; // Accessed only on the rendering thread.
	mutable Mutex status_mutex;
	HashMap<ObjectID, Dictionary> statuses;
	Dictionary readback_result;
	ObjectID debug_selection; // Main-thread editor selection and controls.

	void _publish(uint64_t p_volume, uint64_t p_revision, const Vector3i &p_grid, const PackedByteArray &p_updates, bool p_reset, bool p_baseline_changed, const PackedInt32Array &p_emission_trunks);
	void _prepare(const Array &p_volumes, int p_iterations, uint64_t p_frame, const Dictionary &p_debug, const Dictionary &p_surface_profiles, const PackedByteArray &p_surface_profile_data);
	bool _inject(uint64_t p_volume, RID p_lights, uint32_t p_light_count, RID p_sky, RID p_shadow, RID p_directional);
	bool _propagate(const PackedInt64Array &p_volumes, int p_iterations, uint64_t p_frame, uint32_t p_phase, bool p_recovering);
	void _remove(uint64_t p_volume);
	void _clear();
	void _fail(uint64_t p_volume, const String &p_error);
	void _readback(uint64_t p_volume, const Vector3i &p_coordinate);
	void _publish_positional_status(RID p_scenario);

protected:
	static void _bind_methods() {}

public:
	// Emission metadata contains pairs of linear trunk index and actual nonzero-emission flag.
	static PackedByteArray pack_updates(const LRTGeometry::StaticCache &p_baseline, const Vector<LRTGeometry::Trunk> &p_trunks, bool p_reset, PackedInt32Array &r_emission_trunks);
	void submit_prepared(ObjectID p_volume, uint64_t p_revision, const Vector3i &p_grid, const PackedByteArray &p_updates, bool p_reset, bool p_baseline_changed, const PackedInt32Array &p_emission_trunks);
	void prepare(const Array &p_volumes, int p_iterations, uint64_t p_frame, const Dictionary &p_surface_profiles, const PackedByteArray &p_surface_profile_data);
	uint32_t get_surface_profile(ObjectID p_instance) const;
	static LRTRenderer *get_singleton() { return singleton; }
	bool has_active_volumes() const;
	bool has_active_scenario() const;
	void set_scene(RID p_scenario, const Vector<RID> &p_lights);
	enum PositionalShadowStatus {
		POSITIONAL_SHADOW_PENDING,
		POSITIONAL_SHADOW_WAITING,
		POSITIONAL_SHADOW_FAILED,
		POSITIONAL_SHADOW_SUBMITTED,
	};

	struct PositionalShadowInfo {
		RID source_instance;
		RID shadow_instance;
		RID shadow_atlas;
		bool shadowed = false;
		bool current = false;
		uint32_t pass_count = 0;
		uint32_t submitted_passes = 0;
		uint64_t source_revision = 0;
		uint64_t submitted_frame = UINT64_MAX;
		PositionalShadowStatus status = POSITIONAL_SHADOW_PENDING;
		bool animated_material = false;
		HashSet<RID> casters;
	};

	Error prepare_positional_shadows(RendererSceneCull *p_cull, RID p_scenario, const Vector3 &p_center, uint64_t p_visibility_viewport_mask);
	uint32_t reserve_positional_shadow_passes(RID p_scenario) const;
	void schedule_positional_shadows(RendererSceneCull *p_cull, RID p_scenario);
	void finish_positional_shadows();
	uint64_t get_positional_scenario_id() const;
	Vector<RID> get_positional_shadow_candidates(RID p_scenario) const;
	Error submit_positional_shadow(RendererSceneCull *p_cull, RID p_scenario, RID p_source_instance);
	Error collect_positional_shadow(RID p_scenario, RID p_source_instance, PositionalShadowInfo &r_info) const;
	void remove_scenario(RID p_scenario);
	void prepare_sun_shadows(RendererSceneCull *p_cull, RID p_scenario, const Vector3 &p_center);
	void submit_sun_shadow(RendererSceneCull *p_cull, RID p_scenario, uint32_t p_index);
	void capture_sun_shadow(RID p_framebuffer, bool p_begin);
	void render(RenderDataRD *p_render_data);
	bool needs_gather() const;
	RID debug_render(const RenderDataRD *p_render_data);
	void set_debug_selection(ObjectID p_selection);
	void gather(RenderDataRD *p_render_data, const RID *p_normals);
	RID get_gather_texture(const RenderDataRD *p_render_data, const StringName &p_name);
	RID get_material_bake_buffer();
	RID set_material_bake_buffer(RID p_buffer) {
		RID previous = material_bake_buffer;
		material_bake_buffer = p_buffer;
		return previous;
	}
	RID get_draw_metadata();
	RID get_draw_samples();

	LRTRenderer();
	void remove(ObjectID p_volume);
	Dictionary get_status(ObjectID p_volume) const;
	Dictionary readback(ObjectID p_volume, const Vector3i &p_coordinate);

	~LRTRenderer();
};
