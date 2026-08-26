/**************************************************************************/
/*  test_local_gi_temporal.cpp                                            */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_gi_temporal)

#ifndef _3D_DISABLED

#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_temporal.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

namespace TestLocalGITemporal {

static MeshInstance3D *make_box(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size, const Color &p_albedo, GeometryInstance3D::GIMode p_mode = GeometryInstance3D::GI_MODE_STATIC) {
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
	instance->set_gi_mode(p_mode);
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

static void prepare_white_room(LocalGIVolume3D *p_volume, const Color &p_albedo) {
	p_volume->set_size(Vector3(3, 3, 3));
	p_volume->set_probe_spacing(1.5);
	p_volume->set_rays_per_probe(16);
	p_volume->set_update_fraction(1.0);
	Node *parent = p_volume->get_parent();
	make_box(parent, Vector3(0, -1.45, 0), Vector3(3, 0.1, 3), p_albedo);
	make_box(parent, Vector3(0, 1.45, 0), Vector3(3, 0.1, 3), p_albedo);
	make_box(parent, Vector3(0, 0, -1.45), Vector3(3, 3, 0.1), p_albedo);
	make_box(parent, Vector3(0, 0, 1.45), Vector3(3, 3, 0.1), p_albedo);
	make_box(parent, Vector3(-1.45, 0, 0), Vector3(0.1, 3, 3), p_albedo);
	make_box(parent, Vector3(1.45, 0, 0), Vector3(0.1, 3, 3), p_albedo);
	p_volume->bake();
	p_volume->update_dynamic();
	p_volume->build_probes();
}

static bool color_below_or_near(const Color &p_value, const Color &p_cap, float p_rel = 0.02f) {
	const float eps = 1e-6f;
	for (int i = 0; i < 3; i++) {
		const float cap = MAX(p_cap[i], 0.0f);
		if (p_value[i] > cap * (1.0f + p_rel) + eps) {
			return false;
		}
	}
	return true;
}

static bool colors_approx(const Color &p_a, const Color &p_b, float p_rel = 0.03f) {
	const float eps = 1e-6f;
	for (int i = 0; i < 3; i++) {
		const float denom = MAX(MAX(Math::abs(p_a[i]), Math::abs(p_b[i])), eps);
		if (Math::abs(p_a[i] - p_b[i]) / denom > p_rel) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[LocalGITemporal] EMA weight is one minus hysteresis") {
	CHECK(LocalGITemporal::sample_weight(0.0f) == doctest::Approx(1.0f));
	CHECK(LocalGITemporal::sample_weight(1.0f) == doctest::Approx(0.0f));
	CHECK(LocalGITemporal::sample_weight(0.9f) == doctest::Approx(0.1f));
	CHECK(LocalGITemporal::sample_weight(0.5f) == doctest::Approx(0.5f));
	CHECK(LocalGITemporal::probe_update_count(8, 1.0f) == 8);
	CHECK(LocalGITemporal::probe_update_count(8, 0.25f) == 2);
	CHECK(LocalGITemporal::probe_update_count(8, 0.0f) == 0);
	CHECK(LocalGITemporal::probe_update_count(8, 0.5f) == 4);
}

TEST_CASE("[LocalGITemporal] Blend is lerp, never additive") {
	Vector<Color> estimate;
	estimate.push_back(Color(0, 0, 0, 1));
	Vector<Color> samples;
	samples.push_back(Color(2, 4, 6, 1));
	Vector<float> mean;
	mean.push_back(1.0f);
	Vector<float> second;
	second.push_back(1.0f);
	Vector<float> mean_sample;
	mean_sample.push_back(3.0f);
	Vector<float> second_sample;
	second_sample.push_back(9.0f);

	CHECK(LocalGITemporal::blend(estimate, mean, second, samples, mean_sample, second_sample, nullptr, 1, 1, 0, 1, 0.5f) == 1);
	CHECK(estimate[0].r == doctest::Approx(1.0f));
	CHECK(estimate[0].g == doctest::Approx(2.0f));
	CHECK(estimate[0].b == doctest::Approx(3.0f));
	CHECK(mean[0] == doctest::Approx(2.0f));
	CHECK(second[0] == doctest::Approx(5.0f));
	const float variance = second[0] - mean[0] * mean[0];
	CHECK(variance == doctest::Approx(1.0f));

	CHECK(LocalGITemporal::blend(estimate, mean, second, samples, mean_sample, second_sample, nullptr, 1, 1, 0, 1, 0.5f) == 1);
	CHECK(estimate[0].r == doctest::Approx(1.5f));
	CHECK(estimate[0].r < 2.0f);
}

TEST_CASE("[LocalGITemporal] Inactive probes zero the estimate and ignore the sample") {
	Vector<Color> estimate;
	estimate.push_back(Color(1, 1, 1, 1));
	Vector<Color> samples;
	samples.push_back(Color(8, 8, 8, 1));
	Vector<float> mean;
	mean.push_back(1.0f);
	Vector<float> second;
	second.push_back(1.0f);
	Vector<float> mean_sample;
	mean_sample.push_back(4.0f);
	Vector<float> second_sample;
	second_sample.push_back(16.0f);
	Vector<uint8_t> active;
	active.push_back(0);

	CHECK(LocalGITemporal::blend(estimate, mean, second, samples, mean_sample, second_sample, &active, 1, 1, 0, 1, 0.5f) == 1);
	CHECK(estimate[0].r == doctest::Approx(0.0f));
	CHECK(estimate[0].g == doctest::Approx(0.0f));
	CHECK(estimate[0].b == doctest::Approx(0.0f));
	CHECK(mean[0] == doctest::Approx(1.0f));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Constant input converges and does not grow") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->set_temporal_hysteresis(0.5);

	CHECK(volume->compute_one_bounce());
	const Color sample = volume->get_mean_probe_irradiance_sample();
	CHECK(sample.get_luminance() > 1e-6);
	volume->reset_temporal_history();
	CHECK(volume->has_temporal_history());
	CHECK(volume->get_mean_probe_irradiance().get_luminance() < 1e-8);

	Color previous;
	for (int step = 1; step <= 24; step++) {
		CHECK(volume->update_temporal());
		CHECK(volume->probe_irradiance_is_finite());
		const Color estimate = volume->get_mean_probe_irradiance();
		CHECK(color_below_or_near(estimate, sample));
		const float expected = 1.0f - Math::pow(0.5f, (float)step);
		const Color expected_color = Color(sample.r * expected, sample.g * expected, sample.b * expected);
		CHECK(colors_approx(estimate, expected_color, 0.02f));
		if (step > 1) {
			CHECK(estimate.get_luminance() >= previous.get_luminance());
		}
		previous = estimate;
	}
	CHECK(colors_approx(volume->get_mean_probe_irradiance(), sample, 0.0001f));

	const Color converged = volume->get_mean_probe_irradiance();
	for (int i = 0; i < 8; i++) {
		volume->update_temporal();
	}
	CHECK(colors_approx(volume->get_mean_probe_irradiance(), converged, 0.001f));
	CHECK(color_below_or_near(volume->get_mean_probe_irradiance(), sample));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Light on and off follow EMA hysteresis") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	OmniLight3D *light = make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->set_temporal_hysteresis(0.5);

	CHECK(volume->compute_one_bounce());
	const Color sample_on = volume->get_mean_probe_irradiance_sample();
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());
	const Color first_on = volume->get_mean_probe_irradiance();
	CHECK(first_on.get_luminance() > 1e-6);
	CHECK(colors_approx(first_on, sample_on * Color(0.5, 0.5, 0.5, 1), 0.04f));

	for (int i = 0; i < 20; i++) {
		volume->update_temporal();
	}
	CHECK(colors_approx(volume->get_mean_probe_irradiance(), sample_on, 0.002f));

	light->set_param(Light3D::PARAM_ENERGY, 0.0);
	CHECK(volume->compute_one_bounce());
	CHECK(volume->get_mean_probe_irradiance_sample().get_luminance() < 1e-8);
	const Color before_off = volume->get_mean_probe_irradiance();
	CHECK(volume->update_temporal());
	const Color first_off = volume->get_mean_probe_irradiance();
	CHECK(first_off.get_luminance() == doctest::Approx(before_off.get_luminance() * 0.5f).epsilon(1e-5));
	CHECK(first_off.get_luminance() < before_off.get_luminance());

	for (int i = 0; i < 24; i++) {
		volume->update_temporal();
	}
	CHECK(volume->get_mean_probe_irradiance().get_luminance() < 1e-5);
	CHECK(volume->probe_irradiance_is_finite());

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Hysteresis 0 snaps and hysteresis 1 freezes") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);

	CHECK(volume->compute_one_bounce());
	const Color sample = volume->get_mean_probe_irradiance_sample();

	volume->set_temporal_hysteresis(0.0);
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());
	CHECK(colors_approx(volume->get_mean_probe_irradiance(), sample, 0.001f));

	volume->set_temporal_hysteresis(1.0);
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());
	CHECK(volume->get_mean_probe_irradiance().get_luminance() < 1e-8);

	volume->set_temporal_hysteresis(0.9);
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());
	const float slow = volume->get_mean_probe_irradiance().get_luminance();
	volume->set_temporal_hysteresis(0.5);
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());
	const float fast = volume->get_mean_probe_irradiance().get_luminance();
	CHECK(fast > slow);
	CHECK(slow == doctest::Approx(sample.get_luminance() * 0.1).epsilon(1e-4));
	CHECK(fast == doctest::Approx(sample.get_luminance() * 0.5).epsilon(1e-4));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Update fraction round-robins probes") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->set_temporal_hysteresis(0.0);
	volume->set_update_fraction(0.25);

	CHECK(volume->compute_one_bounce());
	CHECK(volume->get_probe_count() == 8);
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());

	int lit = 0;
	int dark = 0;
	for (int i = 0; i < volume->get_probe_count(); i++) {
		if (volume->get_probe_irradiance(i).get_luminance() > 1e-8) {
			lit++;
		} else {
			dark++;
		}
	}
	CHECK(lit == 2);
	CHECK(dark == 6);

	CHECK(volume->update_temporal());
	lit = 0;
	for (int i = 0; i < volume->get_probe_count(); i++) {
		if (volume->get_probe_irradiance(i).get_luminance() > 1e-8) {
			lit++;
		}
	}
	CHECK(lit == 4);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Inactive probe history stays zero") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.0);
	volume->set_rays_per_probe(16);
	volume->set_temporal_hysteresis(0.5);
	make_box(root, Vector3(0, -1.45, 0), Vector3(3, 0.1, 3), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(0, 1.45, 0), Vector3(3, 0.1, 3), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(0, 0, -1.45), Vector3(3, 3, 0.1), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(0, 0, 1.45), Vector3(3, 3, 0.1), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(-1.45, 0, 0), Vector3(0.1, 3, 3), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(1.45, 0, 0), Vector3(0.1, 3, 3), Color(0.8, 0.8, 0.8));
	MeshInstance3D *blocker = make_box(root, Vector3(0, 0, 0), Vector3(0.4, 0.4, 0.4), Color(0.8, 0.8, 0.8));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	CHECK(volume->compute_one_bounce());

	int center = -1;
	const PackedVector3Array positions = volume->get_probe_positions();
	for (int i = 0; i < positions.size(); i++) {
		if (positions[i].length() < 0.05) {
			center = i;
			break;
		}
	}
	REQUIRE(center >= 0);
	CHECK_FALSE(volume->is_probe_active(center));

	volume->reset_temporal_history();
	for (int i = 0; i < 8; i++) {
		volume->update_temporal();
		CHECK(volume->get_probe_irradiance(center).get_luminance() < 1e-8);
	}
	CHECK(volume->probe_irradiance_is_finite());

	blocker->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	blocker->hide();
	volume->bake();
	volume->build_probes();
	CHECK(volume->compute_one_bounce());
	volume->classify_probes();
	CHECK(volume->is_probe_active(center));
	volume->reset_temporal_history();
	CHECK(volume->update_temporal());
	CHECK(volume->get_probe_irradiance(center).get_luminance() > 1e-8);
	CHECK(volume->get_probe_irradiance(center).get_luminance() < volume->get_probe_irradiance_sample(center).get_luminance());

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Instant one-bounce is unchanged without temporal") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	OmniLight3D *light = make_omni(root, Vector3(0, 0.8, 0), 2.0);

	CHECK(volume->compute_one_bounce());
	CHECK_FALSE(volume->has_temporal_history());
	const Color first = volume->get_mean_probe_irradiance();
	CHECK(colors_approx(first, volume->get_mean_probe_irradiance_sample()));

	light->set_param(Light3D::PARAM_ENERGY, 4.0);
	CHECK(volume->compute_one_bounce());
	CHECK_FALSE(volume->has_temporal_history());
	const Color second = volume->get_mean_probe_irradiance();
	CHECK(second.get_luminance() == doctest::Approx(first.get_luminance() * 2.0).epsilon(1e-4));

	root->queue_free();
}

} // namespace TestLocalGITemporal

#endif // _3D_DISABLED
