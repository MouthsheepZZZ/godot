/**************************************************************************/
/*  lrt_geometry_binding.cpp                                              */
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

#include "lrt_geometry_binding.h"

#include "core/config/project_settings.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/material.h"

const StringName &LRTGeometryBinding::get_meta_name() {
	static const StringName name("_lrt_binding");
	return name;
}

bool LRTGeometryBinding::supports(Object *p_object) {
	return Object::cast_to<MeshInstance3D>(p_object) || Object::cast_to<MultiMeshInstance3D>(p_object);
}

Ref<LRTGeometryBinding> LRTGeometryBinding::get_binding(GeometryInstance3D *p_instance) {
	return p_instance->has_meta(get_meta_name()) ? Ref<LRTGeometryBinding>(p_instance->get_meta(get_meta_name())) : Ref<LRTGeometryBinding>();
}

Ref<Mesh> LRTGeometryBinding::get_mesh(GeometryInstance3D *p_instance) {
	if (MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_instance)) {
		return mesh->get_mesh();
	}
	MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance);
	if (multi && multi->get_multimesh().is_valid()) {
		return multi->get_multimesh()->get_mesh();
	}
	return Ref<Mesh>();
}

Ref<Material> LRTGeometryBinding::get_material(GeometryInstance3D *p_instance, int p_surface) {
	if (MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_instance)) {
		return instance->get_active_material(p_surface);
	}
	const Ref<Material> material = p_instance->get_material_override();
	return material.is_valid() ? material : get_mesh(p_instance)->surface_get_material(p_surface);
}

int LRTGeometryBinding::get_instance_count(GeometryInstance3D *p_instance, bool p_visible_only) {
	if (Object::cast_to<MeshInstance3D>(p_instance)) {
		return 1;
	}
	MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance);
	if (!multi || multi->get_multimesh().is_null()) {
		return 0;
	}
	const Ref<MultiMesh> data = multi->get_multimesh();
	return p_visible_only && data->get_visible_instance_count() >= 0 ? data->get_visible_instance_count() : data->get_instance_count();
}

Transform3D LRTGeometryBinding::get_instance_transform(GeometryInstance3D *p_instance, int p_index) {
	MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance);
	if (multi) {
		return multi->get_multimesh()->get_instance_transform(p_index);
	}
	return Transform3D();
}

Error LRTGeometryBinding::bake(GeometryInstance3D *p_instance, const String &p_path, LRTGeometryBaker::BakeStepFunc p_step) {
	ERR_FAIL_COND_V(!supports(p_instance), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(!p_path.ends_with(".res"), ERR_INVALID_PARAMETER, "Save LRT Color SDF data to a binary .res resource.");
	Ref<LRTInstanceGeometryData> baked_data;
	Error error = LRTGeometryBaker::bake_geometry(p_instance, sdf_resolution, baked_data, p_step);
	if (error != OK) {
		return error;
	}
	error = ResourceSaver::save(baked_data, p_path);
	ERR_FAIL_COND_V_MSG(error != OK, error, "Could not save the LRT Color SDF. The previous bake is unchanged.");
	baked_data->set_path(ProjectSettings::get_singleton()->localize_path(p_path), true);
	set_bake_data(baked_data);
	return OK;
}

void LRTGeometryBinding::set_surface_normal_bias(float p_bias) {
	ERR_FAIL_COND(!Math::is_finite(p_bias) || (p_bias < 0.0f && p_bias != -1.0f));
	if (surface_normal_bias == p_bias) {
		return;
	}
	surface_normal_bias = p_bias;
	emit_changed();
}

void LRTGeometryBinding::set_sdf_resolution(int p_resolution) {
	ERR_FAIL_COND(p_resolution < 6);
	if (sdf_resolution == p_resolution) {
		return;
	}
	sdf_resolution = p_resolution;
	emit_changed();
}

void LRTGeometryBinding::set_bake_data(const Ref<LRTInstanceGeometryData> &p_data) {
	if (bake_data == p_data) {
		return;
	}
	if (bake_data.is_valid()) {
		bake_data->disconnect_changed(callable_mp(this, &LRTGeometryBinding::_bake_data_changed));
	}
	bake_data = p_data;
	if (bake_data.is_valid()) {
		bake_data->connect_changed(callable_mp(this, &LRTGeometryBinding::_bake_data_changed));
	}
	_bake_data_changed();
}

void LRTGeometryBinding::_bake_data_changed() {
	geometry_revision++;
	emit_changed();
}

PackedStringArray LRTGeometryBinding::get_configuration_warnings(GeometryInstance3D *p_instance) const {
	PackedStringArray warnings;
	if (get_mesh(p_instance).is_null()) {
		warnings.push_back(RTR("The instance has no mesh to bake."));
	}
	if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance)) {
		if (multi->get_multimesh().is_valid() && multi->get_multimesh()->get_transform_format() != MultiMesh::TRANSFORM_3D) {
			warnings.push_back(RTR("LRT requires a 3D MultiMesh."));
			return warnings;
		}
	}
	if (p_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DYNAMIC) {
		warnings.push_back(RTR("Color SDF is used only in Dynamic GI mode; Static geometry is baked into the LRT volume."));
		return warnings;
	}
	for (int index = 0; index < get_instance_count(p_instance); index++) {
		const Transform3D transform = p_instance->get_global_transform() * get_instance_transform(p_instance, index);
		if (!transform.is_finite() || !transform.basis.is_conformal() || transform.basis.determinant() <= 0) {
			warnings.push_back(RTR("LRT instances require a rotation and a positive uniform scale, without shear."));
			break;
		}
	}
	if (bake_data.is_null() || !bake_data->has_data()) {
		warnings.push_back(RTR("This instance has no baked Color SDF."));
	} else if (Object::cast_to<MultiMeshInstance3D>(p_instance) && bake_data->get_instance_count() != get_instance_count(p_instance, false)) {
		warnings.push_back(RTR("The MultiMesh instance count differs from its bake. Bake the Color SDF again."));
	}
	return warnings;
}

void LRTGeometryBinding::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bake", "instance", "path"), &LRTGeometryBinding::_bake);
	ClassDB::bind_method(D_METHOD("set_surface_normal_bias", "bias"), &LRTGeometryBinding::set_surface_normal_bias);
	ClassDB::bind_method(D_METHOD("get_surface_normal_bias"), &LRTGeometryBinding::get_surface_normal_bias);
	ClassDB::bind_method(D_METHOD("set_sdf_resolution", "resolution"), &LRTGeometryBinding::set_sdf_resolution);
	ClassDB::bind_method(D_METHOD("get_sdf_resolution"), &LRTGeometryBinding::get_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_bake_data", "data"), &LRTGeometryBinding::set_bake_data);
	ClassDB::bind_method(D_METHOD("get_bake_data"), &LRTGeometryBinding::get_bake_data);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_normal_bias", PROPERTY_HINT_RANGE, "-1,1,0.001,or_greater,suffix:m"), "set_surface_normal_bias", "get_surface_normal_bias");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sdf_resolution", PROPERTY_HINT_RANGE, "6,256,1,or_greater"), "set_sdf_resolution", "get_sdf_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bake_data", PROPERTY_HINT_RESOURCE_TYPE, "LRTInstanceGeometryData"), "set_bake_data", "get_bake_data");
}

LRTGeometryBinding::LRTGeometryBinding() {
	set_local_to_scene(true);
}
