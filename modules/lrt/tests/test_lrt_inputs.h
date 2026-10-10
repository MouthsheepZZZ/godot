#pragma once

#include "../lrt_cache.h"
#include "../lrt_volume_3d.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/marshalls.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "tests/test_macros.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_GRIDMAP_ENABLED
#include "modules/gridmap/grid_map.h"
#include "scene/resources/3d/mesh_library.h"
#endif
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif

#include <set>

// Exercise scheduler costs and shared capture ownership without GPU work or timing assertions.
class LRTVolume3DTestAccess {
public:
	static uint64_t add_memory_fixture(LRTVolume3D *p_volume) {
		auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(2);
		auto skin = std::make_shared<std::vector<lrt::MeshTriangleSkin>>(2);
		uint64_t expected = triangles->capacity() * sizeof(lrt::MeshTriangle) + skin->capacity() * sizeof(lrt::MeshTriangleSkin);
		for (int index = 0; index < 3; index++) {
			auto material = std::make_shared<lrt::MaterialCapture>();
			material->occupied.resize(4 << index);
			expected += material->occupied.capacity();
			LRTVolume3D::MeshCaptureCache capture;
			capture.triangles = index < 2 ? triangles : nullptr;
			capture.particle_skin = skin;
			capture.material = material;
			p_volume->mesh_capture_cache[{ RID(), index }] = capture;
		}
		// A second reference to the entire capture must not count its material twice.
		p_volume->mesh_capture_cache[{ RID(), 3 }] = p_volume->mesh_capture_cache.at({ RID(), 0 });
		return expected;
	}
	static void begin_frame() { LRTVolume3D::_begin_propagation_frame(); }
	static bool take(LRTVolume3D *p_volume) { return p_volume->_take_geometry_budget(); }
	static void finish(LRTVolume3D *p_volume, double p_ms) { p_volume->_finish_geometry_update(p_ms); }
};

namespace TestLRTInputs {

TEST_CASE("[SceneTree][LRT] Capture memory counts independent materials with shared or absent triangles") {
	LRTVolume3D *volume = memnew(LRTVolume3D);
	const uint64_t expected = LRTVolume3DTestAccess::add_memory_fixture(volume);
	CHECK(uint64_t(volume->get_preparation_status()["mesh_capture_active_bytes"]) == expected);
	memdelete(volume);
}

TEST_CASE("[SceneTree][LRT] Geometry CPU budget rotates admission without carrying cold-start stalls") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const StringName setting("rendering/global_illumination/lrt/dynamic_objects/cpu_budget_ms");
	const Variant previous_budget = settings->get(setting);
	settings->set(setting, 1.0);
	Node3D *root = memnew(Node3D);
	LRTVolume3D *first = memnew(LRTVolume3D);
	LRTVolume3D *second = memnew(LRTVolume3D);
	first->set_rebuild_suppressed(true);
	second->set_rebuild_suppressed(true);
	root->add_child(first);
	root->add_child(second);
	SceneTree::get_singleton()->get_root()->add_child(root);

	LRTVolume3DTestAccess::begin_frame();
	CHECK(LRTVolume3DTestAccess::take(first));
	LRTVolume3DTestAccess::finish(first, 1.25);
	CHECK_FALSE(LRTVolume3DTestAccess::take(second));
	CHECK(double(first->get_preparation_status()["geometry_budget_overrun_ms"]) == doctest::Approx(0.25));

	LRTVolume3DTestAccess::begin_frame();
	CHECK_FALSE(LRTVolume3DTestAccess::take(first));
	CHECK(LRTVolume3DTestAccess::take(second));
	LRTVolume3DTestAccess::finish(second, 0.25);

	LRTVolume3DTestAccess::begin_frame();
	CHECK(LRTVolume3DTestAccess::take(first));
	LRTVolume3DTestAccess::finish(first, 0.1);
	CHECK(LRTVolume3DTestAccess::take(second));
	LRTVolume3DTestAccess::finish(second, 0.1);
	CHECK(double(first->get_preparation_status()["global_geometry_frame_work_ms"]) == doctest::Approx(0.2));
	CHECK(int64_t(first->get_preparation_status()["geometry_update_count"]) == 2);
	CHECK(int64_t(second->get_preparation_status()["geometry_update_count"]) == 2);

	second->hide();
	LRTVolume3DTestAccess::begin_frame();
	CHECK(LRTVolume3DTestAccess::take(first));
	LRTVolume3DTestAccess::finish(first, 1000.0);
	LRTVolume3DTestAccess::begin_frame();
	CHECK(LRTVolume3DTestAccess::take(first));

	first->rebuild();
	CHECK(first->is_building());
	CHECK(bool(first->get_preparation_status()["queued"]));
	first->set_rebuild_suppressed(false);
	CHECK(bool(first->get_preparation_status()["queued"]));
	memdelete(root);
	settings->set(setting, previous_budget);
}


static Array triangle_arrays(float p_x) {
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices;
	vertices.push_back(Vector3(p_x, 0, 0));
	vertices.push_back(Vector3(p_x + 1, 0, 0));
	vertices.push_back(Vector3(p_x, 1, 0));
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	return arrays;
}

TEST_CASE("[SceneTree][LRT] Global uniform snapshots track effective values independently") {
	RenderingServer *server = RenderingServer::get_singleton();
	const StringName first("lrt_test_global_first");
	const StringName second("lrt_test_global_second");
	auto snapshot = [&](const StringName &p_name) {
		server->sync(); // Only the test waits for submitted commands to be applied.
		return server->global_shader_parameter_get_state(p_name);
	};
	CHECK(snapshot(first).type == RSE::GLOBAL_VAR_TYPE_MAX);
	server->global_shader_parameter_add(first, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	server->global_shader_parameter_add(second, RSE::GLOBAL_VAR_TYPE_FLOAT, 5.0);
	const auto initial = snapshot(first);
	CHECK(initial.type == RSE::GLOBAL_VAR_TYPE_FLOAT);
	CHECK(double(initial.value) == 1.0);
	CHECK(initial.revision != 0);
	const uint64_t second_revision = snapshot(second).revision;
	server->global_shader_parameter_set(first, 1.0);
	CHECK(snapshot(first).revision == initial.revision);
	server->global_shader_parameter_set_override(first, 2.0);
	const auto overridden = snapshot(first);
	CHECK(double(overridden.value) == 2.0);
	CHECK(overridden.revision > initial.revision);
	server->global_shader_parameter_set(first, 3.0);
	CHECK(snapshot(first).revision == overridden.revision);
	CHECK(double(snapshot(first).value) == 2.0);
	server->global_shader_parameter_set_override(first, 2.0);
	CHECK(snapshot(first).revision == overridden.revision);
	server->global_shader_parameter_set_override(first, Variant());
	const auto restored = snapshot(first);
	CHECK(double(restored.value) == 3.0);
	CHECK(restored.revision > overridden.revision);
	CHECK(snapshot(second).revision == second_revision);
	server->global_shader_parameter_remove(first);
	CHECK(snapshot(first).type == RSE::GLOBAL_VAR_TYPE_MAX);
	server->global_shader_parameter_set_override(first, 9.0);
	CHECK(snapshot(first).type == RSE::GLOBAL_VAR_TYPE_MAX);
	server->global_shader_parameter_add(first, RSE::GLOBAL_VAR_TYPE_FLOAT, 3.0);
	CHECK(snapshot(first).revision > restored.revision);
	server->global_shader_parameter_remove(first);
	server->global_shader_parameter_remove(second);
	server->sync();
}

TEST_CASE("[SceneTree][LRT] Global uniform changes invalidate only contributing material dependencies") {
	RenderingServer *server = RenderingServer::get_singleton();
	const StringName first("lrt_test_dependency_first");
	const StringName second("lrt_test_dependency_second");
	server->global_shader_parameter_add(first, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.25);
	server->global_shader_parameter_add(second, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.5);
	server->sync();
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code("shader_type spatial;\n#define PARAM lrt_test_dependency_first\nglobal uniform highp float PARAM;\nvoid fragment() { ALBEDO = vec3(PARAM); }");
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	auto generation = [&]() {
		server->sync();
		for (int frame = 0; frame < 8; frame++) {
			volume->poll();
		}
		return int64_t(volume->get_preparation_status()["generation"]);
	};
	const int64_t initial = generation();
	server->global_shader_parameter_set(second, 0.75);
	CHECK(generation() == initial);
	server->global_shader_parameter_set(first, 0.75);
	const int64_t changed = generation();
	CHECK(changed > initial);
	server->global_shader_parameter_set(first, 0.75);
	CHECK(generation() == changed);
	server->global_shader_parameter_set_override(first, 1.0);
	const int64_t overridden = generation();
	CHECK(overridden > changed);
	server->global_shader_parameter_set(first, 0.5);
	CHECK(generation() == overridden);
	server->global_shader_parameter_set_override(first, Variant());
	CHECK(generation() > overridden);
	shader->set_code("shader_type spatial; global uniform float lrt_test_dependency_second; void fragment() { ALBEDO = vec3(lrt_test_dependency_second); }");
	const int64_t replaced = generation();
	server->global_shader_parameter_set(first, 0.1);
	CHECK(generation() == replaced);
	server->global_shader_parameter_set(second, 0.2);
	CHECK(generation() > replaced);
	Ref<StandardMaterial3D> opaque;
	opaque.instantiate();
	mesh->set_material(opaque);
	instance->set_material_overlay(material);
	shader->set_code("shader_type spatial; global uniform float lrt_test_dependency_second; void fragment() { ALBEDO = vec3(lrt_test_dependency_second); ALPHA = 0.5; }");
	const int64_t transparent = generation();
	server->global_shader_parameter_set(second, 0.3);
	CHECK(generation() == transparent);
	memdelete(root);
	server->global_shader_parameter_remove(first);
	server->global_shader_parameter_remove(second);
	server->sync();
}

TEST_CASE("[SceneTree][LRT] Global texture snapshots follow content updates through RID overrides") {
	RenderingServer *server = RenderingServer::get_singleton();
	Ref<Image> image = Image::create_empty(4, 4, false, Image::FORMAT_RGBA8);
	image->fill(Color(1, 0, 0));
	Ref<ImageTexture> base = ImageTexture::create_from_image(image);
	Ref<ImageTexture> override_texture = ImageTexture::create_from_image(image);
	Ref<ImageTexture> unrelated = ImageTexture::create_from_image(image);
	const StringName name("lrt_test_global_texture_updates");
	server->global_shader_parameter_add(name, RSE::GLOBAL_VAR_TYPE_SAMPLER2D, base);
	auto snapshot = [&]() {
		server->sync();
		return server->global_shader_parameter_get_state(name);
	};
	const auto initial = snapshot();
	CHECK(initial.texture == base->get_rid());
	image->fill(Color(0, 0, 1));
	base->update(image);
	CHECK(snapshot().revision > initial.revision);
	server->global_shader_parameter_set_override(name, override_texture->get_rid());
	const auto overridden = snapshot();
	CHECK(overridden.texture == override_texture->get_rid());
	base->update(image);
	unrelated->update(image);
	CHECK(snapshot().revision == overridden.revision);
	override_texture->update(image);
	const auto updated = snapshot();
	CHECK(updated.revision > overridden.revision);
	CHECK(updated.texture == overridden.texture);
	override_texture->set_image(image);
	const auto replaced = snapshot();
	CHECK(replaced.revision > updated.revision);
	CHECK(replaced.texture == overridden.texture);
	server->global_shader_parameter_set_override(name, Variant());
	const auto restored = snapshot();
	CHECK(restored.texture == base->get_rid());
	override_texture->update(image);
	CHECK(snapshot().revision == restored.revision);
	base->update(image);
	CHECK(snapshot().revision > restored.revision);
	server->global_shader_parameter_remove(name);
	server->sync();
}

TEST_CASE("[SceneTree][LRT] Layered and volume texture edits notify material dependents") {
	Ref<Image> image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	image->fill(Color(1, 0, 0));
	Vector<Ref<Image>> images;
	images.push_back(image);
	images.push_back(image);
	Array signal_args = { {} };

	Ref<Texture2DArray> layered;
	layered.instantiate();
	SIGNAL_WATCH(layered.ptr(), "changed");
	REQUIRE(layered->create_from_images(images) == OK);
	SIGNAL_CHECK("changed", signal_args);
	layered->update_layer(image, 1);
	SIGNAL_CHECK("changed", signal_args);
	REQUIRE(layered->create_from_images(images) == OK);
	SIGNAL_CHECK("changed", signal_args);
	ERR_PRINT_OFF;
	layered->update_layer(image, 2);
	ERR_PRINT_ON;
	SIGNAL_CHECK_FALSE("changed");
	SIGNAL_UNWATCH(layered.ptr(), "changed");

	Ref<ImageTexture3D> volume;
	volume.instantiate();
	SIGNAL_WATCH(volume.ptr(), "changed");
	REQUIRE(volume->create(Image::FORMAT_RGBA8, 2, 2, 2, false, images) == OK);
	SIGNAL_CHECK("changed", signal_args);
	volume->update(images);
	SIGNAL_CHECK("changed", signal_args);
	REQUIRE(volume->create(Image::FORMAT_RGBA8, 2, 2, 2, false, images) == OK);
	SIGNAL_CHECK("changed", signal_args);
	SIGNAL_UNWATCH(volume.ptr(), "changed");
}

TEST_CASE("[SceneTree][LRT] Texture cache identity uses image content rather than RID") {
	Ref<Image> image = Image::create_empty(4, 4, false, Image::FORMAT_RGBA8);
	image->fill(Color(1, 0, 0));
	Ref<ImageTexture> first = ImageTexture::create_from_image(image);
	Ref<ImageTexture> same = ImageTexture::create_from_image(image);
	CHECK(first->get_rid() != same->get_rid());
	const uint64_t original = lrt::texture_content_signature(first->get_rid(), RSE::TEXTURE_TYPE_2D);
	CHECK(original != 0);
	CHECK(lrt::texture_content_signature(same->get_rid(), RSE::TEXTURE_TYPE_2D) == original);
	image->set_pixel(3, 3, Color(0, 1, 0));
	Ref<ImageTexture> changed = ImageTexture::create_from_image(image);
	CHECK(lrt::texture_content_signature(changed->get_rid(), RSE::TEXTURE_TYPE_2D) != original);
	image->fill(Color(1, 0, 0));
	image->generate_mipmaps();
	Ref<ImageTexture> mipmapped = ImageTexture::create_from_image(image);
	CHECK(lrt::texture_content_signature(mipmapped->get_rid(), RSE::TEXTURE_TYPE_2D) != original);
	CHECK(lrt::texture_content_signature(RID(), RSE::TEXTURE_TYPE_2D) == 0);
}

TEST_CASE("[SceneTree][LRT] Surface exclusion preserves other contributors") {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays(0));
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays(10));
	Vector<int> surfaces;
	surfaces.push_back(1);
	std::vector<lrt::MeshTriangle> triangles;
	REQUIRE(lrt::mesh_triangles(mesh, triangles, nullptr, &surfaces));
	REQUIRE(triangles.size() == 1);
	CHECK(triangles[0].position[0].x == doctest::Approx(10));
	surfaces.clear();
	CHECK_FALSE(lrt::mesh_triangles(mesh, triangles, nullptr, &surfaces));
	CHECK(triangles.empty());
}

TEST_CASE("[SceneTree][LRT] Blend shape snapshots follow the instance pose") {
	for (Mesh::BlendShapeMode mode : { Mesh::BLEND_SHAPE_MODE_NORMALIZED, Mesh::BLEND_SHAPE_MODE_RELATIVE }) {
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->set_blend_shape_mode(mode);
		mesh->add_blend_shape("Move");
		Array shapes;
		shapes.push_back(triangle_arrays(4));
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays(2), shapes);
		MeshInstance3D *instance = memnew(MeshInstance3D);
		instance->set_mesh(mesh);
		const uint64_t initial_signature = lrt::mesh_deformation_signature(instance);
		instance->set_blend_shape_value(0, 0.5);
		CHECK(lrt::mesh_deformation_signature(instance) != initial_signature);
		std::vector<lrt::MeshTriangle> triangles;
		REQUIRE(lrt::mesh_triangles(mesh, triangles, instance));
		CHECK(triangles[0].position[0].x == doctest::Approx(mode == Mesh::BLEND_SHAPE_MODE_NORMALIZED ? 3 : 4));
		memdelete(instance);
	}
}

TEST_CASE("[SceneTree][LRT] Skin snapshots use final bound bone transforms") {
	Skeleton3D *skeleton = memnew(Skeleton3D);
	skeleton->add_bone("Root");
	SceneTree::get_singleton()->get_root()->add_child(skeleton);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays = triangle_arrays(1);
	PackedInt32Array bones;
	PackedFloat32Array weights;
	bones.resize(12);
	weights.resize(12);
	bones.fill(0);
	weights.fill(0.0f);
	for (int vertex = 0; vertex < 3; vertex++) {
		weights.set(vertex * 4, 1.0f);
	}
	arrays[Mesh::ARRAY_BONES] = bones;
	arrays[Mesh::ARRAY_WEIGHTS] = weights;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<Skin> skin;
	skin.instantiate();
	skin->add_bind(0, Transform3D());
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_skin(skin);
	instance->set_skeleton_path(NodePath(".."));
	skeleton->add_child(instance);
	skeleton->set_bone_pose_position(0, Vector3(2, 0, 0));
	skeleton->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
	std::vector<lrt::MeshTriangle> triangles;
	REQUIRE(lrt::mesh_triangles(mesh, triangles, instance));
	CHECK(triangles[0].position[0].x == doctest::Approx(3));
	const uint64_t signature = lrt::mesh_deformation_signature(instance);
	skeleton->set_bone_pose_position(0, Vector3(4, 0, 0));
	skeleton->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
	CHECK(lrt::mesh_deformation_signature(instance) != signature);
	REQUIRE(lrt::mesh_triangles(mesh, triangles, instance));
	CHECK(triangles[0].position[0].x == doctest::Approx(5));
	memdelete(skeleton);
}

TEST_CASE("[SceneTree][LRT] Render light visibility follows ancestors") {
	Node3D *parent = memnew(Node3D);
	OmniLight3D *light = memnew(OmniLight3D);
	parent->add_child(light);
	SceneTree::get_singleton()->get_root()->add_child(parent);
	CHECK(light->is_visible_for_rendering());
	parent->hide();
	CHECK_FALSE(light->is_visible_for_rendering());
	CHECK(light->is_visible());
	parent->show();
	CHECK(light->is_visible_for_rendering());
	memdelete(parent);
}

TEST_CASE("[SceneTree][LRT] Deformed bounds follow pose and mesh edits") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->set_blend_shape_mode(Mesh::BLEND_SHAPE_MODE_NORMALIZED);
	mesh->add_blend_shape("Move");
	Array shapes;
	shapes.push_back(triangle_arrays(0));
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays(10), shapes);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_DYNAMIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	instance->set_blend_shape_value(0, 1.0);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	mesh->clear_surfaces();
	shapes[0] = triangle_arrays(20);
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays(20), shapes);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	memdelete(root);
}

#ifdef MODULE_CSG_ENABLED
TEST_CASE("[SceneTree][LRT] CSG contributes its final root mesh and honors GI mode") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	CSGBox3D *box = memnew(CSGBox3D);
	box->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	CSGBox3D *operand = memnew(CSGBox3D);
	operand->set_size(Vector3(0.5, 0.5, 0.5));
	operand->set_operation(CSGShape3D::OPERATION_SUBTRACTION);
	operand->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	box->add_child(operand);
	root->add_child(box);
	SceneTree::get_singleton()->get_root()->add_child(root);
	box->update_shape();
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	box->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	box->set_gi_mode(GeometryInstance3D::GI_MODE_DYNAMIC);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	box->set_position(Vector3(20, 0, 0));
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	memdelete(root);
}
#endif

#ifdef MODULE_GRIDMAP_ENABLED
TEST_CASE("[SceneTree][LRT] GridMap exposes live render instances without baking cells") {
	Ref<MeshLibrary> library;
	library.instantiate();
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	library->create_item(0);
	library->set_item_mesh(0, mesh);
	Node3D *root = memnew(Node3D);
	GridMap *grid = memnew(GridMap);
	grid->set_mesh_library(library);
	grid->set_cell_item(Vector3i(0, 0, 0), 0);
	grid->set_cell_item(Vector3i(1, 0, 0), 0);
	root->add_child(grid);
	SceneTree::get_singleton()->get_root()->add_child(root);
	MessageQueue::get_singleton()->flush();
	const Array sources = grid->get_render_meshes();
	REQUIRE(sources.size() == 3);
	CHECK(RID(sources[0]).is_valid());
	const Ref<Mesh> source_mesh = sources[1];
	CHECK(source_mesh == mesh);
	CHECK(RID(sources[2]).is_valid());
	CHECK(grid->get_used_cells().size() == 2);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_instance_sdf_resolution(grid, 32);
	CHECK(volume->get_instance_sdf_resolution(grid) == 32);
	memdelete(volume);
	grid->clear();
	MessageQueue::get_singleton()->flush();
	CHECK(grid->get_render_meshes().is_empty());
	memdelete(root);
}
#endif

static Dictionary instance_capture(int p_count, int p_visible) {
	Dictionary capture;
	PackedByteArray bytes;
	bytes.resize(p_count * 12 * sizeof(float));
	bytes.fill(0);
	for (int instance = 0; instance < p_count; instance++) {
		uint8_t *data = bytes.ptrw() + instance * 12 * sizeof(float);
		encode_float(1, data);
		encode_float(1, data + 5 * sizeof(float));
		encode_float(1, data + 10 * sizeof(float));
		encode_float(float(instance * 3), data + 3 * sizeof(float));
	}
	capture["instance_buffer"] = bytes;
	capture["instance_count"] = p_visible;
	capture["instance_stride"] = 12;
	return capture;
}

TEST_CASE("[LRT] Instanced capture preserves transforms and shares geometry") {
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(1);
	Dictionary capture = instance_capture(3, 2);
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_multimesh_capture(capture, triangles, { 0 }, { { 3, true } }, copies, error));
	REQUIRE(copies->size() == 2);
	CHECK((*copies)[0].triangles.get() == triangles.get());
	CHECK((*copies)[1].triangles.get() == triangles.get());
	CHECK((*copies)[1].transform.origin.x == doctest::Approx(3));
	lrt::PrimitiveTransform parent;
	parent.origin = lrt::Vec3(7, 2, 0);
	parent.basis_x = lrt::Vec3(-2, 0, 0);
	const lrt::PrimitiveTransform combined = parent * (*copies)[1].transform;
	CHECK(combined.origin.x == doctest::Approx(1));
	CHECK(combined.basis_x.x == doctest::Approx(-2));
	capture["instance_count"] = 0;
	REQUIRE(lrt::decode_multimesh_capture(capture, triangles, { 0 }, { { 3, true } }, copies, error));
	CHECK(copies->empty());
	capture["instance_count"] = 4;
	CHECK_FALSE(lrt::decode_multimesh_capture(capture, triangles, { 0 }, { { 3, true } }, copies, error));
	CHECK_FALSE(error.is_empty());
}

TEST_CASE("[LRT] Particle instance snapshots exclude inactive zero transforms") {
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(1);
	Dictionary capture = instance_capture(2, 2);
	PackedByteArray buffer;
	buffer.resize(2 * 20 * sizeof(float));
	buffer.fill(0);
	uint8_t *active = buffer.ptrw() + 20 * sizeof(float);
	encode_float(1, active);
	encode_float(1, active + 5 * sizeof(float));
	encode_float(1, active + 10 * sizeof(float));
	encode_float(8, active + 7 * sizeof(float));
	capture["instance_buffer"] = buffer;
	capture["instance_stride"] = 20;
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_multimesh_capture(capture, triangles, { 0 }, { { 3, true } }, copies, error));
	REQUIRE(copies->size() == 1);
	CHECK((*copies)[0].transform.origin.y == doctest::Approx(8));
}

TEST_CASE("[LRT] Indirect instance counts select surfaces without duplicating shared assets") {
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(2);
	Dictionary capture = instance_capture(3, 3);
	PackedByteArray commands;
	commands.resize(2 * 5 * sizeof(uint32_t));
	commands.fill(0);
	encode_uint32(3, commands.ptrw());
	encode_uint32(3, commands.ptrw() + 4);
	encode_uint32(3, commands.ptrw() + 20);
	encode_uint32(1, commands.ptrw() + 24);
	capture["instance_commands"] = commands;
	capture["instance_command_stride"] = 5;
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_multimesh_capture(capture, triangles, { 0, 1 }, { { 3, true }, { 3, true } }, copies, error));
	REQUIRE(copies->size() == 3);
	CHECK((*copies)[0].triangles.get() == triangles.get());
	CHECK((*copies)[1].triangles->size() == 1);
	CHECK((*copies)[1].triangles.get() == (*copies)[2].triangles.get());
	encode_uint32(4, commands.ptrw() + 4);
	capture["instance_commands"] = commands;
	capture["instance_command_stride"] = 5;
	CHECK_FALSE(lrt::decode_multimesh_capture(capture, triangles, { 0, 1 }, { { 3, true }, { 3, true } }, copies, error));
}

TEST_CASE("[LRT] Non-indexed indirect draws preserve first vertex and first instance") {
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(2);
	(*triangles)[1].position[0].x = 7;
	Dictionary capture = instance_capture(3, 3);
	PackedByteArray commands;
	commands.resize(5 * sizeof(uint32_t));
	commands.fill(0);
	encode_uint32(3, commands.ptrw());
	encode_uint32(1, commands.ptrw() + 4);
	encode_uint32(3, commands.ptrw() + 8);
	encode_uint32(2, commands.ptrw() + 12);
	capture["instance_commands"] = commands;
	capture["instance_command_stride"] = 5;
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_multimesh_capture(capture, triangles, { 0, 0 }, { { 6, false } }, copies, error));
	REQUIRE(copies->size() == 1);
	CHECK((*copies)[0].transform.origin.x == doctest::Approx(6));
	REQUIRE((*copies)[0].triangles->size() == 1);
	CHECK((*copies)[0].triangles->front().position[0].x == doctest::Approx(7));
	capture["instance_command_stride"] = 4;
	CHECK_FALSE(lrt::decode_multimesh_capture(capture, triangles, { 0, 0 }, { { 6, false } }, copies, error));
}

TEST_CASE("[LRT] Two-dimensional instance buffers preserve affine transforms") {
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(1);
	Dictionary capture;
	PackedByteArray buffer;
	buffer.resize(8 * sizeof(float));
	buffer.fill(0);
	encode_float(-2, buffer.ptrw() + sizeof(float));
	encode_float(4, buffer.ptrw() + 3 * sizeof(float));
	encode_float(3, buffer.ptrw() + 4 * sizeof(float));
	encode_float(5, buffer.ptrw() + 7 * sizeof(float));
	capture["instance_buffer"] = buffer;
	capture["instance_count"] = 1;
	capture["instance_stride"] = 8;
	capture["instance_format_2d"] = true;
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_multimesh_capture(capture, triangles, { 0 }, { { 3, false } }, copies, error));
	REQUIRE(copies->size() == 1);
	const lrt::Vec3 point = (*copies)[0].transform.xform(lrt::Vec3(2, 1, 3));
	CHECK(point.x == doctest::Approx(2));
	CHECK(point.y == doctest::Approx(11));
	CHECK(point.z == doctest::Approx(3));
}

TEST_CASE("[LRT] Instanced geometry culling retains transformed copies and trunk halos") {
	Ref<LRTVolume> volume;
	volume.instantiate();
	volume->configure_sized(1, Vector3(), Vector3(8, 8, 8));
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(1);
	(*triangles)[0].position[0] = lrt::Vec3(0, 0, 0);
	(*triangles)[0].position[1] = lrt::Vec3(1, 0, 0);
	(*triangles)[0].position[2] = lrt::Vec3(0, 1, 0);
	auto copies = std::make_shared<std::vector<lrt::MeshCopy>>(3);
	for (lrt::MeshCopy &copy : *copies) {
		copy.triangles = triangles;
	}
	(*copies)[0].transform.origin = lrt::Vec3(-1, 1, 1);
	(*copies)[1].transform.origin = lrt::Vec3(2, 2, 2);
	(*copies)[1].transform.basis_x = lrt::Vec3(0, -2, 0);
	(*copies)[1].transform.basis_y = lrt::Vec3(-3, 0, 0);
	(*copies)[2].transform.origin = lrt::Vec3(100, 100, 100);
	LRTVolume::MeshInstance source;
	source.instanced = true;
	source.triangles = triangles;
	source.copies = copies;
	volume->set_mesh_instances({ source });
	CHECK(int(volume->describe_unchanged_local_field()["mesh_volumes"]) == 2);
	volume->configure_sized(1, Vector3(95, 95, 95), Vector3(8, 8, 8));
	volume->set_mesh_instances({ source });
	CHECK(int(volume->describe_unchanged_local_field()["mesh_volumes"]) == 1);
}

TEST_CASE("[LRT] Instance material sampling uses each copy's capture coordinates") {
	lrt::MeshTriangle triangle;
	triangle.position[0] = lrt::Vec3(0, 0, 0);
	triangle.position[1] = lrt::Vec3(0, 1, 0);
	triangle.position[2] = lrt::Vec3(0, 0, 1);
	const lrt::TriangleMesh mesh = lrt::build_triangle_mesh({ triangle });
	lrt::SdfGeometryField geometry;
	geometry.size[0] = geometry.size[1] = geometry.size[2] = 2;
	geometry.cell = 1;
	lrt::MaterialCapture capture;
	capture.size[0] = 2;
	capture.size[1] = capture.size[2] = 1;
	capture.uvw_basis_x = lrt::Vec3(1, 0, 0);
	capture.uvw_basis_y = capture.uvw_basis_z = lrt::Vec3();
	capture.occupied = { 1, 1 };
	capture.albedo = { 1, 0, 0, 0, 0, 1 };
	const lrt::SdfInstanceField left = lrt::bake_mesh_instance_field(mesh, geometry, &capture);
	lrt::PrimitiveTransform transform;
	transform.origin.x = 1;
	const lrt::SdfInstanceField right = lrt::bake_mesh_instance_field(mesh, geometry, &capture, nullptr, 1, transform);
	REQUIRE(left.albedo.size() == right.albedo.size());
	REQUIRE(!left.albedo.empty());
	CHECK(left.albedo[0] == 255);
	CHECK(left.albedo[2] == 0);
	CHECK(right.albedo[0] == 0);
	CHECK(right.albedo[2] == 255);
}

TEST_CASE("[LRT] GPU particle captures preserve world space and discard inactive particles") {
	Dictionary capture;
	capture["instance_count"] = 2;
	capture["instance_stride"] = 20;
	capture["particle_trail_steps"] = 1;
	capture["particle_local_coords"] = false;
	capture["transform"] = Transform3D(Basis(), Vector3(10, 0, 0));
	PackedByteArray buffer;
	buffer.resize(2 * 20 * sizeof(float));
	buffer.fill(0);
	encode_float(1, buffer.ptrw());
	encode_float(1, buffer.ptrw() + 5 * sizeof(float));
	encode_float(1, buffer.ptrw() + 10 * sizeof(float));
	encode_float(12, buffer.ptrw() + 3 * sizeof(float));
	for (int axis = 0; axis < 3; axis++) {
		encode_float(-Math::INF, buffer.ptrw() + (20 + axis * 4 + 3) * sizeof(float));
	}
	capture["instance_buffer"] = buffer;
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(1);
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_particle_capture(capture, triangles, { 0 }, { { 3, false } }, { {} }, copies, error));
	REQUIRE(copies->size() == 1);
	CHECK((*copies)[0].transform.origin.x == doctest::Approx(2));
	capture["particle_local_coords"] = true;
	REQUIRE(lrt::decode_particle_capture(capture, triangles, { 0 }, { { 3, false } }, { {} }, copies, error));
	CHECK((*copies)[0].transform.origin.x == doctest::Approx(12));
}

TEST_CASE("[LRT] GPU trails use weighted bind pose groups and the captured trail buffer") {
	Dictionary capture;
	capture["instance_count"] = 2;
	capture["instance_stride"] = 20;
	capture["particle_trail_steps"] = 2;
	capture["particle_local_coords"] = false;
	capture["transform"] = Transform3D(Basis(), Vector3(10, 0, 0));
	PackedByteArray buffer;
	buffer.resize(2 * 20 * sizeof(float));
	buffer.fill(0);
	capture["instance_buffer"] = buffer;
	for (int bone = 0; bone < 2; bone++) {
		uint8_t *data = buffer.ptrw() + bone * 20 * sizeof(float);
		encode_float(1, data);
		encode_float(1, data + 5 * sizeof(float));
		encode_float(1, data + 10 * sizeof(float));
		encode_float(float(10 + bone * 10), data + 3 * sizeof(float));
	}
	capture["particle_trail_buffer"] = buffer;
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(1);
	(*triangles)[0].position[0] = lrt::Vec3(1, 0, 0);
	(*triangles)[0].position[1] = lrt::Vec3(2, 0, 0);
	(*triangles)[0].position[2] = lrt::Vec3(1, 1, 0);
	lrt::MeshTriangleSkin skin;
	skin.weights[0][0] = 1;
	skin.bones[1][0] = 1;
	skin.weights[1][0] = 1;
	skin.weights[2][0] = 0.25;
	skin.bones[2][1] = 1;
	skin.weights[2][1] = 0.75;
	std::shared_ptr<const std::vector<lrt::MeshCopy>> copies;
	String error;
	REQUIRE(lrt::decode_particle_capture(capture, triangles, { 0 }, { { 3, false, false, true } }, { skin }, copies, error));
	REQUIRE(copies->size() == 1);
	REQUIRE((*copies)[0].triangles->size() == 1);
	const lrt::MeshTriangle &result = (*(*copies)[0].triangles)[0];
	CHECK(result.position[0].x == doctest::Approx(1));
	CHECK(result.position[1].x == doctest::Approx(12));
	CHECK(result.position[2].x == doctest::Approx(8.5));
	CHECK(result.position[2].y == doctest::Approx(1));
	lrt::MeshTriangleSkin unweighted;
	REQUIRE(lrt::decode_particle_capture(capture, triangles, { 0 }, { { 3, false, false, true } }, { unweighted }, copies, error));
	CHECK(copies->empty());
	skin.bones[1][0] = 2;
	CHECK_FALSE(lrt::decode_particle_capture(capture, triangles, { 0 }, { { 3, false, false, true } }, { skin }, copies, error));
	capture["particle_trail_steps"] = 3;
	CHECK_FALSE(lrt::decode_particle_capture(capture, triangles, { 0 }, { { 3, false, false, true } }, { skin }, copies, error));
}

TEST_CASE("[SceneTree][LRT] Material validation tracks every pass and ignores shader comments") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	Ref<StandardMaterial3D> base;
	base.instantiate();
	mesh->set_material(base);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code("shader_type spatial; // TIME, ALPHA and particle_trails are comments\n/* discard; VIEW_MATRIX */ void fragment() { ALBEDO = vec3(0.5); }");
	Ref<ShaderMaterial> next;
	next.instantiate();
	next->set_shader(shader);
	base->set_next_pass(next);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	shader->set_code("shader_type spatial; void fragment() { ALBEDO = vec3(SCREEN_UV, 0.0); }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	CHECK(String(volume->get_collection_stats()["material_message"]).contains("Next Pass 1"));
	CHECK(volume->get_instance_sdf_status(instance) == "Failed");
	CHECK(volume->get_instance_sdf_message(instance).contains("Next Pass 1"));
	base->set_next_pass(Ref<Material>());
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	instance->set_material_overlay(next);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	shader->set_code("shader_type spatial; void fragment() { ALBEDO = vec3(0.25); }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Contribution culling follows standard material vertex transforms") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_volume_size(Vector3(1, 1, 1));
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Camera3D *camera = memnew(Camera3D);
	camera->set_position(Vector3(0, 0, 3));
	root->add_child(camera);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(2, 0.2, 0.2));
	mesh->set_material(material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	instance->set_position(Vector3(0.75, 0, 0));
	instance->set_scale(Vector3(0.1, 1, 1));
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	camera->make_current();
	auto contributors = [&]() {
		volume->rebuild();
		return int(volume->get_collection_stats()["contributors"]);
	};
	CHECK(contributors() == 0);
	for (BaseMaterial3D::BillboardMode mode : { BaseMaterial3D::BILLBOARD_ENABLED, BaseMaterial3D::BILLBOARD_FIXED_Y, BaseMaterial3D::BILLBOARD_PARTICLES }) {
		material->set_billboard_mode(mode);
		material->set_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, false);
		CHECK(contributors() == 1);
		material->set_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, true);
		CHECK(contributors() == 0);
	}
	material->set_billboard_mode(BaseMaterial3D::BILLBOARD_DISABLED);
	instance->set_scale(Vector3(1, 1, 1));
	instance->set_position(Vector3(1, 0, 0));
	mesh->set_size(Vector3(0.2, 0.2, 0.2));
	material->set_flag(BaseMaterial3D::FLAG_FIXED_SIZE, true);
	CHECK(contributors() == 0);
	camera->set_position(Vector3(0, 0, 10));
	CHECK(contributors() == 1);
	camera->set_orthogonal(2, 0.1, 100);
	CHECK(contributors() == 0);
	camera->set_orthogonal(20, 0.1, 100);
	CHECK(contributors() == 1);
	Ref<StandardMaterial3D> transparent;
	transparent.instantiate();
	transparent->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	mesh->set_material(transparent);
	transparent->set_next_pass(material);
	CHECK(contributors() == 1);
	transparent->set_next_pass(Ref<Material>());
	instance->set_material_overlay(material);
	CHECK(contributors() == 1);
	instance->set_material_overlay(Ref<Material>());
	CHECK(contributors() == 0);
	mesh->set_material(material);
	material->set_flag(BaseMaterial3D::FLAG_FIXED_SIZE, false);
	instance->set_position(Vector3(0.7, 0, 0));
	CHECK(contributors() == 0);
	material->set_grow_enabled(true);
	material->set_grow(0.2);
	CHECK(contributors() == 1);
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Offset billboard geometry rotates around its instance origin") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_volume_size(Vector3(1, 1, 1));
	volume->set_position(Vector3(0, 0, -2));
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Camera3D *camera = memnew(Camera3D);
	camera->set_rotation(Vector3(0, Math::PI / 2, 0));
	root->add_child(camera);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	Array arrays = triangle_arrays(1.9);
	PackedVector3Array vertices;
	vertices.push_back(Vector3(1.9, -0.1, 0));
	vertices.push_back(Vector3(2.1, -0.1, 0));
	vertices.push_back(Vector3(2, 0.1, 0));
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	camera->make_current();
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	material->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Fragment view dependencies follow functions and exclude vertex-only use") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Camera3D *camera = memnew(Camera3D);
	root->add_child(camera);
	Ref<Shader> shader;
	shader.instantiate();
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	camera->make_current();
	auto generation = [&]() {
		for (int frame = 0; frame < 8; frame++) {
			volume->poll();
		}
		return int64_t(volume->get_preparation_status()["generation"]);
	};
	struct Case {
		const char *code;
		bool camera_dependent;
	};
	const Case cases[] = {
		{ "void fragment() { ALBEDO = abs(VERTEX); }", true },
		{ "void fragment() { ALBEDO = abs(NORMAL); }", true },
		{ "void fragment() { ALBEDO = vec3(NORMAL[0]); }", true },
		{ "void fragment() { float a[3] = float[3](NORMAL.x, NORMAL.y, NORMAL.z); ALBEDO = vec3(a[0]); }", true },
		{ "void fragment() { if (VERTEX.z < 0.0) { ALBEDO = vec3(1.0); } }", true },
		{ "vec3 tint(vec3 n) { return n; } void fragment() { ALBEDO = abs(tint(NORMAL)); }", true },
		{ "vec3 tint(float x) { return vec3(x); } vec3 tint(vec3 x) { return x; } void fragment() { ALBEDO = tint(NORMAL); }", true },
		{ "vec3 tint(float x) { return vec3(x); } vec3 tint(vec3 x) { return x; } void fragment() { ALBEDO = tint(1.0); }", false },
		{ "void vertex() { VERTEX += NORMAL * 0.1; } void fragment() { ALBEDO = vec3(1.0); }", false },
		{ "void fragment() { /* NORMAL and VERTEX */ ALBEDO = vec3(1.0); }", false },
	};
	for (const Case &test : cases) {
		CAPTURE(String(test.code));
		shader->set_code(String("shader_type spatial; ") + test.code);
		const int64_t before = generation();
		CHECK(String(volume->get_collection_stats()["material_message"]).is_empty());
		camera->set_position(camera->get_position() + Vector3(1, 0, 0));
		const int64_t after = generation();
		CHECK((after > before) == test.camera_dependent);
	}
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Height mapping refreshes contribution when the camera moves") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Camera3D *camera = memnew(Camera3D);
	root->add_child(camera);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	camera->make_current();
	auto generation = [&]() {
		for (int frame = 0; frame < 8; frame++) {
			volume->poll();
		}
		return int64_t(volume->get_preparation_status()["generation"]);
	};
	auto check_camera_change = [&](bool p_expected) {
		const int64_t before = generation();
		camera->set_position(camera->get_position() + Vector3(1, 0, 0));
		CHECK((generation() > before) == p_expected);
	};
	check_camera_change(false);
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	check_camera_change(true);
	material->set_heightmap_deep_parallax(true);
	check_camera_change(true);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
	check_camera_change(false);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, false);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	check_camera_change(false);
	Ref<StandardMaterial3D> pass;
	pass.instantiate();
	pass->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	material->set_next_pass(pass);
	check_camera_change(true);
	material->set_next_pass(Ref<Material>());
	instance->set_material_overlay(pass);
	check_camera_change(true);
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Material analysis follows preprocessed source and include edits") {
	Ref<ShaderInclude> include;
	include.instantiate();
	include->set_path("res://lrt_material_analysis_test.gdshaderinc");
	include->set_code("void fragment() { ALBEDO = vec3(SCREEN_UV, 0.0); }");
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code("shader_type spatial;\n#include \"res://lrt_material_analysis_test.gdshaderinc\"\n");
	CHECK(shader->get_preprocessed_code().contains("SCREEN_UV"));
	const String processed = shader->get_preprocessed_code();
	shader->get_rid();
	CHECK(shader->get_preprocessed_code() == processed);
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	CHECK_FALSE(String(volume->get_collection_stats()["material_message"]).is_empty());
	include->set_code("void fragment() { ALBEDO = vec3(0.5); }");
	CHECK_FALSE(shader->get_preprocessed_code().contains("SCREEN_UV"));
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	include->set_code("#define WRITE_OPACITY ALPHA = 0.5\nvoid fragment() { WRITE_OPACITY; }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	CHECK(volume->get_instance_sdf_status(instance) == "Receive Only");
	include->set_code("void fragment() {\n#if 0\n ALPHA = 0.5; ALBEDO = vec3(SCREEN_UV, 0.0);\n#endif\n ALBEDO = vec3(0.5);\n}");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	CHECK(String(volume->get_collection_stats()["material_message"]).is_empty());
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Opaque and cutout passes contribute independently of transparent passes") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	Ref<StandardMaterial3D> material;
	material.instantiate();
	mesh->set_material(material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	for (BaseMaterial3D::Transparency mode : { BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR, BaseMaterial3D::TRANSPARENCY_ALPHA_HASH, BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS }) {
		material->set_transparency(mode);
		volume->rebuild();
		CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	}
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	CHECK(String(volume->get_collection_stats()["material_message"]).is_empty());
	CHECK(volume->get_instance_sdf_status(instance) == "Receive Only");
	CHECK(volume->get_instance_sdf_message(instance).contains("Contributing passes: 0. Receive-only passes: 1."));
	Ref<StandardMaterial3D> opaque;
	opaque.instantiate();
	material->set_next_pass(opaque);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	CHECK(volume->get_instance_sdf_message(instance).contains("Contributing passes: 1. Receive-only passes: 1."));
	material->set_next_pass(Ref<Material>());
	instance->set_material_overlay(opaque);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	CHECK(volume->get_instance_sdf_message(instance).contains("Contributing passes: 1. Receive-only passes: 1."));
	mesh->set_material(opaque);
	instance->set_material_overlay(material);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	CHECK(String(volume->get_collection_stats()["material_message"]).is_empty());
	instance->set_material_overlay(Ref<Material>());
	opaque->set_next_pass(material);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	opaque->set_next_pass(Ref<Material>());
	opaque->set_feature(BaseMaterial3D::FEATURE_REFRACTION, true);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	opaque->set_feature(BaseMaterial3D::FEATURE_REFRACTION, false);
	opaque->set_proximity_fade_enabled(true);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	opaque->set_proximity_fade_enabled(false);
	opaque->set_distance_fade(BaseMaterial3D::DISTANCE_FADE_PIXEL_ALPHA);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	opaque->set_distance_fade(BaseMaterial3D::DISTANCE_FADE_PIXEL_DITHER);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	opaque->set_distance_fade(BaseMaterial3D::DISTANCE_FADE_DISABLED);
	Ref<ShaderMaterial> shader_material;
	shader_material.instantiate();
	Ref<Shader> shader;
	shader.instantiate();
	shader_material->set_shader(shader);
	instance->set_material_override(shader_material);
	shader->set_code("shader_type spatial; void fragment() { if (UV.x < 0.5) discard; }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	shader->set_code("shader_type spatial; void fragment() { ALPHA = UV.x; ALPHA_SCISSOR_THRESHOLD = 0.5; }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	shader->set_code("shader_type spatial; void vertex() { VERTEX.y += sin(TIME); } void fragment() { ALBEDO = vec3(abs(sin(TIME))); }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	shader->set_code("shader_type spatial; void vertex() { MODELVIEW_MATRIX = VIEW_MATRIX * mat4(MAIN_CAM_INV_VIEW_MATRIX[0], MAIN_CAM_INV_VIEW_MATRIX[1], MAIN_CAM_INV_VIEW_MATRIX[2], MODEL_MATRIX[3]); } void fragment() { ALBEDO = abs(VIEW); }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	shader->set_code("shader_type spatial; void fragment() { ALPHA = 0.5; }");
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	memdelete(root);
}


#ifdef TOOLS_ENABLED
TEST_CASE("[SceneTree][LRT] Export cache keys track world coordinates but preserve local reuse") {
	const String cache_directory = lrt::asset_cache_directory();
	std::set<String> existing_files;
	std::set<String> exported_files;
	if (DirAccess::dir_exists_absolute(cache_directory)) {
		for (const String &name : DirAccess::get_files_at(cache_directory)) {
			existing_files.insert(cache_directory.path_join(name));
		}
	}
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	volume->set_volume_size(Vector3(2, 2, 2));
	volume->set_spacing(1.0);
	root->add_child(volume);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR, true);
	mesh->set_material(material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	auto exported_volume_path = [&]() {
		const Dictionary report = volume->prepare_export_data();
		CHECK(bool(report.get("ok", false)));
		const PackedStringArray paths = report.get("assets", PackedStringArray());
		for (const String &path : paths) {
			exported_files.insert(path);
		}
		for (const String &path : paths) {
			if (path.get_file().begins_with("volume_")) {
				return path;
			}
		}
		return String();
	};
	const String world_before = exported_volume_path();
	CHECK_FALSE(world_before.is_empty());
	root->set_position(Vector3(0.5, 0, 0));
	CHECK(exported_volume_path() != world_before);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR, false);
	const String local_before = exported_volume_path();
	root->set_position(Vector3(1, 0, 0));
	CHECK(exported_volume_path() == local_before);
	material->set_flag(BaseMaterial3D::FLAG_UV2_USE_TRIPLANAR, true);
	material->set_flag(BaseMaterial3D::FLAG_UV2_USE_WORLD_TRIPLANAR, true);
	const String uv2_before = exported_volume_path();
	root->set_position(Vector3(1.5, 0, 0));
	CHECK(exported_volume_path() != uv2_before);
	memdelete(root);
	for (const String &path : exported_files) {
		if (existing_files.find(path) == existing_files.end()) {
			CHECK(DirAccess::remove_absolute(path) == OK);
		}
	}
}
#endif

TEST_CASE("[LRT] Raster distance transform matches nearest surface distances") {
	const int size[3] = { 7, 6, 5 };
	const int count = size[0] * size[1] * size[2];
	std::vector<uint8_t> surface(count, 0);
	const lrt::Vec3 sites[] = { lrt::Vec3(1, 2, 1), lrt::Vec3(5, 1, 3), lrt::Vec3(3, 4, 2) };
	for (const lrt::Vec3 &site : sites) {
		surface[int(site.x) + size[0] * (int(site.y) + size[1] * int(site.z))] = 255;
	}
	lrt::MaterialCapture material;
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, capture, nullptr, 2));
	CHECK(capture.geometry.surface_voxels == 3);
	CHECK(capture.geometry.open_shell_count == 1);
	for (int z = 0; z < size[2]; z++) {
		for (int y = 0; y < size[1]; y++) {
			for (int x = 0; x < size[0]; x++) {
				double expected = 100.0;
				for (const lrt::Vec3 &site : sites) {
					expected = std::min(expected, lrt::hypot3(x - site.x, y - site.y, z - site.z));
				}
				const int index = x + size[0] * (y + size[1] * z);
				CHECK(std::abs(capture.geometry.distance[index] * capture.geometry.distance_scale - expected) <= capture.geometry.distance_scale);
			}
		}
	}
	lrt::RasterGeometryCapture serial;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, serial));
	CHECK(serial.geometry.distance == capture.geometry.distance);
	CHECK(serial.signature == capture.signature);
}

TEST_CASE("[LRT] Raster cutouts open the interior and fully discarded surfaces are empty") {
	const int size[3] = { 7, 7, 7 };
	std::vector<uint8_t> surface(343, 0);
	for (int z = 1; z <= 5; z++) {
		for (int y = 1; y <= 5; y++) {
			for (int x = 1; x <= 5; x++) {
				if (x == 1 || x == 5 || y == 1 || y == 5 || z == 1 || z == 5) {
					surface[x + 7 * (y + 7 * z)] = 1;
				}
			}
		}
	}
	lrt::RasterGeometryCapture closed;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, closed));
	const int center = 3 + 7 * (3 + 7 * 3);
	CHECK(closed.geometry.distance[center] < 0);
	CHECK(closed.geometry.closed_shell_count == 1);
	surface[3 + 7 * (3 + 7)] = 0;
	lrt::RasterGeometryCapture cutout;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, cutout));
	CHECK(cutout.geometry.distance[center] > 0);
	CHECK(cutout.geometry.closed_shell_count == 0);
	CHECK(cutout.signature != closed.signature);
	std::fill(surface.begin(), surface.end(), 0);
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, cutout));
	CHECK(cutout.geometry.distance.empty());
	CHECK(cutout.material.albedo.empty());
	CHECK(cutout.geometry.surface_voxels == 0);
}

TEST_CASE("[LRT] Raster capture rejects invalid layouts and preserves output on cancellation") {
	const int size[3] = { 4, 4, 4 };
	std::vector<uint8_t> surface(64, 0);
	surface[21] = 1;
	lrt::RasterGeometryCapture capture;
	capture.signature = 123;
	std::atomic<bool> cancel(true);
	CHECK_FALSE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, capture, &cancel));
	CHECK(capture.signature == 123);
	surface.pop_back();
	CHECK_FALSE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, capture));
	CHECK(capture.signature == 123);
	const int invalid_size[3] = { 0, 4, 4 };
	CHECK_FALSE(lrt::bake_raster_geometry_capture({}, invalid_size, {}, {}, capture));
}

TEST_CASE("[LRT] Raster materials follow the nearest captured fragment in local space") {
	const int size[3] = { 8, 8, 8 };
	std::vector<uint8_t> surface(512, 0);
	const int left = 1 + 8 * (1 + 8);
	const int right = 6 + 8 * (6 + 8 * 6);
	surface[left] = 1;
	surface[right] = 1;
	lrt::PrimitiveTransform transform;
	transform.origin = lrt::Vec3(10, 20, 30);
	transform.basis_x = lrt::Vec3(2, 0, 0);
	transform.basis_y = lrt::Vec3(0, 3, 0);
	transform.basis_z = lrt::Vec3(0, 0, 4);
	lrt::MaterialCapture material;
	for (int axis = 0; axis < 3; axis++) {
		material.size[axis] = 8;
	}
	material.uvw_basis_x = lrt::Vec3(1.0 / 14, 0, 0);
	material.uvw_basis_y = lrt::Vec3(0, 1.0 / 21, 0);
	material.uvw_basis_z = lrt::Vec3(0, 0, 1.0 / 28);
	material.uvw_offset = lrt::Vec3(-11.0 / 14, -21.5 / 21, -32.0 / 28);
	material.occupied = surface;
	material.albedo.resize(512 * 3, 0);
	material.emission.resize(512 * 3, 0);
	material.albedo[left * 3] = 1;
	material.albedo[right * 3 + 2] = 1;
	material.emission[left * 3] = 5;
	material.emission[right * 3 + 2] = 3;
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, transform, capture));
	CHECK(capture.material.albedo.front() == 255);
	CHECK(capture.material.albedo[2] == 0);
	CHECK(capture.material.albedo[capture.material.albedo.size() - 3] == 0);
	CHECK(capture.material.albedo.back() == 255);
	CHECK(capture.material.emission.front() == doctest::Approx(5));
	CHECK(capture.material.emission.back() == doctest::Approx(3));
}

TEST_CASE("[LRT] Raster SDF sampling preserves small invertible voxel transforms") {
	const int size[3] = { 8, 8, 8 };
	std::vector<uint8_t> surface(512, 0);
	surface[1 + 8 * (1 + 8)] = 1;
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, capture));
	auto geometry = std::make_shared<lrt::SdfGeometryField>(capture.geometry);
	auto material = std::make_shared<lrt::SdfInstanceField>(capture.material);
	const lrt::ColorSdfSample reference = lrt::sample_sdf_fields(*geometry, *material, lrt::Vec3(2.75, 1.75, 1.75));
	REQUIRE(reference.valid);
	for (double scale : { 1.0, 0.01, 0.001, 0.0001, -0.001 }) {
		lrt::PrimitiveTransform transform;
		transform.basis_x = lrt::Vec3(scale, 0, 0);
		transform.basis_y = lrt::Vec3(0, std::abs(scale), 0);
		transform.basis_z = lrt::Vec3(0, 0, std::abs(scale));
		const lrt::SdfPrimitive primitive = lrt::make_sdf_primitive(geometry, material, transform, 1, 1, 1, 1);
		const lrt::ColorSdfSample sample = primitive.sample(transform.xform(lrt::Vec3(2.75, 1.75, 1.75)));
		CHECK(sample.valid);
		CHECK(sample.distance == doctest::Approx(reference.distance * std::abs(scale)).epsilon(0.001));
		CHECK(sample.normal.x == doctest::Approx(reference.normal.x * (scale > 0 ? 1.0 : -1.0)));
		CHECK(sample.normal.y == doctest::Approx(reference.normal.y));
		CHECK(sample.normal.z == doctest::Approx(reference.normal.z));
	}
	lrt::PrimitiveTransform singular;
	singular.basis_x = lrt::Vec3();
	const lrt::SdfPrimitive primitive = lrt::make_sdf_primitive(geometry, material, singular, 1, 1, 1, 1);
	CHECK_FALSE(primitive.sample(lrt::Vec3()).valid);
}

TEST_CASE("[LRT] Propagation traces unsigned slanted surfaces between free probes") {
	std::vector<lrt::MeshTriangle> triangles(2);
	const lrt::Vec3 vertices[4] = { { -1, 1.05, -1 }, { 1, -0.95, -1 }, { 1, -0.95, 1 }, { -1, 1.05, 1 } };
	for (int face = 0; face < 2; face++) {
		const int indices[3] = { 0, face + 1, face + 2 };
		for (int vertex = 0; vertex < 3; vertex++) {
			triangles[face].position[vertex] = vertices[indices[vertex]];
			triangles[face].color[vertex] = lrt::Vec3(1, 1, 1);
		}
	}
	auto mesh = std::make_shared<lrt::TriangleMesh>(lrt::build_triangle_mesh(triangles));
	auto baked = lrt::bake_mesh_sdf(*mesh, 24);
	REQUIRE(baked.error == lrt::MESH_SDF_BAKE_OK);
	auto geometry = std::make_shared<lrt::SdfGeometryField>(std::move(baked.field));
	auto material = std::make_shared<lrt::SdfInstanceField>(lrt::bake_constant_instance_field(*geometry, lrt::Vec3(1, 1, 1)));
	lrt::SdfPrimitive primitive = lrt::make_sdf_primitive(geometry, material, {}, 1, 1, 1, 1);
	primitive.segment_geometry = lrt::SdfPrimitive::TRIANGLES;
	primitive.triangle_mesh = mesh;
	REQUIRE(primitive.sample(lrt::Vec3(0.25, 0.25, 0)).distance > 0.125);
	lrt::Vec3 position;
	lrt::ColorSdfSample sample;
	REQUIRE(primitive.trace_segment({}, lrt::Vec3(0.25, 0.25, 0), position, sample));
	CHECK(position.x == doctest::Approx(0.025));
	CHECK(position.y == doctest::Approx(0.025));
	CHECK_FALSE(primitive.trace_segment({}, lrt::Vec3(-0.25, -0.25, 0), position, sample));
	lrt::PrimitiveTransform transform;
	transform.origin = lrt::Vec3(3, 2, 1);
	transform.basis_x = lrt::Vec3(0, 2, 0);
	transform.basis_y = lrt::Vec3(-1, 0, 0);
	primitive = lrt::make_sdf_primitive(geometry, material, transform, 1, 1, 1, 1);
	primitive.segment_geometry = lrt::SdfPrimitive::TRIANGLES;
	primitive.triangle_mesh = mesh;
	REQUIRE(primitive.trace_segment(transform.origin, transform.xform(lrt::Vec3(0.25, 0.25, 0)), position, sample));
	CHECK(lrt::length(position - transform.xform(lrt::Vec3(0.025, 0.025, 0))) < 1e-6);
	primitive = lrt::make_sdf_primitive(geometry, material, {}, 1, 1, 1, 1);
	primitive.segment_geometry = lrt::SdfPrimitive::TRIANGLES;
	primitive.triangle_mesh = mesh;
	const lrt::Grid grid = lrt::make_grid_sized(0.25, lrt::Vec3(-0.125, -0.125, -0.125), lrt::Vec3(0.75, 0.75, 0.75));
	for (int copies : { 1, 2 }) {
		std::vector<lrt::SdfPrimitive> primitives(copies, primitive);
		const lrt::LocalField field = lrt::build_sdf_local_data(grid, primitives);
		const int index = lrt::index_of(grid, 0, 0, 0);
		CHECK((field.links[index] & (1u << 16)) == 0);
		CHECK(field.material[index * 4 + 1] > 0);
	}
}

TEST_CASE("[LRT] Propagation traces analytic boxes and raster-captured thin surfaces") {
	auto geometry = std::make_shared<lrt::SdfGeometryField>(lrt::bake_box_sdf(lrt::Vec3(0.02, 1, 1), 24));
	auto material = std::make_shared<lrt::SdfInstanceField>(lrt::bake_constant_instance_field(*geometry, lrt::Vec3(1, 1, 1)));
	lrt::SdfPrimitive primitive = lrt::make_sdf_primitive(geometry, material, {});
	primitive.segment_geometry = lrt::SdfPrimitive::BOX;
	primitive.box_half_extent = lrt::Vec3(0.01, 0.5, 0.5);
	lrt::Vec3 position;
	lrt::ColorSdfSample sample;
	REQUIRE(primitive.trace_segment(lrt::Vec3(-0.25, 0, 0), lrt::Vec3(0.25, 0, 0), position, sample));
	CHECK(position.x == doctest::Approx(-0.01));
	CHECK_FALSE(primitive.trace_segment(lrt::Vec3(-0.25, 1, 0), lrt::Vec3(0.25, 1, 0), position, sample));
	const int size[3] = { 8, 8, 8 };
	std::vector<uint8_t> surface(512, 0);
	surface[3 + 8 * (3 + 8 * 3)] = 1;
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, {}, {}, capture));
	geometry = std::make_shared<lrt::SdfGeometryField>(capture.geometry);
	material = std::make_shared<lrt::SdfInstanceField>(capture.material);
	primitive = lrt::make_sdf_primitive(geometry, material, {});
	REQUIRE(primitive.trace_segment(lrt::Vec3(1.5, 3.5, 3.5), lrt::Vec3(6.5, 3.5, 3.5), position, sample));
	CHECK(position.x == doctest::Approx(3.0));
	CHECK_FALSE(primitive.trace_segment(lrt::Vec3(1.5, 4.5, 3.5), lrt::Vec3(6.5, 4.5, 3.5), position, sample));
	geometry->distance[0] = 0;
	REQUIRE(primitive.trace_segment(lrt::Vec3(-1, 0.5, 0.5), lrt::Vec3(1, 0.5, 0.5), position, sample));
	CHECK(position.x == doctest::Approx(0.0));
	CHECK(sample.normal.x == doctest::Approx(-1.0));
}

} // namespace TestLRTInputs
