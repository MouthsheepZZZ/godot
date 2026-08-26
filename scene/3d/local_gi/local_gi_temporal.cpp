/**************************************************************************/
/*  local_gi_temporal.cpp                                                 */
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

#include "local_gi_temporal.h"

#include "core/math/math_funcs.h"

float LocalGITemporal::sample_weight(float p_hysteresis) {
	return 1.0f - CLAMP(p_hysteresis, 0.0f, 1.0f);
}

Color LocalGITemporal::blend_color(const Color &p_previous, const Color &p_sample, float p_alpha) {
	Color out = p_previous.lerp(p_sample, CLAMP(p_alpha, 0.0f, 1.0f));
	out.a = 1.0f;
	return out;
}

int LocalGITemporal::probe_update_count(int p_probe_count, float p_update_fraction) {
	if (p_probe_count <= 0) {
		return 0;
	}
	const float fraction = CLAMP(p_update_fraction, 0.0f, 1.0f);
	if (fraction <= 0.0f) {
		return 0;
	}
	if (fraction >= 1.0f) {
		return p_probe_count;
	}
	return MAX(1, (int)Math::ceil((float)p_probe_count * fraction));
}

int LocalGITemporal::blend(
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
		float p_hysteresis) {
	if (p_probe_count <= 0 || p_update_count <= 0) {
		return 0;
	}
	if (r_irradiances.size() != p_probe_count || p_samples.size() != p_probe_count) {
		return 0;
	}

	const float alpha = sample_weight(p_hysteresis);
	const int visit = MIN(p_update_count, p_probe_count);
	const int start = ((p_start_index % p_probe_count) + p_probe_count) % p_probe_count;
	int updated = 0;
	for (int i = 0; i < visit; i++) {
		const int probe = (start + i) % p_probe_count;
		const bool active = p_active == nullptr || p_active->is_empty() || probe >= p_active->size() || (*p_active)[probe] != 0;
		if (!active) {
			r_irradiances.write[probe] = Color(0, 0, 0, 1);
			updated++;
			continue;
		}

		r_irradiances.write[probe] = blend_color(r_irradiances[probe], p_samples[probe], alpha);

		const int ray_base = probe * p_rays_per_probe;
		for (int r = 0; r < p_rays_per_probe; r++) {
			const int index = ray_base + r;
			if (index >= r_distance_mean.size() || index >= p_distance_mean_samples.size() ||
					index >= r_distance_second_moment.size() || index >= p_distance_second_moment_samples.size()) {
				break;
			}
			r_distance_mean.write[index] = Math::lerp(r_distance_mean[index], p_distance_mean_samples[index], alpha);
			r_distance_second_moment.write[index] = Math::lerp(r_distance_second_moment[index], p_distance_second_moment_samples[index], alpha);
		}
		updated++;
	}
	return updated;
}
