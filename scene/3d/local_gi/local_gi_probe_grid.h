/**************************************************************************/
/*  local_gi_probe_grid.h                                                 */
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

#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/vector.h"

// Regular probe grid and shared deterministic spherical directions.
// Positions stay in LocalGIVolume local space. No GI / shading yet.
class LocalGIProbeGrid {
	Vector<Vector3> positions;
	Vector<Vector3> directions;
	Vector3i resolution;
	Vector3 size;
	float spacing = 0.5;
	int rays_per_probe = 64;

public:
	static Vector3i compute_resolution(const Vector3 &p_size, float p_spacing);
	static Vector3 cell_position(const Vector3 &p_size, const Vector3i &p_resolution, const Vector3i &p_cell);
	static void generate_directions(int p_count, Vector<Vector3> &r_directions);

	void clear();
	void build(const Vector3 &p_size, float p_spacing, int p_rays_per_probe);

	int get_probe_count() const { return positions.size(); }
	int get_rays_per_probe() const { return directions.size(); }
	int get_ray_budget() const { return positions.size() * directions.size(); }
	Vector3i get_resolution() const { return resolution; }
	Vector3 get_size() const { return size; }
	float get_spacing() const { return spacing; }
	int get_center_probe_index() const;

	Vector3 get_position(int p_index) const;
	Vector3i index_to_cell(int p_index) const;
	int cell_to_index(const Vector3i &p_cell) const;

	const Vector<Vector3> &get_positions() const { return positions; }
	const Vector<Vector3> &get_directions() const { return directions; }

	void collect_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const;
	void collect_probe_rays(int p_probe_index, Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const;
};
