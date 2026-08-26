/**************************************************************************/
/*  test_local_gi_probe_classification.cpp                                */
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

TEST_FORCE_LINK(test_local_gi_probe_classification)

#ifndef _3D_DISABLED

#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_probe_classification.h"
#include "scene/3d/local_gi/local_gi_probe_sample.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

namespace TestLocalGIProbeClassification {

static Vector<LocalGITriangle> make_box_triangles(const Vector3 &p_size, const Transform3D &p_xform = Transform3D()) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	Vector<LocalGITriangle> triangles;
	LocalGIStaticGeometry::extract_mesh_triangles(box, p_xform, AABB(Vector3(-50, -50, -50), Vector3(100, 100, 100)), triangles);
	return triangles;
}

static MeshInstance3D *make_box(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size, GeometryInstance3D::GIMode p_mode, const Color &p_albedo = Color(0.8, 0.8, 0.8)) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);

	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(p_albedo);

	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_position(p_position);
	instance->set_material_override(material);
	instance->set_gi_mode(p_mode);
	p_parent->add_child(instance);
	return instance;
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

static int find_center_probe(const LocalGIVolume3D &p_volume) {
	int best = 0;
	float best_len = 1e30f;
	for (int i = 0; i < p_volume.get_probe_count(); i++) {
		const float len = p_volume.get_probe_position(i).length_squared();
		if (len < best_len) {
			best_len = len;
			best = i;
		}
	}
	return best;
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Free-space probes stay active and wall-embedded probes become inactive") {
	Vector<Vector3> directions;
	LocalGIProbeGrid::generate_directions(32, directions);

	LocalGIBVH empty;
	CHECK_FALSE(LocalGIProbeClassifier::is_embedded(empty, Vector3(), directions));

	LocalGIBVH box_bvh;
	box_bvh.build(make_box_triangles(Vector3(1, 1, 1)));
	CHECK(box_bvh.get_triangles().size() >= 12);
	CHECK_FALSE(LocalGIProbeClassifier::is_embedded(box_bvh, Vector3(2, 0, 0), directions));
	CHECK(LocalGIProbeClassifier::is_embedded(box_bvh, Vector3(), directions));

	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.0);
	volume->set_rays_per_probe(32);

	make_box(root, Vector3(0, 0, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_STATIC);
	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	CHECK(volume->get_probe_count() == 27);
	volume->classify_probes();

	const int center = find_center_probe(*volume);
	CHECK(volume->get_probe_position(center).is_equal_approx(Vector3()));
	CHECK_FALSE(volume->is_probe_active(center));

	int active = 0;
	int inactive = 0;
	for (int i = 0; i < volume->get_probe_count(); i++) {
		if (volume->is_probe_active(i)) {
			active++;
			CHECK(volume->get_probe_position(i).length() > 0.4);
		} else {
			inactive++;
		}
	}
	CHECK(active > 0);
	CHECK(inactive >= 1);
	CHECK(volume->get_active_probe_count() == active);

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Inactive probes are excluded from shading and remaining weights renormalize") {
	LocalGIProbeGrid grid;
	grid.build(Vector3(3, 3, 3), 1.5, 8);
	CHECK(grid.get_probe_count() == 8);

	Vector<Color> irradiances;
	Vector<float> mean;
	Vector<float> second;
	fill_constant_field(grid, Color(0, 0.5, 1), 100.0f, irradiances, mean, second);

	Vector<uint8_t> active;
	active.resize(8);
	for (int i = 0; i < 8; i++) {
		const bool keep = (i % 2) == 0;
		active.write[i] = keep ? 1 : 0;
		if (!keep) {
			irradiances.write[i] = Color(100, 0, 0);
		}
	}

	const LocalGIShadingSample sample = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, Vector3(0, 0, 0), Vector3(), 0.0f, &active);
	CHECK(sample.finite);
	CHECK(sample.corner_count == 8);

	float active_trilinear = 0.0f;
	int inactive_corners = 0;
	for (int i = 0; i < sample.corner_count; i++) {
		if (!sample.corners[i].active) {
			inactive_corners++;
			CHECK(sample.corners[i].weight == doctest::Approx(0.0f));
			continue;
		}
		active_trilinear += sample.corners[i].trilinear_weight;
		CHECK(sample.corners[i].weight > 0.0f);
	}
	CHECK(inactive_corners == 4);
	CHECK(active_trilinear == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(sample.weight_sum == doctest::Approx(active_trilinear).epsilon(1e-5));
	CHECK(sample.irradiance.r == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(sample.irradiance.g == doctest::Approx(0.5f).epsilon(1e-4));
	CHECK(sample.irradiance.b == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Neighboring inactive probes do not produce NaN or Inf") {
	LocalGIProbeGrid grid;
	grid.build(Vector3(3, 3, 3), 1.5, 8);

	Vector<Color> irradiances;
	Vector<float> mean;
	Vector<float> second;
	fill_constant_field(grid, Color(8, 4, 2), 100.0f, irradiances, mean, second);

	Vector<uint8_t> active;
	active.resize(8);
	for (int i = 0; i < 8; i++) {
		active.write[i] = 0;
	}

	const LocalGIShadingSample sample = LocalGIProbeSampler::interpolate(grid, irradiances, mean, second, Vector3(0, 0, 0), Vector3(0, 1, 0), 0.0f, &active);
	CHECK(sample.finite);
	CHECK(sample.weight_sum == doctest::Approx(0.0f));
	CHECK(sample.irradiance.get_luminance() < 1e-8);
	CHECK(Math::is_finite(sample.irradiance.r));
	CHECK(Math::is_finite(sample.irradiance.g));
	CHECK(Math::is_finite(sample.irradiance.b));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Dynamic geometry can deactivate a probe and restore it after leaving") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.0);
	volume->set_rays_per_probe(32);

	MeshInstance3D *dynamic_box = make_box(root, Vector3(0, 0, 0), Vector3(0.5, 0.5, 0.5), GeometryInstance3D::GI_MODE_DYNAMIC);
	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	volume->classify_probes();

	const int center = find_center_probe(*volume);
	CHECK(volume->get_probe_position(center).is_equal_approx(Vector3()));
	CHECK_FALSE(volume->is_probe_active(center));

	int free_probe = -1;
	for (int i = 0; i < volume->get_probe_count(); i++) {
		if (volume->is_probe_active(i)) {
			free_probe = i;
			break;
		}
	}
	CHECK(free_probe >= 0);

	dynamic_box->set_position(Vector3(8, 0, 0));
	CHECK(volume->is_dynamic_dirty());
	CHECK(volume->update_dynamic());
	volume->classify_probes();
	CHECK(volume->is_probe_active(center));
	CHECK(volume->is_probe_active(free_probe));

	dynamic_box->set_position(Vector3());
	CHECK(volume->update_dynamic());
	volume->classify_probes();
	CHECK_FALSE(volume->is_probe_active(center));
	CHECK(volume->is_probe_active(free_probe));

	OmniLight3D *light = memnew(OmniLight3D);
	light->set_position(Vector3(0, 1, 0));
	light->set_param(Light3D::PARAM_ENERGY, 4.0);
	light->set_param(Light3D::PARAM_RANGE, 8.0);
	root->add_child(light);
	dynamic_box->set_position(Vector3(8, 0, 0));
	volume->update_dynamic();
	CHECK(volume->compute_one_bounce());
	CHECK(volume->probe_irradiance_is_finite());
	const LocalGIShadingSample shading = volume->sample_shading(Vector3(0, 0, 0), Vector3(0, 1, 0));
	CHECK(shading.finite);
	CHECK(Math::is_finite(shading.irradiance.r));
	CHECK(Math::is_finite(shading.weight_sum));

	root->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Adding a static mesh marks the bake dirty until the next bake") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	root->add_child(volume);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.0);
	volume->bake();
	CHECK_FALSE(volume->is_static_dirty());

	make_box(root, Vector3(0, 0, 0), Vector3(0.4, 0.4, 0.4), GeometryInstance3D::GI_MODE_STATIC);
	CHECK(volume->is_static_dirty());
	volume->bake();
	CHECK_FALSE(volume->is_static_dirty());
	CHECK(volume->get_baked_triangle_count() >= 12);

	volume->build_probes();
	volume->classify_probes();
	const int center = find_center_probe(*volume);
	CHECK_FALSE(volume->is_probe_active(center));

	root->queue_free();
}

} // namespace TestLocalGIProbeClassification

#endif // _3D_DISABLED
