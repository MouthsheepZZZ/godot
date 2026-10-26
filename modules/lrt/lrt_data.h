/**************************************************************************/
/*  lrt_data.h                                                            */
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

#include "core/io/resource.h"

// Replace complete bake payloads on the main thread; never publish partial channels.
class LocalLRTVolumeData : public Resource {
	GDCLASS(LocalLRTVolumeData, Resource);

	Dictionary data;

protected:
	static void _bind_methods();

public:
	void set_data(const Dictionary &p_data);
	Dictionary get_data() const;
	AABB get_bounds() const;
	float get_probe_spacing() const;
	Vector3i get_grid_size() const;
	bool has_data() const { return !data.is_empty(); }
};

class LRTInstanceGeometryData : public Resource {
	GDCLASS(LRTInstanceGeometryData, Resource);

	Dictionary data;
	void _instance_changed() { emit_changed(); }
	void _connect_instances(bool p_connect);

protected:
	static void _bind_methods();

public:
	void set_data(const Dictionary &p_data);
	Dictionary get_data() const;
	AABB get_bounds() const;
	Vector3i get_grid_size() const;
	float get_distance_scale() const;
	int get_instance_count() const;
	Ref<LRTInstanceGeometryData> get_instance_data(int p_index) const;
	bool is_receiver_only() const { return data.get("receiver_only", false); }
	bool has_data() const { return !data.is_empty(); }
};
