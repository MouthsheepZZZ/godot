/**************************************************************************/
/*  local_lrt_volume_3d_editor_plugin.cpp                                 */
/**************************************************************************/

#include "local_lrt_volume_3d_editor_plugin.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_file_dialog.h"
#include "scene/3d/local_lrt_volume_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/main/scene_tree.h"

void LocalLRTVolume3DEditorPlugin::_bake() {
	if (!volume) {
		return;
	}

	Ref<LocalLRTVolumeData> bake_data = volume->get_bake_data();
	if (bake_data.is_null()) {
		String path;
		if (get_tree()->get_edited_scene_root()) {
			path = get_tree()->get_edited_scene_root()->get_scene_file_path();
		}
		if (path.is_empty()) {
			path = "res://" + volume->get_name() + "_data.res";
		} else {
			path = path.get_basename() + "." + volume->get_name() + "_data.res";
		}
		probe_file->set_current_path(path);
		probe_file->popup_file_dialog();
		return;
	}

	const String path = bake_data->get_path();
	if (!path.is_resource_file()) {
		const int srpos = path.find("::");
		if (srpos != -1) {
			const String base = path.substr(0, srpos);
			if (ResourceLoader::get_resource_type(base) == "PackedScene") {
				if (!get_tree()->get_edited_scene_root() || get_tree()->get_edited_scene_root()->get_scene_file_path() != base) {
					EditorNode::get_singleton()->show_warning(TTR("Local LRT data is not local to the scene."));
					return;
				}
			} else if (FileAccess::exists(base + ".import")) {
				EditorNode::get_singleton()->show_warning(TTR("Local LRT data is part of an imported resource."));
				return;
			}
		}
	} else if (FileAccess::exists(path + ".import")) {
		EditorNode::get_singleton()->show_warning(TTR("Local LRT data is an imported resource."));
		return;
	}

	_bake_and_save();
}

void LocalLRTVolume3DEditorPlugin::_bake_and_save() {
	if (!volume) {
		return;
	}
	EditorProgress progress("bake_local_lrt", TTR("Bake LocalLRTVolume3D"), 1);
	progress.step(TTR("Building Local LRT data"), 0, false);
	volume->rebuild();
	Ref<LocalLRTVolumeData> bake_data = volume->get_bake_data();
	ERR_FAIL_COND(bake_data.is_null());
	const String path = bake_data->get_path();
	if (path.is_resource_file()) {
		ResourceSaver::save(bake_data, path);
	}
}

void LocalLRTVolume3DEditorPlugin::_data_save_path_and_bake(const String &p_path) {
	probe_file->hide();
	if (!volume) {
		return;
	}
	EditorProgress progress("bake_local_lrt", TTR("Bake LocalLRTVolume3D"), 1);
	progress.step(TTR("Building Local LRT data"), 0, false);
	volume->rebuild();
	Ref<LocalLRTVolumeData> bake_data = volume->get_bake_data();
	ERR_FAIL_COND(bake_data.is_null());
	bake_data->set_path(p_path);
	ResourceSaver::save(bake_data, p_path, ResourceSaver::FLAG_CHANGE_PATH);
}

void LocalLRTVolume3DEditorPlugin::edit(Object *p_object) {
	volume = Object::cast_to<LocalLRTVolume3D>(p_object);
}

bool LocalLRTVolume3DEditorPlugin::handles(Object *p_object) const {
	return p_object->is_class("LocalLRTVolume3D");
}

void LocalLRTVolume3DEditorPlugin::_notification(int p_what) {
	if (p_what != NOTIFICATION_PROCESS || !volume) {
		return;
	}
	const Vector3i resolution = volume->get_resolution();
	const String text = vformat(TTR("Probe resolution: %s"), vformat(U"%d × %d × %d", resolution.x, resolution.y, resolution.z));
	if (bake->get_tooltip(Point2()) != text) {
		bake->set_tooltip_text(text);
	}
}

void LocalLRTVolume3DEditorPlugin::make_visible(bool p_visible) {
	if (p_visible) {
		bake_hb->show();
		set_process(true);
	} else {
		bake_hb->hide();
		set_process(false);
	}
}

LocalLRTVolume3DEditorPlugin::LocalLRTVolume3DEditorPlugin() {
	bake_hb = memnew(HBoxContainer);
	bake_hb->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	bake_hb->hide();
	bake = memnew(Button);
	bake->set_theme_type_variation(SceneStringName(FlatButton));
	bake->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	bake->set_text(TTR("Bake LocalLRT"));
	bake->connect(SceneStringName(pressed), callable_mp(this, &LocalLRTVolume3DEditorPlugin::_bake));
	bake_hb->add_child(bake);
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bake_hb);

	probe_file = memnew(EditorFileDialog);
	probe_file->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	probe_file->add_filter("*.res");
	probe_file->connect("file_selected", callable_mp(this, &LocalLRTVolume3DEditorPlugin::_data_save_path_and_bake));
	EditorInterface::get_singleton()->get_base_control()->add_child(probe_file);
	probe_file->set_title(TTR("Select path for LocalLRTVolume3D Data File"));
}
