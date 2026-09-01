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
#include "scene/3d/local_lrt_color_sdf.h"
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
		bool inside_solid = false;
		real_t coverage = 0.0;
		real_t signed_distance = 1.0e20;
		Color albedo;
		Color emission;
		Color transfer_emission;
		Vector3 surface_normal;
		Vector4 local_visibility;
		Vector4 global_visibility;
		TransferRGB local_transfer;
		SH2RGB mesh_light;
		SH2RGB injection;
		SH2RGB radiance;
		real_t empty_space_transmission = 1.0;
		uint16_t sample_mask = 0;
		real_t material_weight = 0.0;

		real_t occupancy() const { return CLAMP(coverage, (real_t)0.0, (real_t)1.0); }
	};

	struct GeometrySource {
		LocalLRTColorSDF sdf;
		Transform3D volume_to_object;
		AABB surface_bounds;
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
		real_t attenuation = 1.0;
		bool enabled = true;
	};

	struct SpotLight {
		Vector3 position;
		Vector3 direction;
		Color color = Color(1, 1, 1);
		real_t energy = 1.0;
		real_t range = 1.0;
		real_t attenuation = 1.0;
		real_t angle = Math::PI / 4.0;
		real_t angle_attenuation = 1.0;
		bool enabled = true;
	};

	struct AreaLight {
		Vector3 position;
		Vector3 direction = Vector3(0, 0, -1);
		Vector3 width = Vector3(1, 0, 0);
		Vector3 height = Vector3(0, 1, 0);
		Color color = Color(1, 1, 1);
		real_t energy = 1.0;
		real_t range = 1.0;
		real_t attenuation = 2.0;
		bool normalize_energy = true;
		bool enabled = true;
	};

private:
	Vector3 size;
	Vector3i resolution;
	Transform3D transform;
	Vector<Probe> probes;
	Vector<GeometrySource> geometry_sources;
	Vector<Vector4> visibility_scratch;
	Vector<SH2RGB> radiance_scratch;
	real_t propagation_decay = 1.0;

	bool _is_valid_position(const Vector3i &p_position) const;
	void _sync_occupancy(Probe &r_probe) const;
	void _add_surface(const Vector3i &p_position, uint16_t p_sample_mask, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission, const Vector3 &p_normal);
	void _accumulate_direction_sample(Probe &r_probe, const Vector3i &p_offset, real_t p_coverage, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission);
	LocalLRTColorSDF::Sample _sample_geometry_source(const GeometrySource &p_source, const Vector3 &p_volume_local) const;
	LocalLRTColorSDF::Sample _sample_geometry(const Vector3 &p_volume_local) const;
	LocalLRTColorSDF::Sample _sample_geometry_segment(const Vector3 &p_begin, const Vector3 &p_end) const;
	void _update_geometry_probe_center(const Vector3i &p_position);
	void _build_geometry_probe(const Vector3i &p_position, const Vector3 &p_spacing);
	void _build_from_occupancy_grid();
	void _build_from_geometry_sources();
	void _get_neighbor_local_visibility(const Vector3i &p_position, Vector4 *r_visibility) const;
	void _get_neighbor_global_visibility(const Vector3i &p_position, Vector4 *r_visibility) const;
	void _get_neighbor_radiance(const Vector3i &p_position, int p_channel, Vector4 *r_radiance) const;
	void _add_directional_injection(SH2RGB &r_injection, const Vector3 &p_direction, const Color &p_color, real_t p_energy);

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

	void set_occupancy(const Vector3i &p_position, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());
	void rasterize_triangle(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, const Color &p_albedo, const Color &p_emission = Color(), const Color &p_transfer_emission = Color());
	void add_geometry_source(const LocalLRTColorSDF &p_sdf, const Transform3D &p_object_to_volume);
	void clear_geometry_sources();
	int get_geometry_source_count() const { return geometry_sources.size(); }
	void clear_occupancy();
	void build_local_data();
	void build_local_data_region(const Vector3i &p_begin, const Vector3i &p_end);
	void build_local_data_region_slice(const Vector3i &p_begin, const Vector3i &p_end, int p_offset, int p_probe_count);

	void clear_injection();
	void inject_directional_light(const DirectionalLight &p_light);
	void inject_omni_light(const OmniLight &p_light);
	void inject_spot_light(const SpotLight &p_light);
	void inject_area_light(const AreaLight &p_light);

	void reset_global_visibility();
	void propagate_global_visibility(int p_iterations);
	void reset_radiance();
	void propagate_radiance(int p_iterations);

	void set_propagation_decay(real_t p_decay) { propagation_decay = p_decay; }
	real_t get_propagation_decay() const { return propagation_decay; }
};
