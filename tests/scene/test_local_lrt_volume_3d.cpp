/**************************************************************************/
/*  test_local_lrt_volume_3d.cpp                                          */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_lrt_volume_3d)

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/3d/local_lrt_volume_3d.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/renderer_rd/environment/local_lrt.h"
#include "tests/test_utils.h"

namespace TestLocalLRTVolume3D {

TEST_CASE("[LocalLRTVolume3D] Probe grid follows size and requested spacing") {
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(8.5, 5.5, 8.5));
	volume->set_probe_spacing(1.0);

	CHECK(volume->get_resolution() == Vector3i(10, 7, 10));
	CHECK(volume->get_actual_probe_spacing().is_equal_approx(Vector3(8.5 / 9.0, 5.5 / 6.0, 8.5 / 9.0)));
	CHECK(volume->get_probe_position(Vector3i(0, 0, 0)).is_equal_approx(Vector3(-4.25, -2.75, -4.25)));
	CHECK(volume->get_probe_position(Vector3i(9, 6, 9)).is_equal_approx(Vector3(4.25, 2.75, 4.25)));
	CHECK(volume->get_bounds().is_equal_approx(AABB(Vector3(-4.25, -2.75, -4.25), Vector3(8.5, 5.5, 8.5))));

	memdelete(volume);
}

TEST_CASE("[LocalLRTVolume3D] Properties survive scene save and load") {
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_name("LocalLRTVolume3D");
	volume->set_size(Vector3(6.0, 4.0, 2.0));
	volume->set_probe_spacing(0.75);
	volume->set_propagation_iterations(8);
	volume->set_energy(1.5);
	volume->set_edge_blend_distance(0.5);
	volume->set_debug_draw(true);
	volume->set_debug_probe_scale(0.2);

	Ref<PackedScene> packed_scene;
	packed_scene.instantiate();
	REQUIRE(packed_scene->pack(volume) == OK);
	const String path = TestUtils::get_temp_path("local_lrt_volume_3d.tscn");
	REQUIRE(ResourceSaver::save(packed_scene, path) == OK);
	memdelete(volume);

	Error error = OK;
	Ref<PackedScene> loaded_scene = ResourceLoader::load(path, "PackedScene", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	REQUIRE(error == OK);
	REQUIRE(loaded_scene.is_valid());
	Node *loaded_root = loaded_scene->instantiate();
	LocalLRTVolume3D *loaded_volume = Object::cast_to<LocalLRTVolume3D>(loaded_root);
	REQUIRE(loaded_volume != nullptr);
	CHECK(loaded_volume->get_size() == Vector3(6.0, 4.0, 2.0));
	CHECK(loaded_volume->get_probe_spacing() == doctest::Approx(0.75));
	CHECK(loaded_volume->get_resolution() == Vector3i(9, 7, 4));
	CHECK(loaded_volume->get_propagation_iterations() == 8);
	CHECK(loaded_volume->get_energy() == doctest::Approx(1.5));
	CHECK(loaded_volume->get_edge_blend_distance() == doctest::Approx(0.5));
	CHECK(loaded_volume->is_debug_draw_enabled());
	CHECK(loaded_volume->get_debug_probe_scale() == doctest::Approx(0.2));
	memdelete(loaded_root);
}

TEST_CASE("[LocalLRTVolume3D] Renderer storage owns and releases volume RID") {
	RendererRD::LocalLRT storage;
	RID volume = storage.volume_allocate();
	CHECK(volume.is_valid());
	storage.volume_initialize(volume);
	CHECK(storage.owns_volume(volume));

	storage.volume_set_grid(volume, Vector3(8.0, 6.0, 4.0), Vector3i(9, 7, 5));
	CHECK(storage.volume_get_bounds(volume).is_equal_approx(AABB(Vector3(-4.0, -3.0, -2.0), Vector3(8.0, 6.0, 4.0))));

	storage.volume_free(volume);
	CHECK_FALSE(storage.owns_volume(volume));
}

} // namespace TestLocalLRTVolume3D
