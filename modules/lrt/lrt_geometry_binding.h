/**************************************************************************/
/*  lrt_geometry_binding.h                                                */
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

#include "lrt_data.h"
#include "lrt_geometry_baker.h"

class GeometryInstance3D;
class Mesh;
class Material;

// Stored in GeometryInstance3D metadata; the scene classes do not depend on LRT.
class LRTGeometryBinding : public Resource {
	GDCLASS(LRTGeometryBinding, Resource);

	int sdf_resolution = 32;
	float surface_normal_bias = -1.0f;
	Ref<LRTInstanceGeometryData> bake_data;
	uint64_t geometry_revision = 1;

	void _bake_data_changed();
	Error _bake(GeometryInstance3D *p_instance, const String &p_path) { return bake(p_instance, p_path); }

protected:
	static void _bind_methods();

public:
	static const StringName &get_meta_name();
	static bool supports(Object *p_object);
	static Ref<LRTGeometryBinding> get_binding(GeometryInstance3D *p_instance);
	static Ref<Mesh> get_mesh(GeometryInstance3D *p_instance);
	static Ref<Material> get_material(GeometryInstance3D *p_instance, int p_surface);
	static int get_instance_count(GeometryInstance3D *p_instance, bool p_visible_only = true);
	static Transform3D get_instance_transform(GeometryInstance3D *p_instance, int p_index);

	Error bake(GeometryInstance3D *p_instance, const String &p_path, LRTGeometryBaker::BakeStepFunc p_step = nullptr);
	PackedStringArray get_configuration_warnings(GeometryInstance3D *p_instance) const;
	void set_surface_normal_bias(float p_bias);
	float get_surface_normal_bias() const { return surface_normal_bias; }
	void set_sdf_resolution(int p_resolution);
	int get_sdf_resolution() const { return sdf_resolution; }
	void set_bake_data(const Ref<LRTInstanceGeometryData> &p_data);
	Ref<LRTInstanceGeometryData> get_bake_data() const { return bake_data; }
	uint64_t get_geometry_revision() const { return geometry_revision; }

	LRTGeometryBinding();
};
