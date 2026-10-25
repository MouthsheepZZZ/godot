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
#include "../lrt_geometry_binding_3d.h"

#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
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

EditorProgress *LRTEditorPlugin::bake_progress = nullptr;

void LRTEditorPlugin::edit(Object *p_object) {
	geometry_binding = Object::cast_to<LRTGeometryBinding3D>(p_object);
	volume = Object::cast_to<LocalLRTVolume3D>(p_object);
	bake_button->set_text(volume ? TTR("Bake LRT Volume") : TTR("Bake LRT Color SDF"));
}

bool LRTEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<LRTGeometryBinding3D>(p_object) != nullptr || Object::cast_to<LocalLRTVolume3D>(p_object) != nullptr;
}

void LRTEditorPlugin::make_visible(bool p_visible) {
	bake_toolbar->set_visible(p_visible);
	if (!p_visible) {
		geometry_binding = nullptr;
		volume = nullptr;
	}
}

void LRTEditorPlugin::_bake() {
	ERR_FAIL_COND(!geometry_binding && !volume);
	Ref<Resource> baked_data;
	Node3D *target;
	if (volume) {
		baked_data = volume->get_bake_data();
		target = volume;
	} else {
		baked_data = geometry_binding->get_bake_data();
		target = geometry_binding;
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
	ERR_FAIL_COND(!geometry_binding && !volume);
	EditorProgress progress("bake_lrt_geometry", volume ? TTR("Bake LRT Volume") : TTR("Bake LRT Color SDF"), 100, true);
	bake_progress = &progress;
	LRTGeometryBinding3D::bake_step_function = _bake_step;
	LocalLRTVolume3D::bake_step_function = _bake_step;
	const Error error = volume ? volume->bake(p_path, get_tree()->get_edited_scene_root()) : geometry_binding->bake(p_path);
	LRTGeometryBinding3D::bake_step_function = nullptr;
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
	memdelete(bake_toolbar);
	memdelete(bake_file);
}
