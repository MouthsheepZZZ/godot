#pragma once

#include "../lrt_cache.h"
#include "../lrt_render_bridge.h"
#include "../lrt_volume_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#ifdef TOOLS_ENABLED
#include "../lrt_export_plugin.h"

#include "scene/resources/packed_scene.h"
#endif
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/math/geometry_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "tests/test_macros.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_GLSLANG_ENABLED
#include "../lrt_debug.glsl.gen.h"
#include "../lrt_boundary_inc.glsl.gen.h"
#include "../lrt_config.gen.h"
#include "../lrt_inject.glsl.gen.h"
#include "../lrt_local_patch.glsl.gen.h"
#include "../lrt_screen_gather.glsl.gen.h"
#include "../lrt_forward_inc.glsl.gen.h"
#ifdef LRT_HDDAGI_ENABLED
#include "../lrt_boundary.glsl.gen.h"
#endif
#include "../lrt_propagate.glsl.gen.h"
#include "../lrt_geometry_debug.glsl.gen.h"
#include "../lrt_sky_project.glsl.gen.h"
#include "../lrt_sky_solve.glsl.gen.h"
#include "modules/glslang/shader_compile.h"
#endif
#ifdef MODULE_GRIDMAP_ENABLED
#include "scene/resources/3d/mesh_library.h"

#include "modules/gridmap/grid_map.h"
#endif
#ifdef MODULE_CSG_ENABLED
#include "modules/csg/csg_shape.h"
#endif

#include <algorithm>
#include <cstddef>
#include <set>
#include <thread>

// Exercise scheduler costs and shared capture ownership without GPU work or timing assertions.
class LRTVolume3DTestAccess {
public:
	static bool build_geometry_inputs(LRTVolume3D *p_volume, std::vector<LRTVolume::MeshInstance> &r_meshes, String *r_error = nullptr) {
		p_volume->_collect_geometry();
		String error;
		const bool ready = p_volume->_build_geometry_inputs(r_meshes, false, error, false);
		if (r_error != nullptr) {
			*r_error = error;
		}
		return ready;
	}
	static std::shared_ptr<const lrt::RasterGeometryCapture> seed_local_voxels(LRTVolume3D *p_volume, size_t p_receiver = 0) {
		p_volume->_collect_geometry();
		ERR_FAIL_COND_V(p_receiver >= p_volume->receivers.size(), nullptr);
		const auto &receiver = p_volume->receivers[p_receiver];
		auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
		lrt::mesh_triangles(receiver.mesh, *triangles);
		auto capture = std::make_shared<lrt::RasterGeometryCapture>();
		capture->geometry = lrt::bake_box_sdf({ 1, 1, 1 }, 4);
		capture->material = lrt::bake_constant_instance_field({ 1, 1, 1 });
		LRTVolume3D::MeshCaptureData data;
		data.triangles = triangles;
		data.material = std::make_shared<lrt::MaterialCapture>();
		data.raster_capture = capture;
		LRTVolume3D::MeshCaptureCache cache;
		cache.source = receiver;
		cache.key = hash_murmur3_one_64(receiver.sdf_resolution, uint32_t(receiver.mesh_content_signature) ^ 0x9e3779b9u);
		cache.key = hash_murmur3_one_64(receiver.material_signature, uint32_t(cache.key) ^ 0x9e3779b9u);
		cache.data = std::make_shared<const LRTVolume3D::MeshCaptureData>(std::move(data));
		p_volume->mesh_capture_cache[receiver.get_key()] = std::move(cache);
		return capture;
	}
	static uint64_t geometry_signature(LRTVolume3D *p_volume) {
		p_volume->_collect_geometry();
		return p_volume->_geometry_signature();
	}
	static uint64_t build_fingerprint(LRTVolume3D *p_volume) {
		p_volume->_collect_geometry();
		return p_volume->_build_cache_fingerprint();
	}
	static bool decode_capture(LRTVolume3D *p_volume, Dictionary &p_images, const Vector3i &p_size,
			LRTVolume::MeshInstance &r_mesh, String &r_error) {
		return p_volume->_decode_material_capture(p_images, p_size, r_mesh, r_error);
	}
	static const lrt::LocalField &staged_local(const Ref<LRTVolume> &p_solver) { return p_solver->staged_local; }
	static const lrt::LocalCache &staged_cache(const Ref<LRTVolume> &p_solver) { return p_solver->staged_cache; }
	static uint64_t material_component_signature(LRTVolume3D *p_volume, const Ref<Material> &p_material, uint8_t p_component) {
		return p_volume->_material_content_signature(p_material, false, p_component);
	}
	static Vector4 sky_contribution(const Ref<LRTVolume> &p_solver, int p_channel, int p_direction) {
		const float *values = p_solver->sky_link_radiance[p_channel][p_direction];
		return Vector4(values[0], values[1], values[2], values[3]);
	}
	static void begin_source_changes(const Ref<LRTVolume> &p_solver) {
		p_solver->source_full_dirty = false;
		p_solver->source_dirty_minimum = Vector3i();
		p_solver->source_dirty_maximum = Vector3i();
		p_solver->source_light_snapshots.clear();
	}
	static void include_source_probes(const Ref<LRTVolume> &p_solver, const Vector3i &p_low, const Vector3i &p_high) {
		p_solver->_include_source_probes(p_low, p_high);
	}
	static void include_source_light(const Ref<LRTVolume> &p_solver, const Vector3 &p_origin, float p_radius) {
		LRTVolume::SourceLightSnapshot light;
		light.origin = p_origin;
		light.radius = p_radius;
		p_solver->_include_source_light(light);
	}
	static PackedInt32Array consume_source_changes(const Ref<LRTVolume> &p_solver) {
		MutexLock lock(p_solver->params_mutex);
		const LRTVolume::SourceRegion region = p_solver->_consume_source_region();
		PackedInt32Array result;
		for (int axis = 0; axis < 3; axis++) {
			result.push_back(region.minimum[axis]);
		}
		for (int axis = 0; axis < 4; axis++) {
			result.push_back(region.size[axis]);
		}
		return result;
	}
	static bool light_capture_inputs_equal(const Dictionary &p_left, const Dictionary &p_right) {
		return LRTVolume3D::_light_capture_input_equal(p_left, p_right);
	}
	static int mesh_resolution(LRTVolume3D *p_volume, const Ref<Mesh> &p_mesh, const Object *p_instance = nullptr) { return p_volume->_effective_sdf_resolution(p_mesh, p_instance); }
	static uint64_t add_memory_fixture(LRTVolume3D *p_volume) {
		auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(2);
		uint64_t expected = triangles->capacity() * sizeof(lrt::MeshTriangle);
		for (int index = 0; index < 3; index++) {
			auto material = std::make_shared<lrt::MaterialCapture>();
			material->occupied.resize(4 << index);
			expected += material->occupied.capacity();
			auto data = std::make_shared<LRTVolume3D::MeshCaptureData>();
			data->triangles = index < 2 ? triangles : nullptr;
			data->material = material;
			LRTVolume3D::MeshCaptureCache capture;
			capture.data = std::move(data);
			p_volume->mesh_capture_cache[{ RID(), index }] = std::move(capture);
		}
		// A second reference to the entire capture must not count its material twice.
		p_volume->mesh_capture_cache[{ RID(), 3 }] = p_volume->mesh_capture_cache.at({ RID(), 0 });
		return expected;
	}
	static std::weak_ptr<const lrt::MaterialCapture> add_shared_capture(LRTVolume3D *p_volume, uint64_t p_key, uint64_t p_signature, int p_slot = 0) {
		LRTVolume3D::MeshCaptureData data;
		data.shared_signature = p_signature;
		data.content_signature = 1;
		auto material = std::make_shared<lrt::MaterialCapture>();
		material->occupied.resize(8);
		data.material = std::move(material);
		LRTVolume3D::MeshCaptureCache cache;
		cache.data = LRTVolume3D::_share_mesh_capture(p_key, std::move(data));
		std::weak_ptr<const lrt::MaterialCapture> result = cache.data->material;
		p_volume->mesh_capture_cache[{ RID(), p_slot }] = std::move(cache);
		return result;
	}
	static bool shares_capture(LRTVolume3D *p_first, LRTVolume3D *p_second) {
		return p_first->mesh_capture_cache.at({ RID(), 0 }).data == p_second->mesh_capture_cache.at({ RID(), 0 }).data;
	}
	static size_t shared_capture_entries() { return LRTVolume3D::shared_mesh_capture_cache.size(); }
	static void prepare_solver(LRTVolume3D *p_volume, double p_gpu_ms) {
		p_volume->solver.instantiate();
		p_volume->solver->has_local = true;
		p_volume->solver->last_gpu_ms.store(p_gpu_ms);
		p_volume->_refresh_propagation_budget();
	}
	static void set_local_field_valid(const Ref<LRTVolume> &p_solver) { p_solver->has_local = true; }
	static void release_gpu_resources(const Ref<LRTVolume> &p_solver) { p_solver->_free_gpu_resources(); }
	static int propagation_iterations(LRTVolume3D *p_volume) { return p_volume->_propagation_iterations_per_frame(); }
	static void refresh_budget(LRTVolume3D *p_volume) { p_volume->_refresh_propagation_budget(); }
	static void set_remaining(LRTVolume3D *p_volume, int p_iterations) { p_volume->propagation_iterations_remaining = p_iterations; }
	static int remaining(LRTVolume3D *p_volume) { return p_volume->propagation_iterations_remaining; }
	static void change_source(LRTVolume3D *p_volume) { p_volume->solver->source_version.fetch_add(1); }
	static void mark_submitted_this_frame(LRTVolume3D *p_volume) { p_volume->propagation_last_frame = Engine::get_singleton()->get_process_frames(); }
	static void set_pending_iterations(LRTVolume3D *p_volume, int p_iterations) { p_volume->solver->pending_step_iterations.store(p_iterations); }
	static void set_upload_state(LRTVolume3D *p_volume, bool p_pending, bool p_safe) {
		p_volume->local_apply_pending = p_pending;
		p_volume->solver->apply_pending = p_pending;
		p_volume->solver->apply_preserve_history = p_safe;
		p_volume->solver->apply_propagation_safe.store(p_safe);
	}
	static bool reuse_light_trunks_without_device(const Ref<LRTVolume> &p_solver) {
		p_solver->native_light_count = 1;
		p_solver->native_light_states.resize(1);
		p_solver->native_light_states[0].influence_origin = Vector3(1, 2, 3);
		p_solver->native_light_states[0].influence_radius = 4.0f;
		LRTVolume::LightTrunkInput cached;
		cached.kind = 3;
		cached.origin = Vector3(1, 2, 3);
		cached.radius = 4.0f;
		p_solver->uploaded_light_trunk_inputs = { cached };
		p_solver->light_trunks_dirty = false;
		bool recreate = false;
		if (!p_solver->_upload_light_trunks({ cached }, recreate) || recreate) {
			return false;
		}
		p_solver->native_light_states[0].scale = Vector3(8, 4, 2);
		return p_solver->_upload_light_trunks({ cached }, recreate) && !recreate;
	}
	static uint64_t shadow_signature(LRTVolume3D *p_volume) {
		p_volume->_collect_lights();
		return p_volume->_shadow_inputs_signature();
	}
};

namespace TestLRTInputs {

class CountingArrayMesh : public ArrayMesh {
public:
	mutable int surface_reads = 0;
	Array surface_get_arrays(int p_surface) const override {
		surface_reads++;
		return ArrayMesh::surface_get_arrays(p_surface);
	}
};

TEST_CASE("[SceneTree][LRT] Unbuilt editor volumes only fingerprint changed inputs") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	volume->set_receive_sky(false);
	volume->set_external_gi_enabled(false);
	root->add_child(volume);
	Ref<BoxMesh> box;
	box.instantiate();
	Ref<CountingArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, box->surface_get_arrays(0));
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.17, 0.29, 0.43));
	material->set_emission_energy_multiplier(1.0 + double(OS::get_singleton()->get_ticks_usec()));
	mesh->surface_set_material(0, material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	const bool editor_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	volume->poll();
	const int first_reads = mesh->surface_reads;
	CHECK(first_reads > 0);
	for (int frame = 0; frame < 3; frame++) {
		volume->poll();
	}
	CHECK(mesh->surface_reads == first_reads);
	instance->set_position(Vector3(0.1, 0, 0));
	volume->poll();
	const int moved_reads = mesh->surface_reads;
	CHECK(moved_reads > first_reads);
	volume->poll();
	CHECK(mesh->surface_reads == moved_reads);
	material->set_albedo(Color(0.4, 0.2, 0.1));
	volume->poll();
	const int material_reads = mesh->surface_reads;
	CHECK(material_reads > moved_reads);
	volume->poll();
	CHECK(mesh->surface_reads == material_reads);
	Engine::get_singleton()->set_editor_hint(editor_hint);
	memdelete(root);
}


TEST_CASE("[LRT] Light photometry and shadow parameters reuse Trunk candidates without GPU work") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	CHECK(LRTVolume3DTestAccess::reuse_light_trunks_without_device(solver));
}

TEST_CASE("[LRT] Native light input tracks in-place and removed projector and area textures") {
	Dictionary left;
	Dictionary right;
	CHECK(LRTVolume3DTestAccess::light_capture_inputs_equal(left, right));
	right["projector_signature"] = int64_t(1);
	CHECK_FALSE(LRTVolume3DTestAccess::light_capture_inputs_equal(left, right));
	left["projector_signature"] = int64_t(1);
	CHECK(LRTVolume3DTestAccess::light_capture_inputs_equal(left, right));
	right["area_texture_signature"] = int64_t(2);
	CHECK_FALSE(LRTVolume3DTestAccess::light_capture_inputs_equal(left, right));
	left["area_texture_signature"] = int64_t(2);
	CHECK(LRTVolume3DTestAccess::light_capture_inputs_equal(left, right));
	right["area_texture_signature"] = int64_t(0);
	CHECK_FALSE(LRTVolume3DTestAccess::light_capture_inputs_equal(left, right));
}

TEST_CASE("[LRT] Source regions merge old and new local influences without clearing the Volume") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(1.0, Vector3(), Vector3(16, 8, 16));
	LRTVolume3DTestAccess::begin_source_changes(solver);
	LRTVolume3DTestAccess::include_source_probes(solver, Vector3i(-4, -4, -4), Vector3i(3, 2, 1));
	LRTVolume3DTestAccess::include_source_probes(solver, Vector3i(2, 0, 0), Vector3i(4, 2, 2));
	const PackedInt32Array local = LRTVolume3DTestAccess::consume_source_changes(solver);
	CHECK(local[0] == 0);
	CHECK(local[3] == 4);
	CHECK(local[4] == 2);
	CHECK(local[5] == 2);
	CHECK(local[6] == 16);
	CHECK(LRTVolume3DTestAccess::consume_source_changes(solver)[6] == 0);
	LRTVolume3DTestAccess::include_source_light(solver, Vector3(2, 2, 3), 1.0f);
	LRTVolume3DTestAccess::include_source_light(solver, Vector3(10, 2, 3), 1.0f);
	const PackedInt32Array moved = LRTVolume3DTestAccess::consume_source_changes(solver);
	CHECK(moved[0] == 1);
	CHECK(moved[1] == 1);
	CHECK(moved[2] == 2);
	CHECK(moved[6] == 40);
	LRTVolume3DTestAccess::include_source_light(solver, Vector3(-8, 2, 3), 1.0f);
	CHECK(LRTVolume3DTestAccess::consume_source_changes(solver)[6] == 0);
	LRTVolume3DTestAccess::include_source_light(solver, Vector3(), -1.0f);
	CHECK(LRTVolume3DTestAccess::consume_source_changes(solver)[6] == 2048);
}


// Slot identities are history-dependent; compare the logical per-Probe response.
static bool same_local_records(const lrt::LocalField &p_a, const lrt::LocalField &p_b) {
	if (p_a.local_record_indices.size() != p_b.local_record_indices.size()) {
		return false;
	}
	for (size_t probe = 0; probe < p_a.local_record_indices.size(); probe++) {
		if (!(p_a.get_local_record(probe) == p_b.get_local_record(probe))) {
			return false;
		}
	}
	return p_a.validate_local_records() && p_b.validate_local_records();
}

TEST_CASE("[LRT] GPU local slots preserve the full reflected and emissive record ABI") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(0.5, Vector3(-2, -2, -2), Vector3(4, 4, 4));
	for (const lrt::Vec3 &color : { lrt::Vec3(0.25, 0.5, 0.75), lrt::Vec3() }) {
		LRTVolume::BoxInstance box;
		box.local_extent = { 1, 1, 1 };
		box.sdf_resolution = 8;
		box.color = color;
		box.emission = { 8, 4, 2 };
		solver->set_box_instances({ box });
		REQUIRE(solver->bake_local_field_data(false).ok);
		const lrt::LocalField &field = LRTVolume3DTestAccess::staged_local(solver);
		REQUIRE_FALSE(field.local_records.empty());
		CHECK(field.local_records.size() < field.local_record_indices.size());
		CHECK(field.validate_local_records());
		REQUIRE(field.gpu_local_records.size() == field.local_records.size() * 32);
		bool emission_present = false;
		for (size_t slot = 0; slot < field.local_records.size(); slot++) {
			const lrt::LocalRecord &record = field.local_records[slot];
			const float *encoded = field.gpu_local_records.data() + slot * 32;
			uint32_t reflectance;
			memcpy(&reflectance, encoded + 16, sizeof(reflectance));
			CHECK(reflectance == record.reflectance);
			if (color.x == 0.0) {
				CHECK(reflectance == 0);
			}
			for (size_t component = 0; component < 16; component++) {
				CHECK(encoded[component] == record.geometry[component]);
			}
			CHECK(encoded[17] == record.transport_weight);
			CHECK(encoded[18] == 0.0f);
			CHECK(encoded[19] == 0.0f);
			for (size_t component = 0; component < 12; component++) {
				CHECK(encoded[20 + component] == record.emission[component]);
				emission_present |= record.emission[component] != 0;
			}
		}
		CHECK(emission_present);
	}
}

TEST_CASE("[LRT] Local records separate geometry, averaged RGB8 reflectance and HDR emission") {
	constexpr uint32_t open = (1u << lrt::DIRECTION_COUNT) - 1;
	std::array<lrt::LocalSurface, lrt::DIRECTION_COUNT> surfaces;
	surfaces[0] = { lrt::Vec3(1, 0, 0), lrt::Vec3(16, 4, 0) };
	surfaces[25] = { lrt::Vec3(0, 0, 1), {} };
	const lrt::LocalRecord air = lrt::build_local_record(open, surfaces);
	CHECK(air == lrt::LocalRecord());
	CHECK(sizeof(lrt::LocalRecord) == 128);
	CHECK(offsetof(lrt::LocalRecord, emission) == 80);
	const uint32_t mask = open & ~(1u << 0) & ~(1u << 25);
	const lrt::LocalRecord mixed = lrt::build_local_record(mask, surfaces);
	CHECK(mixed.reflectance == 0x800080u);
	CHECK(mixed.get_reflectance().x == doctest::Approx(128.0 / 255.0));
	CHECK(mixed.get_reflectance().y == 0.0);
	CHECK(mixed.get_reflectance().z == doctest::Approx(128.0 / 255.0));
	CHECK(mixed.emission[0] == doctest::Approx(lrt::WEIGHT * lrt::C0 * 16));
	CHECK(mixed.emission[4] == doctest::Approx(lrt::WEIGHT * lrt::C0 * 4));
	CHECK(mixed.emission[8] == 0.0f);
	CHECK(mixed.emission[1] < 0.0f);
	CHECK(mixed.geometry != air.geometry);
	for (lrt::LocalSurface &surface : surfaces) {
		surface.reflectance = lrt::Vec3(0.5, 0, 0.5);
	}
	const lrt::LocalRecord averaged = lrt::build_local_record(mask, surfaces);
	CHECK(averaged == mixed);
	for (lrt::LocalSurface &surface : surfaces) {
		surface.reflectance = {};
	}
	const lrt::LocalRecord black = lrt::build_local_record(mask, surfaces);
	CHECK(black.reflectance == 0);
	CHECK(black.geometry == mixed.geometry);
	CHECK(black.emission == mixed.emission);
	for (lrt::LocalSurface &surface : surfaces) {
		surface.emission = lrt::Vec3(1024, 0, 0);
	}
	const lrt::LocalRecord emissive_black = lrt::build_local_record(mask, surfaces);
	CHECK(emissive_black.reflectance == 0);
	CHECK(emissive_black.geometry == black.geometry);
	CHECK(emissive_black.transport_weight == black.transport_weight);
	CHECK(emissive_black.emission[0] > 1.0f);
}

TEST_CASE("[LRT] Every RGB iteration covers exactly the six face neighbors") {
	std::set<int> selected;
	for (int direction : lrt::face_directions()) {
		REQUIRE(direction >= 0);
		REQUIRE(direction < lrt::DIRECTION_COUNT);
		const int *offset = lrt::directions()[direction].offset;
		CHECK(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] == 1);
		selected.insert(direction);
	}
	CHECK(selected.size() == 6);
	for (int direction = 0; direction < lrt::DIRECTION_COUNT; direction++) {
		const int *offset = lrt::directions()[direction].offset;
		const bool face = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] == 1;
		CHECK(selected.count(direction) == (face ? 1 : 0));
	}
}

TEST_CASE("[LRT] Encoded local weights bound unit and signed six-face responses") {
	const lrt::Direction *dirs = lrt::directions();
	uint32_t random = 0x4c5254u;
	int attenuated_records = 0;
	for (int sample = 0; sample < 320; sample++) {
		random = random * 1664525u + 1013904223u;
		uint32_t mask = random & ((1u << lrt::DIRECTION_COUNT) - 1);
		if (sample < 64) {
			int face = 0;
			for (int j = 0; j < lrt::DIRECTION_COUNT; j++) {
				if (dirs[j].offset[0] * dirs[j].offset[0] + dirs[j].offset[1] * dirs[j].offset[1] + dirs[j].offset[2] * dirs[j].offset[2] == 1) {
					mask = (mask & ~(1u << j)) | (((uint32_t(sample) >> face++) & 1u) << j);
				}
			}
		}
		std::array<lrt::LocalSurface, lrt::DIRECTION_COUNT> surfaces;
		for (int j = 0; j < lrt::DIRECTION_COUNT; j++) {
			for (int channel = 0; channel < 3; channel++) {
				random = random * 1664525u + 1013904223u;
				surfaces[size_t(j)].reflectance[channel] = (double(random & 0xffu) + 0.49) / 255.0;
			}
		}
		const lrt::LocalRecord record = lrt::build_local_record(mask, surfaces);
		const lrt::Vec3 rho = record.get_reflectance();
		const double rho_max = std::max({ rho.x, rho.y, rho.z });
		double unit[4] = {};
		double signed_input[4] = {};
		for (int j = 0; j < lrt::DIRECTION_COUNT; j++) {
			const int *offset = dirs[j].offset;
			if (!(mask & (1u << j)) ||
					offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] != 1) {
				continue;
			}
			double projected[4];
			lrt::positive_basis(dirs[j].direction, projected);
			random = random * 1664525u + 1013904223u;
			const double input = double(random & 0xffffu) / 32767.5 - 1.0;
			for (int coefficient = 0; coefficient < 4; coefficient++) {
				unit[coefficient] += (4.0 * lrt::PI / 6.0) * projected[coefficient];
				signed_input[coefficient] += (4.0 * lrt::PI / 6.0) * projected[coefficient] * input;
			}
		}
		auto response_peak = [&](const double *p_input, double p_rho) {
			double response[4];
			for (int row = 0; row < 4; row++) {
				double reflected = 0.0;
				for (int column = 0; column < 4; column++) {
					reflected += record.geometry[size_t(row * 4 + column)] * p_input[column];
				}
				response[row] = p_input[row] + p_rho * reflected;
			}
			return std::abs(lrt::C0 * response[0]) + lrt::C1 * std::sqrt(response[1] * response[1] +
					response[2] * response[2] + response[3] * response[3]);
		};
		const float weight = record.transport_weight;
		const double unit_peak = response_peak(unit, rho_max);
		CHECK(weight > 0.0f);
		CHECK(weight <= 1.0f);
		CHECK(weight == doctest::Approx(1.0 / std::max(1.0, unit_peak)));
		CHECK(weight * unit_peak <= 1.0 + 1e-6);
		for (int channel = 0; channel < 3; channel++) {
			CHECK(weight * response_peak(signed_input, rho[channel]) <= 1.0 + 1e-6);
		}
		attenuated_records += weight < 0.999f ? 1 : 0;
	}
	CHECK(attenuated_records > 0);
}

TEST_CASE("[LRT] Shared fields live only while used and survive registry clearing") {
	constexpr uint64_t signature = 0x4c52544c494645ull;
	auto geometry = lrt::share_asset_field(signature, lrt::CoarseGeometryField());
	auto material = lrt::share_instance_field(signature, lrt::SurfaceMaterialField());
	CHECK(lrt::share_asset_field(signature, lrt::CoarseGeometryField()).get() == geometry.get());
	CHECK(lrt::share_instance_field(signature, lrt::SurfaceMaterialField()).get() == material.get());
	std::weak_ptr<const lrt::CoarseGeometryField> old_geometry = geometry;
	std::weak_ptr<const lrt::SurfaceMaterialField> old_material = material;
	lrt::clear_shared_asset_fields();
	CHECK_FALSE(old_geometry.expired());
	CHECK_FALSE(old_material.expired());
	auto replacement_geometry = lrt::share_asset_field(signature, lrt::CoarseGeometryField());
	auto replacement_material = lrt::share_instance_field(signature, lrt::SurfaceMaterialField());
	CHECK(replacement_geometry.get() != geometry.get());
	CHECK(replacement_material.get() != material.get());
	geometry.reset();
	material.reset();
	CHECK(old_geometry.expired());
	CHECK(old_material.expired());
	CHECK(lrt::find_shared_asset_field(signature).get() == replacement_geometry.get());
	CHECK(lrt::find_shared_instance_field(signature).get() == replacement_material.get());
	std::weak_ptr<const lrt::CoarseGeometryField> final_geometry = replacement_geometry;
	std::weak_ptr<const lrt::SurfaceMaterialField> final_material = replacement_material;
	replacement_geometry.reset();
	replacement_material.reset();
	CHECK(final_geometry.expired());
	CHECK(final_material.expired());
	CHECK(lrt::find_shared_asset_field(signature).get() == nullptr);
	CHECK(lrt::find_shared_instance_field(signature).get() == nullptr);
}

TEST_CASE("[LRT] Released fields do not contribute Volume coverage") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	const ObjectID owner = solver->get_instance_id();
	Dictionary state;
	state["owner"] = uint64_t(owner);
	state["solver"] = solver;
	state["enabled"] = true;
	LRTRenderBridge::set_view_scenario(RID());
	LRTRenderBridge::set_state(state);
	CHECK(LRTRenderBridge::get_states().is_empty());
	LRTVolume3DTestAccess::set_local_field_valid(solver);
	LRTRenderBridge::set_state(state);
	CHECK(LRTRenderBridge::get_states().size() == 1);
	LRTVolume3DTestAccess::release_gpu_resources(solver);
	LRTRenderBridge::set_view_scenario(RID());
	CHECK_FALSE(solver->has_local_field());
	CHECK(LRTRenderBridge::get_states().is_empty());
	LRTRenderBridge::clear(owner);
}

TEST_CASE("[LRT] Concurrent field preparation shares a single live allocation") {
	constexpr uint64_t signature = 0x4c525452414345ull;
	std::array<std::shared_ptr<const lrt::CoarseGeometryField>, 8> geometry;
	std::array<std::shared_ptr<const lrt::SurfaceMaterialField>, 8> material;
	std::array<std::thread, 8> workers;
	for (size_t index = 0; index < workers.size(); index++) {
		workers[index] = std::thread([&, index]() {
			geometry[index] = lrt::share_asset_field(signature, lrt::CoarseGeometryField());
			material[index] = lrt::share_instance_field(signature, lrt::SurfaceMaterialField());
		});
	}
	for (std::thread &worker : workers) {
		worker.join();
	}
	for (size_t index = 1; index < workers.size(); index++) {
		CHECK(geometry[index].get() == geometry[0].get());
		CHECK(material[index].get() == material[0].get());
	}
}

TEST_CASE("[SceneTree][LRT] Constant emissive BoxMesh uses the shared GPU capture input path") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	material->set_emission(Color(0.5, 0.25, 1.0));
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	const auto capture = LRTVolume3DTestAccess::seed_local_voxels(volume);
	REQUIRE(bool(capture));
	std::vector<LRTVolume::MeshInstance> meshes;
	REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
	REQUIRE(meshes.size() == 1);
	CHECK(meshes[0].raster_geometry);
	CHECK(meshes[0].raster_capture.get() == capture.get());
	CHECK_FALSE(meshes[0].triangles->empty());
	CHECK(int64_t(volume->get_preparation_status()["voxel_captures"]) == 0);
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Runtime voxel cache misses fail without capture after material or precision edits") {
	CHECK_FALSE(Engine::get_singleton()->is_editor_hint());
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	material->set_emission_energy_multiplier(1.0 + double(OS::get_singleton()->get_ticks_usec()));
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_material(material);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_name("UnpreparedMesh");
	instance->set_mesh(mesh);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	std::vector<LRTVolume::MeshInstance> meshes;
	String error;
	auto check_missing = [&]() {
		CHECK_FALSE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes, &error));
		CHECK(error.contains("UnpreparedMesh"));
		CHECK(error.contains("runtime voxel capture is disabled"));
		CHECK(meshes.empty());
		CHECK(int64_t(volume->get_preparation_status()["voxel_captures"]) == 0);
	};
	check_missing();
	const auto capture = LRTVolume3DTestAccess::seed_local_voxels(volume);
	REQUIRE(bool(capture));
	REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes, &error));
	CHECK(error.is_empty());
	REQUIRE(meshes.size() == 1);
	CHECK(meshes.front().raster_capture.get() == capture.get());
	material->set_emission(Color(0.25, 0.5, 1.0));
	check_missing();
	LRTVolume3DTestAccess::seed_local_voxels(volume);
	REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
	instance->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.037);
	check_missing();
	memdelete(root);
}

TEST_CASE("[LRT] Screen gather validity is per view and cleared when resources are released") {
	using RenderBuffers = RendererSceneRenderImplementation::RenderForwardClustered::RenderBufferDataForwardClustered;
	Ref<RenderBuffers> first;
	first.instantiate();
	Ref<RenderBuffers> second;
	second.instantiate();
	CHECK_FALSE(first->lrt_screen_gather_valid);
	CHECK_FALSE(second->lrt_screen_gather_valid);
	first->lrt_screen_gather_valid = true;
	CHECK_FALSE(second->lrt_screen_gather_valid);
	first->release_lrt_screen_gather();
	CHECK_FALSE(first->lrt_screen_gather_valid);
}

TEST_CASE("[LRT] Trunk registrations match support boxes at partial and floating-point boundaries") {
	const lrt::Grid grid = lrt::make_grid_sized(0.37, lrt::Vec3(-1.2, -2.7, 0.13), lrt::Vec3(8.51, 7.77, 4.58));
	std::vector<lrt::GeometryInstance> inputs;
	auto add = [&](const lrt::Vec3 &p_min, const lrt::Vec3 &p_max) {
		lrt::GeometryInstance primitive;
		primitive.bounds_min = p_min;
		primitive.bounds_max = p_max;
		primitive.signature = inputs.size() + 1;
		primitive.layer_mask = 1u << (inputs.size() % 3);
		inputs.push_back(primitive);
	};
	for (int axis = 0; axis < 3; axis++) {
		const int last = ((grid.size[axis] - 1) / lrt::TRUNK) * lrt::TRUNK;
		for (int boundary : { -1, lrt::TRUNK - 1, lrt::TRUNK * 2 + 1, last + lrt::TRUNK + 1 }) {
			const double value = grid.min[axis] + boundary * grid.spacing;
			for (double position : { std::nextafter(value, value - 1.0), value, std::nextafter(value, value + 1.0) }) {
				lrt::Vec3 point = grid.min + lrt::Vec3(1, 1, 1);
				point[axis] = position;
				add(point, point);
			}
		}
	}
	for (int i = 0; i < 96; i++) {
		const lrt::Vec3 point = grid.min + lrt::Vec3((i * 17) % 97 - 13, (i * 31) % 97 - 13, (i * 43) % 97 - 13) * (grid.spacing * 0.5);
		const lrt::Vec3 extent = lrt::Vec3(0.11, 0.29, 0.03) * (i % 5);
		add(point - extent, point + extent);
	}
	add(lrt::Vec3(-1000, -1000, -1000), lrt::Vec3(1000, 1000, 1000));
	add(lrt::Vec3(1000, 1000, 1000), lrt::Vec3(1001, 1001, 1001));
	inputs.push_back(inputs[6]);
	auto verify = [](const lrt::Grid &p_grid, const std::vector<lrt::GeometryInstance> &p_inputs, const lrt::TrunkIndex &p_index) {
		const int dimensions[3] = {
			(p_grid.size[0] + lrt::TRUNK - 1) / lrt::TRUNK,
			(p_grid.size[1] + lrt::TRUNK - 1) / lrt::TRUNK,
			(p_grid.size[2] + lrt::TRUNK - 1) / lrt::TRUNK,
		};
		for (int z = 0; z < dimensions[2]; z++) {
			for (int y = 0; y < dimensions[1]; y++) {
				for (int x = 0; x < dimensions[0]; x++) {
					std::vector<int> expected;
					const int coordinate[3] = { x, y, z };
					for (int input = 0; input < int(p_inputs.size()); input++) {
						bool overlap = true;
						for (int axis = 0; axis < 3; axis++) {
							const double low = p_grid.min[axis] + (coordinate[axis] * lrt::TRUNK - 1) * p_grid.spacing;
							const double high = p_grid.min[axis] + (coordinate[axis] * lrt::TRUNK + lrt::TRUNK + 1) * p_grid.spacing;
							if (p_inputs[size_t(input)].bounds_min[axis] > high || p_inputs[size_t(input)].bounds_max[axis] < low) {
								overlap = false;
								break;
							}
						}
						if (overlap) {
							expected.push_back(input);
						}
					}
					CHECK(p_index.get_candidates(x + dimensions[0] * (y + dimensions[1] * z)) == expected);
				}
			}
		}
		lrt::TrunkIndex fresh;
		fresh.update(p_grid, p_inputs);
		CHECK(p_index.get_signatures() == fresh.get_signatures());
	};
	lrt::TrunkIndex index;
	index.update(grid, inputs);
	CHECK(index.get_registration_count() == inputs.size() - 1);
	verify(grid, inputs, index);
	index.update(grid, inputs);
	CHECK(index.get_update_stats().added == 0);
	CHECK(index.get_update_stats().removed == 0);
	CHECK(index.get_update_stats().overlap_tests == 0);
	std::reverse(inputs.begin(), inputs.end());
	index.update(grid, inputs);
	CHECK(index.get_update_stats().added == 0);
	CHECK(index.get_update_stats().removed == 0);
	CHECK(index.get_update_stats().overlap_tests == 0);
	verify(grid, inputs, index);
	inputs.erase(inputs.begin() + 2);
	index.update(grid, inputs);
	CHECK(index.get_update_stats().added == 0);
	CHECK(index.get_update_stats().removed == 1);
	verify(grid, inputs, index);
	const lrt::Grid changed_grid = lrt::make_grid_sized(0.51, lrt::Vec3(-2.1, 0.3, -0.71), lrt::Vec3(10.1, 6.3, 8.8));
	index.update(changed_grid, inputs);
	verify(changed_grid, inputs, index);
}

TEST_CASE("[LRT] Trunk registration updates touch only changed support regions") {
	const lrt::Grid grid = lrt::make_grid_sized(1, lrt::Vec3(-40, -40, -40), lrt::Vec3(80, 80, 80));
	std::vector<lrt::GeometryInstance> inputs(2);
	inputs[0].bounds_min = inputs[0].bounds_max = lrt::Vec3(-30, -30, -30);
	inputs[0].signature = 100;
	inputs[1].bounds_min = inputs[1].bounds_max = lrt::Vec3(-6, -6, -6);
	inputs[1].signature = 200;
	lrt::TrunkIndex index;
	index.update(grid, inputs);
	CHECK(index.get_registration_count() == 2);
	CHECK(index.get_candidate_references() == 2);
	CHECK(index.get_update_stats().added == 2);
	CHECK(index.get_update_stats().overlap_tests <= 54);
	const std::vector<uint64_t> initial = index.get_signatures();
	inputs[0].bounds_min = inputs[0].bounds_max = lrt::Vec3(22, 22, 22);
	inputs[0].signature++;
	index.update(grid, inputs);
	CHECK(index.get_update_stats().added == 1);
	CHECK(index.get_update_stats().removed == 1);
	CHECK(index.get_update_stats().overlap_tests <= 27);
	int changed = 0;
	for (size_t trunk = 0; trunk < initial.size(); trunk++) {
		changed += initial[trunk] != index.get_signatures()[trunk];
	}
	CHECK(changed == 2);
	const std::vector<uint64_t> moved = index.get_signatures();
	std::reverse(inputs.begin(), inputs.end());
	index.update(grid, inputs);
	CHECK(index.get_update_stats().added == 0);
	CHECK(index.get_update_stats().removed == 0);
	CHECK(index.get_update_stats().overlap_tests == 0);
	CHECK(index.get_signatures() == moved);
	inputs.push_back(inputs[0]);
	index.update(grid, inputs);
	CHECK(index.get_registration_count() == 2);
	CHECK(index.get_candidate_references() == 3);
	CHECK(index.get_update_stats().added == 0);
	CHECK(index.get_update_stats().overlap_tests == 0);
	inputs.erase(inputs.begin());
	index.update(grid, inputs);
	CHECK(index.get_registration_count() == 2);
	CHECK(index.get_candidate_references() == 2);
	CHECK(index.get_update_stats().removed == 0);
	inputs.clear();
	index.update(grid, inputs);
	CHECK(index.get_registration_count() == 0);
	CHECK(index.get_candidate_references() == 0);
	CHECK(index.get_update_stats().removed == 2);
	CHECK(index.get_update_stats().overlap_tests == 0);
	CHECK(index.payload_bytes() > 0);
}

TEST_CASE("[LRT] Trunk registrations do not retain geometry or material resources") {
	const lrt::Grid grid = lrt::make_grid_sized(1, lrt::Vec3(-4, -4, -4), lrt::Vec3(8, 8, 8));
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf(lrt::Vec3(1, 1, 1), 4));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field(lrt::Vec3(1, 1, 1)));
	std::weak_ptr<const lrt::CoarseGeometryField> weak_geometry = geometry;
	std::weak_ptr<const lrt::SurfaceMaterialField> weak_material = material;
	lrt::GeometryInstance primitive = lrt::make_sdf_primitive(geometry, material, lrt::PrimitiveTransform(), 100);
	lrt::TrunkIndex index;
	index.update(grid, { primitive });
	primitive = {};
	geometry.reset();
	material.reset();
	CHECK(index.get_registration_count() == 1);
	CHECK(weak_geometry.expired());
	CHECK(weak_material.expired());
}

TEST_CASE("[LRT] Material updates preserve geometry and independently refresh rho emission and k") {
	const lrt::Grid grid = lrt::make_grid_sized(0.5, { -8, -4, -4 }, { 16, 8, 8 });
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf({ 1, 1, 1 }, 8));
	const uint64_t asset = lrt::box_field_signature({ 1, 1, 1 }, 8);
	auto instance = [&](const lrt::Vec3 &p_color, const lrt::Vec3 &p_emission, double p_x, uint8_t p_sides = lrt::EMISSION_FRONT) {
		auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field(p_color, p_emission, p_sides));
		lrt::PrimitiveTransform transform;
		transform.origin.x = p_x;
		const uint64_t appearance = lrt::instance_field_signature(*material);
		return lrt::make_sdf_primitive(geometry, material, transform, lrt::primitive_signature(asset, appearance, transform), 1, asset, appearance);
	};
	const lrt::GeometryInstance fixed = instance({ 0.3, 0.5, 0.7 }, { 1, 2, 3 }, -6);
	lrt::Vec3 color(0.4, 0.6, 0.8);
	lrt::Vec3 emission(8, 4, 2);
	std::vector<lrt::GeometryInstance> inputs = { fixed, instance(color, emission, 0) };
	lrt::TrunkIndex index;
	index.update(grid, inputs);
	lrt::LocalCache cache;
	lrt::LocalField previous = lrt::build_sdf_local_data(grid, inputs, nullptr, 3, nullptr, &cache);
	cache.local = &previous;
	REQUIRE(previous.local_records.size() > 0);
	lrt::LocalField recycled;
	lrt::LocalCache recycled_cache;
	for (int phase = 0; phase < 12; phase++) {
		if (phase == 0 || phase == 6) {
			emission = { 3, 6, 9 };
		} else if (phase == 1 || phase == 7) {
			color = { 0.8, 0.2, 0.4 };
		} else if (phase == 2) {
			color = { 0.2, 0.7, 0.3 };
			emission = { 5, 1, 2 };
		} else if (phase == 4) {
			emission = {};
		} else if (phase == 5) {
			color = {};
		}
		uint8_t sides = lrt::EMISSION_FRONT;
		if (phase == 9) {
			sides = lrt::EMISSION_BACK;
		} else if (phase == 10) {
			sides = lrt::EMISSION_BOTH;
		}
		inputs[1] = instance(color, emission, phase >= 8 ? 3.0 : 0.0, sides);
		index.update(grid, inputs);
		lrt::LocalField next = lrt::build_sdf_local_data(grid, inputs, nullptr, 4, &cache, &recycled_cache, &recycled, &index);
		const lrt::LocalField full = lrt::build_sdf_local_data(grid, inputs);
		CHECK(next.material == full.material);
		CHECK(next.links == full.links);
		CHECK(next.solid_count == full.solid_count);
		CHECK(next.surface_count == full.surface_count);
		CHECK(same_local_records(next, full));
		CHECK(lrt::local_material_rgba(next, false) == lrt::local_material_rgba(full, false));
		CHECK(lrt::local_material_rgba(next, true) == lrt::local_material_rgba(full, true));
		if (phase < 8 || phase >= 9) {
			CHECK(next.geometry_dirty_trunks == 0);
			CHECK(next.occupancy_probes_updated == 0);
			CHECK(next.changed_occupancy.empty());
			CHECK_FALSE(next.links_changed);
			CHECK(index.get_update_stats().added == 0);
			CHECK(index.get_update_stats().removed == 0);
			CHECK(index.get_update_stats().overlap_tests == 0);
		}
		if (phase == 0 || phase == 4 || phase >= 9) {
			CHECK(next.reflectance_dirty_trunks == 0);
			CHECK(next.geometry_records_built == 0);
			CHECK(next.reflectance_records_updated == 0);
			CHECK(next.transport_weights_updated == 0);
			CHECK(next.emission_records_updated > 0);
		}
		if (phase == 1 || phase == 5 || phase == 7) {
			CHECK(next.emission_dirty_trunks == 0);
			CHECK(next.emission_records_updated == 0);
			CHECK(next.transport_weights_updated > 0);
		}
		if (phase == 3) {
			CHECK(next.dirty_trunk_count == 0);
			CHECK(next.surface_workspaces_built == 0);
		}
		if (phase == 6) {
			CHECK(next.geometry_records_built > 0); // New pure-emission records initialize G from OpenMask.
			CHECK(next.reflectance_records_updated == 0);
		}
		if (phase == 8) {
			CHECK(next.geometry_dirty_trunks > 0);
			CHECK(next.occupancy_probes_updated > 0);
		}
		if (phase < 8) {
			for (int probe = 0; probe < grid.count; probe++) {
				const uint32_t before = previous.local_record_indices[size_t(probe)];
				const uint32_t after = next.local_record_indices[size_t(probe)];
				if (before != lrt::LocalField::NO_RECORD && after != lrt::LocalField::NO_RECORD) {
					CHECK(before == after);
					CHECK(previous.get_local_record(size_t(probe)).geometry == next.get_local_record(size_t(probe)).geometry);
				}
			}
		}
		recycled = std::move(previous);
		previous = std::move(next);
		std::swap(cache, recycled_cache);
		cache.local = &previous;
		recycled_cache.local = &recycled;
	}
}

TEST_CASE("[LRT] Sparse dirty-region jobs preserve serial transport and open masks") {
	const lrt::Grid grid = lrt::make_grid_sized(0.5, lrt::Vec3(-8, -8, -8), lrt::Vec3(16, 16, 16));
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf(lrt::Vec3(1, 1, 1), 16));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field(lrt::Vec3(0.3, 0.4, 0.5)));
	const uint64_t asset_signature = lrt::box_field_signature(lrt::Vec3(1, 1, 1), 16);
	const uint64_t material_signature = lrt::instance_field_signature(*material);
	lrt::PrimitiveTransform transform;
	transform.origin = lrt::Vec3(-6, -6, -6);
	lrt::GeometryInstance initial = lrt::make_sdf_primitive(geometry, material, transform,
			lrt::primitive_signature(asset_signature, material_signature, transform), 1, asset_signature, material_signature);
	lrt::LocalCache cache;
	lrt::LocalField previous = lrt::build_sdf_local_data(grid, { initial }, nullptr, 1, nullptr, &cache);
	cache.local = &previous;
	lrt::TrunkIndex index;
	index.update(grid, { initial });
	for (const lrt::Vec3 &offset : { lrt::Vec3(0.25, 0, 0), lrt::Vec3(6, 6, 6) }) {
		transform.origin = initial.origin + offset;
		const lrt::GeometryInstance moved = lrt::make_sdf_primitive(geometry, material, transform,
				lrt::primitive_signature(asset_signature, material_signature, transform), 1, asset_signature, material_signature);
		lrt::LocalField serial = lrt::build_sdf_local_data(grid, { moved }, nullptr, 1, &cache);
		index.update(grid, { moved });
		lrt::LocalField parallel = lrt::build_sdf_local_data(grid, { moved }, nullptr, 8, &cache, nullptr, nullptr, &index);
		lrt::LocalField full = lrt::build_sdf_local_data(grid, { moved });
		CHECK(serial.dirty_trunk_count > 0);
		CHECK(serial.dirty_trunk_count < serial.trunk_count);
		CHECK(serial.material == parallel.material);
		CHECK(same_local_records(serial, parallel));
		CHECK(serial.links == parallel.links);
		CHECK(lrt::local_material_rgba(serial, false) == lrt::local_material_rgba(parallel, false));
		CHECK(lrt::local_material_rgba(serial, true) == lrt::local_material_rgba(parallel, true));
		CHECK(serial.material == full.material);
		CHECK(serial.links == full.links);
		CHECK(same_local_records(serial, full));
		CHECK(serial.solid_count == full.solid_count);
		CHECK(serial.surface_count == full.surface_count);
	}
}

TEST_CASE("[LRT] Incremental open masks match full builds for emission and overlapping geometry edits") {
	const lrt::Grid grid = lrt::make_grid_sized(0.5, lrt::Vec3(-4, -4, -4), lrt::Vec3(8, 8, 8));
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf(lrt::Vec3(2, 2, 2), 16));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field(lrt::Vec3(0.3, 0.4, 0.5)));
	const uint64_t asset = lrt::box_field_signature(lrt::Vec3(2, 2, 2), 16);
	auto primitive = [&](const std::shared_ptr<lrt::SurfaceMaterialField> &p_material, const lrt::Vec3 &p_origin, uint32_t p_layers = 1) {
		lrt::PrimitiveTransform transform;
		transform.origin = p_origin;
		const uint64_t appearance = lrt::instance_field_signature(*p_material);
		return lrt::make_sdf_primitive(geometry, p_material, transform,
				lrt::primitive_signature(asset, appearance, transform), p_layers, asset, appearance);
	};
	const std::vector<lrt::GeometryInstance> initial = { primitive(material, {}), primitive(material, lrt::Vec3(1.1, 0.5, 0.7)) };
	lrt::LocalCache cache;
	lrt::LocalField previous = lrt::build_sdf_local_data(grid, initial, nullptr, 1, nullptr, &cache);
	cache.local = &previous;
	lrt::TrunkIndex index;
	index.update(grid, initial);
	for (int edit = 0; edit < 5; edit++) {
		auto changed = std::make_shared<lrt::SurfaceMaterialField>(*material);
		changed->emission = { 3.0f, 1.0f, 0.5f };
		if (edit == 4) {
			std::fill_n(changed->color_size, 3, 2);
			changed->albedo.resize(24);
			changed->emission.resize(24);
			for (int cell = 0; cell < 8; cell++) {
				for (int channel = 0; channel < 3; channel++) {
					changed->albedo[size_t(cell) * 3 + channel] = material->albedo[size_t(channel)];
					changed->emission[size_t(cell) * 3 + channel] = float(cell + channel) * 0.25f;
				}
			}
		}
		if (edit == 1) {
			changed->albedo[0] = 17;
		}
		std::vector<lrt::GeometryInstance> inputs = initial;
		inputs[0] = primitive(changed, edit == 2 ? lrt::Vec3(0.25, 0, 0) : lrt::Vec3(), edit == 3 ? 2 : 1);
		index.update(grid, inputs);
		lrt::LocalField incremental = lrt::build_sdf_local_data(grid, inputs, nullptr, 8, &cache, nullptr, nullptr, &index);
		lrt::LocalField full = lrt::build_sdf_local_data(grid, inputs);
		CHECK(same_local_records(incremental, full));
		CHECK(incremental.links == full.links);
		CHECK(incremental.solid_count == full.solid_count);
		CHECK(incremental.surface_count == full.surface_count);
		CHECK(incremental.material == full.material);
		CHECK(lrt::local_material_rgba(incremental, false) == lrt::local_material_rgba(full, false));
		CHECK(lrt::local_material_rgba(incremental, true) == lrt::local_material_rgba(full, true));
	}
}


static std::vector<lrt::MeshTriangle> precision_box(const Vector3 &p_size) {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(p_size);
	std::vector<lrt::MeshTriangle> triangles;
	lrt::mesh_triangles(mesh, triangles);
	return triangles;
}

static lrt::TriangleMesh coarse_plane_mesh(double p_x) {
	std::vector<lrt::MeshTriangle> triangles(2);
	const lrt::Vec3 vertices[4] = { { p_x, -1, -1 }, { p_x, -1, 1 }, { p_x, 1, 1 }, { p_x, 1, -1 } };
	for (int face = 0; face < 2; face++) {
		const int indices[3] = { 0, face + 2, face + 1 };
		for (int vertex = 0; vertex < 3; vertex++) {
			triangles[size_t(face)].position[vertex] = vertices[indices[vertex]];
			triangles[size_t(face)].color[vertex] = { 1, 1, 1 };
		}
	}
	return lrt::build_triangle_mesh(std::move(triangles));
}

TEST_CASE("[LRT] Coarse segment supercover matches closed-cell intersections and affine reversal") {
	auto geometry = std::make_shared<lrt::CoarseGeometryField>();
	geometry->min = { 0.5, 0.5, 0.5 };
	geometry->cell = 1;
	geometry->size[0] = geometry->size[1] = geometry->size[2] = 4;
	geometry->cells.assign(64, 0);
	for (int index : { 0, 5, 21, 42, 47, 63 }) {
		geometry->cells[size_t(index)] = lrt::coarse_normal_mask({ -1, 0, 0 });
	}
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 1, 1, 1 }));
	auto oracle = [&](const lrt::Vec3 &p_from, const lrt::Vec3 &p_to, double &r_first) {
		bool found = false;
		r_first = 1;
		for (int index = 0; index < 64; index++) {
			if (!geometry->cells[size_t(index)]) {
				continue;
			}
			const int coordinate[3] = { index % 4, (index / 4) % 4, index / 16 };
			double low = 0, high = 1;
			for (int axis = 0; axis < 3; axis++) {
				const double delta = p_to[axis] - p_from[axis];
				if (delta == 0) {
					if (p_from[axis] < coordinate[axis] || p_from[axis] > coordinate[axis] + 1) {
						high = -1;
						break;
					}
				} else {
					const double first = (coordinate[axis] - p_from[axis]) / delta;
					const double last = (coordinate[axis] + 1 - p_from[axis]) / delta;
					low = std::max(low, std::min(first, last));
					high = std::min(high, std::max(first, last));
				}
			}
			if (low <= high) {
				found = true;
				r_first = std::min(r_first, low);
			}
		}
		return found;
	};
	for (int transform_index = 0; transform_index < 2; transform_index++) {
		lrt::PrimitiveTransform transform;
		if (transform_index) {
			transform.origin = { 3, -2, 1 };
			transform.basis_x = { 0, 2, 0 };
			transform.basis_y = { -0.5, 0, 0 };
			transform.basis_z = { 0.25, 0, 1 };
		}
		const auto primitive = lrt::make_sdf_primitive(geometry, material, transform);
		const auto grid = lrt::make_grid_sized(4, { -16, -16, -16 }, { 32, 32, 32 });
		const std::vector<lrt::GeometryInstance> instances = { primitive };
		lrt::TrunkSurfaceWorkspace workspace;
		REQUIRE(workspace.build(grid, 0, instances, { 0 }));
		for (int direction = 0; direction < lrt::DIRECTION_COUNT; direction++) {
			for (int y = 0; y <= 8; y++) {
				for (int z = 0; z <= 8; z++) {
					const int *offset = lrt::directions()[direction].offset;
					const lrt::Vec3 from(-1, y * 0.5, z * 0.5);
					const lrt::Vec3 to(5, from.y + offset[1] * 0.5, from.z + offset[2] * 0.5);
					double expected_first;
					const bool expected = oracle(from, to, expected_first);
					lrt::SurfaceSample hit;
					const bool forward = primitive.trace_segment(transform.xform(from), transform.xform(to), hit);
					CHECK(forward == expected);
					lrt::SurfaceSample composite;
					CHECK(workspace.trace_segment(transform.xform(from), transform.xform(to), composite) == expected);
					if (expected) {
						CHECK(composite.fraction == doctest::Approx(expected_first));
					}
					if (forward) {
						CHECK(hit.fraction == doctest::Approx(expected_first));
					}
					const bool reverse = primitive.trace_segment(transform.xform(to), transform.xform(from), hit);
					CHECK(reverse == forward);
					if (reverse) {
						REQUIRE(oracle(to, from, expected_first));
						CHECK(hit.fraction == doctest::Approx(expected_first));
						REQUIRE(workspace.trace_segment(transform.xform(to), transform.xform(from), composite));
						CHECK(composite.fraction == doctest::Approx(expected_first));
					}
				}
			}
		}
		lrt::SurfaceSample hit;
		CHECK(primitive.trace_segment(transform.xform({ 1, 0, 0 }), transform.xform({ 1, 0, 0 }), hit));
		CHECK(primitive.sample(transform.xform({ 1, 0, 0 })).valid);
		const double outside = std::nextafter(4.0, std::numeric_limits<double>::infinity());
		if (!transform_index) {
			CHECK_FALSE(primitive.sample({ outside, 4, 4 }).valid);
			CHECK_FALSE(primitive.trace_segment({ outside, 4, 4 }, { outside, 5, 4 }, hit));
		}
	}
}

TEST_CASE("[LRT] Resolved front surfaces hide rear emission without material extrapolation") {
	auto triangles = coarse_plane_mesh(0.1875).triangles;
	const auto rear = coarse_plane_mesh(0.4375);
	triangles.insert(triangles.end(), rear.triangles.begin(), rear.triangles.end());
	const auto mesh = lrt::build_triangle_mesh(std::move(triangles));
	const auto bake = lrt::bake_mesh_sdf(mesh, 32);
	REQUIRE(bake.error == lrt::MESH_SDF_BAKE_OK);
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(bake.field);
	lrt::MaterialCapture capture;
	capture.size[0] = 4;
	capture.size[1] = capture.size[2] = 1;
	capture.uvw_basis_x = { 1, 0, 0 };
	capture.occupied.assign(4, 1);
	capture.albedo.assign(12, 0);
	capture.emission.assign(12, 0);
	capture.emission[3] = 20;
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_mesh_instance_field(mesh, *geometry, &capture));
	const auto primitive = lrt::make_sdf_primitive(geometry, material, {});
	const auto grid = lrt::make_grid_sized(0.5, { -0.25, -0.25, -0.25 }, { 1, 1, 1 });
	const auto field = lrt::build_sdf_local_data(grid, { primitive });
	const int probe = lrt::index_of(grid, 0, 0, 0);
	REQUIRE(field.material[size_t(probe) * 4 + 3] == 0);
	CHECK(field.links[size_t(probe)] != ((1u << 26) - 1u));
	CHECK(field.local_record_indices[size_t(probe)] == lrt::LocalField::NO_RECORD);
	CHECK(field.get_local_record(size_t(probe)).reflectance == 0);
	for (float value : field.get_local_record(size_t(probe)).emission) {
		CHECK(value == 0);
	}
	const auto duplicated = lrt::build_sdf_local_data(grid, { primitive, primitive });
	CHECK(duplicated.links == field.links);
	CHECK(same_local_records(duplicated, field));
	const auto rear_bake = lrt::bake_mesh_sdf(rear, 32);
	REQUIRE(rear_bake.error == lrt::MESH_SDF_BAKE_OK);
	geometry = std::make_shared<lrt::CoarseGeometryField>(rear_bake.field);
	material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_mesh_instance_field(rear, *geometry, &capture));
	const auto exposed = lrt::build_sdf_local_data(grid, { lrt::make_sdf_primitive(geometry, material, {}) });
	REQUIRE(exposed.local_record_indices[size_t(probe)] != lrt::LocalField::NO_RECORD);
	CHECK(exposed.get_local_record(size_t(probe)).reflectance == 0);
	CHECK(exposed.get_local_record(size_t(probe)).emission[0] == doctest::Approx(20 * 9 * lrt::WEIGHT * lrt::C0));
}

TEST_CASE("[LRT] Thin mid-segment walls are symmetric across Trunks and removal restores air") {
	const auto bake = lrt::bake_mesh_sdf(coarse_plane_mesh(0), 32);
	REQUIRE(bake.error == lrt::MESH_SDF_BAKE_OK);
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(bake.field);
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 0, 0, 0 }));
	const auto primitive = lrt::make_sdf_primitive(geometry, material, {}, 321);
	const auto grid = lrt::make_grid_sized(0.5, { -4, -0.5, -0.5 }, { 8, 1, 1 });
	lrt::TrunkIndex index;
	index.update(grid, { primitive });
	lrt::LocalCache cache;
	const auto field = lrt::build_sdf_local_data(grid, { primitive }, nullptr, 4, nullptr, &cache, nullptr, &index);
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			const int left = lrt::index_of(grid, 7, y, z);
			const int right = lrt::index_of(grid, 8, y, z);
			CHECK(field.material[size_t(left) * 4 + 3] == 0);
			CHECK(field.material[size_t(right) * 4 + 3] == 0);
			CHECK((field.links[size_t(left)] & (1u << 13)) == 0);
			CHECK((field.links[size_t(right)] & (1u << 12)) == 0);
			CHECK(field.local_record_indices[size_t(left)] == lrt::LocalField::NO_RECORD);
			CHECK(field.local_record_indices[size_t(right)] == lrt::LocalField::NO_RECORD);
		}
	}
	cache.local = &field;
	index.update(grid, {});
	CHECK(index.get_update_stats().overlap_tests == 0);
	const auto removed = lrt::build_sdf_local_data(grid, {}, nullptr, 4, &cache, nullptr, nullptr, &index);
	const auto air = lrt::build_sdf_local_data(grid, {});
	CHECK(removed.dirty_trunk_count == 2);
	CHECK(removed.links == air.links);
	CHECK(same_local_records(removed, air));
	CHECK(removed.solid_count == 0);
	CHECK(removed.surface_count == 0);
}

TEST_CASE("[LRT] Coarse geometry scale follows asset bounds without automatic feature refinement") {
	const auto small = precision_box(Vector3(0.2, 0.2, 0.2));
	const auto ordinary = precision_box(Vector3(2, 2, 2));
	const auto large = precision_box(Vector3(10, 10, 10));
	const auto thin = precision_box(Vector3(2, 2, 0.1));
	CHECK(lrt::estimate_mesh_geometry_resolution(small) == 2);
	CHECK(lrt::estimate_mesh_geometry_resolution(ordinary) == 16);
	CHECK(lrt::estimate_mesh_geometry_resolution(large) == 80);
	CHECK(lrt::estimate_mesh_geometry_resolution(thin) == 16);
	const auto bake = lrt::bake_mesh_sdf(lrt::build_triangle_mesh(thin), 16);
	REQUIRE(bake.error == lrt::MESH_SDF_BAKE_OK);
	const auto material = lrt::bake_constant_instance_field({ 1, 1, 1 });
	CHECK(lrt::sample_sdf_fields(bake.field, material, {}).valid);
	// Neither small details nor repeated layers change the shared target specification.
	auto detail = ordinary;
	for (const auto &face : ordinary) {
		auto inner = face;
		for (auto &point : inner.position) {
			point = point * 0.999;
		}
		detail.push_back(inner);
	}
	CHECK(lrt::estimate_mesh_geometry_resolution(detail) == 16);
}

TEST_CASE("[LRT] Coarse geometry preserves resolved air gaps and conservatively covers subcell gaps") {
	std::vector<lrt::MeshTriangle> planes;
	for (double z : { 0.0, 0.375 }) {
		lrt::MeshTriangle triangle;
		triangle.position[0] = { -1, -1, z };
		triangle.position[1] = { 1, -1, z };
		triangle.position[2] = { 1, 1, z };
		planes.push_back(triangle);
		triangle.position[1] = { 1, 1, z };
		triangle.position[2] = { -1, 1, z };
		planes.push_back(triangle);
	}
	CHECK(lrt::estimate_mesh_geometry_resolution(planes) == 16);
	auto bake = lrt::bake_mesh_sdf(lrt::build_triangle_mesh(planes), 16);
	REQUIRE(bake.error == lrt::MESH_SDF_BAKE_OK);
	const auto material = lrt::bake_constant_instance_field({ 1, 1, 1 });
	CHECK_FALSE(lrt::sample_sdf_fields(bake.field, material, { 0, 0, 0.1875 }).valid);
	for (size_t face = 2; face < planes.size(); face++) {
		for (auto &point : planes[face].position) {
			point.z = 0.1;
		}
	}
	bake = lrt::bake_mesh_sdf(lrt::build_triangle_mesh(planes), 16);
	REQUIRE(bake.error == lrt::MESH_SDF_BAKE_OK);
	CHECK(lrt::sample_sdf_fields(bake.field, material, { 0, 0, 0.05 }).valid);
	planes.resize(2);
	bake = lrt::bake_mesh_sdf(lrt::build_triangle_mesh(planes), 16);
	REQUIRE(bake.error == lrt::MESH_SDF_BAKE_OK);
	CHECK_FALSE(lrt::sample_sdf_fields(bake.field, material, { 0, 0, -0.25 }).valid);
	CHECK_FALSE(lrt::sample_sdf_fields(bake.field, material, { 0, 0, 0.25 }).valid);
}

TEST_CASE("[LRT] Project geometry scale invalidates live estimates without a feature heuristic") {
	const String key = "rendering/global_illumination/lrt/voxel/cell_size";
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const Variant saved = settings->get_setting(key);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(2, 2, 0.1));
	LRTVolume3D *volume = memnew(LRTVolume3D);
	const int original = LRTVolume3DTestAccess::mesh_resolution(volume, mesh);
	settings->set_setting(key, 0.5);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh) == 4);
	settings->set_setting(key, 0.1);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh) == 20);
	settings->set_setting(key, saved);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh) == original);
	memdelete(volume);
}

TEST_CASE("[LRT] Per-instance voxel size overrides share mesh bounds without changing defaults") {
	const String key = "rendering/global_illumination/lrt/voxel/cell_size";
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const Variant saved = settings->get_setting(key);
	settings->set_setting(key, 0.25);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(2, 2, 0.1));
	LRTVolume3D *volume = memnew(LRTVolume3D);
	MeshInstance3D *first = memnew(MeshInstance3D);
	MeshInstance3D *second = memnew(MeshInstance3D);
	first->set_mesh(mesh);
	second->set_mesh(mesh);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, first) == 8);
	first->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.1);
	second->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.5);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, first) == 20);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, second) == 4);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, first) == 20);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh) == 8);
	first->set_scale(Vector3(3, 2, -1));
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, first) == 20);
	settings->set_setting(key, 0.2);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, first) == 20);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, second) == 4);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh) == 10);
	first->remove_meta(lrt::VOXEL_CELL_SIZE_META);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, first) == 10);
	MultiMeshInstance3D *copies = memnew(MultiMeshInstance3D);
	copies->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.05);
	CHECK(LRTVolume3DTestAccess::mesh_resolution(volume, mesh, copies) == 40);
	CHECK(lrt::geometry_cell_size(copies) == doctest::Approx(0.05));
	memdelete(copies);
	memdelete(first);
	memdelete(second);
	memdelete(volume);
	settings->set_setting(key, saved);
}

TEST_CASE("[SceneTree][LRT] Instance voxel overrides invalidate collected build specifications") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(2, 2, 0.1));
	MeshInstance3D *first = memnew(MeshInstance3D);
	first->set_mesh(mesh);
	first->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.1);
	root->add_child(first);
	MeshInstance3D *second = memnew(MeshInstance3D);
	second->set_mesh(mesh);
	second->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.5);
	root->add_child(second);
	SceneTree::get_singleton()->get_root()->add_child(root);
	REQUIRE(bool(LRTVolume3DTestAccess::seed_local_voxels(volume, 0)));
	REQUIRE(bool(LRTVolume3DTestAccess::seed_local_voxels(volume, 1)));
	std::vector<LRTVolume::MeshInstance> meshes;
	REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
	REQUIRE(meshes.size() == 2);
	CHECK(meshes[0].sdf_resolution == 20);
	CHECK(meshes[1].sdf_resolution == 4);
	const uint64_t original_signature = LRTVolume3DTestAccess::geometry_signature(volume);
	const uint64_t original_fingerprint = LRTVolume3DTestAccess::build_fingerprint(volume);
	first->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.2);
	CHECK(LRTVolume3DTestAccess::geometry_signature(volume) != original_signature);
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) != original_fingerprint);
	first->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.1);
	CHECK(LRTVolume3DTestAccess::geometry_signature(volume) == original_signature);
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) == original_fingerprint);
	memdelete(root);
}

TEST_CASE("[LRT] Voxel size overrides survive scene packing and duplication") {
	Node3D *root = memnew(Node3D);
	MeshInstance3D *mesh = memnew(MeshInstance3D);
	root->add_child(mesh);
	mesh->set_owner(root);
	mesh->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.05);
	MultiMeshInstance3D *copies = memnew(MultiMeshInstance3D);
	root->add_child(copies);
	copies->set_owner(root);
	copies->set_meta(lrt::VOXEL_CELL_SIZE_META, 0.2);
	Ref<PackedScene> scene;
	scene.instantiate();
	REQUIRE(scene->pack(root) == OK);
	Node *loaded = scene->instantiate();
	REQUIRE(loaded != nullptr);
	CHECK(lrt::geometry_cell_size(loaded->get_child(0)) == doctest::Approx(0.05));
	CHECK(lrt::geometry_cell_size(loaded->get_child(1)) == doctest::Approx(0.2));
	Node *duplicate = mesh->duplicate();
	CHECK(lrt::geometry_cell_size(duplicate) == doctest::Approx(0.05));
	memdelete(duplicate);
	memdelete(loaded);
	memdelete(root);
}

TEST_CASE("[LRT] Automatic preparation shares geometry across materials and rejects oversized fields") {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size(Vector3(0.2, 0.2, 0.2));
	const Dictionary first = LRTVolume3D::prepare_mesh_sdf(mesh);
	REQUIRE(bool(first.get("ok", false)));
	CHECK(int(first["resolution"]) == 2);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.2, 0.4, 0.6));
	mesh->set_material(material);
	const Dictionary second = LRTVolume3D::prepare_mesh_sdf(mesh);
	REQUIRE(bool(second.get("ok", false)));
	CHECK(first["signature"] == second["signature"]);
	CHECK(bool(second["loaded"]));
	const auto geometry = lrt::build_triangle_mesh(precision_box(Vector3(1000, 1000, 1000)));
	const int resolution = lrt::estimate_mesh_geometry_resolution(geometry.triangles);
	CHECK(lrt::bake_mesh_sdf(geometry, resolution).error == lrt::MESH_SDF_BAKE_TOO_LARGE);
}

TEST_CASE("[LRT] Material preview derives RGB8 reflectance and HDR emission from sparse local records") {
	lrt::LocalField field;
	field.material = {
		0, 0, 0, 0, // Empty probe.
		0, 0, 0, 1, // Occupied probe.
		0, 2, 0, 0, // Two blocked directions with reflection and emission.
		0, 1, 0, 0, // Black, non-emitting occluder without a sparse record.
		0, 1, 0, 0, // Pure emitter without reflection.
	};
	field.local_record_indices.assign(5, lrt::LocalField::NO_RECORD);
	std::array<lrt::LocalSurface, lrt::DIRECTION_COUNT> surfaces;
	surfaces[0] = { { 0.25, 0.5, 0.75 }, { 2, 0, 4 } };
	surfaces[25] = { { 0.75, 0.5, 0.25 }, { 6, 8, 0 } };
	const uint32_t all_open = (1u << lrt::DIRECTION_COUNT) - 1u;
	field.set_local_record(2, lrt::build_local_record(all_open ^ 1u ^ (1u << 25), surfaces));
	surfaces = {};
	surfaces[0].emission = { 10, 2, 5 };
	field.set_local_record(4, lrt::build_local_record(all_open ^ 1u, surfaces));
	REQUIRE(field.validate_local_records());
	CHECK(field.local_records.size() == 2);
	CHECK(field.local_record_indices[3] == lrt::LocalField::NO_RECORD);
	const std::vector<float> albedo = lrt::local_material_rgba(field, false);
	const std::vector<float> emission = lrt::local_material_rgba(field, true);
	const std::vector<float> expected_albedo = { 0, 0, 0, 0, 0, 0, 0, 0, 128.0f / 255, 128.0f / 255, 128.0f / 255, 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	const std::vector<float> expected_emission = { 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 2, 1, 0, 0, 0, 1, 10, 2, 5, 1 };
	CHECK(albedo == expected_albedo);
	REQUIRE(emission.size() == expected_emission.size());
	for (size_t i = 0; i < emission.size(); i++) {
		CHECK(emission[i] == doctest::Approx(expected_emission[i]));
	}
}

TEST_CASE("[LRT] Coarse geometry preview preserves covered native lattice cells") {
	lrt::CoarseGeometryField field;
	field.min = { -2.0, 3.0, 5.0 };
	field.cell = 0.5;
	field.size[0] = field.size[1] = field.size[2] = 2;
	field.cells = { 1, 0, 4, 0, 0, 0, 0, 8 };
	const std::vector<float> expected = {
		-2.0f, 3.0f, 5.0f, 1.0f,
		-2.0f, 3.5f, 5.0f, 1.0f,
		-1.5f, 3.5f, 5.5f, 1.0f,
	};
	CHECK(lrt::sdf_surface_cells(field) == expected);
}

#ifdef MODULE_GLSLANG_ENABLED
TEST_CASE("[LRT] Volume directional shadow projection covers receivers and off-volume casters") {
	const AABB bounds(Vector3(-2, -3, -14), Vector3(4, 6, 28));
	for (const Vector3 &direction : { Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(0.7, -0.5, 0.4).normalized(), Vector3(-0.7, 0.5, -0.4).normalized() }) {
		const auto view = LRTDirectionalShadow::make_view(bounds, direction, {}, 0.2f);
		for (int corner = 0; corner < 8; corner++) {
			const Vector3 point = bounds.get_endpoint(corner);
			const Vector4 sample = view.sample.xform(Vector4(point.x, point.y, point.z, 1.0));
			CHECK(sample.w == doctest::Approx(1.0));
			for (int axis = 0; axis < 3; axis++) {
				CHECK(sample[axis] > 0.0f);
				CHECK(sample[axis] < 1.0f);
			}
		}
		const Vector3 center = bounds.get_center();
		const Vector3 toward_sun = center - direction;
		CHECK(view.sample.xform(Vector4(toward_sun.x, toward_sun.y, toward_sun.z, 1)).z >
				view.sample.xform(Vector4(center.x, center.y, center.z, 1)).z);
	}
	const AABB caster(Vector3(-1, 20, -1), Vector3(2, 1, 2));
	const auto view = LRTDirectionalShadow::make_view(bounds, Vector3(0, -1, 0), { caster }, 0.2f);
	const Vector4 sample = view.sample.xform(Vector4(0, 20.5, 0, 1));
	CHECK(sample.z > 0.0f);
	CHECK(sample.z < 1.0f);
}

TEST_CASE("[LRT] Compact local and sky compute shaders compile without a rendering device") {
	using RD = RenderingDeviceCommons;
	String offsets;
	for (int index = 0; index < lrt::DIRECTION_COUNT; index++) {
		const lrt::Direction &direction = lrt::directions()[index];
		if (!offsets.is_empty()) {
			offsets += ", ";
		}
		offsets += vformat("ivec3(%d, %d, %d)", direction.offset[0], direction.offset[1], direction.offset[2]);
	}
	String faces;
	for (int direction : lrt::face_directions()) {
		faces += (faces.is_empty() ? "" : ", ") + itos(direction);
	}
	for (const char *source : { lrt_propagate_shader_glsl, lrt_sky_solve_shader_glsl, lrt_sky_project_shader_glsl,
			lrt_inject_shader_glsl, lrt_local_patch_shader_glsl, lrt_screen_gather_shader_glsl,
#ifdef LRT_HDDAGI_ENABLED
			lrt_boundary_shader_glsl,
#endif
		 }) {
		String error;
		const String shader = String::utf8(source).replace("%LRT_DIRECTIONS%", offsets).replace("%LRT_FACE_DIRECTIONS%", faces).replace("%LRT_BOUNDARY%", lrt_boundary_inc_shader_glsl);
		const Vector<uint8_t> spirv = compile_glslang_shader(
				RD::SHADER_STAGE_COMPUTE, shader.substr(String("#[compute]").length()),
				RD::SHADER_LANGUAGE_VULKAN_VERSION_1_0, RD::SHADER_SPIRV_VERSION_1_0, &error);
		INFO(error);
		CHECK_FALSE(spirv.is_empty());
	}
}

TEST_CASE("[LRT] Forward adapter compiles opaque and multiview composition") {
	for (bool multiview : { false, true }) {
		String shader = "#version 450\n";
		if (multiview) {
			shader += "#define USE_MULTIVIEW\n#define ViewIndex 0\n";
		}
		shader += "layout(set=0,binding=0) uniform sampler test_sampler;\n#define SAMPLER_LINEAR_CLAMP test_sampler\n";
		shader += String::utf8(lrt_forward_inc_shader_glsl);
		shader += R"(
layout(location=0) out vec4 result;
void main() {
	vec3 gi; float coverage; float sky;
	bool applied = lrt_sample_surface(vec2(0.5), gi, coverage, sky);
	vec3 color = lrt_compose_indirect(vec3(0.2), vec3(0.3), gi, coverage, sky, 0.8, vec3(0.6));
	result = vec4(lrt_is_enabled() && applied ? color : vec3(0.2), lrt_is_lighting_debug() ? 1.0 : 0.0);
}
)";
		String error;
		const Vector<uint8_t> spirv = compile_glslang_shader(RenderingDeviceCommons::SHADER_STAGE_FRAGMENT, shader,
				RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_0,
				RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_0, &error);
		INFO(error);
		CHECK_FALSE(spirv.is_empty());
	}
}

TEST_CASE("[LRT] SDF and probe debug shaders compile without a rendering device") {
	using RD = RenderingDeviceCommons;
	for (const char *source : { lrt_debug_shader_glsl, lrt_geometry_debug_shader_glsl }) {
		const String shader = String::utf8(source).replace("%LRT_BOUNDARY%", lrt_boundary_inc_shader_glsl);
		const int fragment = shader.find("#[fragment]");
		const String stages[] = {
			shader.substr(String("#[vertex]").length(), fragment - String("#[vertex]").length()),
			shader.substr(fragment + String("#[fragment]").length()),
		};
		for (int stage = 0; stage < 2; stage++) {
			String error;
			const Vector<uint8_t> spirv = compile_glslang_shader(
					stage == 0 ? RD::SHADER_STAGE_VERTEX : RD::SHADER_STAGE_FRAGMENT,
					stages[stage].replace("#VERSION_DEFINES", ""), RD::SHADER_LANGUAGE_VULKAN_VERSION_1_0,
					RD::SHADER_SPIRV_VERSION_1_0, &error);
			INFO(error);
			CHECK_FALSE(spirv.is_empty());
		}
	}
}
#endif

TEST_CASE("[SceneTree][LRT] Capture memory counts independent materials with shared or absent triangles") {
	LRTVolume3D *volume = memnew(LRTVolume3D);
	const uint64_t expected = LRTVolume3DTestAccess::add_memory_fixture(volume);
	CHECK(uint64_t(volume->get_preparation_status()["mesh_capture_active_bytes"]) == expected);
	memdelete(volume);
}

TEST_CASE("[SceneTree][LRT] Shared mesh captures follow their last Volume without a memory budget") {
	LRTVolume3D::clear_shared_mesh_capture_cache();
	LRTVolume3D *first = memnew(LRTVolume3D);
	LRTVolume3D *second = memnew(LRTVolume3D);
	const std::weak_ptr<const lrt::MaterialCapture> material = LRTVolume3DTestAccess::add_shared_capture(first, 10, 1);
	LRTVolume3DTestAccess::add_shared_capture(second, 10, 1);
	CHECK(LRTVolume3DTestAccess::shares_capture(first, second));
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 1);
	const Dictionary status = second->get_preparation_status();
	CHECK_FALSE(status.has("mesh_capture_shared_cache_budget_bytes"));
	CHECK(int64_t(status["mesh_capture_shared_cache_bytes"]) == 8);
	second->hide();
	memdelete(first);
	CHECK_FALSE(material.expired());
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 1);
	memdelete(second);
	CHECK(material.expired());
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 0);
}

TEST_CASE("[SceneTree][LRT] Replacing and clearing mesh captures preserve live revisions") {
	LRTVolume3D::clear_shared_mesh_capture_cache();
	LRTVolume3D *first = memnew(LRTVolume3D);
	LRTVolume3D *second = memnew(LRTVolume3D);
	const std::weak_ptr<const lrt::MaterialCapture> old_material = LRTVolume3DTestAccess::add_shared_capture(first, 10, 1);
	const std::weak_ptr<const lrt::MaterialCapture> current_material = LRTVolume3DTestAccess::add_shared_capture(second, 10, 2);
	CHECK_FALSE(LRTVolume3DTestAccess::shares_capture(first, second));
	CHECK_FALSE(old_material.expired());
	CHECK_FALSE(current_material.expired());
	memdelete(first);
	CHECK(old_material.expired());
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 1);
	LRTVolume3D::clear_shared_mesh_capture_cache();
	CHECK_FALSE(current_material.expired());
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 0);
	first = memnew(LRTVolume3D);
	const std::weak_ptr<const lrt::MaterialCapture> replacement = LRTVolume3DTestAccess::add_shared_capture(first, 10, 2);
	memdelete(second);
	CHECK(current_material.expired());
	CHECK_FALSE(replacement.expired());
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 1);
	memdelete(first);
	CHECK(replacement.expired());
	CHECK(LRTVolume3DTestAccess::shared_capture_entries() == 0);
}

TEST_CASE("[LRT] Sky input projects low-order radiance at exactly 26 equally weighted directions") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	PackedVector4Array coefficients;
	coefficients.push_back(Vector4(2, 3, -1, 0.5));
	coefficients.push_back(Vector4(3, -0.5, 1, 2));
	coefficients.push_back(Vector4(4, 0.2, -1.5, -0.75));
	solver->set_sky_radiance(coefficients);
	CHECK(solver->get_sky_radiance() == coefficients);
	int clipped = 0;
	for (int direction = 0; direction < lrt::DIRECTION_COUNT; direction++) {
		const lrt::Vec3 &d = lrt::directions()[direction].direction;
		const Vector4 y(0.2820947918, 0.4886025119 * d.x, 0.4886025119 * d.y, 0.4886025119 * d.z);
		const Vector4 p = Vector4(0.2820947918, (0.4886025119 / 3.0) * d.x,
				(0.4886025119 / 3.0) * d.y, (0.4886025119 / 3.0) * d.z) * (4.0 * Math::PI / 26.0);
		for (int channel = 0; channel < 3; channel++) {
			const float radiance = MAX(0.0f, coefficients[channel].dot(y));
			clipped += radiance == 0.0f;
			const Vector4 expected = p * radiance;
			const Vector4 actual = LRTVolume3DTestAccess::sky_contribution(solver, channel, direction);
			for (int component = 0; component < 4; component++) {
				CHECK(Math::abs(actual[component] - expected[component]) <= 0.0000001 + 0.000002 * Math::abs(expected[component]));
			}
		}
	}
	CHECK(clipped > 0);
	CHECK(clipped < 26 * 3);
	solver->set_sky(Vector3());
	for (int channel = 0; channel < 3; channel++) {
		for (int direction = 0; direction < lrt::DIRECTION_COUNT; direction++) {
			CHECK(LRTVolume3DTestAccess::sky_contribution(solver, channel, direction) == Vector4());
		}
	}
}

TEST_CASE("[SceneTree][LRT] Finite propagation caps every ready Volume without cost throttling") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const StringName key("rendering/global_illumination/lrt/propagation/iterations_per_frame");
	const StringName budget_key("rendering/global_illumination/lrt/propagation/iterations_after_change");
	const Variant saved_iterations = settings->get(key);
	const Variant saved_budget = settings->get(budget_key);
	CHECK(int(settings->property_get_revert(key)) == 1);
	CHECK(int(settings->property_get_revert(budget_key)) == 64);
	settings->set(key, 8);
	settings->set(budget_key, 64);
	Node3D *root = memnew(Node3D);
	LRTVolume3D *first = memnew(LRTVolume3D);
	LRTVolume3D *second = memnew(LRTVolume3D);
	first->set_rebuild_suppressed(true);
	second->set_rebuild_suppressed(true);
	root->add_child(first);
	root->add_child(second);
	SceneTree::get_singleton()->get_root()->add_child(root);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 0);
	LRTVolume3DTestAccess::prepare_solver(first, 0.0);
	LRTVolume3DTestAccess::prepare_solver(second, 20.0);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 8);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(second) == 8);
	settings->set(key, 16);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 16);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(second) == 16);
	CHECK(int(first->get_preparation_status()["propagation_iterations_per_frame"]) == 16);
	LRTVolume3DTestAccess::set_pending_iterations(first, 8);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 0);
	LRTVolume3DTestAccess::set_pending_iterations(first, 0);
	LRTVolume3DTestAccess::set_upload_state(first, true, false);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 0);
	LRTVolume3DTestAccess::set_upload_state(first, true, true);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 16);
	LRTVolume3DTestAccess::set_upload_state(first, false, false);
	first->hide();
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 0);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(second) == 16);
	first->show();
	LRTVolume3DTestAccess::set_remaining(first, 3);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 3);
	LRTVolume3DTestAccess::set_remaining(first, 0);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 0);
	settings->set(key, 4);
	LRTVolume3DTestAccess::refresh_budget(first);
	CHECK(LRTVolume3DTestAccess::remaining(first) == 0);
	settings->set(budget_key, 9);
	LRTVolume3DTestAccess::refresh_budget(first);
	CHECK(LRTVolume3DTestAccess::remaining(first) == 9);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 4);
	LRTVolume3DTestAccess::set_remaining(first, 2);
	LRTVolume3DTestAccess::change_source(first);
	LRTVolume3DTestAccess::refresh_budget(first);
	CHECK(LRTVolume3DTestAccess::remaining(first) == 9);
	LRTVolume3DTestAccess::set_remaining(first, 12);
	LRTVolume3DTestAccess::change_source(first);
	LRTVolume3DTestAccess::refresh_budget(first);
	CHECK(LRTVolume3DTestAccess::remaining(first) == 12);
	LRTVolume3DTestAccess::mark_submitted_this_frame(first);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(first) == 0);
	CHECK(LRTVolume3DTestAccess::propagation_iterations(second) == 4);
	memdelete(root);
	settings->set(key, saved_iterations);
	settings->set(budget_key, saved_budget);
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

TEST_CASE("[SceneTree][LRT] Excluded shader globals do not invalidate local voxel inputs") {
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
	CHECK(changed == initial);
	server->global_shader_parameter_set(first, 0.75);
	CHECK(generation() == changed);
	server->global_shader_parameter_set_override(first, 1.0);
	const int64_t overridden = generation();
	CHECK(overridden == changed);
	server->global_shader_parameter_set(first, 0.5);
	CHECK(generation() == overridden);
	server->global_shader_parameter_set_override(first, Variant());
	CHECK(generation() == overridden);
	shader->set_code("shader_type spatial; global uniform float lrt_test_dependency_second; void fragment() { ALBEDO = vec3(lrt_test_dependency_second); }");
	const int64_t replaced = generation();
	server->global_shader_parameter_set(first, 0.1);
	CHECK(generation() == replaced);
	server->global_shader_parameter_set(second, 0.2);
	CHECK(generation() == replaced);
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
	REQUIRE(lrt::mesh_triangles(mesh, triangles, &surfaces));
	REQUIRE(triangles.size() == 1);
	CHECK(triangles[0].position[0].x == doctest::Approx(10));
	surfaces.clear();
	CHECK_FALSE(lrt::mesh_triangles(mesh, triangles, &surfaces));
	CHECK(triangles.empty());
}

TEST_CASE("[SceneTree][LRT] Local voxel geometry uses the authored mesh instead of blend shape poses") {
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
		REQUIRE(lrt::mesh_triangles(mesh, triangles));
		CHECK(triangles[0].position[0].x == doctest::Approx(2));
		memdelete(instance);
	}
}

TEST_CASE("[SceneTree][LRT] Local voxel geometry ignores bones while native shadow signatures track them") {
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
	REQUIRE(lrt::mesh_triangles(mesh, triangles));
	CHECK(triangles[0].position[0].x == doctest::Approx(1));
	const uint64_t signature = lrt::mesh_deformation_signature(instance);
	skeleton->set_bone_pose_position(0, Vector3(4, 0, 0));
	skeleton->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
	CHECK(lrt::mesh_deformation_signature(instance) != signature);
	REQUIRE(lrt::mesh_triangles(mesh, triangles));
	CHECK(triangles[0].position[0].x == doctest::Approx(1));
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

TEST_CASE("[SceneTree][LRT] Contribution bounds ignore pose but follow authored mesh edits") {
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
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
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
	memdelete(volume);
	grid->clear();
	MessageQueue::get_singleton()->flush();
	CHECK(grid->get_render_meshes().is_empty());
	memdelete(root);
}
#endif


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

TEST_CASE("[LRT] Instance material capture handles collapsed triangle edges") {
	lrt::CoarseGeometryField geometry;
	geometry.size[0] = geometry.size[1] = geometry.size[2] = 9;
	geometry.min = lrt::Vec3(-0.25, -0.25, -0.25);
	geometry.cell = 0.25;
	lrt::MaterialCapture capture;
	capture.size[0] = capture.size[1] = capture.size[2] = 2;
	capture.uvw_basis_x = lrt::Vec3(1, 0, 0);
	capture.uvw_basis_y = lrt::Vec3(0, 1, 0);
	capture.uvw_basis_z = lrt::Vec3(0, 0, 1);
	capture.occupied.assign(8, 1);
	capture.albedo.assign(24, 0.375f);
	for (int shape = 0; shape < 3; shape++) {
		lrt::MeshTriangle triangle;
		triangle.position[0] = lrt::Vec3();
		triangle.position[1] = shape == 1 ? lrt::Vec3(0.5, 0, 0) : lrt::Vec3();
		triangle.position[2] = shape == 2 ? lrt::Vec3() : lrt::Vec3(1, 0, 0);
		const lrt::TriangleMesh mesh = lrt::build_triangle_mesh({ triangle });
		const lrt::SurfaceMaterialField field = lrt::bake_mesh_instance_field(mesh, geometry, &capture);
		REQUIRE(field.albedo.size() == 9 * 9 * 9 * 3);
		CHECK(field.cell_stride == 1);
		for (int z = 0; z < 9; z++) {
			for (int y = 0; y < 9; y++) {
				for (int x = 0; x < 9; x++) {
					const bool covered = y == 1 && z == 1 && x >= 1 && x <= (shape == 2 ? 1 : 5);
					const size_t index = size_t(x + 9 * (y + 9 * z)) * 3;
					for (int channel = 0; channel < 3; channel++) {
						CHECK(field.albedo[index + channel] == (covered ? 96 : 0));
					}
				}
			}
		}
	}
}

TEST_CASE("[LRT] Instance material sampling uses each copy's capture coordinates") {
	lrt::MeshTriangle triangle;
	triangle.position[0] = lrt::Vec3(0, 0, 0);
	triangle.position[1] = lrt::Vec3(0, 1, 0);
	triangle.position[2] = lrt::Vec3(0, 0, 1);
	const lrt::TriangleMesh mesh = lrt::build_triangle_mesh({ triangle });
	lrt::CoarseGeometryField geometry;
	geometry.size[0] = geometry.size[1] = geometry.size[2] = 2;
	geometry.cell = 1;
	lrt::MaterialCapture capture;
	capture.size[0] = 2;
	capture.size[1] = capture.size[2] = 1;
	capture.uvw_basis_x = lrt::Vec3(1, 0, 0);
	capture.uvw_basis_y = capture.uvw_basis_z = lrt::Vec3();
	capture.occupied = { 1, 1 };
	capture.albedo = { 1, 0, 0, 0, 0, 1 };
	const lrt::SurfaceMaterialField left = lrt::bake_mesh_instance_field(mesh, geometry, &capture);
	lrt::PrimitiveTransform transform;
	transform.origin.x = 1;
	const lrt::SurfaceMaterialField right = lrt::bake_mesh_instance_field(mesh, geometry, &capture, nullptr, 1, transform);
	REQUIRE(left.albedo.size() == right.albedo.size());
	REQUIRE(!left.albedo.empty());
	CHECK(left.albedo[0] == 255);
	CHECK(left.albedo[2] == 0);
	CHECK(right.albedo[0] == 0);
	CHECK(right.albedo[2] == 255);
}


TEST_CASE("[SceneTree][LRT] Static and dynamic meshes retain local voxels through transforms and reentry") {
	for (GeometryInstance3D::GIMode mode : { GeometryInstance3D::GI_MODE_STATIC, GeometryInstance3D::GI_MODE_DYNAMIC }) {
		Node3D *root = memnew(Node3D);
		LRTVolume3D *volume = memnew(LRTVolume3D);
		volume->set_rebuild_suppressed(true);
		volume->set_size(Vector3(20, 20, 20));
		root->add_child(volume);
		Ref<BoxMesh> box;
		box.instantiate();
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, box->surface_get_arrays(0));
		MeshInstance3D *instance = memnew(MeshInstance3D);
		instance->set_mesh(mesh);
		instance->set_gi_mode(mode);
		root->add_child(instance);
		SceneTree::get_singleton()->get_root()->add_child(root);
		const auto capture = LRTVolume3DTestAccess::seed_local_voxels(volume);
		REQUIRE(bool(capture));
		std::vector<LRTVolume::MeshInstance> meshes;
		for (int step = 0; step < 4; step++) {
			const Basis basis = Basis::from_euler(Vector3(0.1 * step, 0.4 * step, 0.2)).scaled_local(Vector3(1.0 + step, 0.5, -1.0));
			instance->set_transform(Transform3D(basis, Vector3(step, 0.2, 0)));
			REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
			REQUIRE(meshes.size() == 1);
			CHECK(meshes[0].raster_capture.get() == capture.get());
			CHECK(meshes[0].transform.origin.x == doctest::Approx(step));
			CHECK(meshes[0].transform.basis_x.x == doctest::Approx(basis.get_column(0).x));
			CHECK(int64_t(volume->get_preparation_status()["voxel_captures"]) == 0);
		}
		instance->set_position(Vector3(100, 0, 0));
		REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
		CHECK(meshes.empty());
		instance->set_position(Vector3());
		instance->hide();
		REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
		CHECK(meshes.empty());
		instance->show();
		REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
		REQUIRE(meshes.size() == 1);
		CHECK(meshes[0].raster_capture.get() == capture.get());
		CHECK(int64_t(volume->get_preparation_status()["voxel_captures"]) == 0);
		memdelete(root);
	}
}

TEST_CASE("[SceneTree][LRT] Excluded shader surfaces do not invalidate supported local voxels") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<BoxMesh> box;
	box.instantiate();
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, box->surface_get_arrays(0));
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, box->surface_get_arrays(0));
	Ref<StandardMaterial3D> standard;
	standard.instantiate();
	mesh->surface_set_material(0, standard);
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code("shader_type spatial; uniform float amount = 1.0; void fragment() { ALBEDO = vec3(amount); }");
	Ref<ShaderMaterial> custom;
	custom.instantiate();
	custom->set_shader(shader);
	mesh->surface_set_material(1, custom);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_DYNAMIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	const auto capture = LRTVolume3DTestAccess::seed_local_voxels(volume);
	REQUIRE(bool(capture));
	std::vector<LRTVolume::MeshInstance> meshes;
	for (float value : { 0.2f, 0.5f, 1.0f }) {
		custom->set_shader_parameter("amount", value);
		REQUIRE(LRTVolume3DTestAccess::build_geometry_inputs(volume, meshes));
		REQUIRE(meshes.size() == 1);
		CHECK(meshes.front().raster_capture.get() == capture.get());
		CHECK(int64_t(volume->get_preparation_status()["voxel_captures"]) == 0);
	}
	memdelete(root);
}

TEST_CASE("[LRT] Instanced raster copies reuse one local voxel field under affine transforms") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(0.5, Vector3(-4, -4, -4), Vector3(8, 8, 8));
	auto capture = std::make_shared<lrt::RasterGeometryCapture>();
	capture->geometry = lrt::bake_box_sdf({ 1, 1, 1 }, 4);
	capture->material = lrt::bake_constant_instance_field({ 1, 1, 1 });
	auto copies = std::make_shared<std::vector<lrt::MeshCopy>>();
	lrt::PrimitiveTransform first;
	first.origin = { -2, 0, 0 };
	lrt::PrimitiveTransform second;
	second.origin = { 2, 0, 0 };
	second.basis_x = { 0, 2, 0 };
	second.basis_y = { -0.5, 0, 0 };
	second.basis_z = { 0, 0, -1 };
	copies->push_back({ first, nullptr });
	copies->push_back({ second, nullptr });
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
	REQUIRE(lrt::mesh_triangles(mesh, *triangles));
	LRTVolume::MeshInstance input;
	input.triangles = triangles;
	input.raster_capture = capture;
	input.raster_geometry = true;
	input.instanced = true;
	input.copies = copies;
	solver->set_mesh_instances({ input });
	const auto first_build = solver->bake_local_field_data(false);
	REQUIRE(first_build.ok);
	CHECK(first_build.mesh_volumes == 2);
	const auto first_material = LRTVolume3DTestAccess::staged_local(solver).material;
	copies->front().transform.origin = { 0, 2, 0 };
	solver->set_mesh_instances({ input });
	const auto moved_build = solver->bake_local_field_data(false);
	REQUIRE(moved_build.ok);
	CHECK(moved_build.mesh_volumes == 2);
	CHECK(LRTVolume3DTestAccess::staged_local(solver).material != first_material);
	CHECK(input.raster_capture.get() == capture.get());
}

TEST_CASE("[SceneTree][LRT] Local voxel contributors reject ShaderMaterial and exclude particles") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code("shader_type spatial; void fragment() { ALBEDO = vec3(1.0); }");
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_material_override(material);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	GPUParticles3D *particles = memnew(GPUParticles3D);
	particles->set_draw_pass_mesh(0, mesh);
	particles->set_gi_mode(GeometryInstance3D::GI_MODE_DYNAMIC);
	root->add_child(particles);
	SceneTree::get_singleton()->get_root()->add_child(root);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 0);
	CHECK(volume->get_instance_sdf_message(instance).contains("BaseMaterial3D"));
	CHECK(volume->get_instance_sdf_message(particles).contains("do not contribute"));
	CHECK(volume->get_instance_sdf_status(particles) == "Not Contributing");
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
	CHECK(volume->get_instance_sdf_message(instance).contains("Receive-only passes"));
	Ref<StandardMaterial3D> opaque;
	opaque.instantiate();
	material->set_next_pass(opaque);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	CHECK(volume->get_instance_sdf_message(instance).contains("mesh-local voxels"));
	material->set_next_pass(Ref<Material>());
	instance->set_material_overlay(opaque);
	volume->rebuild();
	CHECK(int(volume->get_collection_stats()["contributors"]) == 1);
	CHECK(volume->get_instance_sdf_message(instance).contains("mesh-local voxels"));
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
	memdelete(root);
}


#ifdef TOOLS_ENABLED
TEST_CASE("[SceneTree][LRT] Local voxel fields preserve relative geometry and cache identity under joint motion") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	volume->set_size(Vector3(4, 4, 4));
	root->add_child(volume);
	Ref<BoxMesh> source;
	source.instantiate();
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, source->surface_get_arrays(0));
	MeshInstance3D *instance = memnew(MeshInstance3D);
	instance->set_mesh(mesh);
	instance->set_gi_mode(GeometryInstance3D::GI_MODE_STATIC);
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	const uint64_t geometry_before = LRTVolume3DTestAccess::geometry_signature(volume);
	const uint64_t fingerprint_before = LRTVolume3DTestAccess::build_fingerprint(volume);
	REQUIRE(fingerprint_before != 0);
	root->set_rotation(Vector3(0, 0.6, 0));
	CHECK(LRTVolume3DTestAccess::geometry_signature(volume) == geometry_before);
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) == fingerprint_before);
	const uint64_t rotated_geometry = LRTVolume3DTestAccess::geometry_signature(volume);
	const uint64_t rotated_fingerprint = LRTVolume3DTestAccess::build_fingerprint(volume);
	root->set_position(Vector3(1, 0, 0));
	CHECK(LRTVolume3DTestAccess::geometry_signature(volume) == rotated_geometry);
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) == rotated_fingerprint);
	memdelete(root);
}

TEST_CASE("[SceneTree][LRT] Persistent build keys remain mesh-local for standard triplanar inputs") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	volume->set_size(Vector3(2, 2, 2));
	volume->set_probe_spacing(1.0);
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
	root->add_child(instance);
	SceneTree::get_singleton()->get_root()->add_child(root);
	const uint64_t world_before = LRTVolume3DTestAccess::build_fingerprint(volume);
	CHECK(world_before != 0);
	root->set_position(Vector3(0.5, 0, 0));
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) == world_before);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR, false);
	const uint64_t local_before = LRTVolume3DTestAccess::build_fingerprint(volume);
	root->set_position(Vector3(1, 0, 0));
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) == local_before);
	material->set_flag(BaseMaterial3D::FLAG_UV2_USE_TRIPLANAR, true);
	material->set_flag(BaseMaterial3D::FLAG_UV2_USE_WORLD_TRIPLANAR, true);
	const uint64_t uv2_before = LRTVolume3DTestAccess::build_fingerprint(volume);
	root->set_position(Vector3(1.5, 0, 0));
	CHECK(LRTVolume3DTestAccess::build_fingerprint(volume) == uv2_before);
	memdelete(root);
}
#endif

static lrt::MaterialCapture raster_material(const int *p_size) {
	lrt::MaterialCapture material;
	int count = 1;
	for (int axis = 0; axis < 3; axis++) {
		material.size[axis] = (p_size[axis] + 1) / 2;
		count *= material.size[axis];
	}
	material.albedo.assign(size_t(count) * 3, 1.0f);
	return material;
}

TEST_CASE("[LRT] Coverage filter removes plane extrapolation but preserves boundary contact and alpha holes") {
	const int size[3] = { 3, 3, 3 };
	lrt::MeshTriangle triangle;
	triangle.position[0] = { 0.01, 0.01, 0.214 };
	triangle.position[1] = { 0.10, 0.01, 0.286 };
	triangle.position[2] = { 0.01, 0.10, 0.268 };
	std::vector<uint32_t> surface(27, 0);
	surface[0] = 17;
	surface[9] = 17; // The old infinite-plane depth sweep also writes z = 1.
	CHECK(lrt::filter_raster_geometry_coverage({ triangle }, {}, size, surface) == 1);
	CHECK(surface[0] == 17);
	CHECK(surface[9] == 0);
	for (auto &vertex : triangle.position) {
		vertex.z = 1.0;
	}
	surface.assign(27, 0);
	surface[0] = 17;
	surface[9] = 17;
	CHECK(lrt::filter_raster_geometry_coverage({ triangle }, {}, size, surface) == 0);
	CHECK(surface[0] == 17);
	CHECK(surface[9] == 17);
	surface[9] = 0; // Never invent occupancy discarded by the GPU material.
	CHECK(lrt::filter_raster_geometry_coverage({ triangle }, {}, size, surface) == 0);
	CHECK(surface[9] == 0);
}

TEST_CASE("[LRT] Dominant projection coverage filtering matches exhaustive triangle box SAT") {
	const int size[3] = { 8, 8, 8 };
	uint32_t random = 12345;
	for (int iteration = 0; iteration < 40; iteration++) {
		lrt::MeshTriangle triangle;
		for (auto &vertex : triangle.position) {
			for (int axis = 0; axis < 3; axis++) {
				random = random * 1664525u + 1013904223u;
				vertex[axis] = double((random >> 16) % 81) / 8.0 - 1.0;
			}
		}
		lrt::PrimitiveTransform transform;
		if (iteration & 1) {
			transform.origin = { 7, 0, 0 };
			transform.basis_x = { -1, 0, 0 };
			transform.basis_y = { 0, 2, 0 };
			transform.basis_z = { 0, 0, 0.5 };
		}
		Vector3 vertices[3];
		for (int vertex = 0; vertex < 3; vertex++) {
			const lrt::Vec3 value = transform.xform(triangle.position[vertex]);
			vertices[vertex] = Vector3(value.x, value.y, value.z);
		}
		std::vector<uint32_t> surface(512, 17);
		for (size_t index = 0; index < surface.size(); index += 11) {
			surface[index] = 0;
		}
		lrt::filter_raster_geometry_coverage({ triangle }, transform, size, surface);
		for (int z = 0; z < 8; z++) {
			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 8; x++) {
					const int index = x + 8 * (y + 8 * z);
					const bool expected = index % 11 != 0 &&
							Geometry3D::triangle_box_overlap(Vector3(x + 0.5, y + 0.5, z + 0.5), Vector3(0.5, 0.5, 0.5), vertices);
					CHECK(surface[size_t(index)] == (expected ? 17u : 0u));
				}
			}
		}
	}
}

TEST_CASE("[LRT] Immutable raster captures reuse material identities across affine instances") {
	const int size[3] = { 2, 2, 2 };
	lrt::MaterialCapture input;
	input.size[0] = input.size[1] = input.size[2] = 2;
	input.albedo.assign(24, 0.3f);
	input.emission.assign(24, 2.0f);
	const std::vector<uint32_t> surface(8, lrt::coarse_normal_mask({ 1, 0, 0 }));
	auto capture = std::make_shared<lrt::RasterGeometryCapture>();
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, input, {}, *capture));
	CHECK(capture->material_signature == lrt::instance_field_signature(capture->material));
	CHECK(capture->material_signatures.reflectance == lrt::material_component_signature(capture->material, false));
	CHECK(capture->material_signatures.emission == lrt::material_component_signature(capture->material, true));
	std::shared_ptr<const lrt::CoarseGeometryField> geometry(capture, &capture->geometry);
	std::shared_ptr<const lrt::SurfaceMaterialField> material(capture, &capture->material);
	for (int index = 0; index < 8; index++) {
		lrt::PrimitiveTransform transform;
		transform.origin = { double(index), 0, 0 };
		transform.basis_x = { -1.5, 0.2, 0 };
		const auto full = lrt::make_sdf_primitive(geometry, material, transform, 3, 1, capture->signature, capture->material_signature);
		const auto cached = lrt::make_sdf_primitive(geometry, material, transform, 3, 1, capture->signature,
				capture->material_signature, &capture->material_signatures);
		CHECK(cached.input_signatures.geometry == full.input_signatures.geometry);
		CHECK(cached.input_signatures.reflectance == full.input_signatures.reflectance);
		CHECK(cached.input_signatures.emission == full.input_signatures.emission);
	}
	input.emission.assign(24, 4.0f);
	lrt::RasterGeometryCapture changed;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, input, {}, changed));
	CHECK(changed.material_signatures.reflectance == capture->material_signatures.reflectance);
	CHECK(changed.material_signatures.emission != capture->material_signatures.emission);
	CHECK(changed.material_signature != capture->material_signature);
}

TEST_CASE("[LRT] Unified native capture keeps coverage and material cells aligned") {
	LRTVolume3D *volume = memnew(LRTVolume3D);
	const Vector3i size(3, 2, 2);
	const Vector3i render_size = size * 2;
	const int count = render_size.x * render_size.y * render_size.z;
	PackedByteArray albedo;
	PackedByteArray emission;
	PackedByteArray aniso;
	PackedByteArray normals;
	albedo.resize(count * 2);
	emission.resize(count * 4);
	aniso.resize(count * 4);
	normals.resize(render_size.x * render_size.y * render_size.z * 4);
	albedo.fill(0);
	emission.fill(0);
	aniso.fill(0);
	normals.fill(0);
	for (int z = 0; z < render_size.z; z++) {
		for (int y = 0; y < render_size.y; y++) {
			const int white = render_size.x * (y + render_size.y * z);
			encode_uint16(65535, albedo.ptrw() + white * 2);
			const int rear = 1 + render_size.x * (y + render_size.y * z);
			encode_uint32(Color(10, 0, 0).to_rgbe9995(), emission.ptrw() + rear * 4);
			encode_uint32(31u | (2u << 30), aniso.ptrw() + rear * 4);
		}
	}
	for (int z = 0; z < render_size.z; z++) {
		for (int y = 0; y < render_size.y; y++) {
			for (int x : { 0, 1 }) {
				const int index = x + render_size.x * (y + render_size.y * z);
				encode_uint32((1u << 1) | (1u << 7), normals.ptrw() + index * 4);
			}
		}
	}
	Dictionary images;
	images["size"] = size;
	images["material_size"] = render_size;
	images["albedo_directions"] = 1;
	images["normal_size"] = render_size;
	images["bounds"] = AABB(Vector3(), Vector3(render_size));
	images["transform"] = Transform3D();
	images["albedo"] = albedo;
	images["emission"] = emission;
	images["emission_aniso"] = aniso;
	const int ghost = 4 + render_size.x * (1 + render_size.y);
	encode_uint32((1u << 1) | (1u << 7), normals.ptrw() + ghost * 4);
	encode_uint16(65535, albedo.ptrw() + ghost * 2);
	encode_uint32(Color(10, 0, 0).to_rgbe9995(), emission.ptrw() + ghost * 4);
	encode_uint32(31u, aniso.ptrw() + ghost * 4);
	images["normal_bits"] = normals;
	images["albedo"] = albedo;
	images["emission"] = emission;
	images["emission_aniso"] = aniso;
	const uint64_t key = 0x7465737400000000ULL | (OS::get_singleton()->get_ticks_usec() & 0xffffffffULL);
	const String path = lrt::static_voxel_capture_path(key);
	REQUIRE_FALSE(FileAccess::exists(path));
	REQUIRE(lrt::store_static_voxel_capture(key, images));
	Dictionary loaded;
	REQUIRE(lrt::load_static_voxel_capture(key, loaded));
	for (const Variant &name : images.keys()) {
		CHECK(bool(loaded[name] == images[name]));
	}
	CHECK(DirAccess::remove_absolute(path) == OK);
	images = loaded;
	LRTVolume::MeshInstance input;
	input.raster_geometry = true;
	// A plane at x = 1 touches both captured layers, but not the ghost at x = 4.
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(2);
	(*triangles)[0].position[0] = { 1, 0, 0 };
	(*triangles)[0].position[1] = { 1, double(render_size.y), 0 };
	(*triangles)[0].position[2] = { 1, double(render_size.y), double(render_size.z) };
	(*triangles)[1].position[0] = { 1, 0, 0 };
	(*triangles)[1].position[1] = { 1, double(render_size.y), double(render_size.z) };
	(*triangles)[1].position[2] = { 1, 0, double(render_size.z) };
	input.triangles = triangles;
	String error;
	REQUIRE(LRTVolume3DTestAccess::decode_capture(volume, images, size, input, error));
	CHECK(error.is_empty());
	REQUIRE(bool(input.raster_capture));
	const auto &capture = *input.raster_capture;
	CHECK(capture.material.cell_stride == 1);
	CHECK(capture.geometry.surface_voxels == 2 * render_size.y * render_size.z);
	CHECK(bool(images["coverage_filtered"]));
	CHECK(int64_t(images["coverage_removed"]) == 1);
	CHECK(input.material->occupied[size_t(ghost)] == 0);
	CHECK(input.material->albedo[size_t(ghost) * 3] == 0.0f);
	CHECK(input.material->emission[size_t(ghost) * 3] == 0.0f);
	images["coverage_filter_usec"] = int64_t(777);
	REQUIRE(lrt::store_static_voxel_capture(key, images));
	Dictionary filtered_cache;
	REQUIRE(lrt::load_static_voxel_capture(key, filtered_cache));
	LRTVolume::MeshInstance warm_input;
	warm_input.raster_geometry = true;
	warm_input.triangles = triangles;
	REQUIRE(LRTVolume3DTestAccess::decode_capture(volume, filtered_cache, size, warm_input, error));
	CHECK(int64_t(filtered_cache["coverage_filter_usec"]) == 777);
	CHECK(warm_input.raster_capture->geometry.cells == capture.geometry.cells);
	CHECK(DirAccess::remove_absolute(path) == OK);
	for (int z = 0; z < render_size.z; z++) {
		for (int y = 0; y < render_size.y; y++) {
			const auto front = lrt::sample_sdf_fields(capture.geometry, capture.material, { 0.5, y + 0.5, z + 0.5 });
			REQUIRE(front.valid);
			CHECK(front.color.x == 1);
			CHECK(front.color.y == 1);
			CHECK(front.color.z == 1);
			CHECK(front.emission.x == 0);
			const auto rear = lrt::sample_sdf_fields(capture.geometry, capture.material, { 1.5, y + 0.5, z + 0.5 });
			REQUIRE(rear.valid);
			CHECK(rear.color.x == 0);
			CHECK(rear.emission.x == doctest::Approx(10));
			CHECK_FALSE(lrt::sample_sdf_fields(capture.geometry, capture.material, { 2.5, y + 0.5, z + 0.5 }).valid);
		}
	}
	memdelete(volume);
}

TEST_CASE("[LRT] Raster geometry preserves only captured coverage and normal bins") {
	const int size[3] = { 7, 6, 5 };
	std::vector<uint32_t> surface(210, 0);
	surface[1 + 7 * (2 + 6)] = lrt::coarse_normal_mask({ -1, 0, 0 });
	surface[5 + 7 * (1 + 6 * 3)] = lrt::coarse_normal_mask({ 0, 1, 0 });
	surface[3 + 7 * (4 + 6 * 2)] = lrt::coarse_normal_mask({ 0, 0, 1 });
	const auto material = raster_material(size);
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, capture, nullptr, 2));
	CHECK(capture.geometry.surface_voxels == 3);
	CHECK(capture.geometry.cells == surface);
	CHECK(capture.material.cell_stride == 2);
	lrt::RasterGeometryCapture serial;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, serial));
	CHECK(serial.geometry.cells == capture.geometry.cells);
	CHECK(serial.signature == capture.signature);
	surface[1 + 7 * (2 + 6)] |= lrt::coarse_normal_mask({ 1, 0, 0 });
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, serial));
	CHECK(serial.signature != capture.signature);
}

TEST_CASE("[LRT] Raster room air is never flood-filled and fully discarded surfaces are empty") {
	const int size[3] = { 7, 7, 7 };
	std::vector<uint32_t> surface(343, 0);
	for (int z = 1; z <= 5; z++) {
		for (int y = 1; y <= 5; y++) {
			for (int x = 1; x <= 5; x++) {
				if (x == 1 || x == 5 || y == 1 || y == 5 || z == 1 || z == 5) {
					surface[size_t(x + 7 * (y + 7 * z))] = 1;
				}
			}
		}
	}
	const auto material = raster_material(size);
	lrt::RasterGeometryCapture closed;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, closed));
	const int center = 3 + 7 * (3 + 7 * 3);
	CHECK(closed.geometry.cells[size_t(center)] == 0);
	CHECK_FALSE(lrt::sample_sdf_fields(closed.geometry, closed.material, { 3.5, 3.5, 3.5 }).valid);
	surface[3 + 7 * (3 + 7)] = 0;
	lrt::RasterGeometryCapture cutout;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, cutout));
	CHECK(cutout.geometry.cells[size_t(center)] == 0);
	CHECK(cutout.signature != closed.signature);
	std::fill(surface.begin(), surface.end(), 0);
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, {}, cutout));
	CHECK(cutout.geometry.cells.empty());
	CHECK(cutout.material.albedo.empty());
	CHECK(cutout.geometry.surface_voxels == 0);
}

TEST_CASE("[LRT] Raster capture rejects invalid layouts and preserves output on cancellation") {
	const int size[3] = { 4, 4, 4 };
	std::vector<uint32_t> surface(64, 0);
	surface[21] = 1;
	const auto material = raster_material(size);
	lrt::RasterGeometryCapture capture;
	capture.signature = 123;
	std::atomic<bool> cancel(true);
	CHECK_FALSE(lrt::bake_raster_geometry_capture(surface, size, material, {}, capture, &cancel));
	CHECK(capture.signature == 123);
	surface.pop_back();
	CHECK_FALSE(lrt::bake_raster_geometry_capture(surface, size, material, {}, capture));
	CHECK(capture.signature == 123);
	const int invalid_size[3] = { 0, 4, 4 };
	CHECK_FALSE(lrt::bake_raster_geometry_capture({}, invalid_size, {}, {}, capture));
}

TEST_CASE("[LRT] Raster materials retain capture cells without nearest-surface extrapolation") {
	const int size[3] = { 8, 8, 8 };
	std::vector<uint32_t> surface(512, 0);
	surface[1 + 8 * (1 + 8)] = 1;
	surface[6 + 8 * (6 + 8 * 6)] = 1;
	lrt::PrimitiveTransform transform;
	transform.origin = { 10, 20, 30 };
	transform.basis_x = { 2, 0, 0 };
	transform.basis_y = { 0, 3, 0 };
	transform.basis_z = { 0, 0, 4 };
	auto material = raster_material(size);
	std::fill(material.albedo.begin(), material.albedo.end(), 0.0f);
	material.emission.assign(64 * 3, 0.0f);
	material.albedo[0] = 1;
	material.albedo.back() = 1;
	material.emission[0] = 5;
	material.emission.back() = 3;
	material.emission_sides.assign(64, lrt::EMISSION_FRONT);
	material.emission_sides.front() = lrt::EMISSION_BACK;
	material.emission_sides.back() = lrt::EMISSION_BOTH;
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, material, transform, capture));
	CHECK(capture.material.albedo.front() == 255);
	CHECK(capture.material.albedo.back() == 255);
	CHECK(capture.material.emission.front() == doctest::Approx(5));
	CHECK(capture.material.emission.back() == doctest::Approx(3));
	CHECK(capture.material.emission_sides == material.emission_sides);
	CHECK(lrt::sample_sdf_fields(capture.geometry, capture.material, { 1.5, 1.5, 1.5 }).color.x == 1.0);
	CHECK(lrt::sample_sdf_fields(capture.geometry, capture.material, { 6.5, 6.5, 6.5 }).color.z == 1.0);
	CHECK_FALSE(lrt::sample_sdf_fields(capture.geometry, capture.material, { 2.5, 1.5, 1.5 }).valid);
}

TEST_CASE("[LRT] Coarse point and segment sampling preserves small invertible voxel transforms") {
	const int size[3] = { 8, 8, 8 };
	std::vector<uint32_t> surface(512, 0);
	surface[1 + 8 * (1 + 8)] = lrt::coarse_normal_mask({ -1, 0, 0 });
	lrt::RasterGeometryCapture capture;
	REQUIRE(lrt::bake_raster_geometry_capture(surface, size, raster_material(size), {}, capture));
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(capture.geometry);
	auto material = std::make_shared<lrt::SurfaceMaterialField>(capture.material);
	const lrt::SurfaceSample reference = lrt::sample_sdf_fields(*geometry, *material, lrt::Vec3(1.5, 1.5, 1.5));
	REQUIRE(reference.valid);
	for (double scale : { 1.0, 0.01, 0.001, 0.0001, -0.001 }) {
		lrt::PrimitiveTransform transform;
		transform.basis_x = lrt::Vec3(scale, 0, 0);
		transform.basis_y = lrt::Vec3(0, std::abs(scale), 0);
		transform.basis_z = lrt::Vec3(0, 0, std::abs(scale));
		const lrt::GeometryInstance primitive = lrt::make_sdf_primitive(geometry, material, transform, 1, 1, 1, 1);
		const lrt::SurfaceSample sample = primitive.sample(transform.xform(lrt::Vec3(1.5, 1.5, 1.5)));
		CHECK(sample.valid);
		lrt::SurfaceSample hit;
		const lrt::Vec3 from = transform.xform({ 0, 1.5, 1.5 });
		const lrt::Vec3 to = transform.xform({ 3, 1.5, 1.5 });
		REQUIRE(primitive.trace_segment(from, to, hit));
		CHECK(hit.fraction == doctest::Approx(1.0 / 3.0));
		REQUIRE(primitive.trace_segment(to, from, hit));
		CHECK(hit.fraction == doctest::Approx(1.0 / 3.0));
		CHECK(sample.normal.x == doctest::Approx(reference.normal.x * (scale > 0 ? 1.0 : -1.0)));
		CHECK(sample.normal.y == doctest::Approx(reference.normal.y));
		CHECK(sample.normal.z == doctest::Approx(reference.normal.z));
	}
	lrt::PrimitiveTransform singular;
	singular.basis_x = lrt::Vec3();
	const lrt::GeometryInstance primitive = lrt::make_sdf_primitive(geometry, material, singular, 1, 1, 1, 1);
	CHECK_FALSE(primitive.sample(lrt::Vec3()).valid);
}

TEST_CASE("[LRT] Thin closed mesh shares conservative coverage across affine transforms") {
	const lrt::Vec3 vertices[8] = {
		{ 1.6812, -2, -11 }, { 1.8312, -2, -11 }, { 1.8312, 2, -11 }, { 1.6812, 2, -11 },
		{ 1.6812, -2, 11 }, { 1.8312, -2, 11 }, { 1.8312, 2, 11 }, { 1.6812, 2, 11 }
	};
	const int faces[12][3] = {
		{ 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 },
		{ 0, 1, 5 }, { 0, 5, 4 }, { 3, 7, 6 }, { 3, 6, 2 },
		{ 0, 4, 7 }, { 0, 7, 3 }, { 1, 2, 6 }, { 1, 6, 5 }
	};
	std::vector<lrt::MeshTriangle> triangles(12);
	for (int face = 0; face < 12; face++) {
		for (int vertex = 0; vertex < 3; vertex++) {
			triangles[face].position[vertex] = vertices[faces[face][vertex]];
		}
	}
	auto mesh = std::make_shared<lrt::TriangleMesh>(lrt::build_triangle_mesh(triangles));
	REQUIRE(mesh->has_closed_shell);
	auto baked = lrt::bake_mesh_sdf(*mesh, 128);
	REQUIRE(baked.error == lrt::MESH_SDF_BAKE_OK);
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(std::move(baked.field));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 1, 1, 1 }));
	const lrt::Vec3 inside(1.7, 0, 0);
	const lrt::SurfaceSample cached_sample = lrt::sample_sdf_fields(*geometry, *material, inside);
	REQUIRE(cached_sample.valid);
	for (double scale : { 1.0, 2.0, -2.0 }) {
		lrt::PrimitiveTransform transform;
		transform.origin = { 3, 2, 1 };
		transform.basis_x = { scale, 0, 0 };
		transform.basis_y = { 0, 3, 0 };
		transform.basis_z = { 0, 0, 0.5 };
		lrt::GeometryInstance primitive = lrt::make_sdf_primitive(geometry, material, transform);
		const lrt::SurfaceSample sample = primitive.sample(transform.xform(inside));
		CHECK(sample.valid);
		CHECK(std::abs(sample.normal.x) == doctest::Approx(1.0));
		CHECK_FALSE(primitive.sample(transform.xform({ 1.5, 0, 0 })).valid);
		CHECK_FALSE(primitive.sample(transform.xform({ 2.0, 0, 0 })).valid);
	}
}

TEST_CASE("[LRT] Slanted mid-segment surfaces block both directions without occupying endpoints") {
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
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(std::move(baked.field));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field(lrt::Vec3(1, 1, 1)));
	lrt::GeometryInstance primitive = lrt::make_sdf_primitive(geometry, material, {}, 1, 1, 1, 1);
	REQUIRE_FALSE(primitive.sample({ -0.25, -0.25, 0 }).valid);
	REQUIRE_FALSE(primitive.sample({ 0.25, 0.25, 0 }).valid);
	const lrt::Grid grid = lrt::make_grid_sized(0.5, lrt::Vec3(-0.5, -0.5, -0.25), lrt::Vec3(1.5, 1.5, 0.5));
	for (int copies : { 1, 2 }) {
		std::vector<lrt::GeometryInstance> primitives(copies, primitive);
		const lrt::LocalField field = lrt::build_sdf_local_data(grid, primitives);
		const int index = lrt::index_of(grid, 0, 0, 0);
		CHECK((field.links[index] & (1u << 16)) == 0);
		CHECK((field.links[lrt::index_of(grid, 1, 1, 0)] & (1u << 9)) == 0);
		CHECK(field.material[index * 4 + 1] > 0);
	}
}

static void check_workspace_surface(const lrt::SurfaceSample &p_actual, const lrt::SurfaceSample &p_expected) {
	CHECK(p_actual.valid == p_expected.valid);
	CHECK(p_actual.fraction == p_expected.fraction);
	CHECK(p_actual.layer_mask == p_expected.layer_mask);
	for (int axis = 0; axis < 3; axis++) {
		CHECK(p_actual.position[axis] == p_expected.position[axis]);
		CHECK(p_actual.normal[axis] == p_expected.normal[axis]);
		CHECK(p_actual.color[axis] == p_expected.color[axis]);
		CHECK(p_actual.emission[axis] == p_expected.emission[axis]);
	}
}

TEST_CASE("[LRT] Trunk grid queries preserve affine point and segment sampling") {
	auto geometry = std::make_shared<lrt::CoarseGeometryField>();
	auto material = std::make_shared<lrt::SurfaceMaterialField>();
	geometry->min = lrt::Vec3(-0.25, -0.25, -0.25);
	geometry->cell = 0.125;
	for (int axis = 0; axis < 3; axis++) {
		geometry->size[axis] = 5;
		material->color_size[axis] = 5;
	}
	geometry->cells.resize(125);
	material->albedo.resize(375);
	material->emission.resize(375);
	for (int i = 0; i < 125; i++) {
		geometry->cells[size_t(i)] = i % 3 == 0 ? 0 : (1u << (i % 26));
		for (int channel = 0; channel < 3; channel++) {
			material->albedo[size_t(i * 3 + channel)] = uint8_t(i * 37 + channel);
			material->emission[size_t(i * 3 + channel)] = float((i + channel) % 7);
		}
	}
	const lrt::Grid grid = lrt::make_grid_sized(0.5, { -2.5, -2.5, -2.5 }, { 5, 5, 5 });
	for (int variant = 0; variant < 4; variant++) {
		lrt::PrimitiveTransform transform;
		transform.origin = { -0.03125, 0.0625, -0.125 };
		if (variant == 1) {
			transform.basis_x = { 0, 2, 0 };
			transform.basis_y = { -0.5, 0, 0 };
			transform.basis_z = { 0.25, 0, 1 };
		} else if (variant == 2) {
			transform.basis_x = { -1, 0.25, 0 };
			transform.basis_y = { 0, 1, 0.125 };
			transform.basis_z = { 0.25, 0, 2 };
		} else if (variant == 3) {
			transform.basis_x = { -0.0001, 0, 0 };
			transform.basis_y = { 0, 0.0001, 0 };
			transform.basis_z = { 0, 0, 0.0001 };
		}
		const std::vector<lrt::GeometryInstance> instances = {
			lrt::make_sdf_primitive(geometry, material, transform, 1, 7),
		};
		lrt::TrunkSurfaceWorkspace workspace;
		REQUIRE(workspace.build(grid, 0, instances, { 0 }));
		CHECK(workspace.get_instance_count() == 1);
		CHECK(workspace.payload_bytes() > 0);
		for (int z = 0; z < 5; z++) {
			for (int y = 0; y < 5; y++) {
				for (int x = 0; x < 5; x++) {
					const lrt::Vec3 center = geometry->min + lrt::Vec3(x, y, z) * geometry->cell;
					for (double offset : { -0.0625, 0.0, 0.0625, std::nextafter(0.0625, 1.0) }) {
						const lrt::Vec3 point = transform.xform(center + lrt::Vec3(offset, 0, 0));
						const lrt::SurfaceSample expected = instances[0].sample(point);
						lrt::SurfaceSample actual;
						CHECK(workspace.sample(point, actual) == expected.valid);
						if (expected.valid) {
							check_workspace_surface(actual, expected);
						}
					}
					for (const lrt::Direction *direction = lrt::directions(); direction < lrt::directions() + 26; direction++) {
						const lrt::Vec3 from = transform.xform(center);
						const lrt::Vec3 to = transform.xform(center + direction->direction * 0.4);
						for (int reverse = 0; reverse < 2; reverse++) {
							const lrt::Vec3 a = reverse ? to : from;
							const lrt::Vec3 b = reverse ? from : to;
							lrt::SurfaceSample expected;
							lrt::SurfaceSample actual;
							const bool hit = instances[0].trace_segment(a, b, expected);
							CHECK(workspace.trace_segment(a, b, actual) == hit);
							if (hit) {
								check_workspace_surface(actual, expected);
							}
						}
					}
				}
			}
		}
	}
}

TEST_CASE("[LRT] Trunk workspaces borrow large grids without copying covered cells") {
	const lrt::Grid grid = lrt::make_grid_sized(0.5, { -8, -0.5, -0.5 }, { 16.5, 1, 1 });
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf({ 32, 1, 1 }, 128));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 0.5, 0.5, 0.5 }));
	lrt::PrimitiveTransform singular;
	singular.basis_x = {};
	const std::vector<lrt::GeometryInstance> instances = {
		lrt::make_sdf_primitive(geometry, material, {}, 1),
		lrt::make_sdf_primitive(geometry, material, singular, 2),
	};
	lrt::TrunkIndex index;
	index.update(grid, instances);
	for (int trunk = 0; trunk < 5; trunk++) {
		lrt::TrunkSurfaceWorkspace workspace;
		REQUIRE(workspace.build(grid, trunk, instances, index.get_candidates(trunk)));
		CHECK(workspace.get_instance_count() == 1);
		CHECK(workspace.payload_bytes() == sizeof(const lrt::GeometryInstance *));
		for (int x = trunk * 8; x < std::min((trunk + 1) * 8, grid.size[0]); x++) {
			const lrt::Vec3 point = lrt::probe_point(grid, x, 0, 0);
			lrt::SurfaceSample actual;
			REQUIRE(workspace.sample(point, actual));
			check_workspace_surface(actual, instances[0].sample(point));
		}
	}
	const lrt::LocalField field = lrt::build_sdf_local_data(grid, instances, nullptr, 3);
	CHECK(field.solid_count == grid.count);
	CHECK(field.surface_workspaces_built == 5);
}

TEST_CASE("[LRT] Trunk DDA keeps material ownership at equal-distance overlapping cells") {
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf({ 1, 1, 1 }, 4));
	auto red = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 1, 0, 0 }, { 3, 0, 0 }, lrt::EMISSION_BOTH));
	auto green = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 0, 1, 0 }, { 0, 5, 0 }, lrt::EMISSION_BOTH));
	const std::vector<lrt::GeometryInstance> instances = {
		lrt::make_sdf_primitive(geometry, red, {}, 1, 2),
		lrt::make_sdf_primitive(geometry, green, {}, 2, 4),
	};
	const auto grid = lrt::make_grid_sized(0.5, { -2, -2, -2 }, { 4, 4, 4 });
	for (int first : { 0, 1 }) {
		lrt::TrunkSurfaceWorkspace workspace;
		REQUIRE(workspace.build(grid, 0, instances, { first, 1 - first }));
		CHECK(workspace.get_instance_count() == 2);
		lrt::SurfaceSample actual;
		const lrt::Vec3 point(0.5, 0, 0);
		REQUIRE(workspace.sample(point, actual));
		check_workspace_surface(actual, instances[size_t(first)].sample(point));
		for (int sign : { -1, 1 }) {
			const lrt::Vec3 from(sign * 2.0, 0, 0);
			const lrt::Vec3 to(-sign * 2.0, 0, 0);
			lrt::SurfaceSample expected;
			REQUIRE(instances[size_t(first)].trace_segment(from, to, expected));
			REQUIRE(workspace.trace_segment(from, to, actual));
			check_workspace_surface(actual, expected);
		}
	}
}

TEST_CASE("[LRT] Dirty Trunk workspaces preserve the complete composite field and clean reuse") {
	const lrt::Grid grid = lrt::make_grid_sized(0.5, { -4, -1, -1 }, { 8.5, 2.5, 2 });
	const auto baked = lrt::bake_mesh_sdf(coarse_plane_mesh(0), 16);
	REQUIRE(baked.error == lrt::MESH_SDF_BAKE_OK);
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(baked.field);
	auto black = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({}));
	auto emission = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 0.2, 0.6, 0.9 }, { 4, 2, 1 }));
	lrt::PrimitiveTransform rear;
	rear.origin = { 0.25, 0, 0 };
	std::vector<lrt::GeometryInstance> instances = {
		lrt::make_sdf_primitive(geometry, black, {}, 1, 1),
		lrt::make_sdf_primitive(geometry, emission, rear, 2, 2),
		lrt::make_sdf_primitive(geometry, black, {}, 1, 1),
	};
	lrt::LocalCache cache;
	const lrt::LocalField field = lrt::build_sdf_local_data(grid, instances, nullptr, 3, nullptr, &cache);
	CHECK(field.surface_workspaces_built == field.dirty_trunk_count);
	CHECK(field.surface_workspace_instances > 0);
	CHECK(field.surface_workspace_payload_bytes > 0);
	for (int y = 0; y < grid.size[1]; y++) {
		for (int z = 0; z < grid.size[2]; z++) {
			for (int x = 0; x < grid.size[0]; x++) {
				const int probe = lrt::index_of(grid, x, y, z);
				const lrt::Vec3 point = lrt::probe_point(grid, x, y, z);
				bool occupied = false;
				for (const auto &instance : instances) {
					occupied |= instance.sample(point).valid;
				}
				CHECK((field.material[size_t(probe) * 4 + 3] > 0.5f) == occupied);
				if (occupied) {
					CHECK(field.links[size_t(probe)] == 0);
					continue;
				}
				uint32_t mask = 0;
				std::array<lrt::LocalSurface, 26> surfaces;
				for (int j = 0; j < 26; j++) {
					const auto &direction = lrt::directions()[j];
					const lrt::Vec3 neighbor = lrt::probe_point(grid, x + direction.offset[0], y + direction.offset[1], z + direction.offset[2]);
					lrt::SurfaceSample nearest;
					for (const auto &instance : instances) {
						lrt::SurfaceSample sample;
						if (instance.trace_segment(point, neighbor, sample) && (!nearest.valid || sample.fraction < nearest.fraction)) {
							nearest = sample;
						}
					}
					if (nearest.valid) {
						surfaces[size_t(j)] = { nearest.color, nearest.emission };
					} else {
						mask |= 1u << j;
					}
				}
				CHECK(field.links[size_t(probe)] == mask);
				const lrt::LocalRecord expected = lrt::build_local_record(mask, surfaces);
				CHECK(field.get_local_record(size_t(probe)) == (expected.has_contribution() ? expected : lrt::LocalRecord()));
			}
		}
	}
	cache.local = &field;
	const lrt::LocalField unchanged = lrt::build_sdf_local_data(grid, instances, nullptr, 2, &cache);
	CHECK(unchanged.surface_workspaces_built == 0);
	CHECK(unchanged.surface_workspace_instances == 0);
	CHECK(unchanged.surface_workspace_payload_bytes == 0);
	CHECK(same_local_records(unchanged, field));
	CHECK(unchanged.links == field.links);
	instances.clear();
	const lrt::LocalField removed = lrt::build_sdf_local_data(grid, instances, nullptr, 2, &cache);
	const lrt::LocalField empty = lrt::build_sdf_local_data(grid, {});
	CHECK(removed.surface_workspaces_built == removed.dirty_trunk_count);
	CHECK(removed.surface_workspace_instances == 0);
	CHECK(same_local_records(removed, empty));
	CHECK(removed.links == empty.links);
	std::atomic<bool> cancel(true);
	lrt::TrunkSurfaceWorkspace cancelled;
	CHECK_FALSE(cancelled.build(grid, 0, instances, {}, &cancel));
}

TEST_CASE("[LRT] Native material capture dependencies separate opaque color emission and shared inputs") {
	LRTVolume3D *volume = memnew(LRTVolume3D);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo(Color(0.2, 0.7, 0.3));
	material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	material->set_emission(Color(8, 4, 2));
	auto signature = [&](uint8_t p_component) {
		return LRTVolume3DTestAccess::material_component_signature(volume, material, p_component);
	};
	uint64_t rho = signature(lrt::LOCAL_REFLECTANCE);
	uint64_t emission = signature(lrt::LOCAL_EMISSION);
	material->set_emission_energy_multiplier(4.0f);
	CHECK(signature(lrt::LOCAL_REFLECTANCE) == rho);
	CHECK(signature(lrt::LOCAL_EMISSION) != emission);
	emission = signature(lrt::LOCAL_EMISSION);
	material->set_albedo(Color(0.8, 0.2, 0.4));
	CHECK(signature(lrt::LOCAL_REFLECTANCE) != rho);
	CHECK(signature(lrt::LOCAL_EMISSION) == emission);
	rho = signature(lrt::LOCAL_REFLECTANCE);
	material->set_cull_mode(BaseMaterial3D::CULL_FRONT);
	CHECK(signature(lrt::LOCAL_REFLECTANCE) == rho);
	CHECK(signature(lrt::LOCAL_EMISSION) != emission);
	emission = signature(lrt::LOCAL_EMISSION);
	material->set_uv1_scale(Vector3(2, 1, 1));
	CHECK(signature(lrt::LOCAL_REFLECTANCE) != rho);
	CHECK(signature(lrt::LOCAL_EMISSION) != emission);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	emission = signature(lrt::LOCAL_EMISSION);
	material->set_albedo(Color(0.8, 0.2, 0.4, 0.25));
	CHECK(signature(lrt::LOCAL_EMISSION) != emission);
	rho = signature(lrt::LOCAL_REFLECTANCE);
	emission = signature(lrt::LOCAL_EMISSION);
	Ref<StandardMaterial3D> receiving;
	receiving.instantiate();
	receiving->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);
	receiving->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
	material->set_next_pass(receiving);
	CHECK(signature(lrt::LOCAL_REFLECTANCE) == rho);
	CHECK(signature(lrt::LOCAL_EMISSION) == emission);
	receiving->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
	receiving->set_emission(Color(1000, 2000, 3000));
	CHECK(signature(lrt::LOCAL_REFLECTANCE) == rho);
	CHECK(signature(lrt::LOCAL_EMISSION) == emission);
	Ref<StandardMaterial3D> tail;
	tail.instantiate();
	receiving->set_next_pass(tail);
	CHECK(signature(lrt::LOCAL_REFLECTANCE) != rho);
	CHECK(signature(lrt::LOCAL_EMISSION) != emission);
	for (uint8_t component : { lrt::LOCAL_REFLECTANCE, lrt::LOCAL_EMISSION }) {
		CHECK(LRTVolume3DTestAccess::material_component_signature(volume, receiving, component) ==
				LRTVolume3DTestAccess::material_component_signature(volume, tail, component));
	}
	memdelete(volume);
}

TEST_CASE("[LRT] Emission sides are independent of conservative blocking and affine orientation") {
	auto geometry = std::make_shared<lrt::CoarseGeometryField>();
	geometry->cell = 1.0;
	geometry->size[0] = geometry->size[1] = geometry->size[2] = 1;
	geometry->cells = { lrt::coarse_normal_mask({ 1, 0, 0 }) };
	const lrt::Vec3 emission(8, 4, 2);
	for (double scale : { 1.0, -2.0 }) {
		lrt::PrimitiveTransform transform;
		transform.origin = { 7, -3, 2 };
		transform.basis_x = { scale, 0.5, 0.25 };
		transform.basis_y = { 0, 3, 0.5 };
		transform.basis_z = { 0.5, 0, 4 };
		const lrt::Vec3 front = transform.xform({ 2, 0, 0 });
		const lrt::Vec3 back = transform.xform({ -2, 0, 0 });
		for (uint8_t sides : { lrt::EMISSION_FRONT, lrt::EMISSION_BACK, lrt::EMISSION_BOTH }) {
			auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({ 0.2, 0.4, 0.6 }, emission, sides));
			const auto instance = lrt::make_sdf_primitive(geometry, material, transform, 123);
			lrt::SurfaceSample sample;
			REQUIRE(instance.trace_segment(front, back, sample));
			CHECK(sample.emission.x == ((sides & lrt::EMISSION_FRONT) ? emission.x : 0.0));
			CHECK(sample.color.x == doctest::Approx(0.2));
			const double fraction = sample.fraction;
			REQUIRE(instance.trace_segment(back, front, sample));
			CHECK(sample.emission.x == ((sides & lrt::EMISSION_BACK) ? emission.x : 0.0));
			CHECK(sample.fraction == doctest::Approx(fraction));
			CHECK(instance.sample(transform.xform({ 0, 0, 0 })).valid);
		}
	}
	const lrt::SurfaceMaterialField front = lrt::bake_constant_instance_field({ 0.2, 0.4, 0.6 }, emission);
	const lrt::SurfaceMaterialField back = lrt::bake_constant_instance_field({ 0.2, 0.4, 0.6 }, emission, lrt::EMISSION_BACK);
	CHECK(lrt::material_component_signature(front, false) == lrt::material_component_signature(back, false));
	CHECK(lrt::material_component_signature(front, true) != lrt::material_component_signature(back, true));
}

TEST_CASE("[LRT] Instance cache preserves emission sides and rejects the prior material layout") {
	const lrt::SurfaceMaterialField field = lrt::bake_constant_instance_field({ 0.2, 0.4, 0.6 }, { 16, 4, 2 }, lrt::EMISSION_BOTH);
	const uint64_t signature = hash_murmur3_one_64(OS::get_singleton()->get_ticks_usec(), 0x5134);
	const String path = lrt::asset_cache_directory().path_join(vformat("instance_%016x.lrt", signature));
	REQUIRE_FALSE(FileAccess::exists(path));
	REQUIRE(lrt::store_instance_field(signature, field));
	lrt::SurfaceMaterialField loaded;
	REQUIRE(lrt::load_instance_field(signature, loaded));
	CHECK(loaded.albedo == field.albedo);
	CHECK(loaded.emission == field.emission);
	CHECK(loaded.emission_sides == field.emission_sides);
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ_WRITE);
	REQUIRE(file.is_valid());
	file->store_32(5);
	file.unref();
	CHECK_FALSE(lrt::load_instance_field(signature, loaded));
	CHECK(DirAccess::remove_absolute(path) == OK);
}

TEST_CASE("[LRT] Translated thin mesh material uses the native capture affine rounding") {
	LRTVolume3D *volume = memnew(LRTVolume3D);
	const auto mesh = lrt::build_triangle_mesh(coarse_plane_mesh(0).triangles);
	const auto geometry = lrt::bake_mesh_sdf(mesh, 16);
	REQUIRE(geometry.error == lrt::MESH_SDF_BAKE_OK);
	const Vector3i size(4, 17, 17);
	const Vector3i render_size = size * 2;
	const int count = size.x * size.y * size.z;
	PackedByteArray albedo;
	PackedByteArray emission;
	PackedByteArray aniso;
	PackedByteArray normals;
	albedo.resize(count * 12);
	emission.resize(count * 4);
	aniso.resize(count * 4);
	normals.resize(render_size.x * render_size.y * render_size.z * 4);
	albedo.fill(0);
	emission.fill(0);
	aniso.fill(0);
	normals.fill(0);
	for (int z = 0; z < size.z; z++) {
		for (int y = 0; y < size.y; y++) {
			const int index = 2 + size.x * (y + size.y * z);
			const int packed = 2 + size.x * (y + size.y * (z * 6));
			encode_uint16(31, albedo.ptrw() + packed * 2);
			encode_uint32(Color(8, 4, 2).to_rgbe9995(), emission.ptrw() + index * 4);
			encode_uint32(31u | (1u << 30), aniso.ptrw() + index * 4);
			const int normal = 4 + render_size.x * (y * 2 + render_size.y * z * 2);
			encode_uint32(1, normals.ptrw() + normal * 4);
		}
	}
	for (real_t x : { real_t(-6), real_t(0), real_t(6) }) {
		Transform3D transform;
		transform.origin.x = x;
		AABB bounds = transform.xform(AABB(Vector3(-0.125, -1.125, -1.125), Vector3(0.25, 2.25, 2.25)));
		const Vector3 cells = bounds.size / Vector3(render_size);
		const real_t cell = MAX(cells.x, MAX(cells.y, cells.z));
		const Vector3 aligned = Vector3(render_size) * cell;
		bounds.position -= (aligned - bounds.size) * 0.5;
		bounds.size = aligned;
		Dictionary images;
		images["size"] = size;
		images["bounds"] = bounds;
		images["transform"] = transform;
		images["albedo"] = albedo;
		images["emission"] = emission;
		images["emission_aniso"] = aniso;
		images["normal_bits"] = normals;
		LRTVolume::MeshInstance input;
		input.triangles = std::make_shared<std::vector<lrt::MeshTriangle>>(mesh.triangles);
		input.capture_input_signatures = { 100, 200, 300 };
		String error;
		REQUIRE(LRTVolume3DTestAccess::decode_capture(volume, images, size, input, error));
		CHECK(error.is_empty());
		REQUIRE(bool(input.material));
		CHECK(int(std::floor(input.material->uvw_offset.x * size.x)) == 2);
		const auto material = lrt::bake_mesh_instance_field(mesh, geometry.field, input.material.get());
		CHECK(*std::max_element(material.albedo.begin(), material.albedo.end()) == 255);
		REQUIRE_FALSE(material.emission.empty());
		CHECK(*std::max_element(material.emission.begin(), material.emission.end()) == 8.0f);
		CHECK(material.emission_sides.size() == material.albedo.size() / 3);
		CHECK(*std::max_element(material.emission_sides.begin(), material.emission_sides.end()) == lrt::EMISSION_BACK);
		const auto initial = input.material;
		PackedByteArray empty_albedo;
		empty_albedo.resize(albedo.size());
		empty_albedo.fill(0);
		images["albedo"] = empty_albedo;
		PackedByteArray dim_emission = emission.duplicate();
		for (int index = 0; index < count; index++) {
			encode_uint32(Color(2, 1, 0.5).to_rgbe9995(), dim_emission.ptrw() + index * 4);
		}
		images["emission"] = dim_emission;
		input.previous_material = initial;
		input.capture_input_signatures.emission++;
		REQUIRE(LRTVolume3DTestAccess::decode_capture(volume, images, size, input, error));
		CHECK(input.material->albedo == initial->albedo);
		CHECK(input.material->emission != initial->emission);
		CHECK_FALSE(bool(input.previous_material));
		const auto updated_emission = input.material;
		input.previous_material = updated_emission;
		input.capture_input_signatures.reflectance++;
		images["emission"] = emission;
		REQUIRE(LRTVolume3DTestAccess::decode_capture(volume, images, size, input, error));
		CHECK(input.material->albedo != updated_emission->albedo);
		CHECK(input.material->emission == updated_emission->emission);
		CHECK(input.material->emission_sides == updated_emission->emission_sides);
		input.previous_material = input.material;
		input.capture_input_signatures.geometry++;
		images["albedo"] = albedo;
		REQUIRE(LRTVolume3DTestAccess::decode_capture(volume, images, size, input, error));
		CHECK(input.material->albedo == initial->albedo);
		CHECK(input.material->emission == initial->emission);
	}
	memdelete(volume);
}

TEST_CASE("[LRT] Sparse local slots retain identities and reuse only released contributions") {
	lrt::LocalField field;
	field.local_record_indices.assign(1024, lrt::LocalField::NO_RECORD);
	lrt::LocalRecord black;
	black.geometry[0] = 1;
	CHECK(field.set_local_record(3, black) == lrt::LocalField::NO_RECORD);
	CHECK(field.local_records.empty());
	lrt::LocalRecord reflected = black;
	reflected.reflectance = 0x00123456;
	const uint32_t first = field.set_local_record(3, reflected);
	lrt::LocalRecord emission;
	emission.emission[0] = 16.0f;
	const uint32_t second = field.set_local_record(700, emission);
	CHECK(first != second);
	CHECK(field.local_records.size() == 2);
	CHECK(field.validate_local_records());
	reflected.reflectance = 0x00654321;
	CHECK(field.set_local_record(3, reflected) == first);
	CHECK(field.get_local_record(700).emission[0] == 16.0f);
	field.set_local_record(700, {});
	CHECK(field.local_record_indices[700] == lrt::LocalField::NO_RECORD);
	CHECK(field.set_local_record(17, emission) == second);
	CHECK(field.local_record_indices[3] == first);
	CHECK(field.local_records.size() == 2);
	CHECK(field.validate_local_records());
	field.set_local_record(3, {});
	field.set_local_record(17, {});
	CHECK(field.free_local_records.size() == 2);
	CHECK(field.local_records.size() == 2);
	CHECK(field.validate_local_records());
	CHECK(field.get_local_record(17) == lrt::LocalRecord());
	CHECK(field.set_local_record(514, reflected) == second);
	CHECK(field.validate_local_records());
	lrt::LocalField broken = field;
	broken.local_record_indices[1] = second;
	CHECK_FALSE(broken.validate_local_records());
	broken = field;
	broken.free_local_records.push_back(broken.free_local_records.back());
	CHECK_FALSE(broken.validate_local_records());
	broken = field;
	broken.local_record_owners[second] = 1;
	CHECK_FALSE(broken.validate_local_records());
}

TEST_CASE("[LRT] Incremental sparse records preserve clean slots and recycle moved emission") {
	const auto baked = lrt::bake_mesh_sdf(coarse_plane_mesh(0), 16);
	REQUIRE(baked.error == lrt::MESH_SDF_BAKE_OK);
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(baked.field);
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field({}, { 8, 4, 2 }));
	const lrt::Grid grid = lrt::make_grid_sized(0.5, { -8, -0.5, -0.5 }, { 16, 1, 1 });
	auto instances = [&](double p_x, uint64_t p_signature) {
		lrt::PrimitiveTransform fixed;
		fixed.origin = { -6, 0, 0 };
		lrt::PrimitiveTransform moved;
		moved.origin = { p_x, 0, 0 };
		return std::vector<lrt::GeometryInstance>{
			lrt::make_sdf_primitive(geometry, material, fixed, 1),
			lrt::make_sdf_primitive(geometry, material, moved, p_signature),
		};
	};
	lrt::LocalCache first_cache;
	lrt::LocalField first = lrt::build_sdf_local_data(grid, instances(0, 2), nullptr, 3, nullptr, &first_cache);
	first_cache.local = &first;
	CHECK(first.validate_local_records());
	CHECK(first.local_records.size() < size_t(grid.count) / 4);
	lrt::LocalCache second_cache;
	lrt::LocalField second = lrt::build_sdf_local_data(grid, instances(4, 3), nullptr, 3, &first_cache, &second_cache);
	CHECK(second.validate_local_records());
	CHECK(second.local_records.size() == first.local_records.size());
	int retained_records = 0;
	for (int z = 0; z < grid.size[2]; z++) {
		for (int y = 0; y < grid.size[1]; y++) {
			for (int x = 0; x < 8; x++) {
				const size_t probe = size_t(lrt::index_of(grid, x, y, z));
				CHECK(second.local_record_indices[probe] == first.local_record_indices[probe]);
				retained_records += first.local_record_indices[probe] != lrt::LocalField::NO_RECORD;
			}
		}
	}
	CHECK(retained_records > 0);
	const lrt::LocalField rebuilt = lrt::build_sdf_local_data(grid, instances(4, 3));
	CHECK(same_local_records(second, rebuilt));
	second_cache.local = &second;
	lrt::LocalField recycled = std::move(first);
	first_cache.local = &recycled;
	lrt::LocalField third = lrt::build_sdf_local_data(grid, instances(2, 4), nullptr, 3, &second_cache, &first_cache, &recycled);
	CHECK(third.snapshot_reused);
	CHECK(third.validate_local_records());
	CHECK(same_local_records(third, lrt::build_sdf_local_data(grid, instances(2, 4))));
}

TEST_CASE("[LRT] Recycled local snapshot matches a full rebuild after successive edits") {
	const lrt::Grid grid = lrt::make_grid_sized(1.0, lrt::Vec3(-16, -16, -16), lrt::Vec3(32, 32, 32));
	auto geometry = std::make_shared<lrt::CoarseGeometryField>(lrt::bake_box_sdf(lrt::Vec3(1, 1, 1), 16));
	auto material = std::make_shared<lrt::SurfaceMaterialField>(lrt::bake_constant_instance_field(lrt::Vec3(1, 0.5, 0.25)));
	auto make_primitives = [&](double p_x, uint64_t p_signature) {
		lrt::PrimitiveTransform transform;
		transform.origin = lrt::Vec3(p_x, -12, -12);
		return std::vector<lrt::GeometryInstance>{ lrt::make_sdf_primitive(geometry, material, transform, p_signature) };
	};
	lrt::LocalCache first_cache;
	lrt::LocalField first = lrt::build_sdf_local_data(grid, make_primitives(-12, 1), nullptr, 1, nullptr, &first_cache);
	first_cache.local = &first;
	lrt::LocalCache second_cache;
	lrt::LocalField second = lrt::build_sdf_local_data(grid, make_primitives(-11.5, 2), nullptr, 1, &first_cache, &second_cache);
	second_cache.local = &second;
	lrt::LocalField recycled = std::move(first);
	lrt::LocalCache recycled_cache = std::move(first_cache);
	recycled_cache.local = &recycled;
	lrt::LocalField third = lrt::build_sdf_local_data(grid, make_primitives(-11, 3), nullptr, 1, &second_cache, &recycled_cache, &recycled);
	lrt::LocalField reference = lrt::build_sdf_local_data(grid, make_primitives(-11, 3));
	CHECK(third.snapshot_reused);
	CHECK(third.snapshot_sync_probes > 0);
	CHECK(third.snapshot_sync_probes < grid.count);
	CHECK(third.material == reference.material);
	CHECK(same_local_records(third, reference));
	CHECK(third.links == reference.links);
	CHECK(recycled_cache.sampled.size() == size_t(grid.count));
	recycled_cache.local = &third;
	lrt::LocalField fourth = lrt::build_sdf_local_data(grid, make_primitives(-10.5, 4), nullptr, 1, &recycled_cache, &second_cache, &second);
	lrt::LocalField fourth_reference = lrt::build_sdf_local_data(grid, make_primitives(-10.5, 4));
	CHECK(fourth.snapshot_reused);
	CHECK(fourth.material == fourth_reference.material);
	CHECK(same_local_records(fourth, fourth_reference));
	CHECK(fourth.links == fourth_reference.links);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[LRT] Compact local caches preserve nonzero records without receiver pages") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(0.5, Vector3(-3, -2, -3), Vector3(6, 4, 6));
	LRTVolume::BoxInstance box;
	box.local_extent = { 1, 1, 1 };
	box.sdf_resolution = 8;
	box.color = { 0.2, 0.7, 0.3 };
	box.emission = { 8, 4, 2 };
	solver->set_box_instances({ box });
	REQUIRE(solver->bake_local_field_data(false).ok);
	const lrt::LocalField original = LRTVolume3DTestAccess::staged_local(solver);
	REQUIRE_FALSE(original.local_records.empty());
	REQUIRE(original.validate_local_records());
	CHECK(original.local_records.front().emission[0] > 0.0f);
	const uint64_t fingerprint = hash_murmur3_one_64(OS::get_singleton()->get_ticks_usec(), 0x5033);
	const String path = lrt::asset_cache_directory().path_join(vformat("volume_%016x.lrt", fingerprint));
	REQUIRE_FALSE(FileAccess::exists(path));
	REQUIRE(solver->store_local_field_cache(fingerprint, true));
	Ref<LRTVolume> restored;
	restored.instantiate();
	LRTVolume::LocalBakeResult loaded;
	REQUIRE(restored->load_local_field_cache(fingerprint, loaded));
	CHECK(loaded.cache_loaded);
	const lrt::LocalField &cached = LRTVolume3DTestAccess::staged_local(restored);
	CHECK(cached.material == original.material);
	CHECK(cached.links == original.links);
	CHECK(cached.local_records == original.local_records);
	CHECK(cached.local_record_indices == original.local_record_indices);
	CHECK(cached.local_record_owners == original.local_record_owners);
	CHECK(cached.free_local_records == original.free_local_records);
	CHECK(cached.gpu_local_records == original.gpu_local_records);
	const lrt::LocalCache &original_cache = LRTVolume3DTestAccess::staged_cache(solver);
	const lrt::LocalCache &restored_cache = LRTVolume3DTestAccess::staged_cache(restored);
	REQUIRE(restored_cache.input_signatures.size() == original_cache.input_signatures.size());
	for (size_t trunk = 0; trunk < original_cache.input_signatures.size(); trunk++) {
		CHECK(restored_cache.input_signatures[trunk].geometry == original_cache.input_signatures[trunk].geometry);
		CHECK(restored_cache.input_signatures[trunk].reflectance == original_cache.input_signatures[trunk].reflectance);
		CHECK(restored_cache.input_signatures[trunk].emission == original_cache.input_signatures[trunk].emission);
	}
	CHECK(cached.validate_local_records());
	CHECK(lrt::local_material_rgba(cached, false) == lrt::local_material_rgba(original, false));
	CHECK(lrt::local_material_rgba(cached, true) == lrt::local_material_rgba(original, true));
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ_WRITE);
		REQUIRE(file.is_valid());
		file->seek(8);
		CHECK(file->get_32() == 17);
		file->seek(8);
		file->store_32(15);
	}
	CHECK_FALSE(restored->load_local_field_cache(fingerprint, loaded));
	CHECK_FALSE(bool(LRTVolume::get_cached_export_dependencies(fingerprint)["ok"]));
	CHECK(DirAccess::remove_absolute(path) == OK);
}

TEST_CASE("[LRT] Export preparation does not generate unused CPU voxel assets") {
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	Ref<LRTExportPlugin> exporter;
	exporter.instantiate();
	CHECK_FALSE(bool(exporter->prepare_resource(mesh)["ok"]));
	MeshInstance3D *root = memnew(MeshInstance3D);
	root->set_mesh(mesh);
	Ref<PackedScene> scene;
	scene.instantiate();
	REQUIRE(scene->pack(root) == OK);
	memdelete(root);
	const Dictionary report = exporter->prepare_resource(scene);
	REQUIRE(bool(report["ok"]));
	CHECK(int(report["count"]) == 0);
}

TEST_CASE("[LRT] Saved export manifests work without a rendering device and reject broken data") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(1.0, Vector3(-1, -1, -1), Vector3(2, 2, 2));
	REQUIRE(solver->bake_local_field_data(true).ok);
	const uint64_t fingerprint = hash_murmur3_one_64(OS::get_singleton()->get_ticks_usec(), 0x4c525431U);
	const String path = lrt::asset_cache_directory().path_join(vformat("volume_%016x.lrt", fingerprint));
	REQUIRE_FALSE(FileAccess::exists(path));
	REQUIRE(solver->store_local_field_cache(fingerprint, true));
	Dictionary report = LRTVolume::get_cached_export_dependencies(fingerprint);
	CHECK(bool(report["ok"]));
	CHECK(PackedStringArray(report["assets"]).size() == 1);
	// Off-volume assets are manifest dependencies even though the local field is empty.
	const String extra_path = lrt::static_voxel_capture_path(fingerprint);
	REQUIRE_FALSE(FileAccess::exists(extra_path));
	{
		Ref<FileAccess> dependency = FileAccess::open(extra_path, FileAccess::WRITE);
		REQUIRE(dependency.is_valid());
		dependency->store_8(1);
	}
	PackedStringArray extra_dependencies;
	extra_dependencies.push_back(extra_path);
	extra_dependencies.push_back(extra_path);
	REQUIRE(solver->store_local_field_cache(fingerprint, true, extra_dependencies));
	report = LRTVolume::get_cached_export_dependencies(fingerprint);
	CHECK(bool(report["ok"]));
	CHECK(PackedStringArray(report["assets"]).size() == 2);
	CHECK(PackedStringArray(report["assets"]).has(extra_path));
	CHECK(DirAccess::remove_absolute(extra_path) == OK);
	report = LRTVolume::get_cached_export_dependencies(fingerprint);
	CHECK_FALSE(bool(report["ok"]));
	CHECK(String(report["error"]).contains("dependency"));
	LRTVolume::LocalBakeResult loaded;
	CHECK(solver->load_local_field_cache(fingerprint, loaded));
	CHECK(loaded.cache_loaded);
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ_WRITE);
		REQUIRE(file.is_valid());
		file->seek_end(-8);
		const uint64_t offset = file->get_position();
		file->store_32(1);
		file->store_pascal_string(vformat("missing_%016x.sdf", fingerprint));
		file->store_64(offset);
	}
	report = LRTVolume::get_cached_export_dependencies(fingerprint);
	CHECK_FALSE(bool(report["ok"]));
	CHECK(String(report["error"]).contains("dependency"));
	{
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ_WRITE);
		REQUIRE(file.is_valid());
		file->seek_end(-8);
		file->store_64(UINT64_MAX);
	}
	report = LRTVolume::get_cached_export_dependencies(fingerprint);
	CHECK_FALSE(bool(report["ok"]));
	CHECK(String(report["error"]).contains("manifest"));
	CHECK(DirAccess::remove_absolute(path) == OK);
	CHECK_FALSE(bool(LRTVolume::get_cached_export_dependencies(fingerprint)["ok"]));
}

TEST_CASE("[LRT] Export reads inherited and instanced Volume fingerprints without instantiation") {
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(1.0, Vector3(-1, -1, -1), Vector3(2, 2, 2));
	REQUIRE(solver->bake_local_field_data(true).ok);
	const uint64_t fingerprint = hash_murmur3_one_64(OS::get_singleton()->get_ticks_usec(), 0x4c525432U);
	const String path = lrt::asset_cache_directory().path_join(vformat("volume_%016x.lrt", fingerprint));
	REQUIRE_FALSE(FileAccess::exists(path));
	REQUIRE(solver->store_local_field_cache(fingerprint, true));
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_name("Volume");
	volume->set_build_cache_fingerprint(int64_t(fingerprint));
	root->add_child(volume);
	volume->set_owner(root);
	Ref<PackedScene> base;
	base.instantiate();
	REQUIRE(base->pack(root) == OK);
	memdelete(root);
	Ref<LRTExportPlugin> exporter;
	exporter.instantiate();
	CHECK(bool(exporter->collect_export_dependencies(base)["ok"]));
	Ref<PackedScene> inherited;
	inherited.instantiate();
	inherited->get_state()->set_base_scene(inherited->get_state()->add_value(base));
	CHECK(bool(exporter->collect_export_dependencies(inherited)["ok"]));
	Ref<PackedScene> parent;
	parent.instantiate();
	Ref<SceneState> state = parent->get_state();
	state->add_node(-1, -1, state->add_name("Node3D"), state->add_name("Root"), -1, -1, 0);
	state->add_node(0, 0, SceneState::TYPE_INSTANTIATED, state->add_name("Child"), state->add_value(base), -1, 0);
	CHECK(bool(exporter->collect_export_dependencies(parent)["ok"]));
	// An override path must replace the nested scene's fingerprint, including its ./ prefix.
	const int override_node = state->add_node(state->add_node_path(NodePath("Child"), PackedInt32Array()) | SceneState::FLAG_ID_IS_PATH,
			0, SceneState::TYPE_INSTANTIATED, state->add_name("Volume"), -1, -1, 0);
	state->add_node_property(override_node, state->add_name("build_cache_fingerprint"), state->add_value(int64_t(0)));
	CHECK_FALSE(bool(exporter->collect_export_dependencies(parent)["ok"]));
	CHECK(DirAccess::remove_absolute(path) == OK);
}

TEST_CASE("[LRT] Dynamic Volumes export prepared dependencies without a persistent field") {
	const String directory = lrt::asset_cache_directory();
	std::set<String> existing;
	for (const String &name : DirAccess::get_files_at(directory)) {
		existing.insert(name);
	}
	Ref<LRTVolume> solver;
	solver.instantiate();
	solver->configure_sized(1.0, Vector3(-1, -1, -1), Vector3(2, 2, 2));
	LRTVolume::BoxInstance box;
	box.local_extent = lrt::Vec3(1, 1, 1);
	box.sdf_resolution = 8;
	box.color = lrt::Vec3(0.5, 0.5, 0.5);
	solver->set_box_instances({ box });
	REQUIRE(solver->bake_local_field_data(false).ok);
	const PackedStringArray paths = solver->store_prepared_dependencies();
	REQUIRE(paths.size() == 2);
	PackedStringArray names;
	for (const String &path : paths) {
		names.push_back(path.get_file());
	}
	Dictionary metadata;
	metadata["runtime_only"] = true;
	metadata["assets"] = names;
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_build_cache_export_data(metadata);
	Ref<PackedScene> scene;
	scene.instantiate();
	REQUIRE(scene->pack(volume) == OK);
	memdelete(volume);
	Ref<LRTExportPlugin> exporter;
	exporter.instantiate();
	const Dictionary report = exporter->collect_export_dependencies(scene);
	CHECK(bool(report["ok"]));
	CHECK(int(report["count"]) == 2);
	metadata = metadata.duplicate();
	names.push_back("../invalid.sdf");
	metadata["assets"] = names;
	volume = memnew(LRTVolume3D);
	volume->set_build_cache_export_data(metadata);
	REQUIRE(scene->pack(volume) == OK);
	memdelete(volume);
	CHECK_FALSE(bool(exporter->collect_export_dependencies(scene)["ok"]));
	for (const String &path : paths) {
		if (existing.find(path.get_file()) == existing.end()) {
			CHECK(DirAccess::remove_absolute(path) == OK);
		}
	}
}
#endif

TEST_CASE("[SceneTree][LRT] Shadow invalidation ignores distant and masked casters but follows relevant motion") {
	Node3D *root = memnew(Node3D);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	volume->set_rebuild_suppressed(true);
	root->add_child(volume);
	OmniLight3D *light = memnew(OmniLight3D);
	light->set_shadow(true);
	light->set_shadow_caster_mask(1);
	light->set_param(Light3D::PARAM_RANGE, 5);
	root->add_child(light);
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	MeshInstance3D *near = memnew(MeshInstance3D);
	MeshInstance3D *far = memnew(MeshInstance3D);
	MeshInstance3D *masked = memnew(MeshInstance3D);
	for (MeshInstance3D *instance : { near, far, masked }) {
		instance->set_mesh(mesh);
		instance->set_custom_aabb(AABB(Vector3(-0.5, -0.5, -0.5), Vector3(1, 1, 1)));
		root->add_child(instance);
	}
	far->set_position(Vector3(20, 0, 0));
	masked->set_layer_mask(2);
	SceneTree::get_singleton()->get_root()->add_child(root);
	// Publish the authored transforms before querying the renderer's spatial index.
	for (MeshInstance3D *instance : { near, far, masked }) {
		instance->force_update_transform();
	}
	mesh->get_surface_count();
	const uint64_t initial = LRTVolume3DTestAccess::shadow_signature(volume);
	far->set_position(Vector3(25, 0, 0));
	far->force_update_transform();
	CHECK(LRTVolume3DTestAccess::shadow_signature(volume) == initial);
	masked->set_position(Vector3(2, 0, 0));
	masked->force_update_transform();
	CHECK(LRTVolume3DTestAccess::shadow_signature(volume) == initial);
	near->set_position(Vector3(1, 0, 0));
	near->force_update_transform();
	CHECK(LRTVolume3DTestAccess::shadow_signature(volume) != initial);
	near->set_position(Vector3(20, 0, 0));
	near->force_update_transform();
	const uint64_t outside = LRTVolume3DTestAccess::shadow_signature(volume);
	near->set_position(Vector3(21, 0, 0));
	near->force_update_transform();
	CHECK(LRTVolume3DTestAccess::shadow_signature(volume) == outside);
	near->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	near->set_position(Vector3());
	near->force_update_transform();
	const uint64_t no_overlay = LRTVolume3DTestAccess::shadow_signature(volume);
	Ref<StandardMaterial3D> overlay;
	overlay.instantiate();
	overlay->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	near->set_material_overlay(overlay);
	const uint64_t with_overlay = LRTVolume3DTestAccess::shadow_signature(volume);
	CHECK(with_overlay != no_overlay);
	overlay->set_alpha_scissor_threshold(0.25);
	const uint64_t edited_overlay = LRTVolume3DTestAccess::shadow_signature(volume);
	CHECK(edited_overlay != with_overlay);
	Ref<StandardMaterial3D> next;
	next.instantiate();
	overlay->set_next_pass(next);
	const uint64_t with_next = LRTVolume3DTestAccess::shadow_signature(volume);
	CHECK(with_next != edited_overlay);
	next->set_grow_enabled(true);
	next->set_grow(0.2);
	CHECK(LRTVolume3DTestAccess::shadow_signature(volume) != with_next);
	near->set_material_overlay(Ref<Material>());
	CHECK(LRTVolume3DTestAccess::shadow_signature(volume) == no_overlay);
	memdelete(root);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[LRT] Editing Volume configuration invalidates saved export identities immediately") {
	const bool previous_hint = Engine::get_singleton()->is_editor_hint();
	Engine::get_singleton()->set_editor_hint(true);
	LRTVolume3D *volume = memnew(LRTVolume3D);
	Dictionary metadata;
	metadata["runtime_only"] = true;
	volume->set_build_cache_fingerprint(123);
	volume->set_build_cache_export_data(metadata);
	volume->set_probe_spacing(0.25);
	const int64_t spacing_key = volume->get_build_cache_fingerprint();
	const bool spacing_metadata_empty = volume->get_build_cache_export_data().is_empty();
	const bool original_metadata_preserved = bool(metadata.get("runtime_only", false));
	volume->set_build_cache_fingerprint(456);
	volume->set_build_cache_export_data(metadata);
	volume->set_size(Vector3(8, 8, 8));
	const int64_t size_key = volume->get_build_cache_fingerprint();
	const bool size_metadata_empty = volume->get_build_cache_export_data().is_empty();
	Engine::get_singleton()->set_editor_hint(previous_hint);
	CHECK(spacing_key == 0);
	CHECK(size_key == 0);
	CHECK(spacing_metadata_empty);
	CHECK(original_metadata_preserved);
	CHECK(size_metadata_empty);
	memdelete(volume);
}
#endif

} // namespace TestLRTInputs
