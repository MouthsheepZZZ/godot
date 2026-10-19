/**************************************************************************/
/*  lrt_volume_3d.cpp                                                     */
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
/* The above copyright notice and this permission notice shall be        */
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

#include "lrt_volume_3d.h"

#include "lrt_display_shaders.h"
#include "lrt_cache.h"
#include "lrt_render_bridge.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/cpu_particles_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/multimesh.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

#include "scene/resources/shader.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#ifdef TOOLS_ENABLED
#include "editor/scene/3d/node_3d_editor_plugin.h"
#endif

namespace {

// The prototype's light `power` is the irradiance scale the source term multiplies into the
// scattered radiance, and the engine's non-physical light units need PI before the diffuse
// BRDF divides by it again (light_storage.cpp multiplies by PI, light_compute divides).
constexpr double LIGHT_INTENSITY_SCALE = 3.14159265358979323846;
// Godot's own inverse-square attenuation (`omni_attenuation == 2`), the engine parameter
// form of the prototype's `power / max(d^2, 0.04)` falloff.
constexpr double INVERSE_SQUARE_ATTENUATION = 2.0;
// Surfaces at or above this metallic value carry no diffuse term in the prototype.
constexpr double DEFAULT_ALBEDO[3] = { 0.72, 0.72, 0.68 };
constexpr int SKY_PANORAMA_WIDTH = 64;
constexpr int SKY_PANORAMA_HEIGHT = 32;
// JSON-style base properties every resource has; skipped when hashing a sky material.
const char *const BASE_RESOURCE_PROPERTIES[] = {
	"resource_local_to_scene", "resource_path", "resource_name", "script"
};

Light3D *light_from_id(ObjectID p_id) {
	return Object::cast_to<Light3D>(ObjectDB::get_instance(p_id));
}

GeometryInstance3D *geometry_from_id(ObjectID p_id) {
	return Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(p_id));
}

bool is_instanced_geometry(GeometryInstance3D *p_instance) {
	return Object::cast_to<MultiMeshInstance3D>(p_instance) != nullptr || Object::cast_to<CPUParticles3D>(p_instance) != nullptr;
}

Ref<Mesh> geometry_mesh(GeometryInstance3D *p_instance, bool p_include_invisible = false) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_instance);
	if (mesh_instance != nullptr) {
		return mesh_instance->get_mesh();
	}
	CPUParticles3D *particles = Object::cast_to<CPUParticles3D>(p_instance);
	if (particles != nullptr) {
		if (RS::get_singleton()->multimesh_get_visible_instances(particles->get_base()) != 0) {
			return particles->get_mesh();
		}
		return Ref<Mesh>();
	}
	MultiMeshInstance3D *multimesh_instance = Object::cast_to<MultiMeshInstance3D>(p_instance);
	if (multimesh_instance != nullptr) {
		const Ref<MultiMesh> multimesh = multimesh_instance->get_multimesh();
		if (multimesh.is_valid() && multimesh->get_instance_count() > 0 && (p_include_invisible || multimesh->get_visible_instance_count() != 0)) {
			return multimesh->get_mesh();
		}
		return Ref<Mesh>();
	}
	// CSG exposes its final root mesh through this existing interface. Children have no
	// render mesh; do not capture their operands separately or depend on the CSG module.
	if (p_instance->is_class("CSGShape3D")) {
		const Array meshes = p_instance->call("get_meshes");
		if (meshes.size() == 2) {
			return meshes[1];
		}
	}
	return Ref<Mesh>();
}

lrt::Vec3 to_lrt(const Vector3 &p_value) {
	return lrt::Vec3(p_value.x, p_value.y, p_value.z);
}

lrt::PrimitiveTransform to_lrt_transform(const Transform3D &p_transform) {
	lrt::PrimitiveTransform result;
	result.origin = to_lrt(p_transform.origin);
	const Basis basis = p_transform.basis;
	result.basis_x = to_lrt(basis.get_column(0));
	result.basis_y = to_lrt(basis.get_column(1));
	result.basis_z = to_lrt(basis.get_column(2));
	return result;
}

// Conservative caster support for Source invalidation; this never requests a shadow draw.
Vector<Plane> directional_shadow_input_planes(const Transform3D &p_volume_to_world, const AABB &p_bounds, const Transform3D &p_light_transform, float p_shadow_angle) {
	const Basis basis = p_light_transform.basis.orthonormalized();
	const Vector3 center = p_volume_to_world.xform(p_bounds.get_center());
	Vector3 minimum;
	Vector3 maximum;
	real_t radius = 0;
	for (int corner = 0; corner < 8; corner++) {
		const Vector3 point = p_volume_to_world.xform(p_bounds.get_endpoint(corner));
		radius = MAX(radius, center.distance_to(point));
		for (int axis = 0; axis < 3; axis++) {
			const real_t distance = basis.get_column(axis).dot(point);
			minimum[axis] = corner == 0 ? distance : MIN(minimum[axis], distance);
			maximum[axis] = corner == 0 ? distance : MAX(maximum[axis], distance);
		}
	}
	const real_t expand = p_shadow_angle > 0 ? Math::tan(Math::deg_to_rad(p_shadow_angle)) * radius * 2 : 0;
	return Vector<Plane>({ Plane(basis.get_column(0), maximum.x + expand), Plane(-basis.get_column(0), -minimum.x + expand),
		Plane(basis.get_column(1), maximum.y + expand), Plane(-basis.get_column(1), -minimum.y + expand),
		Plane(basis.get_column(2), maximum.z + 1e6), Plane(-basis.get_column(2), -minimum.z) });
}

} // namespace

static uint64_t mix_signature(uint64_t p_hash, uint64_t p_value);
static uint64_t mesh_capture_bytes(const std::shared_ptr<const std::vector<lrt::MeshTriangle>> &p_triangles,
		const std::shared_ptr<const lrt::MaterialCapture> &p_material,
		const std::shared_ptr<const lrt::RasterGeometryCapture> &p_raster,
		std::set<const void *> *p_counted = nullptr);


std::map<uint64_t, std::weak_ptr<const LRTVolume3D::MeshCaptureData>> LRTVolume3D::shared_mesh_capture_cache;
std::map<ObjectID, LRTVolume3D *> LRTVolume3D::active_volumes;


LRTVolume3D::LRTVolume3D() {
	set_process(false);
}

LRTVolume3D::~LRTVolume3D() {
	_cancel_build();
}

void LRTVolume3D::_bind_methods() {
	ClassDB::bind_static_method("LRTVolume3D", D_METHOD("prepare_mesh_sdf", "mesh", "resolution"), &LRTVolume3D::prepare_mesh_sdf, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "spacing"), &LRTVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LRTVolume3D::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LRTVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LRTVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_editor_preview", "enabled"), &LRTVolume3D::set_editor_preview);
	ClassDB::bind_method(D_METHOD("is_editor_preview"), &LRTVolume3D::is_editor_preview);
	ClassDB::bind_method(D_METHOD("set_receive_sky", "enabled"), &LRTVolume3D::set_receive_sky);
	ClassDB::bind_method(D_METHOD("is_receiving_sky"), &LRTVolume3D::is_receiving_sky);
	ClassDB::bind_method(D_METHOD("set_external_gi_enabled", "enabled"), &LRTVolume3D::set_external_gi_enabled);
	ClassDB::bind_method(D_METHOD("is_external_gi_enabled"), &LRTVolume3D::is_external_gi_enabled);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &LRTVolume3D::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &LRTVolume3D::get_priority);
	ClassDB::bind_method(D_METHOD("set_blend_distance", "distance"), &LRTVolume3D::set_blend_distance);
	ClassDB::bind_method(D_METHOD("get_blend_distance"), &LRTVolume3D::get_blend_distance);
	ClassDB::bind_method(D_METHOD("set_build_cache_fingerprint", "fingerprint"), &LRTVolume3D::set_build_cache_fingerprint);
	ClassDB::bind_method(D_METHOD("get_build_cache_fingerprint"), &LRTVolume3D::get_build_cache_fingerprint);
	ClassDB::bind_method(D_METHOD("set_build_cache_export_data", "data"), &LRTVolume3D::set_build_cache_export_data);
	ClassDB::bind_method(D_METHOD("get_build_cache_export_data"), &LRTVolume3D::get_build_cache_export_data);

	ClassDB::bind_method(D_METHOD("rebuild"), &LRTVolume3D::rebuild);
	ClassDB::bind_method(D_METHOD("poll"), &LRTVolume3D::poll);
	ClassDB::bind_method(D_METHOD("set_rebuild_suppressed", "suppressed"), &LRTVolume3D::set_rebuild_suppressed);
	ClassDB::bind_method(D_METHOD("is_rebuild_suppressed"), &LRTVolume3D::is_rebuild_suppressed);
	ClassDB::bind_method(D_METHOD("get_volume_warnings"), &LRTVolume3D::get_volume_warnings);
	ClassDB::bind_method(D_METHOD("get_editor_build_state"), &LRTVolume3D::get_editor_build_state);
	ClassDB::bind_method(D_METHOD("get_editor_build_tooltip"), &LRTVolume3D::get_editor_build_tooltip);
	ClassDB::bind_method(D_METHOD("get_instance_sdf_status", "instance"), &LRTVolume3D::get_instance_sdf_status);
	ClassDB::bind_method(D_METHOD("get_instance_sdf_message", "instance"), &LRTVolume3D::get_instance_sdf_message);
	ClassDB::bind_method(D_METHOD("is_building"), &LRTVolume3D::is_building);
	ClassDB::bind_method(D_METHOD("get_error_message"), &LRTVolume3D::get_error_message);
	ClassDB::bind_method(D_METHOD("get_build_stats"), &LRTVolume3D::get_build_stats);
	ClassDB::bind_method(D_METHOD("get_preparation_status"), &LRTVolume3D::get_preparation_status);
	ClassDB::bind_method(D_METHOD("get_collection_stats"), &LRTVolume3D::get_collection_stats);
	ClassDB::bind_method(D_METHOD("get_geometry_builds"), &LRTVolume3D::get_geometry_builds);
	ClassDB::bind_method(D_METHOD("get_source_injections"), &LRTVolume3D::get_source_injections);
	ClassDB::bind_method(D_METHOD("get_dropped_builds"), &LRTVolume3D::get_dropped_builds);
	ClassDB::bind_method(D_METHOD("get_cancelled_builds"), &LRTVolume3D::get_cancelled_builds);
	ClassDB::bind_method(D_METHOD("get_solver"), &LRTVolume3D::get_solver);
	ClassDB::bind_method(D_METHOD("get_sky_radiance"), &LRTVolume3D::get_sky_radiance);
	ClassDB::bind_static_method("LRTVolume3D", D_METHOD("clear_shared_mesh_capture_cache"), &LRTVolume3D::clear_shared_mesh_capture_cache);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.05,2.0,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_GROUP("Boundary", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_distance", PROPERTY_HINT_RANGE, "0,100,0.01,suffix:m"), "set_blend_distance", "get_blend_distance");
	ADD_GROUP("Advanced", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview"), "set_editor_preview", "is_editor_preview");

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "receive_sky"), "set_receive_sky", "is_receiving_sky");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "external_gi_enabled"), "set_external_gi_enabled", "is_external_gi_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "build_cache_fingerprint", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_build_cache_fingerprint", "get_build_cache_fingerprint");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "build_cache_export_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_build_cache_export_data", "get_build_cache_export_data");

}

// --- Configuration ---------------------------------------------------------

void LRTVolume3D::set_probe_spacing(double p_spacing) {
	p_spacing = MAX(0.05, p_spacing);
	if (spacing == p_spacing) {
		return;
	}
	spacing = p_spacing;
	// The probe lattice is drawn from `spacing`, so the box has to be repainted on its own.
	update_gizmos();
	update_configuration_warnings();
	_request_rebuild(REBUILD_REASON_CONFIGURATION);
}

double LRTVolume3D::get_probe_spacing() const {
	return spacing;
}

void LRTVolume3D::set_size(const Vector3 &p_size) {
	const Vector3 clamped = p_size.max(Vector3(0.05, 0.05, 0.05));
	if (volume_size == clamped) {
		return;
	}
	volume_size = clamped;
	set_blend_distance(blend_distance);
	update_gizmos();
	update_configuration_warnings();
	_request_rebuild(REBUILD_REASON_CONFIGURATION);
}

Vector3 LRTVolume3D::get_size() const {
	return volume_size;
}

void LRTVolume3D::set_editor_preview(bool p_enabled) {
	if (editor_preview == p_enabled) {
		return;
	}
	editor_preview = p_enabled;
	if (Engine::get_singleton()->is_editor_hint()) {
		if (editor_preview) {
			_apply_display();
		} else {
			_cancel_build();
			_apply_display();
		}
	}
}

bool LRTVolume3D::is_editor_preview() const {
	return editor_preview;
}

void LRTVolume3D::set_receive_sky(bool p_enabled) {
	if (receive_sky == p_enabled) {
		return;
	}
	receive_sky = p_enabled;
	if (solver.is_valid() && _is_active()) {
		solver->set_receive_sky(p_enabled);
	}
	_update_display_parameters();
}

bool LRTVolume3D::is_receiving_sky() const {
	return receive_sky;
}

void LRTVolume3D::set_external_gi_enabled(bool p_enabled) {
	if (external_gi_enabled == p_enabled) {
		return;
	}
	external_gi_enabled = p_enabled;
	if (solver.is_valid()) {
		solver->set_external_gi_enabled(p_enabled);
	}
	_update_display_parameters();
}

bool LRTVolume3D::is_external_gi_enabled() const {
	return external_gi_enabled;
}

void LRTVolume3D::set_priority(int p_priority) {
	if (priority == p_priority) {
		return;
	}
	priority = p_priority;
	_update_display_parameters();
}

int LRTVolume3D::get_priority() const {
	return priority;
}

void LRTVolume3D::set_blend_distance(double p_distance) {
	const double maximum = MAX(0.0, MIN(volume_size.x, MIN(volume_size.y, volume_size.z)) * 0.5);
	const double clamped = CLAMP(p_distance, 0.0, maximum);
	if (Math::is_equal_approx(blend_distance, clamped)) {
		return;
	}
	blend_distance = clamped;
	update_gizmos();
	_update_display_parameters();
}

double LRTVolume3D::get_blend_distance() const {
	return blend_distance;
}

void LRTVolume3D::set_build_cache_fingerprint(int64_t p_fingerprint) {
	serialized_build_cache_fingerprint = uint64_t(p_fingerprint);
}

int64_t LRTVolume3D::get_build_cache_fingerprint() const {
	return int64_t(serialized_build_cache_fingerprint);
}

void LRTVolume3D::set_build_cache_export_data(const Dictionary &p_data) {
	serialized_export_data = p_data;
}

Dictionary LRTVolume3D::get_build_cache_export_data() const {
	return serialized_export_data;
}

// --- Public operations -----------------------------------------------------

// Configuration edits use the same coalescing queue as scene edits. A running build is allowed
// to finish and apply before the newest snapshot starts; generations therefore only move
// forward, while continuous motion can never cancel every build before it becomes visible.
void LRTVolume3D::_request_rebuild(uint32_t p_reasons) {
	if (Engine::get_singleton()->is_editor_hint() && (p_reasons & REBUILD_REASON_CONFIGURATION)) {
		editor_build_dirty = true;
		editor_rebuild_requested = true;
		serialized_build_cache_fingerprint = 0;
		serialized_export_data = Dictionary();
	}
	has_geometry_signature = false;
	has_material_state_signature = false;
	if (_is_active()) {
		_queue_build(p_reasons);
	}
}

void LRTVolume3D::rebuild() {
	if (!_is_active()) {
		return;
	}
	if (Engine::get_singleton()->is_editor_hint()) {
		editor_rebuild_requested = true;
		build_data_missing = false;
	}
	_collect_geometry();
	_collect_lights();
	// This explicit rebuild already captured the current input. Keep the polling signature in
	// sync so the next frame does not schedule the same build a second time.
	geometry_signature = _geometry_signature();
	material_state_signature = _material_state_signature();
	has_geometry_signature = true;
	has_material_state_signature = true;
	_queue_build(REBUILD_REASON_FORCED);
}

void LRTVolume3D::set_rebuild_suppressed(bool p_suppressed) {
	if (rebuild_suppressed == p_suppressed) {
		return;
	}
	rebuild_suppressed = p_suppressed;
}

bool LRTVolume3D::is_rebuild_suppressed() const {
	return rebuild_suppressed;
}

// Runs one frame of the node's logic immediately: input refresh, a finished background bake
// and propagation within the configured budget. Tests and headless tools use it to
// drive the volume without waiting for frames.
void LRTVolume3D::poll() {
	_refresh_frame();
}

String LRTVolume3D::get_editor_build_state() const {
	if (building || local_apply_pending || rebuild_pending) {
		return "Building";
	}
	if (!error_message.is_empty()) {
		return "Failed";
	}
	if (build_data_missing) {
		return "Build Data Missing";
	}
	if (editor_build_dirty) {
		return has_applied_configuration ? "Out of Date" : "Not Built";
	}
	return has_applied_configuration && !build_stats.is_empty() ? "Ready" : "Not Built";
}

String LRTVolume3D::get_editor_build_tooltip() const {
	const lrt::Grid probe_grid = lrt::make_grid_sized(spacing,
			lrt::Vec3(-volume_size.x * 0.5, -volume_size.y * 0.5, -volume_size.z * 0.5),
			lrt::Vec3(volume_size.x, volume_size.y, volume_size.z));
	const int64_t probe_count = int64_t(probe_grid.size[0]) * probe_grid.size[1] * probe_grid.size[2];
	const Dictionary collection = get_collection_stats();
	String text = vformat("State: %s\nProbe Grid: %d x %d x %d (%d probes)\nSDF Contributors: %d",
			get_editor_build_state(), probe_grid.size[0], probe_grid.size[1], probe_grid.size[2], probe_count,
			int(collection.get("contributors", 0)));
	if (solver.is_valid() && solver->has_local_field()) {
		const uint64_t volume_bytes = uint64_t(solver->get_memory_stats().get("total_bytes", 0));
		const Dictionary bridge = LRTRenderBridge::get_performance_stats(get_instance_id());
		const uint64_t shared_bytes = uint64_t(bridge.get("shared_gpu_bytes", 0)) + uint64_t(bridge.get("screen_gather_bytes", 0));
		text += vformat("\nVolume GPU Memory: %.1f MiB\nShared/View LRT GPU Memory: %.1f MiB",
				double(volume_bytes) / (1024.0 * 1024.0), double(shared_bytes) / (1024.0 * 1024.0));
	}
	if (last_build_latency_ms > 0.0) {
		text += vformat("\nLast Build: %.1f ms", last_build_latency_ms);
	}
	if (!error_message.is_empty()) {
		text += "\n" + error_message;
	}
	return text;
}

String LRTVolume3D::get_instance_sdf_status(Node3D *p_instance) const {
	ERR_FAIL_NULL_V(p_instance, "Failed");
	if (p_instance->get_world_3d() != get_world_3d() || !p_instance->is_visible_in_tree()) {
		return "Not Contributing";
	}
	bool contributes = false;
	bool receives_only = false;
	for (const Receiver &receiver : receivers) {
		if (receiver.instance_id != p_instance->get_instance_id()) {
			continue;
		}
		if (receiver.gi_enabled && !receiver.material_error.is_empty()) {
			return "Failed";
		}
		contributes = contributes || receiver.contributes;
		receives_only = receives_only || !receiver.contributes;
	}
	if (!contributes) {
		return receives_only ? "Receive Only" : "Not Contributing";
	}
	if (!error_message.is_empty()) {
		return "Failed";
	}
	if (building || local_apply_pending || rebuild_pending) {
		return "Building";
	}
	return build_stats.is_empty() ? "Not Built" : "Ready";
}

bool LRTVolume3D::is_building() const {
	return building || local_apply_pending || rebuild_pending;
}

String LRTVolume3D::get_error_message() const {
	return error_message;
}

Dictionary LRTVolume3D::get_build_stats() const {
	return build_stats;
}

Dictionary LRTVolume3D::get_preparation_status() const {
	Dictionary status = solver.is_valid() ? solver->get_preparation_status() : Dictionary();
	const bool native_update_pending = native_capture_pending || deferred_receiver_unit_field_frame != UINT64_MAX ||
			(solver.is_valid() && (solver->is_native_light_resolve_pending() || solver->is_injection_pending()));
	if (!error_message.is_empty()) {
		status["state"] = "failed";
		status["message"] = error_message;
	} else if (building) {
		status["state"] = "preparing";
		status["message"] = rebuild_pending ? "LRT 正在构建；已合并一个较新的输入快照" : "LRT 正在准备局部数据";
	} else if (local_apply_pending) {
		status["state"] = "uploading";
		status["message"] = rebuild_pending ? "LRT 正在异步上传；已合并一个较新的输入快照" : "LRT 正在异步上传局部数据";
	} else if (rebuild_pending) {
		status["state"] = "queued";
		status["message"] = "LRT 已合并输入变化，等待启动构建";
	} else if (native_update_pending) {
		status["state"] = "recapturing_lights";
		status["message"] = "LRT 正在 GPU 更新原生灯光与阴影；完整快照连续混合，传播持续运行";
	} else if (!build_stats.is_empty()) {
		status["state"] = "ready";
		status["message"] = "LRT 局部数据已就绪";
	} else {
		status["state"] = "waiting";
		status["message"] = "LRT 等待相交的贡献几何";
	}
	status["generation"] = generation;
	status["applied_generation"] = applied_generation;
	status["queued"] = rebuild_pending;
	status["active_rebuild_reasons"] = int64_t(active_rebuild_reasons);
	status["queued_rebuild_reasons"] = int64_t(pending_rebuild_reasons);
	status["applied_rebuild_reasons"] = int64_t(applied_rebuild_reasons);
	status["native_light_capture_pending"] = native_update_pending;
	status["native_light_capture_queued"] = native_capture_queued || deferred_receiver_unit_field_frame != UINT64_MAX;
	status["native_source_ready"] = native_source_ready;
	status["native_light_set_signature"] = int64_t(active_native_light_set_signature);
	status["native_light_capture_count"] = native_capture_count;
	status["native_light_capture_gpu_resolves"] = native_capture_gpu_resolves;
	status["native_light_capture_gpu_resolve_pending"] = solver.is_valid() && solver->is_native_light_resolve_pending();
	status["native_light_capture_latency_ms"] = native_capture_last_latency_ms;
	status["native_shadowed_light_count"] = native_capture_shadowed_count;
	status["native_light_capture_updates"] = native_capture_updates;
	status["native_light_snapshot_count"] = int64_t(native_light_snapshots.size());
	status["native_light_diagnostic_count"] = int64_t(native_light_diagnostics.size());
	status["geometry_candidate_count"] = int64_t(geometry_candidates.size());
	status["light_candidate_count"] = int64_t(light_candidates.size());
	status["native_shadow_signature"] = int64_t(shadow_capture_signature);
	status["geometry_update_count"] = int64_t(geometry_update_count);
	status["propagation_iterations_per_frame"] = MAX(1, int(GLOBAL_GET("rendering/global_illumination/lrt/propagation/iterations_per_frame")));
	status["scheduler_frame"] = int64_t(scheduler_frame);
	status["last_frame_work_ms"] = last_frame_work_ms;
	status["peak_frame_work_ms"] = peak_frame_work_ms;
	Dictionary frame_cpu_breakdown;
	frame_cpu_breakdown["collect_geometry_ms"] = last_collect_geometry_ms;
	frame_cpu_breakdown["geometry_update_ms"] = last_geometry_update_ms;
	frame_cpu_breakdown["geometry_input_ms"] = last_geometry_input_ms;
	frame_cpu_breakdown["collect_lights_ms"] = last_collect_lights_ms;
	frame_cpu_breakdown["environment_ms"] = last_environment_ms;
	frame_cpu_breakdown["geometry_signature_ms"] = last_geometry_signature_ms;
	frame_cpu_breakdown["material_signature_ms"] = last_material_signature_ms;
	frame_cpu_breakdown["shadow_signature_ms"] = last_shadow_signature_ms;
	frame_cpu_breakdown["sky_input_ms"] = last_sky_input_ms;
	frame_cpu_breakdown["build_poll_ms"] = last_build_poll_ms;
	frame_cpu_breakdown["apply_begin_ms"] = last_apply_begin_ms;
	frame_cpu_breakdown["apply_finish_ms"] = last_apply_finish_ms;
	frame_cpu_breakdown["build_publish_ms"] = last_build_publish_ms;
	frame_cpu_breakdown["native_input_ms"] = last_native_input_ms;
	frame_cpu_breakdown["native_resolve_poll_ms"] = last_native_resolve_poll_ms;
	frame_cpu_breakdown["display_update_ms"] = last_display_update_ms;
	frame_cpu_breakdown["propagation_schedule_ms"] = last_propagation_schedule_ms;
	status["frame_cpu_breakdown"] = frame_cpu_breakdown;
	status["last_frame_propagation_iterations"] = last_frame_propagation_iterations;
	status["propagation_iterations_remaining"] = propagation_iterations_remaining;
	status["build_latency_ms"] = last_build_latency_ms;
	status["build_start_frame"] = int64_t(build_start_frame);
	status["build_done_frame"] = int64_t(build_done_frame);
	status["build_apply_frame"] = int64_t(build_apply_frame);
	const uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
	const uint64_t build_queued_usec = rebuild_pending ? pending_build_queued_usec : active_build_queued_usec;
	status["build_queue_age_ms"] = (building || local_apply_pending || rebuild_pending) && build_queued_usec > 0 ?
			double(now_usec - build_queued_usec) / 1000.0 : 0.0;
	uint64_t source_queued_usec = 0;
	if ((native_capture_queued || native_update_pending) && native_capture_queued_usec > 0) {
		source_queued_usec = native_capture_queued_usec;
	} else if (native_capture_pending && native_capture_active_started_usec > 0) {
		source_queued_usec = native_capture_active_started_usec;
	}
	status["source_queue_age_ms"] = source_queued_usec > 0 ?
			double(now_usec - source_queued_usec) / 1000.0 : 0.0;
	Array displayed_light_ages;
	double oldest_displayed_light_age_ms = 0.0;
	if (solver.is_valid()) {
		const Array inputs = solver->get_performance_stats().get("displayed_light_inputs", Array());
		for (int i = 0; i < inputs.size(); i++) {
			Dictionary input = inputs[i];
			const uint64_t input_usec = int64_t(input["oldest_input_usec"]);
			// Zero denotes a buffer that has never received a captured snapshot.
			const double age_ms = input_usec == 0 ? -1.0 : double(now_usec - MIN(now_usec, input_usec)) / 1000.0;
			input["age_ms"] = age_ms;
			displayed_light_ages.push_back(input);
			oldest_displayed_light_age_ms = MAX(oldest_displayed_light_age_ms, age_ms);
		}
	}
	status["displayed_light_snapshot_ages"] = displayed_light_ages;
	status["oldest_displayed_light_snapshot_age_ms"] = oldest_displayed_light_age_ms;
	status["native_light_diagnostics"] = native_light_diagnostics;
	status["external_gi_requested"] = external_gi_enabled;
	status["external_gi_provider_status"] = int(LRTRenderBridge::get_external_gi_status(get_instance_id()));
	status["external_gi_active"] = _is_external_gi_active() && LRTRenderBridge::is_external_gi_capture_valid(get_instance_id());
	status["external_gi_capture_valid"] = LRTRenderBridge::is_external_gi_capture_valid(get_instance_id());
	status["external_gi_capture_count"] = int64_t(LRTRenderBridge::get_external_gi_capture_count(get_instance_id()));
	status["environment_capture_pending"] = environment_capture_pending;
	status["environment_capture_submitted"] = environment_capture_submitted;
	status["external_gi_path"] = "hddagi_boundary_rgb_e_over_pi";
	status["external_gi_writeback"] = false;
	status["external_gi_trace_queries"] = 0;
	const Dictionary bridge_stats = LRTRenderBridge::get_performance_stats(get_instance_id());
	status["render_bridge_performance"] = bridge_stats;
	uint64_t volume_gpu_bytes = 0;
	for (const auto &entry : active_volumes) {
		if (entry.second->solver.is_valid()) {
			volume_gpu_bytes += uint64_t(entry.second->solver->get_memory_stats().get("total_bytes", 0));
		}
	}
	const uint64_t shared_gpu_bytes = uint64_t(bridge_stats.get("shared_gpu_bytes", 0));
	const uint64_t screen_gpu_bytes = uint64_t(bridge_stats.get("screen_gather_bytes", 0));
	status["lrt_volume_gpu_bytes"] = int64_t(volume_gpu_bytes);
	status["lrt_shared_gpu_bytes"] = int64_t(shared_gpu_bytes);
	status["lrt_screen_gpu_bytes"] = int64_t(screen_gpu_bytes);
	status["lrt_total_gpu_bytes"] = int64_t(volume_gpu_bytes + shared_gpu_bytes + screen_gpu_bytes);
	uint64_t active_mesh_capture_bytes = 0;
	std::set<const void *> active_mesh_capture_allocations;
	for (const auto &entry : mesh_capture_cache) {
		const MeshCaptureData &capture = *entry.second.data;
		active_mesh_capture_bytes += mesh_capture_bytes(capture.triangles, capture.material,
				capture.raster_capture, &active_mesh_capture_allocations);
	}
	uint64_t shared_mesh_capture_bytes = 0;
	std::set<const void *> shared_mesh_capture_allocations;
	for (const auto &entry : shared_mesh_capture_cache) {
		const std::shared_ptr<const MeshCaptureData> capture = entry.second.lock();
		if (capture) {
			shared_mesh_capture_bytes += mesh_capture_bytes(capture->triangles, capture->material,
					capture->raster_capture, &shared_mesh_capture_allocations);
		}
	}
	status["mesh_capture_active_bytes"] = int64_t(active_mesh_capture_bytes);
	status["mesh_capture_shared_cache_bytes"] = int64_t(shared_mesh_capture_bytes);
	status["mesh_capture_shared_cache_entries"] = int64_t(shared_mesh_capture_cache.size());
	status["voxel_captures"] = int64_t(voxel_captures);
	status["voxel_capture_reuses"] = int64_t(voxel_capture_reuses);
	status["voxel_cache_loads"] = int64_t(voxel_cache_loads);
	status["blend_distance"] = blend_distance;
	return status;
}

Dictionary LRTVolume3D::get_collection_stats() const {
	Dictionary result;
	int contributors = 0;
	int authored_overlays = 0;
	int unsupported_materials = 0;
	String material_message;
	for (const Receiver &receiver : receivers) {
		contributors += receiver.contributes ? 1 : 0;
		authored_overlays += receiver.authored_overlay.is_valid() ? 1 : 0;
		if (!receiver.material_error.is_empty() && receiver.gi_enabled) {
			unsupported_materials++;
			if (material_message.is_empty()) {
				material_message = receiver.material_error;
			}
		}
	}
	result["receivers"] = int(receivers.size());
	result["contributors"] = contributors;
	result["authored_overlays"] = authored_overlays;
	result["lights"] = int(lights.size());
	result["receiver_path"] = "forward_plus_local_field";
	result["receiver_trace_queries"] = 0;
	result["receiver_bounds_test"] = "per_fragment_world_position";
	result["external_gi_path"] = "hddagi_boundary_rgb_e_over_pi";
	result["external_gi_writeback"] = false;
	result["external_gi_trace_queries"] = 0;
	result["unsupported_materials"] = unsupported_materials;
	result["material_message"] = material_message;
	return result;
}

static Transform3D canonical_volume_transform(const Transform3D &p_transform) {
	Transform3D result = p_transform;
	constexpr double step = 0.00001;
	for (int axis = 0; axis < 3; axis++) {
		result.origin[axis] = Math::snapped(result.origin[axis], step);
		for (int column = 0; column < 3; column++) {
			result.basis[axis][column] = Math::snapped(result.basis[axis][column], step);
		}
	}
	return result;
}

// Cancelling two independently accumulated global transforms loses precision while a common
// carrier moves far from the origin. Compose only the local chains below their shared Node3D
// ancestor so carrier motion is exactly absent from the volume-local input.
static Transform3D relative_node_transform(const Node3D *p_from, const Node3D *p_to) {
	for (const Node3D *from_ancestor = p_from; from_ancestor != nullptr; from_ancestor = from_ancestor->get_parent_node_3d()) {
		for (const Node3D *to_ancestor = p_to; to_ancestor != nullptr; to_ancestor = to_ancestor->get_parent_node_3d()) {
			if (from_ancestor == to_ancestor) {
				return p_from->get_relative_transform(from_ancestor).affine_inverse() * p_to->get_relative_transform(from_ancestor);
			}
		}
	}
	return p_from->get_global_transform().affine_inverse() * p_to->get_global_transform();
}

int LRTVolume3D::get_geometry_builds() const {
	return geometry_builds;
}

int LRTVolume3D::get_source_injections() const {
	return source_injections;
}

int LRTVolume3D::get_dropped_builds() const {
	return dropped_builds;
}

int LRTVolume3D::get_cancelled_builds() const {
	return cancelled_builds;
}

Ref<LRTVolume> LRTVolume3D::get_solver() const {
	return solver;
}

PackedVector4Array LRTVolume3D::get_sky_radiance() const {
	return sky_radiance;
}

bool LRTVolume3D::_is_external_gi_active() const {
	return is_visible_in_tree() && transform_valid && external_gi_enabled && solver.is_valid() && solver->has_external_gi_buffers();
}

bool LRTVolume3D::_is_active() const {
	if (!is_inside_tree() || !is_visible_in_tree()) {
		return false;
	}
	if (Engine::get_singleton()->is_editor_hint() && !editor_preview) {
		return false;
	}
	return true;
}

void LRTVolume3D::_refresh_propagation_budget() {
	if (solver.is_null() || !solver->has_local_field()) {
		return;
	}
	const int budget = MAX(1, int(GLOBAL_GET("rendering/global_illumination/lrt/propagation/iterations_after_change")));
	const uint64_t local_version = solver->get_local_field_version();
	const uint64_t source_version = solver->get_source_version();
	const uint64_t boundary_version = external_gi_enabled ? LRTRenderBridge::get_external_gi_capture_count(get_instance_id()) : 0;
	const bool boundary_valid = external_gi_enabled && LRTRenderBridge::is_external_gi_capture_valid(get_instance_id());
	if (budget != propagation_iterations_after_change || local_version != propagation_local_version ||
			source_version != propagation_source_version || boundary_version != propagation_boundary_version ||
			boundary_valid != propagation_boundary_valid) {
		propagation_iterations_remaining = MAX(propagation_iterations_remaining, budget);
		propagation_iterations_after_change = budget;
		propagation_local_version = local_version;
		propagation_source_version = source_version;
		propagation_boundary_version = boundary_version;
		propagation_boundary_valid = boundary_valid;
	}
}

int LRTVolume3D::_propagation_iterations_per_frame() const {
	if (!_is_active() || !transform_valid || !error_message.is_empty() || propagation_iterations_remaining == 0 ||
			propagation_last_frame == Engine::get_singleton()->get_process_frames() ||
			solver.is_null() || !solver->has_local_field() || solver->get_pending_step_iterations() != 0 ||
			(local_apply_pending && !solver->can_step_while_applying())) {
		return 0;
	}
	return MIN(propagation_iterations_remaining,
			MAX(1, int(GLOBAL_GET("rendering/global_illumination/lrt/propagation/iterations_per_frame"))));
}

Vector3 LRTVolume3D::_effective_volume_size() const {
	if (Engine::get_singleton()->is_editor_hint() && editor_build_dirty && has_applied_configuration && !editor_rebuild_requested) {
		return applied_volume_size;
	}
	return volume_size;
}

double LRTVolume3D::_effective_spacing() const {
	if (Engine::get_singleton()->is_editor_hint() && editor_build_dirty && has_applied_configuration && !editor_rebuild_requested) {
		return applied_spacing;
	}
	return spacing;
}

Node *LRTVolume3D::_scene_tree_root() const {
	SceneTree *tree = get_tree();
	return tree != nullptr ? tree->get_root() : nullptr;
}

bool LRTVolume3D::_has_valid_volume_transform() const {
	return get_global_transform().basis.is_rotation();
}

static bool material_pass_contributes(const Ref<Material> &p_material);
static bool material_chain_contributes(const Ref<Material> &p_material);
static bool shadow_material_uses_time(const Ref<Material> &p_material);

bool LRTVolume3D::_intersects_volume(GeometryInstance3D *p_instance) {
	if (p_instance == nullptr) {
		return false;
	}
	const Ref<Mesh> mesh = geometry_mesh(p_instance);
	if (mesh.is_null()) {
		return false;
	}
	_effective_sdf_resolution(mesh);
	const Transform3D to_volume = relative_node_transform(this, p_instance);
	const AABB bounds = is_instanced_geometry(p_instance) ?
			RS::get_singleton()->multimesh_get_aabb(p_instance->get_base()) : resource_dependencies.at(mesh->get_instance_id()).geometry_bounds;
	const Vector3 size = _effective_volume_size();
	return AABB(-size * 0.5, size).intersects_inclusive(to_volume.xform(bounds));
}

// --- Scene inputs ----------------------------------------------------------

static uint64_t mesh_content_signature(const Ref<Mesh> &p_mesh);

void LRTVolume3D::_mark_scene_candidates_dirty() {
	scene_candidates_dirty = true;
	display_collection_dirty = true;
}

void LRTVolume3D::_refresh_scene_candidates() {
	if (!scene_candidates_dirty) {
		return;
	}
	geometry_candidates.clear();
	gridmap_candidates.clear();
	light_candidates.clear();
	Node *root = _scene_tree_root();
	if (root != nullptr) {
		const TypedArray<Node> meshes = root->find_children("*", "GeometryInstance3D", true, false);
		geometry_candidates.reserve(meshes.size());
		for (int i = 0; i < meshes.size(); i++) {
			GeometryInstance3D *mesh = Object::cast_to<GeometryInstance3D>(meshes[i]);
			if (mesh != nullptr) {
				geometry_candidates.push_back(mesh->get_instance_id());
			}
		}
		const TypedArray<Node> gridmaps = root->find_children("*", "GridMap", true, false);
		for (int i = 0; i < gridmaps.size(); i++) {
			Node *gridmap = Object::cast_to<Node>(gridmaps[i]);
			gridmap_candidates.push_back(gridmap->get_instance_id());
		}
		const TypedArray<Node> scene_lights = root->find_children("*", "Light3D", true, false);
		light_candidates.reserve(scene_lights.size());
		for (int i = 0; i < scene_lights.size(); i++) {
			Light3D *light = Object::cast_to<Light3D>(scene_lights[i]);
			if (light != nullptr) {
				light_candidates.push_back(light->get_instance_id());
			}
		}
	}
	scene_candidates_dirty = false;
}

// The SceneTree topology signal refreshes the candidate IDs. Per-frame collection checks only
// those candidates, so transforms, visibility and material changes remain live without walking
// unrelated nodes. Static and Dynamic contribute; Disabled receives without entering the local field.
void LRTVolume3D::_collect_geometry(bool p_all_inputs) {
	_refresh_scene_candidates();
	for (auto &dependency : resource_dependencies) {
		dependency.second.used = false;
	}
	std::vector<Receiver> next;
	std::map<ObjectID, uint64_t> material_signatures;
	std::map<GeometryKey, const Receiver *> previous_by_id;
	for (const Receiver &existing : receivers) {
		previous_by_id[existing.get_key()] = &existing;
	}
	next.reserve(receivers.size());
	{
		for (const ObjectID candidate_id : geometry_candidates) {
			GeometryInstance3D *mesh_instance = geometry_from_id(candidate_id);
			if (mesh_instance == nullptr || mesh_instance->get_world_3d() != get_world_3d()) {
				continue;
			}
			if (!p_all_inputs && !mesh_instance->is_visible_in_tree()) {
				continue;
			}
			const Ref<Mesh> mesh = geometry_mesh(mesh_instance, p_all_inputs);
			if (mesh.is_null() || Object::cast_to<CPUParticles3D>(mesh_instance)) {
				continue;
			}
			const uint64_t mesh_revision = _resource_dependency_revision(mesh);
			if (!p_all_inputs && !_intersects_volume(mesh_instance)) {
				continue;
			}
			Receiver entry;
			entry.instance_id = mesh_instance->get_instance_id();
			entry.render_instance = mesh_instance->get_instance();
			entry.mesh = mesh;
			entry.transform = canonical_volume_transform(relative_node_transform(this, mesh_instance));
			entry.layer_mask = mesh_instance->get_layer_mask();
			entry.sdf_resolution = _effective_sdf_resolution(mesh, mesh_instance);
			entry.gi_enabled = mesh_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED;
			if (is_instanced_geometry(mesh_instance)) {
				entry.multimesh = mesh_instance->get_base();
			}
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				entry.materials.push_back(_surface_material(mesh_instance, surface));
			}
			entry.authored_overlay = mesh_instance->get_material_overlay();
			entry.mesh_content_signature = mix_signature(mesh->get_rid().get_id(), mesh_revision);
			entry.mesh_content_signature = mix_signature(entry.mesh_content_signature, uint64_t(mesh->get_surface_count()));
			if (is_instanced_geometry(mesh_instance) && mesh_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED) {
				entry.instances_signature = RS::get_singleton()->instance_get_geometry_version(mesh_instance->get_instance());
			}
			entry.material_revision_signature = _material_revision_signature(entry);
			const auto previous_entry = previous_by_id.find(entry.get_key());
			const Receiver *previous = previous_entry != previous_by_id.end() ? previous_entry->second : nullptr;
			if (previous != nullptr && previous->material_revision_signature == entry.material_revision_signature) {
				entry.albedo = previous->albedo;
				entry.material_signature = previous->material_signature;
				entry.material_error = previous->material_error;
				entry.contributing_surfaces = previous->contributing_surfaces;
			} else {
				entry.albedo = _surface_albedo(mesh_instance);
				for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
					const String surface_error = _material_support_error(_surface_material(mesh_instance, surface));
					if (surface_error.is_empty()) {
						if (material_chain_contributes(entry.materials[surface]) || (entry.authored_overlay.is_valid() && material_chain_contributes(entry.authored_overlay))) {
							entry.contributing_surfaces.push_back(surface);
						}
					} else {
						entry.material_error += vformat("Surface %d: %s\n", surface, surface_error);
					}
				}
				const String overlay_error = _material_support_error(entry.authored_overlay);
				if (!overlay_error.is_empty()) {
					entry.material_error += "Overlay: " + overlay_error;
					entry.contributing_surfaces.clear();
				}
				entry.material_signature = _material_signature(entry, material_signatures);
			}
			entry.contributes = mesh_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED && !entry.contributing_surfaces.is_empty();
			next.push_back(entry);
		}
	}
	for (ObjectID candidate_id : gridmap_candidates) {
		Node3D *node = Object::cast_to<Node3D>(ObjectDB::get_instance(candidate_id));
		if (node == nullptr || node->get_world_3d() != get_world_3d() || (!p_all_inputs && !node->is_visible_in_tree())) {
			continue;
		}
		const Array meshes = node->call("get_render_meshes");
		const Transform3D transform = canonical_volume_transform(relative_node_transform(this, node));
		const Vector3 effective_size = _effective_volume_size();
		const AABB volume_bounds(-effective_size * 0.5, effective_size);
		for (int index = 0; index < meshes.size(); index += 3) {
			Receiver entry;
			entry.instance_id = candidate_id;
			entry.render_instance = meshes[index];
			entry.mesh = meshes[index + 1];
			entry.multimesh = meshes[index + 2];
			const AABB bounds = entry.multimesh.is_valid() ? RS::get_singleton()->multimesh_get_aabb(entry.multimesh) : entry.mesh->get_aabb();
			if (!p_all_inputs && !volume_bounds.intersects_inclusive(transform.xform(bounds))) {
				continue;
			}
			entry.transform = transform;
			entry.sdf_resolution = _effective_sdf_resolution(entry.mesh);
			entry.mesh_content_signature = mix_signature(entry.mesh->get_rid().get_id(), _resource_dependency_revision(entry.mesh));
			entry.mesh_content_signature = mix_signature(entry.mesh_content_signature, uint64_t(entry.mesh->get_surface_count()));
			entry.instances_signature = entry.multimesh.is_valid() ? RS::get_singleton()->instance_get_geometry_version(entry.render_instance) : 0;
			for (int surface = 0; surface < entry.mesh->get_surface_count(); surface++) {
				entry.materials.push_back(entry.mesh->surface_get_material(surface));
			}
			entry.material_revision_signature = _material_revision_signature(entry);
			const auto previous = previous_by_id.find(entry.get_key());
			if (previous != previous_by_id.end() && previous->second->material_revision_signature == entry.material_revision_signature) {
				entry.material_signature = previous->second->material_signature;
				entry.material_error = previous->second->material_error;
				entry.contributing_surfaces = previous->second->contributing_surfaces;
			} else {
				for (int surface = 0; surface < entry.materials.size(); surface++) {
					const String error = _material_support_error(entry.materials[surface]);
					if (error.is_empty()) {
						if (material_chain_contributes(entry.materials[surface]) || (entry.authored_overlay.is_valid() && material_chain_contributes(entry.authored_overlay))) {
							entry.contributing_surfaces.push_back(surface);
						}
					} else {
						entry.material_error += vformat("Surface %d: %s\n", surface, error);
					}
				}
				entry.material_signature = _material_signature(entry, material_signatures);
			}
			entry.contributes = !entry.contributing_surfaces.is_empty();
			next.push_back(std::move(entry));
		}
	}
	bool collection_changed = next.size() != receivers.size();
	if (!collection_changed) {
		for (size_t i = 0; i < next.size(); i++) {
			if (next[i].get_key() != receivers[i].get_key() || next[i].contributes != receivers[i].contributes ||
					next[i].albedo != receivers[i].albedo || next[i].mesh_content_signature != receivers[i].mesh_content_signature ||
					next[i].material_revision_signature != receivers[i].material_revision_signature ||
					next[i].material_signature != receivers[i].material_signature ||
					next[i].material_error != receivers[i].material_error) {
				collection_changed = true;
				break;
			}
		}
	}
	receivers = next;
	for (auto cache = mesh_capture_cache.begin(); cache != mesh_capture_cache.end();) {
		if (ObjectDB::get_instance(cache->second.source.instance_id) == nullptr) {
			cache = mesh_capture_cache.erase(cache);
		} else {
			// Retain local voxels and dependency revisions while hidden or outside this Volume.
			_resource_dependency_revision(cache->second.source.mesh);
			_material_revision_signature(cache->second.source);
			++cache;
		}
	}
	display_collection_dirty = display_collection_dirty || collection_changed;
	if (collection_changed) {
		update_configuration_warnings();
	}
	_release_resource_dependencies(false);
}

void LRTVolume3D::_collect_lights() {
	_refresh_scene_candidates();
	std::vector<LightEntry> next;
	{
		for (const ObjectID candidate_id : light_candidates) {
			Light3D *light = light_from_id(candidate_id);
			if (light == nullptr || light->get_world_3d() != get_world_3d()) {
				continue;
			}
			if (Object::cast_to<DirectionalLight3D>(light) == nullptr) {
				const AABB light_bounds = light->get_global_transform().xform(light->get_aabb());
				const AABB volume_bounds = get_global_transform().xform(get_aabb());
				if (!light_bounds.intersects(volume_bounds)) {
					continue;
				}
			}
			LightEntry entry;
			entry.light_id = light->get_instance_id();
			entry.visible = light->is_visible_for_rendering();
			next.push_back(entry);
		}
	}
	lights = next;
}

// The engine's own material of one surface: override, then mesh material, exactly as the
// renderer resolves it for the direct term the overlay is added to.
Ref<Material> LRTVolume3D::_surface_material(GeometryInstance3D *p_instance, int p_surface) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_instance);
	if (mesh_instance != nullptr) {
		return mesh_instance->get_active_material(p_surface);
	}
	const Ref<Material> override = p_instance->get_material_override();
	return override.is_valid() ? override : geometry_mesh(p_instance, true)->surface_get_material(p_surface);
}

// N0 recorded convention: the prototype's linear albedo is srgb_to_linear(albedo_color).
Vector3 LRTVolume3D::_material_albedo(const Ref<Material> &p_material) {
	Ref<StandardMaterial3D> standard = p_material;
	if (standard.is_valid()) {
		const Color color = standard->get_albedo().srgb_to_linear();
		return Vector3(color.r, color.g, color.b);
	}
	return Vector3(DEFAULT_ALBEDO[0], DEFAULT_ALBEDO[1], DEFAULT_ALBEDO[2]);
}

Vector3 LRTVolume3D::_surface_albedo(GeometryInstance3D *p_instance) {
	Ref<Mesh> mesh = geometry_mesh(p_instance, true);
	if (mesh.is_valid() && mesh->get_surface_count() > 0) {
		return _material_albedo(_surface_material(p_instance, 0));
	}
	return Vector3(DEFAULT_ALBEDO[0], DEFAULT_ALBEDO[1], DEFAULT_ALBEDO[2]);
}

static bool shader_next_word_bounds(const String &p_code, int &offset, int &r_begin, int &r_length) {
	const int length = p_code.length();
	while (offset < length) {
		if (offset + 1 < length && p_code[offset] == '/' && p_code[offset + 1] == '/') {
			offset += 2;
			while (offset < length && p_code[offset] != '\n') {
				offset++;
			}
			continue;
		}
		if (offset + 1 < length && p_code[offset] == '/' && p_code[offset + 1] == '*') {
			offset += 2;
			while (offset + 1 < length && !(p_code[offset] == '*' && p_code[offset + 1] == '/')) {
				offset++;
			}
			offset += 2;
			continue;
		}
		if (p_code[offset] == '"') {
			offset++;
			while (offset < length && p_code[offset] != '"') {
				offset += p_code[offset] == '\\' ? 2 : 1;
			}
			offset++;
			continue;
		}
		if (p_code[offset] != '_' && !is_ascii_alphanumeric_char(p_code[offset])) {
			offset++;
			continue;
		}
		const int begin = offset++;
		while (offset < length && (p_code[offset] == '_' || is_ascii_alphanumeric_char(p_code[offset]))) {
			offset++;
		}
		r_begin = begin;
		r_length = offset - begin;
		return true;
	}
	return false;
}

static String shader_next_word(const String &p_code, int &r_offset) {
	int begin = 0;
	int length = 0;
	return shader_next_word_bounds(p_code, r_offset, begin, length) ? p_code.substr(begin, length) : String();
}

static bool shader_uses_word(const String &p_code, const String &p_word) {
	int offset = 0;
	int begin = 0;
	int length = 0;
	while (shader_next_word_bounds(p_code, offset, begin, length)) {
		if (length == p_word.length() && p_code.substr(begin, length) == p_word) {
			return true;
		}
	}
	return false;
}

static Vector<StringName> shader_global_uniform_names(const String &p_code) {
	Vector<StringName> names;
	int offset = 0;
	for (String word = shader_next_word(p_code, offset); !word.is_empty(); word = shader_next_word(p_code, offset)) {
		if (word != "global" || shader_next_word(p_code, offset) != "uniform") {
			continue;
		}
		String type = shader_next_word(p_code, offset);
		if (type == "lowp" || type == "mediump" || type == "highp") {
			type = shader_next_word(p_code, offset);
		}
		const String name = shader_next_word(p_code, offset);
		if (!type.is_empty() && !name.is_empty()) {
			names.push_back(name);
		}
	}
	return names;
}

static bool material_pass_contributes(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return true;
	}
	Ref<BaseMaterial3D> standard = p_material;
	if (standard.is_valid()) {
		const bool implicit_alpha = standard->get_transparency() == BaseMaterial3D::TRANSPARENCY_DISABLED &&
				standard->get_distance_fade() == BaseMaterial3D::DISTANCE_FADE_PIXEL_ALPHA;
		return standard->get_blend_mode() == BaseMaterial3D::BLEND_MODE_MIX &&
				standard->get_transparency() != BaseMaterial3D::TRANSPARENCY_ALPHA && !implicit_alpha &&
				!standard->get_feature(BaseMaterial3D::FEATURE_REFRACTION) && !standard->is_proximity_fade_enabled() &&
				!standard->get_flag(BaseMaterial3D::FLAG_USE_SHADOW_TO_OPACITY) &&
				standard->get_depth_draw_mode() != BaseMaterial3D::DEPTH_DRAW_DISABLED &&
				!standard->get_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST);
	}
	Ref<ShaderMaterial> shader = p_material;
	if (shader.is_null() || shader->get_shader().is_null()) {
		return true; // Material validation reports missing or unsupported shaders.
	}
	const String code = shader->get_shader()->get_preprocessed_code();
	for (const char *word : { "blend_add", "blend_sub", "blend_mul", "blend_premul_alpha", "depth_draw_never", "depth_test_disabled", "shadow_to_opacity", "hint_screen_texture", "hint_depth_texture", "hint_normal_roughness_texture" }) {
		if (shader_uses_word(code, word)) {
			return false;
		}
	}
	return !shader_uses_word(code, "ALPHA") || shader_uses_word(code, "ALPHA_SCISSOR_THRESHOLD") ||
			shader_uses_word(code, "ALPHA_HASH_SCALE") || shader_uses_word(code, "depth_prepass_alpha");
}

static bool material_chain_contributes(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return true;
	}
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		if (material_pass_contributes(material)) {
			return true;
		}
	}
	return false;
}

static bool shadow_material_uses_time(const Ref<Material> &p_material) {
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		if (!material_pass_contributes(material)) {
			continue;
		}
		Ref<ShaderMaterial> shader = material;
		if (shader.is_valid() && shader->get_shader().is_valid() && shader_uses_word(shader->get_shader()->get_preprocessed_code(), "TIME")) {
			return true;
		}
	}
	return false;
}

static bool material_requires_raster_geometry(const Ref<Material> &p_material) {
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		const Ref<BaseMaterial3D> standard = material;
		if (standard.is_valid() && standard->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED) {
			return true;
		}
	}
	return false;
}

static String material_pass_support_error(const Ref<Material> &p_material) {
	if (p_material.is_null() || Object::cast_to<BaseMaterial3D>(p_material.ptr())) {
		return String();
	}
	return RTR("LRT local voxels support static BaseMaterial3D inputs only; ShaderMaterial does not contribute.");
}

String LRTVolume3D::_material_support_error(const Ref<Material> &p_material) {
	int pass = 0;
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass(), pass++) {
		const String error = material_pass_support_error(material);
		if (!error.is_empty()) {
			return pass == 0 ? error : vformat("Next Pass %d: %s", pass, error);
		}
	}
	return String();
}

String LRTVolume3D::get_instance_sdf_message(Node3D *p_instance) const {
	ERR_FAIL_NULL_V(p_instance, String());
	if (Object::cast_to<GPUParticles3D>(p_instance) || Object::cast_to<CPUParticles3D>(p_instance)) {
		return RTR("Particles do not contribute LRT geometry or emission.");
	}
	if (p_instance->get_world_3d() != get_world_3d() || !p_instance->is_visible_in_tree()) {
		return RTR("The instance is hidden or belongs to another World3D.");
	}
	for (const Receiver &receiver : receivers) {
		if (receiver.instance_id != p_instance->get_instance_id()) {
			continue;
		}
		if (!receiver.gi_enabled) {
			return RTR("GI Mode is Disabled: receives LRT without contributing geometry, color or emission.");
		}
		if (!receiver.material_error.is_empty()) {
			return receiver.material_error.strip_edges();
		}
		if (!receiver.contributes) {
			return RTR("Receive-only passes do not occlude or contribute color or emission.");
		}
		return error_message.is_empty() ?
				RTR("Reuses mesh-local voxels. Transforms update local transfer only; shader deformation, skeletons and blend shapes are not captured.") : error_message;
	}
	return RTR("No contribution inside this Volume. Check visibility and mesh geometry.");
}

uint64_t LRTVolume3D::_resource_dependency_revision(const Ref<Resource> &p_resource, bool p_shadow) {
	auto &dependencies = p_shadow ? shadow_resource_dependencies : resource_dependencies;
	if (p_resource.is_null()) {
		return 0;
	}
	const ObjectID id = p_resource->get_instance_id();
	auto found = dependencies.find(id);
	if (found == dependencies.end()) {
		ResourceDependency dependency;
		dependency.resource = p_resource;
		found = dependencies.emplace(id, dependency).first;
		const Callable callback = p_shadow ? callable_mp(this, &LRTVolume3D::_shadow_resource_dependency_changed) : callable_mp(this, &LRTVolume3D::_resource_dependency_changed);
		p_resource->connect("changed", callback.bind(id));
	}
	found->second.used = true;
	return found->second.revision;
}

void LRTVolume3D::_resource_dependency_changed(ObjectID p_id) {
	const auto found = resource_dependencies.find(p_id);
	if (found != resource_dependencies.end()) {
		found->second.revision++;
		found->second.sdf_resolution = 0;
	}
}

void LRTVolume3D::_shadow_resource_dependency_changed(ObjectID p_id) {
	const auto found = shadow_resource_dependencies.find(p_id);
	if (found != shadow_resource_dependencies.end()) {
		found->second.revision++;
	}
}

void LRTVolume3D::_release_resource_dependencies(bool p_all, bool p_shadow) {
	auto &dependencies = p_shadow ? shadow_resource_dependencies : resource_dependencies;
	for (auto dependency = dependencies.begin(); dependency != dependencies.end();) {
		if (!p_all && dependency->second.used) {
			++dependency;
			continue;
		}
		const Callable callback = p_shadow ? callable_mp(this, &LRTVolume3D::_shadow_resource_dependency_changed) : callable_mp(this, &LRTVolume3D::_resource_dependency_changed);
		dependency->second.resource->disconnect("changed", callback.bind(dependency->first));
		dependency = dependencies.erase(dependency);
	}
}

uint64_t LRTVolume3D::_material_resource_signature(const Ref<Material> &p_material, bool p_shadow) {
	uint64_t state = 0;
	if (p_material.is_null()) {
		return state;
	}
	auto hash_value = [this, &state, p_shadow](const Variant &p_value) {
		state = mix_signature(state, p_value.hash());
		if (p_value.get_type() == Variant::OBJECT) {
			Ref<Resource> resource = p_value;
			if (resource.is_valid()) {
				state = mix_signature(state, resource->get_rid().get_id());
				state = mix_signature(state, _resource_dependency_revision(resource, p_shadow));
			}
		}
	};
	state = mix_signature(state, p_material->get_rid().get_id());
	state = mix_signature(state, p_material->get_render_priority());
	if (p_shadow && shadow_material_uses_time(p_material)) {
		state = mix_signature(state, Engine::get_singleton()->get_frames_drawn());
	}
	state = mix_signature(state, _material_resource_signature(p_material->get_next_pass(), p_shadow));
	Ref<BaseMaterial3D> base_material = p_material;
	if (base_material.is_valid()) {
		state = mix_signature(state, base_material->get_parameter_change_version());
		for (int texture_index = 0; texture_index < BaseMaterial3D::TEXTURE_MAX; texture_index++) {
			hash_value(base_material->get_texture(BaseMaterial3D::TextureParam(texture_index)));
		}
		return state;
	}
	state = mix_signature(state, _resource_dependency_revision(p_material, p_shadow));
	List<PropertyInfo> properties;
	p_material->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		bool valid = false;
		const Variant value = p_material->get(property.name, &valid);
		if (!valid) {
			continue;
		}
		state = mix_signature(state, property.name.hash());
		hash_value(value);
	}
	Ref<ShaderMaterial> shader_material = p_material;
	if (shader_material.is_valid() && shader_material->get_shader().is_valid()) {
		Ref<Shader> shader = shader_material->get_shader();
		state = mix_signature(state, _resource_dependency_revision(shader, p_shadow));
		if (p_shadow || material_pass_contributes(p_material)) {
			auto &dependencies = p_shadow ? shadow_resource_dependencies : resource_dependencies;
			ResourceDependency &dependency = dependencies.at(shader->get_instance_id());
			if (dependency.global_uniform_revision != dependency.revision) {
				dependency.global_uniform_names = shader_global_uniform_names(shader->get_preprocessed_code());
				dependency.global_uniform_revision = dependency.revision;
			}
			for (const StringName &name : dependency.global_uniform_names) {
				const auto parameter = RS::get_singleton()->global_shader_parameter_get_state(name);
				state = mix_signature(state, name.hash());
				state = mix_signature(state, parameter.revision);
				hash_value(parameter.value);
			}
		}
		List<PropertyInfo> uniforms;
		shader->get_shader_uniform_list(&uniforms);
		for (const PropertyInfo &property : uniforms) {
			hash_value(shader_material->get_shader_parameter(property.name));
		}
	}
	return state;
}

uint64_t LRTVolume3D::_material_content_signature(const Ref<Material> &p_material, bool p_persistent, uint8_t p_component) const {
	if (p_material.is_null()) {
		return 0;
	}
	if (!material_pass_contributes(p_material)) {
		return _material_content_signature(p_material->get_next_pass(), p_persistent, p_component);
	}
	uint64_t state = mix_signature(0, p_material->get_class_name().hash());
	auto hash_value = [this, &state, p_persistent, p_component](const Variant &p_value) {
		if (p_value.get_type() != Variant::OBJECT) {
			state = mix_signature(state, p_value.hash());
			return;
		}
		Ref<Resource> resource = p_value;
		if (resource.is_null()) {
			state = mix_signature(state, 0);
			return;
		}
		// Persistent identity follows content across reloads, moves and PCK remaps.
		const Ref<Material> nested_material = resource;
		if (nested_material.is_valid()) {
			state = mix_signature(state, _material_content_signature(nested_material, p_persistent, p_component));
			return;
		}
		state = mix_signature(state, resource->get_class_name().hash());
		const Ref<Texture2D> texture_2d = resource;
		const Ref<TextureLayered> texture_layered = resource;
		const Ref<Texture3D> texture_3d = resource;
		if (texture_2d.is_valid()) {
			state = mix_signature(state, lrt::texture_content_signature(texture_2d->get_rid(), RSE::TEXTURE_TYPE_2D));
		} else if (texture_layered.is_valid()) {
			state = mix_signature(state, lrt::texture_content_signature(texture_layered->get_rid(), RSE::TEXTURE_TYPE_LAYERED));
		} else if (texture_3d.is_valid()) {
			state = mix_signature(state, lrt::texture_content_signature(texture_3d->get_rid(), RSE::TEXTURE_TYPE_3D));
		}
	};
	const Ref<BaseMaterial3D> standard = p_material;
	List<PropertyInfo> properties;
	p_material->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
		}
		if (standard.is_valid()) {
			const String name = property.name;
			if (p_component == lrt::LOCAL_REFLECTANCE && (name.begins_with("emission") || name == "cull_mode")) {
				continue;
			}
			if (p_component == lrt::LOCAL_EMISSION && !material_requires_raster_geometry(p_material) && name.begins_with("albedo_")) {
				continue;
			}
		}
		bool valid = false;
		const Variant value = p_material->get(property.name, &valid);
		if (valid) {
			state = mix_signature(state, property.name.hash());
			hash_value(value);
		}
	}
	const Ref<ShaderMaterial> material = p_material;
	if (material.is_valid() && material->get_shader().is_valid() && material_pass_contributes(p_material)) {
		for (const StringName &name : shader_global_uniform_names(material->get_shader()->get_preprocessed_code())) {
			const auto parameter = RS::get_singleton()->global_shader_parameter_get_state(name);
			state = mix_signature(state, name.hash());
			state = mix_signature(state, parameter.type);
			if (!p_persistent) {
				state = mix_signature(state, parameter.revision);
			} else if (parameter.type >= RSE::GLOBAL_VAR_TYPE_SAMPLER2D && parameter.type < RSE::GLOBAL_VAR_TYPE_MAX) {
				RSE::TextureType type = RSE::TEXTURE_TYPE_2D;
				if (parameter.type == RSE::GLOBAL_VAR_TYPE_SAMPLER3D) {
					type = RSE::TEXTURE_TYPE_3D;
				} else if (parameter.type == RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY || parameter.type == RSE::GLOBAL_VAR_TYPE_SAMPLERCUBE) {
					type = RSE::TEXTURE_TYPE_LAYERED;
				}
				state = mix_signature(state, lrt::texture_content_signature(parameter.texture, type));
			} else {
				hash_value(parameter.value);
			}
		}
	}
	return state;
}

uint64_t LRTVolume3D::_material_signature(const Receiver &p_receiver,
		std::map<ObjectID, uint64_t> &r_material_signatures) {
	if (p_receiver.contributing_surfaces.is_empty()) {
		return 0;
	}
	uint64_t state = 0;
	auto append = [&](const Ref<Material> &p_material) {
		if (p_material.is_null()) {
			state = mix_signature(state, 0);
			return;
		}
		const ObjectID id = p_material->get_instance_id();
		auto found = r_material_signatures.find(id);
		if (found == r_material_signatures.end()) {
			found = r_material_signatures.emplace(id, _material_resource_signature(p_material)).first;
		}
		state = mix_signature(state, found->second);
	};
	for (int surface : p_receiver.contributing_surfaces) {
		state = mix_signature(state, surface);
		append(p_receiver.materials[surface]);
	}
	append(p_receiver.authored_overlay);
	return state;
}

uint64_t LRTVolume3D::_material_revision_signature(const Receiver &p_receiver) {
	uint64_t state = mix_signature(p_receiver.mesh->get_rid().get_id(), _resource_dependency_revision(p_receiver.mesh));
	auto append = [&](const Ref<Material> &p_material) {
		for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
			state = mix_signature(state, uint64_t(material->get_instance_id()));
			state = mix_signature(state, _resource_dependency_revision(material));
			const Ref<BaseMaterial3D> standard = material;
			if (standard.is_null()) {
				continue;
			}
			state = mix_signature(state, standard->get_parameter_change_version());
			for (int index = 0; index < BaseMaterial3D::TEXTURE_MAX; index++) {
				state = mix_signature(state, _resource_dependency_revision(standard->get_texture(BaseMaterial3D::TextureParam(index))));
			}
		}
	};
	for (const Ref<Material> &material : p_receiver.materials) {
		append(material);
	}
	append(p_receiver.authored_overlay);
	return state;
}

bool LRTVolume3D::_material_inputs_unchanged() {
	for (const Receiver &receiver : receivers) {
		if (receiver.contributes && _material_revision_signature(receiver) != receiver.material_revision_signature) {
			return false;
		}
	}
	return true;
}

int LRTVolume3D::_effective_sdf_resolution(const Ref<Mesh> &p_mesh, const Object *p_instance) {
	_resource_dependency_revision(p_mesh);
	ResourceDependency &dependency = resource_dependencies.at(p_mesh->get_instance_id());
	const double cell_size = lrt::geometry_cell_size();
	if (dependency.sdf_resolution == 0 || dependency.geometry_cell_size != cell_size) {
		std::vector<lrt::MeshTriangle> triangles;
		lrt::mesh_triangles(p_mesh, triangles);
		dependency.geometry_bounds = AABB();
		if (!triangles.empty()) {
			const lrt::Vec3 &first = triangles.front().position[0];
			dependency.geometry_bounds = AABB(Vector3(first.x, first.y, first.z), Vector3());
			for (const lrt::MeshTriangle &triangle : triangles) {
				for (const lrt::Vec3 &position : triangle.position) {
					dependency.geometry_bounds.expand_to(Vector3(position.x, position.y, position.z));
				}
			}
		}
		dependency.sdf_resolution = lrt::estimate_mesh_geometry_resolution(triangles);
		dependency.geometry_cell_size = cell_size;
	}
	const double effective_cell_size = lrt::geometry_cell_size(p_instance);
	if (effective_cell_size != cell_size) {
		return int(std::max(1.0, std::min(double(INT32_MAX), Math::ceil(dependency.geometry_bounds.get_longest_axis_size() / effective_cell_size))));
	}
	return dependency.sdf_resolution;
}

static uint64_t mix_signature(uint64_t p_hash, uint64_t p_value) {
	// hash_murmur3_one_64 returns a 32-bit digest, which is plenty for change detection.
	return hash_murmur3_one_64(p_value, uint32_t(p_hash) ^ 0x9e3779b9u);
}

static uint64_t quantized_signature_value(double p_value, double p_scale) {
	return uint64_t(int64_t(Math::round(p_value * p_scale)));
}

static uint64_t mesh_content_signature(const Ref<Mesh> &p_mesh) {
	uint64_t state = mix_signature(0, uint64_t(p_mesh->get_surface_count()));
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		state = mix_signature(state, uint64_t(p_mesh->surface_get_primitive_type(surface)));
		const Array arrays = p_mesh->surface_get_arrays(surface);
		for (int array_index = 0; array_index < arrays.size(); array_index++) {
			state = mix_signature(state, arrays[array_index].hash());
		}
	}
	return state;
}

// Geometry and appearance requests remain separate. The local builder compares captured
// geometry, reflectance and emission identities per Trunk before selecting its update work.
uint64_t LRTVolume3D::_geometry_signature() const {
	uint64_t state = 0;
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		const Transform3D &transform = receiver.transform;
		state = mix_signature(state, receiver.render_instance.get_id());
		state = mix_signature(state, uint64_t(receiver.sdf_resolution));
		state = mix_signature(state, uint64_t(receiver.layer_mask));
		state = mix_signature(state, quantized_signature_value(transform.origin.x, 10000.0));
		state = mix_signature(state, quantized_signature_value(transform.origin.y, 10000.0));
		state = mix_signature(state, quantized_signature_value(transform.origin.z, 10000.0));
		for (int column = 0; column < 3; column++) {
			for (int row = 0; row < 3; row++) {
				state = mix_signature(state, quantized_signature_value(transform.basis[row][column], 1000000.0));
			}
		}
		Ref<Mesh> mesh = receiver.mesh;
		state = mix_signature(state, mesh.is_valid() ? mesh->get_rid().get_id() : 0);
		state = mix_signature(state, receiver.mesh_content_signature);
		state = mix_signature(state, receiver.instances_signature);
		state = mix_signature(state, mesh.is_valid() ? uint64_t(mesh->get_surface_count()) : 0);
	}
	return state;
}

uint64_t LRTVolume3D::_build_cache_fingerprint() {
	uint64_t state = mix_signature(0, 31); // Unified GPU inputs with exact triangle coverage filtering.
	state = mix_signature(state, quantized_signature_value(spacing, 100000.0));
	for (int axis = 0; axis < 3; axis++) {
		state = mix_signature(state, quantized_signature_value(volume_size[axis], 10000.0));
	}
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		const Transform3D &transform = receiver.transform;
		for (int axis = 0; axis < 3; axis++) {
			state = mix_signature(state, quantized_signature_value(transform.origin[axis], 10000.0));
			for (int column = 0; column < 3; column++) {
				state = mix_signature(state, quantized_signature_value(transform.basis[axis][column], 1000000.0));
			}
		}
		state = mix_signature(state, mesh_content_signature(receiver.mesh));
		state = mix_signature(state, receiver.sdf_resolution);
		state = mix_signature(state, receiver.layer_mask);
		for (int surface : receiver.contributing_surfaces) {
			state = mix_signature(state, surface);
			state = mix_signature(state, _material_content_signature(receiver.materials[surface]));
		}
		state = mix_signature(state, _material_content_signature(receiver.authored_overlay));
		if (receiver.multimesh.is_valid()) {
			state = mix_signature(state, Variant(RS::get_singleton()->multimesh_get_buffer(receiver.multimesh)).hash());
			state = mix_signature(state, RS::get_singleton()->multimesh_get_visible_instances(receiver.multimesh));
		}
	}
	return _material_inputs_unchanged() ? state : 0;
}

uint64_t LRTVolume3D::_material_state_signature() const {
	uint64_t state = 0;
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		state = mix_signature(state, receiver.render_instance.get_id());
		state = mix_signature(state, receiver.material_signature);
		state = mix_signature(state, quantized_signature_value(receiver.albedo.x, 100000.0));
		state = mix_signature(state, quantized_signature_value(receiver.albedo.y, 100000.0));
		state = mix_signature(state, quantized_signature_value(receiver.albedo.z, 100000.0));
	}
	return state;
}

// The solver consumes the engine's own Light3D nodes: position, direction, colour, energy,
// indirect energy, range, attenuation, cone and the shadow flag. The scene tree is the only
// source, so a light outside the camera frustum cannot lose its input. Directions follow the
// prototype's convention (the direction the light travels along, the engine's forward axis).
Array LRTVolume3D::_mapped_lights() const {
	Array result;
	const bool physical = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	for (const LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr) {
			continue;
		}
		const Transform3D transform = relative_node_transform(this, light);
		const Color color = light->get_color().srgb_to_linear();
		double range = 1.0;
		double attenuation = 1.0;
		double spot_angle = 45.0;
		double spot_attenuation = 1.0;
		Vector2 area_size;
		bool area_normalize = false;
		int type = 0;
		if (Object::cast_to<DirectionalLight3D>(light) != nullptr) {
			type = 1;
		} else if (AreaLight3D *area = Object::cast_to<AreaLight3D>(light)) {
			type = 3;
			range = area->get_param(Light3D::PARAM_RANGE);
			attenuation = area->get_param(Light3D::PARAM_ATTENUATION);
			area_size = area->get_area_size();
			area_normalize = area->is_area_normalizing_energy();
		} else if (SpotLight3D *spot = Object::cast_to<SpotLight3D>(light)) {
			type = 2;
			range = spot->get_param(Light3D::PARAM_RANGE);
			attenuation = spot->get_param(Light3D::PARAM_ATTENUATION);
			spot_angle = spot->get_param(Light3D::PARAM_SPOT_ANGLE);
			spot_attenuation = spot->get_param(Light3D::PARAM_SPOT_ATTENUATION);
		} else if (OmniLight3D *omni = Object::cast_to<OmniLight3D>(light)) {
			type = 0;
			range = omni->get_param(Light3D::PARAM_RANGE);
			attenuation = omni->get_param(Light3D::PARAM_ATTENUATION);
		}
		const double energy = light->get_param(Light3D::PARAM_ENERGY) * light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
		double intensity = LIGHT_INTENSITY_SCALE * energy;
		if (physical) {
			const double physical_intensity = energy * light->get_param(Light3D::PARAM_INTENSITY);
			if (type == 0) {
				intensity = physical_intensity / (4.0 * LIGHT_INTENSITY_SCALE);
			} else if (type == 2) {
				intensity = physical_intensity / LIGHT_INTENSITY_SCALE;
			} else if (type == 3) {
				intensity = physical_intensity / (2.0 * LIGHT_INTENSITY_SCALE);
			} else {
				intensity = physical_intensity;
			}
		}
		Dictionary mapped;
		mapped["instance_id"] = int64_t(entry.light_id);
		mapped["type"] = type;
		mapped["enabled"] = entry.visible;
		mapped["casts_shadow"] = light->has_shadow();
		mapped["position"] = transform.origin;
		mapped["direction"] = -transform.basis.get_column(2).normalized();
		mapped["color"] = Vector3(color.r, color.g, color.b);
		mapped["intensity"] = intensity;
		mapped["range"] = range;
		mapped["attenuation"] = attenuation;
		mapped["spot_angle_deg"] = spot_angle;
		mapped["spot_attenuation"] = spot_attenuation;
		mapped["area_size"] = area_size;
		mapped["area_normalize"] = area_normalize;
		mapped["cull_mask"] = int64_t(light->get_cull_mask());
		mapped["shadow_caster_mask"] = int64_t(light->get_shadow_caster_mask());
		// A projector makes the resolve sample the decal atlas, so gaining or losing the texture has
		// to re-resolve the light. The atlas rect itself is only read on the render thread.
		if (type == 0 || type == 2) {
			mapped["has_projector"] = !light->get_projector().is_null();
		}
		result.push_back(mapped);
	}
	return result;
}

Array LRTVolume3D::_cached_mapped_lights() {
	if (!mapped_lights_cache_valid) {
		mapped_lights_cache = _mapped_lights();
		mapped_lights_cache_valid = true;
	}
	return mapped_lights_cache;
}

bool LRTVolume3D::_light_inputs_equal(const Array &p_left, const Array &p_right) {
	if (p_left.size() != p_right.size()) {
		return false;
	}
	for (int i = 0; i < p_left.size(); i++) {
		const Dictionary left = p_left[i];
		const Dictionary right = p_right[i];
		if (left.get("type", -1) != right.get("type", -1) || left.get("enabled", false) != right.get("enabled", false) ||
				left.get("casts_shadow", false) != right.get("casts_shadow", false) ||
				left.get("cull_mask", int64_t(0)) != right.get("cull_mask", int64_t(0)) ||
				left.get("shadow_caster_mask", int64_t(0)) != right.get("shadow_caster_mask", int64_t(0)) ||
				!Vector3(left.get("position", Vector3())).is_equal_approx(Vector3(right.get("position", Vector3()))) ||
				!Vector3(left.get("direction", Vector3())).is_equal_approx(Vector3(right.get("direction", Vector3()))) ||
				Vector3(left.get("color", Vector3())) != Vector3(right.get("color", Vector3()))) {
			return false;
		}
		if (Vector2(left.get("area_size", Vector2())) != Vector2(right.get("area_size", Vector2())) ||
				left.get("area_normalize", false) != right.get("area_normalize", false)) {
			return false;
		}
		for (const char *key : { "intensity", "range", "attenuation", "spot_angle_deg", "spot_attenuation" }) {
			if (!Math::is_equal_approx(double(left.get(key, 0.0)), double(right.get(key, 0.0)))) {
				return false;
			}
		}
	}
	return true;
}

bool LRTVolume3D::_light_capture_inputs_equal(const Array &p_left, const Array &p_right) {
	if (p_left.size() != p_right.size()) {
		return false;
	}
	for (int i = 0; i < p_left.size(); i++) {
		const Dictionary left = p_left[i];
		const Dictionary right = p_right[i];
		if (!_light_capture_input_equal(left, right)) {
			return false;
		}
	}
	return true;
}

bool LRTVolume3D::_light_capture_input_equal(const Dictionary &p_left, const Dictionary &p_right) {
	if (p_left.get("instance_id", int64_t(0)) != p_right.get("instance_id", int64_t(0)) ||
			p_left.get("type", -1) != p_right.get("type", -1) || p_left.get("enabled", false) != p_right.get("enabled", false) ||
			p_left.get("casts_shadow", false) != p_right.get("casts_shadow", false) ||
			p_left.get("cull_mask", int64_t(0)) != p_right.get("cull_mask", int64_t(0)) ||
			p_left.get("shadow_caster_mask", int64_t(0)) != p_right.get("shadow_caster_mask", int64_t(0)) ||
			!Vector3(p_left.get("position", Vector3())).is_equal_approx(Vector3(p_right.get("position", Vector3()))) ||
			!Vector3(p_left.get("direction", Vector3())).is_equal_approx(Vector3(p_right.get("direction", Vector3()))) ||
			Vector2(p_left.get("area_size", Vector2())) != Vector2(p_right.get("area_size", Vector2())) ||
			p_left.get("area_normalize", false) != p_right.get("area_normalize", false)) {
		return false;
	}
	// A projector changes the resolved field, and the decal atlas rect is only read on the render
	// thread, so the capture input tracks whether the light owns one instead of the rect itself.
	if (p_left.get("has_projector", false) != p_right.get("has_projector", false) ||
			p_left.get("projector_signature", int64_t(0)) != p_right.get("projector_signature", int64_t(0)) ||
			p_left.get("area_texture_signature", int64_t(0)) != p_right.get("area_texture_signature", int64_t(0))) {
		return false;
	}
	for (const char *key : { "range", "attenuation", "spot_angle_deg", "spot_attenuation" }) {
		if (!Math::is_equal_approx(double(p_left.get(key, 0.0)), double(p_right.get(key, 0.0)))) {
			return false;
		}
	}
	return true;
}

Vector3 LRTVolume3D::_light_photometric_scale(Light3D *p_light) const {
	ERR_FAIL_NULL_V(p_light, Vector3());
	Color color = p_light->get_color().srgb_to_linear();
	double scale = p_light->get_param(Light3D::PARAM_ENERGY) * p_light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
	if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		const Color temperature = p_light->get_correlated_color().srgb_to_linear();
		color *= temperature;
		scale *= p_light->get_param(Light3D::PARAM_INTENSITY);
		if (Object::cast_to<OmniLight3D>(p_light)) {
			scale /= 4.0 * Math::PI;
		} else if (Object::cast_to<SpotLight3D>(p_light)) {
			scale /= Math::PI;
		} else if (Object::cast_to<AreaLight3D>(p_light)) {
			scale /= 2.0 * Math::PI;
		}
	} else {
		// Match the native unexposed light color. G supplies the receiver Lambert response.
		scale *= Math::PI;
	}
	if (p_light->is_negative()) {
		scale = -scale;
	}
	return Vector3(color.r, color.g, color.b) * scale;
}

uint64_t LRTVolume3D::_light_photometry_signature() const {
	uint64_t state = 0;
	for (const LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr) {
			continue;
		}
		state = mix_signature(state, uint64_t(entry.light_id));
		state = mix_signature(state, uint64_t(entry.visible));
		const Vector3 scale = _light_photometric_scale(light);
		state = mix_signature(state, quantized_signature_value(scale.x, 100000.0));
		state = mix_signature(state, quantized_signature_value(scale.y, 100000.0));
		state = mix_signature(state, quantized_signature_value(scale.z, 100000.0));
	}
	return state;
}

void LRTVolume3D::_apply_native_light_photometry(bool p_count_invalidation) {
	if (solver.is_null() || !solver->has_local_field() || !native_source_ready) {
		return;
	}
	int light_slot = 0;
	for (const LightEntry &entry : lights) {
		if (!entry.visible) {
			continue;
		}
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr) {
			continue;
		}
		solver->set_native_light_scale(light_slot, _light_photometric_scale(light));
		float influence_radius = -1.0f;
		if (Object::cast_to<DirectionalLight3D>(light) == nullptr) {
			influence_radius = float(light->get_param(Light3D::PARAM_RANGE) + spacing * 1.75);
			if (AreaLight3D *area = Object::cast_to<AreaLight3D>(light)) {
				influence_radius += area->get_area_size().length() * 0.5f;
			}
		}
		solver->set_native_light_influence(light_slot, relative_node_transform(this, light).origin,
				influence_radius, light->get_cull_mask());
		light_slot++;
	}
	_inject_sources(p_count_invalidation);
	native_source_ready = true;
}

std::map<ObjectID, uint32_t> LRTVolume3D::_shadow_geometry_candidates() const {
	std::map<ObjectID, uint32_t> candidates;
	const RID scenario = get_world_3d()->get_scenario();
	const Vector3 size = _effective_volume_size();
	const AABB bounds(-size * 0.5, size);
	for (const LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr || !entry.visible || !light->has_shadow()) {
			continue;
		}
		Vector<ObjectID> instances;
		if (Object::cast_to<DirectionalLight3D>(light) != nullptr) {
			instances = RS::get_singleton()->instances_cull_convex(directional_shadow_input_planes(
					get_global_transform(), bounds, light->get_global_transform(), light->get_param(Light3D::PARAM_SIZE)), scenario);
		} else {
			instances = RS::get_singleton()->instances_cull_aabb(light->get_global_transform().xform(light->get_aabb()), scenario);
		}
		for (ObjectID id : instances) {
			candidates[id] |= light->get_shadow_caster_mask();
		}
	}
	return candidates;
}

bool LRTVolume3D::_shadow_bounds_intersect(const AABB &p_bounds, uint32_t p_layers) const {
	for (const LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr || !entry.visible || !light->has_shadow() || !(p_layers & light->get_shadow_caster_mask())) {
			continue;
		}
		if (Object::cast_to<DirectionalLight3D>(light) == nullptr) {
			if (p_bounds.intersects(light->get_global_transform().xform(light->get_aabb()))) {
				return true;
			}
			continue;
		}
		const Vector3 size = _effective_volume_size();
		const Vector<Plane> planes = directional_shadow_input_planes(get_global_transform(), AABB(-size * 0.5, size),
				light->get_global_transform(), light->get_param(Light3D::PARAM_SIZE));
		bool intersects = true;
		for (const Plane &plane : planes) {
			const Vector3 nearest = p_bounds.get_center() - plane.normal.sign() * p_bounds.size * 0.5;
			if (plane.distance_to(nearest) >= 0) {
				intersects = false;
				break;
			}
		}
		if (intersects) {
			return true;
		}
	}
	return false;
}

uint64_t LRTVolume3D::_shadow_inputs_signature(uint64_t *r_resource_signature) {
	for (auto &dependency : shadow_resource_dependencies) {
		dependency.second.used = false;
	}
	uint64_t state = 0;
	uint64_t resource_state = 0;
	const auto mix_resource = [&state, &resource_state](uint64_t p_value) {
		state = mix_signature(state, p_value);
		resource_state = mix_signature(resource_state, p_value);
	};
	mix_resource(uint64_t(GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")));
	// Capture receivers, lights and casters all move together with a carrier. Hash only their
	// volume-relative state; the absolute Volume transform would turn rigid carrier motion into a
	// false shadow invalidation even though the captured unit-light field is unchanged.
	for (const LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr) {
			continue;
		}
		mix_resource(uint64_t(entry.light_id));
		mix_resource(uint64_t(entry.visible));
		mix_resource(uint64_t(light->get_class_name().hash()));
		mix_resource(uint64_t(light->has_shadow()));
		mix_resource(uint64_t(light->get_cull_mask()));
		mix_resource(uint64_t(light->get_shadow_caster_mask()));
		const Transform3D transform = relative_node_transform(this, light);
		state = mix_signature(state, quantized_signature_value(transform.origin.x, 100000.0));
		state = mix_signature(state, quantized_signature_value(transform.origin.y, 100000.0));
		state = mix_signature(state, quantized_signature_value(transform.origin.z, 100000.0));
		const Vector3 direction = -transform.basis.get_column(2).normalized();
		state = mix_signature(state, quantized_signature_value(direction.x, 100000.0));
		state = mix_signature(state, quantized_signature_value(direction.y, 100000.0));
		state = mix_signature(state, quantized_signature_value(direction.z, 100000.0));
		for (int param = 0; param < Light3D::PARAM_MAX; param++) {
			if (param == Light3D::PARAM_ENERGY || param == Light3D::PARAM_INDIRECT_ENERGY || param == Light3D::PARAM_INTENSITY ||
					param == Light3D::PARAM_VOLUMETRIC_FOG_ENERGY || param == Light3D::PARAM_SPECULAR) {
				continue;
			}
			mix_resource(quantized_signature_value(light->get_param(Light3D::Param(param)), 100000.0));
		}
		mix_resource(uint64_t(light->get_shadow_reverse_cull_face()));
		Ref<Texture2D> projector = light->get_projector();
		mix_resource(projector.is_valid() ? uint64_t(projector->get_instance_id()) : 0);
		mix_resource(projector.is_valid() ? uint64_t(_resource_dependency_revision(projector, true)) : 0);
		if (DirectionalLight3D *directional = Object::cast_to<DirectionalLight3D>(light)) {
			mix_resource(uint64_t(directional->get_shadow_mode()));
			mix_resource(uint64_t(directional->is_blend_splits_enabled()));
			mix_resource(uint64_t(directional->get_sky_mode()));
		} else if (OmniLight3D *omni = Object::cast_to<OmniLight3D>(light)) {
			mix_resource(uint64_t(omni->get_shadow_mode()));
		} else if (AreaLight3D *area = Object::cast_to<AreaLight3D>(light)) {
			const Vector2 area_size = area->get_area_size();
			mix_resource(quantized_signature_value(area_size.x, 100000.0));
			mix_resource(quantized_signature_value(area_size.y, 100000.0));
			mix_resource(uint64_t(area->is_area_normalizing_energy()));
			Ref<Texture2D> area_texture = area->get_area_texture();
			mix_resource(area_texture.is_valid() ? uint64_t(area_texture->get_instance_id()) : 0);
			mix_resource(area_texture.is_valid() ? uint64_t(_resource_dependency_revision(area_texture, true)) : 0);
		}
	}
	const uint64_t light_state = state;
	state = 0;
	const std::map<ObjectID, uint32_t> candidates = _shadow_geometry_candidates();
	for (const auto &candidate : candidates) {
		const ObjectID candidate_id = candidate.first;
		GeometryInstance3D *mesh_instance = geometry_from_id(candidate_id);
		if (mesh_instance == nullptr || mesh_instance->get_world_3d() != get_world_3d() ||
				!mesh_instance->is_visible_in_tree() ||
				!(mesh_instance->get_layer_mask() & candidate.second) ||
				mesh_instance->get_cast_shadows_setting() == GeometryInstance3D::SHADOW_CASTING_SETTING_OFF) {
			continue;
		}
		if (GPUParticles3D *particles = Object::cast_to<GPUParticles3D>(mesh_instance)) {
			mix_resource(particles->get_instance().get_id());
			mix_resource(RS::get_singleton()->instance_get_geometry_version(particles->get_instance()));
			mix_resource(Variant(relative_node_transform(this, particles)).hash());
			mix_resource(particles->get_layer_mask());
			mix_resource(particles->get_cast_shadows_setting());
			mix_resource(_material_resource_signature(particles->get_material_override(), true));
			mix_resource(_material_resource_signature(particles->get_material_overlay(), true));
			for (int pass = 0; pass < particles->get_draw_passes(); pass++) {
				const Ref<Mesh> mesh = particles->get_draw_pass_mesh(pass);
				if (mesh.is_null()) {
					continue;
				}
				mix_resource(_resource_dependency_revision(mesh, true));
				mix_resource(mesh->get_rid().get_id());
				for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
					mix_resource(_material_resource_signature(mesh->surface_get_material(surface), true));
				}
			}
			continue;
		}
		Ref<Mesh> mesh = geometry_mesh(mesh_instance);
		if (mesh.is_null()) {
			continue;
		}
		mix_resource(uint64_t(mesh_instance->get_instance_id()));
		const Transform3D transform = canonical_volume_transform(relative_node_transform(this, mesh_instance));
		mix_resource(quantized_signature_value(transform.origin.x, 10000.0));
		mix_resource(quantized_signature_value(transform.origin.y, 10000.0));
		mix_resource(quantized_signature_value(transform.origin.z, 10000.0));
		for (int column = 0; column < 3; column++) {
			for (int row = 0; row < 3; row++) {
				mix_resource(quantized_signature_value(transform.basis[row][column], 1000000.0));
			}
		}
		mix_resource(uint64_t(mesh_instance->get_layer_mask()));
		mix_resource(uint64_t(mesh_instance->get_cast_shadows_setting()));
		mix_resource(uint64_t(mesh->get_instance_id()));
		mix_resource(uint64_t(_resource_dependency_revision(mesh, true)));
		mix_resource(lrt::mesh_deformation_signature(Object::cast_to<MeshInstance3D>(mesh_instance)));
		if (is_instanced_geometry(mesh_instance)) {
			mix_resource(RS::get_singleton()->instance_get_geometry_version(mesh_instance->get_instance()));
		}
		for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
			Ref<Material> material = _surface_material(mesh_instance, surface);
			if (material.is_valid()) {
				mix_resource(uint64_t(material->get_instance_id()));
				mix_resource(_material_resource_signature(material, true));
			}
		}
	}
	for (ObjectID candidate_id : gridmap_candidates) {
		if (!candidates.count(candidate_id) || !(candidates.at(candidate_id) & 1)) {
			continue;
		}
		Node3D *node = Object::cast_to<Node3D>(ObjectDB::get_instance(candidate_id));
		if (node == nullptr || node->get_world_3d() != get_world_3d() || !node->is_visible_in_tree()) {
			continue;
		}
		mix_resource(uint64_t(candidate_id));
		mix_resource(Variant(node->get_global_transform()).hash());
		const Array meshes = node->call("get_render_meshes");
		for (int index = 0; index < meshes.size(); index += 3) {
			const RID render_instance = meshes[index];
			const Ref<Mesh> mesh = meshes[index + 1];
			const RID multimesh = meshes[index + 2];
			const AABB bounds = multimesh.is_valid() ? RS::get_singleton()->multimesh_get_aabb(multimesh) : mesh->get_aabb();
			if (!_shadow_bounds_intersect(node->get_global_transform().xform(bounds), 1)) {
				continue;
			}
			mix_resource(RS::get_singleton()->instance_get_geometry_version(render_instance));
			mix_resource(_resource_dependency_revision(mesh, true));
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				mix_resource(_material_resource_signature(mesh->surface_get_material(surface), true));
			}
		}
	}
	_release_resource_dependencies(false, true);
	if (r_resource_signature != nullptr) {
		*r_resource_signature = resource_state;
	}
	shadow_geometry_signature = state;
	return mix_signature(light_state, mix_signature(state, resource_state));
}

void LRTVolume3D::_queue_native_light_capture(bool p_local_field_changed, bool p_count_invalidation) {
	if (solver.is_null() || !solver->has_local_field()) {
		return;
	}
	const Array mapped_lights = _cached_mapped_lights();
	light_inputs = mapped_lights;
	const uint64_t next_photometry_signature = _light_photometry_signature();
	const bool photometry_changed = !has_light_photometry_signature ||
			next_photometry_signature != light_photometry_signature;
	light_photometry_signature = next_photometry_signature;
	has_light_photometry_signature = true;
	shadow_capture_signature = _shadow_inputs_signature(&shadow_capture_resource_signature);
	shadow_signature_refresh_frame = scheduler_frame;
	has_shadow_capture_signature = true;
	// This counter describes source invalidation requests, not GPU resolve completions.
	if (p_count_invalidation) {
		source_injections++;
	}
	uint64_t next_light_set_signature = 0;
	Array next_capture_inputs;
	for (int mapped_index = 0; mapped_index < mapped_lights.size(); mapped_index++) {
		Dictionary mapped_light = Dictionary(mapped_lights[mapped_index]).duplicate();
		if (!bool(mapped_light.get("enabled", false))) {
			continue;
		}
		Light3D *light = light_from_id(ObjectID(uint64_t(int64_t(mapped_light["instance_id"]))));
		const auto texture_signature = [this](const Ref<Texture2D> &p_texture) -> uint64_t {
			return p_texture.is_null() ? 0 : mix_signature(uint64_t(p_texture->get_instance_id()), _resource_dependency_revision(p_texture, true));
		};
		mapped_light["projector_signature"] = int64_t(texture_signature(light->get_projector()));
		AreaLight3D *area = Object::cast_to<AreaLight3D>(light);
		mapped_light["area_texture_signature"] = int64_t(area != nullptr ? texture_signature(area->get_area_texture()) : 0);
		next_light_set_signature = mix_signature(next_light_set_signature, uint64_t(int64_t(mapped_light.get("instance_id", int64_t(0)))));
		next_capture_inputs.push_back(mapped_light);
	}
	if (native_capture_pending) {
		if (next_light_set_signature == active_native_light_set_signature) {
			if (native_capture_queued_usec == 0) {
				native_capture_queued_usec = OS::get_singleton()->get_ticks_usec();
			}
			native_capture_queued = true;
			return;
		}
		// A visibility/add/remove edit changes which lights the result represents. The queued
		// resolves of the obsolete set are dropped with the batch, and the replacement set
		// rebuilds its own targets before it publishes.
		native_capture_pending = false;
		native_capture_queued = false;
	}
	const bool light_set_changed = next_light_set_signature != native_light_field_set_signature;
	const bool reuse_unit_fields = !light_set_changed &&
			native_light_field_inputs.size() == next_capture_inputs.size() &&
			!native_light_field_inputs.is_empty();
	const bool refresh_shadow_inputs = shadow_capture_signature != active_shadow_capture_signature;
	const int next_light_count = next_capture_inputs.size();
	std::vector<bool> dirty_light_slots(size_t(next_light_count), true);
	if (reuse_unit_fields) {
		for (int slot = 0; slot < next_light_count; slot++) {
			const Dictionary next_input = next_capture_inputs[slot];
			dirty_light_slots[size_t(slot)] = !_light_capture_input_equal(next_input, native_light_field_inputs[slot]);
			if (refresh_shadow_inputs && bool(next_input.get("casts_shadow", false))) {
				dirty_light_slots[size_t(slot)] = true;
			}
		}
	}
	bool has_dirty_light = false;
	for (bool dirty : dirty_light_slots) {
		has_dirty_light = has_dirty_light || dirty;
	}
	if (reuse_unit_fields && !has_dirty_light) {
		native_capture_queued = false;
		if (photometry_changed || p_local_field_changed) {
			// Recompose changed local records or photometry without resolving unchanged light inputs.
			_apply_native_light_photometry(true);
		}
		return;
	}
	native_light_diagnostics.clear();
	if (light_set_changed) {
		const bool initialize_source = native_source_ready && native_light_field_inputs.is_empty() && native_capture_updates == 0;
		// Retain the total Source until parameters for the replacement light set are committed.
		native_source_ready = initialize_source;
		if (!solver->reset_native_lights(next_light_count)) {
			error_message = "LRT native light buffer initialization failed.";
			return;
		}
		native_light_field_set_signature = next_light_set_signature;
		native_light_field_inputs.clear();
	}
	active_native_light_capture_inputs = next_capture_inputs;
	_update_display_parameters();
	const Transform3D capture_volume_to_world = get_global_transform();
	native_capture_count = 0;
	native_capture_shadowed_count = 0;
	int light_slot = 0;
	for (const LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr || !entry.visible) {
			continue;
		}
		if (!dirty_light_slots[size_t(light_slot)]) {
			light_slot++;
			continue;
		}
		NativeLightSnapshot snapshot;
		snapshot.input_usec = OS::get_singleton()->get_ticks_usec();
		snapshot.light_slot = light_slot;
		snapshot.source_id = entry.light_id;
		snapshot.source_name = light->get_name();
		snapshot.source_type = light->get_class();
		snapshot.shadow_enabled = light->has_shadow();
		native_light_snapshots.push_back(snapshot);
		const Transform3D source_transform = light->get_global_transform().orthonormalized();
		const Transform3D source_to_volume = capture_volume_to_world.affine_inverse() * source_transform;
		AreaLight3D *area_light = Object::cast_to<AreaLight3D>(light);
		LRTVolume::NativeLightResolve resolve;
		resolve.scene_light_instance = light->get_instance();
		Dictionary source_input = Dictionary(next_capture_inputs[light_slot]).duplicate();
		source_input.erase("color");
		source_input.erase("intensity");
		resolve.source_signature = Variant(source_input).hash();
		if (snapshot.shadow_enabled) {
			resolve.source_signature = mix_signature(resolve.source_signature, shadow_geometry_signature);
			resolve.source_signature = mix_signature(resolve.source_signature, light->get_shadow_reverse_cull_face());
		}
		resolve.volume_to_source = source_to_volume.affine_inverse();
		resolve.light_position = source_to_volume.origin;
		resolve.light_direction = -source_to_volume.basis.get_column(2).normalized();
		resolve.source_range = light->get_param(Light3D::PARAM_RANGE);
		resolve.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
		resolve.shadow_bias = light->get_param(Light3D::PARAM_SHADOW_BIAS);
		resolve.shadow_enabled = snapshot.shadow_enabled;
		resolve.volume_to_world = capture_volume_to_world;
		resolve.light_slot = snapshot.light_slot;
		resolve.cull_mask = light->get_cull_mask();
		resolve.directional = Object::cast_to<DirectionalLight3D>(light) != nullptr;
		resolve.projector_requested = !light->get_projector().is_null();
		if (resolve.directional) {
			resolve.direct_kind = 2;
		} else if (area_light != nullptr) {
			resolve.direct_kind = 5;
			resolve.area = true;
			resolve.area_texture_requested = area_light->get_area_texture().is_valid();
			resolve.area_half_size = area_light->get_area_size() * 0.5f;
			resolve.area_normalize_energy = area_light->is_area_normalizing_energy();
		} else if (Object::cast_to<OmniLight3D>(light) != nullptr) {
			resolve.direct_kind = 3;
		} else {
			resolve.direct_kind = 4;
			resolve.spot_cos_angle = Math::cos(Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE)));
			resolve.spot_cone_attenuation = 1.0 / MAX(light->get_param(Light3D::PARAM_SPOT_ATTENUATION), 1e-6);
		}
		solver->queue_direct_native_light_resolve(resolve);
		native_capture_count++;
		if (snapshot.shadow_enabled) {
			native_capture_shadowed_count++;
		}
		native_capture_gpu_resolves++;
		light_slot++;
	}
	// Photometric coefficients are independent from the resolve targets.
	_apply_native_light_photometry(false);
	active_shadow_capture_signature = shadow_capture_signature;
	active_shadow_capture_resource_signature = shadow_capture_resource_signature;
	active_native_light_set_signature = next_light_set_signature;
	native_capture_pending = true;
	const uint64_t capture_started_usec = OS::get_singleton()->get_ticks_usec();
	if (native_capture_queued_usec == 0) {
		native_capture_queued_usec = capture_started_usec;
	}
	native_capture_active_started_usec = capture_started_usec;
	native_capture_queued = false;
	if (!solver->is_native_light_resolve_pending()) {
		// No resolve was queued for this batch (for example every light left the Volume), so the
		// empty unit fields publish without waiting for a GPU dependency.
		_finish_native_light_capture();
	}
}

void LRTVolume3D::_finish_native_light_capture() {
	if (native_light_field_inputs.size() != active_native_light_capture_inputs.size()) {
		native_light_field_inputs = active_native_light_capture_inputs.duplicate();
	} else {
		for (const NativeLightSnapshot &snapshot : native_light_snapshots) {
			if (snapshot.light_slot >= 0 && snapshot.light_slot < active_native_light_capture_inputs.size()) {
				native_light_field_inputs[snapshot.light_slot] = active_native_light_capture_inputs[snapshot.light_slot];
			}
		}
	}
	for (const NativeLightSnapshot &snapshot : native_light_snapshots) {
		// A direct resolve publishes a complete unit field through a same-frame GPU dependency, so
		// the newest snapshot becomes the source immediately instead of fading in.
		solver->commit_native_light_input(snapshot.light_slot, uint64_t(snapshot.source_id), snapshot.input_usec);
		Dictionary light_status;
		light_status["instance_id"] = int64_t(snapshot.source_id);
		light_status["name"] = snapshot.source_name;
		light_status["type"] = snapshot.source_type;
		light_status["shadow_enabled"] = snapshot.shadow_enabled;
		light_status["gpu_resolved"] = true;
		native_light_diagnostics.push_back(light_status);
	}
	native_light_snapshots.clear();
	active_native_light_capture_inputs.clear();
	native_capture_pending = false;
	native_capture_updates++;
	native_capture_last_latency_ms = native_capture_active_started_usec == 0 ? 0.0 :
			double(OS::get_singleton()->get_ticks_usec() - native_capture_active_started_usec) / 1000.0;
	native_source_ready = true;
	// Publish photometry and sources only after every replacement unit field is committed.
	// An empty light set must also publish, so removing the final light updates the source.
	_apply_native_light_photometry(false);
	if (native_capture_queued || active_shadow_capture_signature != shadow_capture_signature) {
		if (native_capture_queued_usec == 0) {
			native_capture_queued_usec = OS::get_singleton()->get_ticks_usec();
		}
		native_capture_queued = true;
	}
}

bool LRTVolume3D::_poll_native_light_capture() {
	if (!native_capture_pending || solver.is_null()) {
		return false;
	}
	// The resolves of this batch are still in flight; publishing now would commit a target buffer
	// that is only partially written.
	if (solver->is_native_light_resolve_pending()) {
		return false;
	}
	const String resolve_error = solver->get_native_light_resolve_error();
	if (!resolve_error.is_empty()) {
		error_message = resolve_error;
		native_capture_pending = false;
		native_capture_queued = false;
		native_light_snapshots.clear();
		native_light_field_inputs.clear();
		has_shadow_capture_signature = false;
		return false;
	}
	_finish_native_light_capture();
	return true;
}


// Publishes the batch once every queued resolve has finished on the render thread.
bool LRTVolume3D::_complete_native_light_resolve() {
	if (!native_capture_pending) {
		return false;
	}
	return _poll_native_light_capture();
}

Dictionary LRTVolume3D::prepare_mesh_sdf(const Ref<Mesh> &p_mesh, int p_resolution) {
	return lrt::prepare_mesh_asset(p_mesh, p_resolution);
}

#ifdef TOOLS_ENABLED
Dictionary LRTVolume3D::prepare_export_data() {
	Dictionary report;
	report["ok"] = false;
	if (!is_inside_tree() || !_has_valid_volume_transform()) {
		report["error"] = "Export preparation requires an unscaled Volume in an isolated World3D.";
		return report;
	}
	// Prepare local assets for all scene contributors, including dynamic objects currently
	// outside this Volume. Their later entry must not require a runtime voxel capture.
	_collect_geometry(true);
	std::vector<LRTVolume::MeshInstance> meshes;
	String error;
	if (!_build_geometry_inputs(meshes, true, error, false, true)) {
		report["error"] = error;
		return report;
	}
	Ref<LRTVolume> prepared;
	prepared.instantiate();
	prepared->configure_sized(spacing, -volume_size * 0.5, volume_size);
	prepared->set_mesh_instances(meshes, false);
	const auto asset_result = prepared->bake_local_field_data(false, true);
	if (!asset_result.ok) {
		report["error"] = "Cannot prepare scene-local voxel assets.";
		return report;
	}
	const PackedStringArray scene_dependencies = prepared->store_prepared_dependencies();
	if (asset_result.sdf_instance_references > 0 && scene_dependencies.is_empty()) {
		report["error"] = "Cannot persist scene-local voxel assets.";
		return report;
	}
	_collect_geometry();
	if (!_build_geometry_inputs(meshes, true, error, false)) {
		report["error"] = error;
		return report;
	}
	prepared->set_mesh_instances(meshes);
	const LRTVolume::LocalBakeResult result = prepared->bake_local_field_data(false);
	if (!result.ok) {
		report["error"] = vformat("Volume preparation failed (SDF error %d).", result.preparation_error);
		return report;
	}
	const uint64_t fingerprint = _build_cache_fingerprint();
	if (!_material_inputs_unchanged()) {
		report["error"] = "Material inputs changed during export preparation. Prepare the scene again after the update completes.";
		return report;
	}
	PackedStringArray dependencies = prepared->store_prepared_dependencies();
	if (result.sdf_instance_references > 0 && dependencies.is_empty()) {
		report["error"] = "Cannot store prepared geometry and instance dependencies.";
		return report;
	}
	for (const String &path : scene_dependencies) {
		if (!dependencies.has(path)) {
			dependencies.push_back(path);
		}
	}
	if (fingerprint != 0 && !prepared->store_local_field_cache(fingerprint, true, dependencies)) {
		report["error"] = "Cannot store the prepared Volume field.";
		return report;
	}
	if (fingerprint != 0) {
		dependencies.push_back(lrt::asset_cache_directory().path_join(vformat("volume_%016x.lrt", fingerprint)));
	}
	report["ok"] = true;
	report["assets"] = dependencies;
	report["fingerprint"] = int64_t(fingerprint);
	return report;
}
#endif

static Ref<Material> static_capture_material(const Ref<Material> &p_material, Array &r_excluded) {
	if (p_material.is_null()) {
		return Ref<Material>();
	}
	const Ref<Material> next = static_capture_material(p_material->get_next_pass(), r_excluded);
	const Ref<BaseMaterial3D> source = p_material;
	ERR_FAIL_COND_V(source.is_null(), Ref<Material>());
	Ref<BaseMaterial3D> material = source->duplicate();
	material->set_next_pass(next);
	material->set_grow_enabled(false);
	material->set_billboard_mode(BaseMaterial3D::BILLBOARD_DISABLED);
	material->set_distance_fade(BaseMaterial3D::DISTANCE_FADE_DISABLED);
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, false);
	material->set_flag(BaseMaterial3D::FLAG_FIXED_SIZE, false);
	material->set_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE, false);
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR, false);
	material->set_flag(BaseMaterial3D::FLAG_UV2_USE_WORLD_TRIPLANAR, false);
	if (!material_pass_contributes(p_material)) {
		r_excluded.push_back(material->get_rid());
	}
	return material;
}

bool LRTVolume3D::_capture_mesh(const Receiver &p_receiver,
		int p_resolution, const Vector<int> &p_surfaces, LRTVolume::MeshInstance &r_mesh, String &r_error, bool p_async, Dictionary &r_images) {
	auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
	if (!lrt::mesh_triangles(p_receiver.mesh, *triangles, &p_surfaces)) {
		r_error = "LRT local voxel capture requires triangle surfaces.";
		return false;
	}
	r_mesh.triangles = triangles;
	const lrt::Vec3 &first = triangles->front().position[0];
	AABB bounds(Vector3(first.x, first.y, first.z), Vector3());
	for (const lrt::MeshTriangle &triangle : *triangles) {
		for (const lrt::Vec3 &vertex : triangle.position) {
			bounds.expand_to(Vector3(vertex.x, vertex.y, vertex.z));
		}
	}
	const double longest = bounds.get_longest_axis_size();
	if (longest <= 0.0) {
		r_error = "LRT local voxel capture has empty mesh bounds.";
		return false;
	}
	bounds = bounds.grow(longest / double(MAX(8, p_resolution)));
	const int material_resolution = MAX(1, (p_resolution + 1) / 2);
	Vector3i size;
	for (int axis = 0; axis < 3; axis++) {
		size[axis] = MAX(1, int(Math::ceil(bounds.size[axis] / bounds.get_longest_axis_size() * material_resolution)) + 1);
	}

	// Copy authored vertex arrays without deformation attributes or blend-shape targets.
	Ref<ArrayMesh> capture_mesh;
	capture_mesh.instantiate();
	Vector<Ref<Material>> materials;
	Array excluded;
	for (int surface = 0; surface < p_receiver.materials.size(); surface++) {
		Array arrays = p_receiver.mesh->surface_get_arrays(surface);
		arrays[Mesh::ARRAY_BONES] = Variant();
		arrays[Mesh::ARRAY_WEIGHTS] = Variant();
		for (int attribute = Mesh::ARRAY_CUSTOM0; attribute <= Mesh::ARRAY_CUSTOM3; attribute++) {
			arrays[attribute] = Variant();
		}
		capture_mesh->add_surface_from_arrays(p_receiver.mesh->surface_get_primitive_type(surface), arrays);
		const Ref<Material> material = p_surfaces.has(surface) ? static_capture_material(p_receiver.materials[surface], excluded) : Ref<Material>();
		materials.push_back(material);
		capture_mesh->surface_set_material(surface, material);
	}
	// The private proxy has no scenario, source transform or instance shader uniforms.
	RS *server = RS::get_singleton();
	const RID proxy = server->instance_create();
	RID colored_mesh;
	if (p_receiver.uses_instance_color) {
		colored_mesh = server->multimesh_create();
		server->multimesh_allocate_data(colored_mesh, 1, RSE::MULTIMESH_TRANSFORM_3D, true);
		server->multimesh_set_mesh(colored_mesh, capture_mesh->get_rid());
		server->multimesh_instance_set_transform(colored_mesh, 0, Transform3D());
		server->multimesh_instance_set_color(colored_mesh, 0, p_receiver.instance_color);
	}
	server->instance_set_base(proxy, colored_mesh.is_valid() ? colored_mesh : capture_mesh->get_rid());
	const Ref<Material> overlay = static_capture_material(p_receiver.authored_overlay, excluded);
	materials.push_back(overlay);
	server->instance_geometry_set_material_overlay(proxy, overlay.is_valid() ? overlay->get_rid() : RID());
	BaseMaterial3D::flush_changes();
	Callable callback;
	if (p_async) {
		PendingMaterialCapture request;
		request.geometry_key = p_receiver.get_key();
		request.size = size;
		request.materials = materials;
		request.mesh = capture_mesh;
		pending_material_captures[++material_capture_id] = request;
		callback = callable_mp(this, &LRTVolume3D::_material_capture_ready).bind(material_capture_id);
	}
	Dictionary options;
	options["conservative"] = true;
	options["excluded_materials"] = excluded;
	Dictionary images = server->bake_render_material_volume(proxy, bounds, size, p_surfaces, callback, options);
	server->free_rid(proxy);
	if (colored_mesh.is_valid()) {
		server->free_rid(colored_mesh);
	}
	voxel_captures++;
	if (p_async && bool(images.get("pending", false))) {
		return true;
	}
	if (p_async) {
		pending_material_captures.erase(material_capture_id);
	}
	const bool decoded = _decode_material_capture(images, size, r_mesh, r_error);
	r_images = images;
	return decoded;
}

void LRTVolume3D::_material_capture_ready(const Dictionary &p_images, uint64_t p_id) {
	auto request = pending_material_captures.find(p_id);
	if (request != pending_material_captures.end()) {
		request->second.images = p_images;
		request->second.ready = true;
	}
}

bool LRTVolume3D::_decode_material_capture(Dictionary &images, const Vector3i &p_requested_size,
		LRTVolume::MeshInstance &r_mesh, String &r_error) {
	if (images.has("error")) {
		r_error = images["error"];
		return false;
	}
	if (images.is_empty() || Vector3i(images.get("size", Vector3i())) != p_requested_size) {
		r_error = "Material volume capture failed or returned a mismatched request size.";
		return false;
	}
	const PackedByteArray albedo_data = images.get("albedo", PackedByteArray());
	const PackedByteArray emission_data = images.get("emission", PackedByteArray());
	const PackedByteArray emission_aniso_data = images.get("emission_aniso", PackedByteArray());
	PackedByteArray normal_bits_data = images.get("normal_bits", PackedByteArray());
	const Vector3i capture_size = images.get("material_size", p_requested_size);
	const Vector3i render_size = images.get("normal_size", p_requested_size * 2);
	const int material_stride = capture_size == render_size ? 1 : 2;
	const int albedo_directions = images.get("albedo_directions", 6);
	if ((albedo_directions != 1 && albedo_directions != 6) ||
			render_size != p_requested_size * 2 || capture_size != render_size / material_stride) {
		r_error = "Material capture geometry and attribute grids have inconsistent dimensions.";
		return false;
	}
	const int capture_count = capture_size.x * capture_size.y * capture_size.z;
	if (!images.has("bounds") || !images.has("transform")) {
		r_error = "Material capture is missing its geometry snapshot transform.";
		return false;
	}
	const Transform3D p_transform = images["transform"];
	const AABB world_capture_bounds = images["bounds"];
	const int render_count = render_size.x * render_size.y * render_size.z;
	if (albedo_data.size() != capture_count * albedo_directions * 2 || emission_data.size() != capture_count * 4 ||
			emission_aniso_data.size() != capture_count * 4 || normal_bits_data.size() != render_count * 4) {
		r_error = "LRT 三维材质捕获返回了无效的数据布局";
		return false;
	}
	std::shared_ptr<lrt::MaterialCapture> material = std::make_shared<lrt::MaterialCapture>();
	for (int axis = 0; axis < 3; axis++) {
		material->size[axis] = capture_size[axis];
	}
	material->albedo.resize(size_t(capture_count) * 3);
	material->occupied.resize(capture_count);
	// Match the native voxel pass's affine composition, including FP32 rounding.
	// Subtracting origins before scaling can select the opposite side of a voxel boundary.
	Transform3D uvw_to_world;
	uvw_to_world.origin = world_capture_bounds.position;
	uvw_to_world.basis.scale(world_capture_bounds.size);
	const Transform3D mesh_to_uvw = uvw_to_world.affine_inverse() * p_transform;
	if (r_mesh.raster_geometry && !bool(images.get("coverage_filtered", false))) {
		if (!r_mesh.triangles || r_mesh.triangles->empty()) {
			r_error = "LRT coverage filtering requires authored triangles.";
			return false;
		}
		const uint64_t started_usec = OS::get_singleton()->get_ticks_usec();
		std::vector<uint32_t> coverage(render_count);
		for (int index = 0; index < render_count; index++) {
			coverage[size_t(index)] = decode_uint32(normal_bits_data.ptr() + index * 4);
		}
		const Transform3D mesh_to_grid = Transform3D(Basis::from_scale(Vector3(render_size))) * mesh_to_uvw;
		const int size[3] = { render_size.x, render_size.y, render_size.z };
		const uint64_t removed = lrt::filter_raster_geometry_coverage(*r_mesh.triangles, to_lrt_transform(mesh_to_grid), size, coverage);
		uint8_t *filtered_bytes = normal_bits_data.ptrw();
		for (int index = 0; index < render_count; index++) {
			encode_uint32(coverage[size_t(index)], filtered_bytes + index * 4);
		}
		images["normal_bits"] = normal_bits_data;
		images["coverage_filtered"] = true;
		images["coverage_removed"] = int64_t(removed);
		images["coverage_filter_usec"] = int64_t(OS::get_singleton()->get_ticks_usec() - started_usec);
	}
	material->uvw_offset = to_lrt(mesh_to_uvw.origin);
	material->uvw_basis_x = to_lrt(mesh_to_uvw.basis.get_column(0));
	material->uvw_basis_y = to_lrt(mesh_to_uvw.basis.get_column(1));
	material->uvw_basis_z = to_lrt(mesh_to_uvw.basis.get_column(2));
	const uint8_t *albedo_bytes = albedo_data.ptr();
	const uint8_t *emission_bytes = emission_data.ptr();
	const uint8_t *aniso_bytes = emission_aniso_data.ptr();
	const uint8_t *normal_bytes = normal_bits_data.ptr();
	for (int z = 0; z < capture_size.z; z++) {
		for (int y = 0; y < capture_size.y; y++) {
			for (int x = 0; x < capture_size.x; x++) {
				const int index = x + capture_size.x * (y + capture_size.y * z);
				Color albedo;
				float best_luminance = -1.0f;
				for (int direction = 0; direction < albedo_directions; direction++) {
					const int packed_index = x + capture_size.x * (y + capture_size.y * (z * albedo_directions + direction));
					const uint16_t packed = uint16_t(albedo_bytes[packed_index * 2]) |
							(uint16_t(albedo_bytes[packed_index * 2 + 1]) << 8);
					const Color candidate(float(packed & 31) / 31.0f, float((packed >> 5) & 63) / 63.0f,
							float((packed >> 11) & 31) / 31.0f);
					const float luminance = candidate.get_luminance();
					if (luminance > best_luminance) {
						best_luminance = luminance;
						albedo = candidate;
					}
				}
				const uint32_t packed_emission = decode_uint32(emission_bytes + index * 4);
				const uint32_t packed_aniso = decode_uint32(aniso_bytes + index * 4);
				const uint32_t emission_mode = packed_aniso >> 30;
				if (emission_mode != 0 && material->emission_sides.empty()) {
					material->emission_sides.assign(size_t(capture_count), lrt::EMISSION_FRONT);
				}
				if (!material->emission_sides.empty()) {
					material->emission_sides[size_t(index)] = uint8_t(emission_mode + 1);
				}
				Color emission = Color::from_rgbe9995(packed_emission);
				float normalized_squared = 0.0f;
				for (int direction = 0; direction < 6; direction++) {
					const float weight = float((packed_aniso >> (direction * 5)) & 31) / 31.0f;
					normalized_squared += weight * weight;
				}
				emission *= Math::sqrt(normalized_squared);
				const size_t value_base = size_t(index) * 3;
				if (material->emission.empty() && (emission.r > 0.0f || emission.g > 0.0f || emission.b > 0.0f)) {
					material->emission.resize(size_t(capture_count) * 3, 0.0f);
				}
				for (int channel = 0; channel < 3; channel++) {
					material->albedo[value_base + channel] = albedo[channel];
					if (!material->emission.empty()) {
						material->emission[value_base + channel] = emission[channel];
					}
				}
				for (int dz = 0; dz < material_stride && !material->occupied[size_t(index)]; dz++) {
					for (int dy = 0; dy < material_stride && !material->occupied[size_t(index)]; dy++) {
						for (int dx = 0; dx < material_stride; dx++) {
							const int normal_index = x * material_stride + dx + render_size.x * ((y * material_stride + dy) + render_size.y * (z * material_stride + dz));
							if (decode_uint32(normal_bytes + normal_index * 4) != 0) {
								material->occupied[size_t(index)] = 1;
								break;
							}
						}
					}
				}
			}
		}
	}
	material->input_signatures = r_mesh.capture_input_signatures;
	material->input_signatures.geometry = mix_signature(material->input_signatures.geometry, Variant(world_capture_bounds).hash());
	if (r_mesh.previous_material && r_mesh.previous_material->input_signatures.geometry == material->input_signatures.geometry) {
		// Native voxel imageStore winners may differ between captures. A channel whose
		// authored dependencies did not change retains its already published snapshot.
		const lrt::MaterialCapture &previous = *r_mesh.previous_material;
		if (previous.input_signatures.reflectance == material->input_signatures.reflectance) {
			material->albedo = previous.albedo;
		}
		if (previous.input_signatures.emission == material->input_signatures.emission) {
			material->emission = previous.emission;
			material->emission_sides = previous.emission_sides;
		}
	}
	r_mesh.previous_material.reset();
	for (int index = 0; index < capture_count; index++) {
		if (material->occupied[size_t(index)]) {
			continue;
		}
		std::fill_n(material->albedo.data() + size_t(index) * 3, 3, 0.0f);
		if (!material->emission.empty()) {
			std::fill_n(material->emission.data() + size_t(index) * 3, 3, 0.0f);
		}
		if (!material->emission_sides.empty()) {
			material->emission_sides[size_t(index)] = lrt::EMISSION_FRONT;
		}
	}
	if (r_mesh.raster_geometry) {
		const Vector3 cell_size = world_capture_bounds.size / Vector3(render_size);
		// Native voxel normals use six inclusive axis bits followed by 26 direction bins.
		// Convert mesh-local bins into the capture grid's normal frame once per readback.
		const int native_offsets[26][3] = {
			{ -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 },
			{ -1, -1, -1 }, { -1, -1, 0 }, { -1, -1, 1 }, { -1, 0, -1 }, { -1, 0, 1 },
			{ -1, 1, -1 }, { -1, 1, 0 }, { -1, 1, 1 }, { 0, -1, -1 }, { 0, -1, 1 },
			{ 0, 1, -1 }, { 0, 1, 1 }, { 1, -1, -1 }, { 1, -1, 0 }, { 1, -1, 1 },
			{ 1, 0, -1 }, { 1, 0, 1 }, { 1, 1, -1 }, { 1, 1, 0 }, { 1, 1, 1 },
		};
		uint32_t converted[26];
		for (int direction = 0; direction < 26; direction++) {
			converted[direction] = lrt::coarse_normal_mask(lrt::Vec3(
					native_offsets[direction][0] * cell_size.x,
					native_offsets[direction][1] * cell_size.y,
					native_offsets[direction][2] * cell_size.z));
		}
		std::vector<uint32_t> surface(render_count, 0u);
		for (int index = 0; index < render_count; index++) {
			uint32_t normal_bits = decode_uint32(normal_bytes + index * 4) >> 6;
			for (int direction = 0; normal_bits != 0; direction++, normal_bits >>= 1) {
				if (normal_bits & 1u) {
					surface[size_t(index)] |= converted[direction];
				}
			}
		}
		const Transform3D grid_to_world(Basis::from_scale(cell_size), world_capture_bounds.position);
		auto capture = std::make_shared<lrt::RasterGeometryCapture>();
		const int size[3] = { render_size.x, render_size.y, render_size.z };
		if (!lrt::bake_raster_geometry_capture(surface, size, *material,
				to_lrt_transform(p_transform.affine_inverse() * grid_to_world), *capture)) {
			r_error = "LRT captured coarse surface construction failed.";
			return false;
		}
		r_mesh.raster_capture = std::move(capture);
	}
	r_mesh.material = material;
	r_mesh.material_signature = lrt::material_field_input_signature(*r_mesh.triangles, material.get());
	return true;
}

static uint64_t mesh_capture_bytes(const std::shared_ptr<const std::vector<lrt::MeshTriangle>> &p_triangles,
		const std::shared_ptr<const lrt::MaterialCapture> &p_material,
		const std::shared_ptr<const lrt::RasterGeometryCapture> &p_raster,
		std::set<const void *> *p_counted) {
	std::set<const void *> local_counted;
	std::set<const void *> &counted = p_counted != nullptr ? *p_counted : local_counted;
	uint64_t bytes = 0;
	if (p_triangles && counted.insert(p_triangles.get()).second) {
		bytes += uint64_t(p_triangles->capacity()) * sizeof(lrt::MeshTriangle);
	}
	if (p_material && counted.insert(p_material.get()).second) {
		bytes += uint64_t(p_material->albedo.capacity() + p_material->emission.capacity()) * sizeof(float);
		bytes += uint64_t(p_material->occupied.capacity() + p_material->emission_sides.capacity()) * sizeof(uint8_t);
	}
	if (p_raster && counted.insert(p_raster.get()).second) {
		bytes += sizeof(lrt::RasterGeometryCapture) + lrt::asset_field_bytes(p_raster->geometry) +
				p_raster->material.albedo.capacity() + p_raster->material.emission.capacity() * sizeof(float) + p_raster->material.emission_sides.capacity();
	}
	return bytes;
}

std::shared_ptr<const LRTVolume3D::MeshCaptureData> LRTVolume3D::_share_mesh_capture(uint64_t p_key, MeshCaptureData p_data) {
	const auto found = shared_mesh_capture_cache.find(p_key);
	if (found != shared_mesh_capture_cache.end()) {
		std::shared_ptr<const MeshCaptureData> shared = found->second.lock();
		if (shared && shared->shared_signature == p_data.shared_signature && shared->content_signature == p_data.content_signature) {
			return shared;
		}
	}
	std::shared_ptr<const MeshCaptureData> shared(new MeshCaptureData(std::move(p_data)), [p_key](const MeshCaptureData *p_capture) {
		delete p_capture;
		const auto entry = shared_mesh_capture_cache.find(p_key);
		// An older revision or a cleared registry must not erase a live replacement.
		if (entry != shared_mesh_capture_cache.end() && entry->second.expired()) {
			shared_mesh_capture_cache.erase(entry);
		}
	});
	shared_mesh_capture_cache[p_key] = shared;
	return shared;
}

void LRTVolume3D::clear_shared_mesh_capture_cache() {
	shared_mesh_capture_cache.clear();
}


// Every SDF input keeps asset-local geometry. Its full affine basis is sampled in volume space,
// so rotations and non-uniform scales never force a world-space copy of the distance field.
std::vector<LRTVolume3D::Receiver> LRTVolume3D::_local_voxel_inputs(String &r_error, bool p_all_inputs) {
	std::vector<Receiver> inputs;
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes || receiver.multimesh.is_null()) {
			inputs.push_back(receiver);
			continue;
		}
		RS *server = RS::get_singleton();
		int count = server->multimesh_get_instance_count(receiver.multimesh);
		const int visible = server->multimesh_get_visible_instances(receiver.multimesh);
		if (!p_all_inputs && visible >= 0) {
			count = MIN(count, visible);
		}
		const MultiMeshInstance3D *node = Object::cast_to<MultiMeshInstance3D>(ObjectDB::get_instance(receiver.instance_id));
		const Ref<MultiMesh> multimesh = node ? node->get_multimesh() : Ref<MultiMesh>();
		const bool format_2d = multimesh.is_valid() && multimesh->get_transform_format() == MultiMesh::TRANSFORM_2D;
		const bool colors = multimesh.is_valid() && multimesh->is_using_colors();
		const bool custom_data = multimesh.is_valid() && multimesh->is_using_custom_data();
		const int transform_stride = format_2d ? 8 : 12;
		const int stride = transform_stride + (colors ? 4 : 0) + (custom_data ? 4 : 0);
		const Vector<float> buffer = server->multimesh_get_buffer(receiver.multimesh);
		if (buffer.size() % stride != 0 || count > buffer.size() / stride) {
			r_error = "Invalid LRT MultiMesh buffer layout.";
			return {};
		}
		std::set<std::array<float, 4>> allocated_colors;
		if (colors) {
			for (int offset = transform_stride; offset < buffer.size(); offset += stride) {
				allocated_colors.insert({ buffer[offset], buffer[offset + 1], buffer[offset + 2], buffer[offset + 3] });
			}
		}
		for (auto cached = mesh_capture_cache.begin(); cached != mesh_capture_cache.end();) {
			const Receiver &owner = cached->second.source;
			const Color &color = owner.instance_color;
			if (owner.render_instance == receiver.render_instance && owner.uses_instance_color &&
					allocated_colors.find({ color.r, color.g, color.b, color.a }) == allocated_colors.end()) {
				cached = mesh_capture_cache.erase(cached);
			} else {
				++cached;
			}
		}
		std::map<std::array<float, 4>, size_t> groups;
		std::vector<std::shared_ptr<std::vector<lrt::MeshCopy>>> copies;
		for (int index = 0; index < count; index++) {
			const float *values = buffer.ptr() + index * stride;
			const Color color = colors ? Color(values[transform_stride], values[transform_stride + 1], values[transform_stride + 2], values[transform_stride + 3]) : Color(1, 1, 1, 1);
			const std::array<float, 4> key = { color.r, color.g, color.b, color.a };
			auto found = groups.find(key);
			if (found == groups.end()) {
				Receiver input = receiver;
				input.instance_color = color;
				input.uses_instance_color = colors;
				auto group = std::make_shared<std::vector<lrt::MeshCopy>>();
				input.copies = group;
				found = groups.emplace(key, copies.size()).first;
				copies.push_back(group);
				inputs.push_back(std::move(input));
			}
			Transform3D transform;
			transform.basis.set_column(0, Vector3(values[0], values[4], format_2d ? 0.0f : values[8]));
			transform.basis.set_column(1, Vector3(values[1], values[5], format_2d ? 0.0f : values[9]));
			transform.basis.set_column(2, format_2d ? Vector3(0, 0, 1) : Vector3(values[2], values[6], values[10]));
			transform.origin = Vector3(values[3], values[7], format_2d ? 0.0f : values[11]);
			copies[found->second]->push_back({ to_lrt_transform(transform), nullptr });
		}
	}
	return inputs;
}

bool LRTVolume3D::_build_geometry_inputs(std::vector<LRTVolume::MeshInstance> &r_meshes,
		bool p_validate_mesh_content, String &r_error, bool p_async, bool p_all_inputs) {
	r_meshes.clear();
	std::map<uint64_t, uint64_t> mesh_content_signatures;
	auto get_mesh_content_signature = [&mesh_content_signatures](const Ref<Mesh> &p_mesh) {
		const uint64_t mesh_rid = p_mesh->get_rid().get_id();
		auto content = mesh_content_signatures.find(mesh_rid);
		if (content == mesh_content_signatures.end()) {
			content = mesh_content_signatures.emplace(mesh_rid, mesh_content_signature(p_mesh)).first;
		}
		return content->second;
	};
	const std::vector<Receiver> inputs = _local_voxel_inputs(r_error, p_all_inputs);
	if (!r_error.is_empty()) {
		return false;
	}
	for (const Receiver &receiver : inputs) {
		if (!receiver.contributes) {
			continue;
		}
		const Ref<Mesh> &mesh = receiver.mesh;
		const Transform3D &transform = receiver.transform;
		LRTVolume::MeshInstance entry;
		entry.transform = to_lrt_transform(transform);
		entry.raster_geometry = true;
		entry.sdf_resolution = receiver.sdf_resolution;
		entry.layer_mask = receiver.layer_mask;
		uint64_t key = mix_signature(receiver.mesh_content_signature, receiver.sdf_resolution);
		key = mix_signature(key, receiver.material_signature);
		if (receiver.uses_instance_color) {
			key = mix_signature(key, Variant(receiver.instance_color).hash());
		}
		const auto cached = mesh_capture_cache.find(receiver.get_key());
		uint64_t content = 0;
		bool valid = cached != mesh_capture_cache.end() && cached->second.key == key && cached->second.data->material;
		if (valid && p_validate_mesh_content) {
			content = get_mesh_content_signature(mesh);
			valid = cached->second.data->content_signature == content;
		}
		if (valid) {
			entry.local_capture_key = cached->second.data->shared_signature;
			entry.triangles = cached->second.data->triangles;
			entry.material = cached->second.data->material;
			entry.raster_capture = cached->second.data->raster_capture;
			entry.material_signature = cached->second.data->material_signature;
			voxel_capture_reuses++;
		} else {
			content = content != 0 ? content : get_mesh_content_signature(mesh);
			uint64_t shared_key = mix_signature(content, receiver.sdf_resolution);
			if (receiver.uses_instance_color) {
				shared_key = mix_signature(shared_key, Variant(receiver.instance_color).hash());
			}
			entry.capture_input_signatures.geometry = shared_key;
			for (int surface : receiver.contributing_surfaces) {
				const Ref<Material> &material = receiver.materials[surface];
				shared_key = mix_signature(shared_key, surface);
				entry.capture_input_signatures.geometry = mix_signature(entry.capture_input_signatures.geometry, surface);
				shared_key = mix_signature(shared_key, _material_content_signature(material));
				entry.capture_input_signatures.reflectance = mix_signature(entry.capture_input_signatures.reflectance,
						_material_content_signature(material, false, lrt::LOCAL_REFLECTANCE));
				entry.capture_input_signatures.emission = mix_signature(entry.capture_input_signatures.emission,
						_material_content_signature(material, false, lrt::LOCAL_EMISSION));
			}
			shared_key = mix_signature(shared_key, _material_content_signature(receiver.authored_overlay));
			entry.capture_input_signatures.reflectance = mix_signature(entry.capture_input_signatures.reflectance,
					_material_content_signature(receiver.authored_overlay, false, lrt::LOCAL_REFLECTANCE));
			entry.capture_input_signatures.emission = mix_signature(entry.capture_input_signatures.emission,
					_material_content_signature(receiver.authored_overlay, false, lrt::LOCAL_EMISSION));
			entry.capture_input_signatures.emission = mix_signature(entry.capture_input_signatures.emission,
					bool(GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units")));
			shared_key = mix_signature(shared_key, bool(GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units")));
			entry.local_capture_key = shared_key;
			const auto found = shared_mesh_capture_cache.find(shared_key);
			const auto shared = found == shared_mesh_capture_cache.end() ? nullptr : found->second.lock();
			const bool shared_valid = shared && shared->material;
			Dictionary captured_images;
			if (shared_valid) {
				entry.triangles = shared->triangles;
				entry.material = shared->material;
				entry.raster_capture = shared->raster_capture;
				entry.material_signature = shared->material_signature;
				voxel_capture_reuses++;
			} else {
				if (cached != mesh_capture_cache.end()) {
					entry.previous_material = cached->second.data->material;
				}
				Dictionary images;
				if (lrt::load_static_voxel_capture(shared_key, images)) {
					auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
					if (!lrt::mesh_triangles(mesh, *triangles, &receiver.contributing_surfaces)) {
						r_error = "LRT cached local voxels require triangle surfaces.";
						return false;
					}
					entry.triangles = triangles;
					if (!_decode_material_capture(images, Vector3i(images.get("size", Vector3i())), entry, r_error)) {
						return false;
					}
					voxel_cache_loads++;
				} else if (!_capture_mesh(receiver, entry.sdf_resolution, receiver.contributing_surfaces, entry, r_error, p_async, captured_images)) {
					return false;
				}
			}
			const bool cacheable = _material_revision_signature(receiver) == receiver.material_revision_signature;
			if (cacheable && !captured_images.is_empty() && !lrt::store_static_voxel_capture(shared_key, captured_images)) {
				r_error = "Cannot persist LRT mesh-local voxel capture.";
				return false;
			}
			if (!entry.material) {
				PendingMaterialCapture &request = pending_material_captures.at(material_capture_id);
				request.mesh_index = int(r_meshes.size());
				request.cache_key = cacheable ? key : 0;
				request.shared_cache_key = shared_key;
			}
			MeshCaptureCache cache;
			cache.source = receiver;
			cache.source.copies.reset();
			cache.key = cacheable ? key : 0;
			if (shared_valid) {
				cache.data = shared;
			} else {
				MeshCaptureData capture_data;
				capture_data.shared_signature = shared_key;
				capture_data.content_signature = content;
				capture_data.triangles = entry.triangles;
				capture_data.material = entry.material;
				capture_data.raster_capture = entry.raster_capture;
				capture_data.material_signature = entry.material_signature;
				cache.data = cacheable && entry.material ? _share_mesh_capture(shared_key, std::move(capture_data)) :
						std::make_shared<const MeshCaptureData>(std::move(capture_data));
			}
			mesh_capture_cache[receiver.get_key()] = std::move(cache);
		}
		entry.copies = receiver.copies;
		entry.instanced = receiver.copies != nullptr;
		r_meshes.push_back(std::move(entry));
	}
	return true;
}

// --- Background build ------------------------------------------------------

void LRTVolume3D::_bake_task(void *p_userdata) {
	LRTVolume3D *volume = static_cast<LRTVolume3D *>(p_userdata);
	const uint64_t worker_started_usec = OS::get_singleton()->get_ticks_usec();
	BuildJob *job = volume->job;
	if (job == nullptr || volume->solver.is_null()) {
		return;
	}
	const double queue_wait_ms = double(worker_started_usec - job->queued_usec) / 1000.0;
	for (PendingMaterialCapture &capture : job->captures) {
		bool decoded = _decode_material_capture(capture.images, capture.size,
				job->meshes[capture.mesh_index], job->capture_error);
		if (decoded && capture.cache_key != 0 && !lrt::store_static_voxel_capture(capture.shared_cache_key, capture.images)) {
			job->capture_error = "Cannot persist LRT mesh-local voxel capture.";
			decoded = false;
		}
		// Cache publication only needs the capture keys and mesh index after decoding.
		// Drop this reference without clearing a dictionary shared with a readback callback.
		capture.images = Dictionary();
		if (!decoded) {
			for (PendingMaterialCapture &pending : job->captures) {
				pending.images = Dictionary();
			}
			job->done_usec = OS::get_singleton()->get_ticks_usec();
			job->done.store(true);
			return;
		}
	}
	const double capture_decode_ms = double(OS::get_singleton()->get_ticks_usec() - worker_started_usec) / 1000.0;
	volume->solver->set_mesh_instances(job->meshes);
	job->result = volume->solver->bake_local_field_data(false, job->prepare_cached_assets);
	job->result.queue_wait_ms = queue_wait_ms;
	job->result.capture_decode_ms = capture_decode_ms;
	job->result.geometry_input_ms = job->geometry_input_ms;
	job->done_usec = OS::get_singleton()->get_ticks_usec();
	job->result.worker_total_ms = double(job->done_usec - worker_started_usec) / 1000.0;
	job->done.store(true);
}

// Cancels the running bake and lets it finish before anything else touches the solver: the
// bake only reads plain data, so it stops at the next slice and the wait is short.
void LRTVolume3D::_cancel_build() {
	pending_material_captures.clear();
	if (task_id != 0) {
		if (solver.is_valid()) {
			solver->request_cancel();
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		task_id = 0;
		// The superseded bake stops at the next slice and its result is thrown away.
		cancelled_builds++;
	}
	if (job != nullptr) {
		memdelete(job);
		job = nullptr;
	}
	if (local_apply_pending && solver.is_valid()) {
		solver->finish_apply_local_field(true);
		local_apply_pending = false;
	}
	building = false;
	rebuild_pending = false;
	pending_rebuild_reasons = REBUILD_REASON_NONE;
	active_rebuild_reasons = REBUILD_REASON_NONE;
}

void LRTVolume3D::_queue_build(uint32_t p_reasons) {
	generation++;
	if (!rebuild_pending) {
		pending_build_queued_usec = OS::get_singleton()->get_ticks_usec();
	}
	rebuild_pending = true;
	pending_rebuild_reasons |= p_reasons;
}

bool LRTVolume3D::_try_load_build_cache(uint64_t p_fingerprint) {
	if (p_fingerprint == 0 ||
			p_fingerprint == last_cache_lookup_fingerprint || building || local_apply_pending) {
		return false;
	}
	last_cache_lookup_fingerprint = p_fingerprint;
	if (solver.is_null()) {
		solver.instantiate();
		solver->set_render_owner(get_instance_id());
	}
	LRTVolume::LocalBakeResult cached;
	solver->set_external_gi_enabled(external_gi_enabled);
	solver->set_receive_sky(receive_sky);
	if (!solver->load_local_field_cache(p_fingerprint, cached)) {
		build_data_missing = serialized_build_cache_fingerprint == p_fingerprint;
		return false;
	}
	build_data_missing = false;
	error_message = String();
	editor_rebuild_requested = true;
	generation++;
	// Cache loads publish the same backend operator identity as a freshly baked field.
	// Otherwise the first subsequent edit compares against the default key and resets history.
	solver->prepare_shared_gpu_resources();
	solver->set_geometry_snapshot_input_usec(OS::get_singleton()->get_ticks_usec());
	if (!solver->begin_apply_local_field(false)) {
		error_message = "LRT cached local field upload failed.";
		editor_rebuild_requested = false;
		return false;
	}
	pending_apply_result = cached;
	pending_apply_generation = generation;
	pending_apply_reasons = REBUILD_REASON_FORCED;
	pending_apply_cache_fingerprint = p_fingerprint;
	local_apply_pending = true;
	{
		// The cached field is immediately usable, but its geometry assets may not be resident
		// or even present on disk. Prepare them before an edit needs them, using the same queue.
		_queue_build(REBUILD_REASON_CACHE_ASSETS);
	}
	return true;
}

void LRTVolume3D::_start_build() {
	if (building || local_apply_pending || rebuild_suppressed || !rebuild_pending) {
		return;
	}
	pending_material_captures.clear();
	const uint32_t build_reasons = pending_rebuild_reasons;
	active_build_queued_usec = pending_build_queued_usec;
	rebuild_pending = false;
	pending_rebuild_reasons = REBUILD_REASON_NONE;
	if (solver.is_null()) {
		solver.instantiate();
		solver->set_render_owner(get_instance_id());
	}
	// The prototype grid is the fixed lab region; the node exposes the same region as a box
	// centred on the node, which keeps [-3,-0.5,-3]..[3,3.5,3] for the fixtures.
	const Vector3 effective_size = _effective_volume_size();
	const double effective_spacing = _effective_spacing();
	solver->prepare_shared_gpu_resources();
	std::vector<LRTVolume::MeshInstance> meshes;
	String material_error;
	const uint64_t geometry_input_started_usec = OS::get_singleton()->get_ticks_usec();
	const bool geometry_input_ready = _build_geometry_inputs(meshes, (build_reasons & REBUILD_REASON_FORCED) != 0, material_error);
	const double geometry_input_ms = double(OS::get_singleton()->get_ticks_usec() - geometry_input_started_usec) / 1000.0;
	last_geometry_input_ms += geometry_input_ms;
	if (!geometry_input_ready) {
		error_message = material_error.is_empty() ? "LRT material capture failed." : material_error;
		pending_material_captures.clear();
		editor_rebuild_requested = false;
		return;
	}

	error_message = String();
	const Vector3 grid_min = -effective_size * 0.5;
	solver->configure_sized(effective_spacing, grid_min, effective_size);
	solver->set_external_gi_enabled(external_gi_enabled);
	solver->set_receive_sky(receive_sky);
	solver->clear_cancel();

	// The solver rechecks grid compatibility before preserving the propagated field.
	pending_preserve_history = has_applied_configuration;

	job = memnew(BuildJob);
	job->meshes = std::move(meshes);
	solver->set_geometry_snapshot_input_usec(geometry_input_started_usec);
	job->prepare_cached_assets = build_reasons == REBUILD_REASON_CACHE_ASSETS &&
			_build_cache_fingerprint() == applied_build_cache_fingerprint;
	job->generation = generation;
	job->reasons = build_reasons;
	job->queued_usec = OS::get_singleton()->get_ticks_usec();
	job->geometry_input_ms = geometry_input_ms;
	// Persistent identity reads mesh content and renderer-side instance uniforms. Live motion
	// already has revision/transform signatures; only an explicit build needs the disk key here.
	if (job->prepare_cached_assets) {
		job->cache_fingerprint = applied_build_cache_fingerprint;
	} else if (Engine::get_singleton()->is_editor_hint() && editor_rebuild_requested) {
		job->cache_fingerprint = _build_cache_fingerprint();
	}
	active_rebuild_reasons = build_reasons;
	building = true;
	build_start_frame = scheduler_frame;
	_launch_bake();
}

void LRTVolume3D::_launch_bake() {
	if (job == nullptr || task_id != 0 || job->done.load()) {
		return;
	}
	for (const auto &request : pending_material_captures) {
		if (!request.second.ready) {
			return;
		}
	}
	for (const auto &request : pending_material_captures) {
		PendingMaterialCapture capture = request.second;
		capture.materials.clear();
		capture.mesh.unref();
		job->captures.push_back(std::move(capture));
	}
	pending_material_captures.clear();
	task_id = WorkerThreadPool::get_singleton()->add_native_task(&LRTVolume3D::_bake_task, this, false, "LRT local field bake");
}

void LRTVolume3D::_poll_build() {
	_launch_bake();
	if (local_apply_pending) {
		const uint64_t apply_finish_started_usec = OS::get_singleton()->get_ticks_usec();
		Dictionary applied = solver->finish_apply_local_field();
		last_apply_finish_ms = double(OS::get_singleton()->get_ticks_usec() - apply_finish_started_usec) / 1000.0;
		if (applied.is_empty() && solver->is_apply_pending()) {
			return;
		}
		local_apply_pending = false;
		if (applied.is_empty()) {
			error_message = "LRT 局部场上传失败";
			return;
		}
		const uint64_t publish_started_usec = OS::get_singleton()->get_ticks_usec();
		_finish_build_apply(applied);
		last_build_publish_ms = double(OS::get_singleton()->get_ticks_usec() - publish_started_usec) / 1000.0;
		return;
	}
	if (job == nullptr || !job->done.load()) {
		return;
	}
	// Each published receiver layout must publish its coherent source before the next
	// layout replaces it. Continuous input otherwise starts another upload on the deferred
	// capture frame and postpones that capture indefinitely, in both editor and runtime.
	if (deferred_receiver_unit_field_frame != UINT64_MAX) {
		return;
	}
	// Receiver offsets belong to the applied local field. Let its coherent light snapshot finish
	// before replacing that layout; the queued build already holds the latest geometry and can be
	// applied on the next frame without cancelling every in-flight capture during continuous motion.
	if (native_capture_pending) {
		SubViewport *viewport = Object::cast_to<SubViewport>(get_viewport());
		bool viewport_draws = viewport == nullptr || viewport->get_update_mode() != SubViewport::UPDATE_DISABLED;
#ifdef TOOLS_ENABLED
		// The authored scene viewport is disabled in the editor, but the visible 3D views
		// render its World3D. Their in-flight captures must finish before replacing the layout.
		Node3DEditor *editor = Node3DEditor::get_singleton();
		if (!viewport_draws && Engine::get_singleton()->is_editor_hint() && editor != nullptr) {
			for (unsigned int index = 0; index < Node3DEditor::VIEWPORTS_COUNT; index++) {
				Node3DEditorViewport *editor_view = editor->get_editor_viewport(index);
				const SubViewport *view = editor_view->get_viewport_node();
				if (editor_view->is_visible_in_tree() && view->find_world_3d() == get_world_3d() && view->get_update_mode() != SubViewport::UPDATE_DISABLED) {
					viewport_draws = true;
					break;
				}
			}
		}
#endif
		if (viewport_draws) {
			return;
		}
		// Drop the in-flight batch so the newest local layout replaces it instead of the
		// obsolete one publishing after the layout already changed.
		native_capture_pending = false;
		native_capture_queued = false;
	}
	if (task_id != 0) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		task_id = 0;
	}
	BuildJob *finished = job;
	job = nullptr;
	building = false;
	build_done_frame = scheduler_frame;
	LRTVolume::LocalBakeResult result = finished->result;
	result.publish_delay_ms = double(OS::get_singleton()->get_ticks_usec() - finished->done_usec) / 1000.0;
	const int finished_generation = finished->generation;
	const uint32_t finished_reasons = finished->reasons;
	const uint64_t finished_cache_fingerprint = finished->cache_fingerprint;
	const bool prepared_cached_assets = finished->prepare_cached_assets;
	const String capture_error = finished->capture_error;
	if (capture_error.is_empty()) {
		for (const PendingMaterialCapture &capture : finished->captures) {
			auto cached = mesh_capture_cache.find(capture.geometry_key);
			if (capture.cache_key != 0 && cached != mesh_capture_cache.end() && cached->second.key == capture.cache_key) {
				const LRTVolume::MeshInstance &mesh = finished->meshes[capture.mesh_index];
				MeshCaptureData capture_data = *cached->second.data;
				capture_data.material = mesh.material;
				capture_data.raster_capture = mesh.raster_capture;
				capture_data.material_signature = mesh.material_signature;
				cached->second.data = _share_mesh_capture(capture.shared_cache_key, std::move(capture_data));
			}
		}
	}
	memdelete(finished);
	active_rebuild_reasons = REBUILD_REASON_NONE;
	if (result.cancelled) {
		return;
	}
	if (finished_generation <= applied_generation) {
		// Jobs are launched serially, but keep the version guard at the application boundary:
		// an older result can never replace a state already shown by a newer generation.
		dropped_builds++;
		return;
	}
	if (!result.ok) {
		editor_rebuild_requested = false;
		if (!capture_error.is_empty()) {
			error_message = capture_error;
		} else if (result.needs_axis_aligned) {
			error_message = "解析盒后端不支持旋转的盒体";
		} else if (result.preparation_error == lrt::MESH_SDF_BAKE_DEGENERATE_BOUNDS) {
			error_message = "LRT SDF 准备失败：Mesh 边界退化，无法建立体素尺寸";
		} else if (result.preparation_error == lrt::MESH_SDF_BAKE_TOO_LARGE) {
			error_message = "LRT SDF 准备失败：单个 Mesh 超过 33,554,432 个 SDF 样点";
		} else if (result.preparation_error == lrt::MESH_SDF_BAKE_NO_SURFACE) {
			error_message = "LRT SDF 准备失败：Mesh 没有可体素化的表面";
		} else if (result.preparation_error == lrt::MESH_SDF_BAKE_EMPTY) {
			error_message = "LRT SDF 准备失败：Mesh 没有三角形";
		} else {
			error_message = "LRT 局部场构建未完成";
		}
		return;
	}
	const uint64_t apply_begin_started_usec = OS::get_singleton()->get_ticks_usec();
	if (result.unchanged) {
		if (prepared_cached_assets) {
			solver->finish_cached_asset_preparation();
		}
		// The field the GPU holds is byte-identical to this result, so the upload, the receiver
		// layout and the propagated state all stay as they are.
		pending_apply_result = result;
		pending_apply_generation = finished_generation;
		pending_apply_reasons = finished_reasons;
		pending_apply_cache_fingerprint = finished_cache_fingerprint;
		Dictionary applied = solver->describe_unchanged_local_field();
		applied["preserved_history"] = true;
		applied["unchanged"] = true;
		_finish_build_apply(applied);
		return;
	}
	if (!solver->begin_apply_local_field(pending_preserve_history)) {
		error_message = "LRT local field upload failed.";
		editor_rebuild_requested = false;
		return;
	}
	last_apply_begin_ms = double(OS::get_singleton()->get_ticks_usec() - apply_begin_started_usec) / 1000.0;
	pending_apply_result = result;
	pending_apply_generation = finished_generation;
	pending_apply_reasons = finished_reasons;
	pending_apply_cache_fingerprint = finished_cache_fingerprint;
	// All staged transfers precede descriptor publication in one render-thread submission.
	// Publish immediately when that submission has already completed.
	Dictionary applied = solver->finish_apply_local_field();
	if (applied.is_empty()) {
		if (solver->is_apply_pending()) {
			// The render-thread callback has not completed yet. Poll its completion next frame
			// without splitting or resubmitting the transfer batch.
			local_apply_pending = true;
			return;
		}
		error_message = "LRT 局部场上传失败";
		editor_rebuild_requested = false;
		return;
	}
	const uint64_t publish_started_usec = OS::get_singleton()->get_ticks_usec();
	_finish_build_apply(applied);
	last_build_publish_ms = double(OS::get_singleton()->get_ticks_usec() - publish_started_usec) / 1000.0;
}

void LRTVolume3D::_finish_build_apply(Dictionary p_applied) {
	Dictionary applied = p_applied;
	const LRTVolume::LocalBakeResult &result = pending_apply_result;
	const int finished_generation = pending_apply_generation;
	const uint32_t finished_reasons = pending_apply_reasons;
	// An unchanged field keeps the coherent light snapshot that is already on the GPU; only a
	// rebuild that actually replaced the sources has to capture again.
	const bool field_unchanged = bool(applied.get("unchanged", false));
	if (!field_unchanged) {
		// Local record edits do not invalidate the committed, non-spatial light parameters.
		if (!bool(applied.get("preserved_history", false))) {
			native_source_ready = true;
		}
		_inject_sources(false);
	}
	applied_generation = finished_generation;
	applied_rebuild_reasons = finished_reasons;
	build_apply_frame = scheduler_frame;
	applied["generation"] = finished_generation;
	applied["rebuild_reasons"] = int64_t(finished_reasons);
	applied["build_ms"] = result.build_ms;
	applied["cache_loaded"] = result.cache_loaded;
	applied["assets_ms"] = result.assets_ms;
	applied["signature_ms"] = result.signature_ms;
	applied["topology_ms"] = result.topology_ms;
	applied["cache_read_ms"] = result.cache_read_ms;
	applied["voxelize_ms"] = result.voxelize_ms;
	applied["flood_fill_ms"] = result.flood_fill_ms;
	applied["distance_ms"] = result.distance_ms;
	applied["cache_write_ms"] = result.cache_write_ms;
	applied["instance_field_ms"] = result.instance_field_ms;
	applied["local_ms"] = result.local_ms;
	applied["sdf_setup_ms"] = result.sdf_setup_ms;
	applied["sdf_sample_ms"] = result.sdf_sample_ms;
	applied["sdf_transfer_ms"] = result.sdf_transfer_ms;
	applied["sdf_merge_ms"] = result.sdf_merge_ms;
	applied["local_pack_ms"] = result.local_pack_ms;
	applied["queue_wait_ms"] = result.queue_wait_ms;
	applied["capture_decode_ms"] = result.capture_decode_ms;
	applied["worker_total_ms"] = result.worker_total_ms;
	applied["publish_delay_ms"] = result.publish_delay_ms;
	applied["geometry_input_ms"] = result.geometry_input_ms;
	applied["display_ms"] = result.display_ms;
	applied["assets_loaded"] = result.assets_loaded;
	applied["assets_baked"] = result.assets_baked;
	applied["assets_memory"] = result.assets_memory;
	applied["assets_requested"] = result.assets_requested;
	applied["assets_prepared"] = result.assets_prepared;
	applied["closed_mesh_assets"] = result.closed_mesh_assets;
	applied["open_mesh_assets"] = result.open_mesh_assets;
	applied["surface_voxels"] = result.surface_voxels;
	applied["preparation_error"] = result.preparation_error;
	applied["sdf_specs"] = result.sdf_specs;
	applied["sdf_instance_references"] = result.sdf_instance_references;
	applied["sdf_bytes"] = int64_t(result.sdf_bytes);
	applied["instance_field_bytes"] = int64_t(result.instance_field_bytes);
	applied["input_bytes"] = int64_t(result.input_bytes);
	applied["active_cpu_bytes"] = int64_t(applied.get("active_cpu_bytes", result.active_cpu_bytes));
	applied["staged_cpu_bytes"] = int64_t(applied.get("staged_cpu_bytes", result.staged_cpu_bytes));
	applied["cpu_peak_bytes"] = MAX(int64_t(applied.get("cpu_peak_bytes", 0)), int64_t(result.cpu_peak_bytes));
	applied["sdf_scratch_peak_bytes"] = int64_t(result.sdf_scratch_peak_bytes);
	applied["sdf_samples"] = int64_t(result.sdf_samples);
	applied["largest_sdf_samples"] = int64_t(result.largest_sdf_samples);
	applied["largest_sdf_triangles"] = result.largest_sdf_triangles;
	applied["longest_asset_bake_ms"] = result.longest_asset_bake_ms;
	PackedInt32Array resolutions;
	resolutions.resize(int64_t(result.sdf_resolutions.size()));
	for (size_t i = 0; i < result.sdf_resolutions.size(); i++) {
		resolutions.set(int64_t(i), result.sdf_resolutions[i]);
	}
	applied["sdf_resolutions"] = resolutions;
	applied["dirty_trunks"] = result.dirty_trunks;
	const Dictionary collection = get_collection_stats();
	applied["scene_receivers"] = collection["receivers"];
	applied["scene_contributors"] = collection["contributors"];
	build_stats = applied;
	if (finished_reasons != REBUILD_REASON_CACHE_ASSETS || !result.cache_loaded) {
		geometry_builds++;
	}
	applied["cached_assets_prepared"] = finished_reasons == REBUILD_REASON_CACHE_ASSETS && result.cache_loaded;
	if (!Engine::get_singleton()->is_editor_hint() || editor_rebuild_requested) {
		applied_volume_size = volume_size;
		applied_spacing = spacing;
		has_applied_configuration = true;
		editor_build_dirty = false;
		editor_rebuild_requested = false;
	}
	applied_build_cache_fingerprint = pending_apply_cache_fingerprint;
	if (Engine::get_singleton()->is_editor_hint() && result.cache_loaded) {
		serialized_build_cache_fingerprint = applied_build_cache_fingerprint;
		build_data_missing = false;
	}
	// Continuous edits publish only to the live field. A scene save that arrives before its
	// bake completes records the requested fingerprint; this publication fills that cache later.
	if (Engine::get_singleton()->is_editor_hint() && applied_build_cache_fingerprint != 0 &&
			!result.cache_loaded && ((finished_reasons & REBUILD_REASON_FORCED) ||
					(build_data_missing && serialized_build_cache_fingerprint == applied_build_cache_fingerprint))) {
#ifdef TOOLS_ENABLED
		const Dictionary report = prepare_export_data();
		if (bool(report.get("ok", false))) {
			serialized_build_cache_fingerprint = uint64_t(int64_t(report["fingerprint"]));
			build_data_missing = false;
		} else {
			build_data_missing = true;
			ERR_PRINT(String(report.get("error", "Cannot persist completed LRT export dependencies.")));
		}
#endif
	}
	last_build_latency_ms = active_build_queued_usec == 0 ? 0.0 :
			double(OS::get_singleton()->get_ticks_usec() - active_build_queued_usec) / 1000.0;
	// Native Forward+ lighting is captured after the offscreen shadow view has rendered. Emission,
	// sky and the previous coherent native-light fields propagate while capture is in flight.
	uint64_t finish_segment_started_usec = OS::get_singleton()->get_ticks_usec();
	if (!field_unchanged) {
		// The render thread atomically copied and patched the local/receiver banks for this frame.
		// Start the raster capture on the next frame so the two independent GPU bursts do not stack.
		deferred_receiver_unit_field_frame = scheduler_frame + 1;
		source_injections++;
	}
	applied["capture_queue_ms"] = double(OS::get_singleton()->get_ticks_usec() - finish_segment_started_usec) / 1000.0;
	finish_segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_apply_display();
	applied["display_apply_ms"] = double(OS::get_singleton()->get_ticks_usec() - finish_segment_started_usec) / 1000.0;
	display_collection_dirty = false;
	build_stats = applied;
}

// --- Display ---------------------------------------------------------------

void LRTVolume3D::_update_display_parameters() {
	// Inactive nodes release only their own state and receiver claims.
	if (!_is_active() || solver.is_null() || build_stats.is_empty()) {
		_clear_native_receiver();
		return;
	}
	const Dictionary grid = solver->get_grid();
	const Vector3i size = grid.get("size", Vector3i());
	const Vector3 grid_min = grid.get("min", Vector3());
	const double grid_spacing = grid.get("spacing", 0.25);
	const Vector2 atlas(size.x * size.z, size.y);
	const Transform3D world_to_volume = get_global_transform().affine_inverse();
	Dictionary native_state;
	native_state["owner"] = uint64_t(get_instance_id());
	native_state["world_to_volume"] = world_to_volume;
	// The scene cull instance transform does not carry an authored directional light's
	// rotation, so the Volume shadow camera is oriented from the node instead.
	const Vector3 effective_size = _effective_volume_size();
	native_state["volume_min"] = -effective_size * 0.5;
	native_state["volume_max"] = effective_size * 0.5;
	native_state["grid_min"] = grid_min;
	native_state["grid_size"] = size;
	native_state["spacing"] = grid_spacing;
	native_state["atlas_size"] = atlas;
	native_state["environment"] = environment.is_valid() ? environment->get_rid() : RID();
	native_state["blend_distance"] = blend_distance;
	native_state["priority"] = priority;
	native_state["scenario"] = get_world_3d()->get_scenario();
	PackedInt32Array tree_order;
	for (const Node *node = this; node->get_parent() != nullptr; node = node->get_parent()) {
		tree_order.insert(0, node->get_index());
	}
	native_state["tree_order"] = tree_order;
	native_state["external_gi_enabled"] = _is_external_gi_active();
	native_state["enabled"] = _is_active() && transform_valid;
	native_state["solver"] = solver;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTRenderBridge::set_state).bind(native_state));
}

void LRTVolume3D::_clear_native_receiver() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr) {
		rendering_server->call_on_render_thread(callable_mp_static(&LRTRenderBridge::clear).bind(get_instance_id()));
	}
}

// The native receiver replaces only diffuse indirect light. Viewport debug modes never mutate
// authored lights, materials, environments, or the volume's production state.
void LRTVolume3D::_apply_display() {
	_update_display_parameters();
	display_active = _is_active() && transform_valid;
}

void LRTVolume3D::_refresh_environment() {
	Ref<Environment> next_environment;
	Ref<World3D> world = get_world_3d();
	if (world.is_valid()) {
		next_environment = world->get_environment();
	}
	if (next_environment == environment) {
		return;
	}
	environment = next_environment;
	environment_cache_valid = false;
	update_configuration_warnings();
	_update_display_parameters();
}

// The environment is an LRT input like a light, but neither Environment, Sky nor the sky
// materials emit a change notification when their settings are edited, so the node compares
// a hash of the state that feeds the engine's own ambient and sky lighting.
uint64_t LRTVolume3D::_environment_key() const {
	if (environment.is_null()) {
		return 0;
	}
	uint64_t state = 0;
	const Color background = environment->get_bg_color();
	state = mix_signature(state, uint64_t(environment->get_background()));
	state = mix_signature(state, uint64_t(background.r * 100000.0));
	state = mix_signature(state, uint64_t(background.g * 100000.0));
	state = mix_signature(state, uint64_t(background.b * 100000.0));
	state = mix_signature(state, uint64_t(environment->get_bg_energy_multiplier() * 100000.0));
	state = mix_signature(state, uint64_t(environment->get_ambient_source()));
	const Color ambient = environment->get_ambient_light_color();
	state = mix_signature(state, uint64_t(ambient.r * 100000.0));
	state = mix_signature(state, uint64_t(ambient.g * 100000.0));
	state = mix_signature(state, uint64_t(ambient.b * 100000.0));
	state = mix_signature(state, uint64_t(environment->get_ambient_light_energy() * 100000.0));
	state = mix_signature(state, uint64_t(environment->get_ambient_light_sky_contribution() * 100000.0));
	Ref<Sky> sky_resource = environment->get_sky();
	if (sky_resource.is_valid()) {
		state = mix_signature(state, sky_resource->get_rid().get_id());
		state = mix_signature(state, uint64_t(sky_resource->get_radiance_size()));
		state = mix_signature(state, uint64_t(sky_resource->get_process_mode()));
		Ref<Material> material = sky_resource->get_material();
		if (material.is_valid()) {
			state = mix_signature(state, material->get_rid().get_id());
			List<PropertyInfo> properties;
			material->get_property_list(&properties);
			for (const PropertyInfo &property : properties) {
				if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
					continue;
				}
				const StringName name = property.name;
				bool base_property = false;
				for (const char *const base_property_name : BASE_RESOURCE_PROPERTIES) {
					if (name == base_property_name) {
						base_property = true;
						break;
					}
				}
				if (base_property) {
					continue;
				}
				state = mix_signature(state, uint64_t(material->get(name).hash()));
			}
		}
	}
	return state;
}

// The engine only fills a sky's radiance while a frame is being rendered, so a private
// viewport renders the environment once before its panorama is projected to SH2. The
// private World3D deliberately has no DirectionalLight3D: ProceduralSkyMaterial sun disks
// driven by scene lights cannot be duplicated with the separately captured analytic light.
void LRTVolume3D::_render_environment(const Ref<Environment> &p_environment) {
	if (sky_viewport == nullptr) {
		sky_viewport = memnew(SubViewport);
		sky_viewport->set_name("LrtSkyViewport");
		sky_viewport->set_size(Vector2i(16, 16));
		sky_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
		Ref<World3D> world;
		world.instantiate();
		sky_viewport->set_world_3d(world);
		Camera3D *camera = memnew(Camera3D);
		sky_viewport->add_child(camera);
		Node *host = is_inside_tree() ? (Node *)this : (Node *)SceneTree::get_singleton()->get_root();
		host->add_child(sky_viewport, false, Node::INTERNAL_MODE_FRONT);
	}
	sky_viewport->get_world_3d()->set_environment(p_environment);
	sky_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	sky_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
}

void LRTVolume3D::_refresh_environment_cache() {
	if (environment.is_null() || solver.is_null()) {
		{
			MutexLock lock(environment_capture_mutex);
			environment_capture_generation.fetch_add(1);
			environment_capture_ready.store(false);
			pending_environment_panorama.unref();
		}
		cached_sky_panorama.unref();
		cached_sky_radiance.clear();
		environment_cache_valid = false;
		environment_capture_pending = false;
		environment_capture_submitted = false;
		pending_environment_capture.unref();
		return;
	}
	const uint64_t key = _environment_key();
	if (environment_cache_valid && key == environment_key) {
		return;
	}
	if (!environment_capture_pending || key != pending_environment_key) {
		{
			MutexLock lock(environment_capture_mutex);
			environment_capture_generation.fetch_add(1);
			environment_capture_ready.store(false);
			pending_environment_panorama.unref();
		}
		environment_capture_submitted = false;
		environment_capture_pending = true;
		pending_environment_key = key;
		pending_environment_capture = environment;
		if (environment->get_sky().is_valid()) {
			// SkyRD bakes its brightness multiplier into the radiance octahedron, while
			// environment_bake_panorama() multiplies it once more. Render a private copy at
			// unit brightness, then let the panorama API apply the authored energy exactly once.
			pending_environment_capture = environment->duplicate();
			Ref<Sky> capture_sky = environment->get_sky()->duplicate();
			pending_environment_capture->set_sky(capture_sky);
			Ref<Environment> render_environment = pending_environment_capture->duplicate();
			render_environment->set_sky(capture_sky);
			render_environment->set_bg_energy_multiplier(1.0);
			_render_environment(render_environment);
			pending_environment_ready_frame = scheduler_frame + 2;
		} else {
			pending_environment_ready_frame = scheduler_frame;
		}
		return;
	}
	if (!environment_capture_submitted) {
		if (scheduler_frame < pending_environment_ready_frame) {
			return;
		}
		environment_capture_submitted = true;
		RenderingServer::get_singleton()->call_on_render_thread(
				callable_mp(this, &LRTVolume3D::_read_environment_panorama_render_thread)
						.bind(pending_environment_capture, environment_capture_generation.load()));
		return;
	}
	if (!environment_capture_ready.load()) {
		return;
	}
	{
		MutexLock lock(environment_capture_mutex);
		cached_sky_panorama = pending_environment_panorama;
		pending_environment_panorama.unref();
	}
	cached_sky_radiance = solver->project_panorama_radiance_sh(cached_sky_panorama, Basis());
	environment_key = pending_environment_key;
	environment_cache_valid = true;
	environment_capture_pending = false;
	environment_capture_submitted = false;
	pending_environment_capture.unref();
	if (sky_viewport != nullptr) {
		sky_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
	}
}

void LRTVolume3D::_read_environment_panorama_render_thread(const Ref<Environment> &p_environment, uint64_t p_generation) {
	const Ref<Image> panorama = solver->read_environment_panorama(
			p_environment, Vector2i(SKY_PANORAMA_WIDTH, SKY_PANORAMA_HEIGHT));
	{
		MutexLock lock(environment_capture_mutex);
		if (p_generation != environment_capture_generation.load()) {
			return;
		}
		pending_environment_panorama = panorama;
		environment_capture_ready.store(true);
	}
}

PackedVector4Array LRTVolume3D::_environment_radiance() {
	PackedVector4Array result;
	result.resize(3);
	result.fill(Vector4());
	_refresh_environment_cache();
	if (cached_sky_radiance.size() != 3) {
		return result;
	}
	// environment_bake_panorama() returns unrotated sky-space radiance, matching LightmapGI.
	// Convert it through authored sky rotation and then from world into the moving Volume.
	const Basis sky_to_volume = get_global_transform().basis.inverse() * Basis::from_euler(environment->get_sky_rotation());
	for (int channel = 0; channel < 3; channel++) {
		const Vector4 coefficient = cached_sky_radiance[channel];
		const Vector3 directional = sky_to_volume.xform(Vector3(coefficient.y, coefficient.z, coefficient.w));
		result.set(channel, Vector4(coefficient.x, directional.x, directional.y, directional.z));
	}
	return result;
}

// Replaces Source on the current local field without clearing radiance or resetting phase.
void LRTVolume3D::_inject_sources(bool p_count) {
	if (solver.is_null() || !solver->has_local_field()) {
		return;
	}
	light_inputs = _cached_mapped_lights();
	light_photometry_signature = _light_photometry_signature();
	has_light_photometry_signature = true;
	if (receive_sky) {
		sky_radiance = _environment_radiance();
		solver->set_sky_radiance(sky_radiance);
	}
	solver->inject();
	if (p_count) {
		source_injections++;
	}
	_update_display_parameters();
}

// --- Node lifecycle --------------------------------------------------------

PackedStringArray LRTVolume3D::get_volume_warnings() const {
	PackedStringArray warnings;
	if (OS::get_singleton()->get_current_rendering_method() != "forward_plus") {
		warnings.push_back(RTR("LRT requires the Forward+ renderer."));
	}
	for (const Receiver &receiver : receivers) {
		const Node3D *mesh_instance = Object::cast_to<Node3D>(ObjectDB::get_instance(receiver.instance_id));
		if (!receiver.material_error.is_empty() && mesh_instance != nullptr &&
				receiver.gi_enabled) {
			warnings.push_back(vformat(RTR("LRT material on %s: %s"), String(mesh_instance->get_path()), receiver.material_error));
		}
	}
	if (!_has_valid_volume_transform()) {
		warnings.push_back(RTR("The LRT volume cannot be scaled, including through a parent. Keep its effective scale at (1, 1, 1) and edit volume_size instead."));
	}

	return warnings;
}

// The volume box, so the editor frames (F) and frames the node the way it frames a
// ReflectionProbe or VoxelGI.
AABB LRTVolume3D::get_aabb() const {
	return AABB(-volume_size * 0.5, volume_size);
}

PackedStringArray LRTVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();
	warnings.append_array(get_volume_warnings());
	return warnings;
}

void LRTVolume3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			active_volumes[get_instance_id()] = this;
			set_process(true);
			native_source_ready = false;
			scene_candidates_dirty = true;
			SceneTree *tree = get_tree();
			const Callable candidate_callback = callable_mp(this, &LRTVolume3D::_mark_scene_candidates_dirty);
			if (tree != nullptr && !tree->is_connected("tree_changed", candidate_callback)) {
				tree->connect("tree_changed", candidate_callback);
			}
			// The build starts on the first processed frame, not here: ENTER_TREE reaches the
			// volume node before its sibling receivers, and a mesh that has not entered the
			// tree yet cannot report a world transform.
			has_geometry_signature = false;
			has_material_state_signature = false;
		} break;
		case NOTIFICATION_EXIT_TREE: {
			active_volumes.erase(get_instance_id());
			_release_resource_dependencies(true);
			_release_resource_dependencies(true, true);
			SceneTree *tree = get_tree();
			const Callable candidate_callback = callable_mp(this, &LRTVolume3D::_mark_scene_candidates_dirty);
			if (tree != nullptr && tree->is_connected("tree_changed", candidate_callback)) {
				tree->disconnect("tree_changed", candidate_callback);
			}
			geometry_candidates.clear();
			gridmap_candidates.clear();
			light_candidates.clear();
			scene_candidates_dirty = true;
			_cancel_build();
			_clear_native_receiver();
			native_capture_pending = false;
			native_capture_queued = false;
			native_capture_queued_usec = 0;
			native_light_field_set_signature = 0;
				} break;
		case NOTIFICATION_EDITOR_PRE_SAVE: {
#ifdef TOOLS_ENABLED
			if (has_applied_configuration && !editor_build_dirty) {
				_collect_geometry();
				const uint64_t fingerprint = _build_cache_fingerprint();
				serialized_export_data = Dictionary();
				const bool current_field_matches = !building && !local_apply_pending && !rebuild_pending && solver.is_valid() &&
						has_geometry_signature && geometry_signature == _geometry_signature() &&
						has_material_state_signature && material_state_signature == _material_state_signature();
				if (current_field_matches) {
					// Saving a prepared scene records all local assets, including off-volume inputs.
					// Actual export only reads this manifest and never needs a rendering device.
					const Dictionary report = prepare_export_data();
					if (bool(report.get("ok", false))) {
						serialized_build_cache_fingerprint = uint64_t(int64_t(report["fingerprint"]));
						build_data_missing = false;
					} else {
						serialized_build_cache_fingerprint = fingerprint;
						build_data_missing = true;
						ERR_PRINT(String(report.get("error", "Cannot prepare LRT export dependencies while saving the scene.")));
					}
				} else {
					serialized_build_cache_fingerprint = fingerprint;
					build_data_missing = true;
					if (!building && !local_apply_pending && !rebuild_pending && _is_active()) {
						_queue_build(REBUILD_REASON_GEOMETRY);
					}
				}
			}
#endif
		} break;
		case NOTIFICATION_EDITOR_POST_SAVE: {
			_apply_display();
		} break;
		case NOTIFICATION_PREDELETE: {
			_cancel_build();
			_clear_native_receiver();
			native_light_field_set_signature = 0;
			{
				MutexLock lock(environment_capture_mutex);
				environment_capture_generation.fetch_add(1);
			}
			if (environment_capture_submitted) {
				RenderingServer::get_singleton()->sync();
			}
			// The slice layer and the sky viewport normally hang off this node and die with
			// it; a sky viewport attached to the tree root (tests) is released here.
			if (sky_viewport != nullptr) {
				if (sky_viewport->get_parent() != this) {
					memdelete(sky_viewport);
				}
				sky_viewport = nullptr;
			}
		} break;
		case NOTIFICATION_PROCESS: {
			_refresh_frame();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			// Inherited scaling is invalid, so the warning follows the global transform live.
			update_configuration_warnings();
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			update_gizmos();
			_apply_display();
		} break;
	}
}

// One frame of the node's own logic. NOTIFICATION_PROCESS calls it every frame; [method poll]
// exposes the same work to scripts and tests, which cannot wait for editor frames.
void LRTVolume3D::_refresh_frame() {
	const uint64_t frame_started_usec = OS::get_singleton()->get_ticks_usec();
	scheduler_frame++;
	mapped_lights_cache_valid = false;
	last_collect_geometry_ms = 0.0;
	last_geometry_input_ms = 0.0;
	last_geometry_update_ms = 0.0;
	last_collect_lights_ms = 0.0;
	last_environment_ms = 0.0;
	last_geometry_signature_ms = 0.0;
	last_material_signature_ms = 0.0;
	last_shadow_signature_ms = 0.0;
	last_sky_input_ms = 0.0;
	last_build_poll_ms = 0.0;
	last_apply_begin_ms = 0.0;
	last_apply_finish_ms = 0.0;
	last_build_publish_ms = 0.0;
	last_native_input_ms = 0.0;
	last_native_resolve_poll_ms = 0.0;
	last_display_update_ms = 0.0;
	last_propagation_schedule_ms = 0.0;
	last_frame_propagation_iterations = 0;
	if (solver.is_valid()) {
		auto debug_fields_for = [](Viewport::DebugDraw p_mode) -> uint32_t {
			switch (p_mode) {
				case Viewport::DEBUG_DRAW_LRT_SOURCE_PROBES: return LRTVolume::DEBUG_SOURCE;
				case Viewport::DEBUG_DRAW_LRT_SKY_VISIBILITY: return LRTVolume::DEBUG_SKY_VISIBILITY;
				case Viewport::DEBUG_DRAW_LRT_COARSE_GEOMETRY: return LRTVolume::DEBUG_COARSE_GEOMETRY;
				case Viewport::DEBUG_DRAW_LRT_UPDATE_REGIONS: return LRTVolume::DEBUG_DIRTY;
				default: return 0;
			}
		};
		const Viewport *viewport = get_viewport();
		const Viewport::DebugDraw debug_draw = viewport != nullptr ? viewport->get_debug_draw() : Viewport::DEBUG_DRAW_DISABLED;
		uint32_t debug_fields = debug_fields_for(debug_draw);
		if (is_inside_tree() && !Engine::get_singleton()->is_editor_hint()) {
			for (Node *node : get_tree()->get_nodes_in_group("_viewports")) {
				const Viewport *view = Object::cast_to<Viewport>(node);
				if (view->find_world_3d() == get_world_3d()) {
					debug_fields |= debug_fields_for(view->get_debug_draw());
				}
			}
		}
#ifdef TOOLS_ENABLED
		Node3DEditor *editor = Node3DEditor::get_singleton();
		if (Engine::get_singleton()->is_editor_hint() && editor != nullptr) {
			for (unsigned int index = 0; index < Node3DEditor::VIEWPORTS_COUNT; index++) {
				Node3DEditorViewport *editor_view = editor->get_editor_viewport(index);
				const SubViewport *view = editor_view->get_viewport_node();
				if (!editor_view->is_visible_in_tree() || view->find_world_3d() != get_world_3d()) {
					continue;
				}
				const Viewport::DebugDraw mode = view->get_debug_draw();
				debug_fields |= debug_fields_for(mode);
			}
		}
#endif
		if (_is_active() && solver->set_debug_fields(debug_fields)) {
			display_collection_dirty = true;
		}
	}
	if (!_is_active()) {
		if (display_active) {
			_apply_display();
		}
		last_frame_work_ms = double(OS::get_singleton()->get_ticks_usec() - frame_started_usec) / 1000.0;
		peak_frame_work_ms = MAX(peak_frame_work_ms, last_frame_work_ms);
		return;
	}
	const bool valid_transform = _has_valid_volume_transform();
	if (valid_transform != transform_valid) {
		transform_valid = valid_transform;
		_apply_display();
		if (transform_valid) {
			has_geometry_signature = false;
			has_material_state_signature = false;
		}
	}
	if (!transform_valid) {
		error_message = "LRT Volume 不允许自身或父级缩放；请用 size 调整体积范围";
		last_frame_work_ms = double(OS::get_singleton()->get_ticks_usec() - frame_started_usec) / 1000.0;
		peak_frame_work_ms = MAX(peak_frame_work_ms, last_frame_work_ms);
		return;
	}
	if (error_message.begins_with("LRT Volume 不允许")) {
		error_message = String();
	}
	uint64_t segment_started_usec = OS::get_singleton()->get_ticks_usec();
	double geometry_update_ms = 0.0;
	bool loaded_editor_cache = false;
	_collect_geometry();
	last_collect_geometry_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	if (Engine::get_singleton()->is_editor_hint() && editor_build_dirty && !editor_rebuild_requested) {
		bool has_contributor = false;
		for (const Receiver &receiver : receivers) {
			if (receiver.contributes) {
				has_contributor = true;
				break;
			}
		}
		if (has_contributor) {
			loaded_editor_cache = _try_load_build_cache(_build_cache_fingerprint());
		}
	}
	if (!Engine::get_singleton()->is_editor_hint() && !has_applied_configuration &&
			last_cache_lookup_fingerprint == 0 && !building && !local_apply_pending) {
		loaded_editor_cache = _try_load_build_cache(_build_cache_fingerprint());
	}
	geometry_update_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_collect_lights();
	last_collect_lights_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_refresh_environment();
	const int provider_status = int(LRTRenderBridge::get_external_gi_status(get_instance_id()));
	if (external_gi_status != provider_status) {
		external_gi_status = provider_status;
		update_configuration_warnings();
	}
	const Transform3D current_display_transform = get_global_transform();
	if (!has_display_transform || current_display_transform != display_transform) {
		display_transform = current_display_transform;
		has_display_transform = true;
		_update_display_parameters();
	}
	last_environment_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	const uint64_t geometry_finalize_started_usec = OS::get_singleton()->get_ticks_usec();
	uint32_t rebuild_reasons = REBUILD_REASON_NONE;
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	const uint64_t next_geometry_signature = _geometry_signature();
	last_geometry_signature_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	const uint64_t next_material_signature = _material_state_signature();
	last_material_signature_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	if (!has_geometry_signature || next_geometry_signature != geometry_signature) {
		rebuild_reasons |= REBUILD_REASON_GEOMETRY;
	}
	if (!has_material_state_signature || next_material_signature != material_state_signature) {
		rebuild_reasons |= REBUILD_REASON_MATERIAL;
	}
	geometry_signature = next_geometry_signature;
	material_state_signature = next_material_signature;
	has_geometry_signature = true;
	has_material_state_signature = true;
	if (rebuild_reasons != REBUILD_REASON_NONE && !loaded_editor_cache) {
		const bool wait_for_editor_rebuild = Engine::get_singleton()->is_editor_hint() &&
				!editor_rebuild_requested && !has_applied_configuration;
		if (!wait_for_editor_rebuild) {
			_queue_build(rebuild_reasons);
		}
	}
	geometry_update_ms += double(OS::get_singleton()->get_ticks_usec() - geometry_finalize_started_usec) / 1000.0;
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_poll_build();
	last_build_poll_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	// Retire completed work before starting the newest collected snapshot. Otherwise every
	// publication leaves the worker idle until the next geometry check.
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_start_build();
	geometry_update_ms += double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	last_geometry_update_ms = geometry_update_ms;
	geometry_update_count++;
	if (solver.is_valid()) {
		// Published one frame after the request so the shadow map already reflects the newest
		// caster positions.
		solver->commit_queued_direct_resolves();
	}
	if (!local_apply_pending && error_message.is_empty() && solver.is_valid() && solver->has_local_field()) {
		const uint64_t native_input_started_usec = OS::get_singleton()->get_ticks_usec();
		if (deferred_receiver_unit_field_frame != UINT64_MAX && scheduler_frame >= deferred_receiver_unit_field_frame) {
			deferred_receiver_unit_field_frame = UINT64_MAX;
			_queue_native_light_capture(true, false);
		}
		if (native_capture_queued && !native_capture_pending) {
			_queue_native_light_capture(false, false);
		}
		uint64_t next_shadow_signature = shadow_capture_signature;
		if (shadow_signature_refresh_frame == scheduler_frame) {
			last_shadow_signature_ms = 0.0;
		} else {
			segment_started_usec = OS::get_singleton()->get_ticks_usec();
			next_shadow_signature = _shadow_inputs_signature();
			last_shadow_signature_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
		}
		if (!has_shadow_capture_signature || next_shadow_signature != shadow_capture_signature) {
			// A pending local build already owns the newest geometry/caster snapshot. Capturing the
			// intermediate authored pose would discard its prepared receiver meshes and then be
			// cancelled when that layout applies, so coalesce the light invalidation into the apply.
			if (!building && !local_apply_pending && !rebuild_pending) {
				_queue_native_light_capture();
			}
		} else {
			const uint64_t next_photometry_signature = _light_photometry_signature();
			if (!has_light_photometry_signature || next_photometry_signature != light_photometry_signature) {
			// Energy, indirect energy, color, temperature and negative-light sign are linear
			// photometric changes. Reweight the cached unit fields now; no shadow capture is needed.
				_apply_native_light_photometry(true);
			}
		}
		last_native_input_ms = double(OS::get_singleton()->get_ticks_usec() - native_input_started_usec) / 1000.0;
		segment_started_usec = OS::get_singleton()->get_ticks_usec();
		if (receive_sky) {
			const PackedVector4Array next_sky_radiance = _environment_radiance();
			last_sky_input_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
			if (next_sky_radiance != sky_radiance) {
				// Sky is independent of the raster-light capture and can refresh immediately.
				_inject_sources();
				_apply_display();
			}
		}
		segment_started_usec = OS::get_singleton()->get_ticks_usec();
		if (_complete_native_light_resolve()) {
			_apply_display();
		}
		last_native_resolve_poll_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
		segment_started_usec = OS::get_singleton()->get_ticks_usec();
		if (display_collection_dirty) {
			_update_display_parameters();
			_apply_display();
			display_collection_dirty = false;
		}
		last_display_update_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	}
	if (solver.is_valid() && solver->has_local_field() && !local_apply_pending) {
		solver->set_receive_sky(receive_sky);
		solver->advance_sky();
	}
	// Keep propagation independent of measured GPU cost. Coherent incremental uploads can
	// keep using the active bank; incomplete replacements wait until their data is ready.
	_refresh_propagation_budget();
	const int scheduled_iterations = _propagation_iterations_per_frame();
	if (scheduled_iterations > 0) {
		segment_started_usec = OS::get_singleton()->get_ticks_usec();
		if (native_capture_pending) {
			solver->step_radiance_only(scheduled_iterations);
		} else {
			solver->step(scheduled_iterations);
		}
		propagation_iterations_remaining -= scheduled_iterations;
		propagation_last_frame = Engine::get_singleton()->get_process_frames();
		last_frame_propagation_iterations = scheduled_iterations;
		_update_display_parameters();
		last_propagation_schedule_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	}
	if (!native_capture_queued && !native_capture_pending &&
			(solver.is_null() || (!solver->is_native_light_resolve_pending() && !solver->is_injection_pending()))) {
		native_capture_queued_usec = 0;
	}
	// The editor's 3D viewports keep their render target update mode at UPDATE_WHEN_VISIBLE, so
	// they repaint every visible frame and follow the field without any help from here. The
	// viewport this node lives in is EditorNode::scene_root, which is 2D-only, so changing its
	// mode would not reach what the user sees.
	last_frame_work_ms = double(OS::get_singleton()->get_ticks_usec() - frame_started_usec) / 1000.0;
	peak_frame_work_ms = MAX(peak_frame_work_ms, last_frame_work_ms);
}
