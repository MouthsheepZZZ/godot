/**************************************************************************/
/*  local_lrt_volume_3d_gizmo_plugin.h                                    */
/**************************************************************************/

#pragma once

#include "editor/scene/3d/node_3d_editor_gizmos.h"

class LocalLRTVolume3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(LocalLRTVolume3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_node_3d) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	LocalLRTVolume3DGizmoPlugin();
};
