/**************************************************************************/
/*  local_gi_volume_3d_gizmo_plugin.cpp                                   */
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

#include "local_gi_volume_3d_gizmo_plugin.h"

#include "scene/3d/local_gi/local_gi_volume_3d.h"

LocalGIVolume3DGizmoPlugin::LocalGIVolume3DGizmoPlugin() {
	create_material("local_gi_volume", Color(0.2, 0.7, 1.0, 0.9));
	create_material("local_gi_hit", Color(0.2, 0.95, 0.35, 0.95));
	create_material("local_gi_miss", Color(0.35, 0.4, 0.5, 0.35));
	create_material("local_gi_normal", Color(0.25, 0.55, 1.0, 0.95));
	create_material("local_gi_distance", Color(0.95, 0.55, 0.15, 0.95));
	create_material("local_gi_probe", Color(0.25, 0.75, 1.0, 0.95));
	create_material("local_gi_probe_selected", Color(1.0, 0.85, 0.2, 0.95));
}

bool LocalGIVolume3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LocalGIVolume3D>(p_spatial) != nullptr;
}

String LocalGIVolume3DGizmoPlugin::get_gizmo_name() const {
	return "LocalGIVolume3D";
}

int LocalGIVolume3DGizmoPlugin::get_priority() const {
	return -1;
}

void LocalGIVolume3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();

	LocalGIVolume3D *volume = Object::cast_to<LocalGIVolume3D>(p_gizmo->get_node_3d());
	if (volume == nullptr) {
		return;
	}

	const AABB aabb = volume->get_aabb();
	Vector<Vector3> box_lines;
	for (int i = 0; i < 12; i++) {
		Vector3 a;
		Vector3 b;
		aabb.get_edge(i, a, b);
		box_lines.push_back(a);
		box_lines.push_back(b);
	}
	p_gizmo->add_lines(box_lines, get_material("local_gi_volume", p_gizmo));

	const LocalGIVolume3D::DebugMode mode = volume->get_debug_mode();
	const bool show_probes = mode == LocalGIVolume3D::DEBUG_PROBE_POSITIONS ||
			mode == LocalGIVolume3D::DEBUG_SELECTED_PROBE_RAYS ||
			mode == LocalGIVolume3D::DEBUG_RAW_PROBE_RADIANCE ||
			mode == LocalGIVolume3D::DEBUG_PROBE_IRRADIANCE;
	if (show_probes) {
		if (volume->get_probe_count() == 0) {
			volume->build_probes();
		}

		const PackedVector3Array positions = volume->get_probe_positions();
		const int selected = volume->get_debug_selected_probe() < 0 || volume->get_debug_selected_probe() >= positions.size()
				? volume->get_probe_grid().get_center_probe_index()
				: volume->get_debug_selected_probe();
		const float radius = MIN(0.16f, volume->get_probe_spacing() * 0.28f);
		const int segments = 16;
		Vector<Vector3> probe_lines;
		Vector<Vector3> selected_lines;
		for (int i = 0; i < positions.size(); i++) {
			const Vector3 p = positions[i];
			const float r = i == selected ? radius * 1.35f : radius;
			Vector<Vector3> &target = i == selected ? selected_lines : probe_lines;
			for (int s = 0; s < segments; s++) {
				const float a0 = (float)Math::TAU * ((float)s / (float)segments);
				const float a1 = (float)Math::TAU * ((float)(s + 1) / (float)segments);
				const Vector3 xy0(Math::cos(a0) * r, Math::sin(a0) * r, 0);
				const Vector3 xy1(Math::cos(a1) * r, Math::sin(a1) * r, 0);
				const Vector3 xz0(Math::cos(a0) * r, 0, Math::sin(a0) * r);
				const Vector3 xz1(Math::cos(a1) * r, 0, Math::sin(a1) * r);
				const Vector3 yz0(0, Math::cos(a0) * r, Math::sin(a0) * r);
				const Vector3 yz1(0, Math::cos(a1) * r, Math::sin(a1) * r);
				target.push_back(p + xy0);
				target.push_back(p + xy1);
				target.push_back(p + xz0);
				target.push_back(p + xz1);
				target.push_back(p + yz0);
				target.push_back(p + yz1);
			}
		}
		if (!probe_lines.is_empty()) {
			p_gizmo->add_lines(probe_lines, get_material("local_gi_probe", p_gizmo));
		}
		if (!selected_lines.is_empty()) {
			p_gizmo->add_lines(selected_lines, get_material("local_gi_probe_selected", p_gizmo));
		}

		if ((mode == LocalGIVolume3D::DEBUG_PROBE_IRRADIANCE || mode == LocalGIVolume3D::DEBUG_RAW_PROBE_RADIANCE) && !volume->has_one_bounce()) {
			if (volume->get_baked_triangle_count() == 0) {
				volume->bake();
				volume->update_dynamic();
			}
			volume->compute_one_bounce();
		}

		if (mode == LocalGIVolume3D::DEBUG_SELECTED_PROBE_RAYS || mode == LocalGIVolume3D::DEBUG_RAW_PROBE_RADIANCE) {
			if (volume->get_baked_triangle_count() == 0) {
				volume->bake();
				volume->update_dynamic();
			}
			Vector<Vector3> origins;
			Vector<Vector3> directions;
			volume->collect_selected_probe_rays(origins, directions);
			Vector<Vector3> hit_lines;
			Vector<Vector3> miss_lines;
			Vector<Vector3> extra_lines;
			for (int i = 0; i < origins.size(); i++) {
				LocalGIRayHit hit;
				if (!volume->intersect_gpu_ray(origins[i], directions[i], hit)) {
					volume->intersect_ray(origins[i], directions[i], hit);
				}
				const Vector3 dir = directions[i].normalized();
				const Vector3 end = hit.hit ? hit.position : (origins[i] + dir * aabb.size.length());
				if (hit.hit) {
					hit_lines.push_back(origins[i]);
					hit_lines.push_back(end);
					extra_lines.push_back(hit.position);
					extra_lines.push_back(hit.position + hit.normal * 0.12);
				} else {
					miss_lines.push_back(origins[i]);
					miss_lines.push_back(end);
				}
			}
			if (!hit_lines.is_empty()) {
				p_gizmo->add_lines(hit_lines, get_material("local_gi_hit", p_gizmo));
			}
			if (!miss_lines.is_empty()) {
				p_gizmo->add_lines(miss_lines, get_material("local_gi_miss", p_gizmo));
			}
			if (!extra_lines.is_empty()) {
				p_gizmo->add_lines(extra_lines, get_material("local_gi_normal", p_gizmo));
			}
		}
		return;
	}

	const bool show_rays = mode == LocalGIVolume3D::DEBUG_STATIC_BVH_HIT ||
			mode == LocalGIVolume3D::DEBUG_DYNAMIC_BVH_HIT ||
			mode == LocalGIVolume3D::DEBUG_RAY_HIT_MISS ||
			mode == LocalGIVolume3D::DEBUG_HIT_NORMAL ||
			mode == LocalGIVolume3D::DEBUG_HIT_DISTANCE;
	if (!show_rays) {
		return;
	}

	if (volume->get_baked_triangle_count() == 0) {
		volume->bake();
		volume->update_dynamic();
	}

	Vector<Vector3> origins;
	Vector<Vector3> directions;
	volume->collect_debug_rays(origins, directions);

	Vector<LocalGIRayHit> hits;
	hits.resize(origins.size());
	for (int i = 0; i < origins.size(); i++) {
		if (mode == LocalGIVolume3D::DEBUG_STATIC_BVH_HIT) {
			volume->intersect_static_ray(origins[i], directions[i], hits.write[i]);
		} else if (mode == LocalGIVolume3D::DEBUG_DYNAMIC_BVH_HIT) {
			volume->intersect_dynamic_ray(origins[i], directions[i], hits.write[i]);
		} else if (!volume->intersect_gpu_ray(origins[i], directions[i], hits.write[i])) {
			volume->intersect_ray(origins[i], directions[i], hits.write[i]);
		}
	}

	Vector<Vector3> hit_lines;
	Vector<Vector3> miss_lines;
	Vector<Vector3> extra_lines;
	for (int i = 0; i < origins.size(); i++) {
		const Vector3 dir = directions[i].normalized();
		const LocalGIRayHit &hit = hits[i];
		const Vector3 end = hit.hit ? hit.position : (origins[i] + dir * aabb.size.length());
		if (hit.hit) {
			hit_lines.push_back(origins[i]);
			hit_lines.push_back(end);
			if (mode == LocalGIVolume3D::DEBUG_HIT_NORMAL || mode == LocalGIVolume3D::DEBUG_RAY_HIT_MISS) {
				extra_lines.push_back(hit.position);
				extra_lines.push_back(hit.position + hit.normal * 0.12);
			}
		} else {
			miss_lines.push_back(origins[i]);
			miss_lines.push_back(end);
		}
	}

	if (!hit_lines.is_empty()) {
		const StringName material_name = mode == LocalGIVolume3D::DEBUG_HIT_DISTANCE ? "local_gi_distance" : "local_gi_hit";
		p_gizmo->add_lines(hit_lines, get_material(material_name, p_gizmo));
	}
	if (!miss_lines.is_empty()) {
		p_gizmo->add_lines(miss_lines, get_material("local_gi_miss", p_gizmo));
	}
	if (!extra_lines.is_empty()) {
		p_gizmo->add_lines(extra_lines, get_material("local_gi_normal", p_gizmo));
	}
}
