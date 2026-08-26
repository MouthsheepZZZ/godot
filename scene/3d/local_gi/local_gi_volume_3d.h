/**************************************************************************/
/*  local_gi_volume_3d.h                                                  */
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

#include "scene/3d/visual_instance_3d.h"

class LocalGIVolume3D : public VisualInstance3D {
	GDCLASS(LocalGIVolume3D, VisualInstance3D);

public:
	enum DebugMode {
		DEBUG_DISABLED,
		DEBUG_LOCAL_GEOMETRY,
		DEBUG_STATIC_BVH_HIT,
		DEBUG_DYNAMIC_BVH_HIT,
		DEBUG_RAY_HIT_MISS,
		DEBUG_HIT_NORMAL,
		DEBUG_HIT_DISTANCE,
		DEBUG_PROBE_POSITIONS,
		DEBUG_SELECTED_PROBE_RAYS,
		DEBUG_RAW_PROBE_RADIANCE,
		DEBUG_PROBE_IRRADIANCE,
		DEBUG_VISIBILITY,
		DEBUG_PROBE_WEIGHTS,
		DEBUG_GLOBAL_INDIRECT_CACHE,
		DEBUG_FINAL_LOCAL_GI,
		DEBUG_GLOBAL_GI,
		DEBUG_FINAL_SELECTED_GI,
		DEBUG_MAX,
	};

private:
	Vector3 size = Vector3(4, 4, 4);
	float probe_spacing = 0.5;
	int rays_per_probe = 64;
	float update_fraction = 1.0;
	float temporal_hysteresis = 0.9;
	bool multi_bounce_enabled = false;
	DebugMode debug_mode = DEBUG_DISABLED;

protected:
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const;

	void set_rays_per_probe(int p_rays);
	int get_rays_per_probe() const;

	void set_update_fraction(float p_fraction);
	float get_update_fraction() const;

	void set_temporal_hysteresis(float p_hysteresis);
	float get_temporal_hysteresis() const;

	void set_multi_bounce_enabled(bool p_enabled);
	bool is_multi_bounce_enabled() const;

	void set_debug_mode(DebugMode p_mode);
	DebugMode get_debug_mode() const;

	// Phase 0 skeleton only. Phase 1 implements static triangle bake + CPU BVH.
	void bake(Node *p_from_node = nullptr);

	virtual AABB get_aabb() const override;

	LocalGIVolume3D();
};

VARIANT_ENUM_CAST(LocalGIVolume3D::DebugMode)
