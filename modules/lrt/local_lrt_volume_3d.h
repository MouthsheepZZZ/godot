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

#include "lrt_bake_task.h"
#include "lrt_data.h"
#include "lrt_geometry_baker.h"

#include "scene/3d/visual_instance_3d.h"

class LRTVolume3D : public VisualInstance3D {
	GDCLASS(LRTVolume3D, VisualInstance3D);
	friend class LRTBakeTask;
	Ref<RefCounted> bake_cache;

	Vector3 size = Vector3(10, 10, 10);
	float probe_spacing = 0.5f;
	int priority = 0;
	float blend_width = 1.0f;
	float intensity = 1.0f;
	Ref<LocalLRTVolumeData> bake_data;
	uint64_t geometry_revision = 0;

	void _bake_data_changed();

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;

public:
	Ref<LRTBakeTask> bake(Node *p_from_node = nullptr);
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const { return get_bounds().size; }
	virtual AABB get_bounds() const { return AABB(-size / 2, size); }
	LRTVolume3D *get_group() const;
	bool is_active() const;
	static LRTVolume3D *find_geometry_owner(const Node *p_node);
	bool collects_static(GeometryInstance3D *p_instance) const;
	bool collects_sdf(GeometryInstance3D *p_instance) const;
	AABB get_aabb() const override { return get_bounds(); }
	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const { return probe_spacing; }
	void set_priority(int p_priority);
	int get_priority() const;
	void set_blend_width(float p_width);
	float get_blend_width() const { return blend_width; }
	void set_intensity(float p_intensity);
	float get_intensity() const;
	void set_bake_data(const Ref<LocalLRTVolumeData> &p_data);
	Ref<LocalLRTVolumeData> get_bake_data() const { return bake_data; }
	uint64_t get_geometry_revision() const { return geometry_revision; }

	Dictionary inspect_probe(const Vector3i &p_coordinate) const;
	Dictionary get_geometry_status() const;
	Dictionary get_geometry_trunk(const Vector3i &p_coordinate) const;
	Dictionary get_geometry_gpu_trunk(const Vector3i &p_coordinate) const;
	Vector3i get_grid_size() const;
	PackedStringArray get_configuration_warnings() const override;

	LRTVolume3D();
	~LRTVolume3D();
};

class LRTGI : public LRTVolume3D {
	GDCLASS(LRTGI, LRTVolume3D);
	float extend_margin = 1.0f;
	bool external_static = true;
	bool external_dynamic = false;

protected:
	static void _bind_methods();

public:
	AABB get_bounds() const override;
	void set_extend_margin(float p_margin);
	float get_extend_margin() const { return extend_margin; }
	void set_external_static(bool p_enabled);
	bool is_external_static() const { return external_static; }
	void set_external_dynamic(bool p_enabled);
	bool is_external_dynamic() const { return external_dynamic; }
};

class LRTDetailVolume3D : public LRTVolume3D {
	GDCLASS(LRTDetailVolume3D, LRTVolume3D);

protected:
	static void _bind_methods() {}

public:
	LRTDetailVolume3D() { set_probe_spacing(0.25f); }
};
