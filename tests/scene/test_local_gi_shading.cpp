/**************************************************************************/
/*  test_local_gi_shading.cpp                                             */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_gi_shading)

#ifndef _3D_DISABLED

#include "core/math/math_defs.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_probe_grid.h"
#include "scene/3d/local_gi/local_gi_probe_sample.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

namespace TestLocalGIShading {

static MeshInstance3D *make_box(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size, const Color &p_albedo) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);

	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(p_albedo);
	material->set_metallic(0.0);
	material->set_roughness(1.0);

	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_position(p_position);
	instance->set_material_override(material);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	p_parent->add_child(instance);
	return instance;
}

static OmniLight3D *make_omni(Node *p_parent, const Vector3 &p_position, float p_energy) {
	OmniLight3D *light = memnew(OmniLight3D);
	light->set_position(p_position);
	light->set_color(Color(1, 1, 1));
	light->set_param(Light3D::PARAM_ENERGY, p_energy);
	light->set_param(Light3D::PARAM_INDIRECT_ENERGY, 1.0);
	light->set_param(Light3D::PARAM_RANGE, 8.0);
	light->set_param(Light3D::PARAM_ATTENUATION, 1.0);
	p_parent->add_child(light);
	return light;
}

static void fill_constant_field(const LocalGIProbeGrid &p_grid, const Color &p_irradiance, float p_mean, Vector<Color> &r_irradiances, Vector<float> &r_mean, Vector<float> &r_second) {
	const int probes = p_grid.get_probe_count();
	const int rays = p_grid.get_rays_per_probe();
	r_irradiances.resize(probes);
	r_mean.resize(probes * rays);
	r_second.resize(probes * rays);
	for (int i = 0; i < probes; i++) {
		r_irradiances.write[i] = p_irradiance;
	}
	for (int i = 0; i < r_mean.size(); i++) {
		r_mean.write[i] = p_mean;
		r_second.write[i] = p_mean * p_mean;
	}
}

static float trilinear_sum(const LocalGIShadingSample &p_sample) {
	float sum = 0.0f;
	for (int i = 0; i < p_sample.corner_count; i++) {
		sum += p_sample.corners[i].trilinear_weight;
	}
	return sum;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Chebyshev visibility is 1 in front and 0 behind a hard depth") {
	CHECK(LocalGIProbeSampler::chebyshev_visibility(0.5f, 1.0f, 1.0f, 0.0f) == doctest::Approx(1.0f));
	CHECK(LocalGIProbeSampler::chebyshev_visibility(1.0f, 1.0f, 1.0f, 0.0f) == doctest::Approx(1.0f));
	CHECK(LocalGIProbeSampler::chebyshev_visibility(2.0f, 1.0f, 1.0f, 0.0f) == doctest::Approx(0.0f));
	CHECK(LocalGIProbeSampler::chebyshev_visibility(2.0f, 1.0f, 1.25f, 0.0f) == doctest::Approx(0.2f));
	CHECK(LocalGIProbeSampler::chebyshev_visibility(Math::NaN, 1.0f, 1.0f, 0.0f) == doctest::Approx(0.0f));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Trilinear weights sum to one and stay non-negative") {
	LocalGIProbeGrid grid;
	grid.build(Vector3(3, 3, 3), 1.5, 8);
	CHECK(grid.get_probe_count() == 8);

	Vector<Color> irradiances;
	Vector<float> mean;
	Vector<float> second;
	fill_constant_field(grid, Color(1, 1, 1), 100.0f, irradiances, mean, second);

	const Vector3 points[4] = {
		Vector3(0, 0, 0),
		Vector3(-0.75, -0.75, -0.75),
		Vector3(0.4, -0.2, 0.1),
		Vector3(10, -8, 6),
	};
	for (int p = 0; p < 4; p++) {
		const LocalGIShadingSample sample = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, points[p], Vector3(0, 1, 0), 0.0f);
		CHECK(sample.corner_count == 8);
		CHECK(sample.finite);
		CHECK(trilinear_sum(sample) == doctest::Approx(1.0f).epsilon(1e-5));
		CHECK(sample.weight_sum >= 0.0f);
		for (int i = 0; i < sample.corner_count; i++) {
			CHECK(sample.corners[i].trilinear_weight >= -1e-6f);
			CHECK(sample.corners[i].normal_weight >= -1e-6f);
			CHECK(sample.corners[i].visibility_weight >= -1e-6f);
			CHECK(sample.corners[i].weight >= -1e-6f);
		}
	}
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Zero visibility suppresses probe contribution") {
	LocalGIProbeGrid grid;
	grid.build(Vector3(3, 3, 3), 1.5, 8);
	Vector<Color> irradiances;
	Vector<float> mean;
	Vector<float> second;
	fill_constant_field(grid, Color(1, 0.5, 0.25), 0.01f, irradiances, mean, second);

	const LocalGIShadingSample blocked = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f);
	CHECK(blocked.finite);
	CHECK(blocked.weight_sum == doctest::Approx(0.0f).epsilon(1e-6));
	CHECK(blocked.irradiance.get_luminance() < 1e-8);

	fill_constant_field(grid, Color(1, 0.5, 0.25), 100.0f, irradiances, mean, second);
	const LocalGIShadingSample open = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f);
	CHECK(open.finite);
	CHECK(open.weight_sum > 1e-4);
	CHECK(open.irradiance.r == doctest::Approx(1.0f).epsilon(1e-5));
	CHECK(open.irradiance.g == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(open.irradiance.b == doctest::Approx(0.25f).epsilon(1e-5));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Shading interpolation is deterministic and finite at the boundary") {
	LocalGIProbeGrid grid;
	grid.build(Vector3(4, 4, 4), 0.5, 16);
	Vector<Color> irradiances;
	Vector<float> mean;
	Vector<float> second;
	fill_constant_field(grid, Color(0.2, 0.4, 0.6), 4.0f, irradiances, mean, second);

	const Vector3 pos(1.1, -0.3, 0.8);
	const Vector3 normal(0, 1, 0);
	const LocalGIShadingSample a = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, pos, normal, 0.02f);
	const LocalGIShadingSample b = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, pos, normal, 0.02f);
	CHECK(a.finite);
	CHECK(b.finite);
	CHECK(a.irradiance.is_equal_approx(b.irradiance));
	CHECK(a.weight_sum == doctest::Approx(b.weight_sum));

	const LocalGIShadingSample outside = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, Vector3(20, -15, 9), Vector3(0, 1, 0), 0.02f);
	CHECK(outside.finite);
	CHECK(Math::is_finite(outside.irradiance.r));
	CHECK(Math::is_finite(outside.irradiance.g));
	CHECK(Math::is_finite(outside.irradiance.b));
	CHECK(trilinear_sum(outside) == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Wall visibility blocks far-side probes") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.5);
	volume->set_rays_per_probe(32);

	const Color albedo(0.8, 0.8, 0.8);
	make_box(root, Vector3(0, -1.45, 0), Vector3(3, 0.1, 3), albedo);
	make_box(root, Vector3(0, 1.45, 0), Vector3(3, 0.1, 3), albedo);
	make_box(root, Vector3(0, 0, -1.45), Vector3(3, 3, 0.1), albedo);
	make_box(root, Vector3(0, 0, 1.45), Vector3(3, 3, 0.1), albedo);
	make_box(root, Vector3(-1.45, 0, 0), Vector3(0.1, 3, 3), albedo);
	make_box(root, Vector3(1.45, 0, 0), Vector3(0.1, 3, 3), albedo);
	make_box(root, Vector3(0, 0, 0), Vector3(0.08, 3, 3), albedo);
	make_omni(root, Vector3(-0.8, 0.8, 0), 6.0);

	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	CHECK(volume->compute_one_bounce());
	CHECK(volume->probe_irradiance_is_finite());

	const LocalGIShadingSample lit = volume->sample_shading(Vector3(-0.7, 0, 0), Vector3(0, 1, 0));
	const LocalGIShadingSample dark = volume->sample_shading(Vector3(0.7, 0, 0), Vector3(0, 1, 0));
	CHECK(lit.finite);
	CHECK(dark.finite);
	CHECK(lit.weight_sum >= 0.0f);
	CHECK(dark.weight_sum >= 0.0f);
	CHECK(trilinear_sum(lit) == doctest::Approx(1.0f).epsilon(1e-5));
	CHECK(trilinear_sum(dark) == doctest::Approx(1.0f).epsilon(1e-5));
	CHECK(lit.irradiance.get_luminance() > dark.irradiance.get_luminance());

	bool saw_blocked_probe = false;
	for (int i = 0; i < dark.corner_count; i++) {
		const int index = dark.corners[i].index;
		if (index < 0) {
			continue;
		}
		if (volume->get_probe_position(index).x < -0.1) {
			saw_blocked_probe = true;
			CHECK(dark.corners[i].visibility_weight == doctest::Approx(0.0f).epsilon(1e-4));
			CHECK(dark.corners[i].weight == doctest::Approx(0.0f).epsilon(1e-4));
		}
	}
	CHECK(saw_blocked_probe);

	const Color radiance = volume->sample_indirect_radiance(Vector3(-0.7, 0, 0), Vector3(0, 1, 0), Color(0.5, 0.5, 0.5));
	const Color expected = lit.irradiance * (0.5f / (float)Math::PI);
	CHECK(radiance.r == doctest::Approx(expected.r).epsilon(1e-5));
	CHECK(radiance.g == doctest::Approx(expected.g).epsilon(1e-5));
	CHECK(radiance.b == doctest::Approx(expected.b).epsilon(1e-5));

	const Color before = volume->sample_indirect_irradiance(Vector3(-0.4, 0.1, 0.2), Vector3(0, 1, 0));
	volume->set_global_transform(Transform3D(Basis::from_euler(Vector3(0.3, 1.2, -0.4)), Vector3(2, -1, 3)));
	const Color after = volume->sample_indirect_irradiance(Vector3(-0.4, 0.1, 0.2), Vector3(0, 1, 0));
	CHECK(before.is_equal_approx(after));

	root->queue_free();
}

} // namespace TestLocalGIShading

#endif // _3D_DISABLED
