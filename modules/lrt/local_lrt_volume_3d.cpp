/**************************************************************************/
/*  local_lrt_volume_3d.cpp                                               */
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

#include "local_lrt_volume_3d.h"

#include "lrt_geometry_binding.h"
#include "lrt_runtime.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/main/scene_tree.h"

Ref<LRTBakeTask> LRTVolume3D::bake(Node *p_from_node) {
	ERR_FAIL_COND_V_MSG(Object::cast_to<LRTDetailVolume3D>(this), Ref<LRTBakeTask>(), "Bake a Detail through its parent calculation group.");
	Node *root = p_from_node;
	if (!root && get_tree()) {
#ifdef TOOLS_ENABLED
		if (Engine::get_singleton()->is_editor_hint()) {
			root = get_tree()->get_edited_scene_root();
		}
#endif
		if (!root) {
			root = get_tree()->get_current_scene();
		}
	}
	return LRTBakeTask::start_volume(this, root);
}

void LRTVolume3D::set_size(const Vector3 &p_size) {
	ERR_FAIL_COND(!p_size.is_finite());
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0 || p_size.z <= 0);
	size = p_size;
	update_gizmos();
	update_configuration_warnings();
}

void LRTVolume3D::set_probe_spacing(float p_spacing) {
	ERR_FAIL_COND(!Math::is_finite(p_spacing) || p_spacing <= 0);
	probe_spacing = p_spacing;
	update_gizmos();
	update_configuration_warnings();
}

void LRTVolume3D::set_priority(int p_priority) {
	priority = p_priority;
}

void LRTVolume3D::set_blend_width(float p_width) {
	ERR_FAIL_COND(!Math::is_finite(p_width) || p_width < 0);
	blend_width = p_width;
	update_gizmos();
}

void LRTVolume3D::set_intensity(float p_intensity) {
	ERR_FAIL_COND(!Math::is_finite(p_intensity) || p_intensity < 0);
	intensity = p_intensity;
}

void LRTVolume3D::set_bake_data(const Ref<LocalLRTVolumeData> &p_data) {
	if (bake_data == p_data) {
		return;
	}
	if (bake_data.is_valid()) {
		bake_data->disconnect_changed(callable_mp(this, &LRTVolume3D::_bake_data_changed));
	}
	bake_data = p_data;
	if (bake_data.is_valid()) {
		bake_data->connect_changed(callable_mp(this, &LRTVolume3D::_bake_data_changed));
	}
	_bake_data_changed();
}

void LRTVolume3D::_bake_data_changed() {
	LRTBakeTask::retire_cache(bake_cache);
	geometry_revision++;
	update_gizmos();
	update_configuration_warnings();
}

Dictionary LRTVolume3D::inspect_probe(const Vector3i &p_coordinate) const {
	return LRTRuntime::get_singleton()->inspect_probe(const_cast<LRTVolume3D *>(this), p_coordinate);
}

Dictionary LRTVolume3D::get_geometry_status() const {
	return LRTRuntime::get_singleton()->get_status(const_cast<LRTVolume3D *>(this));
}

Dictionary LRTVolume3D::get_geometry_trunk(const Vector3i &p_coordinate) const {
	return LRTRuntime::get_singleton()->get_trunk(const_cast<LRTVolume3D *>(this), p_coordinate);
}

Dictionary LRTVolume3D::get_geometry_gpu_trunk(const Vector3i &p_coordinate) const {
	return LRTRuntime::get_singleton()->get_gpu_trunk(const_cast<LRTVolume3D *>(this), p_coordinate);
}

Vector3i LRTVolume3D::get_grid_size() const {
	Vector3i result;
	for (int axis = 0; axis < 3; axis++) {
		const double cells = Math::ceil(double(get_bounds().size[axis]) / probe_spacing);
		ERR_FAIL_COND_V_MSG(cells >= INT32_MAX, Vector3i(), "LRT probe grid exceeds integer limits.");
		result[axis] = int(cells) + 1;
	}
	return result;
}

PackedStringArray LRTVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!is_visible_in_tree()) {
		return warnings;
	}
	if (is_inside_tree()) {
		const Basis basis = get_global_transform().basis;
		if (!basis.is_finite() || !basis.is_conformal() || basis.determinant() <= 0) {
			warnings.push_back(RTR("LRT volumes require a rotation and a positive uniform scale, without shear."));
		}
	}
	if (Object::cast_to<LRTGI>(this) && find_geometry_owner(this)) {
		warnings.push_back(RTR("LRTGI nodes cannot be nested. Reparent this calculation group outside its ancestor LRTGI."));
	}
	if (Object::cast_to<LRTDetailVolume3D>(this)) {
		LRTVolume3D *group = get_group();
		if (!group) {
			warnings.push_back(RTR("A Detail requires an LRTGI or LRTVolume3D ancestor and cannot be nested in another Detail."));
			return warnings;
		}
		if (is_inside_tree()) {
			const Transform3D relative = group->get_global_transform().affine_inverse() * get_global_transform();
			if (probe_spacing * relative.basis.get_scale().x >= group->get_probe_spacing()) {
				warnings.push_back(RTR("Detail probe spacing must be finer than the parent in world units."));
			}
			if (group->get_bounds().has_volume() && !group->get_bounds().encloses(relative.xform(get_bounds()))) {
				warnings.push_back(RTR("Detail bounds must be fully contained in the parent calculation grid."));
			}
		}
	}
	if (bake_data.is_null() || !bake_data->has_data()) {
		warnings.push_back(RTR("This volume has no baked static geometry."));
	} else if (bake_data->get_bounds() != get_bounds() || !Math::is_equal_approx(bake_data->get_probe_spacing(), probe_spacing)) {
		warnings.push_back(RTR("The volume layout differs from its baked geometry. Bake the volume again."));
	}
	if (warnings.is_empty() && is_inside_tree()) {
		const String runtime_error = get_geometry_status().get("error", String());
		if (!runtime_error.is_empty()) {
			warnings.push_back(runtime_error);
		}
	}
	return warnings;
}

void LRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		LRTRuntime::get_singleton()->add_volume(this);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		// Editor scene tabs detach their nodes without destroying them.
		LRTRuntime::get_singleton()->remove_volume(this, Engine::get_singleton()->is_editor_hint());
	} else if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		update_gizmos();
		update_configuration_warnings();
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		update_configuration_warnings();
	}
}

void LRTVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bake", "from_node"), &LRTVolume3D::bake, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LRTVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LRTVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("get_bounds"), &LRTVolume3D::get_bounds);
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "spacing"), &LRTVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LRTVolume3D::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &LRTVolume3D::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &LRTVolume3D::get_priority);
	ClassDB::bind_method(D_METHOD("set_blend_width", "width"), &LRTVolume3D::set_blend_width);
	ClassDB::bind_method(D_METHOD("get_blend_width"), &LRTVolume3D::get_blend_width);
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &LRTVolume3D::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &LRTVolume3D::get_intensity);
	ClassDB::bind_method(D_METHOD("set_bake_data", "data"), &LRTVolume3D::set_bake_data);
	ClassDB::bind_method(D_METHOD("get_bake_data"), &LRTVolume3D::get_bake_data);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &LRTVolume3D::get_grid_size);
	ClassDB::bind_method(D_METHOD("get_geometry_status"), &LRTVolume3D::get_geometry_status);
	ClassDB::bind_method(D_METHOD("get_geometry_trunk", "coordinate"), &LRTVolume3D::get_geometry_trunk);
	ClassDB::bind_method(D_METHOD("get_geometry_gpu_trunk", "coordinate"), &LRTVolume3D::get_geometry_gpu_trunk);
	ClassDB::bind_method(D_METHOD("inspect_probe", "coordinate"), &LRTVolume3D::inspect_probe);

	ADD_SIGNAL(MethodInfo("geometry_updated", PropertyInfo(Variant::INT, "revision"), PropertyInfo(Variant::ARRAY, "trunks", PROPERTY_HINT_ARRAY_TYPE, "Vector3i"), PropertyInfo(Variant::BOOL, "reset")));

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-128,127,1,or_less,or_greater"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_width", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater,suffix:m"), "set_blend_width", "get_blend_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bake_data", PROPERTY_HINT_RESOURCE_TYPE, "LocalLRTVolumeData"), "set_bake_data", "get_bake_data");
}

LRTVolume3D *LRTVolume3D::find_geometry_owner(const Node *p_node) {
	for (Node *node = p_node->get_parent(); node; node = node->get_parent()) {
		if (LRTGI *gi = Object::cast_to<LRTGI>(node)) {
			return gi;
		}
	}
	return nullptr;
}

LRTVolume3D *LRTVolume3D::get_group() const {
	if (!Object::cast_to<LRTDetailVolume3D>(this)) {
		return const_cast<LRTVolume3D *>(this);
	}
	for (Node *node = get_parent(); node; node = node->get_parent()) {
		if (LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(node)) {
			return Object::cast_to<LRTDetailVolume3D>(volume) ? nullptr : volume;
		}
	}
	return nullptr;
}

bool LRTVolume3D::is_active() const {
	LRTVolume3D *group = get_group();
	return is_visible_in_tree() && group && group->is_visible_in_tree();
}

int LRTVolume3D::get_priority() const {
	LRTVolume3D *group = get_group();
	return group && group != this ? group->get_priority() : priority;
}

float LRTVolume3D::get_intensity() const {
	LRTVolume3D *group = get_group();
	return group && group != this ? group->get_intensity() : intensity;
}

bool LRTVolume3D::collects_static(GeometryInstance3D *p_instance) const {
	if (p_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_STATIC) {
		return false;
	}
	LRTVolume3D *group = get_group();
	LRTVolume3D *owner = find_geometry_owner(p_instance);
	return group && (Object::cast_to<LRTGI>(group) ? owner == group : owner == nullptr);
}

bool LRTVolume3D::collects_sdf(GeometryInstance3D *p_instance) const {
	if (p_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_DISABLED) {
		return false;
	}
	LRTVolume3D *owner = find_geometry_owner(p_instance);
	if (!owner) {
		return p_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_DYNAMIC;
	}
	if (owner == get_group()) {
		return p_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_DYNAMIC;
	}
	return LRTGeometryBinding::is_external_contributor(p_instance);
}

void LRTVolume3D::_validate_property(PropertyInfo &p_property) const {
	if ((Object::cast_to<LRTGI>(this) && p_property.name == "size") ||
			(Object::cast_to<LRTDetailVolume3D>(this) && (p_property.name == "priority" || p_property.name == "intensity"))) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

AABB LRTGI::get_bounds() const {
	const Ref<LocalLRTVolumeData> resource = get_bake_data();
	return resource.is_valid() && resource->has_data() ? resource->get_bounds() : AABB();
}

void LRTGI::set_extend_margin(float p_margin) {
	ERR_FAIL_COND(!Math::is_finite(p_margin) || p_margin < 0);
	extend_margin = p_margin;
}

void LRTGI::set_external_static(bool p_enabled) {
	external_static = p_enabled;
}

void LRTGI::set_external_dynamic(bool p_enabled) {
	external_dynamic = p_enabled;
}

void LRTGI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_extend_margin", "margin"), &LRTGI::set_extend_margin);
	ClassDB::bind_method(D_METHOD("get_extend_margin"), &LRTGI::get_extend_margin);
	ClassDB::bind_method(D_METHOD("set_external_static", "enabled"), &LRTGI::set_external_static);
	ClassDB::bind_method(D_METHOD("is_external_static"), &LRTGI::is_external_static);
	ClassDB::bind_method(D_METHOD("set_external_dynamic", "enabled"), &LRTGI::set_external_dynamic);
	ClassDB::bind_method(D_METHOD("is_external_dynamic"), &LRTGI::is_external_dynamic);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extend_margin", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_extend_margin", "get_extend_margin");
	ADD_GROUP("External Contribution", "external_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "external_static"), "set_external_static", "is_external_static");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "external_dynamic"), "set_external_dynamic", "is_external_dynamic");
}

LRTVolume3D::LRTVolume3D() {
	set_notify_transform(true);
}

LRTVolume3D::~LRTVolume3D() {
	LRTBakeTask::retire_cache(bake_cache);
	if (LRTRuntime::get_singleton()) {
		LRTRuntime::get_singleton()->remove_volume(this);
	}
}
