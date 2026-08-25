/**************************************************************************/
/*  local_dynamic_gi_3d.h                                                 */
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

#include "core/templates/local_vector.h"
#include "scene/3d/visual_instance_3d.h"

class Light3D;

class LocalDynamicGI3D : public VisualInstance3D {
	GDCLASS(LocalDynamicGI3D, VisualInstance3D);

	RID local_dynamic_gi;
	bool enabled = true;
	Vector3 extend = Vector3(0.25, 0.25, 0.25);
	float blend_distance = 0.5;
	AABB local_bounds;
	bool update_queued = false;

	LocalVector<ObjectID> geometry_contributor_ids;
	LocalVector<ObjectID> receive_only_geometry_ids;
	LocalVector<ObjectID> light_ids;

	void _queue_update();
	void _update_local_data_deferred();
	void _collect_descendants(Node *p_node);
	Transform3D _get_transform_in_local_space(const Node3D *p_node) const;
	void _push_to_rendering_server();
	void _on_child_tree_changed(Node *p_node);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const;

	void set_extend(const Vector3 &p_extend);
	Vector3 get_extend() const;

	void set_blend_distance(float p_blend_distance);
	float get_blend_distance() const;

	void update_local_data();

	AABB get_local_bounds() const;
	TypedArray<GeometryInstance3D> get_geometry_contributors() const;
	TypedArray<GeometryInstance3D> get_receive_only_geometry() const;
	TypedArray<Light3D> get_lights() const;
	RID get_local_dynamic_gi_rid() const;

	virtual AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

	LocalDynamicGI3D();
	~LocalDynamicGI3D();
};
