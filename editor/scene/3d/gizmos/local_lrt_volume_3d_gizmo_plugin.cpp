/**************************************************************************/
/*  local_lrt_volume_3d_gizmo_plugin.cpp                                  */
/**************************************************************************/

#include "local_lrt_volume_3d_gizmo_plugin.h"

#include "scene/3d/local_lrt_volume_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"

static Ref<StandardMaterial3D> create_probe_material(const Color &p_color) {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
	material->set_albedo(p_color);
	return material;
}

LocalLRTVolume3DGizmoPlugin::LocalLRTVolume3DGizmoPlugin() {
	create_material("local_lrt_bounds", Color(0.35, 0.75, 1.0));
	create_material("local_lrt_probes", Color(1.0, 0.75, 0.2, 0.65));
	create_material("local_lrt_open", Color(0.2, 0.55, 1.0, 0.2));
	create_material("local_lrt_occupied", Color(1.0, 0.2, 0.8, 0.9));
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
	const bool has_built_data = volume->has_built_data();
	const LocalLRTVolume3D::DebugMode debug_mode = volume->get_debug_mode();
	const float fully_visible_constant = LocalLRTMath::encode_constant(1.0).x;
	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int x = 0; x < resolution.x; x++) {
				const Vector3i position(x, y, z);
				Ref<Material> probe_material = get_material("local_lrt_probes", p_gizmo);
				if (has_built_data) {
					const bool occupied = volume->is_probe_occupied(position);
					if (occupied) {
						probe_material = get_material("local_lrt_occupied", p_gizmo);
					} else if (debug_mode == LocalLRTVolume3D::DEBUG_MODE_OCCUPANCY) {
						probe_material = get_material("local_lrt_open", p_gizmo);
					} else if (debug_mode == LocalLRTVolume3D::DEBUG_MODE_LOCAL_VISIBILITY) {
						const float visibility = CLAMP(volume->get_probe_local_visibility(position).x / fully_visible_constant, 0.0, 1.0);
						probe_material = create_probe_material(Color(visibility, visibility, visibility, 0.9));
					} else if (debug_mode == LocalLRTVolume3D::DEBUG_MODE_GLOBAL_VISIBILITY) {
						if (volume->has_gpu_data()) {
							const float visibility = CLAMP(volume->get_probe_global_visibility(position).x / fully_visible_constant, 0.0, 1.0);
							probe_material = create_probe_material(Color(visibility, visibility, visibility, 0.9));
						} else {
							probe_material = get_material("local_lrt_open", p_gizmo);
						}
					} else if (debug_mode == LocalLRTVolume3D::DEBUG_MODE_INJECTION) {
						Color injection = volume->get_probe_injection_color(position);
						if (MAX(injection.r, MAX(injection.g, injection.b)) <= 0.0001) {
							probe_material = get_material("local_lrt_open", p_gizmo);
						} else {
							injection.r = CLAMP(injection.r, 0.0, 1.0);
							injection.g = CLAMP(injection.g, 0.0, 1.0);
							injection.b = CLAMP(injection.b, 0.0, 1.0);
							injection.a = 0.9;
							probe_material = create_probe_material(injection);
						}
					} else {
						Color transfer = volume->get_probe_transfer_color(position);
						if (MAX(transfer.r, MAX(transfer.g, transfer.b)) <= 0.0001) {
							probe_material = get_material("local_lrt_open", p_gizmo);
						} else {
							transfer.r = CLAMP(transfer.r, 0.0, 1.0);
							transfer.g = CLAMP(transfer.g, 0.0, 1.0);
							transfer.b = CLAMP(transfer.b, 0.0, 1.0);
							transfer.a = 0.9;
							probe_material = create_probe_material(transfer);
						}
					}
				}
				Transform3D probe_transform = probe_scale_transform;
				probe_transform.origin = volume->get_probe_position(position);
				p_gizmo->add_mesh(sphere, probe_material, probe_transform);
			}
		}
	}
}
