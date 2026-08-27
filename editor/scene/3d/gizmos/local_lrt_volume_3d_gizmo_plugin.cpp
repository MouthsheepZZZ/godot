/**************************************************************************/
/*  local_lrt_volume_3d_gizmo_plugin.cpp                                  */
/**************************************************************************/

#include "local_lrt_volume_3d_gizmo_plugin.h"

#include "scene/3d/local_lrt_volume_3d.h"

LocalLRTVolume3DGizmoPlugin::LocalLRTVolume3DGizmoPlugin() {
	create_material("local_lrt_bounds", Color(0.35, 0.75, 1.0));
}

bool LocalLRTVolume3DGizmoPlugin::has_gizmo(Node3D *p_node_3d) {
	return Object::cast_to<LocalLRTVolume3D>(p_node_3d) != nullptr;
}

String LocalLRTVolume3DGizmoPlugin::get_gizmo_name() const {
	return "LocalLRTVolume3D";
}

int LocalLRTVolume3DGizmoPlugin::get_priority() const {
	return -1;
}

void LocalLRTVolume3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();
	if (!p_gizmo->is_selected()) {
		return;
	}

	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
	const AABB bounds = volume->get_bounds();
	Vector<Vector3> lines;
	for (int edge = 0; edge < 12; edge++) {
		Vector3 from;
		Vector3 to;
		bounds.get_edge(edge, from, to);
		lines.push_back(from);
		lines.push_back(to);
	}
	p_gizmo->add_lines(lines, get_material("local_lrt_bounds", p_gizmo));
}
