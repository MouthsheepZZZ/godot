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
/* The above copyright notice and this permission notice shall be        */
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

#ifdef TOOLS_ENABLED

#include "lrt_editor_plugin.h"

#include "lrt_volume_3d.h"

#include "editor/scene/3d/gizmos/gizmo_3d_helper.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"

// Upper bound of lattice lines drawn per axis. Beyond it the lattice is sampled instead of
// dropped, so a small `spacing` still shows the probe grid (every Nth probe plane).
constexpr int LRT_GIZMO_MAX_DIVISIONS = 32;
// Share of the shortest side used for the centre cross, which keeps the node clickable even
// when the volume is small.
constexpr real_t LRT_GIZMO_CENTER_CROSS_RATIO = 0.1;

LRTVolumeGizmoPlugin::LRTVolumeGizmoPlugin() {
	helper.instantiate();

	// Same default colour as ReflectionProbe, so the two volumes look alike in the viewport.
	const Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/reflection_probe");
	create_material("volume_material", gizmo_color);

	Color disabled_color = gizmo_color;
	disabled_color.a = 0.25;
	create_material("volume_disabled_material", disabled_color);

	Color internal_color = gizmo_color;
	internal_color.a = 0.04;
	create_material("volume_internal_material", internal_color);

	create_handle_material("handles");
}

bool LRTVolumeGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<LRTVolume3D>(p_spatial) != nullptr;
}

String LRTVolumeGizmoPlugin::get_gizmo_name() const {
	return "LRTVolume3D";
}

int LRTVolumeGizmoPlugin::get_priority() const {
	return -1;
}

String LRTVolumeGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return helper->box_get_handle_name(p_id);
}

Variant LRTVolumeGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	return volume->get_volume_size();
}

void LRTVolumeGizmoPlugin::begin_handle_action(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	helper->initialize_handle_action(get_handle_value(p_gizmo, p_id, p_secondary), volume->get_global_transform());
	// A drag is a live resize: no bake per mouse move, exactly one when the drag ends.
	volume->set_rebuild_suppressed(true);
}

void LRTVolumeGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());

	Vector3 segment[2];
	helper->get_segment(p_camera, p_point, segment);

	Vector3 size = volume->get_volume_size();
	Vector3 position;
	helper->box_set_handle(segment, p_id, size, position);
	volume->set_volume_size(size);
	volume->set_global_position(position);
}

void LRTVolumeGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	helper->box_commit_handle(TTR("Change LRT Volume Size"), p_cancel, volume, volume, SNAME("global_position"), SNAME("volume_size"));
	// Releasing the handle releases the bake: the deferred rebuild runs once for the final box.
	volume->set_rebuild_suppressed(false);
}

void LRTVolumeGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	p_gizmo->clear();

	LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(p_gizmo->get_node_3d());
	if (volume == nullptr || !p_gizmo->is_selected()) {
		return;
	}
	const Vector3 size = volume->get_volume_size();
	const AABB aabb(-size / 2.0, size);

	Vector<Vector3> lines;
	for (int i = 0; i < 12; i++) {
		Vector3 a;
		Vector3 b;
		aabb.get_edge(i, a, b);
		lines.push_back(a);
		lines.push_back(b);
	}

	// Centre cross: the volume is the probe region around the node, and this keeps the node
	// clickable in the viewport even when the box is small.
	const real_t cross = MIN(size.x, MIN(size.y, size.z)) * LRT_GIZMO_CENTER_CROSS_RATIO;
	for (int axis = 0; axis < 3; axis++) {
		Vector3 from;
		Vector3 to;
		from[axis] = -cross;
		to[axis] = cross;
		lines.push_back(from);
		lines.push_back(to);
	}

	// The probe lattice the solver actually bakes, so `spacing` is visible in the editor. A
	// small `spacing` would flood the viewport, so past the cap only every Nth plane is drawn:
	// those lines still sit on real probe planes, the density simply stops growing.
	Vector<Vector3> internal_lines;
	const real_t spacing = MAX(real_t(volume->get_spacing()), real_t(0.001));
	int divisions[3] = { 0, 0, 0 };
	for (int axis = 0; axis < 3; axis++) {
		divisions[axis] = MAX(1, int(Math::round(size[axis] / spacing)));
	}
	for (int axis = 0; axis < 3; axis++) {
		const int next_1 = (axis + 1) % 3;
		const int next_2 = (axis + 2) % 3;
		const int stride = MAX(1, (divisions[axis] + LRT_GIZMO_MAX_DIVISIONS - 1) / LRT_GIZMO_MAX_DIVISIONS);
		for (int i = stride; i < divisions[axis]; i += stride) {
			const real_t offset = aabb.position[axis] + i * spacing;
			for (int corner = 0; corner < 4; corner++) {
				Vector3 from = aabb.position;
				Vector3 to = aabb.position;
				from[axis] = offset;
				to[axis] = offset;
				to[corner & 1 ? next_1 : next_2] += size[corner & 1 ? next_1 : next_2];
				if (corner & 2) {
					from[next_1] += size[next_1];
					from[next_2] += size[next_2];
				}
				internal_lines.push_back(from);
				internal_lines.push_back(to);
			}
		}
	}

	p_gizmo->add_lines(lines, get_material(volume->is_enabled() ? "volume_material" : "volume_disabled_material", p_gizmo));
	p_gizmo->add_lines(internal_lines, get_material("volume_internal_material", p_gizmo));
	p_gizmo->add_handles(helper->box_get_handles(size), get_material("handles"));
}

LRTEditorPlugin::LRTEditorPlugin() {
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
}

#endif // TOOLS_ENABLED
