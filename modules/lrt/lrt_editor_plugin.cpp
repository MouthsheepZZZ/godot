/**************************************************************************/
/*  lrt_editor_plugin.cpp                                                 */
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

#ifdef TOOLS_ENABLED

#include "lrt_editor_plugin.h"

#include "lrt_volume_3d.h"
#include "lrt_export_plugin.h"
#include "lrt_cache.h"
#include "lrt_transfer_material.h"

#include "core/math/geometry_3d.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "editor/editor_interface.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/editor_string_names.h"
#include "editor/export/editor_export.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/spin_box.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"

LRTNumericOverrideEditor::LRTNumericOverrideEditor() {
	spin = memnew(EditorSpinSlider);
	spin->set_min(0.001);
	spin->set_max(10.0);
	spin->set_step(0.001);
	spin->set_allow_greater(true);
	spin->set_suffix("m");
	spin->set_flat(true);
	spin->set_deferred_drag_mode_enabled(true);
	add_child(spin);
	add_focusable(spin);
	configure(lrt::VOXEL_CELL_SIZE_META, "rendering/global_illumination/lrt/voxel/cell_size", TTR("Voxel Cell Size"));
	set_checkable(true);
	spin->connect(SceneStringName(value_changed), callable_mp(this, &LRTNumericOverrideEditor::_value_changed));
	connect("property_checked", callable_mp(this, &LRTNumericOverrideEditor::_override_toggled));
	set_process(true);
}

void LRTNumericOverrideEditor::configure(const StringName &p_metadata, const String &p_project_setting, const String &p_label, bool p_integer) {
	metadata = p_metadata;
	project_setting = p_project_setting;
	integer = p_integer;
	displayed_default = -1.0;
	spin->set_min(integer ? 1.0 : 0.001);
	spin->set_max(integer ? 4096.0 : 10.0);
	spin->set_step(integer ? 1.0 : 0.001);
	spin->set_suffix(integer ? String() : String("m"));
	set_label(p_label);
}

void LRTNumericOverrideEditor::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		update_property();
	}
}

void LRTNumericOverrideEditor::_set_read_only(bool p_read_only) {
	spin->set_read_only(p_read_only || !is_checked());
}

void LRTNumericOverrideEditor::update_property() {
	Object *target = get_edited_object();
	if (!target) {
		return;
	}
	const Variant override_size = target->has_meta(metadata) ? target->get_meta(metadata) : Variant();
	const double default_size = GLOBAL_GET(project_setting);
	if (displayed_override == override_size && displayed_default == default_size) {
		return;
	}
	displayed_override = override_size;
	displayed_default = default_size;
	const bool enabled = override_size.get_type() != Variant::NIL;
	set_checked(enabled);
	spin->set_read_only(is_read_only() || !enabled);
	spin->set_value_no_signal(enabled ? double(override_size) : default_size);
	set_tooltip_text(vformat(TTR("Enable to override the project value (%s). Mesh-local lengths scale with the instance. MultiMesh copies share this setting."), default_size));
}

void LRTNumericOverrideEditor::_commit_override(const Variant &p_value) {
	Object *target = get_edited_object();
	if (!target || is_read_only()) {
		return;
	}
	const Variant previous = target->has_meta(metadata) ? target->get_meta(metadata) : Variant();
	if (previous == p_value) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
	undo_redo->create_action(vformat(TTR("Set LRT %s"), get_label()), UndoRedo::MERGE_DISABLE, target);
	undo_redo->add_do_method(target, "set_meta", metadata, p_value);
	undo_redo->add_undo_method(target, "set_meta", metadata, previous);
	undo_redo->add_do_method(target, "notify_property_list_changed");
	undo_redo->add_undo_method(target, "notify_property_list_changed");
	undo_redo->commit_action();
	update_property();
}

void LRTNumericOverrideEditor::_override_toggled(const StringName &p_property, bool p_enabled) {
	_commit_override(p_enabled ? GLOBAL_GET(project_setting) : Variant());
}

void LRTNumericOverrideEditor::_value_changed(double p_value) {
	if (is_checked()) {
		_commit_override(integer ? Variant(int64_t(p_value)) : Variant(p_value));
	}
}

struct LRTRepresentationEditor::BakeJob {
	lrt::TransferInstanceBake input;
	ObjectID owner;
	Dictionary result;
	std::atomic<bool> cancelled{ false };
	std::atomic<uint64_t> evaluated{ 0 };
	std::atomic<int> completed_variants{ 0 };
};

void LRTRepresentationEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("poll"), &LRTRepresentationEditor::poll);
	ADD_SIGNAL(MethodInfo("bake_started"));
	ADD_SIGNAL(MethodInfo("bake_finished", PropertyInfo(Variant::DICTIONARY, "result")));
}

LRTRepresentationEditor::LRTRepresentationEditor() {
	set_label(TTR("Representation"));
	VBoxContainer *controls = memnew(VBoxContainer);
	add_child(controls);
	set_bottom_editor(controls);
	mode = memnew(OptionButton);
	mode->set_name("Representation");
	mode->add_item(TTR("Voxel"), lrt::GEOMETRY_VOXEL);
	mode->add_item(TTR("Precomputed LTM + LocalVis"), lrt::GEOMETRY_PRECOMPUTED_TRANSFER);
	controls->add_child(mode);
	add_focusable(mode);
	mode->connect("item_selected", callable_mp(this, &LRTRepresentationEditor::_mode_selected));
	status = memnew(Label);
	status->set_name("Status");
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	controls->add_child(status);
	HBoxContainer *actions = memnew(HBoxContainer);
	controls->add_child(actions);
	bake = memnew(Button);
	bake->set_name("Bake");
	bake->set_text(TTR("Bake"));
	bake->connect(SceneStringName(pressed), callable_mp(this, &LRTRepresentationEditor::_bake_pressed));
	actions->add_child(bake);
	check = memnew(Button);
	check->set_name("Check");
	check->set_text(TTR("Check"));
	check->connect(SceneStringName(pressed), callable_mp(this, &LRTRepresentationEditor::_check_status));
	actions->add_child(check);
	cancel = memnew(Button);
	cancel->set_name("Cancel");
	cancel->set_text(TTR("Cancel"));
	cancel->connect(SceneStringName(pressed), callable_mp(this, &LRTRepresentationEditor::_cancel_pressed));
	actions->add_child(cancel);
	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &LRTRepresentationEditor::_check_status));
	set_process(true);
}

LRTRepresentationEditor::~LRTRepresentationEditor() {
	if (task_id != WorkerThreadPool::INVALID_TASK_ID) {
		job->cancelled.store(true);
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
	}
}

void LRTRepresentationEditor::_set_read_only(bool p_read_only) {
	mode->set_disabled(p_read_only || job != nullptr || capture_pending);
	bake->set_disabled(p_read_only || job != nullptr || capture_pending);
}

void LRTRepresentationEditor::update_property() {
	Object *target = get_edited_object();
	if (target == nullptr) {
		return;
	}
	const int representation = lrt::geometry_representation(target);
	mode->select(representation >= 0 && representation <= 1 ? representation : -1);
	const bool precomputed = representation == lrt::GEOMETRY_PRECOMPUTED_TRANSFER;
	status->set_visible(precomputed || representation < 0 || representation > 1);
	bake->set_visible(precomputed);
	check->set_visible(precomputed);
	cancel->set_visible(job != nullptr || capture_pending);
	_set_read_only(is_read_only());
	if (precomputed && !checked_status && job == nullptr && !capture_pending) {
		_check_status();
	}
}

void LRTRepresentationEditor::_mode_selected(int p_index) {
	Object *target = get_edited_object();
	if (target == nullptr || is_read_only() || job != nullptr || capture_pending) {
		return;
	}
	const Variant previous = target->has_meta(lrt::REPRESENTATION_META) ? target->get_meta(lrt::REPRESENTATION_META) : Variant();
	if (int(lrt::geometry_representation(target)) == p_index) {
		return;
	}
	EditorUndoRedoManager *undo = EditorUndoRedoManager::get_singleton();
	undo->create_action(TTR("Set LRT Representation"), UndoRedo::MERGE_DISABLE, target);
	undo->add_do_method(target, "set_meta", lrt::REPRESENTATION_META, p_index);
	undo->add_undo_method(target, "set_meta", lrt::REPRESENTATION_META, previous);
	undo->add_do_method(target, "notify_property_list_changed");
	undo->add_undo_method(target, "notify_property_list_changed");
	undo->commit_action();
}

void LRTRepresentationEditor::_check_status() {
	Node3D *target = Object::cast_to<Node3D>(get_edited_object());
	if (target == nullptr || job != nullptr || capture_pending || lrt::geometry_representation(target) != lrt::GEOMETRY_PRECOMPUTED_TRANSFER) {
		return;
	}
	const Dictionary report = LRTVolume3D::get_transfer_instance_status(target);
	checked_status = true;
	if (bool(report.get("ok", false))) {
		const Array variants = Dictionary(report["manifest"])["variants"];
		status->set_text(vformat(TTR("Baked (%d material variants)"), variants.size()));
	} else {
		status->set_text(report.get("error", TTR("Precomputed LRT data is unavailable.")));
	}
}

void LRTRepresentationEditor::_bake_pressed() {
	if (is_read_only() || job != nullptr || capture_pending) {
		return;
	}
	capture_pending = true;
	status->set_text(TTR("Capturing static material inputs..."));
	update_property();
}

void LRTRepresentationEditor::_start_bake() {
	if (!capture_pending) {
		return;
	}
	Node3D *target = Object::cast_to<Node3D>(get_edited_object());
	auto pending = std::make_unique<BakeJob>();
	String error;
	if (!LRTVolume3D::prepare_transfer_instance_inputs(target, pending->input, error)) {
		capture_pending = false;
		checked_status = true;
		status->set_text(error);
		update_property();
		Dictionary report;
		report["ok"] = false;
		report["error"] = error;
		emit_signal("bake_finished", report);
		return;
	}
	pending->owner = target->get_instance_id();
	job = std::move(pending);
	capture_pending = false;
	task_id = WorkerThreadPool::get_singleton()->add_native_task(&LRTRepresentationEditor::_bake_task, job.get(), false, "LRT precomputed transfer bake");
	update_property();
	emit_signal("bake_started");
}

void LRTRepresentationEditor::_bake_task(void *p_userdata) {
	BakeJob *job = static_cast<BakeJob *>(p_userdata);
	job->result = lrt::bake_transfer_instance(job->input, &job->cancelled, &job->evaluated, &job->completed_variants);
}

void LRTRepresentationEditor::_cancel_pressed() {
	if (job != nullptr) {
		job->cancelled.store(true);
		status->set_text(TTR("Cancelling..."));
	} else {
		capture_pending = false;
		checked_status = true;
		status->set_text(TTR("Cancelled"));
		update_property();
		Dictionary report;
		report["ok"] = false;
		report["cancelled"] = true;
		emit_signal("bake_finished", report);
	}
}

void LRTRepresentationEditor::_finish_bake() {
	WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
	task_id = WorkerThreadPool::INVALID_TASK_ID;
	Node3D *target = Object::cast_to<Node3D>(ObjectDB::get_instance(job->owner));
	const uint64_t input_signature = job->input.input_signature;
	const Dictionary result = job->result;
	job.reset();
	checked_status = true;
	if (!bool(result.get("ok", false))) {
		status->set_text(bool(result.get("cancelled", false)) ? TTR("Cancelled") : String(result.get("error", TTR("LRT bake failed."))));
		update_property();
		emit_signal("bake_finished", result);
		return;
	}
	if (target == nullptr || target != get_edited_object() || lrt::geometry_representation(target) != lrt::GEOMETRY_PRECOMPUTED_TRANSFER ||
			uint64_t(int64_t(LRTVolume3D::get_transfer_instance_status(target).get("input_signature", int64_t(0)))) != input_signature) {
		status->set_text(TTR("Inputs changed; bake result was not applied."));
		update_property();
		Dictionary discarded;
		discarded["ok"] = false;
		discarded["error"] = "Inputs changed; bake result was not applied.";
		emit_signal("bake_finished", discarded);
		return;
	}
	const Variant previous = target->has_meta(lrt::TRANSFER_MANIFEST_META) ? target->get_meta(lrt::TRANSFER_MANIFEST_META) : Variant();
	EditorUndoRedoManager *undo = EditorUndoRedoManager::get_singleton();
	undo->create_action(TTR("Apply LRT Bake"), UndoRedo::MERGE_DISABLE, target);
	undo->add_do_method(target, "set_meta", lrt::TRANSFER_MANIFEST_META, result["manifest"]);
	undo->add_undo_method(target, "set_meta", lrt::TRANSFER_MANIFEST_META, previous);
	undo->add_do_method(target, "notify_property_list_changed");
	undo->add_undo_method(target, "notify_property_list_changed");
	undo->commit_action();
	checked_status = false;
	update_property();
	emit_signal("bake_finished", result);
}

void LRTRepresentationEditor::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		poll();
	}
}

void LRTRepresentationEditor::poll() {
	if (capture_pending) {
		_start_bake();
		return;
	}
	if (job == nullptr) {
		return;
	}
	if (WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
		_finish_bake();
		return;
	}
	if (!job->cancelled.load()) {
		const int total = int(job->input.variants.size());
		const int current = MIN(job->completed_variants.load() + 1, total);
		status->set_text(vformat(TTR("Baking variant %d/%d (%d samples)"), current, total, job->evaluated.load()));
	}
}

bool LRTGeometryInspectorPlugin::can_handle(Object *p_object) {
	return Object::cast_to<MeshInstance3D>(p_object) || Object::cast_to<MultiMeshInstance3D>(p_object);
}

bool LRTGeometryInspectorPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	if (p_path != "gi_lightmap_texel_scale") {
		return false;
	}
	EditorInspectorSection *section = memnew(EditorInspectorSection);
	section->setup("global_illumination/lrt", TTR("LRT"), p_object, EditorNode::get_singleton()->get_editor_theme()->get_color(SNAME("prop_subsection"), EditorStringName(Editor)), true, 0, 2);
	LRTRepresentationEditor *representation = memnew(LRTRepresentationEditor);
	representation->set_object_and_property(p_object, "metadata/_lrt_representation");
	section->get_vbox()->add_child(representation);
	const auto add_override = [&](const char *p_metadata, const char *p_project, const String &p_label, bool p_integer = false) {
		LRTNumericOverrideEditor *editor = memnew(LRTNumericOverrideEditor);
		editor->configure(p_metadata, p_project, p_label, p_integer);
		editor->set_object_and_property(p_object, "metadata/" + String(p_metadata));
		section->get_vbox()->add_child(editor);
		editor->update_property();
	};
	add_override(lrt::TRANSFER_RADIUS_META, "rendering/global_illumination/lrt/transfer/support_radius", TTR("Local Support Radius"));
	if (lrt::geometry_representation(p_object) == lrt::GEOMETRY_VOXEL) {
		add_override(lrt::VOXEL_CELL_SIZE_META, "rendering/global_illumination/lrt/voxel/cell_size", TTR("Voxel Cell Size"));
	} else {
		add_override(lrt::TRANSFER_SPACING_META, "rendering/global_illumination/lrt/transfer/sample_spacing", TTR("Sample Spacing"));
		add_override(lrt::TRANSFER_ANGULAR_SAMPLES_META, "rendering/global_illumination/lrt/transfer/angular_samples", TTR("Angular Samples"), true);
		add_override(lrt::OCCUPANCY_BAND_WIDTH_META, "rendering/global_illumination/lrt/transfer/occupancy_band_width", TTR("Occupancy Band Width"));
	}
	// Append after the native GI properties, inside their existing section.
	add_property_editor(p_path, section, true);
	representation->update_property();
	return false;
}

// Upper bound of lattice lines drawn per axis. Beyond it the lattice is sampled instead of
// dropped, so a small `spacing` still shows the probe grid (every Nth probe plane).
constexpr int LRT_GIZMO_MAX_DIVISIONS = 32;
// Share of the shortest side used for the centre cross, which keeps the node clickable even
// when the volume is small.
constexpr real_t LRT_GIZMO_CENTER_CROSS_RATIO = 0.1;
LRTVolumeGizmoPlugin::LRTVolumeGizmoPlugin() {
	helper.instantiate();

	// Same default colour as ReflectionProbe, so the two volumes look alike in the viewport.
	const Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/reflection_probe");
	create_material("volume_material", gizmo_color);

	Color disabled_color = gizmo_color;
	disabled_color.a = 0.25;
	create_material("volume_disabled_material", disabled_color);

	Color internal_color = gizmo_color;
	internal_color.a = 0.04;
	create_material("volume_internal_material", internal_color);

	create_handle_material("handles");
}

bool LRTVolumeGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LRTVolume3D>(p_spatial) != nullptr;
}

String LRTVolumeGizmoPlugin::get_gizmo_name() const {
	return "LRTVolume3D";
}

int LRTVolumeGizmoPlugin::get_priority() const {
	return -1;
}

String LRTVolumeGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	if (p_id == 6) {
		return "Blend Distance";
	}
	return helper->box_get_handle_name(p_id);
}

Variant LRTVolumeGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	if (p_id == 6) {
		return volume->get_blend_distance();
	}
	return volume->get_size();
}

void LRTVolumeGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	if (p_id == 6) {
		return;
	}
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), volume->get_global_transform());
	// A drag is a live resize: no bake per mouse move, exactly one when the drag ends.
	volume->set_rebuild_suppressed(true);
}

void LRTVolumeGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());

	Vector3 segment[2];
	helper->get_segment(p_camera, p_point, segment);
	if (p_id == 6) {
		const Vector3 size = volume->get_size();
		const Vector3 axis(1.0, 0.0, 0.0);
		Vector3 closest_axis;
		Vector3 closest_ray;
		Geometry3D::get_closest_points_between_segments(-axis * 16384.0, axis * 16384.0,
				segment[0], segment[1], closest_axis, closest_ray);
		real_t distance = size.x * 0.5 - closest_axis.x;
		if (Node3DEditor::get_singleton()->is_snap_enabled()) {
			distance = Math::snapped(distance, Node3DEditor::get_singleton()->get_translate_snap());
		}
		volume->set_blend_distance(distance);
		return;
	}

	Vector3 size = volume->get_size();
	Vector3 position;
	helper->box_set_handle(segment, p_id, size, position);
	volume->set_size(size);
	volume->set_global_position(position);
}

void LRTVolumeGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	if (p_id == 6) {
		const double restore = p_restore;
		if (p_cancel) {
			volume->set_blend_distance(restore);
			return;
		}
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action(TTR("Change LRT Blend Distance"));
		undo_redo->add_do_method(volume, "set_blend_distance", volume->get_blend_distance());
		undo_redo->add_undo_method(volume, "set_blend_distance", restore);
		undo_redo->commit_action();
		return;
	}
	helper->box_commit_handle(TTR("Change LRT Volume Size"), p_cancel, volume, volume, SNAME("global_position"), SNAME("size"));
	// Releasing the handle releases the bake: the deferred rebuild runs once for the final box.
	volume->set_rebuild_suppressed(false);
}

void LRTVolumeGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();

	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	if (volume == nullptr || !p_gizmo->is_selected()) {
		return;
	}
	const Vector3 size = volume->get_size();
	const AABB aabb(-size / 2.0, size);
	const real_t blend_distance = volume->get_blend_distance();
	const Vector3 blend_size = (size - Vector3(blend_distance * 2.0, blend_distance * 2.0, blend_distance * 2.0)).max(Vector3());
	const AABB blend_aabb(-blend_size / 2.0, blend_size);

	Vector<Vector3> lines;
	for (int i = 0; i < 12; i++) {
		Vector3 a;
		Vector3 b;
		aabb.get_edge(i, a, b);
		lines.push_back(a);
		lines.push_back(b);
	}
	if (blend_distance > 0.0) {
		for (int i = 0; i < 12; i++) {
			Vector3 a;
			Vector3 b;
			blend_aabb.get_edge(i, a, b);
			lines.push_back(a);
			lines.push_back(b);
		}
		for (int i = 0; i < 8; i++) {
			lines.push_back(aabb.get_endpoint(i));
			lines.push_back(blend_aabb.get_endpoint(i));
		}
	}

	// Centre cross: the volume is the probe region around the node, and this keeps the node
	// clickable in the viewport even when the box is small.
	const real_t cross = MIN(size.x, MIN(size.y, size.z)) * LRT_GIZMO_CENTER_CROSS_RATIO;
	for (int axis = 0; axis < 3; axis++) {
		Vector3 from;
		Vector3 to;
		from[axis] = -cross;
		to[axis] = cross;
		lines.push_back(from);
		lines.push_back(to);
	}

	// The probe lattice the solver actually bakes, so `spacing` is visible in the editor. A
	// small `spacing` would flood the viewport, so past the cap only every Nth plane is drawn:
	// those lines still sit on real probe planes, the density simply stops growing.
	Vector<Vector3> internal_lines;
	const real_t spacing = MAX(real_t(volume->get_probe_spacing()), real_t(0.001));
	int divisions[3] = { 0, 0, 0 };
	for (int axis = 0; axis < 3; axis++) {
		divisions[axis] = MAX(1, int(Math::round(size[axis] / spacing)));
	}
	for (int axis = 0; axis < 3; axis++) {
		const int next_1 = (axis + 1) % 3;
		const int next_2 = (axis + 2) % 3;
		const int stride = MAX(1, (divisions[axis] + LRT_GIZMO_MAX_DIVISIONS - 1) / LRT_GIZMO_MAX_DIVISIONS);
		for (int i = stride; i < divisions[axis]; i += stride) {
			const real_t offset = aabb.position[axis] + i * spacing;
			for (int corner = 0; corner < 4; corner++) {
				Vector3 from = aabb.position;
				Vector3 to = aabb.position;
				from[axis] = offset;
				to[axis] = offset;
				to[corner & 1 ? next_1 : next_2] += size[corner & 1 ? next_1 : next_2];
				if (corner & 2) {
					from[next_1] += size[next_1];
					from[next_2] += size[next_2];
				}
				internal_lines.push_back(from);
				internal_lines.push_back(to);
			}
		}
	}

	p_gizmo->add_lines(lines, get_material(volume->is_visible_in_tree() ? "volume_material" : "volume_disabled_material", p_gizmo));
	p_gizmo->add_lines(internal_lines, get_material("volume_internal_material", p_gizmo));
	Vector<Vector3> handles = helper->box_get_handles(size);
	handles.push_back(Vector3(size.x * 0.5 - blend_distance, 0.0, 0.0));
	p_gizmo->add_handles(handles, get_material("handles"));
}

void LRTEditorPlugin::_rebuild_pressed() {
	if (volume != nullptr) {
		volume->rebuild();
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
		_update_toolbar();
	}
}

void LRTEditorPlugin::_update_toolbar() {
	if (volume == nullptr || !toolbar->is_visible()) {
		return;
	}
	const String state = volume->get_editor_build_state();
	rebuild_button->set_text(vformat(TTR("Rebuild LRT (%s)"), state));
	rebuild_button->set_tooltip_text(volume->get_editor_build_tooltip());

}

void LRTEditorPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		_update_toolbar();
	}
}

void LRTEditorPlugin::edit(Object *p_object) {
	volume = Object::cast_to<LRTVolume3D>(p_object);
	_update_toolbar();
}

bool LRTEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<LRTVolume3D>(p_object) != nullptr;
}

void LRTEditorPlugin::make_visible(bool p_visible) {
	toolbar->set_visible(p_visible);
	set_process(p_visible);
	if (!p_visible) {
		volume = nullptr;
	} else {
		_update_toolbar();
	}
}

LRTEditorPlugin::LRTEditorPlugin() {
	inspector_plugin.instantiate();
	add_inspector_plugin(inspector_plugin);
	export_plugin.instantiate();
	EditorExport::get_singleton()->add_export_plugin(export_plugin);
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);


	toolbar = memnew(HBoxContainer);
	toolbar->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	toolbar->hide();

	rebuild_button = memnew(Button);
	rebuild_button->set_theme_type_variation(SceneStringName(FlatButton));
	rebuild_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	rebuild_button->set_text(TTR("Rebuild LRT"));
	rebuild_button->connect(SceneStringName(pressed), callable_mp(this, &LRTEditorPlugin::_rebuild_pressed));
	toolbar->add_child(rebuild_button);


	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, toolbar);
}

LRTEditorPlugin::~LRTEditorPlugin() {
	remove_inspector_plugin(inspector_plugin);
	EditorExport::get_singleton()->remove_export_plugin(export_plugin);
}

#endif // TOOLS_ENABLED
