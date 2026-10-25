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
#include "lrt_geometry_binding_3d.h"
#include "lrt_renderer.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

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
		return p_a->get_instance_id() < p_b->get_instance_id();
	}
};

struct InstanceLess {
	bool operator()(const Instance &p_a, const Instance &p_b) const {
		return p_a.id < p_b.id;
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
	ObjectID id;
	ObjectID world;
	uint64_t revision = 0;
	Transform3D transform;
	ColorSDF sdf;
	bool active = false;
	String error;
};

struct LRTRuntime::VolumeState {
	uint64_t bake_revision = 0;
	uint64_t revision = 0;
	ObjectID world;
	StaticCache baseline;
	HashMap<ObjectID, Instance> seen;
	HashSet<Vector3i> dirty;
	HashMap<Vector3i, Trunk> trunks;
	Vector<Vector3i> last_changed;
	Batch *batch = nullptr;
	bool initialized = false;
	bool ready = false;
	int last_submitted = 0;
	uint64_t cpu_usec = 0;
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

void LRTRuntime::remove_volume(LocalLRTVolume3D *p_volume) {
	renderer->remove(p_volume->get_instance_id());
	VolumeState **state = states.getptr(p_volume);
	if (state) {
		memdelete(*state);
		states.erase(p_volume);
	}
	volumes.erase(p_volume);
}

void LRTRuntime::add_binding(LRTGeometryBinding3D *p_binding) {
	bindings.insert(p_binding);
}

void LRTRuntime::remove_binding(LRTGeometryBinding3D *p_binding) {
	BindingState **state = binding_states.getptr(p_binding);
	if (state) {
		memdelete(*state);
		binding_states.erase(p_binding);
	}
	bindings.erase(p_binding);
}

void LRTRuntime::_sync_bindings() {
	for (LRTGeometryBinding3D *binding : bindings) {
		BindingState *&state = binding_states[binding];
		if (!state) {
			state = memnew(BindingState);
		}
		state->active = false;
		state->error = String();
		MeshInstance3D *instance = binding->get_geometry_instance();
		if (!instance || instance->get_mesh().is_null() || instance->get_gi_mode() != GeometryInstance3D::GI_MODE_DYNAMIC || !instance->is_visible_in_tree()) {
			continue;
		}
		state->active = true;
		state->id = instance->get_instance_id();
		state->world = instance->get_world_3d()->get_instance_id();
		state->transform = instance->get_global_transform();
		if (!supported_transform(state->transform)) {
			state->error = "Dynamic LRT geometry requires a positive uniform scale without shear.";
			continue;
		}
		const Ref<LRTInstanceGeometryData> resource = binding->get_bake_data();
		if (resource.is_null() || !resource->has_data()) {
			state->error = "Dynamic LRT geometry has no baked Color SDF.";
			continue;
		}
		if (state->revision != binding->get_geometry_revision()) {
			state->sdf.load(resource->get_data());
			state->revision = binding->get_geometry_revision();
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
	} else if (int(GLOBAL_GET("rendering/lrt/geometry_trunks_per_frame")) < 1) {
		error = "rendering/lrt/geometry_trunks_per_frame must be positive.";
	}
	const ObjectID world = p_volume->get_world_3d()->get_instance_id();
	Vector<Instance> current;
	if (error.is_empty()) {
		const VolumeFrame volume_frame(p_volume);
		const AABB domain = AABB(p_volume->get_bounds().position,
				Vector3(p_volume->get_grid_size() - Vector3i(1, 1, 1)) * p_volume->get_probe_spacing())
									.grow(p_volume->get_probe_spacing() * 1.05f);
		HashSet<ObjectID> instances;
		for (const KeyValue<LRTGeometryBinding3D *, BindingState *> &entry : binding_states) {
			const BindingState &binding = *entry.value;
			if (!binding.active || binding.world != world) {
				continue;
			}
			MeshInstance3D *instance = entry.key->get_geometry_instance();
			const Transform3D transform = volume_frame.relative_transform(instance);
			if (!binding.error.is_empty()) {
				if (transform.is_finite() && !domain.intersects_inclusive(transform.xform(instance->get_mesh()->get_aabb()))) {
					continue;
				}
				error = binding.error;
				break;
			}
			const AABB bounds = transform.xform(binding.sdf.bounds);
			if (!domain.intersects_inclusive(bounds)) {
				continue;
			}
			if (instances.has(binding.id)) {
				error = "A dynamic mesh instance must have exactly one LRT geometry binding.";
				break;
			}
			instances.insert(binding.id);
			Instance snapshot;
			snapshot.id = binding.id;
			snapshot.revision = binding.revision;
			snapshot.sdf = binding.sdf;
			snapshot.to_instance = transform.affine_inverse();
			snapshot.bounds = bounds;
			current.push_back(snapshot);
		}
	}
	if (!error.is_empty()) {
		if (r_state.error != error) {
			renderer->remove(p_volume->get_instance_id());
			ERR_PRINT(vformat("%s: %s", p_volume->get_path(), error));
		}
		r_state.error = error;
		r_state.ready = false;
		return;
	}

	if (!r_state.initialized || !r_state.error.is_empty() || r_state.bake_revision != p_volume->get_geometry_revision() || r_state.world != world) {
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
	current.sort_custom<InstanceLess>();
	HashMap<ObjectID, Instance> seen;
	for (const Instance &instance : current) {
		const Instance *previous = r_state.seen.getptr(instance.id);
		if (!previous) {
			r_state.mark_bounds(instance.bounds);
		} else if (previous->revision != instance.revision || previous->to_instance != instance.to_instance) {
			r_state.mark_bounds(previous->bounds.merge(instance.bounds));
		}
		seen[instance.id] = instance;
	}
	for (const KeyValue<ObjectID, Instance> &previous : r_state.seen) {
		if (!seen.has(previous.key)) {
			r_state.mark_bounds(previous.value.bounds);
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
		if (job->result.instances.is_empty()) {
			r_state.trunks.erase(job->coordinate);
		} else {
			r_state.trunks[job->coordinate] = job->result;
		}
	}
	r_state.revision++;
	r_state.ready = true;
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

void LRTRuntime::_update() {
	frame++;
	frame_submitted = 0;
	if (!bool(GLOBAL_GET("rendering/lrt/enabled"))) {
		for (const KeyValue<LocalLRTVolume3D *, VolumeState *> &entry : states) {
			renderer->remove(entry.key->get_instance_id());
			memdelete(entry.value);
		}
		states.clear();
		for (const KeyValue<LRTGeometryBinding3D *, BindingState *> &entry : binding_states) {
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
	active.sort_custom<VolumeLess>();
	const int budget = int(GLOBAL_GET("rendering/lrt/geometry_trunks_per_frame"));
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
		propagating.push_back(description);
	}
	renderer->prepare(propagating, int(GLOBAL_GET("rendering/lrt/propagation_iterations")), double(GLOBAL_GET("rendering/lrt/propagation_relaxation")), frame);
	// Emit after processing all states. Callbacks may remove volumes or bindings.
	for (const Notification &notification : notifications) {
		LocalLRTVolume3D *volume = Object::cast_to<LocalLRTVolume3D>(ObjectDB::get_instance(notification.volume));
		if (volume) {
			volume->emit_signal("geometry_updated", notification.revision, coordinates_to_array(notification.coordinates), notification.reset);
		}
	}
}

Dictionary LRTRuntime::get_status(LocalLRTVolume3D *p_volume) const {
	Dictionary result;
	result["frame"] = frame;
	result["frame_submitted_trunks"] = frame_submitted;
	if (!p_volume->is_inside_tree() || !bool(GLOBAL_GET("rendering/lrt/enabled")) || !p_volume->is_enabled() || !p_volume->is_visible_in_tree()) {
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
	ERR_FAIL_COND_V(!p_volume->is_inside_tree() || !p_volume->is_enabled() || !bool(GLOBAL_GET("rendering/lrt/enabled")), Dictionary());
	return renderer->readback(p_volume->get_instance_id(), p_coordinate);
}

LRTRuntime::LRTRuntime() {
	singleton = this;
	renderer = memnew(LRTRenderer);
	RenderingServer::get_singleton()->connect("frame_pre_draw", callable_mp(this, &LRTRuntime::_update));
}

LRTRuntime::~LRTRuntime() {
	RenderingServer::get_singleton()->disconnect("frame_pre_draw", callable_mp(this, &LRTRuntime::_update));
	for (const KeyValue<LocalLRTVolume3D *, VolumeState *> &entry : states) {
		memdelete(entry.value);
	}
	for (const KeyValue<LRTGeometryBinding3D *, BindingState *> &entry : binding_states) {
		memdelete(entry.value);
	}
	memdelete(renderer);
	singleton = nullptr;
}
