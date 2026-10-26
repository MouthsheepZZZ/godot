/**************************************************************************/
/*  lrt_geometry_binding_3d.h                                             */
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

#include "scene/3d/node_3d.h"

class MeshInstance3D;

// A child binding uses its MeshInstance3D parent's object space, not its own transform.
class LRTGeometryBinding3D : public Node3D {
	GDCLASS(LRTGeometryBinding3D, Node3D);

	int sdf_resolution = 32;
	Ref<LRTInstanceGeometryData> bake_data;
	uint64_t geometry_revision = 0;

	void _bake_data_changed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static LRTGeometryBaker::BakeStepFunc bake_step_function;

	Error bake(const String &p_path);
	void configure_debug(int p_axis = -1, int p_slice = 0);
	void set_sdf_resolution(int p_resolution);
	int get_sdf_resolution() const { return sdf_resolution; }
	void set_bake_data(const Ref<LRTInstanceGeometryData> &p_data);
	Ref<LRTInstanceGeometryData> get_bake_data() const { return bake_data; }
	uint64_t get_geometry_revision() const { return geometry_revision; }
	MeshInstance3D *get_geometry_instance() const;

	PackedStringArray get_configuration_warnings() const override;

	LRTGeometryBinding3D();
};
