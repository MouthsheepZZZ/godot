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

#include "core/object/class_db.h"
#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/gui/color_rect.h"
#include "scene/main/canvas_layer.h"
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
#include "servers/rendering/rendering_server.h"

namespace {

constexpr double METALLIC_THRESHOLD = 0.5;
// The prototype's light `power` is the irradiance scale the source term multiplies into the
// scattered radiance, and the engine's non-physical light units need PI before the diffuse
// BRDF divides by it again (light_storage.cpp multiplies by PI, light_compute divides).
constexpr double LIGHT_INTENSITY_SCALE = 3.14159265358979323846;
// Godot's own inverse-square attenuation (`omni_attenuation == 2`), the engine parameter
// form of the prototype's `power / max(d^2, 0.04)` falloff.
constexpr double INVERSE_SQUARE_ATTENUATION = 2.0;
// Surfaces at or above this metallic value carry no diffuse term in the prototype.
constexpr double DEFAULT_ALBEDO[3] = { 0.72, 0.72, 0.68 };
// Prototype display curve is x/(1+x); Godot's Reinhard with a large white value reproduces
// it to within ~2% below 20x.
constexpr double PROTOTYPE_TONEMAP_WHITE = 128.0;
constexpr int SKY_PANORAMA_WIDTH = 64;
constexpr int SKY_PANORAMA_HEIGHT = 32;
// JSON-style base properties every resource has; skipped when hashing a sky material.
const char *const BASE_RESOURCE_PROPERTIES[] = {
	"resource_local_to_scene", "resource_path", "resource_name", "script"
};

lrt::Vec3 to_lrt(const Vector3 &p_value) {
	return lrt::Vec3(p_value.x, p_value.y, p_value.z);
}

lrt::PrimitiveTransform to_lrt_transform(const Transform3D &p_transform, double p_scale) {
	lrt::PrimitiveTransform result;
	result.origin = to_lrt(p_transform.origin);
	const Basis basis = p_transform.basis;
	result.basis_x = to_lrt(basis.get_column(0).normalized());
	result.basis_y = to_lrt(basis.get_column(1).normalized());
	result.basis_z = to_lrt(basis.get_column(2).normalized());
	result.scale = p_scale;
	return result;
}

} // namespace

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
	ClassDB::bind_method(D_METHOD("set_expand_to_geometry", "expand"), &LRTVolume3D::set_expand_to_geometry);
	ClassDB::bind_method(D_METHOD("is_expanded_to_geometry"), &LRTVolume3D::is_expanded_to_geometry);
	ClassDB::bind_method(D_METHOD("set_geometry_backend", "backend"), &LRTVolume3D::set_geometry_backend);
	ClassDB::bind_method(D_METHOD("get_geometry_backend"), &LRTVolume3D::get_geometry_backend);
	ClassDB::bind_method(D_METHOD("set_visibility_mode", "mode"), &LRTVolume3D::set_visibility_mode);
	ClassDB::bind_method(D_METHOD("get_visibility_mode"), &LRTVolume3D::get_visibility_mode);
	ClassDB::bind_method(D_METHOD("set_mesh_sdf_resolution", "resolution"), &LRTVolume3D::set_mesh_sdf_resolution);
	ClassDB::bind_method(D_METHOD("get_mesh_sdf_resolution"), &LRTVolume3D::get_mesh_sdf_resolution);
	ClassDB::bind_method(D_METHOD("set_multi_bounce", "enabled"), &LRTVolume3D::set_multi_bounce);
	ClassDB::bind_method(D_METHOD("is_multi_bounce"), &LRTVolume3D::is_multi_bounce);
	ClassDB::bind_method(D_METHOD("set_geometry_root", "root"), &LRTVolume3D::set_geometry_root);
	ClassDB::bind_method(D_METHOD("get_geometry_root"), &LRTVolume3D::get_geometry_root);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &LRTVolume3D::set_paused);
	ClassDB::bind_method(D_METHOD("is_paused"), &LRTVolume3D::is_paused);
	ClassDB::bind_method(D_METHOD("set_iterations_per_frame", "iterations"), &LRTVolume3D::set_iterations_per_frame);
	ClassDB::bind_method(D_METHOD("get_iterations_per_frame"), &LRTVolume3D::get_iterations_per_frame);
	ClassDB::bind_method(D_METHOD("set_observe_mode", "mode"), &LRTVolume3D::set_observe_mode);
	ClassDB::bind_method(D_METHOD("get_observe_mode"), &LRTVolume3D::get_observe_mode);
	ClassDB::bind_method(D_METHOD("set_exposure", "exposure"), &LRTVolume3D::set_exposure);
	ClassDB::bind_method(D_METHOD("get_exposure"), &LRTVolume3D::get_exposure);
	ClassDB::bind_method(D_METHOD("set_slice_height", "height"), &LRTVolume3D::set_slice_height);
	ClassDB::bind_method(D_METHOD("get_slice_height"), &LRTVolume3D::get_slice_height);
	ClassDB::bind_method(D_METHOD("set_blur_sampling", "enabled"), &LRTVolume3D::set_blur_sampling);
	ClassDB::bind_method(D_METHOD("is_blur_sampling"), &LRTVolume3D::is_blur_sampling);
	ClassDB::bind_method(D_METHOD("set_editor_preview", "enabled"), &LRTVolume3D::set_editor_preview);
	ClassDB::bind_method(D_METHOD("is_editor_preview"), &LRTVolume3D::is_editor_preview);
	ClassDB::bind_method(D_METHOD("set_prototype_tonemap", "enabled"), &LRTVolume3D::set_prototype_tonemap);
	ClassDB::bind_method(D_METHOD("is_prototype_tonemap"), &LRTVolume3D::is_prototype_tonemap);

	ClassDB::bind_method(D_METHOD("rebuild"), &LRTVolume3D::rebuild);
	ClassDB::bind_method(D_METHOD("poll"), &LRTVolume3D::poll);
	ClassDB::bind_method(D_METHOD("set_rebuild_suppressed", "suppressed"), &LRTVolume3D::set_rebuild_suppressed);
	ClassDB::bind_method(D_METHOD("is_rebuild_suppressed"), &LRTVolume3D::is_rebuild_suppressed);
	ClassDB::bind_method(D_METHOD("get_volume_warnings"), &LRTVolume3D::get_volume_warnings);
	ClassDB::bind_method(D_METHOD("step", "iterations"), &LRTVolume3D::step, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("reset_field"), &LRTVolume3D::reset_field);
	ClassDB::bind_method(D_METHOD("is_building"), &LRTVolume3D::is_building);
	ClassDB::bind_method(D_METHOD("get_error_message"), &LRTVolume3D::get_error_message);
	ClassDB::bind_method(D_METHOD("get_build_stats"), &LRTVolume3D::get_build_stats);
	ClassDB::bind_method(D_METHOD("get_geometry_builds"), &LRTVolume3D::get_geometry_builds);
	ClassDB::bind_method(D_METHOD("get_dropped_builds"), &LRTVolume3D::get_dropped_builds);
	ClassDB::bind_method(D_METHOD("get_cancelled_builds"), &LRTVolume3D::get_cancelled_builds);
	ClassDB::bind_method(D_METHOD("get_solver"), &LRTVolume3D::get_solver);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.05,2.0,0.01,or_greater"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "volume_size", PROPERTY_HINT_NONE, "suffix:m"), "set_volume_size", "get_volume_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "expand_to_geometry"), "set_expand_to_geometry", "is_expanded_to_geometry");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "geometry_backend", PROPERTY_HINT_ENUM, "Color SDF,Analytic boxes"), "set_geometry_backend", "get_geometry_backend");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_mode", PROPERTY_HINT_ENUM, "SH triple product,26-direction mask"), "set_visibility_mode", "get_visibility_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_sdf_resolution", PROPERTY_HINT_RANGE, "8,256,1,or_greater"), "set_mesh_sdf_resolution", "get_mesh_sdf_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "multi_bounce"), "set_multi_bounce", "is_multi_bounce");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "geometry_root", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_geometry_root", "get_geometry_root");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "is_paused");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "iterations_per_frame", PROPERTY_HINT_RANGE, "0,8,1"), "set_iterations_per_frame", "get_iterations_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "observe_mode", PROPERTY_HINT_ENUM, "Full lighting,Direct only,Indirect only,Sky visibility,Slice: indirect,Slice: sky visibility,Slice: matrix"), "set_observe_mode", "get_observe_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "exposure", PROPERTY_HINT_RANGE, "0.2,3.0,0.01"), "set_exposure", "get_exposure");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slice_height", PROPERTY_HINT_RANGE, "-0.5,3.5,0.05"), "set_slice_height", "get_slice_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "blur_sampling"), "set_blur_sampling", "is_blur_sampling");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "editor_preview"), "set_editor_preview", "is_editor_preview");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "prototype_tonemap"), "set_prototype_tonemap", "is_prototype_tonemap");

	BIND_ENUM_CONSTANT(BACKEND_SDF);
	BIND_ENUM_CONSTANT(BACKEND_ANALYTIC);
	BIND_ENUM_CONSTANT(VISIBILITY_SH);
	BIND_ENUM_CONSTANT(VISIBILITY_MASK);
	BIND_ENUM_CONSTANT(OBSERVE_FULL);
	BIND_ENUM_CONSTANT(OBSERVE_DIRECT);
	BIND_ENUM_CONSTANT(OBSERVE_INDIRECT);
	BIND_ENUM_CONSTANT(OBSERVE_SKY_VISIBILITY);
	BIND_ENUM_CONSTANT(OBSERVE_SLICE_RADIANCE);
	BIND_ENUM_CONSTANT(OBSERVE_SLICE_SKY_VISIBILITY);
	BIND_ENUM_CONSTANT(OBSERVE_SLICE_MATRIX);
}

// --- Configuration ---------------------------------------------------------

void LRTVolume3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	// The gizmo colour follows the switch, like ReflectionProbe does.
	update_gizmos();
	if (!enabled) {
		_apply_display();
	} else if (_is_active()) {
		_apply_display();
	}
}

bool LRTVolume3D::is_enabled() const {
	return enabled;
}

void LRTVolume3D::set_spacing(double p_spacing) {
	if (spacing == p_spacing) {
		return;
	}
	spacing = p_spacing;
	// The probe lattice is drawn from `spacing`, so the box has to be repainted on its own.
	update_gizmos();
	_request_rebuild();
}

double LRTVolume3D::get_spacing() const {
	return spacing;
}

void LRTVolume3D::set_volume_size(const Vector3 &p_size) {
	if (volume_size == p_size) {
		return;
	}
	volume_size = p_size;
	update_gizmos();
	_request_rebuild();
}

Vector3 LRTVolume3D::get_volume_size() const {
	return volume_size;
}

void LRTVolume3D::set_expand_to_geometry(bool p_expand) {
	if (expand_to_geometry == p_expand) {
		return;
	}
	expand_to_geometry = p_expand;
	_request_rebuild();
}

bool LRTVolume3D::is_expanded_to_geometry() const {
	return expand_to_geometry;
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
		_inject_sources();
	}
}

int LRTVolume3D::get_visibility_mode() const {
	return visibility_mode;
}

void LRTVolume3D::set_mesh_sdf_resolution(int p_resolution) {
	const int clamped = MAX(8, p_resolution);
	if (mesh_sdf_resolution == clamped) {
		return;
	}
	mesh_sdf_resolution = clamped;
	_request_rebuild();
}

int LRTVolume3D::get_mesh_sdf_resolution() const {
	return mesh_sdf_resolution;
}

void LRTVolume3D::set_multi_bounce(bool p_enabled) {
	if (multi_bounce == p_enabled) {
		return;
	}
	multi_bounce = p_enabled;
	if (solver.is_valid()) {
		solver->set_multi_bounce(multi_bounce);
		_inject_sources();
	}
}

bool LRTVolume3D::is_multi_bounce() const {
	return multi_bounce;
}

void LRTVolume3D::set_geometry_root(const NodePath &p_root) {
	if (geometry_root == p_root) {
		return;
	}
	geometry_root = p_root;
	_request_rebuild();
}

NodePath LRTVolume3D::get_geometry_root() const {
	return geometry_root;
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

void LRTVolume3D::set_observe_mode(int p_mode) {
	if (observe_mode == p_mode) {
		return;
	}
	observe_mode = p_mode;
	_apply_display();
	_update_display_parameters();
}

int LRTVolume3D::get_observe_mode() const {
	return observe_mode;
}

void LRTVolume3D::set_exposure(double p_exposure) {
	exposure = p_exposure;
	_apply_environment(enabled && prototype_tonemap);
	_update_display_parameters();
}

double LRTVolume3D::get_exposure() const {
	return exposure;
}

void LRTVolume3D::set_slice_height(double p_height) {
	slice_height = p_height;
	_update_display_parameters();
}

double LRTVolume3D::get_slice_height() const {
	return slice_height;
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
			rebuild();
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

void LRTVolume3D::set_prototype_tonemap(bool p_enabled) {
	if (prototype_tonemap == p_enabled) {
		return;
	}
	prototype_tonemap = p_enabled;
	_apply_environment(enabled && prototype_tonemap);
	if (!prototype_tonemap) {
		_restore_authored_environment();
	}
}

bool LRTVolume3D::is_prototype_tonemap() const {
	return prototype_tonemap;
}

// --- Public operations -----------------------------------------------------

// Change entry point of the configuration properties: normally an immediate rebuild, but while
// an editor drag is resizing the volume the change is only remembered.
void LRTVolume3D::_request_rebuild() {
	has_signature = false;
	if (rebuild_suppressed) {
		rebuild_pending = true;
		return;
	}
	rebuild();
}

void LRTVolume3D::rebuild() {
	if (!_is_active()) {
		return;
	}
	rebuild_pending = false;
	_collect_geometry();
	_collect_lights();
	_start_build();
}

void LRTVolume3D::set_rebuild_suppressed(bool p_suppressed) {
	if (rebuild_suppressed == p_suppressed) {
		return;
	}
	rebuild_suppressed = p_suppressed;
	if (!rebuild_suppressed && rebuild_pending) {
		rebuild();
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
	solver->refresh_display();
	_update_display_parameters();
}

void LRTVolume3D::reset_field() {
	paused = true;
	if (solver.is_null() || build_stats.is_empty()) {
		return;
	}
	solver->reset();
	solver->refresh_display();
	_update_display_parameters();
}

bool LRTVolume3D::is_building() const {
	return building;
}

String LRTVolume3D::get_error_message() const {
	return error_message;
}

Dictionary LRTVolume3D::get_build_stats() const {
	return build_stats;
}

int LRTVolume3D::get_geometry_builds() const {
	return geometry_builds;
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

bool LRTVolume3D::_is_slice_mode() const {
	return observe_mode >= OBSERVE_SLICE_RADIANCE;
}

bool LRTVolume3D::_is_active() const {
	if (!enabled || !is_inside_tree()) {
		return false;
	}
	if (Engine::get_singleton()->is_editor_hint() && !editor_preview) {
		return false;
	}
	return true;
}

Node3D *LRTVolume3D::_geometry_root() const {
	if (geometry_root.is_empty()) {
		return Object::cast_to<Node3D>(get_parent());
	}
	Node *node = get_node_or_null(geometry_root);
	if (node == nullptr) {
		return nullptr;
	}
	return Object::cast_to<Node3D>(node);
}

// --- Scene inputs ----------------------------------------------------------

// Rescans the receiver list every frame. The scene tree is the authority for geometry and
// materials, so a moved instance, a hidden instance, a new child or an edited colour all
// show up as a signature change; nothing here modifies the scene.
void LRTVolume3D::_collect_geometry() {
	Node3D *root = _geometry_root();
	std::vector<Receiver> next;
	next.reserve(receivers.size());
	if (root != nullptr) {
		const TypedArray<Node> found = root->find_children("*", "MeshInstance3D", true, false);
		for (int i = 0; i < found.size(); i++) {
			MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(found[i]);
			if (mesh_instance == nullptr || (Node *)mesh_instance == (Node *)this || is_ancestor_of(mesh_instance)) {
				continue;
			}
			if (!mesh_instance->is_visible() || mesh_instance->get_mesh().is_null()) {
				continue;
			}
			Receiver entry;
			entry.instance = mesh_instance;
			entry.albedo = _surface_albedo(mesh_instance);
			bool reused = false;
			for (const Receiver &existing : receivers) {
				if (existing.instance == mesh_instance) {
					entry.overlay = existing.overlay;
					entry.authored_overlay = existing.authored_overlay;
					reused = true;
					break;
				}
			}
			if (!reused) {
				entry.authored_overlay = mesh_instance->get_material_overlay();
			}
			const bool receives = _surface_metallic(mesh_instance) < METALLIC_THRESHOLD;
			if (receives && entry.overlay.is_null()) {
				Ref<ShaderMaterial> overlay;
				overlay.instantiate();
				overlay->set_shader(_receive_shader());
				entry.overlay = overlay;
			} else if (!receives && entry.overlay.is_valid()) {
				// A surface that became metallic keeps its authored overlay only.
				entry.overlay = Ref<ShaderMaterial>();
			}
			next.push_back(entry);
		}
	}
	// Receivers that left the scene give the authored overlay back.
	for (const Receiver &existing : receivers) {
		if (existing.overlay.is_null()) {
			continue;
		}
		bool present = false;
		for (const Receiver &entry : next) {
			if (entry.instance == existing.instance) {
				present = true;
				break;
			}
		}
		if (!present && existing.instance != nullptr && existing.instance->get_material_overlay() == existing.overlay) {
			existing.instance->set_material_overlay(existing.authored_overlay);
		}
	}
	receivers = next;
}

void LRTVolume3D::_collect_lights() {
	Node3D *root = _geometry_root();
	std::vector<LightEntry> next;
	if (root != nullptr) {
		const TypedArray<Node> found = root->find_children("*", "Light3D", true, false);
		for (int i = 0; i < found.size(); i++) {
			Light3D *light = Object::cast_to<Light3D>(found[i]);
			if (light == nullptr) {
				continue;
			}
			LightEntry entry;
			entry.light = light;
			entry.visible = light->is_visible();
			entry.written_visible = entry.visible;
			for (const LightEntry &existing : lights) {
				if (existing.light == light) {
					entry.visible = existing.visible;
					entry.written_visible = existing.written_visible;
					break;
				}
			}
			next.push_back(entry);
		}
	}
	lights = next;
}

Ref<Shader> LRTVolume3D::_receive_shader() {
	if (receive_shader.is_null()) {
		receive_shader.instantiate();
		receive_shader->set_code(lrt_receive_shader_source);
	}
	return receive_shader;
}

Ref<Shader> LRTVolume3D::_slice_shader() {
	if (slice_shader.is_null()) {
		slice_shader.instantiate();
		slice_shader->set_code(lrt_slice_shader_source);
	}
	return slice_shader;
}

// The engine's own material of one surface: override, then mesh material, exactly as the
// renderer resolves it for the direct term the overlay is added to.
Ref<Material> LRTVolume3D::_surface_material(MeshInstance3D *p_instance, int p_surface) {
	Ref<Material> override_material = p_instance->get_surface_override_material(p_surface);
	if (override_material.is_valid()) {
		return override_material;
	}
	Ref<Material> instance_material = p_instance->get_material_override();
	if (instance_material.is_valid()) {
		return instance_material;
	}
	return p_instance->get_mesh()->surface_get_material(p_surface);
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

float LRTVolume3D::_surface_metallic(MeshInstance3D *p_instance) {
	Ref<Mesh> mesh = p_instance->get_mesh();
	if (mesh.is_null()) {
		return 0.0f;
	}
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		Ref<StandardMaterial3D> standard = _surface_material(p_instance, surface);
		if (standard.is_valid()) {
			return standard->get_metallic();
		}
	}
	return 0.0f;
}

static uint64_t mix_signature(uint64_t p_hash, uint64_t p_value) {
	// hash_murmur3_one_64 returns a 32-bit digest, which is plenty for change detection.
	return hash_murmur3_one_64(p_value, uint32_t(p_hash) ^ 0x9e3779b9u);
}

// Everything that changes the local field or the source pass, hashed every frame: a
// different hash means the scene changed and the volume parameters, geometry, transforms,
// visibility or colours have to be baked again.
uint64_t LRTVolume3D::_geometry_signature() const {
	uint64_t state = 0;
	state = mix_signature(state, uint64_t(spacing * 100000.0));
	state = mix_signature(state, uint64_t(volume_size.x * 10000.0));
	state = mix_signature(state, uint64_t(volume_size.y * 10000.0));
	state = mix_signature(state, uint64_t(volume_size.z * 10000.0));
	state = mix_signature(state, uint64_t(expand_to_geometry ? 1 : 0));
	state = mix_signature(state, uint64_t(geometry_backend));
	state = mix_signature(state, uint64_t(visibility_mode));
	state = mix_signature(state, uint64_t(mesh_sdf_resolution));
	state = mix_signature(state, uint64_t(multi_bounce ? 1 : 0));
	Node3D *root = _geometry_root();
	state = mix_signature(state, uint64_t(uintptr_t(root)));
	for (const Receiver &receiver : receivers) {
		const Transform3D transform = receiver.instance->get_global_transform();
		state = mix_signature(state, uint64_t(receiver.instance->get_instance_id()));
		state = mix_signature(state, uint64_t(transform.origin.x * 10000.0));
		state = mix_signature(state, uint64_t(transform.origin.y * 10000.0));
		state = mix_signature(state, uint64_t(transform.origin.z * 10000.0));
		state = mix_signature(state, hash_murmur3_buffer(&transform.basis, sizeof(Basis)));
		state = mix_signature(state, uint64_t(receiver.albedo.x * 100000.0));
		state = mix_signature(state, uint64_t(receiver.albedo.y * 100000.0));
		state = mix_signature(state, uint64_t(receiver.albedo.z * 100000.0));
		Ref<Mesh> mesh = receiver.instance->get_mesh();
		state = mix_signature(state, mesh.is_valid() ? mesh->get_rid().get_id() : 0);
		state = mix_signature(state, mesh.is_valid() ? uint64_t(mesh->get_surface_count()) : 0);
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
		Light3D *light = entry.light;
		if (light == nullptr) {
			continue;
		}
		const Transform3D transform = light->get_global_transform();
		const Color color = light->get_color().srgb_to_linear();
		double range = 1.0;
		double attenuation = 1.0;
		double spot_angle = 45.0;
		double spot_attenuation = 1.0;
		int type = 0;
		if (Object::cast_to<DirectionalLight3D>(light) != nullptr) {
			type = 1;
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
			} else {
				intensity = physical_intensity;
			}
		}
		Dictionary mapped;
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
		result.push_back(mapped);
	}
	return result;
}

// Prototype input collection: an axis-aligned box keeps the analytic box path, a rotated or
// non-uniformly scaled mesh instance is baked in world space (the prototype's PrimitiveGI
// only carries a uniform scale), and everything else keeps the asset-local triangle soup so
// the same baked Color SDF can be shared by every instance of that mesh.
bool LRTVolume3D::_build_geometry_inputs(std::vector<LRTVolume::BoxInstance> &r_boxes, std::vector<LRTVolume::MeshInstance> &r_meshes, Vector3 &r_bounds_min, Vector3 &r_bounds_max) {
	r_boxes.clear();
	r_meshes.clear();
	bool first = true;
	for (const Receiver &receiver : receivers) {
		MeshInstance3D *mesh_instance = receiver.instance;
		Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_null()) {
			continue;
		}
		const Transform3D transform = mesh_instance->get_global_transform();
		const Basis basis = transform.basis;
		const Vector3 scale = basis.get_scale();
		const bool uniform = Math::is_equal_approx(scale.x, scale.y) && Math::is_equal_approx(scale.y, scale.z) && scale.x > 0.0;
		const double uniform_scale = uniform ? double(scale.x) : 1.0;
		BoxMesh *box = uniform ? Object::cast_to<BoxMesh>(mesh.ptr()) : nullptr;
		if (box != nullptr) {
			LRTVolume::BoxInstance entry;
			entry.local_extent = to_lrt(box->get_size());
			entry.color = to_lrt(receiver.albedo);
			entry.transform = to_lrt_transform(transform, uniform_scale);
			entry.axis_aligned = _is_axis_aligned(basis);
			// World AABB of the (possibly rotated) box, which is what the analytic backend
			// and the injection's box occlusion test consume.
			// World AABB half extents: |basis| * box size / 2, computed per world axis.
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
			if (first) {
				r_bounds_min = transform.origin - half;
				r_bounds_max = transform.origin + half;
				first = false;
			} else {
				r_bounds_min = r_bounds_min.min(transform.origin - half);
				r_bounds_max = r_bounds_max.max(transform.origin + half);
			}
			continue;
		}
		LRTVolume::MeshInstance entry;
		const bool local_space = uniform;
		entry.transform = local_space ? to_lrt_transform(transform, uniform_scale) : lrt::PrimitiveTransform();
		// The shared bake is keyed by the asset and by the material colours that feed the
		// local colour field, so a plain colour edit bakes a different field while every
		// instance of an unedited asset keeps sharing one.
		uint64_t asset_key = 0;
		if (local_space) {
			asset_key = mix_signature(asset_key, mesh->get_rid().get_id());
			asset_key = mix_signature(asset_key, uint64_t(mesh_sdf_resolution));
		}
		for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
			const Array arrays = mesh->surface_get_arrays(surface);
			if (arrays.size() <= Mesh::ARRAY_VERTEX) {
				continue;
			}
			const Variant raw_points = arrays[Mesh::ARRAY_VERTEX];
			if (raw_points.get_type() != Variant::PACKED_VECTOR3_ARRAY) {
				continue;
			}
			const PackedVector3Array points = raw_points;
			PackedColorArray vertex_colors;
			if (arrays.size() > Mesh::ARRAY_COLOR && arrays[Mesh::ARRAY_COLOR].get_type() == Variant::PACKED_COLOR_ARRAY) {
				vertex_colors = arrays[Mesh::ARRAY_COLOR];
			}
			PackedInt32Array indices;
			if (arrays.size() > Mesh::ARRAY_INDEX && arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
				indices = arrays[Mesh::ARRAY_INDEX];
			}
			const Vector3 material_color = _material_albedo(_surface_material(mesh_instance, surface));
			if (local_space) {
				asset_key = mix_signature(asset_key, uint64_t(material_color.x * 100000.0));
				asset_key = mix_signature(asset_key, uint64_t(material_color.y * 100000.0));
				asset_key = mix_signature(asset_key, uint64_t(material_color.z * 100000.0));
			}
			const int vertex_count = indices.is_empty() ? points.size() : indices.size();
			for (int index = 0; index < vertex_count; index += 3) {
				lrt::MeshTriangle triangle;
				for (int v = 0; v < 3; v++) {
					const int source = indices.is_empty() ? index + v : indices[index + v];
					Vector3 color = material_color;
					if (vertex_colors.size() == points.size()) {
						const Color vertex = vertex_colors[source];
						color *= Vector3(vertex.r, vertex.g, vertex.b);
					}
					const Vector3 local_position = points[source];
					// The bake runs in the asset frame; the region expansion still needs the
					// world position the prototype's buildBVH saw.
					const Vector3 world_position = transform.xform(local_position);
					triangle.position[v] = to_lrt(local_space ? local_position : world_position);
					triangle.color[v] = to_lrt(color);
					if (first) {
						r_bounds_min = world_position;
						r_bounds_max = world_position;
						first = false;
					} else {
						r_bounds_min = r_bounds_min.min(world_position);
						r_bounds_max = r_bounds_max.max(world_position);
					}
				}
				entry.triangles.push_back(triangle);
			}
		}
		entry.asset_key = local_space ? int64_t(asset_key) : 0;
		if (!entry.triangles.empty()) {
			r_meshes.push_back(entry);
		}
	}
	return true;
}

// --- Background build ------------------------------------------------------

void LRTVolume3D::_bake_task(void *p_userdata) {
	LRTVolume3D *volume = static_cast<LRTVolume3D *>(p_userdata);
	BuildJob *job = volume->job;
	if (job == nullptr || volume->solver.is_null()) {
		return;
	}
	job->result = volume->solver->bake_local_field_data(job->analytic);
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
	building = false;
}

void LRTVolume3D::_start_build() {
	// The previous job stops before the solver's inputs are replaced.
	_cancel_build();
	if (solver.is_null()) {
		solver.instantiate();
	}
	std::vector<LRTVolume::BoxInstance> boxes;
	std::vector<LRTVolume::MeshInstance> meshes;
	Vector3 bounds_min;
	Vector3 bounds_max;
	_build_geometry_inputs(boxes, meshes, bounds_min, bounds_max);
	box_min_world.clear();
	box_max_world.clear();
	for (const LRTVolume::BoxInstance &box : boxes) {
		box_min_world.push_back(Vector3(box.world_min.x, box.world_min.y, box.world_min.z));
		box_max_world.push_back(Vector3(box.world_max.x, box.world_max.y, box.world_max.z));
	}
	if (boxes.size() > 16) {
		error_message = vformat("LRT 接收器最多支持 16 个盒体（当前 %d）", int(boxes.size()));
		return;
	}
	if (boxes.empty() && meshes.empty()) {
		error_message = "LRT 体积内没有可用的接收几何";
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
			return;
		}
	}
	error_message = String();
	// The prototype grid is the fixed lab region; the node exposes the same region as a box
	// centred on the node, which keeps [-3,-0.5,-3]..[3,3.5,3] for the fixtures.
	const Vector3 center = get_global_transform().origin;
	const Vector3 grid_min = center - volume_size * 0.5;
	// The prototype expands the probe region only for imported models (lab.js buildBVH);
	// box-only fixtures keep the fixed region, which is the node's own volume box.
	if (expand_to_geometry && !meshes.empty()) {
		solver->configure_sized_with_bounds(spacing, grid_min, volume_size, bounds_min, bounds_max);
	} else {
		solver->configure_sized(spacing, grid_min, volume_size);
	}
	solver->set_mesh_sdf_resolution(mesh_sdf_resolution);
	solver->set_multi_bounce(multi_bounce);
	solver->set_sh_visibility(visibility_mode == VISIBILITY_SH);
	solver->set_box_instances(boxes);
	solver->set_mesh_instances(meshes);
	solver->clear_cancel();

	job = memnew(BuildJob);
	job->analytic = geometry_backend == BACKEND_ANALYTIC;
	generation++;
	job->generation = generation;
	building = true;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	task_id = pool->add_native_task(&LRTVolume3D::_bake_task, this, false, "LRT local field bake");
}

void LRTVolume3D::_poll_build() {
	if (job == nullptr || !job->done.load()) {
		return;
	}
	if (task_id != 0) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
		task_id = 0;
	}
	BuildJob *finished = job;
	job = nullptr;
	building = false;
	const LRTVolume::LocalBakeResult result = finished->result;
	const bool stale = finished->generation != generation;
	memdelete(finished);
	if (result.cancelled) {
		return;
	}
	if (stale) {
		// A newer build already replaced this one, so its field must never be shown.
		dropped_builds++;
		return;
	}
	if (!result.ok) {
		if (result.needs_axis_aligned) {
			error_message = "解析盒后端不支持旋转的盒体";
		} else {
			error_message = "LRT 局部场构建未完成";
		}
		return;
	}
	Dictionary applied = solver->apply_local_field();
	if (applied.is_empty()) {
		error_message = "LRT 局部场上传失败";
		return;
	}
	applied["build_ms"] = result.build_ms;
	build_stats = applied;
	geometry_builds++;
	_ensure_display_resources();
	_inject_sources();
	_apply_display();
}

// --- Display ---------------------------------------------------------------

static Ref<ImageTexture> floats_to_texture(const PackedFloat32Array &p_values, int p_width = 1024) {
	const int texels = MAX(1, (p_values.size() + 3) / 4);
	const int height = MAX(1, (texels + p_width - 1) / p_width);
	PackedFloat32Array padded = p_values;
	padded.resize(p_width * height * 4);
	Ref<Image> image = Image::create_from_data(p_width, height, false, Image::FORMAT_RGBAF, padded.to_byte_array());
	return ImageTexture::create_from_image(image);
}

void LRTVolume3D::_ensure_display_resources() {
	if (solver.is_null()) {
		return;
	}
	if (slice_rect == nullptr) {
		slice_layer = memnew(CanvasLayer);
		slice_layer->set_layer(4);
		slice_rect = memnew(ColorRect);
		slice_rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		Ref<ShaderMaterial> material;
		material.instantiate();
		material->set_shader(_slice_shader());
		slice_rect->set_material(material);
		slice_layer->add_child(slice_rect);
		add_child(slice_layer, false, Node::INTERNAL_MODE_FRONT);
		slice_rect->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		slice_layer->set_visible(false);
	}
	const Dictionary bvh = solver->get_mesh_bvh();
	mesh_node_count = bvh.get("node_count", 0);
	mesh_node_texture = floats_to_texture(bvh.get("nodes", PackedFloat32Array()));
	mesh_triangle_texture = floats_to_texture(bvh.get("triangles", PackedFloat32Array()));
	mesh_material_texture = floats_to_texture(bvh.get("materials", PackedFloat32Array()));
	if (mesh_atlas_texture.is_null()) {
		Ref<Image> white = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
		white->fill(Color(1, 1, 1, 1));
		mesh_atlas_texture = ImageTexture::create_from_image(white);
	}
}

void LRTVolume3D::_update_display_parameters() {
	if (solver.is_null() || build_stats.is_empty()) {
		return;
	}
	const Dictionary grid = solver->get_grid();
	const Vector3i size = grid.get("size", Vector3i());
	const Vector3 grid_min = grid.get("min", Vector3());
	const double grid_spacing = grid.get("spacing", 0.25);
	const Vector2 atlas(size.x * size.z, size.y);
	const Vector2 matrix_atlas(size.x * size.z, size.y * 12);
	PackedVector3Array box_mins;
	PackedVector3Array box_maxs;
	for (int index = 0; index < 16; index++) {
		if (index < int(box_min_world.size())) {
			box_mins.push_back(box_min_world[index]);
			box_maxs.push_back(box_max_world[index]);
		} else {
			box_mins.push_back(Vector3());
			box_maxs.push_back(Vector3());
		}
	}
	const Ref<Texture2D> radiance_r = solver->get_texture("radiance_r");
	const Ref<Texture2D> radiance_g = solver->get_texture("radiance_g");
	const Ref<Texture2D> radiance_b = solver->get_texture("radiance_b");
	const Ref<Texture2D> visibility = solver->get_texture("visibility");
	const Ref<Texture2D> material_field = solver->get_texture("material");
	const Ref<Texture2D> matrix_field = solver->get_texture("matrices");
	for (const Receiver &receiver : receivers) {
		if (receiver.overlay.is_null()) {
			continue;
		}
		Ref<ShaderMaterial> material = receiver.overlay;
		material->set_shader_parameter("albedo", receiver.albedo);
		material->set_shader_parameter("grid_min", grid_min);
		material->set_shader_parameter("grid_size", Vector3(size));
		material->set_shader_parameter("spacing", grid_spacing);
		material->set_shader_parameter("atlas_size", atlas);
		material->set_shader_parameter("gather_count", 27);
		material->set_shader_parameter("blur_sampling", blur_sampling);
		material->set_shader_parameter("sky_color", sky);
		material->set_shader_parameter("mode", observe_mode);
		material->set_shader_parameter("box_count", int(box_min_world.size()));
		material->set_shader_parameter("box_min", box_mins);
		material->set_shader_parameter("box_max", box_maxs);
		material->set_shader_parameter("mesh_node_count", mesh_node_count);
		material->set_shader_parameter("mesh_nodes", mesh_node_texture);
		material->set_shader_parameter("mesh_triangles", mesh_triangle_texture);
		material->set_shader_parameter("mesh_materials", mesh_material_texture);
		material->set_shader_parameter("mesh_atlas", mesh_atlas_texture);
		material->set_shader_parameter("radiance_r", radiance_r);
		material->set_shader_parameter("radiance_g", radiance_g);
		material->set_shader_parameter("radiance_b", radiance_b);
		material->set_shader_parameter("visibility_field", visibility);
		material->set_shader_parameter("material_field", material_field);
	}
	if (slice_rect != nullptr) {
		Ref<ShaderMaterial> slice_material = slice_rect->get_material();
		if (slice_material.is_valid()) {
			slice_material->set_shader_parameter("grid_min", grid_min);
			slice_material->set_shader_parameter("grid_size", Vector3(size));
			slice_material->set_shader_parameter("spacing", grid_spacing);
			slice_material->set_shader_parameter("atlas_size", atlas);
			slice_material->set_shader_parameter("matrix_atlas_size", matrix_atlas);
			slice_material->set_shader_parameter("exposure", exposure);
			slice_material->set_shader_parameter("slice_height", slice_height);
			slice_material->set_shader_parameter("mode", observe_mode);
			slice_material->set_shader_parameter("radiance_r", radiance_r);
			slice_material->set_shader_parameter("radiance_g", radiance_g);
			slice_material->set_shader_parameter("radiance_b", radiance_b);
			slice_material->set_shader_parameter("visibility_field", visibility);
			slice_material->set_shader_parameter("material_field", material_field);
			slice_material->set_shader_parameter("matrix_field", matrix_field);
		}
	}
}

// Prototype display rules: the direct-light mode drops the overlay, the indirect and sky
// modes switch the engine's own lights off instead, and LRT off restores everything the
// scene authored. Neither the light parameters nor the surface materials are modified.
void LRTVolume3D::_apply_display() {
	const bool show_indirect = enabled && observe_mode != OBSERVE_DIRECT && !_is_slice_mode();
	for (const Receiver &receiver : receivers) {
		if (receiver.overlay.is_null() || receiver.instance == nullptr) {
			continue;
		}
		const Ref<Material> overlay = show_indirect ? Ref<Material>(receiver.overlay) : receiver.authored_overlay;
		receiver.instance->set_material_overlay(overlay);
	}
	const bool lights_active = !enabled || observe_mode == OBSERVE_FULL || observe_mode == OBSERVE_DIRECT;
	for (LightEntry &entry : lights) {
		if (entry.light == nullptr) {
			continue;
		}
		const bool visible = lights_active ? entry.visible : false;
		entry.light->set_visible(visible);
		entry.written_visible = visible;
	}
	if (slice_layer != nullptr) {
		slice_layer->set_visible(enabled && _is_slice_mode());
	}
	_apply_environment(enabled && prototype_tonemap);
	display_active = enabled;
}

void LRTVolume3D::_restore_authored_environment() {
	if (environment.is_null() || !tonemap_saved) {
		return;
	}
	environment->set_tonemapper(Environment::ToneMapper(saved_tonemap_mode));
	environment->set_tonemap_white(saved_tonemap_white);
	environment->set_tonemap_exposure(saved_tonemap_exposure);
	tonemap_saved = false;
}

void LRTVolume3D::_apply_environment(bool p_active) {
	if (environment.is_null() || !prototype_tonemap) {
		return;
	}
	if (!p_active) {
		_restore_authored_environment();
		return;
	}
	if (!tonemap_saved) {
		saved_tonemap_mode = int(environment->get_tonemapper());
		saved_tonemap_white = environment->get_tonemap_white();
		saved_tonemap_exposure = environment->get_tonemap_exposure();
		tonemap_saved = true;
	}
	environment->set_tonemapper(Environment::TONE_MAPPER_REINHARDT);
	environment->set_tonemap_white(PROTOTYPE_TONEMAP_WHITE);
	environment->set_tonemap_exposure(exposure);
}

void LRTVolume3D::_refresh_environment() {
	if (environment.is_valid()) {
		return;
	}
	Node3D *root = _geometry_root();
	if (root == nullptr) {
		return;
	}
	const TypedArray<Node> found = root->find_children("*", "WorldEnvironment", true, false);
	for (int i = 0; i < found.size(); i++) {
		WorldEnvironment *world = Object::cast_to<WorldEnvironment>(found[i]);
		if (world != nullptr && world->get_environment().is_valid()) {
			environment = world->get_environment();
			break;
		}
	}
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
	const Vector3 rotation = environment->get_sky_rotation();
	state = mix_signature(state, uint64_t(rotation.x * 10000.0));
	state = mix_signature(state, uint64_t(rotation.y * 10000.0));
	state = mix_signature(state, uint64_t(rotation.z * 10000.0));
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
// viewport renders the environment once before its panorama is averaged.
void LRTVolume3D::_render_environment() {
	if (sky_viewport == nullptr) {
		sky_viewport = memnew(SubViewport);
		sky_viewport->set_name("LrtSkyViewport");
		sky_viewport->set_size(Vector2i(16, 16));
		sky_viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
		Ref<World3D> world;
		world.instantiate();
		sky_viewport->set_world_3d(world);
		Camera3D *camera = memnew(Camera3D);
		sky_viewport->add_child(camera);
		Node *host = is_inside_tree() ? (Node *)this : (Node *)SceneTree::get_singleton()->get_root();
		host->add_child(sky_viewport, false, Node::INTERNAL_MODE_FRONT);
	}
	sky_viewport->get_world_3d()->set_environment(environment);
	RenderingServer::get_singleton()->draw(false, 0.0);
}

Vector3 LRTVolume3D::_environment_radiance() {
	if (environment.is_null() || solver.is_null()) {
		return Vector3();
	}
	const uint64_t key = _environment_key();
	if (key == environment_key) {
		return cached_sky;
	}
	environment_key = key;
	if (environment->get_sky().is_valid()) {
		_render_environment();
	}
	cached_sky = solver->read_environment_radiance(environment, Vector2i(SKY_PANORAMA_WIDTH, SKY_PANORAMA_HEIGHT));
	return cached_sky;
}

// Re-runs the source pass on the existing local field and restarts propagation. Never
// rebuilds the geometry: that is [method _start_build].
void LRTVolume3D::_inject_sources() {
	if (solver.is_null() || !solver->has_local_field()) {
		return;
	}
	light_inputs = _mapped_lights();
	sky = _environment_radiance();
	solver->set_lights(light_inputs);
	solver->set_sky(sky);
	solver->inject();
	solver->reset();
	solver->refresh_display();
	_update_display_parameters();
}

// --- Node lifecycle --------------------------------------------------------

PackedStringArray LRTVolume3D::get_volume_warnings() const {
	PackedStringArray warnings;
	const Basis basis = get_transform().basis;
	const Vector3 scale = basis.get_scale();
	if (!Math::is_equal_approx(scale.x, scale.y) || !Math::is_equal_approx(scale.y, scale.z)) {
		warnings.push_back(RTR("The LRT volume does not support non-uniform scaling: the probe grid is built in world axes around the node."));
	}
	if (!_is_axis_aligned(basis)) {
		warnings.push_back(RTR("The LRT volume does not support rotation: the probe grid stays aligned to the world axes. Move and size the volume instead."));
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
			// The build starts on the first processed frame, not here: ENTER_TREE reaches the
			// volume node before its sibling receivers, and a mesh that has not entered the
			// tree yet cannot report a world transform.
			has_signature = false;
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_cancel_build();
			// Everything the node wrote into the scene goes back to its authored value.
			for (const Receiver &receiver : receivers) {
				if (receiver.overlay.is_valid() && receiver.instance != nullptr && receiver.instance->get_material_overlay() == receiver.overlay) {
					receiver.instance->set_material_overlay(receiver.authored_overlay);
				}
			}
			for (const LightEntry &entry : lights) {
				if (entry.light != nullptr && entry.light->is_visible() != entry.visible) {
					entry.light->set_visible(entry.visible);
				}
			}
			_restore_authored_environment();
			if (slice_layer != nullptr) {
				slice_layer->set_visible(false);
			}
		} break;
		case NOTIFICATION_EDITOR_PRE_SAVE: {
			// A save must never store the preview: overlays, light visibility and the
			// display tonemap go back to what the scene authored, then are re-applied.
			for (const Receiver &receiver : receivers) {
				if (receiver.overlay.is_valid() && receiver.instance != nullptr && receiver.instance->get_material_overlay() == receiver.overlay) {
					receiver.instance->set_material_overlay(receiver.authored_overlay);
				}
			}
			for (const LightEntry &entry : lights) {
				if (entry.light != nullptr && entry.light->is_visible() != entry.visible) {
					entry.light->set_visible(entry.visible);
				}
			}
			_restore_authored_environment();
		} break;
		case NOTIFICATION_EDITOR_POST_SAVE: {
			_apply_display();
		} break;
		case NOTIFICATION_PREDELETE: {
			_cancel_build();
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
			// The world-axis restriction depends on the node's own rotation and scale, so the
			// editor warning follows the transform live.
			update_configuration_warnings();
		} break;
	}
}

// One frame of the node's own logic. NOTIFICATION_PROCESS calls it every frame; [method poll]
// exposes the same work to scripts and tests, which cannot wait for editor frames.
void LRTVolume3D::_refresh_frame() {
	if (!_is_active()) {
		if (display_active) {
			_apply_display();
		}
		return;
	}
	// The user owns light visibility; only a change this node did not write counts.
	for (LightEntry &entry : lights) {
		if (entry.light != nullptr && entry.light->is_visible() != entry.written_visible) {
			entry.visible = entry.light->is_visible();
		}
	}
	_collect_geometry();
	_collect_lights();
	_refresh_environment();
	const uint64_t signature = _geometry_signature();
	if (!has_signature || signature != geometry_signature) {
		geometry_signature = signature;
		has_signature = true;
		if (rebuild_suppressed) {
			// The gizmo drag keeps changing the box: wait for the drag to finish.
			rebuild_pending = true;
		} else {
			_start_build();
		}
	}
	_poll_build();
	if (error_message.is_empty() && solver.is_valid() && solver->has_local_field()) {
		const Array mapped = _mapped_lights();
		if (mapped != light_inputs || _environment_radiance() != sky) {
			_inject_sources();
			_apply_display();
		}
		if (!paused && iterations_per_frame > 0 && !_is_slice_mode()) {
			solver->step(iterations_per_frame);
			solver->refresh_display();
			_update_display_parameters();
		}
	}
	// The editor's 3D viewports keep their render target update mode at UPDATE_WHEN_VISIBLE, so
	// they repaint every visible frame and follow the field without any help from here. The
	// viewport this node lives in is EditorNode::scene_root, which is 2D-only, so changing its
	// mode would not reach what the user sees.
}
