/**************************************************************************/
/*  lrt_runtime.cpp                                                       */
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

#include "lrt_runtime.h"

#include "local_lrt_volume_3d.h"
#include "lrt_geometry.h"
#include "lrt_geometry_binding.h"
#include "lrt_renderer.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

using namespace LRTGeometry;

LRTRuntime *LRTRuntime::singleton = nullptr;

namespace {

bool supported_transform(const Transform3D &p_transform) {
	return p_transform.is_finite() && p_transform.basis.is_conformal() && p_transform.basis.determinant() > 0;
}

struct Batch;

struct Job {
	const Batch *batch = nullptr;
	Vector3i coordinate;
	WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	Trunk result;
	uint64_t usec = 0;
	bool joined = false;
};

struct Batch {
	StaticCache baseline;
	Vector<Instance> instances;
	Vector<Vector3i> coordinates;
	Vector<Job *> jobs;
	int submitted = 0;

	~Batch() {
		for (Job *job : jobs) {
			if (!job->joined) {
				WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
			}
			memdelete(job);
		}
	}
};

void rebuild_job(void *p_userdata) {
	Job &job = *static_cast<Job *>(p_userdata);
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	job.result = rebuild_trunk(job.batch->baseline, job.batch->instances, job.coordinate);
	job.usec = OS::get_singleton()->get_ticks_usec() - start;
}

struct CoordinateLess {
	bool operator()(const Vector3i &p_a, const Vector3i &p_b) const {
		if (p_a.z != p_b.z) {
			return p_a.z < p_b.z;
		}
		return p_a.y != p_b.y ? p_a.y < p_b.y : p_a.x < p_b.x;
	}
};

struct VolumeLess {
	bool operator()(const LocalLRTVolume3D *p_a, const LocalLRTVolume3D *p_b) const {
		if (p_a->get_priority() != p_b->get_priority()) {
			return p_a->get_priority() < p_b->get_priority();
		}
		return p_a->get_instance_id() < p_b->get_instance_id();
	}
};

struct InstanceLess {
	bool operator()(const Instance &p_a, const Instance &p_b) const {
		return p_a.id == p_b.id ? p_a.index < p_b.index : p_a.id < p_b.id;
	}
};

TypedArray<Vector3i> coordinates_to_array(const Vector<Vector3i> &p_coordinates) {
	TypedArray<Vector3i> result;
	for (const Vector3i &coordinate : p_coordinates) {
		result.push_back(coordinate);
	}
	return result;
}

Dictionary trunk_to_dictionary(const Trunk &p_trunk, uint64_t p_revision) {
	Dictionary result;
	result["coordinate"] = p_trunk.coordinate;
	result["origin"] = p_trunk.origin;
	result["size"] = p_trunk.size;
	result["ltm"] = p_trunk.ltm;
	result["local_vis"] = p_trunk.local_vis;
	result["edge_masks"] = p_trunk.edge_masks;
	result["emission"] = p_trunk.emission;
	PackedInt64Array instances;
	for (ObjectID id : p_trunk.instances) {
		instances.push_back(int64_t(uint64_t(id)));
	}
	result["instances"] = instances;
	result["revision"] = p_revision;
	return result;
}

} // namespace

struct LRTRuntime::BindingState {
	ObjectID world;
	Ref<LRTGeometryBinding> binding;
	uint64_t source_revision = 0;
	uint64_t revision = 0;
	Vector<ColorSDF> sdfs;
	Vector<bool> receivers;
	bool active = false;
	String error;
};

struct LRTRuntime::VolumeState {
	uint64_t bake_revision = 0;
	uint64_t revision = 0;
	ObjectID world;
	StaticCache baseline;
	HashMap<InstanceKey, Instance, InstanceKey> seen;
	HashSet<Vector3i> dirty;
	HashMap<Vector3i, Trunk> trunks;
	HashMap<Vector3i, uint64_t> debug_published;
	Vector<Vector3i> last_changed;
	Batch *batch = nullptr;
	bool initialized = false;
	bool ready = false;
	int last_submitted = 0;
	uint64_t cpu_usec = 0;
	AABB debug_old_bounds;
	AABB debug_new_bounds;
	uint64_t debug_bounds_frame = 0;
	String error;

	void cancel_batch() {
		if (batch) {
			memdelete(batch);
			batch = nullptr;
		}
	}

	void reset() {
		cancel_batch();
		seen.clear();
		dirty.clear();
		trunks.clear();
		debug_published.clear();
		debug_old_bounds = AABB();
		debug_new_bounds = AABB();
		debug_bounds_frame = 0;
		last_changed.clear();
		initialized = false;
		ready = false;
		error = String();
	}

	void mark_bounds(const AABB &p_bounds) {
		const AABB region = p_bounds.grow(baseline.spacing * 1.05f);
		Vector3i first;
		Vector3i last;
		for (int axis = 0; axis < 3; axis++) {
			first[axis] = MAX(0, int(Math::ceil((double(region.position[axis]) - baseline.bounds.position[axis]) / baseline.spacing)));
			last[axis] = MIN(baseline.grid[axis] - 1, int(Math::floor((double(region.get_end()[axis]) - baseline.bounds.position[axis]) / baseline.spacing)));
			if (first[axis] > last[axis]) {
				return;
			}
		}
		first /= TRUNK_SIZE;
		last /= TRUNK_SIZE;
		for (int z = first.z; z <= last.z; z++) {
			for (int y = first.y; y <= last.y; y++) {
				for (int x = first.x; x <= last.x; x++) {
					dirty.insert(Vector3i(x, y, z));
				}
			}
		}
	}

	~VolumeState() {
		cancel_batch();
	}
};

struct LRTRuntime::Notification {
	ObjectID volume;
	uint64_t revision = 0;
	Vector<Vector3i> coordinates;
	bool reset = false;
};

void LRTRuntime::add_volume(LocalLRTVolume3D *p_volume) {
	volumes.insert(p_volume);
}

void LRTRuntime::remove_volume(LocalLRTVolume3D *p_volume, bool p_keep_state) {
	volumes.erase(p_volume);
	if (p_keep_state) {
		return;
	}
	renderer->remove(p_volume->get_instance_id());
	VolumeState **state = states.getptr(p_volume);
	if (state) {
		memdelete(*state);
		states.erase(p_volume);
	}
}

void LRTRuntime::_node_added(Node *p_node) {
	if (LRTGeometryBinding::supports(p_node)) {
		bindings.insert(Object::cast_to<GeometryInstance3D>(p_node));
	}
}

void LRTRuntime::_node_removed(Node *p_node) {
	GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(p_node);
	if (!instance) {
		return;
	}
	BindingState **state = binding_states.getptr(instance);
	if (state) {
		memdelete(*state);
		binding_states.erase(instance);
	}
	bindings.erase(instance);
}

void LRTRuntime::_sync_bindings() {
	surface_biases = Dictionary();
	for (GeometryInstance3D *instance : bindings) {
		const Ref<LRTGeometryBinding> binding = LRTGeometryBinding::get_binding(instance);
		if (binding.is_valid() && binding->get_surface_normal_bias() >= 0.0f) {
			surface_biases[int64_t(uint64_t(instance->get_instance_id()))] = binding->get_surface_normal_bias();
		}
		if (binding.is_null() || LRTGeometryBinding::get_mesh(instance).is_null() || instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DYNAMIC || !instance->is_visible_in_tree()) {
			BindingState **state = binding_states.getptr(instance);
			if (state) {
				(*state)->active = false;
			}
			continue;
		}
		BindingState *&state = binding_states[instance];
		if (!state) {
			state = memnew(BindingState);
		}
		state->error = String();
		state->active = true;
		state->world = instance->get_world_3d()->get_instance_id();
		const Ref<LRTInstanceGeometryData> resource = binding->get_bake_data();
		if (resource.is_null() || !resource->has_data()) {
			state->error = "Dynamic LRT geometry has no baked Color SDF.";
			continue;
		}
		const bool multi = Object::cast_to<MultiMeshInstance3D>(instance) != nullptr;
		if ((multi && resource->get_instance_count() != LRTGeometryBinding::get_instance_count(instance, false)) || (!multi && resource->get_instance_count() != -1)) {
			state->error = "LRT geometry bake does not match the instance layout. Bake the Color SDF again.";
			continue;
		}
		if (multi && Object::cast_to<MultiMeshInstance3D>(instance)->get_multimesh()->get_transform_format() != MultiMesh::TRANSFORM_3D) {
			state->error = "LRT requires a 3D MultiMesh.";
			continue;
		}
		if (state->binding != binding || state->source_revision != binding->get_geometry_revision()) {
			state->binding = binding;
			state->source_revision = binding->get_geometry_revision();
			state->revision = hash_murmur3_one_64(uint64_t(binding->get_instance_id()), state->source_revision);
			const int count = LRTGeometryBinding::get_instance_count(instance, false);
			state->sdfs.resize(count);
			state->receivers.resize(count);
			HashMap<ObjectID, ColorSDF> loaded;
			for (int index = 0; index < count; index++) {
				const Ref<LRTInstanceGeometryData> data = multi ? resource->get_instance_data(index) : resource;
				state->receivers.write[index] = data->is_receiver_only();
				if (!data->is_receiver_only()) {
					ColorSDF *sdf = loaded.getptr(data->get_instance_id());
					if (!sdf) {
						ColorSDF snapshot;
						snapshot.load(data->get_data());
						loaded.insert(data->get_instance_id(), snapshot);
						sdf = loaded.getptr(data->get_instance_id());
					}
					state->sdfs.write[index] = *sdf;
				}
			}
		}
	}
}

void LRTRuntime::_sync_volume(LocalLRTVolume3D *p_volume, VolumeState &r_state, Vector<Notification> &r_notifications) {
	const Ref<LocalLRTVolumeData> resource = p_volume->get_bake_data();
	String error;
	if (resource.is_null() || !resource->has_data()) {
		error = "LRT volume has no baked static geometry.";
	} else if (resource->get_bounds() != p_volume->get_bounds() || !Math::is_equal_approx(resource->get_probe_spacing(), p_volume->get_probe_spacing())) {
		error = "LRT volume layout differs from its bake. Bake the volume again.";
	} else if (!supported_transform(p_volume->get_global_transform())) {
		error = "LRT volume requires a positive uniform scale without shear.";
	} else if (int(GLOBAL_GET("rendering/global_illumination/lrt/geometry_trunks_per_frame")) < 1) {
		error = "rendering/global_illumination/lrt/geometry_trunks_per_frame must be positive.";
	}
	const ObjectID world = p_volume->get_world_3d()->get_instance_id();
	Vector<Instance> current;
	if (error.is_empty()) {
		const VolumeFrame volume_frame(p_volume);
		const AABB domain = AABB(p_volume->get_bounds().position,
				Vector3(p_volume->get_grid_size() - Vector3i(1, 1, 1)) * p_volume->get_probe_spacing())
									.grow(p_volume->get_probe_spacing() * 1.05f);
		for (const KeyValue<GeometryInstance3D *, BindingState *> &entry : binding_states) {
			const BindingState &binding = *entry.value;
			if (!binding.active || binding.world != world) {
				continue;
			}
			GeometryInstance3D *instance = entry.key;
			const Transform3D node_transform = volume_frame.relative_transform(instance);
			if (!binding.error.is_empty()) {
				if (node_transform.is_finite() && !domain.intersects_inclusive(node_transform.xform(instance->get_aabb()))) {
					continue;
				}
				error = binding.error;
				break;
			}
			for (int index = 0; index < LRTGeometryBinding::get_instance_count(instance); index++) {
				if (binding.receivers[index]) {
					continue;
				}
				const Transform3D transform = node_transform * LRTGeometryBinding::get_instance_transform(instance, index);
				if (!supported_transform(transform)) {
					error = "Dynamic LRT geometry requires a positive uniform scale without shear.";
					break;
				}
				const AABB bounds = transform.xform(binding.sdfs[index].bounds);
				if (!domain.intersects_inclusive(bounds)) {
					continue;
				}
				Instance snapshot;
				snapshot.id = instance->get_instance_id();
				snapshot.index = index;
				snapshot.revision = binding.revision;
				snapshot.sdf = binding.sdfs[index];
				snapshot.to_instance = transform.affine_inverse();
				snapshot.bounds = bounds;
				current.push_back(snapshot);
			}
			if (!error.is_empty()) {
				break;
			}
		}
	}
	if (!error.is_empty()) {
		if (r_state.error != error) {
			renderer->remove(p_volume->get_instance_id());
			if (Engine::get_singleton()->is_editor_hint()) {
				p_volume->call_deferred(SNAME("update_configuration_warnings"));
			} else {
				WARN_PRINT(vformat("%s: %s", p_volume->get_path(), error));
			}
		}
		r_state.error = error;
		r_state.ready = false;
		return;
	}

	if (!r_state.initialized || !r_state.error.is_empty() || r_state.bake_revision != p_volume->get_geometry_revision() || (!Engine::get_singleton()->is_editor_hint() && r_state.world != world)) {
		if (!r_state.error.is_empty() && Engine::get_singleton()->is_editor_hint()) {
			p_volume->call_deferred(SNAME("update_configuration_warnings"));
		}
		r_state.reset();
		r_state.baseline.load(resource->get_data());
		r_state.bake_revision = p_volume->get_geometry_revision();
		r_state.world = world;
		r_state.initialized = true;
		r_state.revision++;
		renderer->submit(p_volume->get_instance_id(), r_state.revision, r_state.baseline, Vector<Trunk>(), true);
		Notification notification;
		notification.volume = p_volume->get_instance_id();
		notification.revision = r_state.revision;
		notification.reset = true;
		r_notifications.push_back(notification);
	}
	r_state.world = world;
	current.sort_custom<InstanceLess>();
	HashMap<InstanceKey, Instance, InstanceKey> seen;
	for (const Instance &instance : current) {
		const Instance *previous = r_state.seen.getptr(instance.get_key());
		if (!previous) {
			r_state.mark_bounds(instance.bounds);
			r_state.debug_old_bounds = AABB();
			r_state.debug_new_bounds = instance.bounds;
			r_state.debug_bounds_frame = frame;
		} else if (previous->revision != instance.revision || previous->to_instance != instance.to_instance) {
			r_state.mark_bounds(previous->bounds.merge(instance.bounds));
			r_state.debug_old_bounds = previous->bounds;
			r_state.debug_new_bounds = instance.bounds;
			r_state.debug_bounds_frame = frame;
		}
		seen[instance.get_key()] = instance;
	}
	for (const KeyValue<InstanceKey, Instance> &previous : r_state.seen) {
		if (!seen.has(previous.key)) {
			r_state.mark_bounds(previous.value.bounds);
			r_state.debug_old_bounds = previous.value.bounds;
			r_state.debug_new_bounds = AABB();
			r_state.debug_bounds_frame = frame;
		}
	}
	r_state.seen = seen;

	_poll_batch(p_volume, r_state, r_notifications);
	if (!r_state.batch && !r_state.dirty.is_empty() && r_state.error.is_empty()) {
		r_state.batch = memnew(Batch);
		r_state.batch->baseline = r_state.baseline;
		r_state.batch->instances = current;
		for (const Vector3i &coordinate : r_state.dirty) {
			r_state.batch->coordinates.push_back(coordinate);
		}
		r_state.batch->coordinates.sort_custom<CoordinateLess>();
		r_state.dirty.clear();
	}
	if (!r_state.batch && r_state.dirty.is_empty()) {
		r_state.ready = true;
	}
}

void LRTRuntime::_poll_batch(LocalLRTVolume3D *p_volume, VolumeState &r_state, Vector<Notification> &r_notifications) {
	if (!r_state.batch) {
		return;
	}
	Batch &batch = *r_state.batch;
	bool complete = batch.submitted == batch.coordinates.size();
	for (Job *job : batch.jobs) {
		if (!job->joined && WorkerThreadPool::get_singleton()->is_task_completed(job->task)) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
			job->joined = true;
		}
		complete &= job->joined;
	}
	if (!complete) {
		return;
	}
	for (Job *job : batch.jobs) {
		if (job->result.size.x == 0) {
			renderer->remove(p_volume->get_instance_id());
			r_state.error = "LRT geometry worker could not allocate a trunk.";
			r_state.ready = false;
			r_state.cancel_batch();
			return;
		}
	}
	r_state.last_changed = batch.coordinates;
	for (Job *job : batch.jobs) {
		r_state.cpu_usec += job->usec;
		r_state.debug_published[job->coordinate] = frame;
		if (job->result.instances.is_empty()) {
			r_state.trunks.erase(job->coordinate);
		} else {
			r_state.trunks[job->coordinate] = job->result;
		}
	}
	r_state.revision++;
	r_state.ready = true;
	r_state.debug_bounds_frame = frame;
	Vector<Trunk> updates;
	for (const Job *job : batch.jobs) {
		updates.push_back(job->result);
	}
	renderer->submit(p_volume->get_instance_id(), r_state.revision, r_state.baseline, updates, false);
	Notification notification;
	notification.volume = p_volume->get_instance_id();
	notification.revision = r_state.revision;
	notification.coordinates = batch.coordinates;
	r_notifications.push_back(notification);
	r_state.cancel_batch();
}

void LRTRuntime::_request_redraw() {
	if (redraw_pending) {
		RenderingServerDefault::redraw_request();
	}
}

void LRTRuntime::_update() {
	redraw_pending = false;
	if (!tracking_nodes && SceneTree::get_singleton()) {
		SceneTree *tree = SceneTree::get_singleton();
		tree->connect("node_added", callable_mp(this, &LRTRuntime::_node_added));
		tree->connect("node_removed", callable_mp(this, &LRTRuntime::_node_removed));
		Vector<Node *> pending;
		pending.push_back(tree->get_root());
		while (!pending.is_empty()) {
			Node *node = pending[pending.size() - 1];
			pending.resize(pending.size() - 1);
			_node_added(node);
			for (int child = 0; child < node->get_child_count(); child++) {
				pending.push_back(node->get_child(child));
			}
		}
		tracking_nodes = true;
	}
	frame++;
	frame_submitted = 0;
	if (!bool(GLOBAL_GET("rendering/global_illumination/lrt/enabled"))) {
		for (const KeyValue<LocalLRTVolume3D *, VolumeState *> &entry : states) {
			renderer->remove(entry.key->get_instance_id());
			memdelete(entry.value);
		}
		states.clear();
		for (const KeyValue<GeometryInstance3D *, BindingState *> &entry : binding_states) {
			memdelete(entry.value);
		}
		binding_states.clear();
		return;
	}
	_sync_bindings();
	Vector<LocalLRTVolume3D *> active;
	Vector<Notification> notifications;
	for (LocalLRTVolume3D *volume : volumes) {
		if (!volume->is_enabled() || !volume->is_visible_in_tree()) {
			VolumeState **state = states.getptr(volume);
			if (state) {
				renderer->remove(volume->get_instance_id());
				memdelete(*state);
				states.erase(volume);
			}
			continue;
		}
		VolumeState *&state = states[volume];
		if (!state) {
			state = memnew(VolumeState);
		}
		state->last_submitted = 0;
		_sync_volume(volume, *state, notifications);
		if (state->error.is_empty()) {
			active.push_back(volume);
		}
	}
	// frame_pre_draw requests are cleared by RenderingServerDefault::draw().
	// Request the next update after drawing, including while geometry is pending.
	redraw_pending = !active.is_empty();
	active.sort_custom<VolumeLess>();
	const int budget = int(GLOBAL_GET("rendering/global_illumination/lrt/geometry_trunks_per_frame"));
	int idle = 0;
	while (!active.is_empty() && frame_submitted < budget && idle < active.size()) {
		next_volume %= active.size();
		VolumeState &state = *states[active[next_volume]];
		next_volume = (next_volume + 1) % active.size();
		if (!state.batch || state.batch->submitted == state.batch->coordinates.size()) {
			idle++;
			continue;
		}
		idle = 0;
		Batch &batch = *state.batch;
		Job *job = memnew(Job);
		job->batch = &batch;
		job->coordinate = batch.coordinates[batch.submitted++];
		job->task = WorkerThreadPool::get_singleton()->add_native_task(rebuild_job, job, false, "LRT geometry trunk");
		batch.jobs.push_back(job);
		state.last_submitted++;
		frame_submitted++;
	}
	Array propagating;
	for (LocalLRTVolume3D *volume : active) {
		if (!states[volume]->ready) {
			continue;
		}
		Dictionary description;
		description["id"] = uint64_t(volume->get_instance_id());
		description["scenario"] = volume->get_world_3d()->get_scenario();
		description["transform"] = volume->get_global_transform();
		description["size"] = volume->get_size();
		description["spacing"] = volume->get_probe_spacing();
		description["intensity"] = volume->get_intensity();
		description["blend"] = volume->get_blend_width_cells();
		description["priority"] = volume->get_priority();
		VolumeState &cache = *states[volume];
		description["static_ltm"] = cache.baseline.ltm;
		description["static_vis"] = cache.baseline.local_vis;
		description["static_edges"] = cache.baseline.edge_masks;
		Vector3i trunk_grid((cache.baseline.grid.x + 7) / 8, (cache.baseline.grid.y + 7) / 8, (cache.baseline.grid.z + 7) / 8);
		PackedInt32Array trunks;
		trunks.resize(trunk_grid.x * trunk_grid.y * trunk_grid.z);
		trunks.fill(0);
		auto mark = [&](const Vector3i &p_coordinate, int p_state) {
			trunks.set((p_coordinate.z * trunk_grid.y + p_coordinate.y) * trunk_grid.x + p_coordinate.x, p_state);
		};
		Dictionary gpu_status = renderer->get_status(volume->get_instance_id());
		bool published = uint64_t(gpu_status.get("revision", uint64_t(0))) >= cache.revision;
		if (!cache.batch && cache.dirty.is_empty() && published && frame - cache.debug_bounds_frame >= 60) {
			cache.debug_old_bounds = AABB();
			cache.debug_new_bounds = AABB();
		}
		for (const KeyValue<Vector3i, uint64_t> &item : cache.debug_published) {
			if (frame - item.value < 60) {
				mark(item.key, 3);
			}
		}
		if (!published) {
			for (const Vector3i &coordinate : cache.last_changed) {
				mark(coordinate, 2);
			}
		}
		for (const Vector3i &coordinate : cache.dirty) {
			mark(coordinate, 1);
		}
		if (cache.batch) {
			for (int i = 0; i < cache.batch->coordinates.size(); i++) {
				mark(cache.batch->coordinates[i], i < cache.batch->submitted ? 2 : 1);
			}
		}
		description["debug_trunks"] = trunks;
		description["debug_old_bounds"] = cache.debug_old_bounds;
		description["debug_new_bounds"] = cache.debug_new_bounds;
		propagating.push_back(description);
	}
	renderer->prepare(propagating, int(GLOBAL_GET("rendering/global_illumination/lrt/propagation_iterations")), double(GLOBAL_GET("rendering/global_illumination/lrt/propagation_relaxation")), frame, surface_biases);
	// Emit after processing all states. Callbacks may remove volumes or bindings.
	for (const Notification &notification : notifications) {
		LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(ObjectDB::get_instance(notification.volume));
		if (volume) {
			volume->emit_signal("geometry_updated", notification.revision, coordinates_to_array(notification.coordinates), notification.reset);
		}
	}
}

Dictionary LRTRuntime::inspect_probe(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) const {
	Dictionary result = LRTGeometryBaker::inspect_probe(p_volume, p_coordinate);
	if (result.has("error") || !states.has(p_volume)) {
		return result;
	}
	Array samples = result["samples"];
	Vector<LocalLRTVolume3D *> sources;
	for (LocalLRTVolume3D *candidate : volumes) {
		if (candidate->is_enabled() && candidate->get_world_3d() == p_volume->get_world_3d() && states.has(candidate) && states[candidate]->ready) {
			sources.push_back(candidate);
		}
	}
	sources.sort_custom<VolumeLess>();
	const VolumeState &cache = *states[p_volume];
	Vector3 point = result["point"];
	for (int d = 0; d < 26; d++) {
		Dictionary sample = samples[d];
		Vector3 q = sample["position"];
		Vector3 from[4], to[4];
		edge_segments(p_volume->get_bounds(), p_volume->get_probe_spacing(), p_coordinate, d, from, to);
		int mask = sample["edge_mask"];
		for (const KeyValue<InstanceKey, Instance> &entry : cache.seen) {
			Color color, emission;
			if (entry.value.sdf.contains(entry.value.to_instance.xform(q), &color, &emission)) {
				sample["hit"] = true;
				sample["albedo"] = color;
				sample["emission"] = emission;
			}
			for (int line = 0; line < 4; line++) {
				if (entry.value.sdf.intersects_segment(entry.value.to_instance.xform(from[line]), entry.value.to_instance.xform(to[line]))) {
					mask &= ~(1 << line);
				}
			}
		}
		sample["edge_mask"] = mask;
		Vector3i neighbor = p_coordinate + direction(d).offset;
		Vector3i grid = p_volume->get_grid_size();
		if (neighbor.x < 0 || neighbor.y < 0 || neighbor.z < 0 || neighbor.x >= grid.x || neighbor.y >= grid.y || neighbor.z >= grid.z) {
			Vector3 world = p_volume->get_global_transform().xform(q);
			for (int index = sources.size() - 1; index >= 0; index--) {
				LocalLRTVolume3D *source = sources[index];
				if (source->get_priority() >= p_volume->get_priority()) {
					continue;
				}
				if (!source->get_bounds().has_point(source->get_global_transform().affine_inverse().xform(world))) {
					continue;
				}
				sample["boundary_volume"] = source->get_path();
				sample["boundary_index"] = index;
				sample["boundary_direction_in_source"] = source->get_global_basis().orthonormalized().inverse().xform(p_volume->get_global_basis().orthonormalized().xform(Vector3(direction(d).offset).normalized()));
				break;
			}
		}
	}
	result["revision"] = cache.revision;
	return result;
}

void LRTRuntime::set_debug_selection(ObjectID p_selection) {
	debug_selection = p_selection;
	renderer->set_debug_selection(p_selection);
}

String LRTRuntime::get_debug_view_description(int p_mode) const {
	static const char *descriptions[] = {
		"LRT Indirect Lighting\nScreen Gather + guided upsampling | material albedo excluded",
		"LRT Probe Sampling\nFull-resolution direct probe sampling | same exposure as Gather",
		"LRT Probes\nPropagated bounce light L | magenta: Inside / invalid | dark rim: unlit",
		"LRT Local Visibility\nDirectional local transmittance | black: blocked | white: open",
		"LRT Global Visibility\nPropagated environment visibility A | black: blocked | white: open",
		"LRT Edge Visibility\nOpen rays / 4: red 0, orange 1, yellow 2, cyan 3 | 4 hidden\nDim dashes: occluded | solid-interior connections hidden",
		"LRT Static LTM\nStatic baked transport under unit uniform white light\nOnly nonzero valid transport | dynamic geometry excluded",
		"LRT Color SDF\nBaked material color + shape shading | selected geometry instance",
		"LRT Trunk Updates\nGray: idle | yellow: queued | blue: CPU rebuild | green: GPU published\nPurple bounds: old dashed / new solid (brief history) | scene and gizmos preserved"
	};
	ERR_FAIL_INDEX_V(p_mode, 9, String());
	String text = descriptions[p_mode];
	if (!bool(GLOBAL_GET("rendering/global_illumination/lrt/enabled"))) {
		return text + "\nLRT is disabled in Project Settings.";
	}
	if (p_mode == 7) {
		GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(debug_selection));
		if (!instance) {
			return text + "\nSelect a MeshInstance3D or MultiMeshInstance3D with baked Color SDF.";
		}
		Ref<LRTGeometryBinding> binding = LRTGeometryBinding::get_binding(instance);
		if (binding.is_null() || binding->get_bake_data().is_null() || !binding->get_bake_data()->has_data()) {
			return text + "\nThe selected instance has no baked Color SDF.";
		}
		if (binding->get_bake_data()->is_receiver_only()) {
			return text + "\nThe selected geometry is receive-only; no Color SDF surface.";
		}
		int count = binding->get_bake_data()->get_instance_count();
		if (count >= 0 && count != LRTGeometryBinding::get_instance_count(instance, false)) {
			return text + "\nInstance count differs from the bake; bake Color SDF again.";
		}
		return text + "\n" + instance->get_name();
	}
	LocalLRTVolume3D *selected = Object::cast_to<LocalLRTVolume3D>(ObjectDB::get_instance(debug_selection));
	if (selected) {
		Dictionary status = get_status(selected);
		text += "\n" + selected->get_name();
		if (!bool(status.get("ready", false))) {
			text += " | No ready probe cache: " + String(status.get("state", "pending"));
		}
		return text;
	}
	for (LocalLRTVolume3D *volume : volumes) {
		if (bool(get_status(volume).get("ready", false))) {
			return text + "\nAll active volumes | select a Volume to isolate it.";
		}
	}
	return text + "\nNo ready LRT Volume. Check volume enablement and baked data.";
}

String LRTRuntime::get_debug_summary() const {
	if (!bool(GLOBAL_GET("rendering/global_illumination/lrt/enabled")) || volumes.is_empty()) {
		return String();
	}
	int64_t probes = 0, pending = 0, memory = 0, shared = 0, gather = 0;
	double cpu = 0.0, gpu = 0.0, gather_gpu = 0.0, worker = 0.0;
	HashSet<ObjectID> worlds;
	for (LocalLRTVolume3D *volume : volumes) {
		if (!volume->is_enabled()) {
			continue;
		}
		Dictionary status = get_status(volume);
		Dictionary graphics = status.get("gpu", Dictionary());
		Vector3i grid = volume->get_grid_size();
		probes += int64_t(grid.x) * grid.y * grid.z;
		pending += int(status.get("pending_trunks", 0)) + int(status.get("computing_trunks", 0));
		memory += int64_t(graphics.get("allocated_bytes", 0)) + int64_t(graphics.get("debug_bytes", 0));
		worker += double(status.get("cpu_task_usec", 0.0));
		ObjectID world = volume->get_world_3d()->get_instance_id();
		if (!worlds.has(world)) {
			worlds.insert(world);
			shared += int64_t(graphics.get("sampling_bytes", 0)) + int64_t(graphics.get("sky_cache_bytes", 0));
			gather += int64_t(graphics.get("gather_bytes", 0));
			gather_gpu += double(graphics.get("gather_gpu_usec", 0.0));
			cpu += double(graphics.get("gather_cpu_usec", 0.0));
		}
		cpu += double(graphics.get("cpu_submit_usec", 0.0));
		gpu += double(graphics.get("gpu_usec", 0.0));
	}
	return vformat("LRT: %d probes, %d queued trunks\nPropagation / world cache / latest-view gather: %.2f / %.2f / %.2f MiB\nCPU submit: %.1f us | GPU propagation / gather: %.1f / %.1f us\nCPU workers (cumulative): %.1f ms", probes, pending, double(memory) / (1024 * 1024), double(shared) / (1024 * 1024), double(gather) / (1024 * 1024), cpu, gpu, gather_gpu, worker / 1000.0);
}

Dictionary LRTRuntime::get_status(LocalLRTVolume3D *p_volume) const {
	Dictionary result;
	result["frame"] = frame;
	result["frame_submitted_trunks"] = frame_submitted;
	if (!p_volume->is_inside_tree() || !bool(GLOBAL_GET("rendering/global_illumination/lrt/enabled")) || !p_volume->is_enabled() || !p_volume->is_visible_in_tree()) {
		result["state"] = "disabled";
		result["ready"] = false;
		return result;
	}
	VolumeState *const *state = states.getptr(p_volume);
	if (!state) {
		result["state"] = "pending";
		result["ready"] = false;
		return result;
	}
	const VolumeState &volume = **state;
	int pending = volume.dirty.size();
	int computing = 0;
	if (volume.batch) {
		pending += volume.batch->coordinates.size() - volume.batch->submitted;
		for (const Job *job : volume.batch->jobs) {
			computing += !job->joined;
		}
	}
	result["state"] = !volume.error.is_empty() ? "blocked" : (volume.batch || pending > 0 ? "updating" : "ready");
	result["ready"] = volume.ready && volume.error.is_empty();
	result["error"] = volume.error;
	result["revision"] = volume.revision;
	result["pending_trunks"] = pending;
	result["computing_trunks"] = computing;
	result["cached_trunks"] = volume.trunks.size();
	result["last_submitted_trunks"] = volume.last_submitted;
	result["last_changed_trunks"] = coordinates_to_array(volume.last_changed);
	result["cpu_task_usec"] = volume.cpu_usec;
	result["gpu"] = renderer->get_status(p_volume->get_instance_id());
	return result;
}

Dictionary LRTRuntime::get_trunk(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) const {
	VolumeState *const *state = states.getptr(p_volume);
	ERR_FAIL_COND_V_MSG(!state || !(*state)->initialized || !(*state)->error.is_empty(), Dictionary(), "LRT volume runtime geometry is not available.");
	const Vector3i grid = (*state)->baseline.grid;
	ERR_FAIL_COND_V(p_coordinate.x < 0 || p_coordinate.y < 0 || p_coordinate.z < 0 ||
					p_coordinate.x >= (grid.x + 7) / 8 || p_coordinate.y >= (grid.y + 7) / 8 || p_coordinate.z >= (grid.z + 7) / 8,
			Dictionary());
	const Trunk *trunk = (*state)->trunks.getptr(p_coordinate);
	if (trunk) {
		return trunk_to_dictionary(*trunk, (*state)->revision);
	}
	// Untouched regions and removed instances use the immutable static bottom plate.
	return trunk_to_dictionary(rebuild_trunk((*state)->baseline, Vector<Instance>(), p_coordinate), (*state)->revision);
}

Dictionary LRTRuntime::get_gpu_trunk(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) const {
	ERR_FAIL_COND_V(!p_volume->is_inside_tree() || !p_volume->is_enabled() || !bool(GLOBAL_GET("rendering/global_illumination/lrt/enabled")), Dictionary());
	return renderer->readback(p_volume->get_instance_id(), p_coordinate);
}

LRTRuntime::LRTRuntime() {
	singleton = this;
	renderer = memnew(LRTRenderer);
	RenderingServer::get_singleton()->connect("frame_pre_draw", callable_mp(this, &LRTRuntime::_update));
	RenderingServer::get_singleton()->connect("frame_post_draw", callable_mp(this, &LRTRuntime::_request_redraw));
}

LRTRuntime::~LRTRuntime() {
	if (tracking_nodes && SceneTree::get_singleton()) {
		SceneTree::get_singleton()->disconnect("node_added", callable_mp(this, &LRTRuntime::_node_added));
		SceneTree::get_singleton()->disconnect("node_removed", callable_mp(this, &LRTRuntime::_node_removed));
	}
	RenderingServer::get_singleton()->disconnect("frame_pre_draw", callable_mp(this, &LRTRuntime::_update));
	RenderingServer::get_singleton()->disconnect("frame_post_draw", callable_mp(this, &LRTRuntime::_request_redraw));
	for (const KeyValue<LocalLRTVolume3D *, VolumeState *> &entry : states) {
		memdelete(entry.value);
	}
	for (const KeyValue<GeometryInstance3D *, BindingState *> &entry : binding_states) {
		memdelete(entry.value);
	}
	memdelete(renderer);
	singleton = nullptr;
}
