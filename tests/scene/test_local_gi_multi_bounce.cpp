/**************************************************************************/
/*  test_local_gi_multi_bounce.cpp                                       */
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

TEST_FORCE_LINK(test_local_gi_multi_bounce)

#ifndef _3D_DISABLED

#include "core/math/math_funcs.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

namespace TestLocalGIMultiBounce {

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

static void make_omni(Node *p_parent, const Vector3 &p_position, float p_energy) {
	OmniLight3D *light = memnew(OmniLight3D);
	light->set_position(p_position);
	light->set_color(Color(1, 1, 1));
	light->set_param(Light3D::PARAM_ENERGY, p_energy);
	light->set_param(Light3D::PARAM_INDIRECT_ENERGY, 1.0);
	light->set_param(Light3D::PARAM_RANGE, 8.0);
	light->set_param(Light3D::PARAM_ATTENUATION, 1.0);
	p_parent->add_child(light);
}

static void prepare_white_room(LocalGIVolume3D *p_volume, const Color &p_albedo) {
	p_volume->set_size(Vector3(3, 3, 3));
	p_volume->set_probe_spacing(1.5);
	p_volume->set_rays_per_probe(16);
	p_volume->set_update_fraction(1.0);
	p_volume->set_temporal_hysteresis(0.0);
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

static float converge_multi_bounce(float p_albedo) {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(p_albedo, p_albedo, p_albedo));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->set_multi_bounce_enabled(true);

	CHECK(volume->compute_one_bounce());
	const float direct = volume->get_mean_probe_irradiance_sample().get_luminance();
	REQUIRE(direct > 1e-6);
	volume->reset_temporal_history();
	for (int i = 0; i < 32; i++) {
		CHECK(volume->compute_one_bounce());
		CHECK(volume->update_temporal());
		CHECK(volume->probe_irradiance_is_finite());
	}

	const float final = volume->get_mean_probe_irradiance().get_luminance();
	CHECK(final > direct);
	CHECK(final < direct * 20.0f);
	for (int i = 0; i < 8; i++) {
		CHECK(volume->compute_one_bounce());
		CHECK(volume->update_temporal());
	}
	CHECK(volume->get_mean_probe_irradiance().get_luminance() == doctest::Approx(final).epsilon(0.02));

	const float persistence = final / direct;
	root->queue_free();
	return persistence;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Multi-bounce persistence grows with albedo and remains bounded") {
	const float persistence_02 = converge_multi_bounce(0.2f);
	const float persistence_05 = converge_multi_bounce(0.5f);
	const float persistence_08 = converge_multi_bounce(0.8f);

	CHECK(persistence_02 > 1.0f);
	CHECK(persistence_02 < persistence_05);
	CHECK(persistence_05 < persistence_08);
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Multi-bounce reads a completed field without same-pass feedback") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.5, 0.5, 0.5));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->set_multi_bounce_enabled(true);

	CHECK(volume->compute_one_bounce());
	volume->reset_temporal_history();
	CHECK(volume->compute_one_bounce());
	CHECK(volume->update_temporal());
	CHECK(volume->compute_one_bounce());
	const Color first_multi_bounce = volume->get_mean_probe_irradiance_sample();
	CHECK(volume->compute_one_bounce());
	const Color second_multi_bounce = volume->get_mean_probe_irradiance_sample();
	CHECK(first_multi_bounce.r == doctest::Approx(second_multi_bounce.r).epsilon(1e-4));
	CHECK(first_multi_bounce.g == doctest::Approx(second_multi_bounce.g).epsilon(1e-4));
	CHECK(first_multi_bounce.b == doctest::Approx(second_multi_bounce.b).epsilon(1e-4));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Inactive probes do not feed multi-bounce") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	volume->set_probe_spacing(1.0);
	make_box(root, Vector3(0, 0, 0), Vector3(0.4, 0.4, 0.4), Color(0.8, 0.8, 0.8));
	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	volume->set_multi_bounce_enabled(true);

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
	for (int i = 0; i < 24; i++) {
		CHECK(volume->compute_one_bounce());
		CHECK(volume->update_temporal());
		CHECK(volume->get_probe_irradiance(center).get_luminance() < 1e-8);
		CHECK(volume->probe_irradiance_is_finite());
	}

	root->queue_free();
}

} // namespace TestLocalGIMultiBounce

#endif // _3D_DISABLED
