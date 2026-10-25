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

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"

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

LRTEditorPlugin::LRTEditorPlugin() {
	Ref<LRTVolumeGizmoPlugin> gizmo_plugin;
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
}
