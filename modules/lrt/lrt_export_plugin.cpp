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
#include "scene/resources/mesh.h"
#include "scene/resources/packed_scene.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

void LRTExportPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("prepare_resource", "resource"), &LRTExportPlugin::prepare_resource);
	ClassDB::bind_method(D_METHOD("collect_export_dependencies", "scene"), &LRTExportPlugin::collect_export_dependencies);
}

void LRTExportPlugin::_reset() {
	visited_resources.clear();
	packed_paths.clear();
	paths.clear();
	preparation_error = String();
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

void LRTExportPlugin::_pack_precomputed_assets(Node *p_root) {
	Vector<Node *> nodes;
	nodes.push_back(p_root);
	for (int i = 0; i < nodes.size(); i++) {
		Node *node = nodes[i];
		GeometryInstance3D *geometry = Object::cast_to<GeometryInstance3D>(node);
		if (geometry != nullptr && geometry->get_gi_mode() != GeometryInstance3D::GI_MODE_DISABLED &&
				lrt::geometry_representation(geometry) == lrt::GEOMETRY_PRECOMPUTED_TRANSFER) {
			const Dictionary report = LRTVolume3D::get_transfer_instance_status(geometry);
			if (!bool(report.get("ok", false))) {
				preparation_error = String(p_root->get_path_to(node)) + ": " + String(report.get("error", "Invalid precomputed LRT dependency."));
				return;
			}
			const PackedStringArray assets = report["assets"];
			for (const String &asset : assets) {
				_pack_dependency(asset);
				if (!preparation_error.is_empty()) {
					return;
				}
			}
		}
		for (int child = 0; child < node->get_child_count(); child++) {
			nodes.push_back(node->get_child(child));
		}
	}
}

void LRTExportPlugin::_prepare_scene_data(Node *p_root) {
	_pack_precomputed_assets(p_root);
	if (!preparation_error.is_empty()) {
		return;
	}
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
	List<PropertyInfo> properties;
	p_node->get_property_list(&properties);
	for (const PropertyInfo &property : properties) {
		if ((property.usage & PROPERTY_USAGE_STORAGE) && property.name != "script") {
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
		_prepare_scene_data(root);
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
	if (Object::cast_to<Mesh>(p_resource.ptr())) {
		preparation_error = "Prepare a PackedScene with LRTVolume3D to capture mesh, material and instance inputs together.";
		return _report();
	}
	_visit_resource(p_resource);
	return _report();
}

void LRTExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	_reset();
	packing = true;
}

void LRTExportPlugin::_collect_scene_volumes(const Ref<SceneState> &p_state, const String &p_prefix, HashMap<String, Dictionary> &r_volumes) {
	if (p_state.is_null()) {
		return;
	}
	_collect_scene_volumes(p_state->get_base_scene_state(), p_prefix, r_volumes);
	for (int node = 0; node < p_state->get_node_count(); node++) {
		const String relative_path = String(p_state->get_node_path(node));
		const String path = (relative_path == "." ? p_prefix : p_prefix.path_join(relative_path)).simplify_path();
		const Ref<PackedScene> instance = p_state->get_node_instance(node);
		if (instance.is_valid()) {
			_collect_scene_volumes(instance->get_state(), path, r_volumes);
		}
		if (p_state->get_node_type(node) == SNAME("LRTVolume3D")) {
			r_volumes[path]["is_volume"] = true;
		}
		for (int property = 0; property < p_state->get_node_property_count(node); property++) {
			const StringName name = p_state->get_node_property_name(node, property);
			if (name == SNAME("build_cache_fingerprint") || name == SNAME("build_cache_export_data") || name == SNAME("metadata/_lrt_representation")) {
				r_volumes[path][name] = p_state->get_node_property_value(node, property);
			}
		}
	}
}

void LRTExportPlugin::_pack_scene_data(const Ref<PackedScene> &p_scene) {
	HashMap<String, Dictionary> volumes;
	_collect_scene_volumes(p_scene->get_state(), ".", volumes);
	bool has_precomputed = false;
	for (const KeyValue<String, Dictionary> &entry : volumes) {
		has_precomputed |= int(entry.value.get("metadata/_lrt_representation", 0)) == lrt::GEOMETRY_PRECOMPUTED_TRANSFER;
	}
	if (has_precomputed) {
		Node *root = p_scene->instantiate(PackedScene::GEN_EDIT_STATE_DISABLED);
		if (root == nullptr) {
			preparation_error = "Cannot inspect precomputed LRT dependencies: " + p_scene->get_path();
			return;
		}
		_pack_precomputed_assets(root);
		memdelete(root);
		if (!preparation_error.is_empty()) {
			return;
		}
	}
	for (const KeyValue<String, Dictionary> &entry : volumes) {
		if (!bool(entry.value.get("is_volume", false))) {
			continue;
		}
		const uint64_t fingerprint = uint64_t(int64_t(entry.value.get("build_cache_fingerprint", int64_t(0))));
		const Dictionary export_data = entry.value.get("build_cache_export_data", Dictionary());
		if (fingerprint == 0 && bool(export_data.get("runtime_only", false))) {
			const PackedStringArray names = export_data.get("assets", PackedStringArray());
			for (const String &name : names) {
				const String dependency = lrt::asset_cache_directory().path_join(name);
				if (name.is_empty() || name != name.get_file() || !FileAccess::exists(dependency)) {
					preparation_error = p_scene->get_path() + ":" + entry.key + ": Missing or invalid dynamic LRT dependency: " + name;
					return;
				}
				_pack_dependency(dependency);
				if (!preparation_error.is_empty()) {
					return;
				}
			}
			continue;
		}
		const Dictionary report = LRTVolume::get_cached_export_dependencies(fingerprint);
		if (!bool(report.get("ok", false))) {
			preparation_error = p_scene->get_path() + ":" + entry.key + ": " + String(report["error"]);
			return;
		}
		const PackedStringArray dependencies = report["assets"];
		for (const String &dependency : dependencies) {
			_pack_dependency(dependency);
			if (!preparation_error.is_empty()) {
				return;
			}
		}
	}
}

Dictionary LRTExportPlugin::collect_export_dependencies(const Ref<PackedScene> &p_scene) {
	_reset();
	packing = false;
	if (p_scene.is_null()) {
		preparation_error = "A PackedScene is required for LRT export dependency collection.";
	} else {
		_pack_scene_data(p_scene);
	}
	return _report();
}

void LRTExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	if (!preparation_error.is_empty()) {
		return;
	}
	if (p_type != "PackedScene") {
		return;
	}
	const Ref<PackedScene> scene = ResourceLoader::load(p_path);
	ERR_FAIL_COND(scene.is_null());
	_pack_scene_data(scene);
	if (!preparation_error.is_empty()) {
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "LRT", preparation_error);
	}
}

void LRTExportPlugin::_export_end() {
	packing = false;
	_reset();
}

#endif
