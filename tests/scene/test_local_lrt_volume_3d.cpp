/**************************************************************************/
/*  test_local_lrt_volume_3d.cpp                                          */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_local_lrt_volume_3d)

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/local_lrt_volume_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/renderer_rd/environment/local_lrt.h"
#include "tests/test_utils.h"

namespace TestLocalLRTVolume3D {

static Color get_transfer_color(const LocalLRTBuilder::TransferRGB &p_transfer) {
	const Vector4 constant_radiance = LocalLRTMath::encode_constant(1.0);
	return Color(
			MAX(p_transfer.r.xform(constant_radiance).x * LocalLRTMath::SH_Y00, (real_t)0.0),
			MAX(p_transfer.g.xform(constant_radiance).x * LocalLRTMath::SH_Y00, (real_t)0.0),
			MAX(p_transfer.b.xform(constant_radiance).x * LocalLRTMath::SH_Y00, (real_t)0.0));
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

TEST_CASE("[LocalLRTVolume3D] Grid property changes rebuild existing data") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(2.0, 2.0, 2.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);
	volume->rebuild();
	REQUIRE(volume->has_built_data());

	volume->set_probe_spacing(0.5);
	CHECK(volume->get_resolution() == Vector3i(5, 5, 5));
	CHECK(volume->has_built_data());

	volume->set_size(Vector3(3.0, 2.0, 2.0));
	CHECK(volume->get_resolution() == Vector3i(7, 5, 5));
	CHECK(volume->has_built_data());

	memdelete(root);
}

TEST_CASE("[LocalLRTVolume3D] Properties survive scene save and load") {
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_name("LocalLRTVolume3D");
	volume->set_size(Vector3(6.0, 4.0, 2.0));
	volume->set_probe_spacing(0.75);
	volume->set_visibility_iterations(6);
	volume->set_propagation_iterations(8);
	volume->set_energy(1.5);
	volume->set_edge_blend_distance(0.5);
	volume->set_debug_draw(true);
	volume->set_debug_mode(LocalLRTVolume3D::DEBUG_MODE_LOCAL_VISIBILITY);
	volume->set_debug_probe_scale(0.2);
	volume->set_geometry_voxel_size(0.2);

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
	CHECK(loaded_volume->get_visibility_iterations() == 6);
	CHECK(loaded_volume->get_propagation_iterations() == 8);
	CHECK(loaded_volume->get_energy() == doctest::Approx(1.5));
	CHECK(loaded_volume->get_edge_blend_distance() == doctest::Approx(0.5));
	CHECK(loaded_volume->is_debug_draw_enabled());
	CHECK(loaded_volume->get_debug_mode() == LocalLRTVolume3D::DEBUG_MODE_LOCAL_VISIBILITY);
	CHECK(loaded_volume->get_debug_probe_scale() == doctest::Approx(0.2));
	CHECK(loaded_volume->get_geometry_voxel_size() == doctest::Approx(0.2));
	memdelete(loaded_root);
}

TEST_CASE("[LocalLRTVolume3D] Static box builds Color SDF local visibility and colored transfer") {
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
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.8, 0.8, 0.8));
	mesh->set_material(material);
	MeshInstance3D *wall = memnew(MeshInstance3D);
	wall->set_mesh(mesh);
	root->add_child(wall);

	volume->rebuild();
	CHECK(volume->get_built_geometry_count() == 1);
	CHECK(volume->is_probe_inside_solid(Vector3i(2, 2, 2)));
	CHECK(volume->is_probe_occupied(Vector3i(2, 2, 2)));
	CHECK(volume->get_probe_signed_distance(Vector3i(2, 2, 2)) < 0.0);
	CHECK(volume->get_probe_coverage(Vector3i(2, 2, 2)) > 0.0);
	CHECK(volume->get_probe_albedo(Vector3i(2, 2, 2)).is_equal_approx(Color(0.8, 0.1, 0.05)));
	CHECK(volume->get_probe_emission(Vector3i(2, 2, 2)).is_equal_approx(Color(0.4, 0.1, 0.02)));

	const Vector3i adjacent_probe(2, 2, 3);
	CHECK_FALSE(volume->is_probe_inside_solid(adjacent_probe));
	const Color transfer = volume->get_probe_transfer_color(adjacent_probe);
	CHECK(transfer.r > transfer.g);
	CHECK(transfer.g > transfer.b);
	CHECK(transfer.b > 0.0);
	CHECK_FALSE(volume->get_probe_local_visibility(adjacent_probe).is_equal_approx(LocalLRTMath::encode_constant(1.0)));
	CHECK(volume->get_probe_transfer_color(Vector3i(0, 0, 0)).is_equal_approx(Color()));

	LocalLRTBuilder reference(Vector3(4.0, 4.0, 4.0), Vector3i(5, 5, 5));
	const Color albedo(0.8, 0.1, 0.05);
	const Color emission(0.4, 0.1, 0.02);
	reference.add_geometry_source(LocalLRTColorSDF::make_box(Vector3(0.4, 0.4, 0.4), volume->get_geometry_voxel_size(), albedo, emission), Transform3D());
	reference.build_local_data();
	for (int z = 0; z < 5; z++) {
		for (int y = 0; y < 5; y++) {
			for (int x = 0; x < 5; x++) {
				const Vector3i position(x, y, z);
				const LocalLRTBuilder::Probe &expected = reference.get_probe(position);
				CHECK(volume->is_probe_inside_solid(position) == expected.inside_solid);
				CHECK(volume->get_probe_coverage(position) == doctest::Approx(expected.coverage));
				CHECK(volume->get_probe_local_visibility(position).is_equal_approx(expected.local_visibility));
				CHECK(volume->get_probe_transfer_color(position).is_equal_approx(get_transfer_color(expected.local_transfer)));
			}
		}
	}

	memdelete(root);
}

TEST_CASE("[LocalLRTVolume3D] QuadMesh is not collected as Color SDF geometry") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);

	Ref<QuadMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector2(0.8, 0.8));
	MeshInstance3D *quad = memnew(MeshInstance3D);
	quad->set_mesh(mesh);
	root->add_child(quad);

	volume->rebuild();
	CHECK(volume->get_built_geometry_count() == 0);
	CHECK_FALSE(volume->is_probe_inside_solid(Vector3i(2, 2, 2)));
	CHECK(volume->get_probe_local_visibility(Vector3i(2, 2, 2)).is_equal_approx(LocalLRTMath::encode_constant(1.0)));

	memdelete(root);
}

TEST_CASE("[LocalLRTVolume3D] Static material changes rebuild emission data") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);

	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	material->set_emission(Color(0.2, 0.05, 0.01));
	material->set_emission_energy_multiplier(2.0);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.8, 0.8, 0.8));
	mesh->set_material(material);
	MeshInstance3D *emitter = memnew(MeshInstance3D);
	emitter->set_mesh(mesh);
	root->add_child(emitter);

	volume->rebuild();
	const Vector3i center(2, 2, 2);
	CHECK(volume->get_probe_emission(center).is_equal_approx(Color(0.4, 0.1, 0.02)));

	material->set_emission_energy_multiplier(4.0);
	volume->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	CHECK(volume->get_probe_emission(center).is_equal_approx(Color(0.8, 0.2, 0.04)));

	memdelete(root);
}

TEST_CASE("[LocalLRTVolume3D] Collection includes geometry within one probe spacing of the volume") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);

	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.4, 0.4, 0.4));
	MeshInstance3D *near_box = memnew(MeshInstance3D);
	near_box->set_mesh(mesh);
	near_box->set_position(Vector3(2.5, 0.0, 0.0));
	root->add_child(near_box);

	volume->rebuild();
	CHECK(volume->get_built_geometry_count() == 1);

	near_box->set_position(Vector3(4.0, 0.0, 0.0));
	volume->rebuild();
	CHECK(volume->get_built_geometry_count() == 0);

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

TEST_CASE("[LocalLRTVolume3D] Analytic lights update injection without rebuilding geometry") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);

	DirectionalLight3D *directional = memnew(DirectionalLight3D);
	directional->set_rotation(Vector3(0.0, Math::PI / 2.0, 0.0));
	directional->set_color(Color(1.0, 0.0, 0.0));
	root->add_child(directional);

	OmniLight3D *omni = memnew(OmniLight3D);
	omni->set_position(Vector3(1.0, 0.0, 0.0));
	omni->set_color(Color(0.0, 1.0, 0.0));
	omni->set_param(Light3D::PARAM_RANGE, 3.0);
	root->add_child(omni);

	SpotLight3D *spot = memnew(SpotLight3D);
	spot->set_position(Vector3(-2.0, 0.0, 0.0));
	spot->set_rotation(Vector3(0.0, -Math::PI / 2.0, 0.0));
	spot->set_color(Color(0.0, 0.0, 1.0));
	spot->set_param(Light3D::PARAM_RANGE, 5.0);
	spot->set_param(Light3D::PARAM_SPOT_ANGLE, 30.0);
	root->add_child(spot);

	volume->rebuild();
	const Vector3i center(2, 2, 2);
	const Vector4 directional_injection = volume->get_probe_injection(center, 0);
	const Vector4 omni_injection = volume->get_probe_injection(center, 1);
	const Vector4 spot_injection = volume->get_probe_injection(center, 2);
	CHECK(LocalLRTMath::evaluate(directional_injection, Vector3(1.0, 0.0, 0.0)) > LocalLRTMath::evaluate(directional_injection, Vector3(-1.0, 0.0, 0.0)));
	CHECK(omni_injection.length() > 0.0);
	CHECK(spot_injection.length() > 0.0);

	directional->set_param(Light3D::PARAM_ENERGY, 2.0);
	volume->update_light_injection();
	CHECK(volume->get_probe_injection(center, 0).is_equal_approx(directional_injection * 2.0));

	const int geometry_count = volume->get_built_geometry_count();
	omni->set_position(Vector3(-1.0, 0.0, 0.0));
	volume->update_light_injection();
	CHECK(volume->get_built_geometry_count() == geometry_count);
	CHECK_FALSE(volume->get_probe_injection(center, 1).is_equal_approx(omni_injection));

	directional->set_visible(false);
	omni->set_visible(false);
	spot->set_visible(false);
	volume->update_light_injection();
	CHECK(volume->get_probe_injection(center, 0).is_equal_approx(Vector4()));
	CHECK(volume->get_probe_injection(center, 1).is_equal_approx(Vector4()));
	CHECK(volume->get_probe_injection(center, 2).is_equal_approx(Vector4()));

	memdelete(root);
}

TEST_CASE("[LocalLRTVolume3D] Transform updates lights without rebuilding local GI") {
	Node3D *root = memnew(Node3D);
	LocalLRTVolume3D *volume = memnew(LocalLRTVolume3D);
	volume->set_size(Vector3(4.0, 4.0, 4.0));
	volume->set_probe_spacing(1.0);
	root->add_child(volume);

	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.8, 0.8, 0.8));
	MeshInstance3D *cube = memnew(MeshInstance3D);
	cube->set_mesh(mesh);
	root->add_child(cube);

	OmniLight3D *omni = memnew(OmniLight3D);
	omni->set_position(Vector3(1.0, 0.0, 0.0));
	omni->set_param(Light3D::PARAM_RANGE, 3.0);
	root->add_child(omni);

	volume->rebuild();
	const Vector3i sample(1, 2, 2);
	const int geometry_count = volume->get_built_geometry_count();
	const Vector4 local_visibility = volume->get_probe_local_visibility(sample);
	const Color transfer = volume->get_probe_transfer_color(sample);
	const Vector4 injection_before = volume->get_probe_injection(sample, 1);
	CHECK_FALSE(volume->is_probe_occupied(sample));
	CHECK(injection_before.length() > 0.0);

	const Transform3D moved(Basis(Vector3(0.0, 1.0, 0.0), Math::PI / 2.0), Vector3(5.0, 1.0, -2.0));
	volume->set_transform(moved);
	volume->notification(Node3D::NOTIFICATION_TRANSFORM_CHANGED);
	CHECK(volume->get_built_geometry_count() == geometry_count);
	CHECK(volume->is_probe_occupied(Vector3i(2, 2, 2)));
	CHECK(volume->get_probe_local_visibility(sample).is_equal_approx(local_visibility));
	CHECK(volume->get_probe_transfer_color(sample).is_equal_approx(transfer));

	volume->update_light_injection();
	CHECK_FALSE(volume->get_probe_injection(sample, 1).is_equal_approx(injection_before));

	omni->set_transform(moved * Transform3D(Basis(), Vector3(1.0, 0.0, 0.0)));
	volume->update_light_injection();
	CHECK(volume->get_probe_injection(sample, 1).is_equal_approx(injection_before));
	CHECK(volume->get_built_geometry_count() == geometry_count);

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
