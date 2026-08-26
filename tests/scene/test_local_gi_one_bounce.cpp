/**************************************************************************/
/*  test_local_gi_one_bounce.cpp                                          */
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

TEST_FORCE_LINK(test_local_gi_one_bounce)

#ifndef _3D_DISABLED

#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

namespace TestLocalGIOneBounce {

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

static void prepare_white_room(LocalGIVolume3D *p_volume, const Color &p_albedo) {
	p_volume->set_size(Vector3(3, 3, 3));
	p_volume->set_probe_spacing(1.5);
	p_volume->set_rays_per_probe(16);
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

static bool colors_scale_approx(const Color &p_a, const Color &p_b, float p_scale, float p_rel = 0.03f) {
	const float eps = 1e-6f;
	for (int i = 0; i < 3; i++) {
		const float expected = p_a[i] * p_scale;
		const float denom = MAX(Math::abs(expected), eps);
		if (Math::abs(p_b[i] - expected) / denom > p_rel) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Zero light yields zero GI") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));

	CHECK(volume->compute_one_bounce());
	CHECK(volume->get_collected_light_count() == 0);
	CHECK(volume->probe_irradiance_is_finite());
	CHECK(volume->get_mean_probe_irradiance().get_luminance() < 1e-8);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Zero albedo yields zero reflected contribution") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0, 0, 0));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);

	CHECK(volume->compute_one_bounce());
	CHECK(volume->get_collected_light_count() == 1);
	CHECK(volume->probe_irradiance_is_finite());
	CHECK(volume->get_mean_probe_irradiance().get_luminance() < 1e-8);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Light times two scales GI") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	OmniLight3D *light = make_omni(root, Vector3(0, 0.8, 0), 2.0);

	CHECK(volume->compute_one_bounce());
	CHECK(volume->get_collected_light_count() == 1);
	CHECK(volume->get_baked_triangle_count() > 0);
	const Color first = volume->get_mean_probe_irradiance();
	CHECK(first.get_luminance() > 1e-6);
	CHECK(volume->probe_irradiance_is_finite());

	light->set_param(Light3D::PARAM_ENERGY, 4.0);
	CHECK(volume->compute_one_bounce());
	const Color second = volume->get_mean_probe_irradiance();
	CHECK(volume->probe_irradiance_is_finite());
	CHECK(colors_scale_approx(first, second, 2.0f));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Albedo times half scales reflected contribution") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.8, 0.8, 0.8));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);

	CHECK(volume->compute_one_bounce());
	CHECK(volume->get_collected_light_count() == 1);
	const Color first = volume->get_mean_probe_irradiance();
	CHECK(first.get_luminance() > 1e-6);

	for (int i = 0; i < root->get_child_count(); i++) {
		MeshInstance3D *box = Object::cast_to<MeshInstance3D>(root->get_child(i));
		if (box == nullptr) {
			continue;
		}
		Ref<StandardMaterial3D> material = box->get_material_override();
		REQUIRE(material.is_valid());
		material->set_albedo(Color(0.4, 0.4, 0.4));
	}
	volume->bake();
	CHECK(volume->compute_one_bounce());
	const Color second = volume->get_mean_probe_irradiance();
	CHECK(volume->probe_irradiance_is_finite());
	CHECK(colors_scale_approx(first, second, 0.5f));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] One-bounce has no NaN or Inf") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	prepare_white_room(volume, Color(0.5, 0.5, 0.5));
	make_omni(root, Vector3(0, 0.8, 0), 4.0);
	CHECK(volume->compute_one_bounce());
	CHECK(volume->has_one_bounce());
	CHECK(volume->probe_irradiance_is_finite());
	CHECK(volume->get_probe_count() == 8);
	CHECK(volume->get_probe_irradiances().size() == 8);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Red and green walls bleed toward the opposite side") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.5);
	volume->set_rays_per_probe(32);
	root->add_child(volume);

	make_box(root, Vector3(0, -1.45, 0), Vector3(3, 0.1, 3), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(0, 1.45, 0), Vector3(3, 0.1, 3), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(0, 0, -1.45), Vector3(3, 3, 0.1), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(0, 0, 1.45), Vector3(3, 3, 0.1), Color(0.8, 0.8, 0.8));
	make_box(root, Vector3(-1.45, 0, 0), Vector3(0.1, 3, 3), Color(0.8, 0.05, 0.05));
	make_box(root, Vector3(1.45, 0, 0), Vector3(0.1, 3, 3), Color(0.05, 0.8, 0.05));
	make_omni(root, Vector3(0, 0.9, 0), 6.0);

	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	CHECK(volume->compute_one_bounce());
	CHECK(volume->probe_irradiance_is_finite());

	Color left;
	Color right;
	int left_count = 0;
	int right_count = 0;
	const PackedVector3Array positions = volume->get_probe_positions();
	for (int i = 0; i < positions.size(); i++) {
		if (positions[i].x < -0.1) {
			left += volume->get_probe_irradiance(i);
			left_count++;
		} else if (positions[i].x > 0.1) {
			right += volume->get_probe_irradiance(i);
			right_count++;
		}
	}
	REQUIRE(left_count > 0);
	REQUIRE(right_count > 0);
	left *= 1.0f / (float)left_count;
	right *= 1.0f / (float)right_count;
	CHECK(left.r > left.g);
	CHECK(right.g > right.r);

	root->queue_free();
}

} // namespace TestLocalGIOneBounce

#endif // _3D_DISABLED
