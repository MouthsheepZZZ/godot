/**************************************************************************/
/*  local_lrt_builder.cpp                                                 */
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

#include "local_lrt_builder.h"

using namespace LocalLRTMath;

static Vector4 &get_channel(LocalLRTBuilder::SH2RGB &r_value, int p_channel) {
	if (p_channel == 0) {
		return r_value.r;
	}
	if (p_channel == 1) {
		return r_value.g;
	}
	return r_value.b;
}

static const Vector4 &get_channel(const LocalLRTBuilder::SH2RGB &p_value, int p_channel) {
	if (p_channel == 0) {
		return p_value.r;
	}
	if (p_channel == 1) {
		return p_value.g;
	}
	return p_value.b;
}

static SH2Matrix &get_channel(LocalLRTBuilder::TransferRGB &r_value, int p_channel) {
	if (p_channel == 0) {
		return r_value.r;
	}
	if (p_channel == 1) {
		return r_value.g;
	}
	return r_value.b;
}

static const SH2Matrix &get_channel(const LocalLRTBuilder::TransferRGB &p_value, int p_channel) {
	if (p_channel == 0) {
		return p_value.r;
	}
	if (p_channel == 1) {
		return p_value.g;
	}
	return p_value.b;
}

LocalLRTBuilder::LocalLRTBuilder(const Vector3 &p_size, const Vector3i &p_resolution, const Transform3D &p_transform) :
		size(p_size),
		resolution(p_resolution),
		transform(p_transform) {
	const int probe_count = resolution.x * resolution.y * resolution.z;
	probes.resize(probe_count);
	visibility_scratch.resize(probe_count);
	radiance_scratch.resize(probe_count);
	build_local_data();
}

bool LocalLRTBuilder::_is_valid_position(const Vector3i &p_position) const {
	return p_position.x >= 0 && p_position.y >= 0 && p_position.z >= 0 &&
			p_position.x < resolution.x && p_position.y < resolution.y && p_position.z < resolution.z;
}

const LocalLRTBuilder::Probe &LocalLRTBuilder::get_probe(const Vector3i &p_position) const {
	return probes[probe_index(p_position, resolution)];
}

LocalLRTBuilder::Probe &LocalLRTBuilder::get_probe(const Vector3i &p_position) {
	return probes.write[probe_index(p_position, resolution)];
}

Vector3 LocalLRTBuilder::get_probe_local_position(const Vector3i &p_position) const {
	return grid_to_local(Vector3(p_position), size, resolution);
}

Vector3 LocalLRTBuilder::get_probe_world_position(const Vector3i &p_position) const {
	return local_to_world(get_probe_local_position(p_position), transform);
}

void LocalLRTBuilder::set_occupancy(const Vector3i &p_position, const Color &p_albedo, const Color &p_emission) {
	Probe &probe = get_probe(p_position);
	probe.occupied = true;
	probe.albedo = p_albedo;
	probe.emission = p_emission;
}

void LocalLRTBuilder::clear_occupancy() {
	for (Probe &probe : probes) {
		probe.occupied = false;
		probe.albedo = Color();
		probe.emission = Color();
	}
	build_local_data();
}

void LocalLRTBuilder::build_local_data() {
	const Vector4 fully_visible = encode_constant(1.0);
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		probe.local_visibility = Vector4();
		probe.global_visibility = Vector4();
		probe.local_transfer = TransferRGB();
		probe.empty_space_transmission = 0.0;

		if (probe.occupied) {
			continue;
		}

		const Vector3i position = probe_position(index, resolution);
		for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
			const Vector3i offset = neighbor_offset(neighbor);
			const Vector3i neighbor_position = position + offset;
			const real_t weight = neighbor_weight(offset);
			if (!_is_valid_position(neighbor_position) || !get_probe(neighbor_position).occupied) {
				probe.local_visibility += encode_direction(Vector3(offset), 1.0, Math::TAU * 2.0 * weight);
				probe.empty_space_transmission += weight;
				continue;
			}

			const Color albedo = get_probe(neighbor_position).albedo;
			for (int channel = 0; channel < 3; channel++) {
				SH2Matrix &transfer = get_channel(probe.local_transfer, channel);
				const real_t reflectance = albedo[channel] * weight;
				for (int coefficient = 0; coefficient < 4; coefficient++) {
					transfer.rows[coefficient][coefficient] += reflectance;
				}
			}
		}
		probe.global_visibility = probe.local_visibility;
	}

	clear_injection();
	reset_radiance();

	// Avoid accumulated projection error for the common empty-grid case.
	for (Probe &probe : probes) {
		if (!probe.occupied && Math::is_equal_approx(probe.empty_space_transmission, (real_t)1.0)) {
			probe.local_visibility = fully_visible;
			probe.global_visibility = fully_visible;
		}
	}
}

void LocalLRTBuilder::clear_injection() {
	for (Probe &probe : probes) {
		probe.injection = SH2RGB();
	}

	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		if (probe.occupied) {
			continue;
		}
		const Vector3i position = probe_position(index, resolution);
		for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
			const Vector3i offset = neighbor_offset(neighbor);
			const Vector3i neighbor_position = position + offset;
			if (!_is_valid_position(neighbor_position)) {
				continue;
			}
			const Probe &source = get_probe(neighbor_position);
			if (!source.occupied || source.emission == Color()) {
				continue;
			}
			_add_directional_injection(probe, Vector3(offset), source.emission, neighbor_weight(offset));
		}
	}
}

void LocalLRTBuilder::_add_directional_injection(Probe &r_probe, const Vector3 &p_direction, const Color &p_color, real_t p_energy) {
	const Vector4 encoded = encode_direction(p_direction, p_energy, Math::TAU);
	r_probe.injection.r += encoded * p_color.r;
	r_probe.injection.g += encoded * p_color.g;
	r_probe.injection.b += encoded * p_color.b;
}

void LocalLRTBuilder::inject_directional_light(const DirectionalLight &p_light) {
	if (!p_light.enabled) {
		return;
	}
	const Vector3 local_direction = transform.basis.transposed().xform(p_light.direction_to_light).normalized();
	for (Probe &probe : probes) {
		if (!probe.occupied) {
			_add_directional_injection(probe, local_direction, p_light.color, p_light.energy);
		}
	}
}

void LocalLRTBuilder::inject_omni_light(const OmniLight &p_light) {
	if (!p_light.enabled) {
		return;
	}
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		if (probe.occupied) {
			continue;
		}
		const Vector3 to_light_world = p_light.position - get_probe_world_position(probe_position(index, resolution));
		const real_t distance = to_light_world.length();
		if (distance >= p_light.range) {
			continue;
		}
		const real_t attenuation = Math::pow(1.0 - distance / p_light.range, 2.0);
		const Vector3 direction = transform.basis.transposed().xform(to_light_world).normalized();
		_add_directional_injection(probe, direction, p_light.color, p_light.energy * attenuation);
	}
}

void LocalLRTBuilder::inject_spot_light(const SpotLight &p_light) {
	if (!p_light.enabled) {
		return;
	}
	const Vector3 light_direction = p_light.direction.normalized();
	const real_t cone_limit = Math::cos(p_light.angle);
	for (int index = 0; index < probes.size(); index++) {
		Probe &probe = probes.write[index];
		if (probe.occupied) {
			continue;
		}
		const Vector3 light_to_probe = get_probe_world_position(probe_position(index, resolution)) - p_light.position;
		const real_t distance = light_to_probe.length();
		if (distance >= p_light.range || Math::is_zero_approx(distance)) {
			continue;
		}
		const real_t cone_cosine = light_direction.dot(light_to_probe / distance);
		if (cone_cosine <= cone_limit) {
			continue;
		}
		const real_t range_attenuation = Math::pow(1.0 - distance / p_light.range, 2.0);
		const real_t cone_attenuation = Math::pow((cone_cosine - cone_limit) / (1.0 - cone_limit), 2.0);
		const Vector3 direction = transform.basis.transposed().xform(-light_to_probe).normalized();
		_add_directional_injection(probe, direction, p_light.color, p_light.energy * range_attenuation * cone_attenuation);
	}
}

void LocalLRTBuilder::_get_neighbor_visibility(const Vector3i &p_position, Vector4 *r_visibility) const {
	const Vector4 fully_visible = encode_constant(1.0);
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i neighbor_position = p_position + neighbor_offset(neighbor);
		r_visibility[neighbor] = _is_valid_position(neighbor_position) ? get_probe(neighbor_position).global_visibility : fully_visible;
	}
}

void LocalLRTBuilder::reset_global_visibility() {
	for (Probe &probe : probes) {
		probe.global_visibility = probe.local_visibility;
	}
}

void LocalLRTBuilder::propagate_global_visibility(int p_iterations) {
	for (int iteration = 0; iteration < p_iterations; iteration++) {
		for (int index = 0; index < probes.size(); index++) {
			const Probe &probe = probes[index];
			if (probe.occupied) {
				visibility_scratch.write[index] = Vector4();
				continue;
			}
			Vector4 neighbor_visibility[NEIGHBOR_COUNT];
			_get_neighbor_visibility(probe_position(index, resolution), neighbor_visibility);
			visibility_scratch.write[index] = propagate_visibility(probe.local_visibility, neighbor_visibility);
		}
		for (int index = 0; index < probes.size(); index++) {
			probes.write[index].global_visibility = visibility_scratch[index];
		}
	}
}

void LocalLRTBuilder::reset_radiance() {
	for (Probe &probe : probes) {
		probe.radiance = SH2RGB();
	}
}

void LocalLRTBuilder::_get_neighbor_radiance(const Vector3i &p_position, int p_channel, Vector4 *r_radiance) const {
	for (int neighbor = 0; neighbor < NEIGHBOR_COUNT; neighbor++) {
		const Vector3i neighbor_position = p_position + neighbor_offset(neighbor);
		r_radiance[neighbor] = _is_valid_position(neighbor_position) ? get_channel(get_probe(neighbor_position).radiance, p_channel) : Vector4();
	}
}

void LocalLRTBuilder::propagate_radiance(int p_iterations) {
	for (int iteration = 0; iteration < p_iterations; iteration++) {
		for (int index = 0; index < probes.size(); index++) {
			const Probe &probe = probes[index];
			SH2RGB &next = radiance_scratch.write[index];
			next = probe.injection;
			if (probe.occupied) {
				continue;
			}

			const Vector3i position = probe_position(index, resolution);
			Vector4 neighbor_visibility[NEIGHBOR_COUNT];
			_get_neighbor_visibility(position, neighbor_visibility);
			for (int channel = 0; channel < 3; channel++) {
				Vector4 neighbor_radiance[NEIGHBOR_COUNT];
				_get_neighbor_radiance(position, channel, neighbor_radiance);
				get_channel(next, channel) = LocalLRTMath::propagate_radiance(
						probe.local_visibility,
						get_channel(probe.local_transfer, channel),
						get_channel(probe.injection, channel),
						neighbor_radiance,
						neighbor_visibility,
						probe.empty_space_transmission,
						propagation_decay);
			}
		}
		for (int index = 0; index < probes.size(); index++) {
			probes.write[index].radiance = radiance_scratch[index];
		}
	}
}
