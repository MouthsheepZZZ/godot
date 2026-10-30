/**************************************************************************/
/*  lrt_bake_task.h                                                  */
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

#include "core/io/resource.h"
#include "core/object/object_id.h"

class GeometryInstance3D;
class LRTVolume3D;
class LRTGeometryBinding;
class Node;

// One explicit editor/runtime bake. Scene access and publication stay on main.
class LRTBakeTask : public RefCounted {
	GDCLASS(LRTBakeTask, RefCounted);

	struct Data;
	Data *data = nullptr;
	void _collect();
	void _work();
	void _finish();
	void _cleanup();

protected:
	static void _bind_methods();

public:
	enum Status {
		COLLECTING,
		RUNNING,
		SUCCEEDED,
		COMPLETED_WITH_ISSUES,
		FAILED,
		CANCELLED,
	};

	static Ref<LRTBakeTask> start_volume(LRTVolume3D *p_volume, Node *p_root);
	static Ref<LRTBakeTask> start_geometry(const Ref<LRTGeometryBinding> &p_binding, GeometryInstance3D *p_instance);
	static void process();
	static bool has_pending();
	static void cancel_all();
	static void finish_shutdown();
	static void retire_cache(Ref<RefCounted> &r_cache);

	bool cancel();
	bool is_completed() const;
	Status get_status() const;
	Dictionary get_progress() const;
	Ref<Resource> get_result() const;
	Array get_issues() const;

	LRTBakeTask();
	~LRTBakeTask();
};

VARIANT_ENUM_CAST(LRTBakeTask::Status);
