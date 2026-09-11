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
#include <map>

namespace lrt {

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
		// PrimitiveGI.sample transforms the world point into primitive-local space; unit scale keeps distances unchanged.
		const ColorSdfSample value = sample_color_sdf(primitive->field, p_point - primitive->position);
		if (!found || value.distance < r_sample.distance) {
			r_sample = value;
			found = true;
		}
	}
	return found;
}

struct TrunkKey {
	int x = 0;
	int y = 0;
	int z = 0;
	bool operator<(const TrunkKey &p_other) const {
		if (x != p_other.x) {
			return x < p_other.x;
		}
		if (y != p_other.y) {
			return y < p_other.y;
		}
		return z < p_other.z;
	}
};

inline TrunkKey trunk_key(int p_x, int p_y, int p_z) {
	return TrunkKey{ p_x / TRUNK, p_y / TRUNK, p_z / TRUNK };
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

ColorSdfField bake_box_color_sdf(const Vec3 &p_extent, const Vec3 &p_color, int p_resolution) {
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

SdfPrimitive make_sdf_primitive(const Vec3 &p_position, ColorSdfField p_field) {
	SdfPrimitive primitive;
	primitive.position = p_position;
	primitive.field = p_field;
	for (int axis = 0; axis < 3; axis++) {
		primitive.bounds_min[axis] = p_position[axis] + p_field.min[axis];
		primitive.bounds_max[axis] = p_position[axis] + p_field.min[axis] + (p_field.size[axis] - 1) * p_field.cell;
	}
	return primitive;
}

LocalField build_local_data(const Grid &p_grid, const BoxQuery &p_query) {
	LocalField field;
	field.material.assign(size_t(p_grid.count) * 4, 0.0f);
	field.matrices.assign(size_t(p_grid.count) * 48, 0.0f);
	field.links.assign(size_t(p_grid.count), 0u);
	for (int y = 0; y < p_grid.size[1]; y++) {
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
				field.solid_count++;
			}
		}
	}
	const Direction *dirs = directions();
	for (int y = 0; y < p_grid.size[1]; y++) {
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
							field.classification_mismatches++;
						}
						field.links[index] |= 1u << uint32_t(j);
						continue;
					}
					surface = true;
					accumulate_transfer(field.matrices, p_grid, index, direction, hit.normal, hit.color);
				}
				if (surface) {
					field.surface_count++;
				}
			}
		}
	}
	return field;
}

LocalField build_sdf_local_data(const Grid &p_grid, const std::vector<SdfPrimitive> &p_primitives) {
	LocalField field;
	field.material.assign(size_t(p_grid.count) * 4, 0.0f);
	field.matrices.assign(size_t(p_grid.count) * 48, 0.0f);
	field.links.assign(size_t(p_grid.count), 0u);

	// src/sdf-local.js buildTrunks: trunk candidates include the full 26-neighbor support box.
	struct Trunk {
		std::vector<const SdfPrimitive *> candidates;
	};
	std::map<TrunkKey, Trunk> trunks;
	const double spacing = p_grid.spacing;
	const int trunk_size[3] = { p_grid.size[0], p_grid.size[1], p_grid.size[2] };
	for (int y = 0; y < trunk_size[1]; y += TRUNK) {
		for (int z = 0; z < trunk_size[2]; z += TRUNK) {
			for (int x = 0; x < trunk_size[0]; x += TRUNK) {
				const int coord[3] = { x, y, z };
				Vec3 low;
				Vec3 high;
				for (int axis = 0; axis < 3; axis++) {
					low[axis] = p_grid.min[axis] + (coord[axis] - 1) * spacing;
					high[axis] = p_grid.min[axis] + (coord[axis] + 9) * spacing;
				}
				Trunk trunk;
				for (const SdfPrimitive &primitive : p_primitives) {
					bool overlap = true;
					for (int axis = 0; axis < 3; axis++) {
						if (!(primitive.bounds_min[axis] <= high[axis] && primitive.bounds_max[axis] >= low[axis])) {
							overlap = false;
							break;
						}
					}
					if (overlap) {
						trunk.candidates.push_back(&primitive);
					}
				}
				trunks[trunk_key(x, y, z)] = trunk;
			}
		}
	}
	field.trunk_count = int(trunks.size());
	auto trunk_for = [&trunks](int p_x, int p_y, int p_z) -> const Trunk & {
		return trunks[trunk_key(p_x, p_y, p_z)];
	};

	std::vector<ColorSdfSample> samples(size_t(p_grid.count));
	std::vector<uint8_t> sampled(size_t(p_grid.count), 0);
	for (int y = 0; y < p_grid.size[1]; y++) {
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				ColorSdfSample value;
				if (sample_nearest(probe_point(p_grid, x, y, z), trunk_for(x, y, z).candidates, value)) {
					samples[index] = value;
					sampled[index] = 1;
					if (value.distance < 0.0) {
						field.material[index * 4 + 0] = 0.5f;
						field.material[index * 4 + 1] = 0.5f;
						field.material[index * 4 + 2] = 0.5f;
						field.material[index * 4 + 3] = 1.0f;
						field.solid_count++;
					}
				}
			}
		}
	}

	const Direction *dirs = directions();
	for (int y = 0; y < p_grid.size[1]; y++) {
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				if (field.material[index * 4 + 3] != 0.0f) {
					continue;
				}
				const Vec3 origin = probe_point(p_grid, x, y, z);
				const size_t start = field.receivers.size() / 4;
				const Trunk &trunk = trunk_for(x, y, z);
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
					field.receivers.push_back(float(receiver.x));
					field.receivers.push_back(float(receiver.y));
					field.receivers.push_back(float(receiver.z));
					field.receivers.push_back(float(j));
					field.receivers.push_back(float(value.normal.x * facing));
					field.receivers.push_back(float(value.normal.y * facing));
					field.receivers.push_back(float(value.normal.z * facing));
					field.receivers.push_back(0.0f);
					field.receivers.push_back(float(value.color.x));
					field.receivers.push_back(float(value.color.y));
					field.receivers.push_back(float(value.color.z));
					field.receivers.push_back(0.0f);
				}
				const int count = int((field.receivers.size() / 4 - start) / 3);
				field.material[index * 4 + 0] = float(start);
				field.material[index * 4 + 1] = float(count);
				if (count > 0) {
					field.surface_count++;
				}
			}
		}
	}
	return field;
}

void build_local_visibility(LocalField &r_field) {
	const Direction *dirs = directions();
	r_field.local_visibility.assign(size_t(r_field.links.size()) * 4, 0.0f);
	for (size_t i = 0; i < r_field.links.size(); i++) {
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
}

} // namespace lrt
