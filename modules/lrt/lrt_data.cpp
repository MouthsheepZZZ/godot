/**************************************************************************/
/*  lrt_data.cpp                                                          */
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

#include "lrt_data.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

bool has_field(const Dictionary &p_data, const char *p_name, Variant::Type p_type) {
	return p_data.has(p_name) && p_data[p_name].get_type() == p_type;
}

bool valid_bounds(const AABB &p_bounds) {
	return p_bounds.position.is_finite() && p_bounds.size.is_finite() &&
			p_bounds.size.x > 0 && p_bounds.size.y > 0 && p_bounds.size.z > 0;
}

// PackedArray uses signed 32-bit lengths. Check before multiplying by channel stride.
int64_t get_sample_count(const Vector3i &p_size, int p_max_stride) {
	if (p_size.x < 2 || p_size.y < 2 || p_size.z < 2) {
		return 0;
	}
	int64_t count = p_size.x;
	if (count > INT32_MAX / p_max_stride / p_size.y) {
		return 0;
	}
	count *= p_size.y;
	if (count > INT32_MAX / p_max_stride / p_size.z) {
		return 0;
	}
	return count * p_size.z;
}

} // namespace

void LocalLRTVolumeData::set_data(const Dictionary &p_data) {
	if (p_data.is_empty()) {
		data.clear();
		emit_changed();
		return;
	}

	ERR_FAIL_COND_MSG(!has_field(p_data, "bounds", Variant::AABB) ||
					!has_field(p_data, "probe_spacing", Variant::FLOAT) ||
					!has_field(p_data, "grid_size", Variant::VECTOR3I) ||
					!has_field(p_data, "ltm", Variant::PACKED_BYTE_ARRAY) ||
					!has_field(p_data, "local_vis", Variant::PACKED_BYTE_ARRAY) ||
					!has_field(p_data, "edge_masks", Variant::PACKED_BYTE_ARRAY) ||
					!has_field(p_data, "emission", Variant::PACKED_FLOAT32_ARRAY),
			"Invalid LRT volume bake fields.");

	const AABB bounds = p_data["bounds"];
	const float spacing = p_data["probe_spacing"];
	const Vector3i grid_size = p_data["grid_size"];
	const int64_t count = get_sample_count(grid_size, 36);
	ERR_FAIL_COND_MSG(!valid_bounds(bounds) || !Math::is_finite(spacing) || spacing <= 0 || count == 0,
			"Invalid LRT volume bake layout.");
	for (int axis = 0; axis < 3; axis++) {
		ERR_FAIL_COND_MSG(Math::ceil(double(bounds.size[axis]) / spacing) != grid_size[axis] - 1,
				"LRT bounds must match the probe grid and spacing.");
	}

	const Vector<uint8_t> ltm = p_data["ltm"];
	const Vector<uint8_t> local_vis = p_data["local_vis"];
	const Vector<uint8_t> edge_masks = p_data["edge_masks"];
	const Vector<float> emission = p_data["emission"];
	ERR_FAIL_COND_MSG(ltm.size() != count * 36 || local_vis.size() != count * 8 ||
					edge_masks.size() != count * 26 ||
					(!emission.is_empty() && emission.size() != count * 12),
			"LRT volume bake channel sizes do not match the grid.");

	// LTM: 16 FP16 matrix coefficients, RGB8, flags (bit 0 = Inside).
	// LocalVis: four FP16 coefficients. Edge: one four-bit mask per outgoing neighbor,
	// including boundary edges; the baker writes identical masks at shared endpoints.
	data = p_data.duplicate(true);
	emit_changed();
}

Dictionary LocalLRTVolumeData::get_data() const {
	return data.duplicate(true);
}

AABB LocalLRTVolumeData::get_bounds() const {
	return data.get("bounds", AABB());
}

float LocalLRTVolumeData::get_probe_spacing() const {
	return data.get("probe_spacing", 0.0f);
}

Vector3i LocalLRTVolumeData::get_grid_size() const {
	return data.get("grid_size", Vector3i());
}

void LocalLRTVolumeData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &LocalLRTVolumeData::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &LocalLRTVolumeData::get_data);
	ClassDB::bind_method(D_METHOD("get_bounds"), &LocalLRTVolumeData::get_bounds);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LocalLRTVolumeData::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &LocalLRTVolumeData::get_grid_size);
	ClassDB::bind_method(D_METHOD("has_data"), &LocalLRTVolumeData::has_data);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
}

void LRTInstanceGeometryData::_connect_instances(bool p_connect) {
	const Array instances = data.get("instances", Array());
	const Callable callback = callable_mp(this, &LRTInstanceGeometryData::_instance_changed);
	for (int index = 0; index < instances.size(); index++) {
		const Ref<LRTInstanceGeometryData> instance = instances[index];
		if (p_connect && !instance->is_connected("changed", callback)) {
			instance->connect_changed(callback);
		} else if (!p_connect && instance->is_connected("changed", callback)) {
			instance->disconnect_changed(callback);
		}
	}
}

void LRTInstanceGeometryData::set_data(const Dictionary &p_data) {
	if (p_data.is_empty()) {
		_connect_instances(false);
		data.clear();
		emit_changed();
		return;
	}

	if (p_data.has("instances")) {
		ERR_FAIL_COND(p_data["instances"].get_type() != Variant::ARRAY);
		const Array instances = p_data["instances"];
		for (int index = 0; index < instances.size(); index++) {
			const Ref<LRTInstanceGeometryData> instance = instances[index];
			ERR_FAIL_COND(instance.is_null() || !instance->has_data() || instance->get_instance_count() != -1);
		}
		_connect_instances(false);
		data = p_data.duplicate(true);
		_connect_instances(true);
		emit_changed();
		return;
	}

	ERR_FAIL_COND_MSG(!has_field(p_data, "bounds", Variant::AABB) ||
					!has_field(p_data, "grid_size", Variant::VECTOR3I) ||
					!has_field(p_data, "distance_scale", Variant::FLOAT) ||
					!has_field(p_data, "distance", Variant::PACKED_BYTE_ARRAY) ||
					!has_field(p_data, "color", Variant::PACKED_BYTE_ARRAY) ||
					!has_field(p_data, "emission", Variant::PACKED_FLOAT32_ARRAY),
			"Invalid LRT instance bake fields.");

	const AABB bounds = p_data["bounds"];
	const Vector3i grid_size = p_data["grid_size"];
	const float distance_scale = p_data["distance_scale"];
	const int64_t count = get_sample_count(grid_size, 4);
	ERR_FAIL_COND_MSG(!valid_bounds(bounds) || !Math::is_finite(distance_scale) || distance_scale <= 0 || count == 0,
			"Invalid LRT Color SDF layout.");

	const Vector<uint8_t> distance = p_data["distance"];
	const Vector<uint8_t> color = p_data["color"];
	const Vector<float> emission = p_data["emission"];
	ERR_FAIL_COND_MSG(distance.size() != count * 2 || color.size() != count * 4 ||
					(!emission.is_empty() && emission.size() != count * 3),
			"LRT Color SDF channel sizes do not match the grid.");

	// Signed distance is little-endian SNORM16. Color is linear RGBA8.
	_connect_instances(false);
	data = p_data.duplicate(true);
	emit_changed();
}

Dictionary LRTInstanceGeometryData::get_data() const {
	return data.duplicate(true);
}

AABB LRTInstanceGeometryData::get_bounds() const {
	return data.get("bounds", AABB());
}

Vector3i LRTInstanceGeometryData::get_grid_size() const {
	return data.get("grid_size", Vector3i());
}

float LRTInstanceGeometryData::get_distance_scale() const {
	return data.get("distance_scale", 0.0f);
}

int LRTInstanceGeometryData::get_instance_count() const {
	return data.has("instances") ? Array(data["instances"]).size() : -1;
}

Ref<LRTInstanceGeometryData> LRTInstanceGeometryData::get_instance_data(int p_index) const {
	const Array instances = data.get("instances", Array());
	ERR_FAIL_INDEX_V(p_index, instances.size(), Ref<LRTInstanceGeometryData>());
	return instances[p_index];
}

void LRTInstanceGeometryData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_instance_count"), &LRTInstanceGeometryData::get_instance_count);
	ClassDB::bind_method(D_METHOD("get_instance_data", "index"), &LRTInstanceGeometryData::get_instance_data);
	ClassDB::bind_method(D_METHOD("set_data", "data"), &LRTInstanceGeometryData::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &LRTInstanceGeometryData::get_data);
	ClassDB::bind_method(D_METHOD("get_bounds"), &LRTInstanceGeometryData::get_bounds);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &LRTInstanceGeometryData::get_grid_size);
	ClassDB::bind_method(D_METHOD("get_distance_scale"), &LRTInstanceGeometryData::get_distance_scale);
	ClassDB::bind_method(D_METHOD("has_data"), &LRTInstanceGeometryData::has_data);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
}
