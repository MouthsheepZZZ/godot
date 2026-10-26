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
/* The above copyright notice and this permission notice shall be         */
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

#include "lrt_editor_plugin.h"

#include "../local_lrt_volume_3d.h"
#include "../lrt_geometry_binding.h"
#include "../lrt_runtime.h"

#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/spin_box.h"
#include "scene/main/scene_tree.h"

bool LRTVolumeGizmoPlugin::has_gizmo(Node3D *p_node) {
	return Object::cast_to<LocalLRTVolume3D>(p_node) != nullptr;
}

String LRTVolumeGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant LRTVolumeGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
	return volume->get_size();
}

void LRTVolumeGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
}

void LRTVolumeGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
	Vector3 segment[2];
	helper->get_segment(p_camera, p_point, segment);

	Vector3 size = volume->get_size();
	Vector3 position;
	helper->box_set_handle(segment, p_id, size, position);
	volume->set_size(size);
	volume->set_global_position(position);
}

void LRTVolumeGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	helper->box_commit_handle(TTR("Change LRT Volume Size"), p_cancel, p_gizmo->get_node_3d());
}

void LRTVolumeGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());

	if (p_gizmo->is_selected()) {
		const AABB bounds = volume->get_bounds();
		Vector<Vector3> lines;
		for (int edge = 0; edge < 12; edge++) {
			Vector3 from;
			Vector3 to;
			bounds.get_edge(edge, from, to);
			lines.push_back(from);
			lines.push_back(to);
		}
		p_gizmo->add_lines(lines, get_material(volume->is_enabled() ? "volume" : "disabled", p_gizmo));
		p_gizmo->add_handles(helper->box_get_handles(volume->get_size()), get_material("handles"));
	}
	p_gizmo->add_unscaled_billboard(get_material("volume_icon", p_gizmo), 0.05);
}

LRTVolumeGizmoPlugin::LRTVolumeGizmoPlugin() {
	helper.instantiate();
	create_material("volume", Color(0.25, 0.8, 1.0));
	create_material("disabled", Color(0.5, 0.5, 0.5));
	create_icon_material("volume_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoVoxelGI"), EditorStringName(EditorIcons)));
	create_handle_material("handles");
}

bool LRTInspectorPlugin::can_handle(Object *p_object) {
	return LRTGeometryBinding::supports(p_object);
}

bool LRTInspectorPlugin::parse_property(Object *p_object, Variant::Type p_type, const String &p_path, PropertyHint p_hint, const String &p_hint_text, BitField<PropertyUsageFlags> p_usage, bool p_wide) {
	return p_path == "metadata/" + LRTGeometryBinding::get_meta_name();
}

void LRTInspectorPlugin::parse_group(Object *p_object, const String &p_group) {
	if (p_group != "Global Illumination") {
		return;
	}
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(p_object);
	EditorInspectorSection *section = memnew(EditorInspectorSection);
	section->setup("Global Illumination/LRT", "LRT", instance, Color(), true, 1, 2);
	section->get_vbox()->add_child(memnew(LRTBindingInspector(instance, plugin)));
	add_custom_control(section);
}

LRTBindingInspector::LRTBindingInspector(GeometryInstance3D *p_instance, LRTEditorPlugin *p_plugin) {
	target = p_instance->get_instance_id();
	plugin = p_plugin;
	HBoxContainer *bias_row = memnew(HBoxContainer);
	add_child(bias_row);
	Label *bias_label = memnew(Label);
	bias_label->set_text(TTR("Surface Normal Bias"));
	bias_label->set_h_size_flags(SIZE_EXPAND_FILL);
	bias_row->add_child(bias_label);
	normal_bias = memnew(SpinBox);
	normal_bias->set_min(-1.0);
	normal_bias->set_max(1.0);
	normal_bias->set_allow_greater(true);
	normal_bias->set_step(0.001);
	normal_bias->set_suffix("m");
	normal_bias->set_tooltip_text(TTR("World-space surface sampling offset. -1 inherits rendering/lrt/surface_normal_bias. 0 disables the offset for this instance."));
	bias_row->add_child(normal_bias);
	normal_bias->connect("value_changed", callable_mp(this, &LRTBindingInspector::_normal_bias_changed));
	HBoxContainer *row = memnew(HBoxContainer);
	add_child(row);
	Label *label = memnew(Label);
	label->set_text(TTR("SDF Resolution"));
	label->set_h_size_flags(SIZE_EXPAND_FILL);
	row->add_child(label);
	resolution = memnew(SpinBox);
	resolution->set_min(6);
	resolution->set_max(256);
	resolution->set_allow_greater(true);
	resolution->set_step(1);
	row->add_child(resolution);
	resolution->connect("value_changed", callable_mp(this, &LRTBindingInspector::_resolution_changed));
	label = memnew(Label);
	label->set_text(TTR("Bake Data"));
	add_child(label);
	data = memnew(EditorResourcePicker);
	data->set_base_type("LRTInstanceGeometryData");
	add_child(data);
	data->connect("resource_changed", callable_mp(this, &LRTBindingInspector::_data_changed));
	data->connect("resource_selected", callable_mp(this, &LRTBindingInspector::_data_selected));
	Button *bake = memnew(Button);
	bake->set_text(TTR("Bake LRT Color SDF"));
	bake->connect("pressed", callable_mp(this, &LRTBindingInspector::_bake));
	add_child(bake);
	warnings = memnew(Label);
	warnings->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(warnings);
	_refresh();
	set_process(true);
}

void LRTBindingInspector::_refresh() {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	if (!instance) {
		return;
	}
	binding = LRTGeometryBinding::get_binding(instance);
	const Ref<LRTGeometryBinding> config = binding.is_valid() ? binding : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
	normal_bias->set_block_signals(true);
	normal_bias->set_value(config->get_surface_normal_bias());
	normal_bias->set_block_signals(false);
	resolution->set_block_signals(true);
	resolution->set_value(config->get_sdf_resolution());
	resolution->set_block_signals(false);
	data->set_edited_resource(config->get_bake_data());
	warnings->set_text(String("\n").join(config->get_configuration_warnings(instance)));
}

void LRTBindingInspector::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS) {
		elapsed += get_process_delta_time();
		if (elapsed >= 0.5) {
			elapsed = 0;
			_refresh();
		}
	}
}

void LRTBindingInspector::_normal_bias_changed(double p_value) {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	ERR_FAIL_NULL(instance);
	Ref<LRTGeometryBinding> replacement = binding.is_valid() ? Ref<LRTGeometryBinding>(binding->duplicate()) : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
	replacement->set_surface_normal_bias(p_value < 0.0 ? -1.0f : float(p_value));
	plugin->commit_binding(instance, replacement);
	_refresh();
}

void LRTBindingInspector::_resolution_changed(double p_value) {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	ERR_FAIL_NULL(instance);
	Ref<LRTGeometryBinding> replacement = binding.is_valid() ? Ref<LRTGeometryBinding>(binding->duplicate()) : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
	replacement->set_sdf_resolution(int(p_value));
	plugin->commit_binding(instance, replacement);
	_refresh();
}

void LRTBindingInspector::_data_changed(const Ref<Resource> &p_data) {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	ERR_FAIL_NULL(instance);
	Ref<LRTGeometryBinding> replacement = binding.is_valid() ? Ref<LRTGeometryBinding>(binding->duplicate()) : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
	replacement->set_bake_data(p_data);
	plugin->commit_binding(instance, replacement);
	_refresh();
}

void LRTBindingInspector::_data_selected(const Ref<Resource> &p_data, bool p_inspect) {
	EditorInterface::get_singleton()->edit_resource(p_data);
}

void LRTBindingInspector::_bake() {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	ERR_FAIL_NULL(instance);
	plugin->bake_instance(instance);
}

void LRTEditorPlugin::commit_binding(GeometryInstance3D *p_instance, const Ref<LRTGeometryBinding> &p_binding) {
	EditorUndoRedoManager *undo = get_undo_redo();
	undo->create_action(TTR("Change LRT Geometry Binding"), UndoRedo::MERGE_DISABLE, p_instance);
	undo->add_do_method(p_instance, "set_meta", LRTGeometryBinding::get_meta_name(), p_binding);
	if (p_instance->has_meta(LRTGeometryBinding::get_meta_name())) {
		undo->add_undo_method(p_instance, "set_meta", LRTGeometryBinding::get_meta_name(), p_instance->get_meta(LRTGeometryBinding::get_meta_name()));
	} else {
		undo->add_undo_method(p_instance, "remove_meta", LRTGeometryBinding::get_meta_name());
	}
	undo->commit_action();
}

void LRTEditorPlugin::bake_instance(GeometryInstance3D *p_instance) {
	geometry_instance = p_instance;
	volume = nullptr;
	_bake();
}

EditorProgress *LRTEditorPlugin::bake_progress = nullptr;

void LRTEditorPlugin::edit(Object *p_object) {
	geometry_instance = LRTGeometryBinding::supports(p_object) ? Object::cast_to<GeometryInstance3D>(p_object) : nullptr;
	volume = Object::cast_to<LocalLRTVolume3D>(p_object);
	bake_button->set_text(TTR("Bake LRT Volume"));
	bake_button->set_visible(volume != nullptr);
	_apply_debug();
}

bool LRTEditorPlugin::handles(Object *p_object) const {
	return LRTGeometryBinding::supports(p_object) || Object::cast_to<LocalLRTVolume3D>(p_object) != nullptr;
}

void LRTEditorPlugin::make_visible(bool p_visible) {
	bake_toolbar->set_visible(p_visible);
	if (!p_visible) {
		geometry_instance = nullptr;
		volume = nullptr;
		_apply_debug();
	}
}

void LRTEditorPlugin::_apply_debug() {
	ObjectID selection;
	if (volume) {
		selection = volume->get_instance_id();
	} else if (geometry_instance) {
		selection = geometry_instance->get_instance_id();
	}
	LRTRuntime::get_singleton()->set_debug_selection(selection);
}

void LRTEditorPlugin::_bake() {
	ERR_FAIL_COND(!geometry_instance && !volume);
	Ref<Resource> baked_data;
	Node3D *target;
	if (volume) {
		baked_data = volume->get_bake_data();
		target = volume;
	} else {
		const Ref<LRTGeometryBinding> binding = LRTGeometryBinding::get_binding(geometry_instance);
		if (binding.is_valid()) {
			baked_data = binding->get_bake_data();
		}
		target = geometry_instance;
	}
	if (baked_data.is_valid() && baked_data->get_path().is_resource_file() && baked_data->get_path().ends_with(".res")) {
		_bake_to_path(baked_data->get_path());
		return;
	}
	const String scene_path = get_tree()->get_edited_scene_root()->get_scene_file_path();
	const String base_path = scene_path.is_empty() ? "res://" : scene_path.get_basename() + ".";
	bake_file->set_current_path(base_path + String(target->get_name()) + (volume ? "_volume.res" : "_sdf.res"));
	bake_file->popup_file_dialog();
}

bool LRTEditorPlugin::_bake_step(int p_step, int p_total) {
	return bake_progress->step(TTR("Baking LRT geometry"), p_step * 100 / p_total, true);
}

void LRTEditorPlugin::_bake_to_path(const String &p_path) {
	ERR_FAIL_COND(!geometry_instance && !volume);
	EditorProgress progress("bake_lrt_geometry", volume ? TTR("Bake LRT Volume") : TTR("Bake LRT Color SDF"), 100, true);
	bake_progress = &progress;
	LocalLRTVolume3D::bake_step_function = _bake_step;
	Error error;
	if (volume) {
		error = volume->bake(p_path, get_tree()->get_edited_scene_root());
	} else {
		const Ref<LRTGeometryBinding> current = LRTGeometryBinding::get_binding(geometry_instance);
		Ref<LRTGeometryBinding> replacement = current.is_valid() ? Ref<LRTGeometryBinding>(current->duplicate()) : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
		error = replacement->bake(geometry_instance, p_path, _bake_step);
		if (error == OK) {
			commit_binding(geometry_instance, replacement);
		}
	}
	LocalLRTVolume3D::bake_step_function = nullptr;
	bake_progress = nullptr;
	if (error != OK && error != ERR_SKIP) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("LRT geometry baking failed (%s). See Output for details."), error_names[error]));
		return;
	}
	if (error == OK) {
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
	}
}

LRTEditorPlugin::LRTEditorPlugin() {
	inspector_plugin = Ref<LRTInspectorPlugin>(memnew(LRTInspectorPlugin(this)));
	add_inspector_plugin(inspector_plugin);
	Ref<LRTVolumeGizmoPlugin> gizmo_plugin;
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);

	bake_toolbar = memnew(HBoxContainer);
	bake_toolbar->hide();
	bake_button = memnew(Button);
	bake_button->set_text(TTR("Bake LRT Color SDF"));
	bake_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	bake_button->connect("pressed", callable_mp(this, &LRTEditorPlugin::_bake));
	bake_toolbar->add_child(bake_button);

	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bake_toolbar);

	bake_file = memnew(EditorFileDialog);
	bake_file->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	bake_file->add_filter("*.res");
	bake_file->set_title(TTR("Select LRT Bake Data File"));
	bake_file->connect("file_selected", callable_mp(this, &LRTEditorPlugin::_bake_to_path));
	EditorInterface::get_singleton()->get_base_control()->add_child(bake_file);
}

LRTEditorPlugin::~LRTEditorPlugin() {
	remove_inspector_plugin(inspector_plugin);
	memdelete(bake_toolbar);
	memdelete(bake_file);
}
