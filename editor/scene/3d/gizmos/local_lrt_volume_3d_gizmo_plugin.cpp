/**************************************************************************/
/*  local_lrt_volume_3d_gizmo_plugin.cpp                                  */
/**************************************************************************/

#include "local_lrt_volume_3d_gizmo_plugin.h"

#include "scene/3d/local_lrt_volume_3d.h"
#include "scene/resources/3d/primitive_meshes.h"

LocalLRTVolume3DGizmoPlugin::LocalLRTVolume3DGizmoPlugin() {
	create_material("local_lrt_bounds", Color(0.35, 0.75, 1.0));
	create_material("local_lrt_probes", Color(1.0, 0.75, 0.2, 0.65));
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

	if (!volume->is_debug_draw_enabled()) {
		return;
	}

	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(1.0);
	sphere->set_height(2.0);
	sphere->set_radial_segments(8);
	sphere->set_rings(4);

	const Vector3i resolution = volume->get_resolution();
	const float probe_scale = volume->get_debug_probe_scale();
	const Transform3D probe_scale_transform(Basis().scaled(Vector3(probe_scale, probe_scale, probe_scale)));
	const Ref<Material> probe_material = get_material("local_lrt_probes", p_gizmo);
	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int x = 0; x < resolution.x; x++) {
				Transform3D probe_transform = probe_scale_transform;
				probe_transform.origin = volume->get_probe_position(Vector3i(x, y, z));
				p_gizmo->add_mesh(sphere, probe_material, probe_transform);
			}
		}
	}
}
