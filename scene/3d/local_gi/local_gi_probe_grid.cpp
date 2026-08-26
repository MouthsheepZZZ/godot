/**************************************************************************/
/*  local_gi_probe_grid.cpp                                               */
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

#include "local_gi_probe_grid.h"

#include "core/math/math_defs.h"
#include "core/math/math_funcs.h"

Vector3i LocalGIProbeGrid::compute_resolution(const Vector3 &p_size, float p_spacing) {
	const float spacing = MAX(p_spacing, 0.05f);
	Vector3i res;
	for (int i = 0; i < 3; i++) {
		const float axis = MAX(p_size[i], 0.01f);
		res[i] = MAX(2, (int)Math::floor((double)axis / (double)spacing + 1e-4));
	}
	return res;
}

Vector3 LocalGIProbeGrid::cell_position(const Vector3 &p_size, const Vector3i &p_resolution, const Vector3i &p_cell) {
	Vector3 pos;
	for (int i = 0; i < 3; i++) {
		const float axis = MAX(p_size[i], 0.01f);
		const int count = MAX(p_resolution[i], 1);
		const float step = axis / (float)count;
		pos[i] = -axis * 0.5f + step * ((float)p_cell[i] + 0.5f);
	}
	return pos;
}

void LocalGIProbeGrid::generate_directions(int p_count, Vector<Vector3> &r_directions) {
	const int count = MAX(p_count, 1);
	r_directions.resize(count);
	if (count == 1) {
		r_directions.write[0] = Vector3(0, 1, 0);
		return;
	}

	// Deterministic Fibonacci lattice on the unit sphere. Same count always
	// yields the same directions; volume transform does not affect them.
	const real_t golden_angle = (real_t)Math::PI * (3.0 - Math::sqrt(5.0));
	for (int i = 0; i < count; i++) {
		const real_t y = 1.0 - (2.0 * ((real_t)i + 0.5)) / (real_t)count;
		const real_t radius = Math::sqrt(MAX((real_t)0.0, 1.0 - y * y));
		const real_t theta = golden_angle * (real_t)i;
		r_directions.write[i] = Vector3(Math::cos(theta) * radius, y, Math::sin(theta) * radius).normalized();
	}
}

void LocalGIProbeGrid::clear() {
	positions.clear();
	directions.clear();
	resolution = Vector3i();
	size = Vector3();
	spacing = 0.5;
	rays_per_probe = 64;
}

void LocalGIProbeGrid::build(const Vector3 &p_size, float p_spacing, int p_rays_per_probe) {
	size = p_size.maxf(0.01);
	spacing = MAX(p_spacing, 0.05f);
	rays_per_probe = MAX(p_rays_per_probe, 1);
	resolution = compute_resolution(size, spacing);

	const int count = resolution.x * resolution.y * resolution.z;
	positions.resize(count);
	int index = 0;
	for (int x = 0; x < resolution.x; x++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int z = 0; z < resolution.z; z++) {
				positions.write[index] = cell_position(size, resolution, Vector3i(x, y, z));
				index++;
			}
		}
	}

	generate_directions(rays_per_probe, directions);
}

int LocalGIProbeGrid::get_center_probe_index() const {
	if (positions.is_empty()) {
		return 0;
	}
	return cell_to_index(Vector3i(resolution.x / 2, resolution.y / 2, resolution.z / 2));
}

Vector3 LocalGIProbeGrid::get_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, positions.size(), Vector3());
	return positions[p_index];
}

Vector3i LocalGIProbeGrid::index_to_cell(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, positions.size(), Vector3i());
	const int yz = resolution.y * resolution.z;
	const int x = p_index / yz;
	const int rem = p_index - x * yz;
	const int y = rem / resolution.z;
	const int z = rem - y * resolution.z;
	return Vector3i(x, y, z);
}

int LocalGIProbeGrid::cell_to_index(const Vector3i &p_cell) const {
	return (p_cell.x * resolution.y + p_cell.y) * resolution.z + p_cell.z;
}

void LocalGIProbeGrid::local_to_trilinear(const Vector3 &p_local, Vector3i &r_base, Vector3 &r_frac) const {
	r_base = Vector3i();
	r_frac = Vector3();
	if (positions.is_empty()) {
		return;
	}

	for (int i = 0; i < 3; i++) {
		const int count = MAX(resolution[i], 1);
		if (count < 2) {
			continue;
		}
		const float axis = MAX(size[i], 0.01f);
		const float step = axis / (float)count;
		const float grid = (p_local[i] + axis * 0.5f) / step - 0.5f;
		const float clamped = CLAMP(grid, 0.0f, (float)(count - 1));
		int base = (int)Math::floor((double)clamped);
		if (base >= count - 1) {
			base = count - 2;
		}
		r_base[i] = base;
		r_frac[i] = CLAMP(clamped - (float)base, 0.0f, 1.0f);
	}
}

int LocalGIProbeGrid::nearest_direction_index(const Vector3 &p_direction) const {
	if (directions.is_empty()) {
		return 0;
	}
	Vector3 dir = p_direction;
	if (dir.length_squared() < (real_t)CMP_EPSILON2) {
		return 0;
	}
	dir.normalize();
	int best = 0;
	real_t best_dot = dir.dot(directions[0]);
	for (int i = 1; i < directions.size(); i++) {
		const real_t d = dir.dot(directions[i]);
		if (d > best_dot) {
			best_dot = d;
			best = i;
		}
	}
	return best;
}

void LocalGIProbeGrid::collect_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const {
	const int budget = get_ray_budget();
	r_origins.resize(budget);
	r_directions.resize(budget);
	int index = 0;
	for (int p = 0; p < positions.size(); p++) {
		for (int d = 0; d < directions.size(); d++) {
			r_origins.write[index] = positions[p];
			r_directions.write[index] = directions[d];
			index++;
		}
	}
}

void LocalGIProbeGrid::collect_probe_rays(int p_probe_index, Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const {
	ERR_FAIL_INDEX(p_probe_index, positions.size());
	const int count = directions.size();
	r_origins.resize(count);
	r_directions.resize(count);
	const Vector3 origin = positions[p_probe_index];
	for (int d = 0; d < count; d++) {
		r_origins.write[d] = origin;
		r_directions.write[d] = directions[d];
	}
}
