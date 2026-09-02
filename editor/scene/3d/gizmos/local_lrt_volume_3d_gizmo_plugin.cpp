/**************************************************************************/
/*  local_lrt_volume_3d_gizmo_plugin.cpp                                  */
/**************************************************************************/

#include "local_lrt_volume_3d_gizmo_plugin.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "scene/3d/local_lrt_volume_3d.h"

LocalLRTVolume3DGizmoPlugin::LocalLRTVolume3DGizmoPlugin() {
	helper.instantiate();

	Color gizmo_color = Color(0.35, 0.75, 1.0);
	create_material("local_lrt_bounds", gizmo_color);

	gizmo_color.a = 0.5;
	create_material("local_lrt_internal", gizmo_color);

	create_icon_material("local_lrt_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoLocalLRTVolume3D"), EditorStringName(EditorIcons)));
	create_handle_material("handles");
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

String LocalLRTVolume3DGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant LocalLRTVolume3DGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
	return volume->get_size();
}

void LocalLRTVolume3DGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), p_gizmo->get_node_3d()->get_global_transform());
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
	volume->begin_gizmo_size_edit();
}

void LocalLRTVolume3DGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());

	Vector3 sg[2];
	helper->get_segment(p_camera, p_point, sg);

	Vector3 size = volume->get_size();
	Vector3 position;
	helper->box_set_handle(sg, p_id, size, position);
	volume->set_size(size);
	volume->set_global_position(position);
}

void LocalLRTVolume3DGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	helper->box_commit_handle(TTR("Change LocalLRTVolume3D Size"), p_cancel, p_gizmo->get_node_3d());
	LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
	volume->end_gizmo_size_edit();
}

void LocalLRTVolume3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();

	if (p_gizmo->is_selected()) {
		LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(p_gizmo->get_node_3d());
		Vector3 size = volume->get_size();

		AABB aabb;
		aabb.position = -size / 2;
		aabb.size = size;

		Vector<Vector3> lines;
		for (int edge = 0; edge < 12; edge++) {
			Vector3 from;
			Vector3 to;
			aabb.get_edge(edge, from, to);
			lines.push_back(from);
			lines.push_back(to);
		}

		AABB blend_aabb;
		for (int i = 0; i < 3; i++) {
			blend_aabb.position[i] = aabb.position[i] + volume->get_edge_blend_distance();
			blend_aabb.size[i] = aabb.size[i] - volume->get_edge_blend_distance() * 2.0;
			if (blend_aabb.size[i] < blend_aabb.position[i]) {
				blend_aabb.position[i] = aabb.position[i] + aabb.size[i] / 2.0;
				blend_aabb.size[i] = 0.0;
			}
		}

		Vector<Vector3> internal_lines;
		if (volume->get_edge_blend_distance() != 0.0) {
			for (int i = 0; i < 12; i++) {
				Vector3 a;
				Vector3 b;
				blend_aabb.get_edge(i, a, b);
				lines.push_back(a);
				lines.push_back(b);
			}

			for (int i = 0; i < 8; i++) {
				internal_lines.push_back(blend_aabb.get_endpoint(i));
				internal_lines.push_back(aabb.get_endpoint(i));
			}
		}

		p_gizmo->add_lines(lines, get_material("local_lrt_bounds", p_gizmo));
		p_gizmo->add_lines(internal_lines, get_material("local_lrt_internal", p_gizmo));
		p_gizmo->add_handles(helper->box_get_handles(size), get_material("handles"));
	}

	p_gizmo->add_unscaled_billboard(get_material("local_lrt_icon", p_gizmo), 0.05);
}
