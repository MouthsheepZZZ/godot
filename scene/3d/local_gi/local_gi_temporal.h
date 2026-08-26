/**************************************************************************/
/*  local_gi_temporal.h                                                   */
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
/* The above copyright notice shall be included in all copies or          */
/* substantial portions of the Software.                                  */
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
#include "core/templates/vector.h"

// EMA probe-field update: estimate = lerp(previous, sample, 1 - hysteresis).
// Never additive. Inactive probes zero the estimate and skip the current sample.
class LocalGITemporal {
public:
	static float sample_weight(float p_hysteresis);
	static Color blend_color(const Color &p_previous, const Color &p_sample, float p_alpha);
	static int probe_update_count(int p_probe_count, float p_update_fraction);

	// Blends `p_update_count` probes starting at `p_start_index` (wrapping).
	// Returns how many probes were visited. Distance moments are EMA'd per ray.
	static int blend(
			Vector<Color> &r_irradiances,
			Vector<float> &r_distance_mean,
			Vector<float> &r_distance_second_moment,
			const Vector<Color> &p_samples,
			const Vector<float> &p_distance_mean_samples,
			const Vector<float> &p_distance_second_moment_samples,
			const Vector<uint8_t> *p_active,
			int p_probe_count,
			int p_rays_per_probe,
			int p_start_index,
			int p_update_count,
			float p_hysteresis);
};
