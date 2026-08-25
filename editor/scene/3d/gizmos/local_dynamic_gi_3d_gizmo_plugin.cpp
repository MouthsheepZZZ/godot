/**************************************************************************/
/*  local_dynamic_gi_3d_gizmo_plugin.cpp                                  */
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

#include "local_dynamic_gi_3d_gizmo_plugin.h"

#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "scene/3d/local_dynamic_gi_3d.h"

static void add_aabb_lines(Vector<Vector3> &r_lines, const AABB &p_aabb) {
	for (int i = 0; i < 12; i++) {
		Vector3 a;
		Vector3 b;
		p_aabb.get_edge(i, a, b);
		r_lines.push_back(a);
		r_lines.push_back(b);
	}
}

LocalDynamicGI3DGizmoPlugin::LocalDynamicGI3DGizmoPlugin() {
	Color gizmo_color = Color(0.25, 0.72, 1.0);
	create_material("local_dynamic_gi_material", gizmo_color);

	Color disabled_color = Color(0.55, 0.55, 0.55);
	create_material("local_dynamic_gi_disabled_material", disabled_color);

	gizmo_color.a = 0.35;
	create_material("local_dynamic_gi_blend_material", gizmo_color);

	create_icon_material("local_dynamic_gi_icon", EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("GizmoVoxelGI"), EditorStringName(EditorIcons)));
}

bool LocalDynamicGI3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LocalDynamicGI3D>(p_spatial) != nullptr;
}

String LocalDynamicGI3DGizmoPlugin::get_gizmo_name() const {
	return "LocalDynamicGI3D";
}

int LocalDynamicGI3DGizmoPlugin::get_priority() const {
	return -1;
}

void LocalDynamicGI3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	LocalDynamicGI3D *local_gi = Object::cast_to<LocalDynamicGI3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();

	const AABB bounds = local_gi->get_local_bounds();
	if (bounds.has_volume()) {
		const Ref<Material> material = get_material(local_gi->is_enabled() ? "local_dynamic_gi_material" : "local_dynamic_gi_disabled_material", p_gizmo);
		Vector<Vector3> lines;
		add_aabb_lines(lines, bounds);
		p_gizmo->add_lines(lines, material);
		p_gizmo->add_collision_segments(lines);

		if (local_gi->is_enabled() && local_gi->get_blend_distance() > CMP_EPSILON) {
			AABB blend_bounds = bounds;
			blend_bounds.grow_by(local_gi->get_blend_distance());
			Vector<Vector3> blend_lines;
			add_aabb_lines(blend_lines, blend_bounds);
			p_gizmo->add_lines(blend_lines, get_material("local_dynamic_gi_blend_material", p_gizmo));
		}
	}

	p_gizmo->add_unscaled_billboard(get_material("local_dynamic_gi_icon", p_gizmo), 0.05);
}
