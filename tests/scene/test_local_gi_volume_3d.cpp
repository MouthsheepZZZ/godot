/**************************************************************************/
/*  test_local_gi_volume_3d.cpp                                           */
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

TEST_FORCE_LINK(test_local_gi_volume_3d)

#ifndef _3D_DISABLED

#include "core/object/class_db.h"
#include "scene/3d/local_gi/local_gi_volume_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

namespace TestLocalGIVolume3D {

TEST_CASE("[SceneTree][LocalGIVolume3D] Skeleton defaults and AABB") {
	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	SceneTree::get_singleton()->get_root()->add_child(volume);

	CHECK(volume->get_size() == Vector3(4, 4, 4));
	CHECK(volume->get_probe_spacing() == doctest::Approx(0.5));
	CHECK(volume->get_rays_per_probe() == 64);
	CHECK(volume->get_update_fraction() == doctest::Approx(1.0));
	CHECK(volume->get_temporal_hysteresis() == doctest::Approx(0.9));
	CHECK(volume->is_multi_bounce_enabled() == false);
	CHECK(volume->get_debug_mode() == LocalGIVolume3D::DEBUG_DISABLED);
	CHECK(volume->get_aabb() == AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4)));

	volume->set_size(Vector3(6, 2, 8));
	CHECK(volume->get_size() == Vector3(6, 2, 8));
	CHECK(volume->get_aabb() == AABB(Vector3(-3, -1, -4), Vector3(6, 2, 8)));

	volume->bake();
	CHECK(ClassDB::class_exists("LocalGIVolume3D"));

	volume->queue_free();
}

TEST_CASE("[SceneTree][LocalGIVolume3D] Packed scene round-trip") {
	LocalGIVolume3D *volume = memnew(LocalGIVolume3D);
	volume->set_name("LocalGIVolume3D");
	volume->set_size(Vector3(3, 3, 3));
	volume->set_probe_spacing(0.25);
	volume->set_rays_per_probe(32);

	Ref<PackedScene> packed;
	packed.instantiate();
	const Error pack_error = packed->pack(volume);
	CHECK(pack_error == OK);
	memdelete(volume);

	Node *instance = packed->instantiate();
	REQUIRE(instance != nullptr);
	LocalGIVolume3D *restored = Object::cast_to<LocalGIVolume3D>(instance);
	REQUIRE(restored != nullptr);
	CHECK(restored->get_size() == Vector3(3, 3, 3));
	CHECK(restored->get_probe_spacing() == doctest::Approx(0.25));
	CHECK(restored->get_rays_per_probe() == 32);
	memdelete(instance);
}

} // namespace TestLocalGIVolume3D

#endif // _3D_DISABLED
