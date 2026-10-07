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

class EditorFileSystemDirectory;
class Mesh;

// Prepares only resources selected by the export dependency walk, never the cache directory.
class LRTExportPlugin : public EditorExportPlugin {
	GDCLASS(LRTExportPlugin, EditorExportPlugin);

	HashSet<uint64_t> signatures;
	HashSet<ObjectID> visited_resources;
	HashSet<String> packed_paths;
	PackedStringArray paths;
	String preparation_error;
	bool packing = false;

	void _prepare_mesh(const Ref<Mesh> &p_mesh, int p_resolution);
	void _visit_variant(const Variant &p_value);
	void _visit_resource(const Ref<Resource> &p_resource);
	void _visit_node(Node *p_node);
	void _prepare_scene_data(Node *p_root);
	void _pack_dependency(const String &p_path);
	void _visit_directory(EditorFileSystemDirectory *p_directory);
	void _reset();
	Dictionary _report() const;

protected:
	static void _bind_methods();
	void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	void _export_end() override;

public:
	String get_name() const override { return "LRT"; }
	Dictionary prepare_resource(const Ref<Resource> &p_resource);
	Dictionary prepare_project();
};

#endif
