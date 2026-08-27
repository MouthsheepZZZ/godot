/**************************************************************************/
/*  local_lrt_builder.h                                                   */
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
/* SOFTWARE.                                                              */
/**************************************************************************/

#pragma once

#include "core/math/color.h"
#include "core/templates/vector.h"
#include "scene/3d/local_lrt_math.h"

class LocalLRTBuilder {
public:
	struct SH2RGB {
		Vector4 r;
		Vector4 g;
		Vector4 b;
	};

	struct TransferRGB {
		LocalLRTMath::SH2Matrix r;
		LocalLRTMath::SH2Matrix g;
		LocalLRTMath::SH2Matrix b;
	};

	struct Probe {
		bool occupied = false;
		Color albedo;
		Color emission;
		Vector4 local_visibility;
		Vector4 global_visibility;
		TransferRGB local_transfer;
		SH2RGB injection;
		SH2RGB radiance;
		real_t empty_space_transmission = 1.0;
	};

	struct DirectionalLight {
		Vector3 direction_to_light;
		Color color = Color(1, 1, 1);
		real_t energy = 1.0;
		bool enabled = true;
	};

	struct OmniLight {
		Vector3 position;
		Color color = Color(1, 1, 1);
		real_t energy = 1.0;
		real_t range = 1.0;
		bool enabled = true;
	};

	struct SpotLight {
		Vector3 position;
		Vector3 direction;
		Color color = Color(1, 1, 1);
		real_t energy = 1.0;
		real_t range = 1.0;
		real_t angle = Math::PI / 4.0;
		bool enabled = true;
	};

private:
	Vector3 size;
	Vector3i resolution;
	Transform3D transform;
	Vector<Probe> probes;
	Vector<Vector4> visibility_scratch;
	Vector<SH2RGB> radiance_scratch;
	real_t propagation_decay = 0.8;

	bool _is_valid_position(const Vector3i &p_position) const;
	void _get_neighbor_visibility(const Vector3i &p_position, Vector4 *r_visibility) const;
	void _get_neighbor_radiance(const Vector3i &p_position, int p_channel, Vector4 *r_radiance) const;
	void _add_directional_injection(Probe &r_probe, const Vector3 &p_direction, const Color &p_color, real_t p_energy);

public:
	LocalLRTBuilder(const Vector3 &p_size, const Vector3i &p_resolution, const Transform3D &p_transform = Transform3D());

	const Vector3 &get_size() const { return size; }
	const Vector3i &get_resolution() const { return resolution; }
	const Transform3D &get_transform() const { return transform; }
	void set_transform(const Transform3D &p_transform) { transform = p_transform; }

	int get_probe_count() const { return probes.size(); }
	const Probe &get_probe(const Vector3i &p_position) const;
	Probe &get_probe(const Vector3i &p_position);
	Vector3 get_probe_local_position(const Vector3i &p_position) const;
	Vector3 get_probe_world_position(const Vector3i &p_position) const;

	void set_occupancy(const Vector3i &p_position, const Color &p_albedo, const Color &p_emission = Color());
	void clear_occupancy();
	void build_local_data();

	void clear_injection();
	void inject_directional_light(const DirectionalLight &p_light);
	void inject_omni_light(const OmniLight &p_light);
	void inject_spot_light(const SpotLight &p_light);

	void reset_global_visibility();
	void propagate_global_visibility(int p_iterations);
	void reset_radiance();
	void propagate_radiance(int p_iterations);

	void set_propagation_decay(real_t p_decay) { propagation_decay = p_decay; }
	real_t get_propagation_decay() const { return propagation_decay; }
};
