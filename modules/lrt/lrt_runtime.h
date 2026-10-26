/**************************************************************************/
/*  lrt_runtime.h                                                         */
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

#include "core/math/vector3i.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"

class LocalLRTVolume3D;
class LRTGeometryBinding3D;
class LRTRenderer;

class LRTRuntime : public Object {
	GDCLASS(LRTRuntime, Object);

	struct VolumeState;
	struct BindingState;
	struct Notification;
	static LRTRuntime *singleton;
	LRTRenderer *renderer = nullptr;

	HashSet<LocalLRTVolume3D *> volumes;
	HashSet<LRTGeometryBinding3D *> bindings;
	HashMap<LocalLRTVolume3D *, VolumeState *> states;
	HashMap<LRTGeometryBinding3D *, BindingState *> binding_states;
	uint64_t frame = 0;
	int frame_submitted = 0;
	int next_volume = 0;

	void _update();
	void _sync_bindings();
	void _sync_volume(LocalLRTVolume3D *p_volume, VolumeState &r_state, Vector<Notification> &r_notifications);
	void _poll_batch(LocalLRTVolume3D *p_volume, VolumeState &r_state, Vector<Notification> &r_notifications);

protected:
	static void _bind_methods() {}

public:
	static LRTRuntime *get_singleton() { return singleton; }

	void add_volume(LocalLRTVolume3D *p_volume);
	void remove_volume(LocalLRTVolume3D *p_volume);
	void add_binding(LRTGeometryBinding3D *p_binding);
	void remove_binding(LRTGeometryBinding3D *p_binding);

	Dictionary inspect_probe(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) const;
	void configure_debug(ObjectID p_selection, const Vector3i &p_probe, int p_axis, int p_slice, const Vector3 &p_direction);
	String get_debug_summary() const;
	Dictionary get_status(LocalLRTVolume3D *p_volume) const;
	Dictionary get_trunk(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) const;
	Dictionary get_gpu_trunk(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) const;

	LRTRuntime();
	~LRTRuntime();
};
