/**************************************************************************/
/*  lrt_material_baker.cpp                                                */
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

#include "lrt_material_baker.h"

#include "lrt_geometry_binding.h"
#include "lrt_renderer.h"

#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#endif

namespace {
SafeNumeric<int> material_workers;
SafeNumeric<int> material_captures;
SafeNumeric<int> material_retiring;
Mutex material_requests_mutex;
Vector<LRTMaterialBaker::Request *> material_requests;
bool material_shutdown = false;
} //namespace

namespace {

enum BakeStage {
	STAGE_CONTEXT,
	STAGE_CAPTURE,
	STAGE_PREPARE,
	STAGE_SAMPLERS,
	STAGE_DRAW,
	STAGE_MESH,
	STAGE_READBACK,
	STAGE_COUNT,
};
struct BakeStageTiming {
	SafeNumeric<uint64_t> total;
	SafeNumeric<uint64_t> maximum;
};
struct BakeStageClock {
	BakeStageTiming &timing;
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	explicit BakeStageClock(BakeStageTiming &p_timing) :
			timing(p_timing) {}
	~BakeStageClock() {
		const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start;
		timing.total.add(elapsed);
		timing.maximum.exchange_if_greater(elapsed);
	}
};

struct MeshSnapshot : RefCounted {
	GDSOFTCLASS(MeshSnapshot, RefCounted);

public:
	RenderingServerTypes::SurfaceData metadata;
	RenderingServerTypes::SurfaceData data;
	Dictionary key;
	RID buffers[4];
	uint32_t sizes[4] = {};
	uint64_t revisions[4] = {};
	bool ready = false;
};
} // namespace

struct LRTMaterialBaker::Context::Data {
	RID globals;
	Vector<uint8_t> global_values;
	uint64_t download_budget = 0;
	double time = 0;
	double time_step = 0;
	Error error = ERR_UNAVAILABLE;
	HashMap<RID, String> shaders;
	Vector<Ref<Material>> shader_owners;
	HashMap<RID, RID> texture_copies;
	HashMap<RID, uint64_t> texture_revisions;
	HashMap<uint32_t, Vector<Ref<MeshSnapshot>>> mesh_snapshots;
	Vector<RID> texture_rids;
	BakeStageTiming timings[STAGE_COUNT];
};

struct LRTMaterialBaker::Readback : RefCounted {
	GDSOFTCLASS(Readback, RefCounted);

public:
	Ref<Request> owner;
	int slot = 0;
};

struct LRTMaterialBaker::Request::Data {
	Ref<Context> context;
	RenderingServerTypes::SurfaceData source;
	RID source_mesh;
	int source_surface = 0;
	Ref<MeshSnapshot> source_snapshot;
	RID source_copies[4];
	Ref<ArrayMesh> mesh;
	Ref<Material> material;
	Vector<Ref<Material>> passes;
	Vector<String> pass_paths;
	Vector<bool> opaque;
	struct Pass {
		RID shader;
		Dictionary signature;
		Vector<RID> textures;
		HashMap<StringName, Variant> parameters;
		int priority = 0;
	};
	Vector<Pass> captured_passes;
	RID instance;
	int32_t uniform_offset = -1;
	Transform3D transform;
	Transform3D camera;
	Projection projection;
	bool orthogonal = false;
	uint32_t camera_layers = UINT32_MAX;
	Color instance_color = Color(1, 1, 1, 1);
	Color instance_custom = Color(0, 0, 0, 0);
	int instance_index = 0;
	float texel_size = 0;
	Vector<uint8_t> vertices;
	Size2i size;
	Surface surface;
	Vector2i page;
	SafeNumeric<const SafeFlag *> external_cancel;
	SafeFlag cancelled;
	Mutex event_mutex;
	bool ready = false;
	WorkerThreadPool::TaskID waiter = WorkerThreadPool::INVALID_TASK_ID;
	Error error = ERR_UNAVAILABLE;

	struct Download {
		RID buffer;
		int source_channel = 0;
		uint32_t offset = 0;
		uint32_t size = 0;
		bool submitted = false;
		bool complete = false;
		Vector<uint8_t> bytes;
	};
	Vector<Download> downloads;
	Vector<Ref<Readback>> callbacks;
	bool reading_vertices = false;
	bool reading_source = false;
	bool page_drawn = false;
	bool samplers_captured = false;
	bool synchronous = false;
#ifdef RD_ENABLED
	RenderGeometryInstance *geometry = nullptr;
	Vector<RID> gpu_rids;
	Vector<RID> textures;
	RID framebuffer;
	RID buffer;
#endif
	bool is_cancelled() const {
		const SafeFlag *external = external_cancel.get();
		return cancelled.is_set() || (external && external->is_set());
	}
};

namespace {
Vector<Ref<LRTMaterialBaker::Request>> material_pending;
Vector<Ref<LRTMaterialBaker::Request>> material_capturing;
Vector<int64_t> material_retired_contexts;
} //namespace

LRTMaterialBaker::Context::Context() {
	data = memnew(Data);
}

LRTMaterialBaker::Context::~Context() {
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_free_context).bind(int64_t(data)));
	} else {
		memdelete(data);
	}
}

LRTMaterialBaker::Request::Request() {
	data = memnew(Data);
	MutexLock lock(material_requests_mutex);
	material_requests.push_back(this);
}

LRTMaterialBaker::Request::~Request() {
	{
		MutexLock lock(material_requests_mutex);
		material_requests.erase(this);
	}
	memdelete(data);
}

void LRTMaterialBaker::_complete(const Ref<Request> &p_request) {
	MutexLock lock(p_request->data->event_mutex);
	p_request->data->ready = true;
	if (p_request->data->waiter != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->notify_yield_over(p_request->data->waiter);
	}
}

void LRTMaterialBaker::_wait(const Ref<Request> &p_request) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const WorkerThreadPool::TaskID caller = pool->get_caller_task_id();
	while (true) {
		{
			MutexLock lock(p_request->data->event_mutex);
			if (p_request->data->ready) {
				p_request->data->waiter = WorkerThreadPool::INVALID_TASK_ID;
				return;
			}
			p_request->data->waiter = caller;
		}
		pool->yield();
	}
}

Ref<LRTMaterialBaker::Context> LRTMaterialBaker::capture_context() {
	ERR_FAIL_COND_V(!Thread::is_main_thread() || material_shutdown, Ref<Context>());
	Ref<Context> context;
	context.instantiate();
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_capture_context).bind(context));
	return context;
}

void LRTMaterialBaker::_capture_context(const Ref<Context> &p_context) {
	BakeStageClock clock(p_context->data->timings[STAGE_CONTEXT]);
#ifdef RD_ENABLED
	auto *renderer = RendererSceneRenderImplementation::RenderForwardClustered::get_singleton();
	RD *rd = RD::get_singleton();
	if (!renderer || !rd) {
		return;
	}
	static_cast<RendererSceneCull *>(RSG::scene)->update_dirty_instances();
	Context::Data &context = *p_context->data;
	context.global_values = RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_buffer_snapshot();
	if (context.global_values.is_empty()) {
		context.error = ERR_OUT_OF_MEMORY;
		return;
	}
	context.globals = rd->storage_buffer_create(context.global_values.size(), context.global_values);
	context.error = context.globals.is_valid() ? OK : ERR_OUT_OF_MEMORY;
	context.download_budget = rd->get_download_staging_buffer_max_size();
	context.time = renderer->time;
	context.time_step = renderer->time_step;
#endif
}

void LRTMaterialBaker::_free_context(int64_t p_data) {
	Context::Data *context = reinterpret_cast<Context::Data *>(p_data);
#ifdef RD_ENABLED
	RD *rd = RD::get_singleton();
	auto *renderer = RendererSceneRenderImplementation::RenderForwardClustered::get_singleton();
	auto *storage = RendererRD::MaterialStorage::get_singleton();
	if (rd && renderer) {
		bool ready = true;
		for (const KeyValue<RID, String> &program : context->shaders) {
			const auto *source = storage->get_shader(program.key);
			auto *shader = source ? static_cast<RendererSceneRenderImplementation::SceneShaderForwardClustered::ShaderData *>(source->data) : nullptr;
			if (shader && shader->version.is_valid()) {
				ready &= renderer->scene_shader.shader.version_is_ready(shader->version, false) && shader->pipeline_hash_map.is_compilation_ready();
			}
		}
		for (const Ref<Material> &material : context->shader_owners) {
			const auto *source = storage->get_material(material->get_rid());
			if (source && source->shader_type == RendererRD::MaterialStorage::SHADER_TYPE_3D) {
				auto *shader = static_cast<RendererSceneRenderImplementation::SceneShaderForwardClustered::ShaderData *>(source->shader->data);
				if (shader && shader->version.is_valid()) {
					ready &= renderer->scene_shader.shader.version_is_ready(shader->version, false) && shader->pipeline_hash_map.is_compilation_ready();
				}
			}
		}
		if (!ready) {
			material_retired_contexts.push_back(p_data);
			material_retiring.increment();
			return;
		}
	}
	if (rd) {
		for (int i = context->texture_rids.size() - 1; i >= 0; i--) {
			rd->free_rid(context->texture_rids[i]);
		}
		if (context->globals.is_valid()) {
			rd->free_rid(context->globals);
		}
	}
#endif
	memdelete(context);
}

Ref<LRTMaterialBaker::Request> LRTMaterialBaker::capture(GeometryInstance3D *p_instance, int p_surface, float p_texel_size, int p_instance_index, const Ref<Context> &p_context) {
	ERR_FAIL_COND_V(!Thread::is_main_thread() || material_shutdown, Ref<Request>());
	ERR_FAIL_NULL_V(p_instance, Ref<Request>());
	ERR_FAIL_COND_V(p_texel_size <= 0 || !Math::is_finite(p_texel_size), Ref<Request>());
	const Ref<Mesh> source = LRTGeometryBinding::get_mesh(p_instance);
	ERR_FAIL_COND_V(source.is_null(), Ref<Request>());
	Ref<Request> captured;
	captured.instantiate();
	Request::Data &request = *captured->data;
	request.context = p_context.is_valid() ? p_context : capture_context();
	request.source_mesh = source->get_rid();
	request.source_surface = p_surface;
	request.texel_size = p_texel_size;
	request.instance_index = p_instance_index;
	request.instance = p_instance->get_instance();
	request.transform = (p_instance->is_inside_tree() ? p_instance->get_global_transform() : p_instance->get_transform()) * LRTGeometryBinding::get_instance_transform(p_instance, p_instance_index);
	Ref<Material> base = LRTGeometryBinding::get_material(p_instance, p_surface);
	if (base.is_null()) {
		Ref<StandardMaterial3D> material;
		material.instantiate();
		base = material;
	}
	for (const Ref<Material> &first : { base, p_instance->get_material_overlay() }) {
		for (Ref<Material> material = first; material.is_valid(); material = material->get_next_pass()) {
			request.passes.push_back(material);
			request.pass_paths.push_back(material->get_path());
		}
	}
	if (p_instance->is_inside_tree()) {
		Camera3D *camera = p_instance->get_viewport()->get_camera_3d();
		if (camera) {
			request.camera = camera->get_global_transform();
			request.camera_layers = camera->get_cull_mask();
			request.projection = camera->get_camera_projection();
			request.orthogonal = camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
		}
	}
	if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance)) {
		const Ref<MultiMesh> data = multi->get_multimesh();
		if (data->is_using_colors()) {
			request.instance_color = data->get_instance_color(p_instance_index);
		}
		if (data->is_using_custom_data()) {
			request.instance_custom = data->get_instance_custom_data(p_instance_index);
		}
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_capture).bind(captured));
	return captured;
}

void LRTMaterialBaker::_capture(const Ref<Request> &p_request) {
	BakeStageClock clock(p_request->data->context->data->timings[STAGE_CAPTURE]);
	Request::Data &request = *p_request->data;
	request.error = request.context->data->error;
#ifdef RD_ENABLED
	if (request.error == OK && !request.is_cancelled()) {
		using namespace RendererSceneRenderImplementation;
		auto *storage = RendererRD::MaterialStorage::get_singleton();
		auto *cull = static_cast<RendererSceneCull *>(RSG::scene);
		cull->update_dirty_instances();
		Ref<MeshSnapshot> snapshot;
		snapshot.instantiate();
		request.error = RendererRD::MeshStorage::get_singleton()->mesh_get_surface_snapshot_info(request.source_mesh, request.source_surface,
				snapshot->metadata, snapshot->buffers, snapshot->sizes);
		if (request.error != OK) {
			_complete(p_request);
			return;
		}
		RD *rd = RD::get_singleton();
		Array buffers;
		for (int channel = 0; channel < 4; channel++) {
			Dictionary buffer;
			buffer["rid"] = snapshot->buffers[channel];
			buffer["size"] = snapshot->sizes[channel];
			snapshot->revisions[channel] = snapshot->buffers[channel].is_valid() ? rd->buffer_get_content_revision(snapshot->buffers[channel]) : 0;
			buffer["revision"] = int64_t(snapshot->revisions[channel]);
			buffers.push_back(buffer);
		}
		snapshot->key["buffers"] = buffers;
		snapshot->key["format"] = int64_t(snapshot->metadata.format);
		snapshot->key["primitive"] = snapshot->metadata.primitive;
		snapshot->key["vertices"] = snapshot->metadata.vertex_count;
		snapshot->key["indices"] = snapshot->metadata.index_count;
		snapshot->key["bounds"] = snapshot->metadata.aabb;
		snapshot->key["uv_scale"] = snapshot->metadata.uv_scale;
		auto &snapshots = request.context->data->mesh_snapshots[snapshot->key.hash()];
		for (const Ref<MeshSnapshot> &existing : snapshots) {
			if (existing->key == snapshot->key) {
				snapshot = existing;
				break;
			}
		}
		if (!snapshots.has(snapshot)) {
			snapshots.push_back(snapshot);
		}
		request.source_snapshot = snapshot;
		request.source = snapshot->metadata;
		auto *instance = cull->instance_owner.get_or_null(request.instance);
		if (!instance) {
			request.error = ERR_DOES_NOT_EXIST;
		} else if (request.captured_passes.resize(request.passes.size()) != OK) {
			request.error = ERR_OUT_OF_MEMORY;
		} else {
			request.uniform_offset = instance->instance_uniforms.location();
			Context::Data &context = *request.context->data;
			context.shader_owners.append_array(request.passes);
			RenderForwardClustered::get_singleton()->scene_shader.enable_material_shader_group();
			for (int pass = 0; pass < request.passes.size(); pass++) {
				const RID material = request.passes[pass]->get_rid();
				const auto *original = storage->get_material(material);
				auto *shader = original->shader_type == RendererRD::MaterialStorage::SHADER_TYPE_3D ? static_cast<SceneShaderForwardClustered::ShaderData *>(storage->material_get_shader_data(material)) : nullptr;
				const bool valid = shader && shader->version.is_valid();
				const bool opaque = valid && !(shader->uses_alpha || shader->uses_discard || shader->uses_blend_alpha || shader->uses_screen_texture || shader->uses_depth_texture || shader->uses_normal_texture);
				request.opaque.push_back(opaque);
				if (!valid) {
					Dictionary entry;
					entry["pass"] = pass;
					entry["material"] = request.pass_paths[pass];
					entry["reason"] = "Material has no valid Forward+ spatial shader.";
					request.surface.issues.push_back(entry);
				}
				Request::Data::Pass &snapshot = request.captured_passes.write[pass];
				snapshot.signature["code"] = original->shader ? original->shader->code : String();
				snapshot.signature["opaque"] = opaque;
				snapshot.signature["priority"] = original->priority;
				Dictionary parameters;
				for (const KeyValue<StringName, Variant> &parameter : original->params) {
					const Variant value = parameter.value.duplicate(true);
					snapshot.parameters.insert(parameter.key, value);
					parameters[parameter.key] = value;
				}
				snapshot.signature["parameters"] = parameters;
				if (!opaque) {
					continue;
				}
				snapshot.signature["vertex_deformation"] = shader->uses_vertex || shader->uses_position || shader->writes_modelview_or_projection || shader->uses_world_coordinates;
				const bool view_position = shader->uses_vertex_position || shader->uses_normal || shader->uses_tangent;
				if (shader->uses_model_transform || shader->uses_world_coordinates || shader->uses_position || view_position) {
					snapshot.signature["model"] = request.transform;
				}
				if (shader->uses_camera_transform || shader->writes_modelview_or_projection || shader->uses_position || view_position) {
					snapshot.signature["camera"] = request.camera;
					snapshot.signature["projection"] = request.projection;
					snapshot.signature["orthogonal"] = request.orthogonal;
					snapshot.signature["camera_layers"] = request.camera_layers;
				}
				if (shader->uses_time) {
					snapshot.signature["time"] = context.time;
					snapshot.signature["time_step"] = context.time_step;
				}
				if (shader->uses_instance_id) {
					snapshot.signature["instance_id"] = request.instance_index;
				}
				if (shader->uses_instance_custom) {
					snapshot.signature["instance_custom"] = request.instance_custom;
				}
				if (shader->uses_vertex_color) {
					snapshot.signature["instance_color"] = request.instance_color;
				}
				Dictionary globals;
				bool instance_parameters = false;
				for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &uniform : shader->uniforms) {
					if (uniform.value.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
						const auto *variable = storage->global_shader_uniforms.variables.getptr(uniform.key);
						if (variable && variable->buffer_index >= 0) {
							const int begin = variable->buffer_index * 16;
							globals[uniform.key] = context.global_values.slice(begin, begin + variable->buffer_elements * 16);
						}
					}
					instance_parameters |= uniform.value.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE;
				}
				snapshot.signature["globals"] = globals;
				if (instance_parameters && request.uniform_offset >= 0) {
					const int begin = request.uniform_offset * 16;
					snapshot.signature["instance_parameters"] = context.global_values.slice(begin, begin + ShaderLanguage::MAX_INSTANCE_UNIFORM_INDICES * 16);
				}
				const auto *source_shader = original->shader;
				snapshot.shader = source_shader->self;
				if (context.shaders.has(snapshot.shader) && context.shaders[snapshot.shader] != source_shader->code) {
					request.error = ERR_UNAVAILABLE;
					break;
				}
				context.shaders.insert(snapshot.shader, source_shader->code);
				snapshot.priority = original->priority;
			}
		}
	}
#endif
	if (_advance_capture(p_request, false)) {
		return;
	}
	if (material_capturing.push_back(p_request) != OK) {
		request.error = ERR_OUT_OF_MEMORY;
		_complete(p_request);
		return;
	}
	material_captures.increment();
}

bool LRTMaterialBaker::_advance_capture(const Ref<Request> &p_request, bool p_wait) {
	BakeStageClock clock(p_request->data->context->data->timings[STAGE_PREPARE]);
	Request::Data &request = *p_request->data;
	if (material_shutdown || request.is_cancelled()) {
		request.error = ERR_SKIP;
	}
#ifdef RD_ENABLED
	using namespace RendererSceneRenderImplementation;
	auto *renderer = RenderForwardClustered::get_singleton();
	auto *storage = RendererRD::MaterialStorage::get_singleton();
	RD *rd = RD::get_singleton();
	if (request.error == OK && !_check_programs(p_request)) {
		request.error = ERR_UNAVAILABLE;
	}
	if (request.error == OK) {
		for (const Request::Data::Pass &pass : request.captured_passes) {
			if (pass.shader.is_null()) {
				continue;
			}
			const auto *program = storage->get_shader(pass.shader);
			auto *shader = static_cast<SceneShaderForwardClustered::ShaderData *>(program->data);
			if (!shader || !shader->version.is_valid()) {
				request.error = ERR_UNAVAILABLE;
				_complete(p_request);
				return true;
			}
			if (!p_wait && !renderer->scene_shader.shader.version_is_ready(shader->version)) {
				return false;
			}
		}
		for (int pass = 0; pass < request.passes.size(); pass++) {
			if (!request.opaque[pass]) {
				continue;
			}
			Request::Data::Pass &snapshot = request.captured_passes.write[pass];
			auto *shader = static_cast<SceneShaderForwardClustered::ShaderData *>(storage->get_shader(snapshot.shader)->data);
			String issue;
			if (!shader->is_valid()) {
				issue = "Captured Forward+ program compilation failed.";
			} else {
				Ref<ShaderMaterial> copy;
				copy.instantiate();
				copy->set_render_priority(snapshot.priority);
				const RID material = copy->get_rid();
				storage->material_set_shader(material, snapshot.shader);
				for (const KeyValue<StringName, Variant> &parameter : snapshot.parameters) {
					storage->material_set_param(material, parameter.key, parameter.value);
				}
				request.passes.write[pass] = copy;
				auto *native = storage->get_material(material);
				auto *data = static_cast<SceneShaderForwardClustered::MaterialData *>(native->data);
				// Only this private material is initialized, after its native compiles are ready.
				data->update_parameters(native->params, true, true);
				snapshot.signature.erase("parameters");
				snapshot.signature["uniforms"] = data->get_parameter_bytes(true);
				Context::Data &context = *request.context->data;
				snapshot.textures = data->get_parameter_textures();
				for (const RID &texture : snapshot.textures) {
					if (context.texture_copies.has(texture)) {
						continue;
					}
					bool immutable_default = false;
					for (int i = 0; i < RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_MAX; i++) {
						immutable_default |= texture == RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DefaultRDTexture(i));
					}
					if (immutable_default) {
						continue;
					}
					if (!rd->texture_is_valid(texture)) {
						issue = "Material sampler has no valid texture.";
						break;
					}
					RD::TextureFormat format = rd->texture_get_format(texture);
					if (!(format.usage_bits & RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT)) {
						issue = "Sampler texture does not permit copying an immutable bake snapshot.";
						break;
					}
				}

				if (request.error != OK) {
					break;
				}
				if (issue.is_empty()) {
					data->freeze_parameters(context.texture_copies);
					data->free_parameters_uniform_set(data->uniform_set);
					data->uniform_set = RID();
					data->update_parameters(HashMap<StringName, Variant>(), false, false);
				}
			}
			if (!issue.is_empty()) {
				request.opaque.write[pass] = false;
				Dictionary entry;
				entry["pass"] = pass;
				entry["material"] = request.pass_paths[pass];
				entry["reason"] = issue;
				request.surface.issues.push_back(entry);
			}
		}
	}
#endif
	_update_texture_signature(p_request);
	_complete(p_request);
	return true;
}

void LRTMaterialBaker::_update_texture_signature(const Ref<Request> &p_request) {
#ifdef RD_ENABLED
	Request::Data &request = *p_request->data;
	Context::Data &context = *request.context->data;
	RD *rd = RD::get_singleton();
	for (Request::Data::Pass &snapshot : request.captured_passes) {
		Array textures;
		for (const RID &source : snapshot.textures) {
			Dictionary texture;
			texture["rid"] = source;
			const uint64_t *frozen = context.texture_revisions.getptr(source);
			const uint64_t revision = frozen ? *frozen : rd->texture_get_content_revision(source);
			texture["revision"] = int64_t(revision);
			if (revision == UINT64_MAX) {
				// External producers cannot provide an observable immutable content key.
				texture["request"] = p_request->get_instance_id();
			}
			textures.push_back(texture);
		}
		snapshot.signature["textures"] = textures;
	}
#endif
}

Error LRTMaterialBaker::_snapshot_samplers(const Ref<Request> &p_request) {
	BakeStageClock clock(p_request->data->context->data->timings[STAGE_SAMPLERS]);
#ifdef RD_ENABLED
	using namespace RendererSceneRenderImplementation;
	Request::Data &request = *p_request->data;
	if (!_check_programs(p_request)) {
		return ERR_UNAVAILABLE;
	}
	Context::Data &context = *request.context->data;
	RD *rd = RD::get_singleton();
	auto *storage = RendererRD::MaterialStorage::get_singleton();
	for (int pass = 0; pass < request.captured_passes.size(); pass++) {
		if (!request.opaque[pass]) {
			continue;
		}
		const Request::Data::Pass &snapshot = request.captured_passes[pass];
		for (const RID &texture : snapshot.textures) {
			if (context.texture_copies.has(texture)) {
				continue;
			}
			bool immutable_default = false;
			for (int i = 0; i < RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_MAX; i++) {
				immutable_default |= texture == RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DefaultRDTexture(i));
			}
			if (immutable_default) {
				continue;
			}
			if (!rd->texture_is_valid(texture)) {
				return ERR_UNAVAILABLE;
			}
			RD::TextureFormat format = rd->texture_get_format(texture);
			format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			format.is_discardable = false;
			format.is_resolve_buffer = false;
			const RID copy = rd->texture_create(format, RD::TextureView());
			if (copy.is_null()) {
				request.error = ERR_OUT_OF_MEMORY;
				break;
			}
			if (context.texture_rids.push_back(copy) != OK) {
				rd->free_rid(copy);
				request.error = ERR_OUT_OF_MEMORY;
				break;
			}
			for (uint32_t layer = 0; layer < format.array_layers && request.error == OK; layer++) {
				for (uint32_t mip = 0; mip < format.mipmaps && request.error == OK; mip++) {
					const Vector3i size(MAX(1u, format.width >> mip), MAX(1u, format.height >> mip), MAX(1u, format.depth >> mip));
					const Error error = rd->texture_copy(texture, copy, Vector3(), Vector3(), size, mip, mip, layer, layer);
					if (error != OK) {
						request.error = ERR_UNAVAILABLE;
						break;
					}
				}
			}
			if (request.error != OK) {
				break;
			}
			RD::TextureView view = rd->texture_get_view(texture);
			if (view.format_override == format.format) {
				view.format_override = RD::DATA_FORMAT_MAX;
			}
			RID sampled_copy = copy;
			if (!(view == RD::TextureView())) {
				sampled_copy = rd->texture_create_shared(view, copy);
				if (sampled_copy.is_null()) {
					request.error = ERR_OUT_OF_MEMORY;
					break;
				}
				if (context.texture_rids.push_back(sampled_copy) != OK) {
					rd->free_rid(sampled_copy);
					request.error = ERR_OUT_OF_MEMORY;
					break;
				}
			}
			context.texture_copies.insert(texture, sampled_copy);
			context.texture_revisions.insert(texture, rd->texture_get_content_revision(texture));
		}
		if (request.error != OK) {
			return request.error;
		}
		auto *native = storage->get_material(request.passes[pass]->get_rid());
		auto *data = static_cast<SceneShaderForwardClustered::MaterialData *>(native->data);
		data->freeze_parameters(context.texture_copies);
		data->free_parameters_uniform_set(data->uniform_set);
		data->uniform_set = RID();
		data->update_parameters(HashMap<StringName, Variant>(), false, false);
	}
	request.samplers_captured = true;
	_update_texture_signature(p_request);
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

void LRTMaterialBaker::_capture_sync(const Ref<Request> &p_request) {
	if (material_capturing.erase(p_request)) {
		material_captures.decrement();
		_advance_capture(p_request, true);
	}
#ifdef RD_ENABLED
	if (p_request->data->error == OK) {
		p_request->data->source = static_cast<const RendererRD::MeshStorage *>(RendererRD::MeshStorage::get_singleton())->mesh_get_surface(p_request->data->source_mesh, p_request->data->source_surface);
	}
#endif
}

Error LRTMaterialBaker::_prepare(const Ref<Request> &p_request) {
	Request::Data &request = *p_request->data;
	Surface &r_surface = request.surface;
	if (request.error != OK) {
		return request.error;
	}
	if (request.is_cancelled()) {
		return ERR_SKIP;
	}
	for (int pass = request.passes.size() - 1; pass >= 0; pass--) {
		if (request.opaque[pass]) {
			request.passes[pass]->set_next_pass(request.material);
			request.material = request.passes[pass];
		}
	}
	if (request.material.is_null()) {
		r_surface.transparent = true;
		return OK;
	}
	Ref<SurfaceTool> tool;
	tool.instantiate();
	const Array source_arrays = RenderingServer::get_singleton()->mesh_create_arrays_from_surface_data(request.source);
	Array tool_arrays = source_arrays.duplicate();
	// SurfaceTool only decodes float custom channels. Preserve byte/half channels
	// as opaque attribute records, using the same deindexed vertex mapping.
	for (int channel = 0; channel < RSE::ARRAY_CUSTOM_COUNT; channel++) {
		if (tool_arrays[RSE::ARRAY_CUSTOM0 + channel].get_type() == Variant::PACKED_BYTE_ARRAY) {
			tool_arrays[RSE::ARRAY_CUSTOM0 + channel] = Variant();
		}
	}
	tool->create_from_triangle_arrays(tool_arrays);
	tool->deindex();
	Array arrays = tool->commit_to_arrays();
	const Vector<Vector3> positions = arrays[Mesh::ARRAY_VERTEX];
	const Vector<int> indices = source_arrays[Mesh::ARRAY_INDEX];
	uint64_t custom_flags = request.source.format & RSE::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
	for (int channel = 0; channel < RSE::ARRAY_CUSTOM_COUNT; channel++) {
		const int shift = RSE::ARRAY_FORMAT_CUSTOM_BASE + channel * RSE::ARRAY_FORMAT_CUSTOM_BITS;
		const uint64_t format = (request.source.format >> shift) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
		custom_flags |= format << shift;
		const Variant source = source_arrays[RSE::ARRAY_CUSTOM0 + channel];
		if (source.get_type() != Variant::PACKED_BYTE_ARRAY) {
			continue;
		}
		static constexpr int strides[4] = { 4, 4, 4, 8 };
		ERR_FAIL_COND_V(format >= 4, ERR_INVALID_DATA);
		const int stride = strides[format];
		const Vector<uint8_t> bytes = source;
		Vector<uint8_t> remapped;
		ERR_FAIL_COND_V(remapped.resize(int64_t(positions.size()) * stride) != OK, ERR_OUT_OF_MEMORY);
		uint8_t *target = remapped.ptrw();
		for (int vertex = 0; vertex < positions.size(); vertex++) {
			const int original = indices.is_empty() ? vertex : indices[vertex];
			memcpy(target + int64_t(vertex) * stride, bytes.ptr() + int64_t(original) * stride, stride);
		}
		arrays[RSE::ARRAY_CUSTOM0 + channel] = remapped;
	}
	request.mesh.instantiate();
	request.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), custom_flags);
	request.mesh->surface_set_material(0, request.material);
	ERR_FAIL_COND_V(positions.is_empty() || positions.size() % 3 != 0, ERR_INVALID_DATA);
	ERR_FAIL_COND_V(r_surface.atlas_uv.resize(positions.size()) != OK, ERR_OUT_OF_MEMORY);
	// Pack independent triangles by surface-space extent, at four texels per
	// eventual geometry sample cell. Original UV2 and custom channels are untouched.
	struct Tile {
		Vector2 corners[3];
		Vector2i size;
	};
	Vector<Tile> tiles;
	double area = 0;
	int largest = 1;
	for (int face = 0; face < positions.size(); face += 3) {
		if (request.is_cancelled()) {
			return ERR_SKIP;
		}
		int start = 0;
		for (int edge = 1; edge < 3; edge++) {
			if (positions[face + edge].distance_squared_to(positions[face + (edge + 1) % 3]) >
					positions[face + start].distance_squared_to(positions[face + (start + 1) % 3])) {
				start = edge;
			}
		}
		const int second = (start + 1) % 3;
		const int third = (start + 2) % 3;
		const bool finite = positions[face].is_finite() && positions[face + 1].is_finite() && positions[face + 2].is_finite();
		const Vector3 u = finite ? positions[face + second] - positions[face + start] : Vector3();
		const Vector3 v = finite ? positions[face + third] - positions[face + start] : Vector3();
		const real_t length = u.length();
		// Degenerate source triangles still need material vertex capture: the shader
		// may deform them. Give them a minimum tile; geometry filtering runs later.
		const real_t projected = length > 0 ? v.dot(u) / length / length : 0;
		const real_t height = length > 0 ? u.cross(v).length() / length : 0;
		Tile tile;
		tile.corners[start] = Vector2();
		const Vector2 extent(MAX(2.0f, float(length / request.texel_size)), MAX(2.0f, float(height / request.texel_size)));
		tile.corners[second] = Vector2(extent.x, 0);
		tile.corners[third] = Vector2(projected * extent.x, extent.y);
		ERR_FAIL_COND_V(!extent.is_finite() || double(extent.x) > INT32_MAX - 6.0 || double(extent.y) > INT32_MAX - 6.0, ERR_INVALID_DATA);
		tile.size = Vector2i(Math::ceil(extent.x), Math::ceil(extent.y)) + Vector2i(6, 6);
		largest = MAX(largest, MAX(tile.size.x, tile.size.y));
		area += double(tile.size.x) * tile.size.y;
		ERR_FAIL_COND_V(tiles.push_back(tile) != OK, ERR_OUT_OF_MEMORY);
	}
	ERR_FAIL_COND_V(!Math::is_finite(area) || area > double(INT32_MAX) * INT32_MAX, ERR_OUT_OF_MEMORY);
	const int target_width = MAX(largest, int(Math::ceil(Math::sqrt(area))));
	const int width = target_width;
	Vector2i cursor;
	int row_height = 0;
	for (int face = 0; face < tiles.size(); face++) {
		const Tile &tile = tiles[face];
		if (cursor.x > width - tile.size.x) {
			cursor.x = 0;
			ERR_FAIL_COND_V(cursor.y > INT32_MAX - row_height, ERR_OUT_OF_MEMORY);
			cursor.y += row_height;
			row_height = 0;
		}
		for (int corner = 0; corner < 3; corner++) {
			r_surface.atlas_uv.write[face * 3 + corner] = Vector2(cursor + Vector2i(3, 3)) + tile.corners[corner];
		}
		cursor.x += tile.size.x;
		row_height = MAX(row_height, tile.size.y);
	}
	ERR_FAIL_COND_V(cursor.y > INT32_MAX - row_height, ERR_OUT_OF_MEMORY);
	r_surface.atlas_size = Size2i(width, cursor.y + row_height);
	const int side = CLAMP(int(Math::sqrt(double(request.context->data->download_budget) / 128.0)), 1, Surface::PAGE_SIZE);
	request.size = r_surface.atlas_size.min(Size2i(side, side));
	r_surface.page_size = request.size;
	ERR_FAIL_COND_V(uint64_t(positions.size()) > (UINT32_MAX - 48) / 32, ERR_OUT_OF_MEMORY);
	ERR_FAIL_COND_V(request.vertices.resize(48 + positions.size() * 32) != OK, ERR_OUT_OF_MEMORY);
	request.vertices.fill(0);
	encode_float(1, request.vertices.ptrw());
	encode_uint32(request.instance_index, request.vertices.ptrw() + 4);
	encode_float(float(r_surface.atlas_size.x) / request.size.x, request.vertices.ptrw() + 8);
	encode_float(float(r_surface.atlas_size.y) / request.size.y, request.vertices.ptrw() + 12);
	for (int channel = 0; channel < 4; channel++) {
		encode_float(request.instance_color[channel], request.vertices.ptrw() + 16 + channel * 4);
		encode_float(request.instance_custom[channel], request.vertices.ptrw() + 32 + channel * 4);
	}
	for (int index = 0; index < positions.size(); index++) {
		r_surface.atlas_uv.write[index] /= Vector2(r_surface.atlas_size);
		uint8_t *record = request.vertices.ptrw() + 48 + index * 32;
		encode_float(r_surface.atlas_uv[index].x, record);
		encode_float(r_surface.atlas_uv[index].y, record + 4);
		encode_uint32(indices.is_empty() ? index : indices[index], record + 8);
	}

	return OK;
}

void LRTMaterialBaker::Surface::sample(const Vector2 &p_uv, Color &r_albedo, Color &r_emission) const {
	const Vector2 pixel = p_uv * Vector2(atlas_size) - Vector2(0.5, 0.5);
	const Vector2i cell(Math::floor(pixel.x), Math::floor(pixel.y));
	const Vector2 fraction = pixel - Vector2(cell);
	const int columns = (atlas_size.x + page_size.x - 1) / page_size.x;
	auto bilinear = [&](bool p_emission) {
		Color values[4];
		for (int corner = 0; corner < 4; corner++) {
			const int x = CLAMP(cell.x + (corner & 1), 0, atlas_size.x - 1);
			const int y = CLAMP(cell.y + (corner >> 1), 0, atlas_size.y - 1);
			const Page &page = pages[(y / page_size.y) * columns + x / page_size.x];
			const Ref<Image> &image = p_emission ? page.emission : page.albedo;
			values[corner] = image->get_pixel(x % page_size.x, y % page_size.y);
		}
		return values[0].lerp(values[1], fraction.x).lerp(values[2].lerp(values[3], fraction.x), fraction.y);
	};
	r_albedo = bilinear(false);
	r_albedo.a = 1;
	r_emission = has_emission ? bilinear(true) : Color(0, 0, 0);
}

bool LRTMaterialBaker::_check_programs(const Ref<Request> &p_request) {
#ifdef RD_ENABLED
	for (const KeyValue<RID, String> &captured : p_request->data->context->data->shaders) {
		const auto *program = RendererRD::MaterialStorage::get_singleton()->get_shader(captured.key);
		if (!program || program->code != captured.value) {
			Dictionary issue;
			issue["pass"] = -1;
			issue["reason"] = "A shader program changed while evaluating the captured bake inputs.";
			p_request->data->surface.issues.push_back(issue);
			return false;
		}
	}
#endif
	return true;
}

Error LRTMaterialBaker::_render_page(const Ref<Request> &p_request) {
	BakeStageClock clock(p_request->data->context->data->timings[STAGE_DRAW]);
	if (!p_request->data->samplers_captured) {
		const Error error = _snapshot_samplers(p_request);
		if (error != OK) {
			return error;
		}
	}
#ifdef RD_ENABLED
	using namespace RendererSceneRenderImplementation;
	Request::Data &request = *p_request->data;
	RenderForwardClustered *renderer = RenderForwardClustered::get_singleton();
	RD *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(renderer, ERR_UNAVAILABLE);
	ERR_FAIL_NULL_V(rd, ERR_UNAVAILABLE);
	if (!_check_programs(p_request)) {
		return ERR_UNAVAILABLE;
	}
	if (!request.geometry) {
		RSG::utilities->update_dirty_resources();
		request.geometry = renderer->geometry_instance_create(request.mesh->get_rid());
		ERR_FAIL_NULL_V(request.geometry, ERR_OUT_OF_MEMORY);
		request.geometry->set_transform(request.transform, request.mesh->get_aabb(), request.transform.xform(request.mesh->get_aabb()));
		request.geometry->set_instance_shader_uniforms_offset(request.uniform_offset);
		ERR_FAIL_COND_V(request.textures.resize(6) != OK, ERR_OUT_OF_MEMORY);
		for (int attachment = 0; attachment < 6; attachment++) {
			RD::TextureFormat format;
			format.width = request.size.x;
			format.height = request.size.y;
			format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
			format.format = attachment == 0 || attachment == 3 ? RD::DATA_FORMAT_R32G32B32A32_SFLOAT : RD::DATA_FORMAT_R8G8B8A8_UNORM;
			if (attachment == 4) {
				format.format = RD::DATA_FORMAT_R32_SFLOAT;
			} else if (attachment == 5) {
				format.format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D32_SFLOAT, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D32_SFLOAT : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
				format.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
			request.textures.write[attachment] = rd->texture_create(format, RD::TextureView());
			ERR_FAIL_COND_V(request.textures[attachment].is_null(), ERR_OUT_OF_MEMORY);
		}
		request.framebuffer = rd->framebuffer_create(request.textures);
		ERR_FAIL_COND_V(request.framebuffer.is_null(), ERR_OUT_OF_MEMORY);
		request.buffer = rd->storage_buffer_create(request.vertices.size(), request.vertices);
		ERR_FAIL_COND_V(request.buffer.is_null(), ERR_OUT_OF_MEMORY);
	}
	LRTRenderer::get_singleton()->set_material_bake_buffer(request.buffer);
	PagedArrayPool<RenderGeometryInstance *> pool;
	PagedArray<RenderGeometryInstance *> instances;
	instances.set_page_pool(&pool);
	instances.push_back(request.geometry);
	// The same shaders, uniforms, instance uniforms, transforms and texture samplers
	// as normal Forward+ rendering, with an independent rasterization coordinate.
	RenderSceneDataRD scene;
	scene.cam_transform = request.camera;
	scene.main_cam_transform = request.camera;
	scene.cam_projection = request.projection;
	scene.cam_orthogonal = request.orthogonal;
	scene.camera_visible_layers = request.camera_layers;
	scene.view_projection[0] = request.projection;
	scene.emissive_exposure_normalization = 1;
	scene.time = request.context->data->time;
	scene.time_step = request.context->data->time_step;
	RenderDataRD data;
	data.scene_data = &scene;
	data.instances = &instances;
	data.cluster_size = 1;
	data.cluster_max_elements = 32;
	renderer->scene_shader.enable_material_shader_group();
	renderer->_update_render_base_uniform_set(request.context->data->globals);
	const uint32_t ubo = renderer->_setup_environment(&data, true, request.size, request.size, Color());
	const auto list = RenderForwardClustered::RENDER_LIST_SECONDARY;
	const auto pass = RenderForwardClustered::PASS_MODE_DEPTH_MATERIAL;
	renderer->_fill_render_list(list, &data, pass);
	renderer->render_list[list].sort_by_key();
	renderer->_fill_instance_data(list);
	RID uniforms = renderer->_setup_render_pass_uniform_set(list, nullptr, false, RID(), RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default(), ubo);
	ERR_FAIL_COND_V(uniforms.is_null(), ERR_CANT_CREATE);
	auto &render_list = renderer->render_list[list];
	ERR_FAIL_COND_V(render_list.elements.is_empty(), ERR_CANT_CREATE);
	RenderForwardClustered::RenderListParameters params(render_list.elements.ptr(), render_list.element_info.ptr(), render_list.elements.size(), true, pass, 0, true, false, uniforms, true);
	Vector<Color> clear = { Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0) };

	const Vector2 page_offset(-float(request.page.x) / request.size.x, -float(request.page.y) / request.size.y);
	const RD::DrawListID draw = rd->draw_list_begin(request.framebuffer, RD::DRAW_CLEAR_ALL, clear, 0);
	if (!request.synchronous) {
		params.pipeline_preflight = true;
		params.force_wireframe = true;
		renderer->_render_list(draw, rd->framebuffer_get_format(request.framebuffer), &params, 0, params.element_count);
		params.force_wireframe = false;
		renderer->_render_list(draw, rd->framebuffer_get_format(request.framebuffer), &params, 0, params.element_count);
		params.pipeline_preflight = false;
		if (!params.pipelines_ready) {
			rd->draw_list_end();
			LRTRenderer::get_singleton()->set_material_bake_buffer(RID());
			renderer->_update_render_base_uniform_set();
			instances.clear();
			return params.pipelines_failed ? ERR_UNAVAILABLE : ERR_BUSY;
		}
	}
	params.force_wireframe = true;
	// Match the native UV2 baker's conservative border and three-texel gutters.
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			params.uv_offset = page_offset + Vector2(float(x) / request.size.x, float(y) / request.size.y);
			renderer->_render_list(draw, rd->framebuffer_get_format(request.framebuffer), &params, 0, params.element_count);
		}
	}
	params.force_wireframe = false;
	params.uv_offset = page_offset;
	renderer->_render_list(draw, rd->framebuffer_get_format(request.framebuffer), &params, 0, params.element_count);
	rd->draw_list_end();

	LRTRenderer::get_singleton()->set_material_bake_buffer(RID());
	renderer->_update_render_base_uniform_set();
	instances.clear();
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

void LRTMaterialBaker::_release_gpu(const Ref<Request> &p_request) {
	Request::Data &request = *p_request->data;
	// Detach callbacks before allowing the worker to return or release its inputs.
	for (const Ref<Readback> &callback : request.callbacks) {
		callback->owner.unref();
	}
	request.callbacks.clear();
#ifdef RD_ENABLED
	using namespace RendererSceneRenderImplementation;
	LRTRenderer::get_singleton()->set_material_bake_buffer(RID());
	RenderForwardClustered *renderer = RenderForwardClustered::get_singleton();
	renderer->_update_render_base_uniform_set();
	RD *rd = RD::get_singleton();
	if (request.framebuffer.is_valid()) {
		rd->free_rid(request.framebuffer);
		request.framebuffer = RID();
	}
	if (request.buffer.is_valid()) {
		rd->free_rid(request.buffer);
		request.buffer = RID();
	}
	for (RID &source : request.source_copies) {
		if (source.is_valid()) {
			rd->free_rid(source);
			source = RID();
		}
	}
	for (const RID &texture : request.textures) {
		if (texture.is_valid()) {
			rd->free_rid(texture);
		}
	}
	request.textures.clear();
	if (request.geometry) {
		renderer->geometry_instance_free(request.geometry);
		request.geometry = nullptr;
	}
#endif
}

void LRTMaterialBaker::_render_sync(const Ref<Request> &p_request) {
	Request::Data &request = *p_request->data;
#ifdef RD_ENABLED
	RD *rd = RD::get_singleton();
	for (request.page.y = 0; request.page.y < request.surface.atlas_size.y; request.page.y += request.size.y) {
		for (request.page.x = 0; request.page.x < request.surface.atlas_size.x; request.page.x += request.size.x) {
			request.error = _render_page(p_request);
			if (request.error != OK) {
				_release_gpu(p_request);
				return;
			}
			Surface::Page page;
			page.albedo = Image::create_from_data(request.size.x, request.size.y, false, Image::FORMAT_RGBAF, rd->texture_get_data(request.textures[0], 0));
			page.emission = Image::create_from_data(request.size.x, request.size.y, false, Image::FORMAT_RGBAF, rd->texture_get_data(request.textures[3], 0));
			if (page.albedo.is_null() || page.emission.is_null() || request.surface.pages.push_back(page) != OK) {
				request.error = ERR_OUT_OF_MEMORY;
				_release_gpu(p_request);
				return;
			}
		}
	}
	request.vertices = rd->buffer_get_data(request.buffer);
#endif
	_release_gpu(p_request);
}

void LRTMaterialBaker::_readback(const Vector<uint8_t> &p_bytes, const Ref<Readback> &p_readback) {
	const Ref<Request> owner = p_readback->owner;
	p_readback->owner.unref();
	if (owner.is_null()) {
		return;
	}
	Request::Data &request = *owner->data;
	Request::Data::Download &download = request.downloads.write[p_readback->slot];
	if (p_bytes.size() != download.size) {
		request.error = ERR_UNAVAILABLE;
	} else {
		download.bytes = p_bytes;
		download.complete = true;
	}
}

void LRTMaterialBaker::_begin_page(const Ref<Request> &p_request) {
	Request::Data &request = *p_request->data;
	request.error = request.is_cancelled() ? ERR_SKIP : _render_page(p_request);
	request.page_drawn = request.error == OK;
	if (request.error == ERR_BUSY) {
		request.error = OK;
	}
	if (request.error != OK) {
		_release_gpu(p_request);
		_complete(p_request);
		return;
	}
	request.reading_vertices = false;
	request.reading_source = false;
	request.downloads.clear();
	if (request.downloads.resize(2) != OK) {
		request.error = ERR_OUT_OF_MEMORY;
		_release_gpu(p_request);
		_complete(p_request);
		return;
	}
	for (int i = 0; i < 2; i++) {
		request.downloads.write[i].size = request.size.x * request.size.y * 16;
	}
	if (material_pending.push_back(p_request) != OK) {
		request.error = ERR_OUT_OF_MEMORY;
		_release_gpu(p_request);
		_complete(p_request);
		return;
	}
	_process_pending();
}

void LRTMaterialBaker::_begin_source(const Ref<Request> &p_request) {
	BakeStageClock clock(p_request->data->context->data->timings[STAGE_MESH]);
	Request::Data &request = *p_request->data;
	request.reading_source = true;
	request.downloads.clear();
#ifdef RD_ENABLED
	RD *rd = RD::get_singleton();
	const MeshSnapshot &source = **request.source_snapshot;
	const uint32_t chunk = MIN(uint64_t(16 * 1024 * 1024), request.context->data->download_budget / 4);
	for (int channel = 0; channel < 4 && request.error == OK; channel++) {
		if (!source.sizes[channel]) {
			continue;
		}
		if (rd->buffer_get_content_revision(source.buffers[channel]) != source.revisions[channel]) {
			Dictionary issue;
			issue["pass"] = -1;
			issue["reason"] = vformat("Mesh input buffer %d changed or became unavailable after capture.", channel);
			request.surface.issues.push_back(issue);
			request.error = ERR_UNAVAILABLE;
			break;
		}
		const RID copy = rd->storage_buffer_create(source.sizes[channel]);
		if (copy.is_null()) {
			request.error = ERR_OUT_OF_MEMORY;
			break;
		}
		request.source_copies[channel] = copy;
		request.error = rd->buffer_copy(source.buffers[channel], copy, 0, 0, source.sizes[channel]);
		for (uint64_t offset = 0; offset < source.sizes[channel] && request.error == OK; offset += chunk) {
			Request::Data::Download download;
			download.buffer = copy;
			download.source_channel = channel;
			download.offset = offset;
			download.size = MIN(uint64_t(chunk), source.sizes[channel] - offset);
			if (request.downloads.push_back(download)) {
				request.error = ERR_OUT_OF_MEMORY;
			}
		}
	}
#endif
	if (request.error != OK || material_pending.push_back(p_request)) {
		if (request.error == OK) {
			request.error = ERR_OUT_OF_MEMORY;
		}
		_release_gpu(p_request);
		_complete(p_request);
		return;
	}
	_process_pending();
}

void LRTMaterialBaker::_begin_vertices(const Ref<Request> &p_request) {
	Request::Data &request = *p_request->data;
	request.reading_vertices = true;
	const uint32_t chunk = MIN(uint64_t(16 * 1024 * 1024), request.context->data->download_budget / 4);
	request.downloads.clear();
	const int count = (request.vertices.size() + chunk - 1) / chunk;
	if (request.downloads.resize(count) != OK) {
		request.error = ERR_OUT_OF_MEMORY;
		_release_gpu(p_request);
		_complete(p_request);
		return;
	}
	for (int i = 0; i < count; i++) {
		request.downloads.write[i].offset = uint64_t(i) * chunk;
		request.downloads.write[i].size = MIN(uint64_t(chunk), uint64_t(request.vertices.size()) - uint64_t(i) * chunk);
	}
	if (material_pending.push_back(p_request) != OK) {
		request.error = ERR_OUT_OF_MEMORY;
		_release_gpu(p_request);
		_complete(p_request);
		return;
	}
	_process_pending();
}

void LRTMaterialBaker::_process_pending() {
	for (int index = material_retired_contexts.size() - 1; index >= 0; index--) {
		const int64_t context = material_retired_contexts[index];
		material_retired_contexts.remove_at(index);
		material_retiring.decrement();
		_free_context(context);
	}
	for (int index = material_capturing.size() - 1; index >= 0; index--) {
		if (_advance_capture(material_capturing[index], false)) {
			material_capturing.remove_at(index);
			material_captures.decrement();
		}
	}
	for (int index = material_pending.size() - 1; index >= 0; index--) {
		const Ref<Request> owner = material_pending[index];
		BakeStageClock clock(owner->data->context->data->timings[STAGE_READBACK]);
		Request::Data &request = *owner->data;
		if (material_shutdown || request.is_cancelled()) {
			request.error = ERR_SKIP;
		}
		if (request.error == OK && !request.reading_source && !request.reading_vertices && !request.page_drawn) {
			const Error error = _render_page(owner);
			if (error == ERR_BUSY) {
				continue;
			}
			request.error = error;
			request.page_drawn = error == OK;
		}
		bool complete = request.error != OK;
#ifdef RD_ENABLED
		if (!complete) {
			complete = true;
			for (int slot = 0; slot < request.downloads.size(); slot++) {
				Request::Data::Download &download = request.downloads.write[slot];
				if (!download.submitted) {
					Ref<Readback> callback;
					callback.instantiate();
					callback->owner = owner;
					callback->slot = slot;
					if (request.callbacks.push_back(callback) != OK) {
						callback->owner.unref();
						request.error = ERR_OUT_OF_MEMORY;
						break;
					}
					const Callable action = callable_mp_static(&LRTMaterialBaker::_readback).bind(callback);
					RD *rd = RD::get_singleton();
					const Error error = (request.reading_vertices || request.reading_source) ? rd->buffer_get_data_async(request.reading_source ? download.buffer : request.buffer, action, download.offset, download.size, false) : rd->texture_get_data_async(request.textures[slot == 0 ? 0 : 3], 0, action, false);
					if (error != OK) {
						request.callbacks.resize(request.callbacks.size() - 1);
					}
					if (error == ERR_BUSY) {
						callback->owner.unref();
						complete = false;
						break;
					}
					if (error != OK) {
						callback->owner.unref();
						request.error = error;
						break;
					}
					download.submitted = true;
				}
				complete &= download.complete;
			}
		}
#endif
		if (request.error != OK || complete) {
			for (const Ref<Readback> &callback : request.callbacks) {
				callback->owner.unref();
			}
			request.callbacks.clear();
			if (request.error != OK || request.reading_vertices || request.reading_source) {
				_release_gpu(owner);
			}
			material_pending.remove_at(index);
			_complete(owner);
		}
	}
}

bool LRTMaterialBaker::has_pending() {
	return material_workers.get() != 0 || material_captures.get() != 0 || material_retiring.get() != 0;
}

void LRTMaterialBaker::process() {
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_process_pending));
}

Error LRTMaterialBaker::get_signature(const Ref<Request> &p_request, Dictionary &r_signature, const SafeFlag *p_cancel) {
	Request::Data &request = *p_request->data;
	request.external_cancel.set(p_cancel);
	_wait(p_request);
	request.external_cancel.set(nullptr);
	if (request.error != OK) {
		return request.error;
	}
	Array passes;
	for (const Request::Data::Pass &pass : request.captured_passes) {
		passes.push_back(pass.signature);
	}
	r_signature["passes"] = passes;
	r_signature["format"] = int64_t(request.source.format);
	r_signature["source"] = request.source_snapshot->key;
	r_signature["vertex_count"] = request.source.vertex_count;
	r_signature["index_count"] = request.source.index_count;
	r_signature["bounds"] = request.source.aabb;
	r_signature["uv_scale"] = request.source.uv_scale;
	r_signature["texel_size"] = request.texel_size;
	r_signature["download_budget"] = int64_t(request.context->data->download_budget);
	return OK;
}

Ref<RefCounted> LRTMaterialBaker::get_source_snapshot(const Ref<Request> &p_request) {
	return p_request->data->source_snapshot;
}

Dictionary LRTMaterialBaker::get_render_timings(const Ref<Context> &p_context) {
	static constexpr const char *names[STAGE_COUNT] = { "context", "capture", "prepare", "samplers", "draw", "mesh", "readback" };
	Dictionary result;
	for (int stage = 0; stage < STAGE_COUNT; stage++) {
		Dictionary timing;
		timing["total_usec"] = int64_t(p_context->data->timings[stage].total.get());
		timing["max_usec"] = int64_t(p_context->data->timings[stage].maximum.get());
		result[names[stage]] = timing;
	}
	return result;
}

void LRTMaterialBaker::clear_source_cache(const Ref<Context> &p_context) {
	p_context->data->mesh_snapshots.clear();
}

void LRTMaterialBaker::cancel(const Ref<Request> &p_request) {
	p_request->data->cancelled.set();
}

void LRTMaterialBaker::shutdown() {
	{
		MutexLock lock(material_requests_mutex);
		for (Request *request : material_requests) {
			request->data->cancelled.set();
		}
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(+[]() {
		material_shutdown = true;
		_process_pending();
	}));
	// Shutdown only: drain late render commands until every cooperative waiter retires.
	do {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_process_pending));
		RenderingServer::get_singleton()->sync();
		if (has_pending()) {
			OS::get_singleton()->delay_usec(1000);
		}
	} while (has_pending());
}

Error LRTMaterialBaker::bake_on_worker(const Ref<Request> &p_request, Surface &r_surface, const SafeFlag *p_cancel, const Ref<RefCounted> &p_source) {
	ERR_FAIL_COND_V(WorkerThreadPool::get_singleton()->get_thread_index() < 0, ERR_UNAVAILABLE);
	ERR_FAIL_COND_V(p_request.is_null(), ERR_INVALID_PARAMETER);
	struct ActiveWorker {
		ActiveWorker() { material_workers.increment(); }
		~ActiveWorker() { material_workers.decrement(); }
	} active;
	RenderingServerDefault::redraw_request();
	Request::Data &request = *p_request->data;
	request.external_cancel.set(p_cancel);
	struct CancellationScope {
		Request::Data &request;
		~CancellationScope() { request.external_cancel.set(nullptr); }
	} cancellation{ request };
	_wait(p_request);
	if (request.error != OK) {
		r_surface = request.surface;
		return request.error;
	}
	bool opaque = false;
	for (bool pass : request.opaque) {
		opaque |= pass;
	}
	if (!opaque) {
		request.surface.transparent = true;
		r_surface = request.surface;
		return OK;
	}
	MeshSnapshot &source = **request.source_snapshot;
	const Ref<MeshSnapshot> previous = p_source;
	if (!source.ready && previous.is_valid() && previous->ready && previous->key == source.key) {
		source.data = previous->data;
		source.ready = true;
	}
	if (!source.ready) {
		{
			MutexLock lock(request.event_mutex);
			request.ready = false;
		}
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_begin_source).bind(p_request));
		_wait(p_request);
		if (request.error != OK) {
			r_surface = request.surface;
			return request.error;
		}
		source.data = source.metadata;
		Vector<uint8_t> *channels[4] = { &source.data.vertex_data, &source.data.attribute_data, &source.data.skin_data, &source.data.index_data };
		for (int channel = 0; channel < 4; channel++) {
			request.surface.source_read_bytes += source.sizes[channel];
			if (channels[channel]->resize(source.sizes[channel]) != OK) {
				return ERR_OUT_OF_MEMORY;
			}
		}
		for (const Request::Data::Download &download : request.downloads) {
			memcpy(channels[download.source_channel]->ptrw() + download.offset, download.bytes.ptr(), download.size);
		}
		if (!(source.data.format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) && (source.data.format & RSE::ARRAY_FORMAT_NORMAL) && !(source.data.format & RSE::ARRAY_FORMAT_TANGENT)) {
			source.data.vertex_data.resize(source.data.vertex_data.size() - sizeof(uint16_t) * 2);
		}
		source.ready = true;
	}
	request.source = source.data;
	request.reading_source = false;
	request.error = _prepare(p_request);
	if (request.error != OK || request.surface.transparent) {
		r_surface = request.surface;
		return request.error;
	}
	auto dispatch = [&](bool p_vertices) {
		{
			MutexLock lock(request.event_mutex);
			request.ready = false;
		}
		RenderingServer::get_singleton()->call_on_render_thread(
				p_vertices ? callable_mp_static(&LRTMaterialBaker::_begin_vertices).bind(p_request) : callable_mp_static(&LRTMaterialBaker::_begin_page).bind(p_request));
		_wait(p_request);
	};
	for (request.page.y = 0; request.page.y < request.surface.atlas_size.y && request.error == OK; request.page.y += request.size.y) {
		for (request.page.x = 0; request.page.x < request.surface.atlas_size.x && request.error == OK; request.page.x += request.size.x) {
			dispatch(false);
			if (request.error != OK) {
				break;
			}
			Surface::Page page;
			page.albedo = Image::create_from_data(request.size.x, request.size.y, false, Image::FORMAT_RGBAF, request.downloads[0].bytes);
			page.emission = Image::create_from_data(request.size.x, request.size.y, false, Image::FORMAT_RGBAF, request.downloads[1].bytes);
			if (page.albedo.is_null() || page.emission.is_null() || request.surface.pages.push_back(page) != OK) {
				request.error = ERR_OUT_OF_MEMORY;
				break;
			}
		}
	}
	if (request.error == OK) {
		dispatch(true);
		if (request.error == OK) {
			uint8_t *target = request.vertices.ptrw();
			for (const Request::Data::Download &download : request.downloads) {
				memcpy(target + download.offset, download.bytes.ptr(), download.size);
			}
			request.error = _validate(p_request);
		}
	} else {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_release_gpu).bind(p_request));
	}
	r_surface = request.surface;
	return request.error;
}

Error LRTMaterialBaker::inspect_surface(GeometryInstance3D *p_instance, int p_surface, float p_texel_size, Surface &r_surface, int p_instance_index) {
	const Ref<Request> captured = capture(p_instance, p_surface, p_texel_size, p_instance_index);
	ERR_FAIL_COND_V(captured.is_null(), ERR_INVALID_PARAMETER);
	RenderingServer *server = RenderingServer::get_singleton();
	server->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_capture_sync).bind(captured));
	server->sync();
	Request::Data &request = *captured->data;
	request.synchronous = true;
	request.error = _prepare(captured);
	if (request.error == OK && !request.surface.transparent) {
		server->call_on_render_thread(callable_mp_static(&LRTMaterialBaker::_render_sync).bind(captured));
		server->sync();
		if (request.error == OK) {
			request.error = _validate(captured);
		}
	}
	r_surface = request.surface;
	return request.error;
}

Error LRTMaterialBaker::_validate(const Ref<Request> &p_request) {
	Request::Data &request = *p_request->data;
	Surface &output = request.surface;
	const Vector<uint8_t> &vertices = request.vertices;
	ERR_FAIL_COND_V(vertices.size() < 48 || (vertices.size() - 48) % 32 != 0, ERR_UNAVAILABLE);
	if (output.positions.resize((vertices.size() - 48) / 32) != OK) {
		return ERR_OUT_OF_MEMORY;
	}
	for (int index = 0; index < output.positions.size(); index++) {
		const uint8_t *record = vertices.ptr() + 64 + index * 32;
		if (decode_float(record + 12) != 1) {
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("LRT material vertex capture missed vertex %d.", index));
		}
		output.positions.write[index] = Vector3(decode_float(record), decode_float(record + 4), decode_float(record + 8));
	}
	for (int face = 0; face < output.atlas_uv.size(); face += 3) {
		const Vector2 center = (output.atlas_uv[face] + output.atlas_uv[face + 1] + output.atlas_uv[face + 2]) / 3 * Vector2(output.atlas_size);
		const int columns = (output.atlas_size.x + request.size.x - 1) / request.size.x;
		const Surface::Page &page = output.pages[(int(center.y) / request.size.y) * columns + int(center.x) / request.size.x];
		if (page.albedo->get_pixel(int(center.x) % request.size.x, int(center.y) % request.size.y).a <= 0) {
			ERR_FAIL_V_MSG(ERR_CANT_CREATE, vformat("LRT material atlas did not cover triangle %d.", face / 3));
		}
	}
	for (const Surface::Page &page : output.pages) {
		for (int y = 0; y < request.size.y; y++) {
			for (int x = 0; x < request.size.x; x++) {
				const Color color = page.albedo->get_pixel(x, y);
				const Color light = page.emission->get_pixel(x, y);
				if (!Math::is_finite(color.r) || !Math::is_finite(color.g) || !Math::is_finite(color.b) ||
						!Math::is_finite(light.r) || !Math::is_finite(light.g) || !Math::is_finite(light.b)) {
					request.error = ERR_INVALID_DATA;
				}
				output.has_emission |= light.r != 0 || light.g != 0 || light.b != 0;
			}
		}
	}
	if (!output.has_emission) {
		for (int page = 0; page < output.pages.size(); page++) {
			output.pages.write[page].emission.unref();
		}
	}

	return request.error;
}
