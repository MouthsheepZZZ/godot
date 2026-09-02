/**************************************************************************/
/*  local_lrt_volume_3d_editor_plugin.h                                   */
/**************************************************************************/

#pragma once

#include "editor/plugins/editor_plugin.h"

class Button;
class EditorFileDialog;
class HBoxContainer;
class LocalLRTVolume3D;

class LocalLRTVolume3DEditorPlugin : public EditorPlugin {
	GDCLASS(LocalLRTVolume3DEditorPlugin, EditorPlugin);

	LocalLRTVolume3D *volume = nullptr;
	HBoxContainer *bake_hb = nullptr;
	Button *bake = nullptr;
	EditorFileDialog *probe_file = nullptr;

	void _bake();
	void _bake_and_save();
	void _data_save_path_and_bake(const String &p_path);

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "LocalLRTVolume3D"; }
	virtual void edit(Object *p_object) override;
	virtual bool handles(Object *p_object) const override;
	virtual void make_visible(bool p_visible) override;

	LocalLRTVolume3DEditorPlugin();
};
