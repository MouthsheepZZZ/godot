/**************************************************************************/
/*  local_dynamic_gi_3d.cpp                                               */
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

#include "local_dynamic_gi_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/os/os.h"
#include "scene/3d/light_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

void LocalDynamicGI3D::_queue_update() {
	if (update_queued) {
		return;
	}
	update_queued = true;
	callable_mp(this, &LocalDynamicGI3D::_update_local_data_deferred).call_deferred();
}

void LocalDynamicGI3D::_update_local_data_deferred() {
	update_queued = false;
	update_local_data();
}

void LocalDynamicGI3D::_collect_descendants(Node *p_node) {
	const int child_count = p_node->get_child_count();
	for (int i = 0; i < child_count; i++) {
		Node *child = p_node->get_child(i);
		if (Object::cast_to<LocalDynamicGI3D>(child)) {
			continue;
		}

		GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(child);
		if (geometry && geometry->is_visible()) {
			if (geometry->get_gi_mode() == GeometryInstance3D::GI_MODE_DISABLED) {
				receive_only_geometry_ids.push_back(geometry->get_instance_id());
			} else {
				geometry_contributor_ids.push_back(geometry->get_instance_id());
			}
		}

		Light3D *light = Object::cast_to<Light3D>(child);
		if (light && light->is_visible()) {
			light_ids.push_back(light->get_instance_id());
		}

		_collect_descendants(child);
	}
}

Transform3D LocalDynamicGI3D::_get_transform_in_local_space(const Node3D *p_node) const {
	Transform3D xf;
	const Node3D *current = p_node;
	while (current && current != this) {
		xf = current->get_transform() * xf;
		current = Object::cast_to<Node3D>(current->get_parent());
	}
	return xf;
}

void LocalDynamicGI3D::_push_to_rendering_server() {
	ERR_FAIL_COND(local_dynamic_gi.is_null());
	RS::get_singleton()->local_dynamic_gi_set_enabled(local_dynamic_gi, enabled);
	RS::get_singleton()->local_dynamic_gi_set_extend(local_dynamic_gi, extend);
	RS::get_singleton()->local_dynamic_gi_set_blend_distance(local_dynamic_gi, blend_distance);
	RS::get_singleton()->local_dynamic_gi_set_local_bounds(local_dynamic_gi, local_bounds);
	if (is_inside_world() && get_world_3d().is_valid()) {
		RS::get_singleton()->local_dynamic_gi_set_transform(local_dynamic_gi, get_global_transform());
		RS::get_singleton()->local_dynamic_gi_set_scenario(local_dynamic_gi, get_world_3d()->get_scenario());
	}
}

void LocalDynamicGI3D::_on_child_tree_changed(Node *p_node) {
	_queue_update();
}

void LocalDynamicGI3D::update_local_data() {
	geometry_contributor_ids.clear();
	receive_only_geometry_ids.clear();
	light_ids.clear();

	AABB merged;
	bool has_contributor = false;
	_collect_descendants(this);

	for (const ObjectID &id : geometry_contributor_ids) {
		GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(id));
		if (!geometry) {
			continue;
		}
		const AABB local_aabb = _get_transform_in_local_space(geometry).xform(geometry->get_aabb());
		if (!has_contributor) {
			merged = local_aabb;
			has_contributor = true;
		} else {
			merged.merge_with(local_aabb);
		}
	}

	if (has_contributor) {
		merged.position -= extend;
		merged.size += extend * 2.0;
	}

	const bool bounds_changed = local_bounds != merged;
	local_bounds = merged;
	_push_to_rendering_server();

	if (bounds_changed) {
		update_gizmos();
	}
}

void LocalDynamicGI3D::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	if (local_dynamic_gi.is_valid()) {
		RS::get_singleton()->local_dynamic_gi_set_enabled(local_dynamic_gi, enabled);
	}
	update_gizmos();
	update_configuration_warnings();
}

bool LocalDynamicGI3D::is_enabled() const {
	return enabled;
}

void LocalDynamicGI3D::set_extend(const Vector3 &p_extend) {
	extend = p_extend.maxf(0.0);
	update_local_data();
}

Vector3 LocalDynamicGI3D::get_extend() const {
	return extend;
}

void LocalDynamicGI3D::set_blend_distance(float p_blend_distance) {
	blend_distance = MAX(p_blend_distance, 0.0f);
	if (local_dynamic_gi.is_valid()) {
		RS::get_singleton()->local_dynamic_gi_set_blend_distance(local_dynamic_gi, blend_distance);
	}
	update_gizmos();
}

float LocalDynamicGI3D::get_blend_distance() const {
	return blend_distance;
}

AABB LocalDynamicGI3D::get_local_bounds() const {
	return local_bounds;
}

TypedArray<GeometryInstance3D> LocalDynamicGI3D::get_geometry_contributors() const {
	TypedArray<GeometryInstance3D> contributors;
	for (const ObjectID &id : geometry_contributor_ids) {
		GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(id));
		if (geometry) {
			contributors.push_back(geometry);
		}
	}
	return contributors;
}

TypedArray<GeometryInstance3D> LocalDynamicGI3D::get_receive_only_geometry() const {
	TypedArray<GeometryInstance3D> receivers;
	for (const ObjectID &id : receive_only_geometry_ids) {
		GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(id));
		if (geometry) {
			receivers.push_back(geometry);
		}
	}
	return receivers;
}

TypedArray<Light3D> LocalDynamicGI3D::get_lights() const {
	TypedArray<Light3D> lights;
	for (const ObjectID &id : light_ids) {
		Light3D *light = Object::cast_to<Light3D>(ObjectDB::get_instance(id));
		if (light) {
			lights.push_back(light);
		}
	}
	return lights;
}

RID LocalDynamicGI3D::get_local_dynamic_gi_rid() const {
	return local_dynamic_gi;
}

AABB LocalDynamicGI3D::get_aabb() const {
	return local_bounds;
}

PackedStringArray LocalDynamicGI3D::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();

	if (OS::get_singleton()->get_current_rendering_method() != "forward_plus") {
		warnings.push_back(RTR("LocalDynamicGI3D is only supported when using the Forward+ renderer."));
	}
	if (!enabled) {
		warnings.push_back(RTR("LocalDynamicGI3D is disabled."));
	}
	if (geometry_contributor_ids.is_empty()) {
		warnings.push_back(RTR("No descendant GeometryInstance3D contributors found. Static or Dynamic GI mode meshes must be children of this node."));
	}

	return warnings;
}

void LocalDynamicGI3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(Engine::get_singleton()->is_editor_hint());
			if (!is_connected(SNAME("child_entered_tree"), callable_mp(this, &LocalDynamicGI3D::_on_child_tree_changed))) {
				connect(SNAME("child_entered_tree"), callable_mp(this, &LocalDynamicGI3D::_on_child_tree_changed));
				connect(SNAME("child_exiting_tree"), callable_mp(this, &LocalDynamicGI3D::_on_child_tree_changed));
			}
			update_local_data();
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			ERR_FAIL_COND(get_world_3d().is_null());
			RS::get_singleton()->local_dynamic_gi_set_scenario(local_dynamic_gi, get_world_3d()->get_scenario());
			RS::get_singleton()->local_dynamic_gi_set_transform(local_dynamic_gi, get_global_transform());
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (is_inside_tree()) {
				RS::get_singleton()->local_dynamic_gi_set_transform(local_dynamic_gi, get_global_transform());
			}
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			RS::get_singleton()->local_dynamic_gi_set_scenario(local_dynamic_gi, RID());
		} break;

		case NOTIFICATION_EXIT_TREE: {
			set_process_internal(false);
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			update_local_data();
		} break;
	}
}

void LocalDynamicGI3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LocalDynamicGI3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LocalDynamicGI3D::is_enabled);

	ClassDB::bind_method(D_METHOD("set_extend", "extend"), &LocalDynamicGI3D::set_extend);
	ClassDB::bind_method(D_METHOD("get_extend"), &LocalDynamicGI3D::get_extend);

	ClassDB::bind_method(D_METHOD("set_blend_distance", "blend_distance"), &LocalDynamicGI3D::set_blend_distance);
	ClassDB::bind_method(D_METHOD("get_blend_distance"), &LocalDynamicGI3D::get_blend_distance);

	ClassDB::bind_method(D_METHOD("update_local_data"), &LocalDynamicGI3D::update_local_data);
	ClassDB::bind_method(D_METHOD("get_local_bounds"), &LocalDynamicGI3D::get_local_bounds);
	ClassDB::bind_method(D_METHOD("get_geometry_contributors"), &LocalDynamicGI3D::get_geometry_contributors);
	ClassDB::bind_method(D_METHOD("get_receive_only_geometry"), &LocalDynamicGI3D::get_receive_only_geometry);
	ClassDB::bind_method(D_METHOD("get_lights"), &LocalDynamicGI3D::get_lights);
	ClassDB::bind_method(D_METHOD("get_local_dynamic_gi_rid"), &LocalDynamicGI3D::get_local_dynamic_gi_rid);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "extend", PROPERTY_HINT_RANGE, "0,64,0.001,or_greater,suffix:m"), "set_extend", "get_extend");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_distance", PROPERTY_HINT_RANGE, "0,64,0.001,or_greater,suffix:m"), "set_blend_distance", "get_blend_distance");
}

LocalDynamicGI3D::LocalDynamicGI3D() {
	local_dynamic_gi = RS::get_singleton()->local_dynamic_gi_create();
	set_notify_transform(true);
	_push_to_rendering_server();
}

LocalDynamicGI3D::~LocalDynamicGI3D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(local_dynamic_gi);
}
