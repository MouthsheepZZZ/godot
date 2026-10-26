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

#include "lrt_runtime.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

LRTGeometryBaker::BakeStepFunc LocalLRTVolume3D::bake_step_function = nullptr;

Error LocalLRTVolume3D::bake(const String &p_path, Node *p_from_node) {
	ERR_FAIL_COND_V_MSG(!p_path.ends_with(".res"), ERR_INVALID_PARAMETER, "Save LRT volume data to a binary .res resource.");
	ERR_FAIL_COND_V(!is_inside_tree(), ERR_UNCONFIGURED);
	Ref<LocalLRTVolumeData> baked_data;
	Error error = LRTGeometryBaker::bake_volume(this, p_from_node ? p_from_node : get_parent(), baked_data, bake_step_function);
	if (error != OK) {
		return error;
	}
	error = ResourceSaver::save(baked_data, p_path);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Could not save the LRT volume. The previous bake is unchanged.");
	baked_data->set_path(ProjectSettings::get_singleton()->localize_path(p_path), true);
	set_bake_data(baked_data);
	notify_property_list_changed();
	return OK;
}

void LocalLRTVolume3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	update_gizmos();
	update_configuration_warnings();
}

void LocalLRTVolume3D::set_size(const Vector3 &p_size) {
	ERR_FAIL_COND(!p_size.is_finite());
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0 || p_size.z <= 0);
	size = p_size;
	update_gizmos();
	update_configuration_warnings();
}

void LocalLRTVolume3D::set_probe_spacing(float p_spacing) {
	ERR_FAIL_COND(!Math::is_finite(p_spacing) || p_spacing <= 0);
	probe_spacing = p_spacing;
	update_gizmos();
	update_configuration_warnings();
}

void LocalLRTVolume3D::set_priority(int p_priority) {
	priority = p_priority;
}

void LocalLRTVolume3D::set_blend_width_cells(float p_width) {
	ERR_FAIL_COND(!Math::is_finite(p_width) || p_width < 0);
	blend_width_cells = p_width;
	update_gizmos();
}

void LocalLRTVolume3D::set_intensity(float p_intensity) {
	ERR_FAIL_COND(!Math::is_finite(p_intensity) || p_intensity < 0);
	intensity = p_intensity;
}

void LocalLRTVolume3D::set_bake_data(const Ref<LocalLRTVolumeData> &p_data) {
	if (bake_data == p_data) {
		return;
	}
	if (bake_data.is_valid()) {
		bake_data->disconnect_changed(callable_mp(this, &LocalLRTVolume3D::_bake_data_changed));
	}
	bake_data = p_data;
	if (bake_data.is_valid()) {
		bake_data->connect_changed(callable_mp(this, &LocalLRTVolume3D::_bake_data_changed));
	}
	_bake_data_changed();
}

void LocalLRTVolume3D::_bake_data_changed() {
	geometry_revision++;
	update_gizmos();
	update_configuration_warnings();
}

Dictionary LocalLRTVolume3D::inspect_probe(const Vector3i &p_coordinate) const {
	return LRTRuntime::get_singleton()->inspect_probe(const_cast<LocalLRTVolume3D *>(this), p_coordinate);
}

Dictionary LocalLRTVolume3D::get_geometry_status() const {
	return LRTRuntime::get_singleton()->get_status(const_cast<LocalLRTVolume3D *>(this));
}

Dictionary LocalLRTVolume3D::get_geometry_trunk(const Vector3i &p_coordinate) const {
	return LRTRuntime::get_singleton()->get_trunk(const_cast<LocalLRTVolume3D *>(this), p_coordinate);
}

Dictionary LocalLRTVolume3D::get_geometry_gpu_trunk(const Vector3i &p_coordinate) const {
	return LRTRuntime::get_singleton()->get_gpu_trunk(const_cast<LocalLRTVolume3D *>(this), p_coordinate);
}

Vector3i LocalLRTVolume3D::get_grid_size() const {
	Vector3i result;
	for (int axis = 0; axis < 3; axis++) {
		const double cells = Math::ceil(double(size[axis]) / probe_spacing);
		ERR_FAIL_COND_V_MSG(cells >= INT32_MAX, Vector3i(), "LRT probe grid exceeds integer limits.");
		result[axis] = int(cells) + 1;
	}
	return result;
}

PackedStringArray LocalLRTVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!enabled) {
		return warnings;
	}
	if (!GLOBAL_GET("rendering/lrt/enabled").operator bool()) {
		warnings.push_back(RTR("LRT is disabled in Project Settings (rendering/lrt/enabled)."));
	}
	if (is_inside_tree()) {
		const Basis basis = get_global_transform().basis;
		if (!basis.is_finite() || !basis.is_conformal() || basis.determinant() <= 0) {
			warnings.push_back(RTR("LRT volumes require a rotation and a positive uniform scale, without shear."));
		}
	}
	if (bake_data.is_null() || !bake_data->has_data()) {
		warnings.push_back(RTR("This volume has no baked static geometry."));
	} else if (bake_data->get_bounds() != get_bounds() || !Math::is_equal_approx(bake_data->get_probe_spacing(), probe_spacing)) {
		warnings.push_back(RTR("The volume layout differs from its baked geometry. Bake the volume again."));
	}
	return warnings;
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		LRTRuntime::get_singleton()->add_volume(this);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		// Editor scene tabs detach their nodes without destroying them.
		LRTRuntime::get_singleton()->remove_volume(this, Engine::get_singleton()->is_editor_hint());
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		update_configuration_warnings();
	}
}

void LocalLRTVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bake", "path", "from_node"), &LocalLRTVolume3D::bake, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LocalLRTVolume3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LocalLRTVolume3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LocalLRTVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LocalLRTVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("get_bounds"), &LocalLRTVolume3D::get_bounds);
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "spacing"), &LocalLRTVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LocalLRTVolume3D::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &LocalLRTVolume3D::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &LocalLRTVolume3D::get_priority);
	ClassDB::bind_method(D_METHOD("set_blend_width_cells", "width"), &LocalLRTVolume3D::set_blend_width_cells);
	ClassDB::bind_method(D_METHOD("get_blend_width_cells"), &LocalLRTVolume3D::get_blend_width_cells);
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &LocalLRTVolume3D::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &LocalLRTVolume3D::get_intensity);
	ClassDB::bind_method(D_METHOD("set_bake_data", "data"), &LocalLRTVolume3D::set_bake_data);
	ClassDB::bind_method(D_METHOD("get_bake_data"), &LocalLRTVolume3D::get_bake_data);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &LocalLRTVolume3D::get_grid_size);
	ClassDB::bind_method(D_METHOD("get_geometry_status"), &LocalLRTVolume3D::get_geometry_status);
	ClassDB::bind_method(D_METHOD("get_geometry_trunk", "coordinate"), &LocalLRTVolume3D::get_geometry_trunk);
	ClassDB::bind_method(D_METHOD("get_geometry_gpu_trunk", "coordinate"), &LocalLRTVolume3D::get_geometry_gpu_trunk);
	ClassDB::bind_method(D_METHOD("inspect_probe", "coordinate"), &LocalLRTVolume3D::inspect_probe);

	ADD_SIGNAL(MethodInfo("geometry_updated", PropertyInfo(Variant::INT, "revision"), PropertyInfo(Variant::ARRAY, "trunks", PROPERTY_HINT_ARRAY_TYPE, "Vector3i"), PropertyInfo(Variant::BOOL, "reset")));

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-128,127,1,or_less,or_greater"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_width_cells", PROPERTY_HINT_RANGE, "0,16,0.1,or_greater"), "set_blend_width_cells", "get_blend_width_cells");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bake_data", PROPERTY_HINT_RESOURCE_TYPE, "LocalLRTVolumeData"), "set_bake_data", "get_bake_data");
}

LocalLRTVolume3D::LocalLRTVolume3D() {
	set_notify_transform(true);
}

LocalLRTVolume3D::~LocalLRTVolume3D() {
	if (LRTRuntime::get_singleton()) {
		LRTRuntime::get_singleton()->remove_volume(this);
	}
}
