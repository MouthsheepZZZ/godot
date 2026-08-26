/**************************************************************************/
/*  local_gi_probe_sample.h                                               */
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

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/templates/vector.h"
#include "scene/3d/local_gi/local_gi_probe_grid.h"

// One of the eight trilinear corners around a shading point.
struct LocalGIProbeCorner {
	int index = -1;
	float trilinear_weight = 0.0f;
	float normal_weight = 0.0f;
	float visibility_weight = 0.0f;
	float weight = 0.0f;
};

// 8-probe interpolated indirect irradiance after visibility.
struct LocalGIShadingSample {
	Color irradiance;
	float weight_sum = 0.0f;
	float visibility_mean = 0.0f;
	LocalGIProbeCorner corners[8];
	int corner_count = 0;
	bool finite = true;
};

// CPU 8-probe trilinear * normal * Chebyshev visibility sampling.
class LocalGIProbeSampler {
public:
	static float chebyshev_visibility(float p_distance, float p_mean, float p_second_moment, float p_bias);
	static LocalGIShadingSample interpolate(
			const LocalGIProbeGrid &p_grid,
			const Vector<Color> &p_irradiances,
			const Vector<float> &p_distance_mean,
			const Vector<float> &p_distance_second_moment,
			const Vector3 &p_position,
			const Vector3 &p_normal,
			float p_visibility_bias);
};
