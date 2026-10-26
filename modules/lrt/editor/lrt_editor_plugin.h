/**************************************************************************/
/*  lrt_editor_plugin.h                                                   */
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

#pragma once

#include "../lrt_geometry_binding.h"

#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "scene/gui/box_container.h"

class Button;
class EditorFileDialog;
struct EditorProgress;
class Gizmo3DHelper;
class HBoxContainer;
class PopupPanel;
class SpinBox;
class OptionButton;
class GeometryInstance3D;
class EditorResourcePicker;
class Label;
class LRTEditorPlugin;
class LocalLRTVolume3D;

class LRTVolumeGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(LRTVolumeGizmoPlugin, EditorNode3DGizmoPlugin);

	Ref<Gizmo3DHelper> helper;

public:
	bool has_gizmo(Node3D *p_node) override;
	String get_gizmo_name() const override { return "LRT Volume"; }
	int get_priority() const override { return -1; }
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	String get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const override;
	Variant get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const override;
	void begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) override;
	void set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) override;
	void commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel = false) override;

	LRTVolumeGizmoPlugin();
};

class LRTBindingInspector : public VBoxContainer {
	GDCLASS(LRTBindingInspector, VBoxContainer);

	ObjectID target;
	LRTEditorPlugin *plugin = nullptr;
	Ref<LRTGeometryBinding> binding;
	SpinBox *resolution = nullptr;
	EditorResourcePicker *data = nullptr;
	Label *warnings = nullptr;
	double elapsed = 0;
	void _refresh();
	void _resolution_changed(double p_value);
	void _data_changed(const Ref<Resource> &p_data);
	void _data_selected(const Ref<Resource> &p_data, bool p_inspect);
	void _bake();

protected:
	void _notification(int p_what);

public:
	LRTBindingInspector(GeometryInstance3D *p_instance, LRTEditorPlugin *p_plugin);
};

class LRTInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(LRTInspectorPlugin, EditorInspectorPlugin);

	LRTEditorPlugin *plugin = nullptr;

public:
	bool can_handle(Object *p_object) override;
	void parse_group(Object *p_object, const String &p_group) override;
	bool parse_property(Object *p_object, Variant::Type p_type, const String &p_path, PropertyHint p_hint, const String &p_hint_text, BitField<PropertyUsageFlags> p_usage, bool p_wide) override;
	explicit LRTInspectorPlugin(LRTEditorPlugin *p_plugin) :
			plugin(p_plugin) {}
};

class LRTEditorPlugin : public EditorPlugin {
	GDCLASS(LRTEditorPlugin, EditorPlugin);

	GeometryInstance3D *geometry_instance = nullptr;
	Ref<LRTInspectorPlugin> inspector_plugin;
	LocalLRTVolume3D *volume = nullptr;
	Button *bake_button = nullptr;
	HBoxContainer *bake_toolbar = nullptr;
	EditorFileDialog *bake_file = nullptr;
	static EditorProgress *bake_progress;
	PopupPanel *debug_panel = nullptr;
	SpinBox *debug_probe[3] = {};
	SpinBox *debug_direction[3] = {};
	OptionButton *debug_axis = nullptr;
	SpinBox *debug_slice = nullptr;
	void _show_debug();
	void _apply_debug();

	void _bake();
	void _bake_to_path(const String &p_path);
	static bool _bake_step(int p_step, int p_total);

public:
	void commit_binding(GeometryInstance3D *p_instance, const Ref<LRTGeometryBinding> &p_binding);
	void bake_instance(GeometryInstance3D *p_instance);
	void edit(Object *p_object) override;
	bool handles(Object *p_object) const override;
	void make_visible(bool p_visible) override;
	String get_plugin_name() const override { return "LRT"; }

	LRTEditorPlugin();
	~LRTEditorPlugin();
};
