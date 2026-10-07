/**************************************************************************/
/*  lrt_export_plugin.cpp                                                 */
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

#include "lrt_export_plugin.h"

#include "lrt_cache.h"
#include "lrt_volume_3d.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "core/object/class_db.h"
#include "editor/editor_node.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/packed_scene.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

void LRTExportPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("prepare_resource", "resource"), &LRTExportPlugin::prepare_resource);
}

void LRTExportPlugin::_reset() {
	signatures.clear();
	visited_resources.clear();
	packed_paths.clear();
	paths.clear();
	preparation_error = String();
	prepare_volumes = true;
}

Dictionary LRTExportPlugin::_report() const {
	Dictionary report;
	report["ok"] = preparation_error.is_empty();
	report["assets"] = paths;
	report["count"] = paths.size();
	if (!preparation_error.is_empty()) {
		report["error"] = preparation_error;
	}
	return report;
}

void LRTExportPlugin::_prepare_mesh(const Ref<Mesh> &p_mesh, int p_resolution) {
	if (p_mesh.is_null() || !preparation_error.is_empty()) {
		return;
	}
	std::vector<lrt::MeshTriangle> triangles;
	// Non-triangle resources never contribute an SDF at runtime either.
	if (!lrt::mesh_triangles(p_mesh, triangles)) {
		return;
	}
	const uint64_t signature = lrt::asset_signature(triangles, p_resolution);
	if (signatures.has(signature)) {
		return;
	}
	const Dictionary report = LRTVolume3D::prepare_mesh_sdf(p_mesh, p_resolution);
	if (!bool(report.get("ok", false))) {
		preparation_error = p_mesh->get_path() + ": " + String(report.get("error", String()));
		return;
	}
	const String cache_path = report["path"];
	signatures.insert(signature);
	_pack_dependency(cache_path);
}

void LRTExportPlugin::_pack_dependency(const String &p_path) {
	const String target = "res://.godot/lrt_export/" + p_path.get_file();
	if (packed_paths.has(target)) {
		return;
	}
	if (packing) {
		Error error;
		const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(p_path, &error);
		if (error != OK) {
			preparation_error = "Cannot read prepared LRT dependency: " + p_path;
			return;
		}
		add_file(target, bytes, false);
	}
	packed_paths.insert(target);
	paths.push_back(p_path);
}

void LRTExportPlugin::_prepare_scene_data(Node *p_root) {
	Vector<Node *> nodes;
	nodes.push_back(p_root);
	Vector<LRTVolume3D *> volumes;
	for (int i = 0; i < nodes.size(); i++) {
		Node *node = nodes[i];
		LRTVolume3D *volume = Object::cast_to<LRTVolume3D>(node);
		if (volume != nullptr) {
			volumes.push_back(volume);
		}
		for (int child = 0; child < node->get_child_count(); child++) {
			nodes.push_back(node->get_child(child));
		}
	}
	if (volumes.is_empty()) {
		return;
	}
	// The export copy retains authored resources and transforms, but executes no tool/game code.
	for (Node *node : nodes) {
		node->set_script(Variant());
	}
	p_root->set_process_mode(Node::PROCESS_MODE_DISABLED);
	SubViewport *viewport = memnew(SubViewport);
	viewport->set_use_own_world_3d(true);
	EditorNode::get_singleton()->get_tree()->get_root()->add_child(viewport);
	viewport->add_child(p_root);
	RenderingServer::get_singleton()->sync();
	for (LRTVolume3D *volume : volumes) {
		const Dictionary report = volume->prepare_export_data();
		if (!bool(report.get("ok", false))) {
			preparation_error = String(volume->get_path()) + ": " + String(report.get("error", String()));
			break;
		}
		const PackedStringArray dependencies = report["assets"];
		for (const String &path : dependencies) {
			_pack_dependency(path);
		}
	}
	viewport->remove_child(p_root);
	memdelete(viewport);
}

void LRTExportPlugin::_visit_variant(const Variant &p_value) {
	if (p_value.get_type() == Variant::OBJECT) {
		const Ref<Resource> resource = p_value;
		_visit_resource(resource);
	} else if (p_value.get_type() == Variant::ARRAY) {
		const Array array = p_value;
		for (int i = 0; i < array.size(); i++) {
			_visit_variant(array[i]);
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary dictionary = p_value;
		const Array keys = dictionary.keys();
		for (int i = 0; i < keys.size(); i++) {
			_visit_variant(keys[i]);
			_visit_variant(dictionary[keys[i]]);
		}
	}
}

void LRTExportPlugin::_visit_node(Node *p_node) {
	if (!preparation_error.is_empty()) {
		return;
	}
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_node);
	if (instance != nullptr) {
		int resolution = int(instance->get_meta("lrt_sdf_resolution", 0));
		resolution = resolution > 0 ? MAX(8, resolution) : CLAMP(int(GLOBAL_GET("rendering/global_illumination/lrt/sdf/default_resolution")), 8, 256);
		_prepare_mesh(instance->get_mesh(), resolution);
	}
	List<PropertyInfo> properties;
	p_node->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if ((property.usage & PROPERTY_USAGE_STORAGE) && property.name != "script" && !(instance != nullptr && property.name == "mesh")) {
			_visit_variant(p_node->get(property.name));
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_visit_node(p_node->get_child(i));
	}
}

void LRTExportPlugin::_visit_resource(const Ref<Resource> &p_resource) {
	if (p_resource.is_null() || !preparation_error.is_empty() || visited_resources.has(p_resource->get_instance_id())) {
		return;
	}
	visited_resources.insert(p_resource->get_instance_id());
	const Ref<Mesh> mesh = p_resource;
	if (mesh.is_valid()) {
		_prepare_mesh(mesh, CLAMP(int(GLOBAL_GET("rendering/global_illumination/lrt/sdf/default_resolution")), 8, 256));
		return;
	}
	const Ref<PackedScene> scene = p_resource;
	if (scene.is_valid()) {
		Node *root = scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
		if (root == nullptr) {
			preparation_error = "Cannot instantiate LRT export dependency: " + scene->get_path();
			return;
		}
		// Detached inspection avoids running _ready(), renderer registration, and scene mutation.
		_visit_node(root);
		if (prepare_volumes) {
			_prepare_scene_data(root);
		}
		memdelete(root);
		return;
	}
	if (Object::cast_to<Script>(p_resource.ptr()) != nullptr) {
		return;
	}
	List<PropertyInfo> properties;
	p_resource->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if (property.usage & PROPERTY_USAGE_STORAGE) {
			_visit_variant(p_resource->get(property.name));
		}
	}
}

Dictionary LRTExportPlugin::prepare_resource(const Ref<Resource> &p_resource) {
	_reset();
	packing = false;
	if (p_resource.is_null()) {
		preparation_error = "A resource is required for LRT preparation.";
		return _report();
	}
	_visit_resource(p_resource);
	return _report();
}

Dictionary LRTExportPlugin::prepare_imported_resource(const Ref<Resource> &p_resource) {
	_reset();
	packing = false;
	prepare_volumes = false;
	if (p_resource.is_null()) {
		preparation_error = "Cannot load the imported resource.";
		return _report();
	}
	_visit_resource(p_resource);
	return _report();
}

void LRTExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	_reset();
	packing = true;
}

void LRTExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	if (!preparation_error.is_empty()) {
		return;
	}
	if (p_type == "PackedScene" || ClassDB::is_parent_class(p_type, "Resource")) {
		_visit_resource(ResourceLoader::load(p_path));
	}
	if (!preparation_error.is_empty()) {
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "LRT", preparation_error);
	}
}

void LRTExportPlugin::_export_end() {
	packing = false;
	_reset();
}

#endif
