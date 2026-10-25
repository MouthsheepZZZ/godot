/**************************************************************************/
/*  local_lrt_volume_3d.h                                                 */
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

#include "scene/3d/visual_instance_3d.h"

class LocalLRTVolume3D : public VisualInstance3D {
	GDCLASS(LocalLRTVolume3D, VisualInstance3D);

	bool enabled = true;
	Vector3 size = Vector3(10, 10, 10);
	float probe_spacing = 0.5f;
	int priority = 0;
	float blend_width_cells = 2.0f;
	float intensity = 1.0f;
	Ref<LocalLRTVolumeData> bake_data;

	void _bake_data_changed();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	static LRTGeometryBaker::BakeStepFunc bake_step_function;

	Error bake(const String &p_path, Node *p_from_node = nullptr);
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const { return size; }
	AABB get_bounds() const { return AABB(-size / 2, size); }
	AABB get_aabb() const override { return get_bounds(); }
	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const { return probe_spacing; }
	void set_priority(int p_priority);
	int get_priority() const { return priority; }
	void set_blend_width_cells(float p_width);
	float get_blend_width_cells() const { return blend_width_cells; }
	void set_intensity(float p_intensity);
	float get_intensity() const { return intensity; }
	void set_bake_data(const Ref<LocalLRTVolumeData> &p_data);
	Ref<LocalLRTVolumeData> get_bake_data() const { return bake_data; }

	Vector3i get_grid_size() const;
	PackedStringArray get_configuration_warnings() const override;

	LocalLRTVolume3D();
};
