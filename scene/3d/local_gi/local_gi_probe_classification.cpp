/**************************************************************************/
/*  local_gi_probe_classification.cpp                                     */
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

#include "local_gi_probe_classification.h"

#include "core/math/math_defs.h"

bool LocalGIProbeClassifier::is_embedded(const LocalGIBVH &p_bvh, const Vector3 &p_position, const Vector<Vector3> &p_directions) {
	if (p_bvh.is_empty()) {
		return false;
	}

	(void)p_directions;
	// Face3 winding in LocalGI extraction points inward on BoxMesh, so a first hit
	// with dir·n < 0 means the ray started inside that solid. First-hit only also
	// stays correct when the volume clips the outer faces of Cornell wall boxes.
	const Vector3 axes[6] = {
		Vector3(1, 0, 0),
		Vector3(-1, 0, 0),
		Vector3(0, 1, 0),
		Vector3(0, -1, 0),
		Vector3(0, 0, 1),
		Vector3(0, 0, -1),
	};

	int inside_hits = 0;
	for (int i = 0; i < 6; i++) {
		const Vector3 dir = axes[i];
		LocalGIRayHit hit;
		if (!p_bvh.intersect_ray(p_position, dir, hit) || !hit.hit) {
			continue;
		}
		Vector3 normal = hit.normal;
		if (normal.length_squared() < (real_t)CMP_EPSILON2) {
			continue;
		}
		normal.normalize();
		if (dir.dot(normal) < 0.0f) {
			inside_hits++;
		}
	}

	return ((float)inside_hits / 6.0f) > INSIDE_HIT_RATIO_THRESHOLD;
}

void LocalGIProbeClassifier::classify(const LocalGIProbeGrid &p_grid, const LocalGIBVH &p_static_bvh, const LocalGIBVH &p_dynamic_bvh, Vector<uint8_t> &r_active) {
	const int count = p_grid.get_probe_count();
	r_active.resize(count);
	const Vector<Vector3> &directions = p_grid.get_directions();
	for (int i = 0; i < count; i++) {
		const Vector3 position = p_grid.get_position(i);
		const bool inactive = is_embedded(p_static_bvh, position, directions) || is_embedded(p_dynamic_bvh, position, directions);
		r_active.write[i] = inactive ? 0 : 1;
	}
}
