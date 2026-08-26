/**************************************************************************/
/*  test_local_gi_moving_volume.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
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

TEST_FORCE_LINK(test_local_gi_moving_volume)

#ifndef _3D_DISABLED

#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

namespace TestLocalGIMovingVolume {

static void make_box(Node *p_parent, const Vector3 &p_position, const Vector3 &p_size) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);

	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.7, 0.7, 0.7));

	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_position(p_position);
	instance->set_material_override(material);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	p_parent->add_child(instance);
}

static void make_room(Node *p_parent) {
	make_box(p_parent, Vector3(0, -1.45, 0), Vector3(3, 0.1, 3));
	make_box(p_parent, Vector3(0, 1.45, 0), Vector3(3, 0.1, 3));
	make_box(p_parent, Vector3(0, 0, -1.45), Vector3(3, 3, 0.1));
	make_box(p_parent, Vector3(0, 0, 1.45), Vector3(3, 3, 0.1));
	make_box(p_parent, Vector3(-1.45, 0, 0), Vector3(0.1, 3, 3));
	make_box(p_parent, Vector3(1.45, 0, 0), Vector3(0.1, 3, 3));
	make_box(p_parent, Vector3(), Vector3(0.4, 0.4, 0.4));
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Moving parent preserves local geometry probes classification and history") {
	Node3D *moving_root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(moving_root);

	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(1.0);
	volume->set_rays_per_probe(16);
	moving_root->add_child(volume);
	make_room(moving_root);

	OmniLight3D *light = memnew(OmniLight3D);
	light->set_position(Vector3(0, 0.8, 0));
	light->set_param(Light3D::PARAM_ENERGY, 4.0);
	light->set_param(Light3D::PARAM_INDIRECT_ENERGY, 1.0);
	light->set_param(Light3D::PARAM_RANGE, 8.0);
	moving_root->add_child(light);

	volume->bake();
	volume->update_dynamic();
	volume->build_probes();
	volume->classify_probes();
	REQUIRE(volume->compute_one_bounce());
	REQUIRE(volume->update_temporal());
	REQUIRE(volume->has_temporal_history());

	const int static_rebuild_count = volume->get_static_rebuild_count();
	const int dynamic_rebuild_count = volume->get_dynamic_rebuild_count();
	const PackedVector3Array positions = volume->get_probe_positions();
	const PackedByteArray active_states = volume->get_probe_active_states();
	const PackedColorArray irradiances = volume->get_probe_irradiances();
	REQUIRE(positions.size() == 27);
	REQUIRE(active_states.size() == positions.size());
	REQUIRE(irradiances.size() == positions.size());
	CHECK(volume->get_active_probe_count() < volume->get_probe_count());

	const Transform3D motions[] = {
		Transform3D(Basis(), Vector3(6, -2, 4)),
		Transform3D(Basis::from_euler(Vector3(0.3, 1.1, -0.2)), Vector3(6, -2, 4)),
		Transform3D(Basis(), Vector3(250, 40, -180)),
		Transform3D(Basis::from_euler(Vector3(-0.5, 2.2, 0.4)), Vector3(-220, 35, 190)),
	};

	for (const Transform3D &motion : motions) {
		moving_root->set_transform(motion);
		CHECK_FALSE(volume->is_static_dirty());
		CHECK_FALSE(volume->is_dynamic_dirty());
		CHECK(volume->get_static_rebuild_count() == static_rebuild_count);
		CHECK(volume->get_dynamic_rebuild_count() == dynamic_rebuild_count);
		CHECK(volume->has_temporal_history());

		const PackedVector3Array moved_positions = volume->get_probe_positions();
		const PackedByteArray moved_active_states = volume->get_probe_active_states();
		const PackedColorArray moved_irradiances = volume->get_probe_irradiances();
		REQUIRE(moved_positions.size() == positions.size());
		REQUIRE(moved_active_states.size() == active_states.size());
		REQUIRE(moved_irradiances.size() == irradiances.size());
		for (int i = 0; i < positions.size(); i++) {
			CHECK(moved_positions[i].is_equal_approx(positions[i]));
			CHECK(moved_active_states[i] == active_states[i]);
			CHECK(moved_irradiances[i].is_equal_approx(irradiances[i]));
		}
	}

	CHECK(volume->get_static_rebuild_count() - static_rebuild_count == 0);
	CHECK(volume->probe_irradiance_is_finite());
	REQUIRE(volume->compute_one_bounce());
	REQUIRE(volume->update_temporal());
	CHECK(volume->has_temporal_history());
	CHECK(volume->probe_irradiance_is_finite());

	moving_root->queue_free();
}

} // namespace TestLocalGIMovingVolume

#endif // _3D_DISABLED
