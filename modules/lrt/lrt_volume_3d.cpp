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
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

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
// Material capture meshes are shared between Volumes up to this budget.
constexpr uint64_t SHARED_MESH_CAPTURE_BUDGET_BYTES = 128ull * 1024ull * 1024ull;
// JSON-style base properties every resource has; skipped when hashing a sky material.
const char *const BASE_RESOURCE_PROPERTIES[] = {
	"resource_local_to_scene", "resource_path", "resource_name", "script"
};
const char *const DEFAULT_SDF_RESOLUTION_SETTING = "rendering/global_illumination/lrt/sdf/default_resolution";
const char *const INSTANCE_SDF_RESOLUTION_META = "lrt_sdf_resolution";

Light3D *light_from_id(ObjectID p_id) {
	return Object::cast_to<Light3D>(ObjectDB::get_instance(p_id));
}

GeometryInstance3D *geometry_from_id(ObjectID p_id) {
	return Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(p_id));
}

bool is_instanced_geometry(GeometryInstance3D *p_instance) {
	return Object::cast_to<MultiMeshInstance3D>(p_instance) != nullptr || Object::cast_to<CPUParticles3D>(p_instance) != nullptr;
}

Ref<Mesh> geometry_mesh(GeometryInstance3D *p_instance) {
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
		if (multimesh.is_valid() && multimesh->get_instance_count() > 0 && multimesh->get_visible_instance_count() != 0) {
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

} // namespace

static uint64_t mix_signature(uint64_t p_hash, uint64_t p_value);
static uint64_t mesh_capture_bytes(const std::shared_ptr<const std::vector<lrt::MeshTriangle>> &p_triangles,
		const std::shared_ptr<const lrt::MaterialCapture> &p_material, const std::shared_ptr<const std::vector<lrt::MeshCopy>> &p_copies,
		const std::shared_ptr<const lrt::RasterGeometryCapture> &p_raster,
		const std::shared_ptr<const std::vector<lrt::MeshTriangleSkin>> &p_particle_skin, std::set<const void *> *p_counted = nullptr);


std::map<uint64_t, LRTVolume3D::MeshCaptureCache> LRTVolume3D::shared_mesh_capture_cache;
uint64_t LRTVolume3D::shared_mesh_capture_cache_bytes = 0;
uint64_t LRTVolume3D::shared_mesh_capture_cache_clock = 0;
std::map<ObjectID, LRTVolume3D *> LRTVolume3D::propagation_volumes;
uint64_t LRTVolume3D::propagation_budget_frame = 0;
uint64_t LRTVolume3D::propagation_allocated_frame = UINT64_MAX;
uint64_t LRTVolume3D::propagation_budget_round = 0;
std::set<ObjectID> LRTVolume3D::propagation_polled_volumes;
double LRTVolume3D::propagation_frame_estimated_ms = 0.0;
int LRTVolume3D::propagation_frame_participants = 0;
int LRTVolume3D::propagation_frame_iterations = 0;
bool LRTVolume3D::propagation_frame_calibration = false;
uint64_t LRTVolume3D::geometry_budget_frame = UINT64_MAX;
double LRTVolume3D::geometry_frame_work_ms = 0.0;
uint64_t LRTVolume3D::geometry_budget_round = 0;
ObjectID LRTVolume3D::geometry_priority_volume;
bool LRTVolume3D::geometry_priority_served = false;


LRTVolume3D::LRTVolume3D() {
	set_process(false);
}

LRTVolume3D::~LRTVolume3D() {
	_cancel_build();
}

void LRTVolume3D::_bind_methods() {
	ClassDB::bind_static_method("LRTVolume3D", D_METHOD("prepare_mesh_sdf", "mesh", "resolution"), &LRTVolume3D::prepare_mesh_sdf, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LRTVolume3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LRTVolume3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &LRTVolume3D::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &LRTVolume3D::get_spacing);
	ClassDB::bind_method(D_METHOD("set_volume_size", "size"), &LRTVolume3D::set_volume_size);
	ClassDB::bind_method(D_METHOD("get_volume_size"), &LRTVolume3D::get_volume_size);
	ClassDB::bind_method(D_METHOD("set_geometry_backend", "backend"), &LRTVolume3D::set_geometry_backend);
	ClassDB::bind_method(D_METHOD("get_geometry_backend"), &LRTVolume3D::get_geometry_backend);
	ClassDB::bind_method(D_METHOD("set_visibility_mode", "mode"), &LRTVolume3D::set_visibility_mode);
	ClassDB::bind_method(D_METHOD("get_visibility_mode"), &LRTVolume3D::get_visibility_mode);
	ClassDB::bind_method(D_METHOD("set_mesh_sdf_resolution", "resolution"), &LRTVolume3D::set_mesh_sdf_resolution);
	ClassDB::bind_method(D_METHOD("get_mesh_sdf_resolution"), &LRTVolume3D::get_mesh_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_instance_sdf_resolution", "instance", "resolution"), &LRTVolume3D::set_instance_sdf_resolution);
	ClassDB::bind_method(D_METHOD("get_instance_sdf_resolution", "instance"), &LRTVolume3D::get_instance_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_multi_bounce", "enabled"), &LRTVolume3D::set_multi_bounce);
	ClassDB::bind_method(D_METHOD("is_multi_bounce"), &LRTVolume3D::is_multi_bounce);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &LRTVolume3D::set_paused);
	ClassDB::bind_method(D_METHOD("is_paused"), &LRTVolume3D::is_paused);
	ClassDB::bind_method(D_METHOD("set_iterations_per_frame", "iterations"), &LRTVolume3D::set_iterations_per_frame);
	ClassDB::bind_method(D_METHOD("get_iterations_per_frame"), &LRTVolume3D::get_iterations_per_frame);
	ClassDB::bind_method(D_METHOD("set_propagation_sampling", "sampling"), &LRTVolume3D::set_propagation_sampling);
	ClassDB::bind_method(D_METHOD("get_propagation_sampling"), &LRTVolume3D::get_propagation_sampling);
	ClassDB::bind_method(D_METHOD("set_blur_sampling", "enabled"), &LRTVolume3D::set_blur_sampling);
	ClassDB::bind_method(D_METHOD("is_blur_sampling"), &LRTVolume3D::is_blur_sampling);
	ClassDB::bind_method(D_METHOD("set_editor_preview", "enabled"), &LRTVolume3D::set_editor_preview);
	ClassDB::bind_method(D_METHOD("is_editor_preview"), &LRTVolume3D::is_editor_preview);
	ClassDB::bind_method(D_METHOD("set_external_gi_enabled", "enabled"), &LRTVolume3D::set_external_gi_enabled);
	ClassDB::bind_method(D_METHOD("is_external_gi_enabled"), &LRTVolume3D::is_external_gi_enabled);
	ClassDB::bind_method(D_METHOD("set_display_blend_enabled", "enabled"), &LRTVolume3D::set_display_blend_enabled);
	ClassDB::bind_method(D_METHOD("is_display_blend_enabled"), &LRTVolume3D::is_display_blend_enabled);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &LRTVolume3D::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &LRTVolume3D::get_priority);
	ClassDB::bind_method(D_METHOD("set_blend_distance", "distance"), &LRTVolume3D::set_blend_distance);
	ClassDB::bind_method(D_METHOD("get_blend_distance"), &LRTVolume3D::get_blend_distance);
	ClassDB::bind_method(D_METHOD("set_build_cache_fingerprint", "fingerprint"), &LRTVolume3D::set_build_cache_fingerprint);
	ClassDB::bind_method(D_METHOD("get_build_cache_fingerprint"), &LRTVolume3D::get_build_cache_fingerprint);

	ClassDB::bind_method(D_METHOD("rebuild"), &LRTVolume3D::rebuild);
	ClassDB::bind_method(D_METHOD("poll"), &LRTVolume3D::poll);
	ClassDB::bind_method(D_METHOD("set_rebuild_suppressed", "suppressed"), &LRTVolume3D::set_rebuild_suppressed);
	ClassDB::bind_method(D_METHOD("is_rebuild_suppressed"), &LRTVolume3D::is_rebuild_suppressed);
	ClassDB::bind_method(D_METHOD("get_volume_warnings"), &LRTVolume3D::get_volume_warnings);
	ClassDB::bind_method(D_METHOD("step", "iterations"), &LRTVolume3D::step, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("step_update"), &LRTVolume3D::step_update);
	ClassDB::bind_method(D_METHOD("reset_field"), &LRTVolume3D::reset_field);
	ClassDB::bind_method(D_METHOD("get_editor_build_state"), &LRTVolume3D::get_editor_build_state);
	ClassDB::bind_method(D_METHOD("get_editor_build_tooltip"), &LRTVolume3D::get_editor_build_tooltip);
	ClassDB::bind_method(D_METHOD("get_instance_sdf_status", "instance"), &LRTVolume3D::get_instance_sdf_status);
	ClassDB::bind_method(D_METHOD("get_instance_sdf_message", "instance"), &LRTVolume3D::get_instance_sdf_message);
	ClassDB::bind_method(D_METHOD("is_building"), &LRTVolume3D::is_building);
	ClassDB::bind_method(D_METHOD("get_error_message"), &LRTVolume3D::get_error_message);
	ClassDB::bind_method(D_METHOD("get_build_stats"), &LRTVolume3D::get_build_stats);
	ClassDB::bind_method(D_METHOD("get_preparation_status"), &LRTVolume3D::get_preparation_status);
	ClassDB::bind_method(D_METHOD("read_volume_shadow_stats"), &LRTVolume3D::read_volume_shadow_stats);
	ClassDB::bind_method(D_METHOD("get_collection_stats"), &LRTVolume3D::get_collection_stats);
	ClassDB::bind_method(D_METHOD("get_geometry_builds"), &LRTVolume3D::get_geometry_builds);
	ClassDB::bind_method(D_METHOD("get_source_injections"), &LRTVolume3D::get_source_injections);
	ClassDB::bind_method(D_METHOD("get_dropped_builds"), &LRTVolume3D::get_dropped_builds);
	ClassDB::bind_method(D_METHOD("get_cancelled_builds"), &LRTVolume3D::get_cancelled_builds);
	ClassDB::bind_method(D_METHOD("get_solver"), &LRTVolume3D::get_solver);
	ClassDB::bind_method(D_METHOD("get_sky_radiance"), &LRTVolume3D::get_sky_radiance);
	ClassDB::bind_static_method("LRTVolume3D", D_METHOD("clear_shared_mesh_capture_cache"), &LRTVolume3D::clear_shared_mesh_capture_cache);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_volume_size", "get_volume_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.05,2.0,0.01,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_GROUP("Boundary", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blend_distance", PROPERTY_HINT_RANGE, "0,100,0.01,suffix:m"), "set_blend_distance", "get_blend_distance");
	ADD_GROUP("Advanced", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview"), "set_editor_preview", "is_editor_preview");

	// Script-only controls retained for algorithm comparison and automated regression tests.
	// They are deliberately absent from the production inspector.
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_volume_size", "get_volume_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "geometry_backend", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_geometry_backend", "get_geometry_backend");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_visibility_mode", "get_visibility_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_sdf_resolution", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_mesh_sdf_resolution", "get_mesh_sdf_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "multi_bounce", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_multi_bounce", "is_multi_bounce");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_paused", "is_paused");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "iterations_per_frame", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_iterations_per_frame", "get_iterations_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blur_sampling", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_blur_sampling", "is_blur_sampling");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_sampling", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_propagation_sampling", "get_propagation_sampling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "external_gi_enabled"), "set_external_gi_enabled", "is_external_gi_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "display_blend_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_display_blend_enabled", "is_display_blend_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "build_cache_fingerprint", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_build_cache_fingerprint", "get_build_cache_fingerprint");

	BIND_ENUM_CONSTANT(BACKEND_SDF);
	BIND_ENUM_CONSTANT(BACKEND_ANALYTIC);
	BIND_ENUM_CONSTANT(VISIBILITY_SH);
	BIND_ENUM_CONSTANT(VISIBILITY_MASK);
	BIND_ENUM_CONSTANT(PROPAGATION_FULL_26);
	BIND_ENUM_CONSTANT(PROPAGATION_FOUR_POINT_DITHERED);
}

// --- Configuration ---------------------------------------------------------

void LRTVolume3D::set_enabled(bool p_enabled) {
	set_visible(p_enabled);
	update_gizmos();
	_apply_display();
}

bool LRTVolume3D::is_enabled() const {
	return is_visible();
}

void LRTVolume3D::set_spacing(double p_spacing) {
	p_spacing = MAX(0.05, p_spacing);
	if (spacing == p_spacing) {
		return;
	}
	spacing = p_spacing;
	// The probe lattice is drawn from `spacing`, so the box has to be repainted on its own.
	update_gizmos();
	if (Engine::get_singleton()->is_editor_hint()) {
		editor_build_dirty = true;
	} else {
		_request_rebuild();
	}
}

double LRTVolume3D::get_spacing() const {
	return spacing;
}

void LRTVolume3D::set_volume_size(const Vector3 &p_size) {
	const Vector3 clamped = p_size.max(Vector3(0.05, 0.05, 0.05));
	if (volume_size == clamped) {
		return;
	}
	volume_size = clamped;
	set_blend_distance(blend_distance);
	update_gizmos();
	if (Engine::get_singleton()->is_editor_hint()) {
		editor_build_dirty = true;
	} else {
		_request_rebuild();
	}
}

Vector3 LRTVolume3D::get_volume_size() const {
	return volume_size;
}

void LRTVolume3D::set_geometry_backend(int p_backend) {
	if (geometry_backend == p_backend) {
		return;
	}
	geometry_backend = p_backend;
	_request_rebuild();
}

int LRTVolume3D::get_geometry_backend() const {
	return geometry_backend;
}

void LRTVolume3D::set_visibility_mode(int p_mode) {
	if (visibility_mode == p_mode) {
		return;
	}
	visibility_mode = p_mode;
	if (solver.is_valid() && !build_stats.is_empty()) {
		solver->set_sh_visibility(visibility_mode == VISIBILITY_SH);
		// The visibility mode changes the propagation operator itself, which is the one input
		// the prototype clears the field for (propagationDirty).
		_inject_sources(true);
	}
}

int LRTVolume3D::get_visibility_mode() const {
	return visibility_mode;
}

void LRTVolume3D::set_mesh_sdf_resolution(int p_resolution) {
	const int clamped = p_resolution <= 0 ? 0 : MAX(8, p_resolution);
	if (mesh_sdf_resolution == clamped) {
		return;
	}
	mesh_sdf_resolution = clamped;
	_request_rebuild();
}

int LRTVolume3D::get_mesh_sdf_resolution() const {
	return mesh_sdf_resolution;
}

void LRTVolume3D::set_instance_sdf_resolution(Node3D *p_instance, int p_resolution) {
	ERR_FAIL_NULL(p_instance);
	if (p_resolution <= 0) {
		p_instance->remove_meta(INSTANCE_SDF_RESOLUTION_META);
	} else {
		p_instance->set_meta(INSTANCE_SDF_RESOLUTION_META, MAX(8, p_resolution));
	}
	_request_rebuild(REBUILD_REASON_GEOMETRY);
}

int LRTVolume3D::get_instance_sdf_resolution(Node3D *p_instance) const {
	ERR_FAIL_NULL_V(p_instance, 0);
	return int(p_instance->get_meta(INSTANCE_SDF_RESOLUTION_META, 0));
}

void LRTVolume3D::set_multi_bounce(bool p_enabled) {
	if (multi_bounce == p_enabled) {
		return;
	}
	multi_bounce = p_enabled;
	if (solver.is_valid()) {
		solver->set_multi_bounce(multi_bounce);
		// A plain uniform of the propagation pass: the field keeps its history, only the source
		// term is refreshed.
		_inject_sources(false);
	}
}

bool LRTVolume3D::is_multi_bounce() const {
	return multi_bounce;
}

void LRTVolume3D::set_paused(bool p_paused) {
	paused = p_paused;
	if (paused) {
		propagation_budget_credit_ms = 0.0;
		propagation_budget_share_ms = 0.0;
		propagation_granted_iterations = 0;
	}
}

bool LRTVolume3D::is_paused() const {
	return paused;
}

void LRTVolume3D::set_iterations_per_frame(int p_iterations) {
	iterations_per_frame = MAX(0, p_iterations);
}

int LRTVolume3D::get_iterations_per_frame() const {
	return iterations_per_frame;
}

void LRTVolume3D::set_propagation_sampling(int p_sampling) {
	const int clamped = CLAMP(p_sampling, int(PROPAGATION_FULL_26), int(PROPAGATION_FOUR_POINT_DITHERED));
	if (propagation_sampling == clamped) {
		return;
	}
	propagation_sampling = clamped;
	if (solver.is_valid() && solver->has_local_field()) {
		solver->set_propagation_sampling(propagation_sampling);
		solver->reset();
	}
}

int LRTVolume3D::get_propagation_sampling() const {
	return propagation_sampling;
}

void LRTVolume3D::set_blur_sampling(bool p_enabled) {
	blur_sampling = p_enabled;
	_update_display_parameters();
}

bool LRTVolume3D::is_blur_sampling() const {
	return blur_sampling;
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

void LRTVolume3D::set_external_gi_enabled(bool p_enabled) {
	if (external_gi_enabled == p_enabled) {
		return;
	}
	external_gi_enabled = p_enabled;
	_update_display_parameters();
}

bool LRTVolume3D::is_external_gi_enabled() const {
	return external_gi_enabled;
}

void LRTVolume3D::set_display_blend_enabled(bool p_enabled) {
	set_blend_distance(p_enabled ? (blend_distance > 0.0 ? blend_distance : 0.5) : 0.0);
}

bool LRTVolume3D::is_display_blend_enabled() const {
	return blend_distance > 0.0;
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

// --- Public operations -----------------------------------------------------

// Configuration edits use the same coalescing queue as scene edits. A running build is allowed
// to finish and apply before the newest snapshot starts; generations therefore only move
// forward, while continuous motion can never cancel every build before it becomes visible.
void LRTVolume3D::_request_rebuild(uint32_t p_reasons) {
	if (Engine::get_singleton()->is_editor_hint() && (p_reasons & REBUILD_REASON_CONFIGURATION)) {
		editor_build_dirty = true;
		return;
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
// and, when unpaused, one propagation round. Scripts, tests and headless tools use it to
// drive the volume without waiting for frames.
void LRTVolume3D::poll() {
	_refresh_frame();
}

void LRTVolume3D::step(int p_iterations) {
	paused = true;
	if (solver.is_null() || build_stats.is_empty()) {
		return;
	}
	solver->step(MAX(1, p_iterations));
	_update_display_parameters();
}

void LRTVolume3D::step_update() {
	const bool was_paused = paused;
	paused = false;
	_refresh_frame();
	paused = true;
	if (!was_paused) {
		// The editor command is explicitly a one-shot operation and always leaves updates paused.
		_update_display_parameters();
	}
}

void LRTVolume3D::reset_field() {
	paused = true;
	if (solver.is_null() || build_stats.is_empty()) {
		return;
	}
	solver->reset();
	_update_display_parameters();
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
	const Vector3i probe_grid(
			MAX(1, int(Math::ceil(volume_size.x / spacing)) + 1),
			MAX(1, int(Math::ceil(volume_size.y / spacing)) + 1),
			MAX(1, int(Math::ceil(volume_size.z / spacing)) + 1));
	const int64_t probe_count = int64_t(probe_grid.x) * probe_grid.y * probe_grid.z;
	const double estimated_mib = double(probe_count * 4096ll) / (1024.0 * 1024.0);
	const Dictionary collection = get_collection_stats();
	String text = vformat("State: %s\nProbe Grid: %d × %d × %d (%d probes)\nEstimated GPU Memory: %.1f MiB\nSDF Contributors: %d",
			get_editor_build_state(), probe_grid.x, probe_grid.y, probe_grid.z, probe_count, estimated_mib,
			int(collection.get("contributors", 0)));
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

Dictionary LRTVolume3D::read_volume_shadow_stats() {
	Dictionary result;
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server == nullptr) {
		return result;
	}
	rendering_server->call_on_render_thread(callable_mp_static(&LRTRenderBridge::read_volume_shadow_depth).bind(get_instance_id()));
	rendering_server->sync();
	const LRTRenderBridge::VolumeShadowStats stats = LRTRenderBridge::get_last_volume_shadow_stats();
	result["valid"] = stats.valid;
	result["width"] = int64_t(stats.width);
	result["height"] = int64_t(stats.height);
	result["min_value"] = stats.min_value;
	result["max_value"] = stats.max_value;
	result["center_value"] = stats.center_value;
	result["written_pixels"] = int64_t(stats.written_pixels);
	result["min_x"] = int64_t(stats.min_x);
	result["min_y"] = int64_t(stats.min_y);
	result["max_x"] = int64_t(stats.max_x);
	result["max_y"] = int64_t(stats.max_y);
	return result;
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
	status["native_light_buffer_state"] = solver.is_valid() ? solver->get_native_light_buffer_state() : PackedInt32Array();
	status["geometry_candidate_count"] = int64_t(geometry_candidates.size());
	status["light_candidate_count"] = int64_t(light_candidates.size());
	status["native_shadow_signature"] = int64_t(shadow_capture_signature);
	status["update_budget_ms"] = double(GLOBAL_GET("rendering/global_illumination/lrt/propagation/update_budget_ms"));
	status["geometry_cpu_budget_ms"] = double(GLOBAL_GET("rendering/global_illumination/lrt/dynamic_objects/cpu_budget_ms"));
	status["geometry_budget_overrun_ms"] = MAX(0.0, geometry_frame_work_ms - double(GLOBAL_GET("rendering/global_illumination/lrt/dynamic_objects/cpu_budget_ms")));
	status["global_geometry_frame_work_ms"] = geometry_frame_work_ms;
	status["geometry_update_count"] = int64_t(geometry_update_count);
	status["geometry_budget_deferred_frames"] = int64_t(geometry_budget_deferred_frames);
	status["global_propagation_budget_ms"] = double(GLOBAL_GET("rendering/global_illumination/lrt/propagation/update_budget_ms"));
	status["propagation_budget_share_ms"] = propagation_budget_share_ms;
	status["propagation_budget_credit_ms"] = propagation_budget_credit_ms;
	status["propagation_budget_participants"] = propagation_frame_participants;
	status["propagation_frame_estimated_ms"] = propagation_frame_estimated_ms;
	status["propagation_frame_iterations"] = propagation_frame_iterations;
	status["propagation_frame_calibration"] = propagation_frame_calibration;

	status["propagation_response_frames"] = int(GLOBAL_GET("rendering/global_illumination/lrt/propagation/response_frames"));
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
	status["build_latency_ms"] = last_build_latency_ms;
	status["build_start_frame"] = int64_t(build_start_frame);
	status["build_done_frame"] = int64_t(build_done_frame);
	status["build_apply_frame"] = int64_t(build_apply_frame);
	status["propagation_sampling"] = propagation_sampling;
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
	status["external_gi_path"] = "diffuse_gi_provider_boundary_sh2";
	status["external_gi_writeback"] = false;
	status["external_gi_trace_queries"] = 0;
	status["render_bridge_performance"] = LRTRenderBridge::get_performance_stats(get_instance_id());
	uint64_t active_mesh_capture_bytes = 0;
	std::set<const void *> active_mesh_capture_allocations;
	for (const auto &entry : mesh_capture_cache) {
		const MeshCaptureCache &capture = entry.second;
		active_mesh_capture_bytes += mesh_capture_bytes(capture.triangles, capture.material, capture.copies,
				capture.raster_capture, capture.particle_skin, &active_mesh_capture_allocations);
	}
	status["mesh_capture_active_bytes"] = int64_t(active_mesh_capture_bytes);
	status["mesh_capture_shared_cache_bytes"] = int64_t(shared_mesh_capture_cache_bytes);
	status["mesh_capture_shared_cache_budget_bytes"] = int64_t(SHARED_MESH_CAPTURE_BUDGET_BYTES);
	status["mesh_capture_shared_cache_entries"] = int64_t(shared_mesh_capture_cache.size());
	status["particle_buffer_async_readbacks"] = int64_t(particle_buffer_async_readbacks);
	status["particle_buffer_async_readback_reuses"] = int64_t(particle_buffer_async_readback_reuses);
	status["particle_buffer_async_readback_bytes"] = int64_t(particle_buffer_async_readback_bytes);
	status["display_blend_enabled"] = blend_distance > 0.0;
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
	result["external_gi_path"] = "diffuse_gi_provider_boundary_sh2";
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
	return is_visible_in_tree() && transform_valid && external_gi_enabled;
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

int LRTVolume3D::_convergence_iterations() const {
	const int frames = CLAMP(int(GLOBAL_GET("rendering/global_illumination/lrt/propagation/response_frames")), 6, 32);
	return CLAMP(int(Math::ceil(36.0 / frames)), 1, 8);
}

// Credit carries fractional work across frames. Rotation prevents tree-order starvation.
// One indivisible iteration may exceed the budget, but only as the sole grant.
void LRTVolume3D::_begin_propagation_frame() {
	propagation_budget_frame++;
	propagation_polled_volumes.clear();
}

// Rotate first admission independently of scene-tree order. An indivisible snapshot can
// overrun this frame, but cold-start compilation must not stall later updates for seconds.
bool LRTVolume3D::_take_geometry_budget() {
	const double budget_ms = MAX(0.01, double(GLOBAL_GET("rendering/global_illumination/lrt/dynamic_objects/cpu_budget_ms")));
	if (geometry_budget_frame != propagation_budget_frame) {
		geometry_budget_frame = propagation_budget_frame;
		geometry_frame_work_ms = 0.0;
		geometry_priority_served = false;
		std::vector<ObjectID> candidates;
		const int interval = CLAMP(int(GLOBAL_GET("rendering/global_illumination/lrt/dynamic_objects/update_interval")), 1, 8);
		for (const auto &entry : propagation_volumes) {
			LRTVolume3D *volume = entry.second;
			if (!volume->_is_active() || !volume->_has_valid_volume_transform() ||
					(volume != this && (!volume->is_processing() || !volume->can_process()))) {
				continue;
			}
			const uint64_t frame = volume->scheduler_frame + (propagation_polled_volumes.count(entry.first) == 0 ? 1 : 0);
			if (!volume->has_geometry_signature || frame % uint64_t(interval) == 0 ||
					(volume->rebuild_pending && !volume->building && !volume->local_apply_pending && !volume->rebuild_suppressed)) {
				candidates.push_back(entry.first);
			}
		}
		geometry_priority_volume = candidates.empty() ? get_instance_id() : candidates[geometry_budget_round++ % candidates.size()];
	}
	if (geometry_frame_work_ms >= budget_ms || (!geometry_priority_served && geometry_priority_volume != get_instance_id())) {
		geometry_budget_deferred_frames++;
		return false;
	}
	geometry_priority_served = true;
	return true;
}

void LRTVolume3D::_finish_geometry_update(double p_work_ms) {
	geometry_frame_work_ms += p_work_ms;
	last_geometry_update_ms = p_work_ms;
	geometry_update_count++;
}

int LRTVolume3D::_take_propagation_budget() {
	if (propagation_allocated_frame != propagation_budget_frame) {
		propagation_allocated_frame = propagation_budget_frame;
		propagation_frame_estimated_ms = 0.0;
		propagation_frame_iterations = 0;
		propagation_frame_calibration = false;
		std::vector<LRTVolume3D *> candidates;
		for (const auto &entry : propagation_volumes) {
			LRTVolume3D *volume = entry.second;
			volume->propagation_granted_iterations = 0;
			volume->propagation_budget_share_ms = 0.0;
			if (!volume->_is_active() || volume->paused || !volume->transform_valid ||
					!volume->error_message.is_empty() || volume->solver.is_null() || !volume->solver->has_local_field() ||
					(volume->local_apply_pending && !volume->solver->can_step_while_applying())) {
				volume->propagation_budget_credit_ms = 0.0;
				continue;
			}
			volume->solver->request_scheduler_feedback();
			if (volume->solver->get_pending_step_iterations() == 0) {
				candidates.push_back(volume);
			}
		}
		propagation_frame_participants = int(candidates.size());
		if (candidates.empty()) {
			return 0;
		}
		const double budget_ms = MAX(0.01, double(GLOBAL_GET("rendering/global_illumination/lrt/propagation/update_budget_ms")));
		for (LRTVolume3D *volume : candidates) {
			volume->propagation_budget_share_ms = budget_ms / candidates.size();
			volume->propagation_budget_credit_ms += volume->propagation_budget_share_ms;
		}
		const size_t start = propagation_budget_round++ % candidates.size();
		for (size_t offset = 0; offset < candidates.size(); offset++) {
			LRTVolume3D *volume = candidates[(start + offset) % candidates.size()];
			const double measured_ms = volume->solver->get_scheduler_gpu_ms();
			const int measured_iterations = volume->solver->get_scheduler_gpu_work_items();
			if (measured_ms <= 0.0 || measured_iterations <= 0) {
				// Only one unknown-cost calibration pass is admitted in a frame.
				if (propagation_frame_estimated_ms == 0.0) {
					volume->propagation_granted_iterations = 1;
					propagation_frame_iterations = 1;
					propagation_frame_calibration = true;
					volume->propagation_budget_credit_ms = 0.0;
					propagation_frame_estimated_ms = budget_ms;
				}
				continue;
			}
			const double iteration_ms = measured_ms / measured_iterations;
			const int max_iterations = volume->_convergence_iterations();
			volume->propagation_budget_credit_ms = MIN(volume->propagation_budget_credit_ms,
					MAX(budget_ms, iteration_ms * max_iterations));
			const double available_ms = MIN(volume->propagation_budget_credit_ms,
					MAX(0.0, budget_ms - propagation_frame_estimated_ms));
			int iterations = MIN(max_iterations, int(available_ms / iteration_ms));
			if (iterations == 0 && propagation_frame_estimated_ms == 0.0 &&
					volume->propagation_budget_credit_ms >= iteration_ms && iteration_ms > budget_ms) {
				iterations = 1;
			}
			volume->propagation_granted_iterations = iterations;
			volume->propagation_budget_credit_ms -= iterations * iteration_ms;
			propagation_frame_estimated_ms += iterations * iteration_ms;
			propagation_frame_iterations += iterations;
		}
	}
	const int iterations = propagation_granted_iterations;
	propagation_granted_iterations = 0;
	return iterations;
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

static AABB geometry_capture_bounds(GeometryInstance3D *p_instance, AABB p_bounds) {
	if (p_instance != nullptr) {
		if (p_instance->get_custom_aabb() != AABB()) {
			p_bounds = p_instance->get_custom_aabb();
		}
		p_bounds = p_bounds.grow(p_instance->get_extra_cull_margin());
	}
	return p_bounds;
}

static uint64_t geometry_capture_bounds_signature(ObjectID p_instance_id) {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(p_instance_id));
	return instance ? mix_signature(Variant(instance->get_custom_aabb()).hash(), Variant(instance->get_extra_cull_margin()).hash()) : 0;
}

static bool material_pass_contributes(const Ref<Material> &p_material);
static bool material_chain_contributes(const Ref<Material> &p_material);
static bool material_uses_time(const Ref<Material> &p_material);
static bool material_uses_view(const Ref<Material> &p_material);

AABB LRTVolume3D::_material_capture_bounds(const Receiver &p_receiver, const AABB &p_bounds) const {
	// Instanced draws use their aggregate visibility bounds. A single mesh can follow
	// the standard material's vertex transform exactly, including camera-facing passes.
	if (p_receiver.multimesh.is_valid() || p_receiver.particles) {
		return p_bounds;
	}
	const Transform3D model = p_receiver.global_transform;
	const Transform3D to_local = model.affine_inverse();
	const Transform3D camera = material_capture_view.get("transform", Transform3D());
	const Projection projection = material_capture_view.get("projection", Projection());
	AABB result;
	bool has_bounds = false;
	auto append_pass = [&](const Ref<Material> &p_material) {
		if (!material_pass_contributes(p_material)) {
			return;
		}
		AABB bounds = p_bounds;
		Transform3D rendered = model;
		const Ref<BaseMaterial3D> standard = p_material;
		if (standard.is_valid() && standard->is_grow_enabled()) {
			bounds = bounds.grow(Math::abs(standard->get_grow()));
		}
		if (standard.is_valid() && !material_capture_view.is_empty()) {
			const auto billboard = standard->get_billboard_mode();
			if (billboard != BaseMaterial3D::BILLBOARD_DISABLED) {
				rendered.basis = camera.basis;
				if (billboard == BaseMaterial3D::BILLBOARD_FIXED_Y) {
					const Vector3 up(0, 1, 0);
					rendered.basis.set_columns(up.cross(camera.basis.get_column(2)).normalized(), up,
							camera.basis.get_column(0).cross(up).normalized());
				}
				if (standard->get_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE)) {
					rendered.basis = rendered.basis.scaled_local(model.basis.get_scale_abs());
				}
			}
			if (standard->get_flag(BaseMaterial3D::FLAG_FIXED_SIZE)) {
				const real_t scale = projection[3][3] != 0.0 ? Math::abs(1.0 / projection[1][1]) :
						-camera.affine_inverse().xform(model.origin).z;
				rendered.basis = rendered.basis.scaled_local(Vector3(scale, scale, scale));
			}
		}
		bounds = (to_local * rendered).xform(bounds);
		result = has_bounds ? result.merge(bounds) : bounds;
		has_bounds = true;
	};
	auto append_chain = [&](const Ref<Material> &p_material) {
		append_pass(p_material);
		for (Ref<Material> material = p_material.is_valid() ? p_material->get_next_pass() : Ref<Material>(); material.is_valid(); material = material->get_next_pass()) {
			append_pass(material);
		}
	};
	for (const Ref<Material> &material : p_receiver.materials) {
		append_chain(material);
	}
	if (p_receiver.authored_overlay.is_valid()) {
		append_chain(p_receiver.authored_overlay);
	}
	return has_bounds ? result : p_bounds;
}

bool LRTVolume3D::_intersects_volume(GeometryInstance3D *p_instance) const {
	if (p_instance == nullptr) {
		return false;
	}
	const Ref<Mesh> mesh = geometry_mesh(p_instance);
	if (mesh.is_null()) {
		return false;
	}
	const Transform3D to_volume = relative_node_transform(this, p_instance);
	AABB mesh_bounds = is_instanced_geometry(p_instance) ? RS::get_singleton()->multimesh_get_aabb(p_instance->get_base()) : mesh->get_aabb();
	if (is_instanced_geometry(p_instance) && mesh_bounds.get_longest_axis_size() == 0.0) {
		return false;
	}
	MeshInstance3D *deformed_instance = Object::cast_to<MeshInstance3D>(p_instance);
	if (deformed_instance != nullptr && (deformed_instance->get_skin_reference().is_valid() || deformed_instance->get_blend_shape_count() > 0)) {
		const ObjectID mesh_id = mesh->get_instance_id();
		const auto dependency = resource_dependencies.find(mesh_id);
		const uint64_t deformation = lrt::mesh_deformation_signature(deformed_instance);
		auto cached = pose_bounds_cache.find(p_instance->get_instance_id());
		if (dependency != resource_dependencies.end() && cached != pose_bounds_cache.end() &&
				cached->second.mesh_id == mesh_id && cached->second.mesh_revision == dependency->second.revision &&
				cached->second.deformation_signature == deformation) {
			cached->second.used = true;
			mesh_bounds = cached->second.bounds;
		} else {
			std::vector<lrt::MeshTriangle> triangles;
			if (!lrt::mesh_triangles(mesh, triangles, deformed_instance)) {
				return false;
			}
			const lrt::Vec3 &first = triangles.front().position[0];
			mesh_bounds = AABB(Vector3(first.x, first.y, first.z), Vector3());
			for (const lrt::MeshTriangle &triangle : triangles) {
				for (const lrt::Vec3 &vertex : triangle.position) {
					mesh_bounds.expand_to(Vector3(vertex.x, vertex.y, vertex.z));
				}
			}
			// Only retain bounds while the mesh's changed signal is tracked.
			if (dependency != resource_dependencies.end()) {
				PoseBounds &snapshot = pose_bounds_cache[p_instance->get_instance_id()];
				snapshot.mesh_id = mesh_id;
				snapshot.mesh_revision = dependency->second.revision;
				snapshot.deformation_signature = deformation;
				snapshot.bounds = mesh_bounds;
				snapshot.used = true;
			}
		}
	}
	Receiver receiver;
	receiver.global_transform = p_instance->get_global_transform();
	if (is_instanced_geometry(p_instance)) {
		receiver.multimesh = p_instance->get_base();
	}
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		receiver.materials.push_back(_surface_material(p_instance, surface));
	}
	receiver.authored_overlay = p_instance->get_material_overlay();
	const AABB local_bounds = to_volume.xform(_material_capture_bounds(receiver, geometry_capture_bounds(p_instance, mesh_bounds)));
	const Vector3 effective_size = _effective_volume_size();
	return AABB(-effective_size * 0.5, effective_size).intersects(local_bounds);
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
void LRTVolume3D::_collect_geometry() {
	material_capture_view = Dictionary();
	Camera3D *capture_camera = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && Node3DEditor::get_singleton()) {
		capture_camera = Node3DEditor::get_singleton()->get_last_used_viewport()->get_camera_3d();
	}
#endif
	if (capture_camera) {
		material_capture_view["transform"] = capture_camera->get_camera_transform();
		material_capture_view["projection"] = capture_camera->get_camera_projection();
		material_capture_view["orthogonal"] = capture_camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
		material_capture_view["near"] = capture_camera->get_near();
		material_capture_view["far"] = capture_camera->get_far();
		material_capture_view["size"] = capture_camera->get_viewport()->get_visible_rect().size;
		material_capture_view["visible_layers"] = capture_camera->get_cull_mask();
	}
	_refresh_scene_candidates();
	for (auto &bounds : pose_bounds_cache) {
		bounds.second.used = false;
	}
	for (auto &dependency : resource_dependencies) {
		dependency.second.used = false;
	}
	std::vector<Receiver> next;
	std::map<ObjectID, uint64_t> material_signatures;
	std::map<GeometryKey, const Receiver *> previous_by_id;
	std::set<GeometryKey> contributing_ids;
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
			if (!mesh_instance->is_visible_in_tree()) {
				continue;
			}
			const Ref<Mesh> mesh = geometry_mesh(mesh_instance);
			if (mesh.is_null()) {
				continue;
			}
			const uint64_t mesh_revision = _resource_dependency_revision(mesh);
			if (!_intersects_volume(mesh_instance)) {
				continue;
			}
			Receiver entry;
			entry.instance_id = mesh_instance->get_instance_id();
			entry.render_instance = mesh_instance->get_instance();
			entry.mesh = mesh;
			entry.transform = canonical_volume_transform(relative_node_transform(this, mesh_instance));
			entry.global_transform = mesh_instance->get_global_transform();
			entry.layer_mask = mesh_instance->get_layer_mask();
			entry.sdf_resolution = _effective_sdf_resolution(mesh_instance);
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
			entry.deformation_signature = lrt::mesh_deformation_signature(Object::cast_to<MeshInstance3D>(mesh_instance));
			if (is_instanced_geometry(mesh_instance) && mesh_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED) {
				entry.deformation_signature = RS::get_singleton()->instance_get_geometry_version(mesh_instance->get_instance());
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
				entry.material_signature = _material_signature(entry, material_signatures);
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
			}
			entry.contributes = mesh_instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED && !entry.contributing_surfaces.is_empty();
			if (entry.contributes) {
				contributing_ids.insert(entry.get_key());
			}
			next.push_back(entry);
		}
	}
	for (ObjectID candidate_id : geometry_candidates) {
		GPUParticles3D *particles = Object::cast_to<GPUParticles3D>(ObjectDB::get_instance(candidate_id));
		if (particles == nullptr || particles->get_world_3d() != get_world_3d() || !particles->is_visible_in_tree() ||
				RS::get_singleton()->particles_is_inactive(particles->get_base())) {
			continue;
		}
		const Transform3D transform = canonical_volume_transform(relative_node_transform(this, particles));
		const Vector3 effective_size = _effective_volume_size();
		const AABB bounds = particles->get_visibility_aabb();
		if (!AABB(-effective_size * 0.5, effective_size).intersects_inclusive(transform.xform(bounds))) {
			continue;
		}
		for (int pass = 0; pass < particles->get_draw_passes(); pass++) {
			const Ref<Mesh> mesh = particles->get_draw_pass_mesh(pass);
			if (mesh.is_null()) {
				continue;
			}
			Receiver entry;
			entry.instance_id = candidate_id;
			entry.render_instance = particles->get_instance();
			entry.draw_pass = pass;
			entry.particles = true;
			entry.particle_bounds = bounds;
			entry.mesh = mesh;
			entry.transform = transform;
			entry.global_transform = particles->get_global_transform();
			entry.layer_mask = particles->get_layer_mask();
			entry.gi_enabled = particles->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED;
			entry.sdf_resolution = _effective_sdf_resolution(particles);
			entry.authored_overlay = particles->get_material_overlay();
			entry.mesh_content_signature = mix_signature(mesh->get_rid().get_id(), _resource_dependency_revision(mesh));
			entry.deformation_signature = RS::get_singleton()->instance_get_geometry_version(entry.render_instance);
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				const Ref<Material> override = particles->get_material_override();
				entry.materials.push_back(override.is_valid() ? override : mesh->surface_get_material(surface));
			}
			entry.material_revision_signature = _material_revision_signature(entry);
			const auto previous = previous_by_id.find(entry.get_key());
			if (previous != previous_by_id.end() && previous->second->material_revision_signature == entry.material_revision_signature) {
				entry.material_signature = previous->second->material_signature;
				entry.material_error = previous->second->material_error;
				entry.contributing_surfaces = previous->second->contributing_surfaces;
			} else {
				entry.material_signature = _material_signature(entry, material_signatures);
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
				const String overlay_error = _material_support_error(entry.authored_overlay);
				if (!overlay_error.is_empty()) {
					entry.material_error += "Overlay: " + overlay_error;
					entry.contributing_surfaces.clear();
				}
			}
			entry.contributes = entry.gi_enabled && !entry.contributing_surfaces.is_empty();
			if (entry.contributes) {
				contributing_ids.insert(entry.get_key());
			}
			next.push_back(std::move(entry));
		}
	}
	for (ObjectID candidate_id : gridmap_candidates) {
		Node3D *node = Object::cast_to<Node3D>(ObjectDB::get_instance(candidate_id));
		if (node == nullptr || node->get_world_3d() != get_world_3d() || !node->is_visible_in_tree()) {
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
			if (!volume_bounds.intersects_inclusive(transform.xform(bounds))) {
				continue;
			}
			entry.transform = transform;
			entry.global_transform = node->get_global_transform();
			entry.sdf_resolution = _effective_sdf_resolution(node);
			entry.mesh_content_signature = mix_signature(entry.mesh->get_rid().get_id(), _resource_dependency_revision(entry.mesh));
			entry.mesh_content_signature = mix_signature(entry.mesh_content_signature, uint64_t(entry.mesh->get_surface_count()));
			entry.deformation_signature = entry.multimesh.is_valid() ? RS::get_singleton()->instance_get_geometry_version(entry.render_instance) : 0;
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
					entry.material_signature = mix_signature(entry.material_signature, _material_content_signature(entry.materials[surface]));
					const String error = _material_support_error(entry.materials[surface]);
					if (error.is_empty()) {
						if (material_chain_contributes(entry.materials[surface]) || (entry.authored_overlay.is_valid() && material_chain_contributes(entry.authored_overlay))) {
							entry.contributing_surfaces.push_back(surface);
						}
					} else {
						entry.material_error += vformat("Surface %d: %s\n", surface, error);
					}
				}
			}
			entry.contributes = !entry.contributing_surfaces.is_empty();
			if (entry.contributes) {
				contributing_ids.insert(entry.get_key());
			}
			next.push_back(std::move(entry));
		}
	}
	for (Receiver &entry : next) {
		const auto previous = previous_by_id.find(entry.get_key());
		if (previous != previous_by_id.end() && previous->second->material_revision_signature == entry.material_revision_signature) {
			entry.material_uses_time = previous->second->material_uses_time;
			entry.material_uses_view = previous->second->material_uses_view;
		} else {
			entry.material_uses_time = material_uses_time(entry.authored_overlay);
			entry.material_uses_view = material_uses_view(entry.authored_overlay);
			for (int surface : entry.contributing_surfaces) {
				entry.material_uses_time = entry.material_uses_time || material_uses_time(entry.materials[surface]);
				entry.material_uses_view = entry.material_uses_view || material_uses_view(entry.materials[surface]);
			}
		}
		entry.material_view_signature = entry.material_uses_view ? material_capture_view.hash() : 0;
		entry.material_frame = entry.material_uses_time ? Engine::get_singleton()->get_frames_drawn() + 1 : 0;
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
	for (auto cached = pose_bounds_cache.begin(); cached != pose_bounds_cache.end();) {
		if (cached->second.used) {
			++cached;
		} else {
			cached = pose_bounds_cache.erase(cached);
		}
	}
	for (auto cache = mesh_capture_cache.begin(); cache != mesh_capture_cache.end();) {
		if (contributing_ids.find(cache->first) != contributing_ids.end()) {
			++cache;
		} else {
			cache = mesh_capture_cache.erase(cache);
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
	return override.is_valid() ? override : geometry_mesh(p_instance)->surface_get_material(p_surface);
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
	Ref<Mesh> mesh = geometry_mesh(p_instance);
	if (mesh.is_valid() && mesh->get_surface_count() > 0) {
		return _material_albedo(_surface_material(p_instance, 0));
	}
	return Vector3(DEFAULT_ALBEDO[0], DEFAULT_ALBEDO[1], DEFAULT_ALBEDO[2]);
}

Vector3 LRTVolume3D::_material_emission(const Ref<Material> &p_material) {
	Ref<StandardMaterial3D> standard = p_material;
	if (standard.is_null() || !standard->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
		return Vector3();
	}
	const Color color = standard->get_emission().srgb_to_linear();
	return Vector3(color.r, color.g, color.b) * standard->get_emission_energy_multiplier();
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

static bool material_uses_time(const Ref<Material> &p_material) {
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

static bool shader_is_view_space_builtin(const StringName &p_name) {
	return p_name == "VERTEX" || p_name == "NORMAL" || p_name == "TANGENT" || p_name == "BINORMAL" || p_name == "LIGHT_VERTEX";
}

// These built-ins are view-space in fragment(), but mesh-local in vertex(). Helper
// functions receive them as arguments, so inspect the fragment AST, including calls.
static bool shader_node_uses_fragment_view(const ShaderLanguage::Node *p_node) {
	using SL = ShaderLanguage;
	if (p_node == nullptr) {
		return false;
	}
	auto uses_view = [](const SL::Node *p_child) {
		return shader_node_uses_fragment_view(p_child);
	};
	switch (p_node->type) {
		case SL::Node::NODE_TYPE_VARIABLE: {
			const auto *variable = static_cast<const SL::VariableNode *>(p_node);
			return !variable->is_local && shader_is_view_space_builtin(variable->name);
		}
		case SL::Node::NODE_TYPE_BLOCK:
			for (const SL::Node *statement : static_cast<const SL::BlockNode *>(p_node)->statements) {
				if (uses_view(statement)) {
					return true;
				}
			}
			break;
		case SL::Node::NODE_TYPE_OPERATOR: {
			const auto *operation = static_cast<const SL::OperatorNode *>(p_node);
			for (const SL::Node *argument : operation->arguments) {
				if (uses_view(argument)) {
					return true;
				}
			}
			break;
		}
		case SL::Node::NODE_TYPE_VARIABLE_DECLARATION:
			for (const auto &declaration : static_cast<const SL::VariableDeclarationNode *>(p_node)->declarations) {
				for (const SL::Node *initializer : declaration.initializer) {
					if (uses_view(initializer)) {
						return true;
					}
				}
			}
			break;
		case SL::Node::NODE_TYPE_CONTROL_FLOW: {
			const auto *flow = static_cast<const SL::ControlFlowNode *>(p_node);
			for (const SL::Node *expression : flow->expressions) {
				if (uses_view(expression)) {
					return true;
				}
			}
			for (const SL::Node *block : flow->blocks) {
				if (uses_view(block)) {
					return true;
				}
			}
			break;
		}
		case SL::Node::NODE_TYPE_MEMBER: {
			const auto *member = static_cast<const SL::MemberNode *>(p_node);
			return uses_view(member->owner) || uses_view(member->index_expression) || uses_view(member->assign_expression) || uses_view(member->call_expression);
		}
		case SL::Node::NODE_TYPE_ARRAY: {
			const auto *array = static_cast<const SL::ArrayNode *>(p_node);
			return (!array->is_local && shader_is_view_space_builtin(array->name)) || uses_view(array->index_expression) || uses_view(array->assign_expression) || uses_view(array->call_expression);
		}
		case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT:
			for (const SL::Node *initializer : static_cast<const SL::ArrayConstructNode *>(p_node)->initializer) {
				if (uses_view(initializer)) {
					return true;
				}
			}
			break;
		default:
			break;
	}
	return false;
}

struct ShaderViewAnalysis {
	String code;
	Vector<StringName> globals;
	uint64_t global_types = 0;
	String error;
	bool fragment_uses_view = false;
};

static ShaderViewAnalysis shader_view_analysis(const Ref<Shader> &p_shader) {
	static std::map<ObjectID, ShaderViewAnalysis> cache;
	const String code = p_shader->get_preprocessed_code();
	const auto found = cache.find(p_shader->get_instance_id());
	const bool same_code = found != cache.end() && found->second.code == code;
	const Vector<StringName> globals = same_code ? found->second.globals : shader_global_uniform_names(code);
	uint64_t global_types = 0;
	for (const StringName &name : globals) {
		global_types = mix_signature(global_types, RS::get_singleton()->global_shader_parameter_get_state(name).type);
	}
	if (same_code && found->second.global_types == global_types && found->second.error.is_empty()) {
		return found->second;
	}
	for (auto entry = cache.begin(); entry != cache.end();) {
		if (ObjectDB::get_instance(entry->first) == nullptr) {
			entry = cache.erase(entry);
		} else {
			++entry;
		}
	}
	ShaderViewAnalysis result;
	result.code = code;
	result.globals = globals;
	result.global_types = global_types;
	ShaderLanguage parser;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types.insert("spatial");
	info.global_shader_uniform_type_func = [](const StringName &p_name) {
		return ShaderLanguage::DataType(RS::global_shader_uniform_type_get_shader_datatype(RS::get_singleton()->global_shader_parameter_get_type(p_name)));
	};
	if (parser.compile(code, info) != OK) {
		result.error = parser.get_error_text();
	} else {
		const ShaderLanguage::ShaderNode *shader = parser.get_shader();
		const auto *fragment = shader->functions.getptr("fragment");
		result.fragment_uses_view = fragment && shader_node_uses_fragment_view(fragment->function->body);
	}
	cache[p_shader->get_instance_id()] = result;
	return result;
}

static bool material_uses_view(const Ref<Material> &p_material) {
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		if (!material_pass_contributes(material)) {
			continue;
		}
		Ref<BaseMaterial3D> standard = material;
		if (standard.is_valid()) {
			const bool uses_height_mapping = standard->get_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING) && !standard->get_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR);
			if (uses_height_mapping || standard->get_billboard_mode() != BaseMaterial3D::BILLBOARD_DISABLED || standard->get_flag(BaseMaterial3D::FLAG_FIXED_SIZE) || standard->get_distance_fade() != BaseMaterial3D::DISTANCE_FADE_DISABLED) {
				return true;
			}
		}
		Ref<ShaderMaterial> shader = material;
		if (shader.is_valid() && shader->get_shader().is_valid()) {
			const String code = shader->get_shader()->get_preprocessed_code();
			if (shader_view_analysis(shader->get_shader()).fragment_uses_view) {
				return true;
			}
			for (const char *word : { "VIEW", "VIEW_MATRIX", "INV_VIEW_MATRIX", "MAIN_CAM_INV_VIEW_MATRIX", "MODELVIEW_MATRIX", "MODELVIEW_NORMAL_MATRIX", "PROJECTION_MATRIX", "INV_PROJECTION_MATRIX", "CAMERA_POSITION_WORLD", "CAMERA_DIRECTION_WORLD", "NODE_POSITION_VIEW", "POSITION", "VIEWPORT_SIZE", "CAMERA_VISIBLE_LAYERS", "skip_vertex_transform" }) {
				if (shader_uses_word(code, word)) {
					return true;
				}
			}
		}
	}
	return false;
}

static bool material_uses_world_transform(const Ref<Material> &p_material) {
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		if (!material_pass_contributes(material)) {
			continue;
		}
		Ref<BaseMaterial3D> standard = material;
		if (standard.is_valid() &&
				((standard->get_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR) && standard->get_flag(BaseMaterial3D::FLAG_UV1_USE_WORLD_TRIPLANAR)) ||
						(standard->get_flag(BaseMaterial3D::FLAG_UV2_USE_TRIPLANAR) && standard->get_flag(BaseMaterial3D::FLAG_UV2_USE_WORLD_TRIPLANAR)))) {
			return true;
		}
		Ref<ShaderMaterial> shader = material;
		if (shader.is_valid() && shader->get_shader().is_valid()) {
			const String code = shader->get_shader()->get_preprocessed_code();
			for (const char *word : { "MODEL_MATRIX", "MODEL_NORMAL_MATRIX", "NODE_POSITION_WORLD", "world_vertex_coords" }) {
				if (shader_uses_word(code, word)) {
					return true;
				}
			}
		}
	}
	return material_uses_view(p_material);
}

static bool material_requires_raster_geometry(const Ref<Material> &p_material) {
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		if (!material_pass_contributes(material)) {
			continue;
		}
		if (Object::cast_to<ShaderMaterial>(material.ptr())) {
			return true;
		}
		Ref<BaseMaterial3D> standard = material;
		if (standard.is_valid() && (material_uses_view(material) || standard->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED || standard->is_grow_enabled())) {
			return true;
		}
	}
	return false;
}

static String material_pass_support_error(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return String();
	}
	if (!material_pass_contributes(p_material)) {
		return String();
	}
	Ref<BaseMaterial3D> standard = p_material;
	if (standard.is_valid()) {
		return String();
	}
	Ref<ShaderMaterial> shader_material = p_material;
	if (shader_material.is_null() || shader_material->get_shader().is_null()) {
		return RTR("LRT supports BaseMaterial3D and ShaderMaterial only.");
	}
	const ShaderViewAnalysis analysis = shader_view_analysis(shader_material->get_shader());
	if (!analysis.error.is_empty()) {
		return vformat("LRT requires a valid spatial shader: %s", analysis.error);
	}
	const String code = shader_material->get_shader()->get_preprocessed_code();
	static const char *unsupported[] = {
		"SCREEN_UV",
		"SCREEN_TEXTURE",
		"DEPTH_TEXTURE",
		"NORMAL_ROUGHNESS_TEXTURE",
		"FRAGCOORD",
		"EYE_OFFSET",
		"VIEW_INDEX",

	};
	for (const char *word : unsupported) {
		if (shader_uses_word(code, word)) {
			return vformat(RTR("LRT material capture does not support shader input or operation: %s"), word);
		}
	}
	return String();
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
	if (p_instance->get_world_3d() != get_world_3d() || !p_instance->is_visible_in_tree()) {
		return RTR("The instance is hidden or belongs to another World3D.");
	}
	int contributing_passes = 0;
	int receiving_passes = 0;
	bool uses_shader = false;
	bool uses_view = false;
	bool instanced = false;
	bool particles = false;
	bool found = false;
	String errors;
	auto count_chain = [&](const Ref<Material> &p_material) {
		Ref<Material> material = p_material;
		do {
			if (material_pass_contributes(material)) {
				contributing_passes++;
				uses_shader = uses_shader || Object::cast_to<ShaderMaterial>(material.ptr()) != nullptr;
			} else {
				receiving_passes++;
			}
			material = material.is_valid() ? material->get_next_pass() : Ref<Material>();
		} while (material.is_valid());
	};
	for (const Receiver &receiver : receivers) {
		if (receiver.instance_id != p_instance->get_instance_id()) {
			continue;
		}
		found = true;
		if (!receiver.gi_enabled) {
			return RTR("GI Mode is Disabled: receives LRT without contributing geometry, color or emission.");
		}
		if (!receiver.material_error.is_empty()) {
			if (!errors.is_empty()) {
				errors += "\n";
			}
			errors += receiver.particles ? vformat(RTR("Draw Pass %d: %s"), receiver.draw_pass, receiver.material_error) : receiver.material_error;
		}
		uses_view = uses_view || receiver.material_uses_view;
		instanced = instanced || receiver.multimesh.is_valid();
		particles = particles || receiver.particles;
		for (const Ref<Material> &material : receiver.materials) {
			count_chain(material);
			if (receiver.authored_overlay.is_valid()) {
				count_chain(receiver.authored_overlay);
			}
		}
	}
	if (!errors.is_empty()) {
		return errors.strip_edges();
	}
	if (!found) {
		return RTR("No contribution inside this Volume. Check visibility, mesh geometry and capture bounds.");
	}
	if (contributing_passes > 0 && !error_message.is_empty()) {
		return error_message;
	}
	String message = vformat(RTR("Contributing passes: %d. Receive-only passes: %d."), contributing_passes, receiving_passes);
	if (receiving_passes > 0) {
		message += "\n" + RTR("Receive-only passes do not occlude or contribute color or emission.");
	}
	if (contributing_passes == 0) {
		return message;
	}
	if (uses_view) {
		message += "\n" + RTR("Uses the active camera; camera changes update the capture.");
	}
	if (uses_shader) {
		message += "\n" + RTR("Shader vertex displacement must fit Custom AABB or Extra Cull Margin; geometry outside is clipped.");
	}
	if (particles) {
		message += "\n" + RTR("Particle Visibility AABB must cover all draw passes, billboards and trails.");
	} else if (instanced) {
		message += "\n" + RTR("Mesh or instance visibility bounds must cover all instanced vertex transformations.");
	}
	return message;
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
	}
}

void LRTVolume3D::_shadow_resource_dependency_changed(ObjectID p_id) {
	const auto found = shadow_resource_dependencies.find(p_id);
	if (found != shadow_resource_dependencies.end()) {
		found->second.revision++;
	}
}

void LRTVolume3D::_release_resource_dependencies(bool p_all, bool p_shadow) {
	if (p_all && !p_shadow) {
		pose_bounds_cache.clear();
	}
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

uint64_t LRTVolume3D::_material_content_signature(const Ref<Material> &p_material, bool p_persistent) const {
	if (p_material.is_null()) {
		return 0;
	}
	uint64_t state = mix_signature(0, p_material->get_class_name().hash());
	auto hash_value = [this, &state, p_persistent](const Variant &p_value) {
		if (p_value.get_type() != Variant::OBJECT) {
			state = mix_signature(state, p_value.hash());
			return;
		}
		Ref<Resource> resource = p_value;
		if (resource.is_null()) {
			state = mix_signature(state, 0);
			return;
		}
		state = mix_signature(state, resource->get_class_name().hash());
		// Persistent identity follows content across reloads, moves and PCK remaps.
		const Ref<Material> nested_material = resource;
		if (nested_material.is_valid()) {
			state = mix_signature(state, _material_content_signature(nested_material, p_persistent));
		}
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
		Ref<Shader> shader = resource;
		if (shader.is_valid()) {
			state = mix_signature(state, shader->get_preprocessed_code().hash());
		}
	};
	List<PropertyInfo> properties;
	p_material->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
			continue;
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
	uint64_t state = 0;
	Ref<Mesh> mesh = p_receiver.mesh;
	if (mesh.is_null()) {
		return 0;
	}
	auto hash_value = [this, &state](const Variant &p_value) {
		state = mix_signature(state, p_value.hash());
		if (p_value.get_type() == Variant::OBJECT) {
			Ref<Resource> resource = p_value;
			if (resource.is_valid()) {
				state = mix_signature(state, resource->get_rid().get_id());
				state = mix_signature(state, _resource_dependency_revision(resource));
			}
		}
	};
	auto hash_material = [this, &state, &r_material_signatures](const Ref<Material> &p_material) {
		if (p_material.is_null()) {
			state = mix_signature(state, 0);
			return;
		}
		const ObjectID material_id = p_material->get_instance_id();
		auto found = r_material_signatures.find(material_id);
		if (found == r_material_signatures.end()) {
			found = r_material_signatures.emplace(material_id, _material_resource_signature(p_material)).first;
		}
		state = mix_signature(state, uint64_t(material_id));
		state = mix_signature(state, found->second);
	};
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		hash_material(p_receiver.materials[surface]);
	}
	hash_material(p_receiver.authored_overlay);
	List<PropertyInfo> instance_uniforms;
	RS::get_singleton()->instance_geometry_get_shader_parameter_list(p_receiver.render_instance, &instance_uniforms);
	for (const PropertyInfo &property : instance_uniforms) {
		const Variant value = RS::get_singleton()->instance_geometry_get_shader_parameter(p_receiver.render_instance, property.name);
		hash_value(value);
	}
	state = mix_signature(state, geometry_capture_bounds_signature(p_receiver.instance_id));
	bool uses_world = material_uses_world_transform(p_receiver.authored_overlay);
	for (const Ref<Material> &material : p_receiver.materials) {
		uses_world = uses_world || material_uses_world_transform(material);
	}
	return uses_world ? mix_signature(state, Variant(p_receiver.global_transform).hash()) : state;
}

uint64_t LRTVolume3D::_material_revision_signature(const Receiver &p_receiver) {
	Ref<Mesh> mesh = p_receiver.mesh;
	if (mesh.is_null()) {
		return 0;
	}
	uint64_t state = mix_signature(mesh->get_rid().get_id(), _resource_dependency_revision(mesh));
	bool uses_shader_material = false;
	auto hash_material_revision = [this, &state, &uses_shader_material](const Ref<Material> &p_material) {
		if (p_material.is_null()) {
			state = mix_signature(state, 0);
			return;
		}
		for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
			state = mix_signature(state, uint64_t(material->get_instance_id()));
			state = mix_signature(state, material->get_render_priority());
			Ref<BaseMaterial3D> base_material = material;
			if (base_material.is_valid()) {
				state = mix_signature(state, base_material->get_parameter_change_version());
				for (int index = 0; index < BaseMaterial3D::TEXTURE_MAX; index++) {
					state = mix_signature(state, _resource_dependency_revision(base_material->get_texture(BaseMaterial3D::TextureParam(index))));
				}
			}
			Ref<ShaderMaterial> shader_material = material;
			if (shader_material.is_valid()) {
				state = mix_signature(state, _material_resource_signature(material));
				uses_shader_material = true;
			}
		}
	};
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		hash_material_revision(p_receiver.materials[surface]);
	}
	hash_material_revision(p_receiver.authored_overlay);
	if (uses_shader_material) {
		List<PropertyInfo> instance_uniforms;
		RS::get_singleton()->instance_geometry_get_shader_parameter_list(p_receiver.render_instance, &instance_uniforms);
		for (const PropertyInfo &property : instance_uniforms) {
			state = mix_signature(state, RS::get_singleton()->instance_geometry_get_shader_parameter(p_receiver.render_instance, property.name).hash());
		}
	}
	state = mix_signature(state, geometry_capture_bounds_signature(p_receiver.instance_id));
	bool uses_world = material_uses_world_transform(p_receiver.authored_overlay);
	for (const Ref<Material> &material : p_receiver.materials) {
		uses_world = uses_world || material_uses_world_transform(material);
	}
	return uses_world ? mix_signature(state, Variant(p_receiver.global_transform).hash()) : state;
}

bool LRTVolume3D::_material_inputs_unchanged() {
	for (const Receiver &receiver : receivers) {
		if (receiver.contributes && _material_revision_signature(receiver) != receiver.material_revision_signature) {
			return false;
		}
	}
	return true;
}

int LRTVolume3D::_effective_sdf_resolution(Node3D *p_instance) const {
	const int instance_override = int(p_instance->get_meta(INSTANCE_SDF_RESOLUTION_META, 0));
	if (instance_override > 0) {
		return instance_override;
	}
	return CLAMP(int(GLOBAL_GET(DEFAULT_SDF_RESOLUTION_SETTING)), 8, 256);
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

// Everything that changes the geometric local field is hashed every frame. Material output
// deliberately stays out of this key. Both changes rebuild the local field today, but keeping
// their invalidation classes separate guarantees that a material edit reuses the shared SDF.
uint64_t LRTVolume3D::_geometry_signature() const {
	uint64_t state = 0;
	state = mix_signature(state, uint64_t(geometry_backend));
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
		state = mix_signature(state, receiver.deformation_signature);
		state = mix_signature(state, mesh.is_valid() ? uint64_t(mesh->get_surface_count()) : 0);
	}
	return state;
}

uint64_t LRTVolume3D::_build_cache_fingerprint(bool p_defer_instances) {
	uint64_t state = mix_signature(0, 14); // Persistent local-cache algorithm and content identity version.
	state = mix_signature(state, quantized_signature_value(spacing, 100000.0));
	state = mix_signature(state, quantized_signature_value(volume_size.x, 10000.0));
	state = mix_signature(state, quantized_signature_value(volume_size.y, 10000.0));
	state = mix_signature(state, quantized_signature_value(volume_size.z, 10000.0));
	state = mix_signature(state, uint64_t(geometry_backend));
	state = mix_signature(state, uint64_t(visibility_mode));
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		state = mix_signature(state, geometry_capture_bounds_signature(receiver.instance_id));
		if (receiver.material_uses_time || receiver.material_uses_view) {
			return 0; // A renderer-time snapshot has no stable identity across runs.
		}
		bool uses_world = material_uses_world_transform(receiver.authored_overlay);
		for (int surface : receiver.contributing_surfaces) {
			uses_world = uses_world || material_uses_world_transform(receiver.materials[surface]);
		}
		if (uses_world) {
			state = mix_signature(state, Variant(receiver.global_transform).hash());
		}
		const Transform3D &transform = receiver.transform;
		for (int axis = 0; axis < 3; axis++) {
			state = mix_signature(state, quantized_signature_value(transform.origin[axis], 10000.0));
			for (int column = 0; column < 3; column++) {
				state = mix_signature(state, quantized_signature_value(transform.basis[axis][column], 1000000.0));
			}
		}
		const Ref<Mesh> mesh = receiver.mesh;
		state = mix_signature(state, mesh_content_signature(mesh));
		if (receiver.multimesh.is_null() && !receiver.particles) {
			state = mix_signature(state, receiver.deformation_signature);
		}
		state = mix_signature(state, uint64_t(receiver.sdf_resolution));
		state = mix_signature(state, uint64_t(receiver.layer_mask));
		for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
			state = mix_signature(state, _material_content_signature(receiver.materials[surface]));
		}
		state = mix_signature(state, _material_content_signature(receiver.authored_overlay));
		List<PropertyInfo> instance_uniforms;
		RS::get_singleton()->instance_geometry_get_shader_parameter_list(receiver.render_instance, &instance_uniforms);
		for (const PropertyInfo &property : instance_uniforms) {
			state = mix_signature(state, property.name.hash());
			state = mix_signature(state, RS::get_singleton()->instance_geometry_get_shader_parameter(receiver.render_instance, property.name).hash());
		}
	}
	if (!p_defer_instances) {
		for (const Receiver &receiver : receivers) {
			if (!receiver.contributes || (receiver.multimesh.is_null() && !receiver.particles)) {
				continue;
			}
			const auto cached = mesh_capture_cache.find(receiver.get_key());
			if (cached == mesh_capture_cache.end() || (!cached->second.copies && !cached->second.raster_capture) || cached->second.source_version != receiver.deformation_signature) {
				return 0; // The asynchronous instance snapshot is needed before checking persistent data.
			}
			state = mix_signature(state, cached->second.instance_signature);
		}
	}
	// Content reads may flush queued global-uniform edits after collection. Never label
	// an earlier input snapshot with the newer material's persistent identity.
	return _material_inputs_unchanged() ? state : 0;
}

uint64_t LRTVolume3D::_material_state_signature() const {
	uint64_t state = 0;
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		state = mix_signature(state, receiver.render_instance.get_id());
		state = mix_signature(state, receiver.material_frame);
		state = mix_signature(state, receiver.material_view_signature);
		state = mix_signature(state, receiver.material_signature);
		state = mix_signature(state, quantized_signature_value(receiver.albedo.x, 100000.0));
		state = mix_signature(state, quantized_signature_value(receiver.albedo.y, 100000.0));
		state = mix_signature(state, quantized_signature_value(receiver.albedo.z, 100000.0));
	}
	return state;
}

// The prototype's analytic box path is world axis-aligned, so a box instance counts as one
// only when its basis maps the box axes onto the world axes.
bool LRTVolume3D::_is_axis_aligned(const Basis &p_basis) {
	const Basis normalized = p_basis.orthonormalized();
	for (int column = 0; column < 3; column++) {
		const Vector3 axis = normalized.get_column(column);
		int nonzero = 0;
		for (int row = 0; row < 3; row++) {
			const real_t value = Math::abs(axis[row]);
			if (value > 0.99999) {
				nonzero++;
			} else if (value > 0.00001) {
				return false;
			}
		}
		if (nonzero != 1) {
			return false;
		}
	}
	return true;
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
	if (p_left.get("has_projector", false) != p_right.get("has_projector", false)) {
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
	_inject_sources(false, p_count_invalidation);
	native_source_ready = true;
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
	// The Volume depth map only becomes valid one frame after a directional light first resolves.
	// Mixing that state in re-resolves the shadowed lights when the map arrives, so a settled light
	// does not keep the unshadowed field it published while the map was still missing.
	mix_resource(uint64_t(LRTRenderBridge::is_volume_shadow_valid(get_instance_id())));
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
	for (const ObjectID candidate_id : geometry_candidates) {
		GeometryInstance3D *mesh_instance = geometry_from_id(candidate_id);
		if (mesh_instance == nullptr || mesh_instance->get_world_3d() != get_world_3d() ||
				!mesh_instance->is_visible_in_tree() ||
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
	return mix_signature(state, resource_state);
}

void LRTVolume3D::_queue_native_light_capture(bool p_receiver_layout_changed, bool p_count_invalidation) {
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
		const Dictionary mapped_light = mapped_lights[mapped_index];
		if (!bool(mapped_light.get("enabled", false))) {
			continue;
		}
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
	const bool refresh_shadow_resources = shadow_capture_resource_signature != active_shadow_capture_resource_signature;
	const int next_light_count = next_capture_inputs.size();
	std::vector<bool> dirty_light_slots(size_t(next_light_count), true);
	if (reuse_unit_fields) {
		for (int slot = 0; slot < next_light_count; slot++) {
			const Dictionary next_input = next_capture_inputs[slot];
			dirty_light_slots[size_t(slot)] = !_light_capture_input_equal(next_input, native_light_field_inputs[slot]);
			if (p_receiver_layout_changed) {
				// Packed layer masks live in the receiver buffer. Reusing an unshadowed unit
				// field would keep lighting on receivers that just left the light cull mask.
				dirty_light_slots[size_t(slot)] = true;
			} else if (refresh_shadow_resources && !dirty_light_slots[size_t(slot)] && bool(next_input.get("casts_shadow", false))) {
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
		if (photometry_changed) {
			// Energy, indirect energy, colour, temperature and the negative-light sign are linear
			// photometric changes: reweighting the published unit fields is enough, and a queued
			// follow-up that finds no dirty light must not drop the reweight.
			_apply_native_light_photometry(true);
		}
		return;
	}
	native_light_diagnostics.clear();
	if (light_set_changed || p_receiver_layout_changed) {
		const bool initialize_source = native_source_ready && native_light_field_inputs.is_empty() && native_capture_updates == 0;
		// A new grid keeps its coherent emission/sky source. Otherwise retain the old probe
		// source until unit buffers for the replacement receiver layout are committed.
		native_source_ready = initialize_source;
		solver->reset_native_lights(next_light_count);
		native_light_field_set_signature = next_light_set_signature;
		native_light_field_inputs.clear();
	}
	active_native_light_capture_inputs = next_capture_inputs;
	const Transform3D capture_volume_to_world = get_global_transform();
	const int receiver_count = solver->get_receiver_count();
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
		resolve.volume_to_source = source_to_volume.affine_inverse();
		resolve.light_position = source_to_volume.origin;
		resolve.light_direction = -source_to_volume.basis.get_column(2).normalized();
		resolve.source_range = light->get_param(Light3D::PARAM_RANGE);
		resolve.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
		resolve.shadow_bias = light->get_param(Light3D::PARAM_SHADOW_BIAS);
		resolve.shadow_enabled = snapshot.shadow_enabled;
		resolve.receiver_count = receiver_count;
		resolve.light_slot = snapshot.light_slot;
		resolve.cull_mask = light->get_cull_mask();
		resolve.target_buffer = solver->begin_native_light_unit_field(snapshot.light_slot);
		resolve.directional = Object::cast_to<DirectionalLight3D>(light) != nullptr;
		resolve.projector_requested = !light->get_projector().is_null();
		if (resolve.directional) {
			resolve.direct_kind = 2;
			resolve.volume_to_source.origin = source_to_volume.basis.get_column(2).normalized();
		} else if (area_light != nullptr) {
			resolve.direct_kind = 5;
			resolve.area = true;
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
		solver->commit_native_light_unit_field(snapshot.light_slot, uint64_t(snapshot.source_id), snapshot.input_usec);
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
	if (p_resolution <= 0) {
		p_resolution = CLAMP(int(GLOBAL_GET(DEFAULT_SDF_RESOLUTION_SETTING)), 8, 256);
	}
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
	_collect_geometry();
	std::vector<LRTVolume::BoxInstance> boxes;
	std::vector<LRTVolume::MeshInstance> meshes;
	String error;
	if (!_build_geometry_inputs(boxes, meshes, true, error, false)) {
		report["error"] = error;
		return report;
	}
	Ref<LRTVolume> prepared;
	prepared.instantiate();
	prepared->configure_sized(spacing, -volume_size * 0.5, volume_size);
	prepared->set_sh_visibility(visibility_mode == VISIBILITY_SH);
	prepared->set_box_instances(boxes);
	prepared->set_mesh_instances(meshes);
	const LRTVolume::LocalBakeResult result = prepared->bake_local_field_data(geometry_backend == BACKEND_ANALYTIC);
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
	if (geometry_backend == BACKEND_SDF && result.sdf_instance_references > 0 && dependencies.is_empty()) {
		report["error"] = "Cannot store prepared geometry and instance dependencies.";
		return report;
	}
	if (fingerprint != 0 && !prepared->store_local_field_cache(fingerprint, true)) {
		report["error"] = "Cannot store the prepared Volume field.";
		return report;
	}
	if (fingerprint != 0) {
		dependencies.push_back(lrt::asset_cache_directory().path_join(vformat("volume_%016x.lrt", fingerprint)));
	}
	report["ok"] = true;
	report["assets"] = dependencies;
	return report;
}
#endif

static void particle_material_modes(const Ref<Material> &p_material, bool &r_rigid, bool &r_trails) {
	if (p_material.is_null()) {
		r_rigid = true;
		return;
	}
	for (Ref<Material> material = p_material; material.is_valid(); material = material->get_next_pass()) {
		const Ref<BaseMaterial3D> standard = material;
		const Ref<ShaderMaterial> shader = material;
		const bool trails = standard.is_valid() ? standard->get_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE) :
				(shader.is_valid() && shader->get_shader().is_valid() && shader_uses_word(shader->get_shader()->get_preprocessed_code(), "particle_trails"));
		r_trails = r_trails || trails;
		r_rigid = r_rigid || !trails;
	}
}

bool LRTVolume3D::_capture_mesh(const Receiver &p_receiver,
		const Transform3D &p_transform, int p_resolution, const Vector<int> &p_surfaces,
		LRTVolume::MeshInstance &r_mesh, String &r_error, bool p_async) {
	if (p_receiver.material_uses_view && material_capture_view.is_empty()) {
		r_error = "Camera-dependent LRT contributors require an active Camera3D.";
		return false;
	}
	Ref<Mesh> source_mesh = p_receiver.mesh;
	if (!r_mesh.triangles) {
		auto triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
		if (!lrt::mesh_triangles(source_mesh, *triangles, Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(p_receiver.instance_id)), &p_surfaces)) {
			r_error = "LRT 材质捕获找不到三角形表面";
			return false;
		}
		r_mesh.triangles = triangles;
	}
	const auto &triangles = r_mesh.triangles;
	if (r_mesh.instanced) {
		const bool skin_cached = !r_mesh.particle_skin.empty();
		r_mesh.draw_surfaces.resize(source_mesh->get_surface_count());
		for (int surface = 0; surface < source_mesh->get_surface_count(); surface++) {
			const int indices = source_mesh->surface_get_array_index_len(surface);
			const int elements = indices > 0 ? indices : source_mesh->surface_get_array_len(surface);
			r_mesh.draw_surfaces[surface] = { elements, indices > 0 };
			if (p_receiver.particles) {
				lrt::MeshDrawSurface &draw = r_mesh.draw_surfaces[surface];
				draw.particle_rigid = false;
				particle_material_modes(p_receiver.materials[surface], draw.particle_rigid, draw.particle_trails);
				if (p_receiver.authored_overlay.is_valid()) {
					particle_material_modes(p_receiver.authored_overlay, draw.particle_rigid, draw.particle_trails);
				}
			}
			if (p_surfaces.has(surface) && source_mesh->surface_get_primitive_type(surface) == Mesh::PRIMITIVE_TRIANGLES) {
				r_mesh.triangle_surfaces.insert(r_mesh.triangle_surfaces.end(), elements / 3, surface);
				if (p_receiver.particles && !skin_cached && !r_mesh.draw_surfaces[surface].particle_trails) {
					r_mesh.particle_skin.resize(r_mesh.particle_skin.size() + elements / 3);
				} else if (p_receiver.particles && !skin_cached) {
					const Array arrays = source_mesh->surface_get_arrays(surface);
					const PackedInt32Array vertex_indices = arrays[Mesh::ARRAY_INDEX];
					const PackedInt32Array bones = arrays[Mesh::ARRAY_BONES];
					const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
					const int influences = (source_mesh->surface_get_format(surface) & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS) ? 8 : 4;
					for (int triangle = 0; triangle < elements / 3; triangle++) {
						lrt::MeshTriangleSkin skin;
						for (int corner = 0; corner < 3; corner++) {
							const int vertex = indices > 0 ? vertex_indices[triangle * 3 + corner] : triangle * 3 + corner;
							// Particle trails use the first four attributes, including on eight-weight meshes.
							for (int influence = 0; influence < 4; influence++) {
								skin.bones[corner][influence] = bones.is_empty() ? 0 : bones[vertex * influences + influence];
								skin.weights[corner][influence] = weights.is_empty() ? 0.0f : weights[vertex * influences + influence];
							}
						}
						r_mesh.particle_skin.push_back(skin);
					}
				}
			}
		}
	}
	const lrt::Vec3 &first = triangles->front().position[0];
	AABB mesh_bounds(Vector3(first.x, first.y, first.z), Vector3());
	for (const lrt::MeshTriangle &triangle : *triangles) {
		for (const lrt::Vec3 &vertex : triangle.position) {
			mesh_bounds.expand_to(Vector3(vertex.x, vertex.y, vertex.z));
		}
	}
	if (r_mesh.instanced) {
		mesh_bounds = p_receiver.particles ? p_receiver.particle_bounds : RS::get_singleton()->multimesh_get_aabb(p_receiver.multimesh);
	}
	mesh_bounds = geometry_capture_bounds(Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(p_receiver.instance_id)), mesh_bounds);
	mesh_bounds = _material_capture_bounds(p_receiver, mesh_bounds);
	const double longest = mesh_bounds.get_longest_axis_size();
	if (longest <= 0.0) {
		r_error = "LRT 材质捕获的 Mesh 边界为空";
		return false;
	}
	const AABB local_capture_bounds = mesh_bounds.grow(longest / double(MAX(8, p_resolution)));
	const AABB world_capture_bounds = p_transform.xform(local_capture_bounds);
	const double world_longest = world_capture_bounds.get_longest_axis_size();
	const int material_resolution = MAX(16, p_resolution / (r_mesh.raster_geometry ? 2 : 4));
	Vector3i capture_size;
	for (int axis = 0; axis < 3; axis++) {
		capture_size[axis] = CLAMP(int(Math::ceil(world_capture_bounds.size[axis] / world_longest * material_resolution)) + 1, 4, 128);
	}
	// SceneTree transform notifications may still be queued when a newly added node is captured.
	Node3D *source = Object::cast_to<Node3D>(ObjectDB::get_instance(p_receiver.instance_id));
	ERR_FAIL_NULL_V(source, false);
	source->force_update_transform();
	BaseMaterial3D::flush_changes();
	Callable callback;
	if (p_async) {
		PendingMaterialCapture request;
		request.geometry_key = p_receiver.get_key();
		request.size = capture_size;
		request.transform = p_transform;
		pending_material_captures[++material_capture_id] = request;
		callback = callable_mp(this, &LRTVolume3D::_material_capture_ready).bind(material_capture_id);
	}
	Vector<int> capture_surfaces = p_surfaces;
	for (int &surface : capture_surfaces) {
		surface |= p_receiver.draw_pass << 16;
	}
	const Dictionary images = RS::get_singleton()->bake_render_material_volume(
			p_receiver.render_instance, local_capture_bounds, capture_size, capture_surfaces, callback, p_receiver.material_uses_view ? material_capture_view : Dictionary());
	if (p_async && bool(images.get("pending", false))) {
		return true;
	}
	if (p_async) {
		pending_material_captures.erase(material_capture_id);
	}
	return _decode_material_capture(images, capture_size, p_transform, r_mesh, r_error);
}

void LRTVolume3D::_material_capture_ready(const Dictionary &p_images, uint64_t p_id) {
	particle_buffer_async_readbacks += uint64_t(p_images.get("particle_buffer_async_readbacks", 0));
	particle_buffer_async_readback_reuses += uint64_t(p_images.get("particle_buffer_async_readback_reuses", 0));
	particle_buffer_async_readback_bytes += uint64_t(p_images.get("particle_buffer_async_readback_bytes", 0));
	auto request = pending_material_captures.find(p_id);
	if (request != pending_material_captures.end()) {
		request->second.images = p_images;
		request->second.ready = true;
	}
}

bool LRTVolume3D::_decode_material_capture(const Dictionary &images, const Vector3i &capture_size,
		const Transform3D &p_requested_transform, LRTVolume::MeshInstance &r_mesh, String &r_error) {
	if (images.has("error")) {
		r_error = images["error"];
		return false;
	}
	if (images.is_empty() || Vector3i(images.get("size", Vector3i())) != capture_size) {
		r_error = "LRT 三维材质捕获失败；当前路径要求 D3D12 Forward+";
		return false;
	}
	const PackedByteArray albedo_data = images.get("albedo", PackedByteArray());
	const PackedByteArray emission_data = images.get("emission", PackedByteArray());
	const PackedByteArray emission_aniso_data = images.get("emission_aniso", PackedByteArray());
	const PackedByteArray normal_bits_data = images.get("normal_bits", PackedByteArray());
	const int capture_count = capture_size.x * capture_size.y * capture_size.z;
	if (!images.has("bounds") || !images.has("transform")) {
		r_error = "Material capture is missing its geometry snapshot transform.";
		return false;
	}
	const Transform3D p_transform = images["transform"];
	const AABB world_capture_bounds = images["bounds"];
	r_mesh.transform = r_mesh.transform * to_lrt_transform(p_requested_transform.affine_inverse() * p_transform);
	const Vector3i render_size = capture_size * 2;
	const int render_count = render_size.x * render_size.y * render_size.z;
	if (albedo_data.size() != capture_count * 6 * 2 || emission_data.size() != capture_count * 4 ||
			emission_aniso_data.size() != capture_count * 4 || normal_bits_data.size() != render_count * 4) {
		r_error = "LRT 三维材质捕获返回了无效的数据布局";
		return false;
	}
	if (r_mesh.instanced && !r_mesh.raster_geometry) {
		const bool decoded = images.has("particle_trail_steps") ?
				lrt::decode_particle_capture(images, r_mesh.triangles, r_mesh.triangle_surfaces, r_mesh.draw_surfaces, r_mesh.particle_skin, r_mesh.copies, r_error) :
				lrt::decode_multimesh_capture(images, r_mesh.triangles, r_mesh.triangle_surfaces, r_mesh.draw_surfaces, r_mesh.copies, r_error);
		if (!decoded) {
			return false;
		}
	}
	if (r_mesh.instanced) {
		r_mesh.instance_signature = mix_signature(0, images.get("instance_buffer", PackedByteArray()).hash());
		for (const char *key : { "instance_commands", "instance_command_stride", "instance_count", "instance_stride", "instance_format_2d", "particle_trail_steps", "particle_local_coords", "particle_trail_buffer" }) {
			r_mesh.instance_signature = mix_signature(r_mesh.instance_signature, images.get(key, Variant()).hash());
		}
		if (images.has("particle_local_coords") && !bool(images["particle_local_coords"])) {
			// World-space particles remain fixed when their emitter and Volume move together.
			r_mesh.instance_signature = mix_signature(r_mesh.instance_signature, Variant(p_transform).hash());
		}
	}
	std::shared_ptr<lrt::MaterialCapture> material = std::make_shared<lrt::MaterialCapture>();
	for (int axis = 0; axis < 3; axis++) {
		material->size[axis] = capture_size[axis];
	}
	material->albedo.resize(size_t(capture_count) * 3);
	material->occupied.resize(capture_count);
	const Vector3 inverse_size = Vector3(1.0 / world_capture_bounds.size.x, 1.0 / world_capture_bounds.size.y,
			1.0 / world_capture_bounds.size.z);
	const Vector3 world_offset = p_transform.origin - world_capture_bounds.position;
	material->uvw_offset = to_lrt(world_offset * inverse_size);
	material->uvw_basis_x = to_lrt(p_transform.basis.get_column(0) * inverse_size);
	material->uvw_basis_y = to_lrt(p_transform.basis.get_column(1) * inverse_size);
	material->uvw_basis_z = to_lrt(p_transform.basis.get_column(2) * inverse_size);
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
				for (int direction = 0; direction < 6; direction++) {
					const int packed_index = x + capture_size.x * (y + capture_size.y * (z * 6 + direction));
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
				for (int dz = 0; dz < 2 && !material->occupied[size_t(index)]; dz++) {
					for (int dy = 0; dy < 2 && !material->occupied[size_t(index)]; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							const int normal_index = x * 2 + dx + render_size.x * ((y * 2 + dy) + render_size.y * (z * 2 + dz));
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
	if (r_mesh.raster_geometry) {
		std::vector<uint8_t> surface(render_count);
		for (int index = 0; index < render_count; index++) {
			surface[index] = decode_uint32(normal_bytes + index * 4) != 0;
		}
		const Vector3 cell_size = world_capture_bounds.size / Vector3(render_size);
		const Transform3D grid_to_world(Basis::from_scale(cell_size), world_capture_bounds.position);
		auto capture = std::make_shared<lrt::RasterGeometryCapture>();
		const int size[3] = { render_size.x, render_size.y, render_size.z };
		if (!lrt::bake_raster_geometry_capture(surface, size, *material,
				to_lrt_transform(p_transform.affine_inverse() * grid_to_world), *capture)) {
			r_error = "LRT captured surface distance field construction failed.";
			return false;
		}
		r_mesh.raster_capture = std::move(capture);
	}
	r_mesh.material = material;
	r_mesh.material_signature = lrt::material_field_input_signature(*r_mesh.triangles, material.get());
	return true;
}

static bool standard_material_is_constant(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return true;
	}
	Ref<StandardMaterial3D> standard = p_material;
	return standard.is_valid() && standard->get_next_pass().is_null() && !material_requires_raster_geometry(p_material) && !standard->get_feature(BaseMaterial3D::FEATURE_EMISSION) &&
			!standard->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR) &&
			standard->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_null() &&
			standard->get_texture(BaseMaterial3D::TEXTURE_DETAIL_ALBEDO).is_null();
}

static uint64_t mesh_capture_bytes(const std::shared_ptr<const std::vector<lrt::MeshTriangle>> &p_triangles,
		const std::shared_ptr<const lrt::MaterialCapture> &p_material, const std::shared_ptr<const std::vector<lrt::MeshCopy>> &p_copies,
		const std::shared_ptr<const lrt::RasterGeometryCapture> &p_raster,
		const std::shared_ptr<const std::vector<lrt::MeshTriangleSkin>> &p_particle_skin, std::set<const void *> *p_counted) {
	std::set<const void *> local_counted;
	std::set<const void *> &counted = p_counted != nullptr ? *p_counted : local_counted;
	uint64_t bytes = 0;
	if (p_particle_skin && counted.insert(p_particle_skin.get()).second) {
		bytes += p_particle_skin->capacity() * sizeof(lrt::MeshTriangleSkin);
	}
	if (p_triangles && counted.insert(p_triangles.get()).second) {
		bytes += uint64_t(p_triangles->capacity()) * sizeof(lrt::MeshTriangle);
	}
	if (p_material && counted.insert(p_material.get()).second) {
		bytes += uint64_t(p_material->albedo.capacity() + p_material->emission.capacity()) * sizeof(float);
		bytes += uint64_t(p_material->occupied.capacity()) * sizeof(uint8_t);
	}
	if (p_copies && counted.insert(p_copies.get()).second) {
		bytes += p_copies->capacity() * sizeof(lrt::MeshCopy);
		for (const lrt::MeshCopy &copy : *p_copies) {
			if (copy.triangles && counted.insert(copy.triangles.get()).second) {
				bytes += copy.triangles->capacity() * sizeof(lrt::MeshTriangle);
			}
		}
	}
	if (p_raster && counted.insert(p_raster.get()).second) {
		bytes += sizeof(lrt::RasterGeometryCapture) + lrt::asset_field_bytes(p_raster->geometry) +
				p_raster->material.albedo.capacity() + p_raster->material.emission.capacity() * sizeof(float);
	}
	return bytes;
}

void LRTVolume3D::_store_shared_mesh_capture(uint64_t p_key, const MeshCaptureCache &p_cache) {
	if (p_cache.byte_size > SHARED_MESH_CAPTURE_BUDGET_BYTES) {
		return;
	}
	auto existing = shared_mesh_capture_cache.find(p_key);
	if (existing != shared_mesh_capture_cache.end()) {
		shared_mesh_capture_cache_bytes -= existing->second.byte_size;
		shared_mesh_capture_cache.erase(existing);
	}
	while (!shared_mesh_capture_cache.empty() && shared_mesh_capture_cache_bytes + p_cache.byte_size > SHARED_MESH_CAPTURE_BUDGET_BYTES) {
		auto least_recent = shared_mesh_capture_cache.begin();
		for (auto candidate = shared_mesh_capture_cache.begin(); candidate != shared_mesh_capture_cache.end(); ++candidate) {
			if (candidate->second.last_used < least_recent->second.last_used) {
				least_recent = candidate;
			}
		}
		shared_mesh_capture_cache_bytes -= least_recent->second.byte_size;
		shared_mesh_capture_cache.erase(least_recent);
	}
	shared_mesh_capture_cache_bytes += p_cache.byte_size;
	shared_mesh_capture_cache[p_key] = p_cache;
}

void LRTVolume3D::clear_shared_mesh_capture_cache() {
	shared_mesh_capture_cache.clear();
	shared_mesh_capture_cache_bytes = 0;
}


// Every SDF input keeps asset-local geometry. Its full affine basis is sampled in volume space,
// so rotations and non-uniform scales never force a world-space copy of the distance field.
bool LRTVolume3D::_build_geometry_inputs(std::vector<LRTVolume::BoxInstance> &r_boxes,
		std::vector<LRTVolume::MeshInstance> &r_meshes, bool p_validate_mesh_content, String &r_error, bool p_async) {
	r_boxes.clear();
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
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		const Ref<Mesh> &mesh = receiver.mesh;
		const Transform3D &transform = receiver.transform;
		const Basis basis = transform.basis;
		BoxMesh *box = Object::cast_to<BoxMesh>(mesh.ptr());
		const Ref<Material> first_material = mesh->get_surface_count() > 0 ? receiver.materials[0] : Ref<Material>();
		if (box != nullptr && (receiver.multimesh.is_null() && !receiver.particles) && receiver.authored_overlay.is_null() && standard_material_is_constant(first_material)) {
			LRTVolume::BoxInstance entry;
			entry.local_extent = to_lrt(box->get_size());
			entry.color = to_lrt(receiver.albedo);
			entry.emission = to_lrt(_material_emission(first_material));
			entry.transform = to_lrt_transform(transform);
			entry.layer_mask = receiver.layer_mask;
			entry.axis_aligned = _is_axis_aligned(basis);
			// Volume-local AABB of the (possibly rotated) box, which is what the analytic backend
			// and the injection's box occlusion test consume.
			// Local AABB half extents: |basis| * box size / 2, computed per volume axis.
			const Vector3 box_size = box->get_size();
			Vector3 half;
			for (int row = 0; row < 3; row++) {
				half[row] = (Math::abs(basis[row][0]) * box_size.x +
									Math::abs(basis[row][1]) * box_size.y +
									Math::abs(basis[row][2]) * box_size.z) *
						0.5;
			}
			entry.world_min = to_lrt(transform.origin - half);
			entry.world_max = to_lrt(transform.origin + half);
			r_boxes.push_back(entry);
			continue;
		}
		LRTVolume::MeshInstance entry;
		entry.transform = to_lrt_transform(transform);
		entry.instanced = receiver.multimesh.is_valid() || receiver.particles;
		entry.raster_geometry = material_requires_raster_geometry(receiver.authored_overlay);
		for (int surface : receiver.contributing_surfaces) {
			entry.raster_geometry = entry.raster_geometry || material_requires_raster_geometry(receiver.materials[surface]);
		}
		entry.sdf_resolution = receiver.sdf_resolution;
		entry.layer_mask = receiver.layer_mask;
		const Transform3D capture_transform = receiver.global_transform;
		uint64_t capture_key = 0;
		capture_key = mix_signature(capture_key, mesh->get_rid().get_id());
		capture_key = mix_signature(capture_key, receiver.mesh_content_signature);
		capture_key = mix_signature(capture_key, receiver.deformation_signature);
		capture_key = mix_signature(capture_key, uint64_t(mesh->get_surface_count()));
		capture_key = mix_signature(capture_key, uint64_t(entry.sdf_resolution));
		capture_key = mix_signature(capture_key, receiver.material_signature);
		capture_key = mix_signature(capture_key, receiver.material_frame);
		capture_key = mix_signature(capture_key, receiver.material_view_signature);
		if (entry.raster_geometry) {
			capture_key = mix_signature(capture_key, Variant(capture_transform.basis).hash());
		}
		const auto cached = mesh_capture_cache.find(receiver.get_key());
		const uint64_t triangles_signature = mix_signature(receiver.mesh_content_signature, receiver.material_signature);
		if (!p_validate_mesh_content && cached != mesh_capture_cache.end() &&
				cached->second.mesh_revision_signature == receiver.mesh_content_signature) {
			mesh_content_signatures.emplace(mesh->get_rid().get_id(), cached->second.content_signature);
			if (entry.instanced && cached->second.triangles_signature == triangles_signature) {
				entry.triangles = cached->second.triangles;
				if (cached->second.particle_skin) {
					entry.particle_skin = *cached->second.particle_skin;
				}
			}
		}
		uint64_t content_signature = 0;
		bool cache_valid = cached != mesh_capture_cache.end() && cached->second.key == capture_key && cached->second.material != nullptr;
		if (cache_valid && p_validate_mesh_content) {
			content_signature = get_mesh_content_signature(mesh);
			cache_valid = cached->second.content_signature == content_signature;
		}
		if (cache_valid) {
			entry.triangles = cached->second.triangles;
			entry.material = cached->second.material;
			entry.raster_capture = cached->second.raster_capture;
			entry.copies = cached->second.copies;
			entry.instance_signature = cached->second.instance_signature;
			entry.material_signature = cached->second.material_signature;
		} else {
			const uint64_t stable_mesh_signature = get_mesh_content_signature(mesh);
			uint64_t shared_capture_key = mix_signature(geometry_capture_bounds_signature(receiver.instance_id), stable_mesh_signature);
			shared_capture_key = mix_signature(shared_capture_key, receiver.deformation_signature);
			shared_capture_key = mix_signature(shared_capture_key, receiver.material_frame);
			shared_capture_key = mix_signature(shared_capture_key, receiver.material_view_signature);
			if (entry.instanced) {
				shared_capture_key = mix_signature(shared_capture_key, receiver.render_instance.get_id());
				shared_capture_key = mix_signature(shared_capture_key, receiver.draw_pass);
			}
			shared_capture_key = mix_signature(shared_capture_key, uint64_t(entry.sdf_resolution));
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				shared_capture_key = mix_signature(shared_capture_key,
						_material_content_signature(receiver.materials[surface], false));
			}
			shared_capture_key = mix_signature(shared_capture_key, _material_content_signature(receiver.authored_overlay, false));
			List<PropertyInfo> instance_uniforms;
			RS::get_singleton()->instance_geometry_get_shader_parameter_list(receiver.render_instance, &instance_uniforms);
			for (const PropertyInfo &property : instance_uniforms) {
				shared_capture_key = mix_signature(shared_capture_key, property.name.hash());
				const Variant value = RS::get_singleton()->instance_geometry_get_shader_parameter(receiver.render_instance, property.name);
				shared_capture_key = mix_signature(shared_capture_key, value.hash());
			}
			for (int column = 0; column < 3; column++) {
				for (int row = 0; row < 3; row++) {
					shared_capture_key = mix_signature(shared_capture_key,
							quantized_signature_value(capture_transform.basis[row][column], 100000.0));
				}
			}
			shared_capture_key = mix_signature(shared_capture_key, quantized_signature_value(capture_transform.origin.x, 100000.0));
			shared_capture_key = mix_signature(shared_capture_key, quantized_signature_value(capture_transform.origin.y, 100000.0));
			shared_capture_key = mix_signature(shared_capture_key, quantized_signature_value(capture_transform.origin.z, 100000.0));
			const uint64_t shared_signature = shared_capture_key;
			if (receiver.deformation_signature != 0 || receiver.material_uses_time || receiver.material_uses_view) {
				// Keep one snapshot per deforming source and precision. Its full signature still
				// validates pose, materials and capture transform before another volume reuses it.
				shared_capture_key = mix_signature(receiver.render_instance.get_id(), uint64_t(entry.sdf_resolution));
				shared_capture_key = mix_signature(shared_capture_key, receiver.draw_pass);
			}
			auto shared = shared_mesh_capture_cache.find(shared_capture_key);
			bool shared_valid = shared != shared_mesh_capture_cache.end() && shared->second.shared_signature == shared_signature;
			if (shared_valid && p_validate_mesh_content) {
				content_signature = content_signature != 0 ? content_signature : get_mesh_content_signature(mesh);
				shared_valid = shared->second.content_signature == content_signature;
			}
			if (shared_valid) {
				shared->second.last_used = ++shared_mesh_capture_cache_clock;
				entry.triangles = shared->second.triangles;
				entry.material = shared->second.material;
				entry.raster_capture = shared->second.raster_capture;
				entry.copies = shared->second.copies;
				entry.instance_signature = shared->second.instance_signature;
				entry.material_signature = shared->second.material_signature;
			} else if (!_capture_mesh(receiver, capture_transform,
							  entry.sdf_resolution, receiver.contributing_surfaces, entry, r_error, p_async)) {
				return false;
			}
			// The call submits the capture synchronously; only its readback is asynchronous.
			// A queued global-uniform edit can be applied between collection and submission.
			// Keep displaying the captured frame, but do not cache it under the earlier key.
			const bool capture_cacheable = _material_revision_signature(receiver) == receiver.material_revision_signature;
			if (entry.material == nullptr) {
				PendingMaterialCapture &request = pending_material_captures.at(material_capture_id);
				request.mesh_index = int(r_meshes.size());
				request.cache_key = capture_cacheable ? capture_key : 0;
				request.shared_cache_key = shared_capture_key;
			}
			MeshCaptureCache cache;
			cache.mesh_revision_signature = receiver.mesh_content_signature;
			cache.triangles_signature = triangles_signature;
			cache.key = capture_cacheable ? capture_key : 0;
			cache.shared_signature = shared_signature;
			cache.content_signature = content_signature != 0 ? content_signature : get_mesh_content_signature(mesh);
			cache.triangles = entry.triangles;
			if (shared_valid) {
				cache.particle_skin = shared->second.particle_skin;
			} else if (cached != mesh_capture_cache.end() && cached->second.triangles == entry.triangles &&
					cached->second.triangles_signature == triangles_signature && cached->second.particle_skin) {
				cache.particle_skin = cached->second.particle_skin;
			} else if (!entry.particle_skin.empty()) {
				cache.particle_skin = std::make_shared<const std::vector<lrt::MeshTriangleSkin>>(entry.particle_skin);
			}
			cache.material = entry.material;
			cache.raster_capture = entry.raster_capture;
			cache.copies = entry.copies;
			cache.instance_signature = entry.instance_signature;
			cache.source_version = receiver.deformation_signature;
			cache.material_signature = entry.material_signature;
			cache.byte_size = mesh_capture_bytes(cache.triangles, cache.material, cache.copies, cache.raster_capture, cache.particle_skin);
			cache.last_used = ++shared_mesh_capture_cache_clock;
			mesh_capture_cache[receiver.get_key()] = cache;
			if (capture_cacheable && !shared_valid && cache.material != nullptr) {
				_store_shared_mesh_capture(shared_capture_key, cache);
			}
		}
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
		const bool decoded = _decode_material_capture(capture.images, capture.size, capture.transform,
				job->meshes[capture.mesh_index], job->capture_error);
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
	if (job->fingerprint_instances && job->cache_fingerprint != 0) {
		for (const LRTVolume::MeshInstance &mesh : job->meshes) {
			if (mesh.instanced) {
				job->cache_fingerprint = mix_signature(job->cache_fingerprint, mesh.instance_signature);
			}
		}
	}
	volume->solver->set_mesh_instances(job->meshes);
	if (!job->fingerprint_instances || (job->reasons & REBUILD_REASON_FORCED) || job->prepare_cached_assets ||
			!volume->solver->load_local_field_cache(job->cache_fingerprint, job->result)) {
		job->result = volume->solver->bake_local_field_data(job->analytic, job->prepare_cached_assets);
	}
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
	pending_operator_key = mix_signature(0, uint64_t(geometry_backend));
	solver->prepare_shared_gpu_resources();
	solver->set_geometry_snapshot_input_usec(OS::get_singleton()->get_ticks_usec());
	if (!solver->begin_apply_local_field(false)) {
		error_message = "LRT 持久化构建数据上传失败";
		editor_rebuild_requested = false;
		return false;
	}
	pending_apply_result = cached;
	pending_apply_generation = generation;
	pending_apply_reasons = REBUILD_REASON_FORCED;
	pending_apply_cache_fingerprint = p_fingerprint;
	local_apply_pending = true;
	if (geometry_backend == BACKEND_SDF) {
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
	const lrt::Grid requested_grid = lrt::make_grid_sized(effective_spacing,
			lrt::Vec3(-effective_size.x * 0.5, -effective_size.y * 0.5, -effective_size.z * 0.5),
			lrt::Vec3(effective_size.x, effective_size.y, effective_size.z));
	const uint64_t estimated_gpu_bytes = uint64_t(requested_grid.count) * 4096ull;
	const uint64_t gpu_limit_bytes = uint64_t(MAX(64, int(GLOBAL_GET("rendering/global_illumination/lrt/limits/max_volume_gpu_memory_mb")))) * 1024ull * 1024ull;
	if (estimated_gpu_bytes > gpu_limit_bytes) {
		error_message = vformat("LRT 预计需要 %.1f MiB GPU 内存，超过 Project Settings 中 %.1f MiB 的单 Volume 上限",
				double(estimated_gpu_bytes) / (1024.0 * 1024.0), double(gpu_limit_bytes) / (1024.0 * 1024.0));
		editor_rebuild_requested = false;
		return;
	}
	solver->prepare_shared_gpu_resources();
	std::vector<LRTVolume::BoxInstance> boxes;
	std::vector<LRTVolume::MeshInstance> meshes;
	String material_error;
	const uint64_t geometry_input_started_usec = OS::get_singleton()->get_ticks_usec();
	const bool geometry_input_ready = _build_geometry_inputs(boxes, meshes, (build_reasons & REBUILD_REASON_FORCED) != 0, material_error);
	const double geometry_input_ms = double(OS::get_singleton()->get_ticks_usec() - geometry_input_started_usec) / 1000.0;
	last_geometry_input_ms += geometry_input_ms;
	if (!geometry_input_ready) {
		error_message = material_error.is_empty() ? "LRT material capture failed." : material_error;
		pending_material_captures.clear();
		editor_rebuild_requested = false;
		return;
	}
	if (geometry_backend == BACKEND_ANALYTIC) {
		for (const LRTVolume::BoxInstance &box : boxes) {
			if (!box.axis_aligned) {
				error_message = "解析盒后端（原型 geometry-query.js 的 AABB 盒）不支持旋转的盒体";
				return;
			}
		}
		if (!meshes.empty()) {
			error_message = "解析盒后端只接受盒体接收器，请改用 Color SDF";
			editor_rebuild_requested = false;
			return;
		}
	}
	error_message = String();
	const Vector3 grid_min = -effective_size * 0.5;
	solver->configure_sized(effective_spacing, grid_min, effective_size);
	solver->set_multi_bounce(multi_bounce);
	solver->set_sh_visibility(visibility_mode == VISIBILITY_SH);
	solver->set_propagation_sampling(propagation_sampling);
	solver->set_box_instances(boxes);
	solver->clear_cancel();

	// A rebuild keeps the propagated field when the local grid and backend stay compatible; the
	// grid part is re-checked by the solver when it applies. Scene-tree parentage is irrelevant.
	uint64_t next_operator_key = 0;
	next_operator_key = mix_signature(next_operator_key, uint64_t(geometry_backend));
	pending_operator_key = next_operator_key;
	pending_preserve_history = has_applied_operator_key && next_operator_key == applied_operator_key;

	job = memnew(BuildJob);
	job->meshes = std::move(meshes);
	solver->set_geometry_snapshot_input_usec(geometry_input_started_usec);
	job->analytic = geometry_backend == BACKEND_ANALYTIC;
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
	if (!has_applied_configuration || editor_rebuild_requested) {
		for (const LRTVolume::MeshInstance &mesh : job->meshes) {
			if (mesh.instanced) {
				job->fingerprint_instances = true;
				job->cache_fingerprint = _build_cache_fingerprint(true);
				break;
			}
		}
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
		job->captures.push_back(request.second);
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
				cached->second.material = mesh.material;
				cached->second.raster_capture = mesh.raster_capture;
				cached->second.copies = mesh.copies;
				cached->second.instance_signature = mesh.instance_signature;
				cached->second.material_signature = mesh.material_signature;
				cached->second.byte_size = mesh_capture_bytes(mesh.triangles, mesh.material, mesh.copies, mesh.raster_capture, cached->second.particle_skin);
				cached->second.last_used = ++shared_mesh_capture_cache_clock;
				_store_shared_mesh_capture(capture.shared_cache_key, cached->second);
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
		error_message = "LRT 局部场上传失败";
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
		// Keep the previous probe source while the changed receiver layout is re-resolved.
		// Reweighting old unit fields against the new layout would publish an incoherent source.
		native_source_ready = false;
		if (!bool(applied.get("preserved_history", false))) {
			// A new grid has no prior source to retain. Its initialized empty unit fields
			// already form a coherent emission/sky source before the first light capture.
			native_source_ready = true;
			_inject_sources(false, false);
		}
	}
	applied_operator_key = pending_operator_key;
	has_applied_operator_key = true;
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
	applied["sdf_receiver_links_ms"] = result.sdf_receiver_links_ms;
	applied["sdf_transfer_ms"] = result.sdf_transfer_ms;
	applied["sdf_merge_ms"] = result.sdf_merge_ms;
	applied["visibility_ms"] = result.visibility_ms;
	applied["receiver_layout_ms"] = result.receiver_layout_ms;
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
	applied["sdf_ray_queries"] = int64_t(result.sdf_ray_queries);
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
	// Continuous edits publish only to the live field. Persisting the entire field on every
	// transform change stalls the editor; explicit builds and scene saves own disk publication.
	if (Engine::get_singleton()->is_editor_hint() && applied_build_cache_fingerprint != 0 &&
			(finished_reasons & REBUILD_REASON_FORCED) && !result.cache_loaded) {
		if (solver->store_local_field_cache(applied_build_cache_fingerprint)) {
			serialized_build_cache_fingerprint = applied_build_cache_fingerprint;
			build_data_missing = false;
		}
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
	const Ref<Texture2D> radiance_r = solver->get_texture("radiance_r");
	const Ref<Texture2D> radiance_g = solver->get_texture("radiance_g");
	const Ref<Texture2D> radiance_b = solver->get_texture("radiance_b");
	const Ref<Texture2D> sky_r = solver->get_texture("sky_r");
	const Ref<Texture2D> sky_g = solver->get_texture("sky_g");
	const Ref<Texture2D> sky_b = solver->get_texture("sky_b");
	const Ref<Texture2D> visibility = solver->get_texture("visibility");
	const Ref<Texture2D> material_field = solver->get_texture("material");
	const Ref<Texture2D> links = solver->get_texture("links");
	const Ref<Texture2D> receiver_links = solver->get_texture("receiver_links");
	const Ref<Texture2D> matrix_field = solver->get_texture("matrices");
	const Ref<Texture2D> source_r = solver->get_texture("source_r");
	const Ref<Texture2D> source_g = solver->get_texture("source_g");
	const Ref<Texture2D> source_b = solver->get_texture("source_b");
	const Ref<Texture2D> local_visibility = solver->get_texture("local_visibility");
	const Ref<Texture2D> diagnostic_sdf = solver->get_texture("diagnostic_sdf");
	const Ref<Texture2D> diagnostic_albedo = solver->get_texture("diagnostic_albedo");
	const Ref<Texture2D> diagnostic_emission = solver->get_texture("diagnostic_emission");
	const Ref<Texture2D> diagnostic_dirty = solver->get_texture("diagnostic_dirty");
	const Dictionary external_gi_buffers = solver->get_external_gi_buffers();
	const Transform3D world_to_volume = get_global_transform().affine_inverse();
	Dictionary native_state;
	native_state["owner"] = uint64_t(get_instance_id());
	native_state["world_to_volume"] = world_to_volume;
	// The scene cull instance transform does not carry an authored directional light's
	// rotation, so the Volume shadow camera is oriented from the node instead.
	Transform3D directional_light_transform;
	bool has_directional_light = false;
	for (const LightEntry &entry : lights) {
		Light3D *mapped_light = light_from_id(entry.light_id);
		if (mapped_light == nullptr || !entry.visible || !mapped_light->has_shadow()) {
			continue;
		}
		if (Object::cast_to<DirectionalLight3D>(mapped_light) == nullptr) {
			continue;
		}
		directional_light_transform = mapped_light->get_global_transform();
		has_directional_light = true;
		break;
	}
	native_state["directional_light_transform"] = directional_light_transform;
	native_state["has_directional_light"] = has_directional_light;
	const Vector3 effective_size = _effective_volume_size();
	native_state["volume_min"] = -effective_size * 0.5;
	native_state["volume_max"] = effective_size * 0.5;
	native_state["grid_min"] = grid_min;
	native_state["grid_size"] = size;
	native_state["spacing"] = grid_spacing;
	native_state["atlas_size"] = atlas;
	native_state["environment"] = environment.is_valid() ? environment->get_rid() : RID();
	native_state["blur_sampling"] = blur_sampling;
	native_state["blend_distance"] = blend_distance;
	native_state["priority"] = priority;
	native_state["scenario"] = get_world_3d()->get_scenario();
	PackedInt32Array tree_order;
	for (const Node *node = this; node->get_parent() != nullptr; node = node->get_parent()) {
		tree_order.insert(0, node->get_index());
	}
	native_state["tree_order"] = tree_order;
	native_state["display_blend_enabled"] = blend_distance > 0.0;
	native_state["external_gi_enabled"] = _is_external_gi_active();
	native_state["enabled"] = _is_active() && transform_valid;
	native_state["radiance_r"] = radiance_r.is_valid() ? radiance_r->get_rid() : RID();
	native_state["radiance_g"] = radiance_g.is_valid() ? radiance_g->get_rid() : RID();
	native_state["radiance_b"] = radiance_b.is_valid() ? radiance_b->get_rid() : RID();
	native_state["visibility"] = visibility.is_valid() ? visibility->get_rid() : RID();
	native_state["material"] = material_field.is_valid() ? material_field->get_rid() : RID();
	native_state["links"] = links.is_valid() ? links->get_rid() : RID();
	native_state["receiver_links"] = receiver_links.is_valid() ? receiver_links->get_rid() : RID();
	native_state["sky_r"] = sky_r.is_valid() ? sky_r->get_rid() : RID();
	native_state["sky_g"] = sky_g.is_valid() ? sky_g->get_rid() : RID();
	native_state["sky_b"] = sky_b.is_valid() ? sky_b->get_rid() : RID();
	native_state["source_r"] = source_r.is_valid() ? source_r->get_rid() : RID();
	native_state["source_g"] = source_g.is_valid() ? source_g->get_rid() : RID();
	native_state["source_b"] = source_b.is_valid() ? source_b->get_rid() : RID();
	native_state["local_visibility"] = local_visibility.is_valid() ? local_visibility->get_rid() : RID();
	native_state["matrices"] = matrix_field.is_valid() ? matrix_field->get_rid() : RID();
	native_state["diagnostic_sdf"] = diagnostic_sdf.is_valid() ? diagnostic_sdf->get_rid() : RID();
	native_state["diagnostic_albedo"] = diagnostic_albedo.is_valid() ? diagnostic_albedo->get_rid() : RID();
	native_state["diagnostic_emission"] = diagnostic_emission.is_valid() ? diagnostic_emission->get_rid() : RID();
	native_state["diagnostic_dirty"] = diagnostic_dirty.is_valid() ? diagnostic_dirty->get_rid() : RID();
	native_state["external_gi_r"] = external_gi_buffers.get("r", RID());
	native_state["external_gi_g"] = external_gi_buffers.get("g", RID());
	native_state["external_gi_b"] = external_gi_buffers.get("b", RID());
	native_state["solver"] = solver;
	native_state["volume_shadow_requested"] = true;
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

PackedVector3Array LRTVolume3D::_environment_samples() {
	PackedVector3Array result;
	result.resize(LRTVolume::SKY_DIRECTION_COUNT);
	result.fill(Vector3());
	_refresh_environment_cache();
	if (cached_sky_panorama.is_null()) {
		return result;
	}
	// The boundary directions live in Volume space. Undo the authored sky rotation after
	// moving them to world space to sample the unrotated baked panorama.
	const Basis local_to_sky = Basis::from_euler(environment->get_sky_rotation()).inverse() * get_global_transform().basis;
	return solver->sample_panorama_radiance(cached_sky_panorama, local_to_sky);
}

// Re-runs the source pass on the existing local field and restarts propagation. Never
// rebuilds the geometry: that is [method _start_build].
void LRTVolume3D::_inject_sources(bool p_restart, bool p_count) {
	if (solver.is_null() || !solver->has_local_field() || (!native_source_ready && !p_restart)) {
		return;
	}
	light_inputs = _cached_mapped_lights();
	light_photometry_signature = _light_photometry_signature();
	has_light_photometry_signature = true;
	sky_radiance = _environment_radiance();
	sky_samples = _environment_samples();
	// Missing native-light ranges stay zero, so a new layout can inject emission and sky now and
	// then incorporate each persistent unit-light range as it becomes available.
	solver->set_sky_radiance(sky_radiance);
	solver->set_sky_samples(sky_samples);
	solver->inject();
	if (p_count) {
		source_injections++;
	}
	if (p_restart) {
		// Prototype src/lab.js reset(): only an operator change (visibility mode) or an explicit
		// reset throws the propagated field away. A new source term alone does not.
		solver->reset();
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
	if (external_gi_enabled && LRTRenderBridge::get_external_gi_status(get_instance_id()) == LRTRenderBridge::EXTERNAL_GI_SKY_INCOMPATIBLE) {
		warnings.push_back(RTR("The external diffuse GI provider includes sky radiance and cannot supply sky-free boundary input. External injection is inactive; LRT owns its sky illumination independently."));
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
			if (propagation_volumes.empty()) {
				RS::get_singleton()->connect("frame_pre_draw", callable_mp_static(&LRTVolume3D::_begin_propagation_frame));
				propagation_allocated_frame = UINT64_MAX;
				geometry_budget_frame = UINT64_MAX;
				geometry_budget_round = 0;
			}
			propagation_volumes[get_instance_id()] = this;
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
			propagation_volumes.erase(get_instance_id());
			propagation_polled_volumes.erase(get_instance_id());
			if (propagation_volumes.empty()) {
				RS::get_singleton()->disconnect("frame_pre_draw", callable_mp_static(&LRTVolume3D::_begin_propagation_frame));
			}
			propagation_budget_credit_ms = 0.0;
			propagation_granted_iterations = 0;
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
				if (fingerprint != serialized_build_cache_fingerprint) {
					bool stored = false;
					if (!building && !local_apply_pending && !rebuild_pending && solver.is_valid() &&
							has_geometry_signature && geometry_signature == _geometry_signature() &&
							has_material_state_signature && material_state_signature == _material_state_signature()) {
						stored = solver->store_local_field_cache(fingerprint);
					} else {
						// Save the authored pose even if its preview build is still in flight. The
						// isolated preparation path already captures exactly this input for export.
						const Dictionary prepared = prepare_export_data();
						stored = bool(prepared.get("ok", false));
					}
					if (stored) {
						serialized_build_cache_fingerprint = fingerprint;
						build_data_missing = false;
					} else {
						ERR_PRINT("Cannot persist the current LRT Volume field while saving the scene.");
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
	// A repeated participant also starts the next update cycle when no viewport draws.
	// This keeps offscreen process ticks and explicit poll() batches advancing together.
	if (propagation_polled_volumes.find(get_instance_id()) != propagation_polled_volumes.end()) {
		_begin_propagation_frame();
	}
	propagation_polled_volumes.insert(get_instance_id());
	const uint64_t frame_started_usec = OS::get_singleton()->get_ticks_usec();
	const double display_delta = display_update_usec != 0 ? double(frame_started_usec - display_update_usec) / 1000000.0 : 0.0;
	display_update_usec = frame_started_usec;
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
		const Viewport *viewport = get_viewport();
		const Viewport::DebugDraw debug_draw = viewport != nullptr ? viewport->get_debug_draw() : Viewport::DEBUG_DRAW_DISABLED;
		bool needs_debug = debug_draw >= Viewport::DEBUG_DRAW_LRT_RADIANCE_PROBES && debug_draw <= Viewport::DEBUG_DRAW_LRT_UPDATE_REGIONS;
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
				needs_debug = needs_debug || (mode >= Viewport::DEBUG_DRAW_LRT_RADIANCE_PROBES && mode <= Viewport::DEBUG_DRAW_LRT_UPDATE_REGIONS);
			}
		}
#endif
		if (solver->set_local_debug_textures_enabled(_is_active() && needs_debug)) {
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
		error_message = "LRT Volume 不允许自身或父级缩放；请用 volume_size 调整体积范围";
		last_frame_work_ms = double(OS::get_singleton()->get_ticks_usec() - frame_started_usec) / 1000.0;
		peak_frame_work_ms = MAX(peak_frame_work_ms, last_frame_work_ms);
		return;
	}
	if (error_message.begins_with("LRT Volume 不允许")) {
		error_message = String();
	}
	uint64_t segment_started_usec = OS::get_singleton()->get_ticks_usec();
	const int dynamic_update_interval = CLAMP(int(GLOBAL_GET("rendering/global_illumination/lrt/dynamic_objects/update_interval")), 1, 8);
	const bool geometry_update_due = !has_geometry_signature || scheduler_frame % uint64_t(dynamic_update_interval) == 0 ||
			(rebuild_pending && !building && !local_apply_pending && !rebuild_suppressed);
	const bool refresh_dynamic_objects = geometry_update_due && _take_geometry_budget();
	double geometry_update_ms = 0.0;
	bool loaded_editor_cache = false;
	if (refresh_dynamic_objects) {
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
	}
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
	if (refresh_dynamic_objects) {
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
	}
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
	if (refresh_dynamic_objects) {
		// Retire completed work before starting the newest collected snapshot. Otherwise every
		// publication leaves the worker idle until another admitted geometry update.
		segment_started_usec = OS::get_singleton()->get_ticks_usec();
		_start_build();
		geometry_update_ms += double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
		_finish_geometry_update(geometry_update_ms);
	}
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
		const PackedVector4Array next_sky_radiance = _environment_radiance();
		const PackedVector3Array next_sky_samples = _environment_samples();
		last_sky_input_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
		if (next_sky_radiance != sky_radiance || next_sky_samples != sky_samples) {
			// Sky is independent of the raster-light capture and can refresh immediately.
			_inject_sources(false);
			_apply_display();
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
		// Source injection and light capture are independent of propagation. A new local field starts
		// from emission, sky and the current coherent light fields; completed snapshots blend in before
		// the propagation work queued by the same frame.
		const int convergence_iterations = _convergence_iterations();
		if (!paused && convergence_iterations > 0 && solver->get_pending_step_iterations() == 0) {
			segment_started_usec = OS::get_singleton()->get_ticks_usec();
			const int scheduled_iterations = _take_propagation_budget();
			if (scheduled_iterations > 0 && native_capture_pending) {
				solver->step_radiance_only(scheduled_iterations);
			} else if (scheduled_iterations > 0) {
				solver->step(scheduled_iterations);
			}
			last_frame_propagation_iterations = scheduled_iterations;
			_update_display_parameters();
			last_propagation_schedule_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
		}
	}
	// Incremental geometry uploads use an inactive grid-buffer bank. Once that bank exists, the
	// previous coherent field can keep propagating while upload and capture data finish; the final
	// render-thread callback switches all geometry descriptors together.
	if (local_apply_pending && error_message.is_empty() && solver.is_valid() && solver->can_step_while_applying() &&
			!paused && solver->get_pending_step_iterations() == 0) {
		segment_started_usec = OS::get_singleton()->get_ticks_usec();
		const int scheduled_iterations = _take_propagation_budget();
		if (scheduled_iterations > 0 && native_capture_pending) {
			solver->step_radiance_only(scheduled_iterations);
		} else if (scheduled_iterations > 0) {
			solver->step(scheduled_iterations);
		}
		last_frame_propagation_iterations = scheduled_iterations;
		_update_display_parameters();
		last_propagation_schedule_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	}
	if (!paused && error_message.is_empty() && solver.is_valid() && solver->has_local_field()) {
		// Display evolution must not depend on bake completions or the propagation budget.
		solver->advance_display(display_delta,
				double(GLOBAL_GET("rendering/global_illumination/lrt/propagation/display_response_time")));
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
