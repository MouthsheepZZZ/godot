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

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

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
	update_gizmos();
	update_configuration_warnings();
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
	warnings.push_back(RTR("LRT rendering is not implemented yet. This node currently stores volume configuration only."));
	return warnings;
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		update_configuration_warnings();
	}
}

void LocalLRTVolume3D::_bind_methods() {
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
