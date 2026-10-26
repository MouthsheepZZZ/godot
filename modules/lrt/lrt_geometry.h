/**************************************************************************/
/*  lrt_geometry.h                                                        */
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

#include "core/math/aabb.h"
#include "core/math/color.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

class Node3D;

namespace LRTGeometry {

constexpr int TRUNK_SIZE = 8;
constexpr float SH_C0 = 0.28209479177387814f;
constexpr float SH_C1 = 0.4886025119029199f;

struct Direction {
	Vector3i offset;
	float sh[4];
	float diffuse[4];
	Vector3 edge_offsets[4];
};

const Direction &direction(int p_index);
void edge_segments(const AABB &p_bounds, float p_spacing, const Vector3i &p_probe, int p_direction, Vector3 *r_from, Vector3 *r_to);

// Construct and use on the main thread; workers receive only the resulting transforms.
class VolumeFrame {
	Transform3D world_to_volume;
	HashMap<Node3D *, Transform3D> volume_to_ancestor;

public:
	explicit VolumeFrame(Node3D *p_volume);
	Transform3D relative_transform(Node3D *p_instance) const;
};

struct Probe {
	float transfer[3][16] = {};
	float visibility[4] = { 1.0f / SH_C0, 0, 0, 0 };
	float emission[12] = {};
	bool inside = false;

	void clear_inside();
	void add_sample(int p_direction, const Color &p_albedo, const Color &p_emission);
	void compose(const Probe &p_other);
	void decode(const uint8_t *p_ltm, const uint8_t *p_vis, const float *p_emission);
	void encode(uint8_t *r_ltm, uint8_t *r_vis, float *r_emission) const;
};

struct StaticCache {
	AABB bounds;
	Vector3i grid;
	float spacing = 0;
	Vector<uint8_t> ltm;
	Vector<uint8_t> local_vis;
	Vector<uint8_t> edge_masks;
	Vector<float> emission;

	void load(const Dictionary &p_data);
};

struct ColorSDF {
	AABB bounds;
	Vector3i grid;
	Vector<uint8_t> distance;
	Vector<uint8_t> color;
	Vector<float> emission;

	void load(const Dictionary &p_data);
	bool contains(const Vector3 &p_point, Color *r_color = nullptr, Color *r_emission = nullptr) const;
	bool intersects_segment(const Vector3 &p_from, const Vector3 &p_to) const;

private:
	double cell_value(const Vector3i &p_cell, int p_corner) const;
	bool cell_intersects(const Vector3i &p_cell, const double *p_from, const double *p_to) const;
};

struct InstanceKey {
	ObjectID node;
	int index = 0;

	bool operator==(const InstanceKey &p_other) const { return node == p_other.node && index == p_other.index; }
	static uint32_t hash(const InstanceKey &p_key) { return hash_murmur3_one_32(p_key.index, hash_one_uint64(uint64_t(p_key.node))); }
};

struct Instance {
	ObjectID id;
	int index = 0;
	InstanceKey get_key() const { return { id, index }; }
	uint64_t revision = 0;
	ColorSDF sdf;
	Transform3D to_instance;
	AABB bounds; // SDF domain in volume space.
};

struct Trunk {
	Vector3i coordinate;
	Vector3i origin;
	Vector3i size;
	Vector<uint8_t> ltm;
	Vector<uint8_t> local_vis;
	Vector<uint8_t> edge_masks;
	Vector<float> emission;
	Vector<ObjectID> instances;
};

Trunk rebuild_trunk(const StaticCache &p_static, const Vector<Instance> &p_instances, const Vector3i &p_coordinate);

} // namespace LRTGeometry
