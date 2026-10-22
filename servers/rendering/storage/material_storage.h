/**************************************************************************/
/*  material_storage.h                                                    */
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

#pragma once

#include "core/io/resource.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_types.h"
#include "servers/rendering/storage/utilities.h"

class RendererMaterialStorage {
	mutable Mutex global_shader_parameter_state_mutex;
	HashMap<StringName, RenderingServerTypes::GlobalShaderParameterState> global_shader_parameter_states;
	HashMap<RID, HashSet<StringName>> global_shader_parameter_texture_users;
	uint64_t global_shader_parameter_revision = 0;

	void _global_shader_parameter_unregister_texture(const StringName &p_name, RID p_texture) {
		HashSet<StringName> *users = global_shader_parameter_texture_users.getptr(p_texture);
		if (users) {
			users->erase(p_name);
			if (users->is_empty()) {
				global_shader_parameter_texture_users.erase(p_texture);
			}
		}
	}

protected:
	void _global_shader_parameter_publish(const StringName &p_name, RSE::GlobalShaderParameterType p_type, const Variant &p_value) {
		RID texture;
		if (p_type >= RSE::GLOBAL_VAR_TYPE_SAMPLER2D && p_type < RSE::GLOBAL_VAR_TYPE_MAX) {
			if (p_value.get_type() == Variant::RID) {
				texture = p_value;
			} else if (p_value.get_type() == Variant::OBJECT) {
				Ref<Resource> resource = p_value;
				if (resource.is_valid()) {
					texture = resource->get_rid();
				}
			}
		}
		MutexLock lock(global_shader_parameter_state_mutex);
		RenderingServerTypes::GlobalShaderParameterState &state = global_shader_parameter_states[p_name];
		if (state.type == p_type && state.value == p_value && state.texture == texture) {
			return;
		}
		_global_shader_parameter_unregister_texture(p_name, state.texture);
		if (texture.is_valid()) {
			global_shader_parameter_texture_users[texture].insert(p_name);
		}
		state.texture = texture;
		state.type = p_type;
		state.value = p_value;
		state.revision = ++global_shader_parameter_revision;
	}

	void _global_shader_parameter_unpublish(const StringName &p_name) {
		MutexLock lock(global_shader_parameter_state_mutex);
		const RenderingServerTypes::GlobalShaderParameterState *state = global_shader_parameter_states.getptr(p_name);
		if (state) {
			_global_shader_parameter_unregister_texture(p_name, state->texture);
		}
		global_shader_parameter_states.erase(p_name);
	}

	void _global_shader_parameters_unpublish() {
		MutexLock lock(global_shader_parameter_state_mutex);
		global_shader_parameter_texture_users.clear();
		global_shader_parameter_states.clear();
	}

public:
	void global_shader_parameter_texture_changed(RID p_texture) {
		MutexLock lock(global_shader_parameter_state_mutex);
		const HashSet<StringName> *users = global_shader_parameter_texture_users.getptr(p_texture);
		if (!users) {
			return;
		}
		for (const StringName &name : *users) {
			global_shader_parameter_states[name].revision = ++global_shader_parameter_revision;
		}
	}

	// Reads the last applied effective value without flushing the render command queue.
	RenderingServerTypes::GlobalShaderParameterState global_shader_parameter_get_state(const StringName &p_name) const {
		MutexLock lock(global_shader_parameter_state_mutex);
		const RenderingServerTypes::GlobalShaderParameterState *state = global_shader_parameter_states.getptr(p_name);
		return state ? *state : RenderingServerTypes::GlobalShaderParameterState();
	}

	virtual ~RendererMaterialStorage() {}

	/* GLOBAL SHADER UNIFORM API */
	virtual void global_shader_parameter_add(const StringName &p_name, RSE::GlobalShaderParameterType p_type, const Variant &p_value) = 0;
	virtual void global_shader_parameter_remove(const StringName &p_name) = 0;
	virtual Vector<StringName> global_shader_parameter_get_list() const = 0;

	virtual void global_shader_parameter_set(const StringName &p_name, const Variant &p_value) = 0;
	virtual void global_shader_parameter_set_override(const StringName &p_name, const Variant &p_value) = 0;
	virtual Variant global_shader_parameter_get(const StringName &p_name) const = 0;
	virtual RSE::GlobalShaderParameterType global_shader_parameter_get_type(const StringName &p_name) const = 0;

	virtual void global_shader_parameters_load_settings(bool p_load_textures = true) = 0;
	virtual void global_shader_parameters_clear() = 0;

	virtual int32_t global_shader_parameters_instance_allocate(RID p_instance) = 0;
	virtual void global_shader_parameters_instance_free(RID p_instance) = 0;
	virtual void global_shader_parameters_instance_update(RID p_instance, int p_index, const Variant &p_value, int p_flags_count = 0) = 0;

	/* SHADER API */
	virtual RID shader_allocate() = 0;
	virtual void shader_initialize(RID p_rid, bool p_embedded = true) = 0;
	virtual void shader_free(RID p_rid) = 0;

	virtual void shader_set_code(RID p_shader, const String &p_code) = 0;
	virtual void shader_set_path_hint(RID p_shader, const String &p_path) = 0;
	virtual String shader_get_code(RID p_shader) const = 0;
	virtual void get_shader_parameter_list(RID p_shader, List<PropertyInfo> *p_param_list) const = 0;

	virtual void shader_set_default_texture_parameter(RID p_shader, const StringName &p_name, RID p_texture, int p_index) = 0;
	virtual RID shader_get_default_texture_parameter(RID p_shader, const StringName &p_name, int p_index) const = 0;
	virtual Variant shader_get_parameter_default(RID p_material, const StringName &p_param) const = 0;

	virtual RenderingServerTypes::ShaderNativeSourceCode shader_get_native_source_code(RID p_shader) const = 0;
	virtual void shader_embedded_set_lock() = 0;
	virtual const HashSet<RID> &shader_embedded_set_get() const = 0;
	virtual void shader_embedded_set_unlock() = 0;

	/* MATERIAL API */

	virtual RID material_allocate() = 0;
	virtual void material_initialize(RID p_rid) = 0;
	virtual void material_free(RID p_rid) = 0;

	virtual void material_set_render_priority(RID p_material, int priority) = 0;
	virtual void material_set_shader(RID p_shader_material, RID p_shader) = 0;

	virtual void material_set_param(RID p_material, const StringName &p_param, const Variant &p_value) = 0;
	virtual Variant material_get_param(RID p_material, const StringName &p_param) const = 0;

	virtual void material_set_next_pass(RID p_material, RID p_next_material) = 0;

	virtual bool material_is_animated(RID p_material) = 0;
	virtual bool material_casts_shadows(RID p_material) = 0;
	virtual RSE::CullMode material_get_cull_mode(RID p_material) const = 0;

	struct InstanceShaderParam {
		PropertyInfo info;
		int index;
		Variant default_value;
	};

	virtual void material_get_instance_shader_parameters(RID p_material, List<InstanceShaderParam> *r_parameters) = 0;

	virtual void material_update_dependency(RID p_material, DependencyTracker *p_instance) = 0;
};
