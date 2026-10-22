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

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/plugins/editor_plugin.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"
#include "core/object/worker_thread_pool.h"

#include <memory>

class Gizmo3DHelper;
class Button;
class HBoxContainer;
class LRTVolume3D;
class LRTExportPlugin;
class EditorSpinSlider;
class Label;
class OptionButton;

// Native optional-property editor backed by scene-persistent node metadata.
class LRTNumericOverrideEditor : public EditorProperty {
	GDCLASS(LRTNumericOverrideEditor, EditorProperty);

	EditorSpinSlider *spin = nullptr;
	Variant displayed_override;
	double displayed_default = -1.0;
	StringName metadata;
	String project_setting;
	bool integer = false;

	void _override_toggled(const StringName &p_property, bool p_enabled);
	void _value_changed(double p_value);
	void _commit_override(const Variant &p_value);

protected:
	void _notification(int p_what);
	void _set_read_only(bool p_read_only) override;

public:
	void configure(const StringName &p_metadata, const String &p_project_setting, const String &p_label, bool p_integer = false);
	void update_property() override;
	LRTNumericOverrideEditor();
};

class LRTRepresentationEditor : public EditorProperty {
	GDCLASS(LRTRepresentationEditor, EditorProperty);

	struct BakeJob;
	std::unique_ptr<BakeJob> job;
	WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
	OptionButton *mode = nullptr;
	Label *status = nullptr;
	Button *bake = nullptr;
	Button *check = nullptr;
	Button *cancel = nullptr;
	bool checked_status = false;
	bool capture_pending = false;

	void _mode_selected(int p_index);
	void _check_status();
	void _bake_pressed();
	void _start_bake();
	void _cancel_pressed();
	void _finish_bake();
	static void _bake_task(void *p_userdata);

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void _set_read_only(bool p_read_only) override;

public:
	void update_property() override;
	void poll();
	LRTRepresentationEditor();
	~LRTRepresentationEditor();
};

class LRTGeometryInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(LRTGeometryInspectorPlugin, EditorInspectorPlugin);

public:
	bool can_handle(Object *p_object) override;
	bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;
};

// Editor-side viewport control for LRTVolume3D, matching ReflectionProbe: the volume box is
// drawn as gizmo lines, the six face handles resize it (the probe region is centred on the
// node, so a face drag moves the node as well), and a seventh handle edits the shared
// six-face blend distance. The editor's undo/redo records both edits.
// While a handle is being dragged the volume must not restart its bake for every mouse move,
// so the drag suppresses the rebuild and the commit releases it.
class LRTVolumeGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(LRTVolumeGizmoPlugin, EditorNode3DGizmoPlugin);

	Ref<Gizmo3DHelper> helper;

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	String get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const override;
	Variant get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const override;
	void begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) override;
	void set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) override;
	void commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel = false) override;

	LRTVolumeGizmoPlugin();
};

// Adds the selected-volume rebuild button. Visual
// diagnostics remain viewport-owned draw modes in View > Display Advanced.
class LRTEditorPlugin : public EditorPlugin {
	GDCLASS(LRTEditorPlugin, EditorPlugin);

	Ref<LRTVolumeGizmoPlugin> gizmo_plugin;
	Ref<LRTGeometryInspectorPlugin> inspector_plugin;
	Ref<LRTExportPlugin> export_plugin;
	LRTVolume3D *volume = nullptr;
	HBoxContainer *toolbar = nullptr;
	Button *rebuild_button = nullptr;

	void _rebuild_pressed();
	void _update_toolbar();

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "LRTVolume3D"; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;

	LRTEditorPlugin();
	~LRTEditorPlugin();
};

#endif // TOOLS_ENABLED
