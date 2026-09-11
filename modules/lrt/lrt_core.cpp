/**************************************************************************/
/*  lrt_core.cpp                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be        */
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

#include "lrt_core.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <utility>

namespace lrt {

void parallel_for(int p_count, int p_threads, const std::function<void(int)> &p_body) {
	if (p_count <= 0) {
		return;
	}
	const int threads = std::max(1, std::min(p_threads, p_count));
	if (threads == 1) {
		for (int i = 0; i < p_count; i++) {
			p_body(i);
		}
		return;
	}
	std::atomic<int> next(0);
	std::vector<std::thread> workers;
	workers.reserve(size_t(threads - 1));
	for (int t = 0; t < threads - 1; t++) {
		workers.emplace_back([&next, p_count, &p_body]() {
			for (;;) {
				const int i = next.fetch_add(1, std::memory_order_relaxed);
				if (i >= p_count) {
					return;
				}
				p_body(i);
			}
		});
	}
	// The calling thread works too, which keeps small counts from paying for a thread that
	// would otherwise sit idle.
	for (;;) {
		const int i = next.fetch_add(1, std::memory_order_relaxed);
		if (i >= p_count) {
			break;
		}
		p_body(i);
	}
	for (std::thread &worker : workers) {
		worker.join();
	}
}

namespace {

// JavaScript Math.round: ties go towards +Infinity (floor(x + 0.5)).
inline double js_round(double p_value) { return std::floor(p_value + 0.5); }

inline double max3(double p_a, double p_b, double p_c) { return std::max(p_a, std::max(p_b, p_c)); }

struct DirectionTable {
	Direction items[DIRECTION_COUNT];
	DirectionTable() {
		int slot = 0;
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					if (x == 0 && y == 0 && z == 0) {
						continue;
					}
					const double len = hypot3(x, y, z);
					items[slot].offset[0] = x;
					items[slot].offset[1] = y;
					items[slot].offset[2] = z;
					items[slot].direction = Vec3(x / len, y / len, z / len);
					slot++;
				}
			}
		}
	}
};

const DirectionTable &direction_table() {
	static const DirectionTable table;
	return table;
}

// src/sdf-local.js sampleNearest.
bool sample_nearest(const Vec3 &p_point, const std::vector<const SdfPrimitive *> &p_candidate_ptrs, ColorSdfSample &r_sample) {
	bool found = false;
	for (const SdfPrimitive *primitive : p_candidate_ptrs) {
		const ColorSdfSample value = primitive->sample(p_point);
		if (!found || value.distance < r_sample.distance) {
			r_sample = value;
			found = true;
		}
	}
	return found;
}

// src/core.js accumulateTransfer.
void accumulate_transfer(std::vector<float> &p_matrices, const Grid &p_grid, int p_index, const Vec3 &p_direction, const Vec3 &p_normal, const Vec3 &p_color) {
	double outgoing[4];
	double incident[4];
	positive_basis(p_direction, outgoing);
	cosine_basis(p_normal, incident);
	for (int channel = 0; channel < 3; channel++) {
		const double factor = WEIGHT * p_color[channel] / PI;
		for (int row = 0; row < 4; row++) {
			const int base = ((channel * 4 + row) * p_grid.count + p_index) * 4;
			for (int column = 0; column < 4; column++) {
				// Float32 accumulation, matching the prototype's Float32Array exactly.
				p_matrices[base + column] = float(double(p_matrices[base + column]) + factor * outgoing[row] * incident[column]);
			}
		}
	}
}

} // namespace

const Direction *directions() {
	return direction_table().items;
}

void basis(const Vec3 &p_direction, double r_out[4]) {
	r_out[0] = C0;
	r_out[1] = C1 * p_direction.x;
	r_out[2] = C1 * p_direction.y;
	r_out[3] = C1 * p_direction.z;
}

void positive_basis(const Vec3 &p_direction, double r_out[4]) {
	r_out[0] = C0;
	r_out[1] = C1 * p_direction.x / 3.0;
	r_out[2] = C1 * p_direction.y / 3.0;
	r_out[3] = C1 * p_direction.z / 3.0;
}

void cosine_basis(const Vec3 &p_normal, double r_out[4]) {
	r_out[0] = PI * C0;
	r_out[1] = 2.0 * PI / 3.0 * C1 * p_normal.x;
	r_out[2] = 2.0 * PI / 3.0 * C1 * p_normal.y;
	r_out[3] = 2.0 * PI / 3.0 * C1 * p_normal.z;
}

void sh_triple_product(const double p_a[4], const double p_b[4], double r_out[4]) {
	double dc = 0.0;
	for (int i = 0; i < 4; i++) {
		dc += p_a[i] * p_b[i];
	}
	r_out[0] = C0 * dc;
	for (int i = 1; i < 4; i++) {
		r_out[i] = C0 * (p_a[0] * p_b[i] + p_b[0] * p_a[i]);
	}
}

Grid make_grid(double p_spacing) {
	Vec3 lo(-3, -0.5, -3);
	Vec3 hi(3, 3.5, 3);
	Grid grid;
	grid.min = lo;
	grid.spacing = p_spacing;
	for (int axis = 0; axis < 3; axis++) {
		grid.size[axis] = int(std::ceil((hi[axis] - lo[axis]) / p_spacing - 1e-9));
	}
	grid.width = grid.size[0] * grid.size[2];
	grid.height = grid.size[1];
	grid.count = grid.size[0] * grid.size[1] * grid.size[2];
	return grid;
}

Grid make_grid(double p_spacing, const Vec3 &p_bounds_min, const Vec3 &p_bounds_max) {
	// Prototype: start from the default region, then keep two air cells around geometry.
	Vec3 lo(-3, -0.5, -3);
	Vec3 hi(3, 3.5, 3);
	for (int axis = 0; axis < 3; axis++) {
		lo[axis] = std::min(lo[axis], std::floor(p_bounds_min[axis] / p_spacing) * p_spacing - 2.0 * p_spacing);
		hi[axis] = std::max(hi[axis], std::ceil(p_bounds_max[axis] / p_spacing) * p_spacing + 2.0 * p_spacing);
	}
	Grid grid;
	grid.min = lo;
	grid.spacing = p_spacing;
	for (int axis = 0; axis < 3; axis++) {
		grid.size[axis] = int(std::ceil((hi[axis] - lo[axis]) / p_spacing - 1e-9));
	}
	grid.width = grid.size[0] * grid.size[2];
	grid.height = grid.size[1];
	grid.count = grid.size[0] * grid.size[1] * grid.size[2];
	return grid;
}

static Grid make_grid_from_region(double p_spacing, const Vec3 &p_lo, const Vec3 &p_hi) {
	Grid grid;
	grid.min = p_lo;
	grid.spacing = p_spacing;
	for (int axis = 0; axis < 3; axis++) {
		grid.size[axis] = int(std::ceil((p_hi[axis] - p_lo[axis]) / p_spacing - 1e-9));
	}
	grid.width = grid.size[0] * grid.size[2];
	grid.height = grid.size[1];
	grid.count = grid.size[0] * grid.size[1] * grid.size[2];
	return grid;
}

Grid make_grid_sized(double p_spacing, const Vec3 &p_min, const Vec3 &p_size) {
	return make_grid_from_region(p_spacing, p_min, p_min + p_size);
}

Grid make_grid_sized(double p_spacing, const Vec3 &p_min, const Vec3 &p_size, const Vec3 &p_bounds_min, const Vec3 &p_bounds_max) {
	// Same rule as make_grid(spacing, bounds): two air cells around the geometry.
	Vec3 lo = p_min;
	Vec3 hi = p_min + p_size;
	for (int axis = 0; axis < 3; axis++) {
		lo[axis] = std::min(lo[axis], std::floor(p_bounds_min[axis] / p_spacing) * p_spacing - 2.0 * p_spacing);
		hi[axis] = std::max(hi[axis], std::ceil(p_bounds_max[axis] / p_spacing) * p_spacing + 2.0 * p_spacing);
	}
	return make_grid_from_region(p_spacing, lo, hi);
}

bool BoxQuery::contains(const Vec3 &p_point) const {
	for (const Box &box : boxes) {
		bool within = true;
		for (int axis = 0; axis < 3; axis++) {
			if (p_point[axis] < box.min[axis] - GEOMETRY_EPSILON || p_point[axis] > box.max[axis] + GEOMETRY_EPSILON) {
				within = false;
				break;
			}
		}
		if (within) {
			return true;
		}
	}
	return false;
}

bool BoxQuery::trace(const Vec3 &p_origin, const Vec3 &p_direction, double p_limit, Hit &r_hit) const {
	bool found = false;
	double nearest = 0.0;
	Vec3 nearest_position;
	Vec3 nearest_normal;
	Vec3 nearest_color;
	for (const Box &box : boxes) {
		double near_t = -1e20;
		double far_t = 1e20;
		int axis = -1;
		int sign = 0;
		bool skipped = false;
		for (int a = 0; a < 3; a++) {
			if (std::fabs(p_direction[a]) < 1e-12) {
				if (p_origin[a] < box.min[a] - GEOMETRY_EPSILON || p_origin[a] > box.max[a] + GEOMETRY_EPSILON) {
					skipped = true;
					break;
				}
				continue;
			}
			double t0 = (box.min[a] - p_origin[a]) / p_direction[a];
			double t1 = (box.max[a] - p_origin[a]) / p_direction[a];
			if (t0 > t1) {
				std::swap(t0, t1);
			}
			if (t0 > near_t) {
				near_t = t0;
				axis = a;
				sign = p_direction[a] > 0.0 ? -1 : 1;
			}
			far_t = std::min(far_t, t1);
		}
		if (skipped) {
			continue;
		}
		if (far_t + GEOMETRY_EPSILON < near_t || near_t < GEOMETRY_EPSILON || near_t > p_limit + GEOMETRY_EPSILON) {
			continue;
		}
		if (found && near_t >= nearest) {
			continue;
		}
		Vec3 normal;
		normal[axis] = double(sign);
		found = true;
		nearest = near_t;
		nearest_position = Vec3(p_origin.x + p_direction.x * near_t, p_origin.y + p_direction.y * near_t, p_origin.z + p_direction.z * near_t);
		nearest_normal = normal;
		nearest_color = box.color;
	}
	if (!found) {
		return false;
	}
	r_hit.valid = true;
	r_hit.distance = nearest;
	r_hit.position = nearest_position;
	r_hit.normal = nearest_normal;
	r_hit.color = nearest_color;
	return true;
}

ColorSdfField bake_box_color_sdf(const Vec3 &p_extent, const Vec3 &p_color, int p_resolution, const std::atomic<bool> *p_cancel) {
	const Vec3 half = p_extent * 0.5;
	auto signed_distance = [&half](const Vec3 &p_point) {
		double q[3] = { std::fabs(p_point.x) - half.x, std::fabs(p_point.y) - half.y, std::fabs(p_point.z) - half.z };
		const double positive = hypot3(std::max(q[0], 0.0), std::max(q[1], 0.0), std::max(q[2], 0.0));
		const double negative = std::min(std::max(q[0], std::max(q[1], q[2])), 0.0);
		return positive + negative;
	};

	ColorSdfField field;
	field.cell = max3(p_extent.x, p_extent.y, p_extent.z) / double(p_resolution);
	field.min = Vec3(-half.x - 2.0 * field.cell, -half.y - 2.0 * field.cell, -half.z - 2.0 * field.cell);
	for (int axis = 0; axis < 3; axis++) {
		field.size[axis] = int(std::ceil(p_extent[axis] / field.cell)) + 5;
	}
	const int count = field.size[0] * field.size[1] * field.size[2];
	field.distance_scale = hypot3(field.size[0] * field.cell, field.size[1] * field.cell, field.size[2] * field.cell) / 32767.0;
	field.distance.resize(count);
	for (int z = 0; z < field.size[2]; z++) {
		if (p_cancel && p_cancel->load()) {
			return ColorSdfField();
		}
		for (int y = 0; y < field.size[1]; y++) {
			for (int x = 0; x < field.size[0]; x++) {
				const Vec3 p(field.min.x + x * field.cell, field.min.y + y * field.cell, field.min.z + z * field.cell);
				const double signed_value = signed_distance(p);
				// Prototype stores |distance| with the sign taken from the containment test, which is the signed distance itself.
				field.distance[x + field.size[0] * (y + field.size[1] * z)] = int16_t(js_round(signed_value / field.distance_scale));
			}
		}
	}
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = int(std::ceil(double(field.size[axis] - 1) / 4.0)) + 1;
	}
	const int color_count = field.color_size[0] * field.color_size[1] * field.color_size[2];
	field.color.resize(color_count * 3);
	for (int z = 0; z < field.color_size[2]; z++) {
		for (int y = 0; y < field.color_size[1]; y++) {
			for (int x = 0; x < field.color_size[0]; x++) {
				const int index = x + field.color_size[0] * (y + field.color_size[1] * z);
				for (int channel = 0; channel < 3; channel++) {
					const double clamped = std::max(0.0, std::min(1.0, p_color[channel]));
					field.color[index * 3 + channel] = uint8_t(js_round(clamped * 255.0));
				}
			}
		}
	}
	return field;
}

ColorSdfSample sample_color_sdf(const ColorSdfField &p_field, const Vec3 &p_point) {
	double coordinate[3];
	double g[3];
	int base[3];
	double f[3];
	for (int axis = 0; axis < 3; axis++) {
		coordinate[axis] = (p_point[axis] - p_field.min[axis]) / p_field.cell;
		g[axis] = std::max(0.0, std::min(double(p_field.size[axis] - 1), coordinate[axis]));
		base[axis] = std::min(p_field.size[axis] - 2, int(std::floor(g[axis])));
		f[axis] = g[axis] - base[axis];
	}
	Vec3 normal;
	Vec3 color;
	double distance = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				const int corner[3] = { x, y, z };
				double w[3];
				for (int axis = 0; axis < 3; axis++) {
					w[axis] = corner[axis] ? f[axis] : 1.0 - f[axis];
				}
				const int index = base[0] + x + p_field.size[0] * (base[1] + y + p_field.size[1] * (base[2] + z));
				const double weight = w[0] * w[1] * w[2];
				const double d = double(p_field.distance[index]) * p_field.distance_scale;
				distance += weight * d;
				for (int axis = 0; axis < 3; axis++) {
					normal[axis] += d * (corner[axis] ? 1.0 : -1.0) * w[(axis + 1) % 3] * w[(axis + 2) % 3] / p_field.cell;
				}
			}
		}
	}
	double cg[3];
	int cb[3];
	double cf[3];
	for (int axis = 0; axis < 3; axis++) {
		cg[axis] = g[axis] / double(p_field.size[axis] - 1) * double(p_field.color_size[axis] - 1);
		cb[axis] = std::min(p_field.color_size[axis] - 2, int(std::floor(cg[axis])));
		cf[axis] = cg[axis] - cb[axis];
	}
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				const int corner[3] = { x, y, z };
				const double w = (x ? cf[0] : 1.0 - cf[0]) * (y ? cf[1] : 1.0 - cf[1]) * (z ? cf[2] : 1.0 - cf[2]);
				const int index = cb[0] + x + p_field.color_size[0] * (cb[1] + y + p_field.color_size[1] * (cb[2] + z));
				for (int channel = 0; channel < 3; channel++) {
					color[channel] += w * double(p_field.color[index * 3 + channel]) / 255.0;
				}
			}
		}
	}
	Vec3 outside;
	for (int axis = 0; axis < 3; axis++) {
		outside[axis] = (coordinate[axis] - g[axis]) * p_field.cell;
	}
	const double outside_length = hypot3(outside.x, outside.y, outside.z);
	ColorSdfSample sample;
	sample.valid = true;
	if (outside_length > 0.0) {
		distance = std::max(0.0, distance) + outside_length;
		normal = outside / outside_length;
	} else {
		const double n = hypot3(normal.x, normal.y, normal.z);
		if (n > 0.0) {
			normal = normal / n;
		}
	}
	sample.distance = distance;
	sample.normal = normal;
	sample.color = color;
	return sample;
}

bool PrimitiveTransform::is_identity() const {
	return origin.x == 0.0 && origin.y == 0.0 && origin.z == 0.0 &&
			basis_x.x == 1.0 && basis_x.y == 0.0 && basis_x.z == 0.0 &&
			basis_y.x == 0.0 && basis_y.y == 1.0 && basis_y.z == 0.0 &&
			basis_z.x == 0.0 && basis_z.y == 0.0 && basis_z.z == 1.0 &&
			scale == 1.0;
}

// PrimitiveGI.sample (src/primitive-gi.js): the inverse transform maps the world point
// into the asset frame, sampleColorSDF reads the local field, and the sample goes back to
// world units through the rotation and the uniform scale.
ColorSdfSample SdfPrimitive::sample(const Vec3 &p_point) const {
	const Vec3 delta = p_point - origin;
	const Vec3 local(dot(delta, basis_x) / scale, dot(delta, basis_y) / scale, dot(delta, basis_z) / scale);
	ColorSdfSample value = sample_color_sdf(field, local);
	value.distance *= scale;
	value.normal = basis_x * value.normal.x + basis_y * value.normal.y + basis_z * value.normal.z;
	return value;
}

SdfPrimitive make_sdf_primitive(ColorSdfField p_field, const PrimitiveTransform &p_transform, uint64_t p_signature) {
	SdfPrimitive primitive;
	primitive.field = p_field;
	primitive.signature = p_signature;
	primitive.origin = p_transform.origin;
	primitive.basis_x = p_transform.basis_x;
	primitive.basis_y = p_transform.basis_y;
	primitive.basis_z = p_transform.basis_z;
	primitive.scale = p_transform.scale;
	// PrimitiveGI bounds: the local field box transformed and re-boxed (Box3.applyMatrix4).
	const Vec3 local_low = p_field.min;
	const Vec3 local_high = p_field.min + Vec3(double(p_field.size[0] - 1) * p_field.cell,
			double(p_field.size[1] - 1) * p_field.cell, double(p_field.size[2] - 1) * p_field.cell);
	for (int corner = 0; corner < 8; corner++) {
		const Vec3 local((corner & 1) ? local_high.x : local_low.x,
				(corner & 2) ? local_high.y : local_low.y,
				(corner & 4) ? local_high.z : local_low.z);
		const Vec3 world = p_transform.origin +
				p_transform.basis_x * (local.x * p_transform.scale) +
				p_transform.basis_y * (local.y * p_transform.scale) +
				p_transform.basis_z * (local.z * p_transform.scale);
		for (int axis = 0; axis < 3; axis++) {
			if (corner == 0 || world[axis] < primitive.bounds_min[axis]) {
				primitive.bounds_min[axis] = world[axis];
			}
			if (corner == 0 || world[axis] > primitive.bounds_max[axis]) {
				primitive.bounds_max[axis] = world[axis];
			}
		}
	}
	return primitive;
}

SdfPrimitive make_sdf_primitive(const Vec3 &p_position, ColorSdfField p_field, uint64_t p_signature) {
	PrimitiveTransform transform;
	transform.origin = p_position;
	return make_sdf_primitive(p_field, transform, p_signature);
}

namespace {

// FNV-1a over raw doubles: the signature only has to be stable within a run and change whenever
// a primitive's field or matrix changes.
void mix_bytes(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; i++) {
		r_hash ^= uint64_t(bytes[i]);
		r_hash *= 1099511628211ull;
	}
}

void mix_value(uint64_t &r_hash, double p_value) {
	mix_bytes(r_hash, &p_value, sizeof(double));
}

// Prototype gridKey (JSON of min/size/spacing): an incremental build is only valid on its own
// grid, so a spacing or box change falls back to the full bake.
uint64_t grid_signature(const Grid &p_grid) {
	uint64_t hash = 1469598103934665603ull;
	mix_value(hash, p_grid.min.x);
	mix_value(hash, p_grid.min.y);
	mix_value(hash, p_grid.min.z);
	mix_value(hash, double(p_grid.size[0]));
	mix_value(hash, double(p_grid.size[1]));
	mix_value(hash, double(p_grid.size[2]));
	mix_value(hash, p_grid.spacing);
	return hash;
}

} // namespace

uint64_t box_field_signature(const Vec3 &p_extent, const Vec3 &p_color, int p_resolution) {
	uint64_t hash = 1469598103934665603ull;
	mix_value(hash, p_extent.x);
	mix_value(hash, p_extent.y);
	mix_value(hash, p_extent.z);
	mix_value(hash, p_color.x);
	mix_value(hash, p_color.y);
	mix_value(hash, p_color.z);
	mix_value(hash, double(p_resolution));
	return hash;
}

uint64_t primitive_signature(uint64_t p_field_signature, const PrimitiveTransform &p_transform) {
	uint64_t hash = 1469598103934665603ull;
	mix_bytes(hash, &p_field_signature, sizeof(uint64_t));
	mix_value(hash, p_transform.origin.x);
	mix_value(hash, p_transform.origin.y);
	mix_value(hash, p_transform.origin.z);
	mix_value(hash, p_transform.basis_x.x);
	mix_value(hash, p_transform.basis_x.y);
	mix_value(hash, p_transform.basis_x.z);
	mix_value(hash, p_transform.basis_y.x);
	mix_value(hash, p_transform.basis_y.y);
	mix_value(hash, p_transform.basis_y.z);
	mix_value(hash, p_transform.basis_z.x);
	mix_value(hash, p_transform.basis_z.y);
	mix_value(hash, p_transform.basis_z.z);
	mix_value(hash, p_transform.scale);
	return hash;
}

LocalField build_local_data(const Grid &p_grid, const BoxQuery &p_query, const std::atomic<bool> *p_cancel, int p_threads) {
	LocalField field;
	field.material.assign(size_t(p_grid.count) * 4, 0.0f);
	field.matrices.assign(size_t(p_grid.count) * 48, 0.0f);
	field.links.assign(size_t(p_grid.count), 0u);
	// One row of probes per parallel unit; every counter is summed in y order afterwards, so the
	// field is identical to the serial sweep.
	const int rows = p_grid.size[1];
	std::vector<int> row_solid(size_t(rows), 0);
	std::vector<int> row_surface(size_t(rows), 0);
	std::vector<int> row_mismatches(size_t(rows), 0);
	std::atomic<bool> cancelled(false);
	parallel_for(rows, p_threads, [&](int y) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				if (!p_query.contains(probe_point(p_grid, x, y, z))) {
					continue;
				}
				const int index = index_of(p_grid, x, y, z);
				field.material[index * 4 + 0] = 0.5f;
				field.material[index * 4 + 1] = 0.5f;
				field.material[index * 4 + 2] = 0.5f;
				field.material[index * 4 + 3] = 1.0f;
				row_solid[size_t(y)]++;
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	const Direction *dirs = directions();
	parallel_for(rows, p_threads, [&](int y) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				if (field.material[index * 4 + 3] != 0.0f) {
					continue;
				}
				const Vec3 origin = probe_point(p_grid, x, y, z);
				bool surface = false;
				for (int j = 0; j < DIRECTION_COUNT; j++) {
					const Vec3 direction = dirs[j].direction;
					const double limit = hypot3(dirs[j].offset[0], dirs[j].offset[1], dirs[j].offset[2]) * p_grid.spacing;
					Hit hit;
					if (!p_query.trace(origin, direction, limit, hit)) {
						const int qx = x + dirs[j].offset[0];
						const int qy = y + dirs[j].offset[1];
						const int qz = z + dirs[j].offset[2];
						if (inside(p_grid, qx, qy, qz) && field.material[index_of(p_grid, qx, qy, qz) * 4 + 3] != 0.0f) {
							row_mismatches[size_t(y)]++;
						}
						field.links[index] |= 1u << uint32_t(j);
						continue;
					}
					surface = true;
					accumulate_transfer(field.matrices, p_grid, index, direction, hit.normal, hit.color);
				}
				if (surface) {
					row_surface[size_t(y)]++;
				}
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	for (int y = 0; y < rows; y++) {
		field.solid_count += row_solid[size_t(y)];
		field.surface_count += row_surface[size_t(y)];
		field.classification_mismatches += row_mismatches[size_t(y)];
	}
	return field;
}

// src/sdf-local.js buildSDFLocalData, including its incremental cache: a trunk of 8^3 probes is
// recomputed only when its candidate primitives changed, and every other probe is copied from the
// previous field. Without a usable previous field this is exactly the full bake it always was.
LocalField build_sdf_local_data(const Grid &p_grid, const std::vector<SdfPrimitive> &p_primitives,
		const std::atomic<bool> *p_cancel, int p_threads, const LocalCache *p_previous, LocalCache *r_cache) {
	LocalField field;
	field.material.assign(size_t(p_grid.count) * 4, 0.0f);
	field.matrices.assign(size_t(p_grid.count) * 48, 0.0f);
	field.links.assign(size_t(p_grid.count), 0u);

	const double spacing = p_grid.spacing;
	// src/sdf-local.js buildTrunks: trunk candidates include the full 26-neighbor support box.
	struct Trunk {
		std::vector<const SdfPrimitive *> candidates;
	};
	// The trunk lattice is flat and indexed, not a map: probing needs one lookup per probe, and
	// the incremental test below is a plain compare over the same index space.
	const int trunk_size[3] = {
		(p_grid.size[0] + TRUNK - 1) / TRUNK,
		(p_grid.size[1] + TRUNK - 1) / TRUNK,
		(p_grid.size[2] + TRUNK - 1) / TRUNK,
	};
	const int trunk_count = trunk_size[0] * trunk_size[1] * trunk_size[2];
	auto trunk_of = [&trunk_size](int p_x, int p_y, int p_z) {
		return (p_x / TRUNK) + trunk_size[0] * ((p_y / TRUNK) + trunk_size[1] * (p_z / TRUNK));
	};
	std::vector<Trunk> trunks;
	trunks.resize(size_t(trunk_count));
	std::vector<uint64_t> signatures(size_t(trunk_count), 0);
	parallel_for(trunk_count, p_threads, [&](int p_trunk) {
		const int tx = p_trunk % trunk_size[0];
		const int ty = (p_trunk / trunk_size[0]) % trunk_size[1];
		const int tz = p_trunk / (trunk_size[0] * trunk_size[1]);
		const int base[3] = { tx * TRUNK, ty * TRUNK, tz * TRUNK };
		Vec3 low;
		Vec3 high;
		for (int axis = 0; axis < 3; axis++) {
			low[axis] = p_grid.min[axis] + (base[axis] - 1) * spacing;
			high[axis] = p_grid.min[axis] + (base[axis] + 9) * spacing;
		}
		Trunk &trunk = trunks[size_t(p_trunk)];
		// PrimitiveGI.signature joined per trunk: order-sensitive, so a removed or moved
		// primitive always changes the digest of the trunks it touches.
		uint64_t signature = 1469598103934665603ull;
		for (const SdfPrimitive &primitive : p_primitives) {
			bool overlap = true;
			for (int axis = 0; axis < 3; axis++) {
				if (!(primitive.bounds_min[axis] <= high[axis] && primitive.bounds_max[axis] >= low[axis])) {
					overlap = false;
					break;
				}
			}
			if (!overlap) {
				continue;
			}
			trunk.candidates.push_back(&primitive);
			mix_bytes(signature, &primitive.signature, sizeof(uint64_t));
		}
		signatures[size_t(p_trunk)] = signature;
	});
	field.trunk_count = trunk_count;

	// Prototype dirty test: reuse the previous field only when it describes this very grid.
	const uint64_t grid_key = grid_signature(p_grid);
	const bool have_previous = p_previous != nullptr && p_previous->grid_key == grid_key &&
			p_previous->local != nullptr &&
			p_previous->local->material.size() == field.material.size() &&
			p_previous->trunk_signatures.size() == signatures.size() &&
			p_previous->samples.size() == size_t(p_grid.count) &&
			p_previous->sampled.size() == size_t(p_grid.count);
	std::vector<uint8_t> dirty(size_t(trunk_count), 1);
	for (size_t t = 0; t < signatures.size(); t++) {
		if (have_previous && p_previous->trunk_signatures[t] == signatures[t]) {
			dirty[t] = 0;
		} else {
			field.dirty_trunk_count++;
		}
	}
	const LocalField &previous_field = have_previous ? *p_previous->local : field;

	std::vector<ColorSdfSample> samples(size_t(p_grid.count));
	std::vector<uint8_t> sampled(size_t(p_grid.count), 0);
	// Two parallel passes over probe rows: the first samples the field, the second builds the
	// links, transfer matrices and the receiver list of each row. Rows own their slots, and the
	// receivers are concatenated in y order afterwards, so the field is byte-identical to the
	// prototype's serial sweep.
	struct SdfRow {
		std::vector<float> receivers;
		// (probe index, receiver count) in the row's own x/z order.
		std::vector<std::pair<int, int>> entries;
		int solid = 0;
		int surface = 0;
	};
	const int rows = p_grid.size[1];
	std::vector<SdfRow> row_data;
	row_data.resize(size_t(rows));
	std::atomic<bool> cancelled(false);
	parallel_for(rows, p_threads, [&](int y) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				const size_t trunk = size_t(trunk_of(x, y, z));
				if (!dirty[trunk]) {
					// Clean trunk: the previous sample is still the answer, no field query.
					samples[size_t(index)] = p_previous->samples[size_t(index)];
					sampled[size_t(index)] = p_previous->sampled[size_t(index)];
				} else {
					ColorSdfSample value;
					if (sample_nearest(probe_point(p_grid, x, y, z), trunks[trunk].candidates, value)) {
						samples[size_t(index)] = value;
						sampled[size_t(index)] = 1;
					}
				}
				if (sampled[size_t(index)] && samples[size_t(index)].distance < 0.0) {
					field.material[index * 4 + 0] = 0.5f;
					field.material[index * 4 + 1] = 0.5f;
					field.material[index * 4 + 2] = 0.5f;
					field.material[index * 4 + 3] = 1.0f;
					row_data[size_t(y)].solid++;
				}
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}

	const Direction *dirs = directions();
	parallel_for(rows, p_threads, [&](int y) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		SdfRow &row = row_data[size_t(y)];
		std::vector<float> &receivers = row.receivers;
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				if (field.material[index * 4 + 3] != 0.0f) {
					continue;
				}
				const size_t trunk_index = size_t(trunk_of(x, y, z));
				if (!dirty[trunk_index]) {
					// src/sdf-local.js copyCleanProbe: links, transfer matrices and the receiver
					// slice come straight from the previous field, repacked in scan order.
					const int count = int(previous_field.material[index * 4 + 1]);
					const size_t offset = size_t(previous_field.material[index * 4 + 0]);
					field.links[index] = previous_field.links[index];
					for (int block = 0; block < 12; block++) {
						const size_t base = (size_t(block) * size_t(p_grid.count) + size_t(index)) * 4;
						for (int column = 0; column < 4; column++) {
							field.matrices[base + column] = previous_field.matrices[base + column];
						}
					}
					const size_t from = offset * 4;
					const size_t to = (offset + size_t(count) * 3) * 4;
					if (to > from) {
						receivers.insert(receivers.end(), previous_field.receivers.begin() + from, previous_field.receivers.begin() + to);
					}
					if (count > 0) {
						row.surface++;
					}
					row.entries.emplace_back(index, count);
					continue;
				}
				const Vec3 origin = probe_point(p_grid, x, y, z);
				const size_t start_floats = receivers.size();
				const Trunk &trunk = trunks[trunk_index];
				for (int j = 0; j < DIRECTION_COUNT; j++) {
					const int qx = x + dirs[j].offset[0];
					const int qy = y + dirs[j].offset[1];
					const int qz = z + dirs[j].offset[2];
					ColorSdfSample value;
					bool have_value = false;
					if (inside(p_grid, qx, qy, qz)) {
						const int qindex = index_of(p_grid, qx, qy, qz);
						if (sampled[qindex]) {
							value = samples[qindex];
							have_value = true;
						}
					} else {
						have_value = sample_nearest(probe_point(p_grid, qx, qy, qz), trunk.candidates, value);
					}
					// Prototype band: h/2 surface band; PDF p.23 does not specify the threshold.
					if (!have_value || value.distance > spacing / 2.0) {
						field.links[index] |= 1u << uint32_t(j);
						continue;
					}
					const Vec3 direction = dirs[j].direction;
					const Vec3 normal = -direction;
					accumulate_transfer(field.matrices, p_grid, index, direction, normal, value.color);
					const Vec3 neighbor_position = probe_point(p_grid, qx, qy, qz);
					const Vec3 receiver = Vec3(neighbor_position.x - value.distance * value.normal.x,
							neighbor_position.y - value.distance * value.normal.y,
							neighbor_position.z - value.distance * value.normal.z);
					double facing = 1.0;
					if (dot(value.normal, origin - receiver) < 0.0) {
						facing = -1.0;
					}
					receivers.push_back(float(receiver.x));
					receivers.push_back(float(receiver.y));
					receivers.push_back(float(receiver.z));
					receivers.push_back(float(j));
					receivers.push_back(float(value.normal.x * facing));
					receivers.push_back(float(value.normal.y * facing));
					receivers.push_back(float(value.normal.z * facing));
					receivers.push_back(0.0f);
					receivers.push_back(float(value.color.x));
					receivers.push_back(float(value.color.y));
					receivers.push_back(float(value.color.z));
					receivers.push_back(0.0f);
				}
				const int count = int(((receivers.size() - start_floats) / 4) / 3);
				if (count > 0) {
					row.surface++;
				}
				row.entries.emplace_back(index, count);
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	// Merge the rows in y order: probe start offsets and the receiver list come out exactly as
	// the serial version produced them.
	size_t receiver_floats = 0;
	for (int y = 0; y < rows; y++) {
		SdfRow &row = row_data[size_t(y)];
		field.solid_count += row.solid;
		field.surface_count += row.surface;
		for (const std::pair<int, int> &entry : row.entries) {
			field.material[entry.first * 4 + 0] = float(receiver_floats / 4);
			field.material[entry.first * 4 + 1] = float(entry.second);
			receiver_floats += size_t(entry.second) * 12;
		}
		field.receivers.insert(field.receivers.end(), row.receivers.begin(), row.receivers.end());
	}
	if (r_cache != nullptr) {
		// The next build's previous state. `local` is filled in by the caller, which owns the
		// field these entries belong to.
		r_cache->grid_key = grid_key;
		r_cache->trunk_signatures = std::move(signatures);
		r_cache->samples = std::move(samples);
		r_cache->sampled = std::move(sampled);
		r_cache->local = nullptr;
	}
	return field;
}

void build_local_visibility(LocalField &r_field, int p_threads) {
	const Direction *dirs = directions();
	r_field.local_visibility.assign(size_t(r_field.links.size()) * 4, 0.0f);
	// Probe indices are independent here, so the sweep runs in blocks of probes.
	constexpr int VISIBILITY_BLOCK = 4096;
	const int blocks = int((r_field.links.size() + VISIBILITY_BLOCK - 1) / VISIBILITY_BLOCK);
	const int count = int(r_field.links.size());
	parallel_for(blocks, p_threads, [&](int p_block) {
		const int begin = p_block * VISIBILITY_BLOCK;
		const int end = std::min(count, begin + VISIBILITY_BLOCK);
		for (int i = begin; i < end; i++) {
		if (r_field.material[i * 4 + 3] != 0.0f) {
			continue;
		}
		double value[4] = { 0.0, 0.0, 0.0, 0.0 };
		for (int j = 0; j < DIRECTION_COUNT; j++) {
			if (!(r_field.links[i] & (1u << uint32_t(j)))) {
				continue;
			}
			double b[4];
			basis(dirs[j].direction, b);
			for (int k = 0; k < 4; k++) {
				value[k] += WEIGHT * b[k];
			}
		}
		r_field.local_visibility[i * 4 + 0] = float(value[0]);
		r_field.local_visibility[i * 4 + 1] = float(value[1]);
		r_field.local_visibility[i * 4 + 2] = float(value[2]);
		r_field.local_visibility[i * 4 + 3] = float(value[3]);
		}
	});
}

// ---------------------------------------------------------------------------
// Triangle meshes: prototype src/model-geometry.js::buildBVH,
// src/geometry-query.js::closest/contains and src/primitive-gi.js::bakeMeshSDF.
// ---------------------------------------------------------------------------

namespace {

constexpr double MESH_EPSILON = 1e-6;
constexpr double MESH_WELD = 1e-6;

double point_triangle_distance_squared(const Vec3 &p_point, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, Vec3 &r_closest) {
	// Ericson, Real-Time Collision Detection 5.1.5: the same construction as
	// three.js Triangle.closestPointToPoint.
	const Vec3 ab = p_b - p_a;
	const Vec3 ac = p_c - p_a;
	const double d1 = dot(ab, p_point - p_a);
	const double d2 = dot(ac, p_point - p_a);
	if (d1 <= 0.0 && d2 <= 0.0) {
		r_closest = p_a;
	} else {
		const Vec3 bp = p_point - p_b;
		const double d3 = dot(ab, bp);
		const double d4 = dot(ac, bp);
		if (d3 >= 0.0 && d4 <= d3) {
			r_closest = p_b;
		} else {
			const double vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
				r_closest = p_a + ab * (d1 / (d1 - d3));
			} else {
				const Vec3 cp = p_point - p_c;
				const double d5 = dot(ab, cp);
				const double d6 = dot(ac, cp);
				if (d6 >= 0.0 && d5 <= d6) {
					r_closest = p_c;
				} else {
					const double vb = d5 * d2 - d1 * d6;
					if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
						r_closest = p_a + ac * (d2 / (d2 - d6));
					} else {
						const double va = d3 * d6 - d5 * d4;
						if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
							r_closest = p_b + (p_c - p_b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
						} else {
							const double denominator = 1.0 / (va + vb + vc);
							r_closest = p_a + ab * (vb * denominator) + ac * (vc * denominator);
						}
					}
				}
			}
		}
	}
	const Vec3 delta = p_point - r_closest;
	return dot(delta, delta);
}

bool mesh_bounds_hit(const Vec3 &p_low, const Vec3 &p_high, const Vec3 &p_origin, const Vec3 &p_direction, double p_limit, double &r_near) {
	double near_t = 0.0;
	double far_t = p_limit;
	for (int axis = 0; axis < 3; axis++) {
		if (std::fabs(p_direction[axis]) < 1e-12) {
			if (p_origin[axis] < p_low[axis] - MESH_EPSILON || p_origin[axis] > p_high[axis] + MESH_EPSILON) {
				return false;
			}
			continue;
		}
		double t0 = (p_low[axis] - p_origin[axis]) / p_direction[axis];
		double t1 = (p_high[axis] - p_origin[axis]) / p_direction[axis];
		if (t0 > t1) {
			std::swap(t0, t1);
		}
		near_t = std::max(near_t, t0);
		far_t = std::min(far_t, t1);
		if (far_t < near_t) {
			return false;
		}
	}
	r_near = near_t;
	return true;
}

struct MeshRayHit {
	double distance = 0.0;
	Vec3 normal;
};

// geometry-query.js walk(): every opaque intersection inside the limit, in BVH order.
template <typename Visitor>
void mesh_walk(const TriangleMesh &p_mesh, const Vec3 &p_origin, const Vec3 &p_direction, double p_limit, const Visitor &p_visit) {
	const int node_count = int(p_mesh.node_min.size());
	int node = 0;
	while (node < node_count) {
		double near_t = 0.0;
		const bool inside_bounds = p_origin.x >= p_mesh.node_min[node].x - MESH_EPSILON && p_origin.x <= p_mesh.node_max[node].x + MESH_EPSILON &&
				p_origin.y >= p_mesh.node_min[node].y - MESH_EPSILON && p_origin.y <= p_mesh.node_max[node].y + MESH_EPSILON &&
				p_origin.z >= p_mesh.node_min[node].z - MESH_EPSILON && p_origin.z <= p_mesh.node_max[node].z + MESH_EPSILON;
		if (!mesh_bounds_hit(p_mesh.node_min[node], p_mesh.node_max[node], p_origin, p_direction, p_limit, near_t) ||
				(!inside_bounds && near_t > p_limit + MESH_EPSILON)) {
			node = p_mesh.node_escape[node];
			continue;
		}
		const int leaf = p_mesh.node_leaf[node];
		if (leaf < 0) {
			node++;
			continue;
		}
		const int start = leaf / 4;
		const int count = leaf % 4 + 1;
		for (int i = start; i < start + count; i++) {
			const MeshTriangle &triangle = p_mesh.triangles[p_mesh.order[i]];
			// three.js Ray.intersectTriangle with backfaceCulling disabled.
			const Vec3 edge1 = triangle.position[1] - triangle.position[0];
			const Vec3 edge2 = triangle.position[2] - triangle.position[0];
			const Vec3 h = cross(p_direction, edge2);
			const double det = dot(edge1, h);
			if (std::fabs(det) < 1e-12) {
				continue;
			}
			const double inv_det = 1.0 / det;
			const Vec3 s = p_origin - triangle.position[0];
			const double u = dot(s, h) * inv_det;
			if (u < 0.0 || u > 1.0) {
				continue;
			}
			const Vec3 q = cross(s, edge1);
			const double v = dot(p_direction, q) * inv_det;
			if (v < 0.0 || u + v > 1.0) {
				continue;
			}
			const double t = dot(edge2, q) * inv_det;
			// three.js Ray.intersectTriangle only reports hits in front of the origin.
			if (t < 0.0 || t > p_limit + MESH_EPSILON) {
				continue;
			}
			MeshRayHit hit;
			hit.distance = t;
			hit.normal = normalized(cross(edge1, edge2));
			p_visit(p_mesh.order[i], u, v, hit);
		}
		node = p_mesh.node_escape[node];
	}
}

} // namespace

TriangleMesh build_triangle_mesh(std::vector<MeshTriangle> p_triangles) {
	TriangleMesh mesh;
	mesh.triangles = std::move(p_triangles);
	const int triangle_count = int(mesh.triangles.size());
	if (triangle_count == 0) {
		return mesh;
	}
	struct Bounds {
		Vec3 min;
		Vec3 max;
	};
	std::vector<Bounds> bounds(triangle_count);
	for (int i = 0; i < triangle_count; i++) {
		const MeshTriangle &triangle = mesh.triangles[i];
		Bounds box;
		box.min = triangle.position[0];
		box.max = triangle.position[0];
		for (int v = 1; v < 3; v++) {
			for (int axis = 0; axis < 3; axis++) {
				box.min[axis] = std::min(box.min[axis], triangle.position[v][axis]);
				box.max[axis] = std::max(box.max[axis], triangle.position[v][axis]);
			}
		}
		bounds[i] = box;
	}
	// Prototype buildBVH(): preorder nodes with an escape index, leaves hold up to four
	// triangles split by the median of the longest axis.
	std::function<void(const std::vector<int> &)> split = [&](const std::vector<int> &p_indices) {
		Bounds box = bounds[p_indices[0]];
		for (int index : p_indices) {
			for (int axis = 0; axis < 3; axis++) {
				box.min[axis] = std::min(box.min[axis], bounds[index].min[axis]);
				box.max[axis] = std::max(box.max[axis], bounds[index].max[axis]);
			}
		}
		const int node = int(mesh.node_min.size());
		mesh.node_min.push_back(box.min);
		mesh.node_max.push_back(box.max);
		mesh.node_escape.push_back(0);
		mesh.node_leaf.push_back(-1);
		if (int(p_indices.size()) <= 4) {
			mesh.node_leaf[node] = int(mesh.order.size()) * 4 + int(p_indices.size()) - 1;
			mesh.order.insert(mesh.order.end(), p_indices.begin(), p_indices.end());
		} else {
			const Vec3 size = box.max - box.min;
			int axis = 0;
			for (int candidate = 1; candidate < 3; candidate++) {
				if (size[candidate] > size[axis]) {
					axis = candidate;
				}
			}
			std::vector<int> sorted = p_indices;
			std::stable_sort(sorted.begin(), sorted.end(), [&](int p_left, int p_right) {
				return bounds[p_left].min[axis] + bounds[p_left].max[axis] < bounds[p_right].min[axis] + bounds[p_right].max[axis];
			});
			const int middle = int(sorted.size()) / 2;
			split(std::vector<int>(sorted.begin(), sorted.begin() + middle));
			split(std::vector<int>(sorted.begin() + middle, sorted.end()));
		}
		mesh.node_escape[node] = int(mesh.node_min.size());
	};
	std::vector<int> indices(triangle_count);
	for (int i = 0; i < triangle_count; i++) {
		indices[i] = i;
	}
	split(indices);

	// classifyVolumes(): weld attribute seams at the intersection query tolerance and
	// keep only shells whose every edge has two opposite halves.
	std::vector<int> parents(triangle_count);
	for (int i = 0; i < triangle_count; i++) {
		parents[i] = i;
	}
	std::function<int(int)> root = [&](int p_index) {
		while (parents[p_index] != p_index) {
			parents[p_index] = parents[parents[p_index]];
			p_index = parents[p_index];
		}
		return p_index;
	};
	struct EdgeKey {
		int64_t a[3];
		int64_t b[3];
		bool operator<(const EdgeKey &p_other) const {
			for (int i = 0; i < 3; i++) {
				if (a[i] != p_other.a[i]) {
					return a[i] < p_other.a[i];
				}
			}
			for (int i = 0; i < 3; i++) {
				if (b[i] != p_other.b[i]) {
					return b[i] < p_other.b[i];
				}
			}
			return false;
		}
	};
	struct EdgeEntry {
		std::vector<int> triangles;
		int balance = 0;
	};
	std::map<EdgeKey, EdgeEntry> edges;
	for (int i = 0; i < triangle_count; i++) {
		int64_t points[3][3];
		for (int v = 0; v < 3; v++) {
			for (int axis = 0; axis < 3; axis++) {
				points[v][axis] = int64_t(js_round(mesh.triangles[i].position[v][axis] / MESH_WELD));
			}
		}
		for (int j = 0; j < 3; j++) {
			const int64_t *first = points[j];
			const int64_t *second = points[(j + 1) % 3];
			bool forward = false;
			bool ordered = false;
			for (int axis = 0; axis < 3 && !ordered; axis++) {
				if (first[axis] != second[axis]) {
					ordered = true;
					forward = first[axis] < second[axis];
				}
			}
			EdgeKey key;
			for (int axis = 0; axis < 3; axis++) {
				key.a[axis] = forward ? first[axis] : second[axis];
				key.b[axis] = forward ? second[axis] : first[axis];
			}
			EdgeEntry &edge = edges[key];
			if (!edge.triangles.empty()) {
				parents[root(i)] = root(edge.triangles[0]);
			}
			edge.triangles.push_back(i);
			edge.balance += forward ? 1 : -1;
		}
	}
	std::set<int> closed;
	for (int i = 0; i < triangle_count; i++) {
		closed.insert(root(i));
	}
	for (const auto &entry : edges) {
		if (entry.second.triangles.size() != 2 || entry.second.balance != 0) {
			closed.erase(root(entry.second.triangles[0]));
		}
	}
	mesh.shell.resize(triangle_count);
	mesh.shell_closed.assign(triangle_count, 0);
	for (int i = 0; i < triangle_count; i++) {
		mesh.shell[i] = root(i);
		mesh.shell_closed[i] = closed.count(mesh.shell[i]) ? 1 : 0;
	}
	mesh.has_closed_shell = !closed.empty();
	return mesh;
}

MeshSample mesh_closest(const TriangleMesh &p_mesh, const Vec3 &p_point, bool p_attributes) {
	MeshSample sample;
	const int node_count = int(p_mesh.node_min.size());
	double limit = std::numeric_limits<double>::infinity();
	Vec3 closest_point;
	int node = 0;
	while (node < node_count) {
		const Vec3 nearest = Vec3(std::max(p_mesh.node_min[node].x, std::min(p_point.x, p_mesh.node_max[node].x)),
				std::max(p_mesh.node_min[node].y, std::min(p_point.y, p_mesh.node_max[node].y)),
				std::max(p_mesh.node_min[node].z, std::min(p_point.z, p_mesh.node_max[node].z)));
		const Vec3 outside = nearest - p_point;
		if (dot(outside, outside) > limit) {
			node = p_mesh.node_escape[node];
			continue;
		}
		const int leaf = p_mesh.node_leaf[node];
		if (leaf < 0) {
			node++;
			continue;
		}
		const int start = leaf / 4;
		const int count = leaf % 4 + 1;
		for (int i = start; i < start + count; i++) {
			const int triangle_index = p_mesh.order[i];
			const MeshTriangle &triangle = p_mesh.triangles[triangle_index];
			const double squared = point_triangle_distance_squared(p_point, triangle.position[0], triangle.position[1], triangle.position[2], closest_point);
			if (squared >= limit) {
				continue;
			}
			const double distance = std::sqrt(squared);
			limit = squared;
			sample.valid = true;
			sample.distance = distance;
			if (p_attributes) {
				// Barycentric weights of the closest point, as in geometry-query.js.
				const Vec3 edge1 = triangle.position[1] - triangle.position[0];
				const Vec3 edge2 = triangle.position[2] - triangle.position[0];
				const Vec3 relative = closest_point - triangle.position[0];
				const double d00 = dot(edge1, edge1);
				const double d01 = dot(edge1, edge2);
				const double d11 = dot(edge2, edge2);
				const double d20 = dot(relative, edge1);
				const double d21 = dot(relative, edge2);
				const double denominator = d00 * d11 - d01 * d01;
				double v = 0.0;
				double w = 0.0;
				if (std::fabs(denominator) > 0.0) {
					v = (d11 * d20 - d01 * d21) / denominator;
					w = (d00 * d21 - d01 * d20) / denominator;
				}
				const double u = 1.0 - v - w;
				sample.color = triangle.color[0] * u + triangle.color[1] * v + triangle.color[2] * w;
				sample.normal = normalized(cross(edge1, edge2));
			}
		}
		node = p_mesh.node_escape[node];
	}
	return sample;
}

bool mesh_contains(const TriangleMesh &p_mesh, const Vec3 &p_point) {
	if (!p_mesh.has_closed_shell) {
		return false;
	}
	const Vec3 direction = normalized(Vec3(0.81, 0.30, 0.50));
	bool on_surface = false;
	struct ShellCrossings {
		std::vector<std::pair<double, double>> hits;
	};
	std::map<int, ShellCrossings> crossings;
	mesh_walk(p_mesh, p_point, direction, std::numeric_limits<double>::infinity(), [&](int p_triangle, double, double, const MeshRayHit &p_hit) {
		if (p_hit.distance < MESH_EPSILON) {
			on_surface = true;
			return;
		}
		if (!p_mesh.shell_closed[p_triangle]) {
			return;
		}
		crossings[p_mesh.shell[p_triangle]].hits.emplace_back(p_hit.distance, dot(p_hit.normal, direction) >= 0.0 ? 1.0 : -1.0);
	});
	if (on_surface) {
		return true;
	}
	for (auto &entry : crossings) {
		std::vector<std::pair<double, double>> &hits = entry.second.hits;
		std::sort(hits.begin(), hits.end(), [](const std::pair<double, double> &p_left, const std::pair<double, double> &p_right) {
			return p_left.first < p_right.first;
		});
		int winding = 0;
		double last = -std::numeric_limits<double>::infinity();
		// The prototype collects the *distinct* signs of coincident hits (a Set), so
		// paired front/back crossings at the same distance cancel.
		bool positive = false;
		bool negative = false;
		auto flush = [&]() {
			if (positive) {
				winding += 1;
			}
			if (negative) {
				winding -= 1;
			}
			positive = false;
			negative = false;
		};
		for (const auto &hit : hits) {
			if (hit.first - last > MESH_EPSILON) {
				flush();
				last = hit.first;
			}
			if (hit.second > 0.0) {
				positive = true;
			} else {
				negative = true;
			}
		}
		flush();
		if (winding != 0) {
			return true;
		}
	}
	return false;
}

ColorSdfField bake_mesh_color_sdf(const TriangleMesh &p_mesh, int p_resolution, const std::atomic<bool> *p_cancel, int p_threads) {
	ColorSdfField field;
	const int triangle_count = int(p_mesh.triangles.size());
	if (triangle_count == 0) {
		return field;
	}
	Vec3 bounds_min = p_mesh.triangles[0].position[0];
	Vec3 bounds_max = p_mesh.triangles[0].position[0];
	for (const MeshTriangle &triangle : p_mesh.triangles) {
		for (int v = 0; v < 3; v++) {
			for (int axis = 0; axis < 3; axis++) {
				bounds_min[axis] = std::min(bounds_min[axis], triangle.position[v][axis]);
				bounds_max[axis] = std::max(bounds_max[axis], triangle.position[v][axis]);
			}
		}
	}
	field.cell = max3(bounds_max.x - bounds_min.x, bounds_max.y - bounds_min.y, bounds_max.z - bounds_min.z) / double(p_resolution);
	field.min = bounds_min - Vec3(2.0 * field.cell, 2.0 * field.cell, 2.0 * field.cell);
	for (int axis = 0; axis < 3; axis++) {
		field.size[axis] = int(std::ceil((bounds_max[axis] - bounds_min[axis]) / field.cell)) + 5;
	}
	const int count = field.size[0] * field.size[1] * field.size[2];
	field.distance_scale = hypot3(field.size[0] * field.cell, field.size[1] * field.cell, field.size[2] * field.cell) / 32767.0;
	field.distance.resize(count);
	// One z slice per parallel unit: every voxel owns its distance and colour slot, so the
	// field is identical to the serial sweep.
	std::atomic<bool> cancelled(false);
	parallel_for(field.size[2], p_threads, [&](int z) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int y = 0; y < field.size[1]; y++) {
			for (int x = 0; x < field.size[0]; x++) {
				const Vec3 p(field.min.x + x * field.cell, field.min.y + y * field.cell, field.min.z + z * field.cell);
				const MeshSample sample = mesh_closest(p_mesh, p, false);
				const double signed_value = mesh_contains(p_mesh, p) ? -sample.distance : sample.distance;
				field.distance[x + field.size[0] * (y + field.size[1] * z)] = int16_t(js_round(signed_value / field.distance_scale));
			}
		}
	});
	if (cancelled.load()) {
		return ColorSdfField();
	}
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = int(std::ceil(double(field.size[axis] - 1) / 4.0)) + 1;
	}
	const int color_count = field.color_size[0] * field.color_size[1] * field.color_size[2];
	field.color.resize(color_count * 3);
	parallel_for(field.color_size[2], p_threads, [&](int z) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int y = 0; y < field.color_size[1]; y++) {
			for (int x = 0; x < field.color_size[0]; x++) {
				const Vec3 p(field.min.x + double(x) / double(field.color_size[0] - 1) * (field.size[0] - 1) * field.cell,
						field.min.y + double(y) / double(field.color_size[1] - 1) * (field.size[1] - 1) * field.cell,
						field.min.z + double(z) / double(field.color_size[2] - 1) * (field.size[2] - 1) * field.cell);
				const MeshSample sample = mesh_closest(p_mesh, p, true);
				const int index = x + field.color_size[0] * (y + field.color_size[1] * z);
				for (int channel = 0; channel < 3; channel++) {
					const double clamped = std::max(0.0, std::min(1.0, sample.color[channel]));
					field.color[index * 3 + channel] = uint8_t(js_round(clamped * 255.0));
				}
			}
		}
	});
	if (cancelled.load()) {
		return ColorSdfField();
	}
	return field;
}

std::vector<float> mesh_node_data(const TriangleMesh &p_mesh) {
	std::vector<float> data(size_t(p_mesh.node_min.size()) * 8);
	for (size_t i = 0; i < p_mesh.node_min.size(); i++) {
		const float values[8] = {
			float(p_mesh.node_min[i].x), float(p_mesh.node_min[i].y), float(p_mesh.node_min[i].z), float(p_mesh.node_escape[i]),
			float(p_mesh.node_max[i].x), float(p_mesh.node_max[i].y), float(p_mesh.node_max[i].z), float(p_mesh.node_leaf[i])
		};
		std::copy(values, values + 8, data.begin() + i * 8);
	}
	return data;
}

std::vector<float> mesh_triangle_data(const TriangleMesh &p_mesh) {
	// 40 floats per triangle: position.xyz + u, normal.xyz + v, color.rgb + 0 for each
	// vertex, then the material index, matching the prototype's triangleData layout.
	std::vector<float> data(p_mesh.order.size() * 40, 0.0f);
	for (size_t i = 0; i < p_mesh.order.size(); i++) {
		const MeshTriangle &triangle = p_mesh.triangles[p_mesh.order[i]];
		float *base = data.data() + i * 40;
		const Vec3 normal = normalized(cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0]));
		for (int v = 0; v < 3; v++) {
			base[v * 4 + 0] = float(triangle.position[v].x);
			base[v * 4 + 1] = float(triangle.position[v].y);
			base[v * 4 + 2] = float(triangle.position[v].z);
			base[v * 4 + 3] = 0.0f;
			base[12 + v * 4 + 0] = float(normal.x);
			base[12 + v * 4 + 1] = float(normal.y);
			base[12 + v * 4 + 2] = float(normal.z);
			base[12 + v * 4 + 3] = 0.0f;
			base[24 + v * 4 + 0] = float(triangle.color[v].x);
			base[24 + v * 4 + 1] = float(triangle.color[v].y);
			base[24 + v * 4 + 2] = float(triangle.color[v].z);
		}
		base[36] = 0.0f;
	}
	return data;
}

std::vector<float> mesh_material_data() {
	// One opaque, untextured material: full atlas rect (a 1x1 white atlas), clamp wrap,
	// zero cutoff and double-sided, so traceMesh matches the prototype's default material.
	// Rect follows prototype buildAtlas(): (0.5/1, 0.5/1, 0/1, 0/1).
	return { 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
}

} // namespace lrt
