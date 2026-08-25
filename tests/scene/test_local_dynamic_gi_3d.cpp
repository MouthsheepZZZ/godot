/**************************************************************************/
/*  test_local_dynamic_gi_3d.cpp                                          */
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

TEST_FORCE_LINK(test_local_dynamic_gi_3d)

#ifndef _3D_DISABLED

#include "scene/3d/light_3d.h"
#include "scene/3d/local_dynamic_gi_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

namespace TestLocalDynamicGI3D {

static MeshInstance3D *make_box(const Vector3 &p_position, const Vector3 &p_size, GeometryInstance3D::GIMode p_mode) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);

	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_position(p_position);
	instance->set_gi_mode(p_mode);
	return instance;
}

TEST_CASE("[SceneTree][LocalDynamicGI3D] Create delete and property storage") {
	for (int i = 0; i < 3; i++) {
		LocalDynamicGI3D *local_gi = memnew(LocalDynamicGI3D);
		CHECK(local_gi->get_local_dynamic_gi_rid().is_valid());
		CHECK(local_gi->is_enabled());
		CHECK(local_gi->get_extend() == Vector3(0.25, 0.25, 0.25));
		CHECK(local_gi->get_blend_distance() == doctest::Approx(0.5));

		local_gi->set_enabled(false);
		local_gi->set_extend(Vector3(1, 2, 3));
		local_gi->set_blend_distance(1.25);
		CHECK_FALSE(local_gi->is_enabled());
		CHECK(local_gi->get_extend() == Vector3(1, 2, 3));
		CHECK(local_gi->get_blend_distance() == doctest::Approx(1.25));
		memdelete(local_gi);
	}
}

TEST_CASE("[SceneTree][LocalDynamicGI3D] Bounds merge, extend, and GI mode classification") {
	SubViewport *viewport = memnew(SubViewport);
	SceneTree::get_singleton()->get_root()->add_child(viewport);

	LocalDynamicGI3D *local_gi = memnew(LocalDynamicGI3D);
	local_gi->set_extend(Vector3(0.5, 0.5, 0.5));
	viewport->add_child(local_gi);

	MeshInstance3D *static_box = make_box(Vector3(0, 0, 0), Vector3(2, 2, 2), GeometryInstance3D::GI_MODE_STATIC);
	MeshInstance3D *dynamic_box = make_box(Vector3(4, 0, 0), Vector3(2, 2, 2), GeometryInstance3D::GI_MODE_DYNAMIC);
	MeshInstance3D *disabled_box = make_box(Vector3(20, 0, 0), Vector3(2, 2, 2), GeometryInstance3D::GI_MODE_DISABLED);
	OmniLight3D *light = memnew(OmniLight3D);
	light->set_position(Vector3(0, 2, 0));

	local_gi->add_child(static_box);
	local_gi->add_child(dynamic_box);
	local_gi->add_child(disabled_box);
	local_gi->add_child(light);
	local_gi->update_local_data();

	CHECK(local_gi->get_geometry_contributors().size() == 2);
	CHECK(local_gi->get_receive_only_geometry().size() == 1);
	CHECK(local_gi->get_lights().size() == 1);

	const AABB bounds = local_gi->get_local_bounds();
	CHECK(bounds.position.is_equal_approx(Vector3(-1.5, -1.5, -1.5)));
	CHECK(bounds.size.is_equal_approx(Vector3(7, 3, 3)));
	CHECK_FALSE(bounds.has_point(Vector3(20, 0, 0)));

	CHECK(RS::get_singleton()->local_dynamic_gi_get_local_bounds(local_gi->get_local_dynamic_gi_rid()).is_equal_approx(bounds));

	memdelete(viewport);
}

TEST_CASE("[SceneTree][LocalDynamicGI3D] Nested Local nodes are excluded from parent collection") {
	SubViewport *viewport = memnew(SubViewport);
	SceneTree::get_singleton()->get_root()->add_child(viewport);

	LocalDynamicGI3D *parent = memnew(LocalDynamicGI3D);
	viewport->add_child(parent);
	parent->add_child(make_box(Vector3(0, 0, 0), Vector3(2, 2, 2), GeometryInstance3D::GI_MODE_STATIC));

	LocalDynamicGI3D *nested = memnew(LocalDynamicGI3D);
	parent->add_child(nested);
	nested->add_child(make_box(Vector3(8, 0, 0), Vector3(2, 2, 2), GeometryInstance3D::GI_MODE_STATIC));

	parent->update_local_data();
	nested->update_local_data();

	CHECK(parent->get_geometry_contributors().size() == 1);
	CHECK(nested->get_geometry_contributors().size() == 1);
	CHECK_FALSE(parent->get_local_bounds().has_point(Vector3(8, 0, 0)));

	memdelete(viewport);
}

TEST_CASE("[SceneTree][LocalDynamicGI3D] Multiple nodes register independently") {
	SubViewport *viewport = memnew(SubViewport);
	SceneTree::get_singleton()->get_root()->add_child(viewport);

	LocalDynamicGI3D *first = memnew(LocalDynamicGI3D);
	LocalDynamicGI3D *second = memnew(LocalDynamicGI3D);
	viewport->add_child(first);
	viewport->add_child(second);

	const RID scenario = viewport->find_world_3d()->get_scenario();
	const Vector<RID> registered = RS::get_singleton()->local_dynamic_gi_get_registered(scenario);
	CHECK(registered.size() == 2);
	CHECK(first->get_local_dynamic_gi_rid() != second->get_local_dynamic_gi_rid());
	CHECK(registered.has(first->get_local_dynamic_gi_rid()));
	CHECK(registered.has(second->get_local_dynamic_gi_rid()));

	memdelete(first);
	const Vector<RID> after_delete = RS::get_singleton()->local_dynamic_gi_get_registered(scenario);
	CHECK(after_delete.size() == 1);
	CHECK(after_delete.has(second->get_local_dynamic_gi_rid()));

	memdelete(viewport);
}

} // namespace TestLocalDynamicGI3D

#endif // _3D_DISABLED
