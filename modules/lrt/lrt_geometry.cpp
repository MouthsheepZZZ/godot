/**************************************************************************/
/*  lrt_geometry.cpp                                                      */
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

#include "lrt_geometry.h"

#include "core/io/marshalls.h"
#include "scene/3d/node_3d.h"

namespace LRTGeometry {

namespace {

struct DirectionTable {
	Direction values[26];

	DirectionTable() {
		int index = 0;
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					if (x == 0 && y == 0 && z == 0) {
						continue;
					}
					Direction &entry = values[index++];
					entry.offset = Vector3i(x, y, z);
					const Vector3 normal = Vector3(x, y, z).normalized();
					entry.sh[0] = SH_C0;
					entry.diffuse[0] = SH_C0;
					for (int axis = 0; axis < 3; axis++) {
						entry.sh[axis + 1] = SH_C1 * normal[axis];
						entry.diffuse[axis + 1] = -(2.0f / 3.0f) * entry.sh[axis + 1];
					}
					Vector3 axis;
					axis[normal.abs().min_axis_index()] = 1;
					const Vector3 tangent = normal.cross(axis).normalized() * 0.05f;
					const Vector3 bitangent = normal.cross(tangent);
					entry.edge_offsets[0] = tangent;
					entry.edge_offsets[1] = -tangent;
					entry.edge_offsets[2] = bitangent;
					entry.edge_offsets[3] = -bitangent;
				}
			}
		}
	}
};

double polynomial(const double *p_coefficients, double p_t) {
	return ((p_coefficients[3] * p_t + p_coefficients[2]) * p_t + p_coefficients[1]) * p_t + p_coefficients[0];
}

} // namespace

const Direction &direction(int p_index) {
	static const DirectionTable table;
	return table.values[p_index];
}

void edge_segments(const AABB &p_bounds, float p_spacing, const Vector3i &p_probe, int p_direction, Vector3 *r_from, Vector3 *r_to) {
	const Vector3i delta = direction(p_direction).offset;
	const Vector3i start = p_direction >= 13 ? p_probe : p_probe + delta;
	const Vector3i end = p_direction >= 13 ? p_probe + delta : p_probe;
	const Direction &canonical = direction(p_direction >= 13 ? p_direction : 25 - p_direction);
	for (int sample = 0; sample < 4; sample++) {
		const Vector3 offset = canonical.edge_offsets[sample] * p_spacing;
		r_from[sample] = p_bounds.position + Vector3(start) * p_spacing + offset;
		r_to[sample] = p_bounds.position + Vector3(end) * p_spacing + offset;
	}
}

VolumeFrame::VolumeFrame(Node3D *p_volume) {
	world_to_volume = p_volume->get_global_transform().affine_inverse();
	Transform3D transform;
	Node3D *ancestor = p_volume;
	while (ancestor) {
		volume_to_ancestor[ancestor] = transform;
		if (ancestor->is_set_as_top_level()) {
			break;
		}
		transform = ancestor->get_transform() * transform;
		ancestor = ancestor->get_parent_node_3d();
	}
}

Transform3D VolumeFrame::relative_transform(Node3D *p_instance) const {
	Transform3D transform;
	Node3D *ancestor = p_instance;
	while (ancestor) {
		const Transform3D *relative = volume_to_ancestor.getptr(ancestor);
		if (relative) {
			return relative->affine_inverse() * transform;
		}
		if (ancestor->is_set_as_top_level()) {
			break;
		}
		transform = ancestor->get_transform() * transform;
		ancestor = ancestor->get_parent_node_3d();
	}
	return world_to_volume * p_instance->get_global_transform();
}

void Probe::clear_inside() {
	*this = Probe();
	inside = true;
	visibility[0] = 0;
}

void Probe::add_sample(int p_direction, const Color &p_albedo, const Color &p_emission) {
	const Direction &sample = direction(p_direction);
	constexpr float weight = 4.0f * Math::PI / 26.0f;
	for (int row = 0; row < 4; row++) {
		visibility[row] -= weight * sample.sh[row];
		for (int channel = 0; channel < 3; channel++) {
			emission[channel * 4 + row] += weight * p_emission[channel] * sample.sh[row] * (row == 0 ? 1.0f : -1.0f);
			for (int column = 0; column < 4; column++) {
				transfer[channel][row + column * 4] += weight * p_albedo[channel] * sample.diffuse[row] * sample.sh[column];
			}
		}
	}
}

void Probe::compose(const Probe &p_other) {
	if (inside || p_other.inside) {
		clear_inside();
		return;
	}
	for (int channel = 0; channel < 3; channel++) {
		for (int coefficient = 0; coefficient < 16; coefficient++) {
			transfer[channel][coefficient] += p_other.transfer[channel][coefficient];
		}
	}
	for (int coefficient = 0; coefficient < 12; coefficient++) {
		emission[coefficient] += p_other.emission[coefficient];
	}
	float product[4] = {};
	for (int coefficient = 0; coefficient < 4; coefficient++) {
		product[0] += SH_C0 * visibility[coefficient] * p_other.visibility[coefficient];
	}
	for (int coefficient = 1; coefficient < 4; coefficient++) {
		product[coefficient] = SH_C0 * (visibility[0] * p_other.visibility[coefficient] + p_other.visibility[0] * visibility[coefficient]);
	}
	for (int coefficient = 0; coefficient < 4; coefficient++) {
		visibility[coefficient] = product[coefficient];
	}
}

void Probe::decode(const uint8_t *p_ltm, const uint8_t *p_vis, const float *p_emission) {
	inside = (p_ltm[35] & 1) != 0;
	for (int channel = 0; channel < 3; channel++) {
		const float color = p_ltm[32 + channel] / 255.0f;
		for (int coefficient = 0; coefficient < 16; coefficient++) {
			transfer[channel][coefficient] = color * decode_half(p_ltm + coefficient * 2);
		}
	}
	for (int coefficient = 0; coefficient < 4; coefficient++) {
		visibility[coefficient] = decode_half(p_vis + coefficient * 2);
	}
	for (int coefficient = 0; coefficient < 12; coefficient++) {
		emission[coefficient] = p_emission ? p_emission[coefficient] : 0;
	}
}

void Probe::encode(uint8_t *r_ltm, uint8_t *r_vis, float *r_emission) const {
	float magnitude[3];
	for (int channel = 0; channel < 3; channel++) {
		magnitude[channel] = MAX(transfer[channel][0], 0.0f);
	}
	const float maximum = MAX(magnitude[0], MAX(magnitude[1], magnitude[2]));
	const float weights[3] = { 0.2126f, 0.7152f, 0.0722f };
	float color[3] = {};
	float luminance = 0;
	if (maximum > 0) {
		for (int channel = 0; channel < 3; channel++) {
			color[channel] = magnitude[channel] / maximum;
			luminance += weights[channel] * color[channel];
		}
	}
	for (int coefficient = 0; coefficient < 16; coefficient++) {
		float value = 0;
		if (luminance > 0) {
			for (int channel = 0; channel < 3; channel++) {
				value += weights[channel] * transfer[channel][coefficient];
			}
			value /= luminance;
		}
		encode_half(value, r_ltm + coefficient * 2);
	}
	for (int channel = 0; channel < 3; channel++) {
		r_ltm[32 + channel] = uint8_t(Math::round(color[channel] * 255.0f));
	}
	r_ltm[35] = inside ? 1 : 0;
	for (int coefficient = 0; coefficient < 4; coefficient++) {
		encode_half(visibility[coefficient], r_vis + coefficient * 2);
	}
	if (r_emission) {
		for (int coefficient = 0; coefficient < 12; coefficient++) {
			r_emission[coefficient] = emission[coefficient];
		}
	}
}

void StaticCache::load(const Dictionary &p_data) {
	bounds = p_data["bounds"];
	grid = p_data["grid_size"];
	spacing = p_data["probe_spacing"];
	ltm = p_data["ltm"];
	local_vis = p_data["local_vis"];
	edge_masks = p_data["edge_masks"];
	emission = p_data["emission"];
}

void ColorSDF::load(const Dictionary &p_data) {
	bounds = p_data["bounds"];
	grid = p_data["grid_size"];
	distance = p_data["distance"];
	color = p_data["color"];
	emission = p_data["emission"];
}

double ColorSDF::cell_value(const Vector3i &p_cell, int p_corner) const {
	const Vector3i point = p_cell + Vector3i(p_corner & 1, (p_corner >> 1) & 1, (p_corner >> 2) & 1);
	const int64_t index = (int64_t(point.z) * grid.y + point.y) * grid.x + point.x;
	return int16_t(decode_uint16(distance.ptr() + index * 2));
}

bool ColorSDF::contains(const Vector3 &p_point, Color *r_color, Color *r_emission) const {
	if (!bounds.has_point(p_point)) {
		return false;
	}
	Vector3i cell;
	double fraction[3];
	for (int axis = 0; axis < 3; axis++) {
		const double position = (double(p_point[axis]) - bounds.position[axis]) / bounds.size[axis] * (grid[axis] - 1);
		cell[axis] = CLAMP(int(Math::floor(position)), 0, grid[axis] - 2);
		fraction[axis] = position - cell[axis];
	}
	double value = 0;
	Color albedo(0, 0, 0, 0);
	Color light(0, 0, 0, 0);
	for (int corner = 0; corner < 8; corner++) {
		const double weight = ((corner & 1) ? fraction[0] : 1 - fraction[0]) *
				((corner & 2) ? fraction[1] : 1 - fraction[1]) * ((corner & 4) ? fraction[2] : 1 - fraction[2]);
		value += weight * cell_value(cell, corner);
		const Vector3i point = cell + Vector3i(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
		const int64_t index = (int64_t(point.z) * grid.y + point.y) * grid.x + point.x;
		if (r_color) {
			for (int channel = 0; channel < 3; channel++) {
				albedo[channel] += weight * color[index * 4 + channel] / 255.0;
			}
		}
		if (r_emission && !emission.is_empty()) {
			for (int channel = 0; channel < 3; channel++) {
				light[channel] += weight * emission[index * 3 + channel];
			}
		}
	}
	if (value > 0) {
		return false;
	}
	if (r_color) {
		*r_color = albedo;
	}
	if (r_emission) {
		*r_emission = light;
	}
	return true;
}

bool ColorSDF::cell_intersects(const Vector3i &p_cell, const double *p_from, const double *p_to) const {
	double coefficients[4] = {};
	for (int corner = 0; corner < 8; corner++) {
		double a[3], b[3];
		for (int axis = 0; axis < 3; axis++) {
			const double sign = (corner & (1 << axis)) ? 1 : -1;
			a[axis] = sign > 0 ? p_from[axis] : 1 - p_from[axis];
			b[axis] = (p_to[axis] - p_from[axis]) * sign;
		}
		const double value = cell_value(p_cell, corner);
		coefficients[0] += value * a[0] * a[1] * a[2];
		coefficients[1] += value * (b[0] * a[1] * a[2] + a[0] * b[1] * a[2] + a[0] * a[1] * b[2]);
		coefficients[2] += value * (b[0] * b[1] * a[2] + b[0] * a[1] * b[2] + a[0] * b[1] * b[2]);
		coefficients[3] += value * b[0] * b[1] * b[2];
	}
	if (coefficients[0] <= 0 || polynomial(coefficients, 1) <= 0) {
		return true;
	}
	// Along a ray the trilinear field is cubic; testing its stationary points
	// catches interior crossings even when both ends of the cell are outside.
	const double a = 3 * coefficients[3], b = 2 * coefficients[2], c = coefficients[1];
	if (a == 0) {
		if (b == 0) {
			return false;
		}
		const double root = -c / b;
		return root > 0 && root < 1 && polynomial(coefficients, root) <= 0;
	}
	const double discriminant = b * b - 4 * a * c;
	if (discriminant < 0) {
		return false;
	}
	const double q = -0.5 * (b + (b >= 0 ? 1 : -1) * Math::sqrt(discriminant));
	if (q == 0) {
		return false; // The only stationary point is at the already tested endpoint.
	}
	const double roots[2] = { q / a, c / q };
	for (double root : roots) {
		if (root > 0 && root < 1 && polynomial(coefficients, root) <= 0) {
			return true;
		}
	}
	return false;
}

bool ColorSDF::intersects_segment(const Vector3 &p_from, const Vector3 &p_to) const {
	double origin[3], delta[3];
	double begin = 0, end = 1;
	for (int axis = 0; axis < 3; axis++) {
		const double factor = double(grid[axis] - 1) / bounds.size[axis];
		origin[axis] = (double(p_from[axis]) - bounds.position[axis]) * factor;
		delta[axis] = (double(p_to[axis]) - p_from[axis]) * factor;
		if (delta[axis] == 0) {
			if (origin[axis] < 0 || origin[axis] > grid[axis] - 1) {
				return false;
			}
			continue;
		}
		const double first = -origin[axis] / delta[axis];
		const double last = (grid[axis] - 1 - origin[axis]) / delta[axis];
		begin = MAX(begin, MIN(first, last));
		end = MIN(end, MAX(first, last));
	}
	if (begin > end) {
		return false;
	}
	Vector3i cell;
	for (int axis = 0; axis < 3; axis++) {
		const double position = origin[axis] + delta[axis] * begin;
		const double base = Math::floor(position);
		cell[axis] = int(base) - (delta[axis] < 0 && position == base ? 1 : 0);
		cell[axis] = CLAMP(cell[axis], 0, grid[axis] - 2);
	}
	while (true) {
		double next = end;
		double boundary[3];
		for (int axis = 0; axis < 3; axis++) {
			boundary[axis] = delta[axis] == 0 ? INFINITY : (cell[axis] + (delta[axis] > 0 ? 1 : 0) - origin[axis]) / delta[axis];
			next = MIN(next, boundary[axis]);
		}
		double from[3], to[3];
		for (int axis = 0; axis < 3; axis++) {
			from[axis] = origin[axis] + delta[axis] * begin - cell[axis];
			to[axis] = origin[axis] + delta[axis] * next - cell[axis];
		}
		if (cell_intersects(cell, from, to)) {
			return true;
		}
		if (next >= end) {
			return false;
		}
		for (int axis = 0; axis < 3; axis++) {
			if (boundary[axis] == next) {
				cell[axis] += delta[axis] > 0 ? 1 : -1;
			}
		}
		begin = next;
	}
}

Trunk rebuild_trunk(const StaticCache &p_static, const Vector<Instance> &p_instances, const Vector3i &p_coordinate) {
	Trunk result;
	result.coordinate = p_coordinate;
	result.origin = p_coordinate * TRUNK_SIZE;
	result.size = (p_static.grid - result.origin).min(Vector3i(TRUNK_SIZE, TRUNK_SIZE, TRUNK_SIZE));
	const AABB region(p_static.bounds.position + Vector3(result.origin) * p_static.spacing,
			Vector3(result.size - Vector3i(1, 1, 1)) * p_static.spacing);
	Vector<int> candidates;
	bool has_emission = !p_static.emission.is_empty();
	for (int index = 0; index < p_instances.size(); index++) {
		if (region.grow(p_static.spacing * 1.05f).intersects_inclusive(p_instances[index].bounds)) {
			candidates.push_back(index);
			result.instances.push_back(p_instances[index].id);
			has_emission |= !p_instances[index].sdf.emission.is_empty();
		}
	}
	const int count = result.size.x * result.size.y * result.size.z;
	if (result.ltm.resize(count * 36) != OK || result.local_vis.resize(count * 8) != OK ||
			result.edge_masks.resize(count * 26) != OK || (has_emission && result.emission.resize(count * 12) != OK)) {
		return Trunk();
	}
	uint8_t *ltm = result.ltm.ptrw();
	uint8_t *vis = result.local_vis.ptrw();
	uint8_t *masks = result.edge_masks.ptrw();
	float *emission = result.emission.ptrw();
	for (int z = 0; z < result.size.z; z++) {
		for (int y = 0; y < result.size.y; y++) {
			for (int x = 0; x < result.size.x; x++) {
				const Vector3i coordinate = result.origin + Vector3i(x, y, z);
				const int index = (z * result.size.y + y) * result.size.x + x;
				const int64_t source = (int64_t(coordinate.z) * p_static.grid.y + coordinate.y) * p_static.grid.x + coordinate.x;
				memcpy(ltm + index * 36, p_static.ltm.ptr() + source * 36, 36);
				memcpy(vis + index * 8, p_static.local_vis.ptr() + source * 8, 8);
				memcpy(masks + index * 26, p_static.edge_masks.ptr() + source * 26, 26);
				if (has_emission) {
					for (int coefficient = 0; coefficient < 12; coefficient++) {
						emission[index * 12 + coefficient] = p_static.emission.is_empty() ? 0 : p_static.emission[source * 12 + coefficient];
					}
				}
				const Vector3 point = p_static.bounds.position + Vector3(coordinate) * p_static.spacing;
				if ((ltm[index * 36 + 35] & 1) == 0) {
					Probe composed;
					bool changed = false;
					for (int candidate : candidates) {
						const Instance &instance = p_instances[candidate];
						if (instance.sdf.contains(instance.to_instance.xform(point))) {
							composed.clear_inside();
							changed = true;
							break;
						}
						Probe contribution;
						bool occupied = false;
						for (int neighbor = 0; neighbor < 26; neighbor++) {
							const Vector3 sample = p_static.bounds.position + Vector3(coordinate + direction(neighbor).offset) * p_static.spacing;
							Color albedo, light;
							if (instance.sdf.contains(instance.to_instance.xform(sample), &albedo, &light)) {
								contribution.add_sample(neighbor, albedo, light);
								occupied = true;
							}
						}
						if (!occupied) {
							continue;
						}
						if (!changed) {
							composed.decode(ltm + index * 36, vis + index * 8, has_emission ? emission + index * 12 : nullptr);
							changed = true;
						}
						composed.compose(contribution);
					}
					if (changed) {
						composed.encode(ltm + index * 36, vis + index * 8, has_emission ? emission + index * 12 : nullptr);
					}
				}
				for (int neighbor = 0; neighbor < 26; neighbor++) {
					uint8_t &mask = masks[index * 26 + neighbor];
					if (mask == 0 || candidates.is_empty()) {
						continue;
					}
					Vector3 from[4], to[4];
					edge_segments(p_static.bounds, p_static.spacing, coordinate, neighbor, from, to);
					for (int candidate : candidates) {
						const Instance &instance = p_instances[candidate];
						for (int bit = 0; bit < 4; bit++) {
							if ((mask & (1 << bit)) && instance.sdf.intersects_segment(instance.to_instance.xform(from[bit]), instance.to_instance.xform(to[bit]))) {
								mask &= ~(1 << bit);
							}
						}
						if (mask == 0) {
							break;
						}
					}
				}
			}
		}
	}
	return result;
}

} // namespace LRTGeometry
