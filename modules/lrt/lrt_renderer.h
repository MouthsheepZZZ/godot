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

class RenderDataRD;

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
	Vector3i debug_probe = Vector3i(-1, -1, -1);
	int debug_axis = -1;
	int debug_slice = 0;
	Vector3 debug_direction = Vector3(0, 1, 0);
	Dictionary debug_samples;

	void _publish(uint64_t p_volume, uint64_t p_revision, const Vector3i &p_grid, const PackedByteArray &p_updates, bool p_reset);
	void _prepare(const Array &p_volumes, int p_iterations, double p_relaxation, uint64_t p_frame, const Dictionary &p_debug);
	bool _inject(uint64_t p_volume, RID p_lights, uint32_t p_light_count, RID p_sky, RID p_shadow, RID p_directional);
	bool _propagate(const PackedInt64Array &p_volumes, int p_iterations, double p_relaxation, uint64_t p_frame);
	void _remove(uint64_t p_volume);
	void _clear();
	void _fail(uint64_t p_volume, const String &p_error);
	void _readback(uint64_t p_volume, const Vector3i &p_coordinate);

protected:
	static void _bind_methods() {}

public:
	void submit(ObjectID p_volume, uint64_t p_revision, const LRTGeometry::StaticCache &p_baseline, const Vector<LRTGeometry::Trunk> &p_trunks, bool p_reset);
	void prepare(const Array &p_volumes, int p_iterations, double p_relaxation, uint64_t p_frame);
	static LRTRenderer *get_singleton() { return singleton; }
	bool has_active_volumes() const;
	bool has_active_scenario() const;
	void set_scene(RID p_scenario, const Vector<RID> &p_lights);
	void render(RenderDataRD *p_render_data);
	bool needs_gather() const;
	RID debug_render(const RenderDataRD *p_render_data);
	void configure_debug(ObjectID p_selection, const Vector3i &p_probe, int p_axis, int p_slice, const Vector3 &p_direction, const Dictionary &p_samples);
	void gather(RenderDataRD *p_render_data, const RID *p_normals);
	RID get_gather_texture(const RenderDataRD *p_render_data, bool p_guide);
	RID get_material_bake_buffer();
	void set_material_bake_buffer(RID p_buffer) { material_bake_buffer = p_buffer; }
	RID get_draw_metadata();
	RID get_draw_samples();

	LRTRenderer();
	void remove(ObjectID p_volume);
	Dictionary get_status(ObjectID p_volume) const;
	Dictionary readback(ObjectID p_volume, const Vector3i &p_coordinate);

	~LRTRenderer();
};
