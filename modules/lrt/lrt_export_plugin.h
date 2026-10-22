/**************************************************************************/
/*  lrt_export_plugin.h                                                   */
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

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/export/editor_export_plugin.h"

class PackedScene;
class SceneState;

// Prepares imported resources and export dependencies, never the cache directory.
class LRTExportPlugin : public EditorExportPlugin {
	GDCLASS(LRTExportPlugin, EditorExportPlugin);

	HashSet<ObjectID> visited_resources;
	HashSet<String> packed_paths;
	PackedStringArray paths;
	String preparation_error;
	bool packing = false;

	void _visit_variant(const Variant &p_value);
	void _visit_resource(const Ref<Resource> &p_resource);
	void _visit_node(Node *p_node);
	void _prepare_scene_data(Node *p_root);
	void _pack_precomputed_assets(Node *p_root);
	void _collect_scene_volumes(const Ref<SceneState> &p_state, const String &p_prefix, HashMap<String, Dictionary> &r_volumes);
	void _pack_scene_data(const Ref<PackedScene> &p_scene);
	void _pack_dependency(const String &p_path);
	void _reset();
	Dictionary _report() const;

protected:
	static void _bind_methods();
	void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	void _export_end() override;

public:
	String get_name() const override { return "LRT"; }
	// Prepare static voxel and field dependencies offline in an isolated world.
	Dictionary prepare_resource(const Ref<Resource> &p_resource);
	// Inspect dependencies already recorded by serialized field caches, without preparing inputs.
	Dictionary collect_export_dependencies(const Ref<PackedScene> &p_scene);
};

#endif
