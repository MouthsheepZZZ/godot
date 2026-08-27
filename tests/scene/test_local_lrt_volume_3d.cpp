/**************************************************************************/
/*  test_local_lrt_volume_3d.cpp                                          */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_lrt_volume_3d)

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/3d/local_lrt_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/renderer_rd/environment/local_lrt.h"
#include "tests/test_utils.h"

namespace TestLocalLRTVolume3D {

static Color get_transfer_color(const LocalLRTBuilder::TransferRGB &p_transfer) {
	Color color;
	for (int coefficient = 0; coefficient < 4; coefficient++) {
		color.r += p_transfer.r.rows[coefficient][coefficient] * 0.25;
		color.g += p_transfer.g.rows[coefficient][coefficient] * 0.25;
		color.b += p_transfer.b.rows[coefficient][coefficient] * 0.25;
	}
	return color;
}

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

TEST_CASE("[LocalLRTVolume3D] Static wall builds local visibility and colored transfer") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);

	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.8, 0.1, 0.05));
	material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	material->set_emission(Color(0.2, 0.05, 0.01));
	material->set_emission_energy_multiplier(2.0);
	Ref<QuadMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector2(0.8, 0.8));
	mesh->set_material(material);
	MeshInstance3D *wall = memnew(MeshInstance3D);
	wall->set_mesh(mesh);
	root->add_child(wall);

	volume->rebuild();
	CHECK(volume->get_built_geometry_count() == 1);
	CHECK(volume->is_probe_occupied(Vector3i(2, 2, 2)));
	CHECK(volume->get_probe_albedo(Vector3i(2, 2, 2)).is_equal_approx(Color(0.8, 0.1, 0.05)));
	CHECK(volume->get_probe_emission(Vector3i(2, 2, 2)).is_equal_approx(Color(0.4, 0.1, 0.02)));

	const Vector3i adjacent_probe(2, 2, 3);
	const Color transfer = volume->get_probe_transfer_color(adjacent_probe);
	CHECK(transfer.r > transfer.g);
	CHECK(transfer.g > transfer.b);
	CHECK(transfer.b > 0.0);
	CHECK_FALSE(volume->get_probe_local_visibility(adjacent_probe).is_equal_approx(LocalLRTMath::encode_constant(1.0)));
	CHECK(volume->get_probe_transfer_color(Vector3i(0, 0, 0)).is_equal_approx(Color()));

	LocalLRTBuilder reference(Vector3(4.0, 4.0, 4.0), Vector3i(5, 5, 5));
	reference.set_occupancy(Vector3i(2, 2, 2), Color(0.8, 0.1, 0.05), Color(0.4, 0.1, 0.02));
	reference.build_local_data();
	for (int z = 0; z < 5; z++) {
		for (int y = 0; y < 5; y++) {
			for (int x = 0; x < 5; x++) {
				const Vector3i position(x, y, z);
				const LocalLRTBuilder::Probe &expected = reference.get_probe(position);
				CHECK(volume->is_probe_occupied(position) == expected.occupied);
				CHECK(volume->get_probe_local_visibility(position).is_equal_approx(expected.local_visibility));
				CHECK(volume->get_probe_transfer_color(position).is_equal_approx(get_transfer_color(expected.local_transfer)));
			}
		}
	}

	memdelete(root);
}

TEST_CASE("[LocalLRTVolume3D] Static geometry is rasterized in volume local space") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	const Transform3D volume_transform(Basis(Vector3(0.0, 1.0, 0.0), Math::deg_to_rad(90.0)), Vector3(6.0, 2.0, -3.0));
	volume->set_transform(volume_transform);
	root->add_child(volume);

	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.8, 0.8, 0.8));
	MeshInstance3D *cube = memnew(MeshInstance3D);
	cube->set_mesh(mesh);
	cube->set_transform(volume_transform);
	root->add_child(cube);

	volume->rebuild();
	CHECK(volume->get_built_geometry_count() == 1);
	CHECK(volume->is_probe_occupied(Vector3i(2, 2, 2)));
	CHECK_FALSE(volume->is_probe_occupied(Vector3i(0, 0, 0)));

	memdelete(root);
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
