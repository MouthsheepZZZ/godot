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

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/gui/editor_validation_panel.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/main/scene_tree.h"

bool LRTVolumeGizmoPlugin::has_gizmo(Node3D *p_node) {
	return Object::cast_to<LRTVolume3D>(p_node) != nullptr;
}

String LRTVolumeGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant LRTVolumeGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	return volume->get_size();
}

void LRTVolumeGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
}

void LRTVolumeGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
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
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());

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
		p_gizmo->add_lines(lines, get_material(volume->is_visible_in_tree() ? "volume" : "disabled", p_gizmo));
		if (!Object::cast_to<LRTGI>(volume)) {
			p_gizmo->add_handles(helper->box_get_handles(volume->get_size()), get_material("handles"));
		}
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
	if (p_path == "gi_lightmap_texel_scale") {
		GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(p_object);
		EditorInspectorSection *section = memnew(EditorInspectorSection);
		section->setup("Global Illumination/LRT", "LRT", instance, Color(), true, 0, 2);
		section->get_vbox()->add_child(memnew(LRTBindingInspector(instance, plugin)));
		add_property_editor(p_path, section, true);
	}
	return p_path == "metadata/" + LRTGeometryBinding::get_meta_name();
}

void LRTSurfaceNormalBiasEditor::_set_read_only(bool p_read_only) {
	override_checkbox->set_disabled(p_read_only);
	spin->set_read_only(p_read_only || !override_checkbox->is_pressed());
}

void LRTSurfaceNormalBiasEditor::_value_changed(double p_value) {
	emit_changed(get_edited_property(), p_value);
}

void LRTSurfaceNormalBiasEditor::_override_toggled(bool p_enabled) {
	emit_changed(get_edited_property(), p_enabled ? float(GLOBAL_GET("rendering/global_illumination/lrt/surface_normal_bias")) : -1.0f);
	update_property();
}

void LRTSurfaceNormalBiasEditor::update_property() {
	const float bias = get_edited_property_value();
	override_checkbox->set_pressed_no_signal(bias >= 0.0f);
	spin->set_value_no_signal(bias >= 0.0f ? bias : float(GLOBAL_GET("rendering/global_illumination/lrt/surface_normal_bias")));
	_set_read_only(is_read_only());
}

LRTSurfaceNormalBiasEditor::LRTSurfaceNormalBiasEditor() {
	HBoxContainer *controls = memnew(HBoxContainer);
	add_child(controls);
	override_checkbox = memnew(CheckBox);
	override_checkbox->set_tooltip_text(TTR("Override the project-wide surface normal bias."));
	controls->add_child(override_checkbox);
	add_focusable(override_checkbox);
	override_checkbox->connect("toggled", callable_mp(this, &LRTSurfaceNormalBiasEditor::_override_toggled));
	spin = memnew(EditorSpinSlider);
	spin->set_min(0);
	spin->set_max(1);
	spin->set_step(0.001);
	spin->set_allow_greater(true);
	spin->set_suffix("m");
	spin->set_flat(true);
	spin->set_control_state(EditorSpinSlider::CONTROL_STATE_HIDE);
	spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	controls->add_child(spin);
	add_focusable(spin);
	set_tooltip_text(TTR("Override the project-wide surface normal bias for this instance. Uncheck to inherit the project setting. The offset is in world-space meters; 0 disables it. No rebake is required."));
	spin->connect("value_changed", callable_mp(this, &LRTSurfaceNormalBiasEditor::_value_changed));
}

LRTBindingInspector::LRTBindingInspector(GeometryInstance3D *p_instance, LRTEditorPlugin *p_plugin) {
	target = p_instance->get_instance_id();
	plugin = p_plugin;
	config.instantiate();
	external_contribution = EditorInspector::instantiate_property_editor(config.ptr(), Variant::INT, "external_contribution", PROPERTY_HINT_ENUM, "Inherit,Disabled,Enabled", PROPERTY_USAGE_DEFAULT);
	external_contribution->set_label(TTR("External Contribution"));
	external_contribution->set_tooltip_text(TTR("Controls geometry contribution to other calculation groups, not this group or lighting reception. Inherit uses the owning LRTGI default for the current GI mode."));
	normal_bias = memnew(LRTSurfaceNormalBiasEditor);
	normal_bias->set_label(TTR("Surface Normal Bias"));
	resolution = EditorInspector::instantiate_property_editor(config.ptr(), Variant::INT, "sdf_resolution", PROPERTY_HINT_RANGE, "6,256,1,or_greater", PROPERTY_USAGE_DEFAULT);
	resolution->set_label(TTR("SDF Resolution"));
	resolution->set_tooltip_text(TTR("Number of Color SDF samples along the longest axis, including padding. Other axes are proportional. Changes require rebaking."));
	data = EditorInspector::instantiate_property_editor(config.ptr(), Variant::OBJECT, "bake_data", PROPERTY_HINT_RESOURCE_TYPE, "LRTInstanceGeometryData", PROPERTY_USAGE_DEFAULT);
	data->set_label(TTR("Bake Data"));
	data->set_tooltip_text(TTR("Baked object-space Color SDF. Mesh or material changes require rebaking."));
	EditorProperty *properties[] = { normal_bias, external_contribution, resolution, data };
	const char *names[] = { "surface_normal_bias", "external_contribution", "sdf_resolution", "bake_data" };
	for (int i = 0; i < 4; i++) {
		properties[i]->set_object_and_property(config.ptr(), names[i]);
		properties[i]->update_property();
		properties[i]->update_cache();
		properties[i]->set_doc_path("class_property:LRTGeometryBinding:" + String(names[i]));
		properties[i]->connect("property_changed", callable_mp(this, &LRTBindingInspector::_property_changed));
		add_child(properties[i]);
	}
	data->connect("resource_selected", callable_mp(this, &LRTBindingInspector::_resource_selected));
	bake = memnew(Button);
	bake->set_text(TTR("Bake Color SDF"));
	bake->connect("pressed", callable_mp(this, &LRTBindingInspector::_bake));
	add_child(bake);
	warnings = memnew(EditorValidationPanel);
	warnings->add_line(EditorValidationPanel::MSG_ID_DEFAULT);
	add_child(warnings);
	_refresh();
	set_process(true);
}

void LRTBindingInspector::_refresh() {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	if (!instance) {
		return;
	}
	const Ref<LRTGeometryBinding> current = LRTGeometryBinding::get_binding(instance);
	if (current != binding) {
		binding = current;
		config = binding.is_valid() ? binding : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
	}
	EditorProperty *properties[] = { normal_bias, external_contribution, resolution, data };
	const char *names[] = { "surface_normal_bias", "external_contribution", "sdf_resolution", "bake_data" };
	for (int i = 0; i < 4; i++) {
		properties[i]->set_object_and_property(config.ptr(), names[i]);
		if (!properties[i]->is_cache_valid() || (i == 0 && config->get_surface_normal_bias() < 0.0f)) {
			properties[i]->update_editor_property_status();
			properties[i]->update_property();
			properties[i]->update_cache();
		}
	}
	const bool dynamic = LRTGeometryBinding::uses_sdf(instance);
	external_contribution->set_visible(instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED && LRTVolume3D::find_geometry_owner(instance));
	resolution->set_visible(dynamic);
	data->set_visible(dynamic);
	bake->set_visible(dynamic);
	bake->set_disabled(plugin->is_baking());
	const String message = dynamic ? String("\n").join(config->get_configuration_warnings(instance)) : String();
	warnings->set_message(EditorValidationPanel::MSG_ID_DEFAULT, message, EditorValidationPanel::MSG_WARNING);
	warnings->set_visible(!message.is_empty());
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

void LRTBindingInspector::_property_changed(const StringName &p_property, const Variant &p_value, const StringName &p_field, bool p_changing) {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
	ERR_FAIL_NULL(instance);
	Ref<LRTGeometryBinding> replacement = config->duplicate();
	replacement->set(p_property, p_value);
	plugin->commit_binding(instance, replacement);
	_refresh();
}

void LRTBindingInspector::_resource_selected(const String &p_property, const Ref<Resource> &p_resource) {
	EditorInterface::get_singleton()->edit_resource(p_resource);
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

void LRTEditorPlugin::edit(Object *p_object) {
	geometry_instance = LRTGeometryBinding::supports(p_object) ? Object::cast_to<GeometryInstance3D>(p_object) : nullptr;
	volume = Object::cast_to<LRTVolume3D>(p_object);
	_update_bake_button();
	_apply_debug();
}

bool LRTEditorPlugin::handles(Object *p_object) const {
	return LRTGeometryBinding::supports(p_object) || Object::cast_to<LRTVolume3D>(p_object) != nullptr;
}

void LRTEditorPlugin::make_visible(bool p_visible) {
	if (!p_visible) {
		geometry_instance = nullptr;
		volume = nullptr;
		_apply_debug();
	}
	_update_bake_button();
}

void LRTEditorPlugin::_update_bake_button() {
	const bool active = bake_task.is_valid();
	const bool group = volume && !Object::cast_to<LRTDetailVolume3D>(volume);
	bake_toolbar->set_visible(active || group || geometry_instance);
	bake_button->set_visible(active || group);
	bake_button->set_disabled(active);
	bake_button->set_text(active ? TTR("Baking LRT") : TTR("Bake LRT Volume"));
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
	if (bake_task.is_valid()) {
		if (!bake_task->is_completed()) {
			bake_task->cancel();
		}
		return;
	}
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

struct LRTEditorPlugin::SaveJob {
	Ref<Resource> resource;
	String path;
	Error error = OK;
	WorkerThreadPool::TaskID worker = WorkerThreadPool::INVALID_TASK_ID;

	static void save(void *p_userdata) {
		SaveJob *job = static_cast<SaveJob *>(p_userdata);
		const Ref<LocalLRTVolumeData> group = job->resource;
		if (group.is_valid()) {
			const Dictionary data = group->get_data();
			HashSet<ObjectID> saved;
			for (const String &field : { String("group_details"), String("group_sdfs") }) {
				const Array resources = data.get(field, Array());
				for (int index = 0; index < resources.size(); index++) {
					const Ref<Resource> resource = resources[index];
					if (saved.has(resource->get_instance_id())) {
						continue;
					}
					String path = resource->get_path();
					if (!path.is_resource_file() || !path.ends_with(".res")) {
						int suffix = index;
						do {
							path = job->path.get_basename() + "." + field.trim_prefix("group_") + "_" + itos(suffix++) + ".res";
						} while (FileAccess::exists(path));
					}
					job->error = ResourceSaver::save(resource, path, ResourceSaver::FLAG_COMPRESS);
					if (job->error != OK) {
						return;
					}
					resource->set_path(path, true);
					saved.insert(resource->get_instance_id());
				}
			}
		}
		job->error = ResourceSaver::save(job->resource, job->path, ResourceSaver::FLAG_COMPRESS);
	}
};

void LRTEditorPlugin::_bake_to_path(const String &p_path) {
	ERR_FAIL_COND(bake_task.is_valid());
	ERR_FAIL_COND(!geometry_instance && !volume);
	const ObjectID target = volume ? volume->get_instance_id() : geometry_instance->get_instance_id();
	Ref<LRTGeometryBinding> binding;
	Error save_error = OK;
	{
		EditorProgress progress("bake_lrt_geometry", volume ? TTR("Bake LRT Volume") : TTR("Bake LRT Color SDF"), 1, true);
		if (volume) {
			bake_task = volume->bake(get_tree()->get_edited_scene_root());
		} else {
			const Ref<LRTGeometryBinding> current = LRTGeometryBinding::get_binding(geometry_instance);
			binding = current.is_valid() ? Ref<LRTGeometryBinding>(current->duplicate()) : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
			bake_task = binding->bake(geometry_instance);
		}
		_update_bake_button();
		while (!bake_task->is_completed()) {
			const Dictionary state = bake_task->get_progress();
			if (progress.step(vformat(TTR("Baking LRT: %d sources, %d slices"), state["completed_sources"], state["completed_slices"]), 0)) {
				bake_task->cancel();
			}
			OS::get_singleton()->delay_usec(1000);
		}
	}
	const LRTBakeTask::Status status = bake_task->get_status();
	if (status == LRTBakeTask::SUCCEEDED || status == LRTBakeTask::COMPLETED_WITH_ISSUES) {
		if (binding.is_valid()) {
			auto *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(target));
			if (instance) {
				commit_binding(instance, binding);
			}
		}
		EditorInterface::get_singleton()->mark_scene_as_unsaved();
		EditorProgress progress("save_lrt_geometry", TTR("Saving LRT Bake"), 1);
		SaveJob job;
		job.resource = bake_task->get_result();
		job.path = p_path;
		job.worker = WorkerThreadPool::get_singleton()->add_native_task(&SaveJob::save, &job, false, "Save LRT bake resource");
		while (!WorkerThreadPool::get_singleton()->is_task_completed(job.worker)) {
			progress.step(TTR("Saving LRT Bake"), 0);
			OS::get_singleton()->delay_usec(1000);
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(job.worker);
		save_error = job.error;
		if (save_error == OK) {
			job.resource->set_path(ProjectSettings::get_singleton()->localize_path(p_path), true);
		}
	}
	if (status == LRTBakeTask::FAILED) {
		const Dictionary state = bake_task->get_progress();
		EditorNode::get_singleton()->show_warning(vformat(TTR("LRT geometry baking failed (%s). The previous result is unchanged."), state["error_message"]));
	} else if (save_error != OK) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("LRT baking succeeded, but saving failed (%s). The in-memory result is still available."), error_names[save_error]));
	} else if (status == LRTBakeTask::COMPLETED_WITH_ISSUES) {
		EditorNode::get_singleton()->show_warning(vformat(TTR("LRT baking completed with %d resource issues. Some contributions are missing. See bake_issues for node, instance, surface and pass details."), bake_task->get_issues().size()));
	}
	bake_task.unref();
	_update_bake_button();
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
