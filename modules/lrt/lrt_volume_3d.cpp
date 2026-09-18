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
#include "scene/3d/mesh_instance_3d.h"
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

MeshInstance3D *mesh_from_id(ObjectID p_id) {
	return Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(p_id));
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

std::map<uint64_t, LRTVolume3D::MeshCaptureCache> LRTVolume3D::shared_mesh_capture_cache;
uint64_t LRTVolume3D::shared_mesh_capture_cache_bytes = 0;
uint64_t LRTVolume3D::shared_mesh_capture_cache_clock = 0;

LRTVolume3D::LRTVolume3D() {
	set_process(false);
}

LRTVolume3D::~LRTVolume3D() {
	_cancel_build();
}

void LRTVolume3D::_bind_methods() {
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
	ClassDB::bind_method(D_METHOD("set_update_budget_ms", "budget_ms"), &LRTVolume3D::set_update_budget_ms);
	ClassDB::bind_method(D_METHOD("get_update_budget_ms"), &LRTVolume3D::get_update_budget_ms);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_budget_ms", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_update_budget_ms", "get_update_budget_ms");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blur_sampling", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_blur_sampling", "is_blur_sampling");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_sampling", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_propagation_sampling", "get_propagation_sampling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "external_gi_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_external_gi_enabled", "is_external_gi_enabled");
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

void LRTVolume3D::set_instance_sdf_resolution(MeshInstance3D *p_instance, int p_resolution) {
	ERR_FAIL_NULL(p_instance);
	if (p_resolution <= 0) {
		p_instance->remove_meta(INSTANCE_SDF_RESOLUTION_META);
	} else {
		p_instance->set_meta(INSTANCE_SDF_RESOLUTION_META, MAX(8, p_resolution));
	}
	_request_rebuild(REBUILD_REASON_GEOMETRY);
}

int LRTVolume3D::get_instance_sdf_resolution(MeshInstance3D *p_instance) const {
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

void LRTVolume3D::set_update_budget_ms(double p_budget_ms) {
	update_budget_ms = MAX(0.1, p_budget_ms);
}

double LRTVolume3D::get_update_budget_ms() const {
	return update_budget_ms;
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
	// Kept as a script-only compatibility hook. Production ownership follows Environment.
	(void)p_enabled;
}

bool LRTVolume3D::is_external_gi_enabled() const {
	return environment.is_valid() && environment->is_dynamic_gi_enabled();
}

void LRTVolume3D::set_display_blend_enabled(bool p_enabled) {
	set_blend_distance(p_enabled ? (blend_distance > 0.0 ? blend_distance : 0.5) : 0.0);
}

bool LRTVolume3D::is_display_blend_enabled() const {
	return blend_distance > 0.0;
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
	if (!_is_active()) {
		return;
	}
	_collect_geometry();
	_collect_lights();
	geometry_signature = _geometry_signature();
	material_state_signature = _material_state_signature();
	has_geometry_signature = true;
	has_material_state_signature = true;
	_queue_build(p_reasons);
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
	if (!rebuild_suppressed && rebuild_pending) {
		_start_build();
	}
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
	return text;
}

String LRTVolume3D::get_instance_sdf_status(MeshInstance3D *p_instance) const {
	ERR_FAIL_NULL_V(p_instance, "Failed");
	if (get_instance_sdf_resolution(p_instance) <= 0) {
		return "Inherited";
	}
	if (!error_message.is_empty()) {
		return "Failed";
	}
	if (building || local_apply_pending || rebuild_pending || build_stats.is_empty()) {
		return "Building";
	}
	for (const Receiver &receiver : receivers) {
		if (receiver.instance_id == p_instance->get_instance_id() && receiver.contributes) {
			return "Ready";
		}
	}
	return "Building";
}

bool LRTVolume3D::is_building() const {
	return building || local_apply_pending;
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
	rendering_server->call_on_render_thread(callable_mp_static(&LRTRenderBridge::read_volume_shadow_depth));
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
	const bool native_update_pending = native_capture_pending ||
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
	status["native_light_capture_queued"] = native_capture_queued;
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
	status["lrt_flag_commands"] = int64_t(lrt_flag_commands);
	status["native_shadow_signature"] = int64_t(shadow_capture_signature);
	status["update_budget_ms"] = update_budget_ms;
	status["propagation_response_frames"] = int(GLOBAL_GET("rendering/global_illumination/lrt/propagation/response_frames"));
	status["scheduler_frame"] = int64_t(scheduler_frame);
	status["last_frame_work_ms"] = last_frame_work_ms;
	status["peak_frame_work_ms"] = peak_frame_work_ms;
	Dictionary frame_cpu_breakdown;
	frame_cpu_breakdown["collect_geometry_ms"] = last_collect_geometry_ms;
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
	status["external_gi_requested"] = environment.is_valid() && environment->is_dynamic_gi_enabled();
	status["external_gi_active"] = _is_external_gi_active();
	status["external_gi_capture_valid"] = LRTRenderBridge::is_external_gi_capture_valid(get_instance_id());
	status["external_gi_capture_count"] = int64_t(LRTRenderBridge::get_external_gi_capture_count(get_instance_id()));
	status["environment_capture_pending"] = environment_capture_pending;
	status["environment_capture_submitted"] = environment_capture_submitted;
	status["external_gi_path"] = "hddagi_diffuse_boundary_sh2";
	status["external_gi_writeback"] = false;
	status["external_gi_trace_queries"] = 0;
	status["render_bridge_performance"] = LRTRenderBridge::get_performance_stats(get_instance_id());
	uint64_t active_mesh_capture_bytes = 0;
	std::set<const void *> active_mesh_capture_allocations;
	for (const auto &entry : mesh_capture_cache) {
		const void *allocation = entry.second.triangles.get();
		if (allocation != nullptr && active_mesh_capture_allocations.insert(allocation).second) {
			active_mesh_capture_bytes += entry.second.byte_size;
		}
	}
	status["mesh_capture_active_bytes"] = int64_t(active_mesh_capture_bytes);
	status["mesh_capture_shared_cache_bytes"] = int64_t(shared_mesh_capture_cache_bytes);
	status["mesh_capture_shared_cache_budget_bytes"] = int64_t(SHARED_MESH_CAPTURE_BUDGET_BYTES);
	status["mesh_capture_shared_cache_entries"] = int64_t(shared_mesh_capture_cache.size());
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
		if (!receiver.material_error.is_empty()) {
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
	result["external_gi_path"] = "hddagi_diffuse_boundary_sh2";
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
	return is_visible_in_tree() && transform_valid && environment.is_valid() &&
			environment->is_dynamic_gi_enabled() && !environment->is_dynamic_gi_reading_sky_light();
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

bool LRTVolume3D::_intersects_volume(MeshInstance3D *p_instance) const {
	if (p_instance == nullptr || p_instance->get_mesh().is_null()) {
		return false;
	}
	const Transform3D to_volume = relative_node_transform(this, p_instance);
	const AABB local_bounds = to_volume.xform(p_instance->get_mesh()->get_aabb());
	const Vector3 effective_size = _effective_volume_size();
	return AABB(-effective_size * 0.5, effective_size).intersects(local_bounds);
}

// --- Scene inputs ----------------------------------------------------------

static uint64_t mesh_content_signature(const Ref<Mesh> &p_mesh);

void LRTVolume3D::_mark_scene_candidates_dirty() {
	scene_candidates_dirty = true;
}

void LRTVolume3D::_refresh_scene_candidates() {
	if (!scene_candidates_dirty) {
		return;
	}
	geometry_candidates.clear();
	light_candidates.clear();
	Node *root = _scene_tree_root();
	if (root != nullptr) {
		const TypedArray<Node> meshes = root->find_children("*", "MeshInstance3D", true, false);
		geometry_candidates.reserve(meshes.size());
		for (int i = 0; i < meshes.size(); i++) {
			MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(meshes[i]);
			if (mesh != nullptr) {
				geometry_candidates.push_back(mesh->get_instance_id());
			}
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
// unrelated nodes. GI_MODE_STATIC contributes and receives, GI_MODE_DYNAMIC only receives.
void LRTVolume3D::_collect_geometry() {
	_refresh_scene_candidates();
	for (auto &dependency : material_dependencies) {
		dependency.second.used = false;
	}
	std::vector<Receiver> next;
	std::map<ObjectID, uint64_t> material_signatures;
	std::map<ObjectID, const Receiver *> previous_by_id;
	std::set<ObjectID> next_ids;
	std::set<ObjectID> contributing_ids;
	for (const Receiver &existing : receivers) {
		previous_by_id[existing.instance_id] = &existing;
	}
	next.reserve(receivers.size());
	{
		for (const ObjectID candidate_id : geometry_candidates) {
			MeshInstance3D *mesh_instance = mesh_from_id(candidate_id);
			if (mesh_instance == nullptr || mesh_instance->get_world_3d() != get_world_3d()) {
				continue;
			}
			if (!mesh_instance->is_visible_in_tree() || mesh_instance->get_mesh().is_null() ||
					mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_DISABLED || !_intersects_volume(mesh_instance)) {
				continue;
			}
			Receiver entry;
			entry.instance_id = mesh_instance->get_instance_id();
			entry.authored_overlay = mesh_instance->get_material_overlay();
			const Ref<Mesh> mesh = mesh_instance->get_mesh();
			entry.mesh_content_signature = mix_signature(mesh->get_rid().get_id(), mesh->get_edited_version());
			entry.mesh_content_signature = mix_signature(entry.mesh_content_signature, uint64_t(mesh->get_surface_count()));
			entry.material_revision_signature = _material_revision_signature(mesh_instance, entry.authored_overlay);
			const auto previous_entry = previous_by_id.find(entry.instance_id);
			const Receiver *previous = previous_entry != previous_by_id.end() ? previous_entry->second : nullptr;
			if (previous != nullptr && previous->material_revision_signature == entry.material_revision_signature) {
				entry.albedo = previous->albedo;
				entry.material_signature = previous->material_signature;
				entry.material_error = previous->material_error;
			} else {
				entry.albedo = _surface_albedo(mesh_instance);
				entry.material_signature = _material_signature(mesh_instance, entry.authored_overlay, material_signatures);
				for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
					entry.material_error = _material_support_error(_surface_material(mesh_instance, surface));
					if (!entry.material_error.is_empty()) {
						break;
					}
				}
				if (entry.material_error.is_empty()) {
					entry.material_error = _material_support_error(entry.authored_overlay);
				}
			}
			entry.contributes = mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && entry.material_error.is_empty();
			if (previous != nullptr && previous->contributes && entry.contributes &&
					previous->mesh_content_signature != entry.mesh_content_signature) {
				stale_mesh_content_receivers.insert(entry.instance_id);
			}
			const bool use_lrt = stale_mesh_content_receivers.find(entry.instance_id) == stale_mesh_content_receivers.end();
			const bool was_using_lrt = lrt_enabled_receivers.find(entry.instance_id) != lrt_enabled_receivers.end();
			if (use_lrt != was_using_lrt) {
				RS::get_singleton()->instance_geometry_set_flag(mesh_instance->get_instance(), RSE::INSTANCE_FLAG_USE_LRT, use_lrt);
				lrt_flag_commands++;
				if (use_lrt) {
					lrt_enabled_receivers.insert(entry.instance_id);
				} else {
					lrt_enabled_receivers.erase(entry.instance_id);
				}
			}
			next_ids.insert(entry.instance_id);
			if (entry.contributes) {
				contributing_ids.insert(entry.instance_id);
			}
			next.push_back(entry);
		}
	}
	bool collection_changed = next.size() != receivers.size();
	if (!collection_changed) {
		for (size_t i = 0; i < next.size(); i++) {
			if (next[i].instance_id != receivers[i].instance_id || next[i].contributes != receivers[i].contributes ||
					next[i].albedo != receivers[i].albedo || next[i].mesh_content_signature != receivers[i].mesh_content_signature ||
					next[i].material_revision_signature != receivers[i].material_revision_signature ||
					next[i].material_signature != receivers[i].material_signature ||
					next[i].material_error != receivers[i].material_error) {
				collection_changed = true;
				break;
			}
		}
	}
	// Receivers that left the volume stop selecting the native LRT path.
	for (const Receiver &existing : receivers) {
		const bool present = next_ids.find(existing.instance_id) != next_ids.end();
		MeshInstance3D *mesh_instance = mesh_from_id(existing.instance_id);
		if (!present && mesh_instance != nullptr) {
			if (lrt_enabled_receivers.erase(existing.instance_id) > 0) {
				RS::get_singleton()->instance_geometry_set_flag(mesh_instance->get_instance(), RSE::INSTANCE_FLAG_USE_LRT, false);
				lrt_flag_commands++;
			}
		}
		if (!present) {
			stale_mesh_content_receivers.erase(existing.instance_id);
		}
	}
	receivers = next;
	for (auto cache = mesh_capture_cache.begin(); cache != mesh_capture_cache.end();) {
		if (contributing_ids.find(cache->first) != contributing_ids.end()) {
			++cache;
		} else {
			cache = mesh_capture_cache.erase(cache);
		}
	}
	display_collection_dirty = display_collection_dirty || collection_changed;
	_release_material_dependencies(false);
}

void LRTVolume3D::_collect_lights() {
	_refresh_scene_candidates();
	std::vector<LightEntry> next;
	std::map<ObjectID, const LightEntry *> previous_by_id;
	for (const LightEntry &existing : lights) {
		previous_by_id[existing.light_id] = &existing;
	}
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
			entry.visible = light->is_visible();
			entry.written_visible = entry.visible;
			const auto previous = previous_by_id.find(entry.light_id);
			if (previous != previous_by_id.end()) {
				entry.visible = previous->second->visible;
				entry.written_visible = previous->second->written_visible;
			}
			next.push_back(entry);
		}
	}
	lights = next;
}

// The engine's own material of one surface: override, then mesh material, exactly as the
// renderer resolves it for the direct term the overlay is added to.
Ref<Material> LRTVolume3D::_surface_material(MeshInstance3D *p_instance, int p_surface) {
	return p_instance->get_active_material(p_surface);
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

Vector3 LRTVolume3D::_surface_albedo(MeshInstance3D *p_instance) {
	Ref<Mesh> mesh = p_instance->get_mesh();
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

static bool shader_uses_word(const String &p_code, const String &p_word) {
	int offset = 0;
	while ((offset = p_code.find(p_word, offset)) >= 0) {
		const int end = offset + p_word.length();
		const bool left = offset == 0 || !(p_code[offset - 1] == '_' || is_ascii_alphanumeric_char(p_code[offset - 1]));
		const bool right = end == p_code.length() || !(p_code[end] == '_' || is_ascii_alphanumeric_char(p_code[end]));
		if (left && right) {
			return true;
		}
		offset = end;
	}
	return false;
}

String LRTVolume3D::_material_support_error(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return String();
	}
	Ref<StandardMaterial3D> standard = p_material;
	if (standard.is_valid()) {
		if (standard->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED) {
			return "透明与 Alpha Mask 材质尚未进入 LRT 静态捕获范围";
		}
		return String();
	}
	Ref<ShaderMaterial> shader_material = p_material;
	if (shader_material.is_null() || shader_material->get_shader().is_null()) {
		return "LRT 仅支持不透明 StandardMaterial3D 与静态 ShaderMaterial";
	}
	const String code = shader_material->get_shader()->get_code();
	static const char *unsupported[] = {
		"TIME",
		"VIEW",
		"VIEW_MATRIX",
		"INV_VIEW_MATRIX",
		"PROJECTION_MATRIX",
		"INV_PROJECTION_MATRIX",
		"SCREEN_UV",
		"SCREEN_TEXTURE",
		"DEPTH_TEXTURE",
		"NORMAL_ROUGHNESS_TEXTURE",
		"FRAGCOORD",
		"CAMERA_POSITION_WORLD",
		"CAMERA_DIRECTION_WORLD",
		"MODEL_MATRIX",
		"MODEL_NORMAL_MATRIX",
		"NODE_POSITION_WORLD",
		"NODE_POSITION_VIEW",
		"EYE_OFFSET",
		"VIEW_INDEX",
		"discard",
		"ALPHA",
	};
	for (const char *word : unsupported) {
		if (shader_uses_word(code, word)) {
			return vformat("LRT 静态材质捕获不支持 shader 输入或操作：%s", word);
		}
	}
	return String();
}

uint64_t LRTVolume3D::_material_dependency_revision(const Ref<Resource> &p_resource) {
	if (p_resource.is_null()) {
		return 0;
	}
	const ObjectID id = p_resource->get_instance_id();
	auto found = material_dependencies.find(id);
	if (found == material_dependencies.end()) {
		MaterialDependency dependency;
		dependency.resource = p_resource;
		found = material_dependencies.emplace(id, dependency).first;
		p_resource->connect("changed", callable_mp(this, &LRTVolume3D::_material_dependency_changed).bind(id));
	}
	found->second.used = true;
	return found->second.revision;
}

void LRTVolume3D::_material_dependency_changed(ObjectID p_id) {
	const auto found = material_dependencies.find(p_id);
	if (found != material_dependencies.end()) {
		found->second.revision++;
	}
}

void LRTVolume3D::_release_material_dependencies(bool p_all) {
	for (auto dependency = material_dependencies.begin(); dependency != material_dependencies.end();) {
		if (!p_all && dependency->second.used) {
			++dependency;
			continue;
		}
		dependency->second.resource->disconnect("changed",
				callable_mp(this, &LRTVolume3D::_material_dependency_changed).bind(dependency->first));
		dependency = material_dependencies.erase(dependency);
	}
}

uint64_t LRTVolume3D::_material_resource_signature(const Ref<Material> &p_material) {
	uint64_t state = 0;
	if (p_material.is_null()) {
		return state;
	}
	auto hash_value = [this, &state](const Variant &p_value) {
		state = mix_signature(state, p_value.hash());
		if (p_value.get_type() == Variant::OBJECT) {
			Ref<Resource> resource = p_value;
			if (resource.is_valid()) {
				state = mix_signature(state, resource->get_rid().get_id());
				state = mix_signature(state, resource->get_edited_version());
				state = mix_signature(state, _material_dependency_revision(resource));
			}
		}
	};
	state = mix_signature(state, p_material->get_rid().get_id());
	Ref<BaseMaterial3D> base_material = p_material;
	if (base_material.is_valid()) {
		state = mix_signature(state, base_material->get_parameter_change_version());
		for (int texture_index = 0; texture_index < BaseMaterial3D::TEXTURE_MAX; texture_index++) {
			hash_value(base_material->get_texture(BaseMaterial3D::TextureParam(texture_index)));
		}
		return state;
	}
	state = mix_signature(state, p_material->get_edited_version());
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
		state = mix_signature(state, shader->get_edited_version());
		List<PropertyInfo> uniforms;
		shader->get_shader_uniform_list(&uniforms);
		for (const PropertyInfo &property : uniforms) {
			hash_value(shader_material->get_shader_parameter(property.name));
		}
	}
	return state;
}

uint64_t LRTVolume3D::_material_content_signature(const Ref<Material> &p_material) const {
	if (p_material.is_null()) {
		return 0;
	}
	uint64_t state = mix_signature(0, p_material->get_class_name().hash());
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
		if (value.get_type() != Variant::OBJECT) {
			state = mix_signature(state, value.hash());
			continue;
		}
		Ref<Resource> resource = value;
		if (resource.is_null()) {
			state = mix_signature(state, 0);
			continue;
		}
		state = mix_signature(state, resource->get_class_name().hash());
		const String path = resource->get_path();
		const String scene_id = resource->get_scene_unique_id();
		uint64_t identity = resource->get_rid().get_id();
		if (!path.is_empty()) {
			identity = path.hash();
		} else if (!scene_id.is_empty()) {
			identity = scene_id.hash();
		}
		state = mix_signature(state, identity);
		state = mix_signature(state, resource->get_edited_version());
		Ref<Texture2D> texture = resource;
		if (texture.is_valid()) {
			const Ref<Image> image = texture->get_image();
			if (image.is_valid()) {
				state = mix_signature(state, uint64_t(image->get_width()));
				state = mix_signature(state, uint64_t(image->get_height()));
				state = mix_signature(state, uint64_t(image->get_format()));
				state = mix_signature(state, Variant(image->get_data()).hash());
			}
		}
		Ref<Shader> shader = resource;
		if (shader.is_valid()) {
			state = mix_signature(state, shader->get_code().hash());
		}
	}
	return state;
}

uint64_t LRTVolume3D::_material_signature(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay,
		std::map<ObjectID, uint64_t> &r_material_signatures) {
	uint64_t state = 0;
	Ref<Mesh> mesh = p_instance->get_mesh();
	if (mesh.is_null()) {
		return state;
	}
	auto hash_value = [&state](const Variant &p_value) {
		state = mix_signature(state, p_value.hash());
		if (p_value.get_type() == Variant::OBJECT) {
			Ref<Resource> resource = p_value;
			if (resource.is_valid()) {
				state = mix_signature(state, resource->get_rid().get_id());
				state = mix_signature(state, resource->get_edited_version());
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
		hash_material(_surface_material(p_instance, surface));
	}
	hash_material(p_authored_overlay);
	List<PropertyInfo> instance_uniforms;
	RS::get_singleton()->instance_geometry_get_shader_parameter_list(p_instance->get_instance(), &instance_uniforms);
	for (const PropertyInfo &property : instance_uniforms) {
		const Variant value = p_instance->get_instance_shader_parameter(property.name);
		hash_value(value);
	}
	return state;
}

uint64_t LRTVolume3D::_material_revision_signature(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay) {
	Ref<Mesh> mesh = p_instance->get_mesh();
	if (mesh.is_null()) {
		return 0;
	}
	uint64_t state = mix_signature(mesh->get_rid().get_id(), mesh->get_edited_version());
	bool uses_shader_material = false;
	auto hash_material_revision = [this, &state, &uses_shader_material](const Ref<Material> &p_material) {
		if (p_material.is_null()) {
			state = mix_signature(state, 0);
			return;
		}
		state = mix_signature(state, uint64_t(p_material->get_instance_id()));
		Ref<BaseMaterial3D> base_material = p_material;
		if (base_material.is_valid()) {
			state = mix_signature(state, base_material->get_parameter_change_version());
			for (int index = 0; index < BaseMaterial3D::TEXTURE_MAX; index++) {
				state = mix_signature(state, _material_dependency_revision(base_material->get_texture(BaseMaterial3D::TextureParam(index))));
			}
		}
		Ref<ShaderMaterial> shader_material = p_material;
		if (shader_material.is_valid()) {
			state = mix_signature(state, _material_resource_signature(p_material));
			uses_shader_material = true;
		}
	};
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		hash_material_revision(_surface_material(p_instance, surface));
	}
	hash_material_revision(p_authored_overlay);
	if (uses_shader_material) {
		List<PropertyInfo> instance_uniforms;
		RS::get_singleton()->instance_geometry_get_shader_parameter_list(p_instance->get_instance(), &instance_uniforms);
		for (const PropertyInfo &property : instance_uniforms) {
			state = mix_signature(state, p_instance->get_instance_shader_parameter(property.name).hash());
		}
	}
	return state;
}

int LRTVolume3D::_effective_sdf_resolution(MeshInstance3D *p_instance) const {
	const int instance_override = get_instance_sdf_resolution(p_instance);
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
		MeshInstance3D *mesh_instance = mesh_from_id(receiver.instance_id);
		if (mesh_instance == nullptr) {
			continue;
		}
		const Transform3D transform = canonical_volume_transform(relative_node_transform(this, mesh_instance));
		state = mix_signature(state, uint64_t(receiver.instance_id));
		state = mix_signature(state, uint64_t(_effective_sdf_resolution(mesh_instance)));
		state = mix_signature(state, uint64_t(mesh_instance->get_layer_mask()));
		state = mix_signature(state, quantized_signature_value(transform.origin.x, 10000.0));
		state = mix_signature(state, quantized_signature_value(transform.origin.y, 10000.0));
		state = mix_signature(state, quantized_signature_value(transform.origin.z, 10000.0));
		for (int column = 0; column < 3; column++) {
			for (int row = 0; row < 3; row++) {
				state = mix_signature(state, quantized_signature_value(transform.basis[row][column], 1000000.0));
			}
		}
		Ref<Mesh> mesh = mesh_instance->get_mesh();
		state = mix_signature(state, mesh.is_valid() ? mesh->get_rid().get_id() : 0);
		state = mix_signature(state, mesh.is_valid() ? mesh->get_edited_version() : 0);
		state = mix_signature(state, mesh.is_valid() ? uint64_t(mesh->get_surface_count()) : 0);
	}
	return state;
}

uint64_t LRTVolume3D::_build_cache_fingerprint() const {
	uint64_t state = mix_signature(0, 5); // Persistent local-cache algorithm version.
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
		MeshInstance3D *mesh_instance = mesh_from_id(receiver.instance_id);
		if (mesh_instance == nullptr || mesh_instance->get_mesh().is_null()) {
			continue;
		}
		const Transform3D transform = canonical_volume_transform(relative_node_transform(this, mesh_instance));
		for (int axis = 0; axis < 3; axis++) {
			state = mix_signature(state, quantized_signature_value(transform.origin[axis], 10000.0));
			for (int column = 0; column < 3; column++) {
				state = mix_signature(state, quantized_signature_value(transform.basis[axis][column], 1000000.0));
			}
		}
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		state = mix_signature(state, mesh_content_signature(mesh));
		state = mix_signature(state, uint64_t(_effective_sdf_resolution(mesh_instance)));
		state = mix_signature(state, uint64_t(mesh_instance->get_layer_mask()));
		for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
			state = mix_signature(state, _material_content_signature(_surface_material(mesh_instance, surface)));
		}
		state = mix_signature(state, _material_content_signature(receiver.authored_overlay));
		List<PropertyInfo> instance_uniforms;
		RS::get_singleton()->instance_geometry_get_shader_parameter_list(mesh_instance->get_instance(), &instance_uniforms);
		for (const PropertyInfo &property : instance_uniforms) {
			state = mix_signature(state, property.name.hash());
			state = mix_signature(state, mesh_instance->get_instance_shader_parameter(property.name).hash());
		}
	}
	return state;
}

uint64_t LRTVolume3D::_material_state_signature() const {
	uint64_t state = 0;
	for (const Receiver &receiver : receivers) {
		if (!receiver.contributes) {
			continue;
		}
		state = mix_signature(state, uint64_t(receiver.instance_id));
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
		// Only the user's own visibility feeds the solver: the direct-light display modes
		// switch the engine's lights off, and that must not look like an input change (it
		// would re-inject the source term and throw the propagated iterations away).
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
	if (solver.is_null() || !solver->has_local_field()) {
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

uint64_t LRTVolume3D::_shadow_inputs_signature(uint64_t *r_resource_signature) const {
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
		mix_resource(projector.is_valid() ? uint64_t(projector->get_edited_version()) : 0);
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
			mix_resource(area_texture.is_valid() ? uint64_t(area_texture->get_edited_version()) : 0);
		}
	}
	for (const ObjectID candidate_id : geometry_candidates) {
		MeshInstance3D *mesh_instance = mesh_from_id(candidate_id);
		if (mesh_instance == nullptr || mesh_instance->get_world_3d() != get_world_3d() ||
				!mesh_instance->is_visible_in_tree() ||
				mesh_instance->get_cast_shadows_setting() == GeometryInstance3D::SHADOW_CASTING_SETTING_OFF) {
			continue;
		}
		Ref<Mesh> mesh = mesh_instance->get_mesh();
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
		mix_resource(uint64_t(mesh->get_edited_version()));
		Ref<Material> material_override = mesh_instance->get_material_override();
		if (material_override.is_valid()) {
			mix_resource(uint64_t(material_override->get_instance_id()));
			mix_resource(uint64_t(material_override->get_edited_version()));
		}
		for (int surface = 0; surface < mesh_instance->get_surface_override_material_count(); surface++) {
			Ref<Material> material = mesh_instance->get_surface_override_material(surface);
			if (material.is_valid()) {
				mix_resource(uint64_t(material->get_instance_id()));
				mix_resource(uint64_t(material->get_edited_version()));
			}
		}
	}
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
	bool published_source = false;
	for (const NativeLightSnapshot &snapshot : native_light_snapshots) {
		// A direct resolve publishes a complete unit field through a same-frame GPU dependency, so
		// the newest snapshot becomes the source immediately instead of fading in.
		solver->commit_native_light_unit_field(snapshot.light_slot, uint64_t(snapshot.source_id), snapshot.input_usec);
		published_source = true;
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
	if (published_source) {
		// Same-frame publish: the resolve was submitted earlier in this frame, and the injection is
		// queued behind it on the render thread. A newer request that is already waiting must not
		// delay the field this batch already committed.
		_inject_sources(false, false);
	}
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

bool LRTVolume3D::_capture_mesh(MeshInstance3D *p_instance, const Ref<Material> &p_authored_overlay,
		const Transform3D &p_transform, int p_resolution,
		LRTVolume::MeshInstance &r_mesh, String &r_error) const {
	std::shared_ptr<std::vector<lrt::MeshTriangle>> triangles = std::make_shared<std::vector<lrt::MeshTriangle>>();
	Ref<Mesh> source_mesh = p_instance->get_mesh();
	for (int surface = 0; surface < source_mesh->get_surface_count(); surface++) {
		if (source_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		Array arrays = source_mesh->surface_get_arrays(surface);
		if (arrays.size() < Mesh::ARRAY_MAX || arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::PACKED_VECTOR3_ARRAY) {
			continue;
		}
		const PackedVector3Array points = arrays[Mesh::ARRAY_VERTEX];
		PackedInt32Array indices;
		if (arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
			indices = arrays[Mesh::ARRAY_INDEX];
		}
		const int index_count = indices.is_empty() ? points.size() : indices.size();
		for (int index = 0; index + 2 < index_count; index += 3) {
			lrt::MeshTriangle triangle;
			for (int vertex = 0; vertex < 3; vertex++) {
				const int source = indices.is_empty() ? index + vertex : indices[index + vertex];
				triangle.position[vertex] = to_lrt(points[source]);
				triangle.color[vertex] = lrt::Vec3(1.0, 1.0, 1.0);
			}
			triangles->push_back(triangle);
		}
	}
	if (triangles->empty()) {
		r_error = "LRT 材质捕获找不到三角形表面";
		return false;
	}
	r_mesh.triangles = triangles;
	const AABB mesh_bounds = source_mesh->get_aabb();
	const double longest = mesh_bounds.get_longest_axis_size();
	if (longest <= 0.0) {
		r_error = "LRT 材质捕获的 Mesh 边界为空";
		return false;
	}
	const AABB local_capture_bounds = mesh_bounds.grow(longest / double(MAX(8, p_resolution)));
	const AABB world_capture_bounds = p_transform.xform(local_capture_bounds);
	const double world_longest = world_capture_bounds.get_longest_axis_size();
	const int material_resolution = MAX(16, p_resolution / 4);
	Vector3i capture_size;
	for (int axis = 0; axis < 3; axis++) {
		capture_size[axis] = CLAMP(int(Math::ceil(world_capture_bounds.size[axis] / world_longest * material_resolution)) + 1, 4, 128);
	}
	BaseMaterial3D::flush_changes();
	const Ref<Material> displayed_overlay = p_instance->get_material_overlay();
	RS::get_singleton()->instance_set_transform(p_instance->get_instance(), p_transform);
	RS::get_singleton()->instance_geometry_set_material_overlay(p_instance->get_instance(),
			p_authored_overlay.is_valid() ? p_authored_overlay->get_rid() : RID());
	const Dictionary images = RS::get_singleton()->bake_render_material_volume(
			p_instance->get_instance(), world_capture_bounds, capture_size);
	RS::get_singleton()->instance_geometry_set_material_overlay(p_instance->get_instance(),
			displayed_overlay.is_valid() ? displayed_overlay->get_rid() : RID());
	if (images.is_empty() || Vector3i(images.get("size", Vector3i())) != capture_size) {
		r_error = "LRT 三维材质捕获失败；当前路径要求 D3D12 Forward+";
		return false;
	}
	const PackedByteArray albedo_data = images.get("albedo", PackedByteArray());
	const PackedByteArray emission_data = images.get("emission", PackedByteArray());
	const PackedByteArray emission_aniso_data = images.get("emission_aniso", PackedByteArray());
	const PackedByteArray normal_bits_data = images.get("normal_bits", PackedByteArray());
	const int capture_count = capture_size.x * capture_size.y * capture_size.z;
	const Vector3i render_size = capture_size * 2;
	const int render_count = render_size.x * render_size.y * render_size.z;
	if (albedo_data.size() != capture_count * 6 * 2 || emission_data.size() != capture_count * 4 ||
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
	r_mesh.material = material;
	r_mesh.material_signature = lrt::material_field_input_signature(*r_mesh.triangles, material.get());
	return true;
}

static bool standard_material_is_constant(const Ref<Material> &p_material) {
	if (p_material.is_null()) {
		return true;
	}
	Ref<StandardMaterial3D> standard = p_material;
	return standard.is_valid() && !standard->get_feature(BaseMaterial3D::FEATURE_EMISSION) &&
			!standard->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR) &&
			standard->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_null() &&
			standard->get_texture(BaseMaterial3D::TEXTURE_DETAIL_ALBEDO).is_null();
}

static uint64_t mesh_capture_bytes(const std::shared_ptr<const std::vector<lrt::MeshTriangle>> &p_triangles,
		const std::shared_ptr<const lrt::MaterialCapture> &p_material) {
	uint64_t bytes = p_triangles != nullptr ? uint64_t(p_triangles->capacity()) * sizeof(lrt::MeshTriangle) : 0;
	if (p_material != nullptr) {
		bytes += uint64_t(p_material->albedo.capacity() + p_material->emission.capacity()) * sizeof(float);
		bytes += uint64_t(p_material->occupied.capacity()) * sizeof(uint8_t);
	}
	return bytes;
}

void LRTVolume3D::clear_shared_mesh_capture_cache() {
	shared_mesh_capture_cache.clear();
	shared_mesh_capture_cache_bytes = 0;
}


// Every SDF input keeps asset-local geometry. Its full affine basis is sampled in volume space,
// so rotations and non-uniform scales never force a world-space copy of the distance field.
bool LRTVolume3D::_build_geometry_inputs(std::vector<LRTVolume::BoxInstance> &r_boxes,
		std::vector<LRTVolume::MeshInstance> &r_meshes, bool p_validate_mesh_content, String &r_error) {
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
		MeshInstance3D *mesh_instance = mesh_from_id(receiver.instance_id);
		if (mesh_instance == nullptr) {
			continue;
		}
		Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_null()) {
			continue;
		}
		const Transform3D transform = canonical_volume_transform(relative_node_transform(this, mesh_instance));
		const Basis basis = transform.basis;
		BoxMesh *box = Object::cast_to<BoxMesh>(mesh.ptr());
		const Ref<Material> first_material = mesh->get_surface_count() > 0 ? _surface_material(mesh_instance, 0) : Ref<Material>();
		if (box != nullptr && receiver.authored_overlay.is_null() && standard_material_is_constant(first_material)) {
			LRTVolume::BoxInstance entry;
			entry.local_extent = to_lrt(box->get_size());
			entry.color = to_lrt(receiver.albedo);
			entry.emission = to_lrt(_material_emission(first_material));
			entry.transform = to_lrt_transform(transform);
			entry.layer_mask = mesh_instance->get_layer_mask();
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
		entry.sdf_resolution = _effective_sdf_resolution(mesh_instance);
		entry.layer_mask = mesh_instance->get_layer_mask();
		const Transform3D capture_transform = mesh_instance->get_global_transform();
		uint64_t capture_key = 0;
		capture_key = mix_signature(capture_key, mesh->get_rid().get_id());
		capture_key = mix_signature(capture_key, mesh->get_edited_version());
		capture_key = mix_signature(capture_key, uint64_t(mesh->get_surface_count()));
		capture_key = mix_signature(capture_key, uint64_t(entry.sdf_resolution));
		capture_key = mix_signature(capture_key, receiver.material_signature);
		const auto cached = mesh_capture_cache.find(receiver.instance_id);
		uint64_t content_signature = 0;
		bool cache_valid = cached != mesh_capture_cache.end() && cached->second.key == capture_key;
		if (cache_valid && p_validate_mesh_content) {
			content_signature = get_mesh_content_signature(mesh);
			cache_valid = cached->second.content_signature == content_signature;
		}
		if (cache_valid) {
			entry.triangles = cached->second.triangles;
			entry.material = cached->second.material;
			entry.material_signature = cached->second.material_signature;
		} else {
			const uint64_t stable_mesh_signature = get_mesh_content_signature(mesh);
			uint64_t shared_capture_key = mix_signature(0, stable_mesh_signature);
			shared_capture_key = mix_signature(shared_capture_key, uint64_t(entry.sdf_resolution));
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				shared_capture_key = mix_signature(shared_capture_key,
						_material_content_signature(_surface_material(mesh_instance, surface)));
			}
			shared_capture_key = mix_signature(shared_capture_key, _material_content_signature(receiver.authored_overlay));
			List<PropertyInfo> instance_uniforms;
			RS::get_singleton()->instance_geometry_get_shader_parameter_list(mesh_instance->get_instance(), &instance_uniforms);
			for (const PropertyInfo &property : instance_uniforms) {
				shared_capture_key = mix_signature(shared_capture_key, property.name.hash());
				const Variant value = mesh_instance->get_instance_shader_parameter(property.name);
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
			auto shared = shared_mesh_capture_cache.find(shared_capture_key);
			bool shared_valid = shared != shared_mesh_capture_cache.end();
			if (shared_valid && p_validate_mesh_content) {
				content_signature = content_signature != 0 ? content_signature : get_mesh_content_signature(mesh);
				shared_valid = shared->second.content_signature == content_signature;
			}
			if (shared_valid) {
				shared->second.last_used = ++shared_mesh_capture_cache_clock;
				entry.triangles = shared->second.triangles;
				entry.material = shared->second.material;
				entry.material_signature = shared->second.material_signature;
			} else if (!_capture_mesh(mesh_instance, receiver.authored_overlay, capture_transform,
							  entry.sdf_resolution, entry, r_error)) {
				return false;
			}
			MeshCaptureCache cache;
			cache.key = capture_key;
			cache.content_signature = content_signature != 0 ? content_signature : get_mesh_content_signature(mesh);
			cache.triangles = entry.triangles;
			cache.material = entry.material;
			cache.material_signature = entry.material_signature;
			cache.byte_size = mesh_capture_bytes(cache.triangles, cache.material);
			cache.last_used = ++shared_mesh_capture_cache_clock;
			mesh_capture_cache[receiver.instance_id] = cache;
			if (!shared_valid && cache.byte_size <= SHARED_MESH_CAPTURE_BUDGET_BYTES) {
				if (shared != shared_mesh_capture_cache.end()) {
					shared_mesh_capture_cache_bytes -= shared->second.byte_size;
					shared_mesh_capture_cache.erase(shared);
				}
				while (!shared_mesh_capture_cache.empty() &&
						shared_mesh_capture_cache_bytes + cache.byte_size > SHARED_MESH_CAPTURE_BUDGET_BYTES) {
					auto least_recent = shared_mesh_capture_cache.begin();
					for (auto candidate = shared_mesh_capture_cache.begin(); candidate != shared_mesh_capture_cache.end(); ++candidate) {
						if (candidate->second.last_used < least_recent->second.last_used) {
							least_recent = candidate;
						}
					}
					shared_mesh_capture_cache_bytes -= least_recent->second.byte_size;
					shared_mesh_capture_cache.erase(least_recent);
				}
				shared_mesh_capture_cache_bytes += cache.byte_size;
				shared_mesh_capture_cache[shared_capture_key] = std::move(cache);
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
	const double queue_wait_ms = double(OS::get_singleton()->get_ticks_usec() - job->queued_usec) / 1000.0;
	job->result = volume->solver->bake_local_field_data(job->analytic);
	job->result.queue_wait_ms = queue_wait_ms;
	job->result.geometry_input_ms = job->geometry_input_ms;
	job->done_usec = OS::get_singleton()->get_ticks_usec();
	job->result.worker_total_ms = double(job->done_usec - worker_started_usec) / 1000.0;
	job->done.store(true);
}

// Cancels the running bake and lets it finish before anything else touches the solver: the
// bake only reads plain data, so it stops at the next slice and the wait is short.
void LRTVolume3D::_cancel_build() {
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
	rebuild_pending = true;
	pending_rebuild_reasons |= p_reasons;
	pending_build_queued_usec = OS::get_singleton()->get_ticks_usec();
	if (!building && !rebuild_suppressed) {
		_start_build();
	}
}

bool LRTVolume3D::_try_load_editor_cache(uint64_t p_fingerprint) {
	if (!Engine::get_singleton()->is_editor_hint() || p_fingerprint == 0 ||
			p_fingerprint == last_cache_lookup_fingerprint || building || local_apply_pending) {
		return false;
	}
	last_cache_lookup_fingerprint = p_fingerprint;
	if (solver.is_null()) {
		solver.instantiate();
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
	solver->prepare_shared_gpu_resources();
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
	return true;
}

void LRTVolume3D::_start_build() {
	if (building || local_apply_pending || rebuild_suppressed || !rebuild_pending) {
		return;
	}
	const uint32_t build_reasons = pending_rebuild_reasons;
	active_build_queued_usec = pending_build_queued_usec;
	rebuild_pending = false;
	pending_rebuild_reasons = REBUILD_REASON_NONE;
	if (solver.is_null()) {
		solver.instantiate();
	}
	solver->prepare_shared_gpu_resources();
	std::vector<LRTVolume::BoxInstance> boxes;
	std::vector<LRTVolume::MeshInstance> meshes;
	String material_error;
	const uint64_t geometry_input_started_usec = OS::get_singleton()->get_ticks_usec();
	if (!_build_geometry_inputs(boxes, meshes, (build_reasons & REBUILD_REASON_FORCED) != 0, material_error)) {
		error_message = material_error.is_empty() ? "LRT 无法捕获静态材质" : material_error;
		editor_rebuild_requested = false;
		return;
	}
	const double geometry_input_ms = double(OS::get_singleton()->get_ticks_usec() - geometry_input_started_usec) / 1000.0;
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
	const Vector3 grid_min = -effective_size * 0.5;
	solver->configure_sized(effective_spacing, grid_min, effective_size);
	solver->set_multi_bounce(multi_bounce);
	solver->set_sh_visibility(visibility_mode == VISIBILITY_SH);
	solver->set_propagation_sampling(propagation_sampling);
	solver->set_box_instances(boxes);
	solver->set_mesh_instances(meshes);
	solver->clear_cancel();

	// A rebuild keeps the propagated field when the local grid and backend stay compatible; the
	// grid part is re-checked by the solver when it applies. Scene-tree parentage is irrelevant.
	uint64_t next_operator_key = 0;
	next_operator_key = mix_signature(next_operator_key, uint64_t(geometry_backend));
	pending_operator_key = next_operator_key;
	pending_preserve_history = has_applied_operator_key && next_operator_key == applied_operator_key;

	job = memnew(BuildJob);
	job->analytic = geometry_backend == BACKEND_ANALYTIC;
	job->generation = generation;
	job->reasons = build_reasons;
	job->queued_usec = OS::get_singleton()->get_ticks_usec();
	job->geometry_input_ms = geometry_input_ms;
	job->cache_fingerprint = Engine::get_singleton()->is_editor_hint() && (!editor_build_dirty || editor_rebuild_requested) ?
			_build_cache_fingerprint() : 0;
	active_rebuild_reasons = build_reasons;
	building = true;
	build_start_frame = scheduler_frame;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	task_id = pool->add_native_task(&LRTVolume3D::_bake_task, this, false, "LRT local field bake");
}

void LRTVolume3D::_poll_build() {
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
			_start_build();
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
	// Receiver offsets belong to the applied local field. Let its coherent light snapshot finish
	// before replacing that layout; the queued build already holds the latest geometry and can be
	// applied on the next frame without cancelling every in-flight capture during continuous motion.
	if (native_capture_pending) {
		SubViewport *viewport = Object::cast_to<SubViewport>(get_viewport());
		if (viewport == nullptr || viewport->get_update_mode() != SubViewport::UPDATE_DISABLED) {
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
	memdelete(finished);
	active_rebuild_reasons = REBUILD_REASON_NONE;
	if (result.cancelled) {
		_start_build();
		return;
	}
	if (finished_generation <= applied_generation) {
		// Jobs are launched serially, but keep the version guard at the application boundary:
		// an older result can never replace a state already shown by a newer generation.
		dropped_builds++;
		_start_build();
		return;
	}
	if (!result.ok) {
		editor_rebuild_requested = false;
		if (result.needs_axis_aligned) {
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
		_start_build();
		return;
	}
	const uint64_t apply_begin_started_usec = OS::get_singleton()->get_ticks_usec();
	if (result.unchanged) {
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
		_start_build();
		return;
	}
	last_apply_begin_ms = double(OS::get_singleton()->get_ticks_usec() - apply_begin_started_usec) / 1000.0;
	pending_apply_result = result;
	pending_apply_generation = finished_generation;
	pending_apply_reasons = finished_reasons;
	pending_apply_cache_fingerprint = finished_cache_fingerprint;
	// Finish the upload in this frame. The upload is a sparse patch plus small buffer copies, and
	// leaving it pending would push the publish one frame past the edit, which the dynamic-geometry
	// response gate measures against the frame that detected the change.
	Dictionary applied = solver->finish_apply_local_field();
	if (applied.is_empty()) {
		if (solver->is_apply_pending()) {
			// The upload still needs a render-thread pass (a headless harness that never draws, or a
			// patch too large for one submission). Let the frame loop finish it as before.
			local_apply_pending = true;
			return;
		}
		error_message = "LRT 局部场上传失败";
		editor_rebuild_requested = false;
		_start_build();
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
	if (!bool(applied.get("preserved_history", false))) {
		// A new grid first propagates emission and sky. Native lights join the source as soon as
		// their first coherent GPU capture snapshot becomes available.
		native_source_ready = false;
	}
	applied_operator_key = pending_operator_key;
	has_applied_operator_key = true;
	applied_generation = finished_generation;
	applied_rebuild_reasons = finished_reasons;
	build_apply_frame = scheduler_frame;
	applied["generation"] = finished_generation;
	applied["rebuild_reasons"] = int64_t(finished_reasons);
	applied["build_ms"] = result.build_ms;
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
	applied["visibility_ms"] = result.visibility_ms;
	applied["receiver_layout_ms"] = result.receiver_layout_ms;
	applied["queue_wait_ms"] = result.queue_wait_ms;
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
	geometry_builds++;
	if (Engine::get_singleton()->is_editor_hint() && editor_rebuild_requested) {
		applied_volume_size = volume_size;
		applied_spacing = spacing;
		has_applied_configuration = true;
		editor_build_dirty = false;
		editor_rebuild_requested = false;
	}
	applied_build_cache_fingerprint = pending_apply_cache_fingerprint;
	if (Engine::get_singleton()->is_editor_hint() && applied_build_cache_fingerprint != 0) {
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
	stale_mesh_content_receivers.clear();
	_apply_display();
	applied["display_apply_ms"] = double(OS::get_singleton()->get_ticks_usec() - finish_segment_started_usec) / 1000.0;
	display_collection_dirty = false;
	finish_segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_start_build();
	applied["next_build_start_ms"] = double(OS::get_singleton()->get_ticks_usec() - finish_segment_started_usec) / 1000.0;
	build_stats = applied;
}

// --- Display ---------------------------------------------------------------

void LRTVolume3D::_update_display_parameters() {
	// The render bridge is process-wide and single-owner. An inactive Volume must
	// release only if it currently owns the bind; writing enabled=false under its
	// own id would steal the bind from whichever Volume is actually displaying.
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
	const Dictionary debug_resources = solver->get_debug_resources();
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
	native_state["receiver_buffer"] = debug_resources.get("receiver_buffer", RID());
	native_state["receiver_count"] = debug_resources.get("receiver_count", 0);
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
	for (const Receiver &receiver : receivers) {
		MeshInstance3D *mesh_instance = mesh_from_id(receiver.instance_id);
		if (mesh_instance == nullptr) {
			continue;
		}
		const bool use_lrt = _is_active() && transform_valid && stale_mesh_content_receivers.find(receiver.instance_id) == stale_mesh_content_receivers.end();
		const bool was_using_lrt = lrt_enabled_receivers.find(receiver.instance_id) != lrt_enabled_receivers.end();
		if (use_lrt != was_using_lrt) {
			RS::get_singleton()->instance_geometry_set_flag(mesh_instance->get_instance(), RSE::INSTANCE_FLAG_USE_LRT, use_lrt);
			lrt_flag_commands++;
			if (use_lrt) {
				lrt_enabled_receivers.insert(receiver.instance_id);
			} else {
				lrt_enabled_receivers.erase(receiver.instance_id);
			}
		}
	}
	for (LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light == nullptr) {
			continue;
		}
		light->set_visible(entry.visible);
		entry.written_visible = entry.visible;
	}
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
	external_gi_environment_state = -1;
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
	if (solver.is_null() || !solver->has_local_field()) {
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
	if (!_has_valid_volume_transform()) {
		warnings.push_back(RTR("The LRT volume cannot be scaled, including through a parent. Keep its effective scale at (1, 1, 1) and edit volume_size instead."));
	}
	if (environment.is_valid() && environment->is_dynamic_gi_enabled() &&
			environment->is_dynamic_gi_reading_sky_light()) {
		warnings.push_back(RTR("External Dynamic GI injection requires Environment.dynamic_gi_read_sky_light to be disabled. LRT owns sky injection separately; enabling both would inject sky energy twice."));
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
			_release_material_dependencies(true);
			SceneTree *tree = get_tree();
			const Callable candidate_callback = callable_mp(this, &LRTVolume3D::_mark_scene_candidates_dirty);
			if (tree != nullptr && tree->is_connected("tree_changed", candidate_callback)) {
				tree->disconnect("tree_changed", candidate_callback);
			}
			geometry_candidates.clear();
			light_candidates.clear();
			scene_candidates_dirty = true;
			_cancel_build();
			_clear_native_receiver();
			native_capture_pending = false;
			native_capture_queued = false;
			native_capture_queued_usec = 0;
			native_light_field_set_signature = 0;
			// Everything the node wrote into the scene goes back to its authored value.
			for (const Receiver &receiver : receivers) {
				MeshInstance3D *mesh_instance = mesh_from_id(receiver.instance_id);
				if (mesh_instance != nullptr) {
					RS::get_singleton()->instance_geometry_set_flag(mesh_instance->get_instance(), RSE::INSTANCE_FLAG_USE_LRT, false);
				}
			}
			stale_mesh_content_receivers.clear();
			lrt_enabled_receivers.clear();
			for (const LightEntry &entry : lights) {
				Light3D *light = light_from_id(entry.light_id);
				if (light != nullptr && light->is_visible() != entry.visible) {
					light->set_visible(entry.visible);
				}
			}
		} break;
		case NOTIFICATION_EDITOR_PRE_SAVE: {
			// A save must never store preview light visibility or display tonemapping.
			for (const LightEntry &entry : lights) {
				Light3D *light = light_from_id(entry.light_id);
				if (light != nullptr && light->is_visible() != entry.visible) {
					light->set_visible(entry.visible);
				}
			}
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
	// The user owns light visibility; only a change this node did not write counts.
	for (LightEntry &entry : lights) {
		Light3D *light = light_from_id(entry.light_id);
		if (light != nullptr && light->is_visible() != entry.written_visible) {
			entry.visible = light->is_visible();
		}
	}
	uint64_t segment_started_usec = OS::get_singleton()->get_ticks_usec();
	const int dynamic_update_interval = CLAMP(int(GLOBAL_GET("rendering/global_illumination/lrt/dynamic_objects/update_interval")), 1, 8);
	const bool refresh_dynamic_objects = !has_geometry_signature || scheduler_frame % uint64_t(dynamic_update_interval) == 0;
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
				loaded_editor_cache = _try_load_editor_cache(_build_cache_fingerprint());
			}
		}
	}
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_collect_lights();
	last_collect_lights_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_refresh_environment();
	const int next_external_gi_environment_state = environment.is_valid() ?
			(int(environment->is_dynamic_gi_enabled()) | (int(environment->is_dynamic_gi_reading_sky_light()) << 1)) : 0;
	if (next_external_gi_environment_state != external_gi_environment_state) {
		external_gi_environment_state = next_external_gi_environment_state;
		update_configuration_warnings();
		_update_display_parameters();
	}
	const Transform3D current_display_transform = get_global_transform();
	if (!has_display_transform || current_display_transform != display_transform) {
		display_transform = current_display_transform;
		has_display_transform = true;
		_update_display_parameters();
	}
	last_environment_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
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
	segment_started_usec = OS::get_singleton()->get_ticks_usec();
	_poll_build();
	last_build_poll_ms = double(OS::get_singleton()->get_ticks_usec() - segment_started_usec) / 1000.0;
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
			int scheduled_iterations = convergence_iterations;
			const double measured_gpu_ms = solver->get_scheduler_gpu_ms();
			const int timed_iterations = solver->get_scheduler_gpu_work_items();
			if (measured_gpu_ms > 0.0 && timed_iterations > 0) {
				const double per_iteration_ms = measured_gpu_ms / timed_iterations;
				scheduled_iterations = CLAMP(int(update_budget_ms / per_iteration_ms), 1, convergence_iterations);
			}
			if (native_capture_pending) {
				solver->step_radiance_only(scheduled_iterations);
			} else {
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
		const int convergence_iterations = _convergence_iterations();
		int scheduled_iterations = convergence_iterations;
		const Dictionary solver_stats = solver->get_stats();
		const double measured_gpu_ms = solver_stats.get("last_gpu_ms", 0.0);
		const int timed_iterations = solver_stats.get("last_gpu_work_items", 0);
		if (measured_gpu_ms > 0.0 && timed_iterations > 0) {
			const double per_iteration_ms = measured_gpu_ms / timed_iterations;
			scheduled_iterations = CLAMP(int(update_budget_ms / per_iteration_ms), 1, convergence_iterations);
		}
		if (native_capture_pending) {
			solver->step_radiance_only(scheduled_iterations);
		} else {
			solver->step(scheduled_iterations);
		}
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
