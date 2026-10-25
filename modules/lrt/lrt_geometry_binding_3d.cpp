/**************************************************************************/
/*  lrt_geometry_binding_3d.cpp                                           */
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

#include "lrt_geometry_binding_3d.h"

#include "lrt_runtime.h"

#include "core/config/project_settings.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"

LRTGeometryBaker::BakeStepFunc LRTGeometryBinding3D::bake_step_function = nullptr;

Error LRTGeometryBinding3D::bake(const String &p_path) {
	ERR_FAIL_COND_V_MSG(!p_path.ends_with(".res"), ERR_INVALID_PARAMETER, "Save LRT Color SDF data to a binary .res resource.");
	Ref<LRTInstanceGeometryData> baked_data;
	Error error = LRTGeometryBaker::bake_instance(get_geometry_instance(), sdf_resolution, baked_data, bake_step_function);
	if (error != OK) {
		return error;
	}
	error = ResourceSaver::save(baked_data, p_path);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Could not save the LRT Color SDF. The previous bake is unchanged.");
	baked_data->set_path(ProjectSettings::get_singleton()->localize_path(p_path), true);
	set_bake_data(baked_data);
	notify_property_list_changed();
	return OK;
}

void LRTGeometryBinding3D::set_sdf_resolution(int p_resolution) {
	ERR_FAIL_COND(p_resolution < 6);
	sdf_resolution = p_resolution;
	update_configuration_warnings();
}

void LRTGeometryBinding3D::set_bake_data(const Ref<LRTInstanceGeometryData> &p_data) {
	if (bake_data == p_data) {
		return;
	}
	if (bake_data.is_valid()) {
		bake_data->disconnect_changed(callable_mp(this, &LRTGeometryBinding3D::_bake_data_changed));
	}
	bake_data = p_data;
	if (bake_data.is_valid()) {
		bake_data->connect_changed(callable_mp(this, &LRTGeometryBinding3D::_bake_data_changed));
	}
	_bake_data_changed();
}

void LRTGeometryBinding3D::_bake_data_changed() {
	geometry_revision++;
	update_configuration_warnings();
}

MeshInstance3D *LRTGeometryBinding3D::get_geometry_instance() const {
	return Object::cast_to<MeshInstance3D>(get_parent());
}

PackedStringArray LRTGeometryBinding3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	MeshInstance3D *instance = get_geometry_instance();
	if (!instance) {
		warnings.push_back(RTR("Place this binding directly under the MeshInstance3D whose Color SDF it owns."));
		return warnings;
	}
	if (instance->get_mesh().is_null()) {
		warnings.push_back(RTR("The bound instance has no mesh to bake."));
	}
	if (instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DYNAMIC) {
		warnings.push_back(RTR("Set the parent instance's Global Illumination Mode to Dynamic to use its Color SDF in LRT."));
	}
	if (instance->is_inside_tree()) {
		const Basis basis = instance->get_global_transform().basis;
		if (!basis.is_finite() || !basis.is_conformal() || basis.determinant() <= 0) {
			warnings.push_back(RTR("LRT instances require a rotation and a positive uniform scale, without shear."));
		}
	}
	if (bake_data.is_null() || !bake_data->has_data()) {
		warnings.push_back(RTR("This instance has no baked Color SDF."));
	}
	return warnings;
}

void LRTGeometryBinding3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		LRTRuntime::get_singleton()->add_binding(this);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		LRTRuntime::get_singleton()->remove_binding(this);
	}
	if (p_what == NOTIFICATION_TRANSFORM_CHANGED || p_what == NOTIFICATION_PARENTED) {
		update_configuration_warnings();
	}
}

void LRTGeometryBinding3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bake", "path"), &LRTGeometryBinding3D::bake);
	ClassDB::bind_method(D_METHOD("set_sdf_resolution", "resolution"), &LRTGeometryBinding3D::set_sdf_resolution);
	ClassDB::bind_method(D_METHOD("get_sdf_resolution"), &LRTGeometryBinding3D::get_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_bake_data", "data"), &LRTGeometryBinding3D::set_bake_data);
	ClassDB::bind_method(D_METHOD("get_bake_data"), &LRTGeometryBinding3D::get_bake_data);
	ClassDB::bind_method(D_METHOD("get_geometry_instance"), &LRTGeometryBinding3D::get_geometry_instance);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "sdf_resolution", PROPERTY_HINT_RANGE, "6,256,1,or_greater"), "set_sdf_resolution", "get_sdf_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bake_data", PROPERTY_HINT_RESOURCE_TYPE, "LRTInstanceGeometryData"), "set_bake_data", "get_bake_data");
}

LRTGeometryBinding3D::LRTGeometryBinding3D() {
	set_notify_transform(true);
}
