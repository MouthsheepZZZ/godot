/**************************************************************************/
/*  local_gi_volume_3d.cpp                                                */
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

#include "local_gi_volume_3d.h"

#include "core/config/engine.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "scene/3d/local_gi/local_gi_probe_classification.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/3d/local_gi/local_gi_temporal.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"

namespace {

void _store_vec3(float *p_destination, const Vector3 &p_value) {
	p_destination[0] = p_value.x;
	p_destination[1] = p_value.y;
	p_destination[2] = p_value.z;
}

void _pack_runtime_bvh(const LocalGIBVH &p_bvh, Vector<RendererRD::LocalGIRuntime::Node> &r_nodes, Vector<RendererRD::LocalGIRuntime::Triangle> &r_triangles) {
	const Vector<LocalGIBVHNode> &source_nodes = p_bvh.get_nodes();
	r_nodes.resize(source_nodes.size());
	for (int i = 0; i < source_nodes.size(); i++) {
		RendererRD::LocalGIRuntime::Node node;
		_store_vec3(node.bounds_min, source_nodes[i].bounds_min);
		_store_vec3(node.bounds_max, source_nodes[i].bounds_max);
		node.left = source_nodes[i].left;
		node.right = source_nodes[i].right;
		node.first_triangle = source_nodes[i].first_triangle;
		node.triangle_count = source_nodes[i].triangle_count;
		r_nodes.write[i] = node;
	}

	const Vector<LocalGITriangle> &source_triangles = p_bvh.get_triangles();
	r_triangles.resize(source_triangles.size());
	for (int i = 0; i < source_triangles.size(); i++) {
		RendererRD::LocalGIRuntime::Triangle triangle;
		_store_vec3(triangle.v0, source_triangles[i].v0);
		_store_vec3(triangle.v1, source_triangles[i].v1);
		_store_vec3(triangle.v2, source_triangles[i].v2);
		_store_vec3(triangle.normal, source_triangles[i].normal);
		triangle.albedo[0] = source_triangles[i].albedo.r;
		triangle.albedo[1] = source_triangles[i].albedo.g;
		triangle.albedo[2] = source_triangles[i].albedo.b;
		triangle.albedo[3] = 1.0f;
		r_triangles.write[i] = triangle;
	}
}

void _pack_runtime_lights(const Vector<LocalGIDirectLight> &p_lights, Vector<RendererRD::LocalGIRuntime::Light> &r_lights) {
	r_lights.resize(p_lights.size());
	for (int i = 0; i < p_lights.size(); i++) {
		RendererRD::LocalGIRuntime::Light light;
		_store_vec3(light.position_type, p_lights[i].position);
		light.position_type[3] = p_lights[i].type;
		_store_vec3(light.direction_range, p_lights[i].direction);
		light.direction_range[3] = p_lights[i].range;
		light.intensity_attenuation[0] = p_lights[i].intensity.r;
		light.intensity_attenuation[1] = p_lights[i].intensity.g;
		light.intensity_attenuation[2] = p_lights[i].intensity.b;
		light.intensity_attenuation[3] = p_lights[i].attenuation;
		light.spot[0] = p_lights[i].spot_angle_cos;
		light.spot[1] = p_lights[i].spot_attenuation;
		r_lights.write[i] = light;
	}
}

Color _opaque_rgb(const Color &p_color) {
	return Color(p_color.r, p_color.g, p_color.b, 1.0f);
}

Color _debug_tonemap_color(const Color &p_color) {
	const float r = MAX(p_color.r, 0.0f);
	const float g = MAX(p_color.g, 0.0f);
	const float b = MAX(p_color.b, 0.0f);
	return Color(r / (1.0f + r), g / (1.0f + g), b / (1.0f + b), 1.0f);
}

Color _radiance_along_direction(const Vector3 &p_dir, const Color &p_fallback, const Vector<Vector3> *p_directions, const Color *p_radiance, int p_ray_count) {
	if (p_directions == nullptr || p_radiance == nullptr || p_ray_count <= 0) {
		return _opaque_rgb(p_fallback);
	}
	int best = 0;
	float best_dot = (*p_directions)[0].dot(p_dir);
	for (int i = 1; i < p_ray_count; i++) {
		const float d = (*p_directions)[i].dot(p_dir);
		if (d > best_dot) {
			best_dot = d;
			best = i;
		}
	}
	return _debug_tonemap_color(p_radiance[best]);
}

void _add_debug_sphere(ImmediateMesh *p_mesh, const Vector3 &p_center, float p_radius, const Color &p_color, const Vector<Vector3> *p_directions = nullptr, const Color *p_radiance = nullptr, int p_ray_count = 0) {
	const int stacks = 8;
	const int slices = 12;
	for (int y = 0; y < stacks; y++) {
		const float v0 = Math::PI * ((float)y / (float)stacks);
		const float v1 = Math::PI * ((float)(y + 1) / (float)stacks);
		const float y0 = Math::cos(v0);
		const float y1 = Math::cos(v1);
		const float r0 = Math::sin(v0);
		const float r1 = Math::sin(v1);
		for (int x = 0; x < slices; x++) {
			const float u0 = (float)Math::TAU * ((float)x / (float)slices);
			const float u1 = (float)Math::TAU * ((float)(x + 1) / (float)slices);
			const Vector3 na(Math::cos(u0) * r0, y0, Math::sin(u0) * r0);
			const Vector3 nb(Math::cos(u1) * r0, y0, Math::sin(u1) * r0);
			const Vector3 nc(Math::cos(u0) * r1, y1, Math::sin(u0) * r1);
			const Vector3 nd(Math::cos(u1) * r1, y1, Math::sin(u1) * r1);
			const Color ca = _radiance_along_direction(na, p_color, p_directions, p_radiance, p_ray_count);
			const Color cb = _radiance_along_direction(nb, p_color, p_directions, p_radiance, p_ray_count);
			const Color cc = _radiance_along_direction(nc, p_color, p_directions, p_radiance, p_ray_count);
			const Color cd = _radiance_along_direction(nd, p_color, p_directions, p_radiance, p_ray_count);
			p_mesh->surface_set_color(ca);
			p_mesh->surface_add_vertex(p_center + na * p_radius);
			p_mesh->surface_set_color(cc);
			p_mesh->surface_add_vertex(p_center + nc * p_radius);
			p_mesh->surface_set_color(cd);
			p_mesh->surface_add_vertex(p_center + nd * p_radius);
			p_mesh->surface_set_color(ca);
			p_mesh->surface_add_vertex(p_center + na * p_radius);
			p_mesh->surface_set_color(cd);
			p_mesh->surface_add_vertex(p_center + nd * p_radius);
			p_mesh->surface_set_color(cb);
			p_mesh->surface_add_vertex(p_center + nb * p_radius);
		}
	}
}

} // namespace

void LocalGIVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.01);
	_mark_probes_dirty();
	update_gizmos();
	_update_debug_mesh();
}

Vector3 LocalGIVolume3D::get_size() const {
	return size;
}

void LocalGIVolume3D::set_probe_spacing(float p_spacing) {
	probe_spacing = MAX(p_spacing, 0.05f);
	_mark_probes_dirty();
	update_gizmos();
	_update_debug_mesh();
}

float LocalGIVolume3D::get_probe_spacing() const {
	return probe_spacing;
}

void LocalGIVolume3D::set_rays_per_probe(int p_rays) {
	rays_per_probe = MAX(p_rays, 1);
	_mark_probes_dirty();
	update_gizmos();
	_update_debug_mesh();
}

int LocalGIVolume3D::get_rays_per_probe() const {
	return rays_per_probe;
}

void LocalGIVolume3D::set_update_fraction(float p_fraction) {
	update_fraction = CLAMP(p_fraction, 0.0f, 1.0f);
}

float LocalGIVolume3D::get_update_fraction() const {
	return update_fraction;
}

void LocalGIVolume3D::set_temporal_hysteresis(float p_hysteresis) {
	temporal_hysteresis = CLAMP(p_hysteresis, 0.0f, 1.0f);
}

float LocalGIVolume3D::get_temporal_hysteresis() const {
	return temporal_hysteresis;
}

void LocalGIVolume3D::set_multi_bounce_enabled(bool p_enabled) {
	multi_bounce_enabled = p_enabled;
}

bool LocalGIVolume3D::is_multi_bounce_enabled() const {
	return multi_bounce_enabled;
}

void LocalGIVolume3D::set_debug_mode(DebugMode p_mode) {
	ERR_FAIL_INDEX(p_mode, DEBUG_MAX);
	if (debug_mode == p_mode) {
		return;
	}
	debug_mode = p_mode;
	update_gizmos();
	_update_debug_mesh();
}

LocalGIVolume3D::DebugMode LocalGIVolume3D::get_debug_mode() const {
	return debug_mode;
}

Node *LocalGIVolume3D::_resolve_from_node(Node *p_from_node) const {
	Node *from_node = p_from_node;
	if (!from_node) {
		from_node = const_cast<LocalGIVolume3D *>(this);
		while (from_node->get_parent() != nullptr && Object::cast_to<Viewport>(from_node->get_parent()) == nullptr) {
			from_node = from_node->get_parent();
		}
	}
	return from_node;
}

void LocalGIVolume3D::_collect_dynamic_keys(Node *p_from_node, Vector<LocalGIContributorKey> &r_keys) const {
	r_keys.clear();
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), this, get_aabb(), nullptr, &r_keys, GeometryInstance3D::GI_MODE_DYNAMIC);
}

void LocalGIVolume3D::_collect_static_keys(Node *p_from_node, Vector<LocalGIContributorKey> &r_keys) const {
	r_keys.clear();
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), this, get_aabb(), nullptr, &r_keys, GeometryInstance3D::GI_MODE_STATIC);
}

void LocalGIVolume3D::_set_editor_preview_enabled(bool p_enabled) {
	const bool want = p_enabled && is_inside_tree() && Engine::get_singleton()->is_editor_hint();
	set_process_internal(want);
	if (!is_inside_tree()) {
		return;
	}

	SceneTree *tree = get_tree();
	const Callable changed = callable_mp(this, &LocalGIVolume3D::_editor_scene_changed);
	if (want) {
		if (!tree->is_connected("node_added", changed)) {
			tree->connect("node_added", changed);
			tree->connect("node_removed", changed);
		}
	} else if (tree->is_connected("node_added", changed)) {
		tree->disconnect("node_added", changed);
		tree->disconnect("node_removed", changed);
	}
}

void LocalGIVolume3D::_editor_scene_changed(Node *p_node) {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_node != nullptr && Object::cast_to<GeometryInstance3D>(p_node) == nullptr) {
		return;
	}
	_queue_editor_preview_tick();
}

void LocalGIVolume3D::_queue_editor_preview_tick() {
	if (editor_preview_queued || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	editor_preview_queued = true;
	callable_mp(this, &LocalGIVolume3D::_editor_preview_tick).call_deferred();
}

bool LocalGIVolume3D::_refresh_contributors_if_dirty() {
	const bool static_dirty = is_static_dirty();
	const bool dyn_dirty = is_dynamic_dirty();
	if (!static_dirty && !dyn_dirty) {
		return false;
	}
	if (static_dirty) {
		bake();
	}
	if (is_dynamic_dirty()) {
		update_dynamic();
	}
	return true;
}

void LocalGIVolume3D::_editor_preview_tick() {
	editor_preview_queued = false;
	if (!is_inside_tree() || updating_debug_mesh || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}

	_refresh_contributors_if_dirty();
	compute_runtime_transport();
}

void LocalGIVolume3D::_mark_one_bounce_dirty() {
	one_bounce_ready = false;
	temporal_history_valid = false;
	temporal_probe_cursor = 0;
	collected_lights.clear();
	probe_irradiances.clear();
	probe_irradiance_samples.clear();
	probe_ray_radiances.clear();
	probe_ray_distance_mean.clear();
	probe_ray_distance_mean_samples.clear();
	probe_ray_distance_second_moment.clear();
	probe_ray_distance_second_moment_samples.clear();
}

void LocalGIVolume3D::_copy_samples_to_estimate() {
	probe_irradiances = probe_irradiance_samples;
	probe_ray_distance_mean = probe_ray_distance_mean_samples;
	probe_ray_distance_second_moment = probe_ray_distance_second_moment_samples;
}

void LocalGIVolume3D::_mark_gpu_dirty() {
	gpu_dirty = true;
	probe_rays_traced = false;
	probe_ray_hits.clear();
	probes_classified = false;
	probe_active.clear();
	_mark_one_bounce_dirty();
	update_gizmos();
	_update_debug_mesh();
}

void LocalGIVolume3D::_mark_dynamic_gpu_dirty() {
	gpu_dirty = true;
	probe_rays_traced = false;
	probe_ray_hits.clear();
	probes_classified = false;
	probe_active.clear();
	one_bounce_ready = false;
	probe_irradiance_samples.clear();
	probe_ray_radiances.clear();
	probe_ray_distance_mean_samples.clear();
	probe_ray_distance_second_moment_samples.clear();
	update_gizmos();
	_update_debug_mesh();
}

void LocalGIVolume3D::_mark_probes_dirty() {
	probes_dirty = true;
	probe_rays_traced = false;
	probe_ray_hits.clear();
	probes_classified = false;
	probe_active.clear();
	_mark_one_bounce_dirty();
}

void LocalGIVolume3D::_ensure_probes() {
	if (!probes_dirty && probe_grid.get_probe_count() > 0) {
		return;
	}
	probe_grid.build(size, probe_spacing, rays_per_probe);
	probes_dirty = false;
	probe_rays_traced = false;
	probe_ray_hits.clear();
	probes_classified = false;
	probe_active.clear();
}

void LocalGIVolume3D::_ensure_classified() {
	_ensure_probes();
	if (probes_classified && probe_active.size() == probe_grid.get_probe_count()) {
		return;
	}
	classify_probes();
}

int LocalGIVolume3D::_resolved_selected_probe() const {
	const int count = probe_grid.get_probe_count();
	if (count <= 0) {
		return 0;
	}
	if (debug_selected_probe < 0 || debug_selected_probe >= count) {
		return probe_grid.get_center_probe_index();
	}
	return debug_selected_probe;
}

void LocalGIVolume3D::bake(Node *p_from_node) {
	Vector<LocalGITriangle> triangles;
	Vector<LocalGIContributorKey> keys;
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), this, get_aabb(), &triangles, &keys, GeometryInstance3D::GI_MODE_STATIC);
	static_bvh.build(triangles);
	static_snapshot = keys;
	static_snapshot_bounds = get_aabb();
	static_has_snapshot = true;
	static_rebuild_count++;
	_mark_gpu_dirty();
}

int LocalGIVolume3D::get_static_rebuild_count() const {
	return static_rebuild_count;
}

int LocalGIVolume3D::get_baked_triangle_count() const {
	return static_bvh.get_triangles().size();
}

bool LocalGIVolume3D::is_static_dirty(Node *p_from_node) const {
	if (!static_has_snapshot || !static_snapshot_bounds.is_equal_approx(get_aabb())) {
		return true;
	}

	Vector<LocalGIContributorKey> keys;
	_collect_static_keys(p_from_node, keys);
	return !LocalGIStaticGeometry::keys_equal(keys, static_snapshot);
}

bool LocalGIVolume3D::intersect_static_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const {
	return static_bvh.intersect_ray(p_origin, p_direction, r_hit);
}

bool LocalGIVolume3D::is_dynamic_dirty(Node *p_from_node) const {
	if (!dynamic_has_snapshot || !dynamic_snapshot_bounds.is_equal_approx(get_aabb())) {
		return true;
	}

	Vector<LocalGIContributorKey> keys;
	_collect_dynamic_keys(p_from_node, keys);
	return !LocalGIStaticGeometry::keys_equal(keys, dynamic_snapshot);
}

bool LocalGIVolume3D::update_dynamic(Node *p_from_node) {
	if (!is_dynamic_dirty(p_from_node)) {
		return false;
	}

	Vector<LocalGITriangle> triangles;
	Vector<LocalGIContributorKey> keys;
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), this, get_aabb(), &triangles, &keys, GeometryInstance3D::GI_MODE_DYNAMIC);
	dynamic_bvh.build(triangles);
	dynamic_snapshot = keys;
	dynamic_snapshot_bounds = get_aabb();
	dynamic_has_snapshot = true;
	dynamic_rebuild_count++;
	_mark_dynamic_gpu_dirty();
	return true;
}

int LocalGIVolume3D::get_dynamic_rebuild_count() const {
	return dynamic_rebuild_count;
}

int LocalGIVolume3D::get_dynamic_triangle_count() const {
	return dynamic_bvh.get_triangles().size();
}

int LocalGIVolume3D::get_dynamic_contributor_count() const {
	return dynamic_snapshot.size();
}

bool LocalGIVolume3D::intersect_dynamic_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const {
	return dynamic_bvh.intersect_ray(p_origin, p_direction, r_hit);
}

bool LocalGIVolume3D::intersect_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) const {
	LocalGIRayHit static_hit;
	LocalGIRayHit dynamic_hit;
	const bool hit_static = static_bvh.intersect_ray(p_origin, p_direction, static_hit);
	const bool hit_dynamic = dynamic_bvh.intersect_ray(p_origin, p_direction, dynamic_hit);

	if (hit_static && hit_dynamic) {
		r_hit = static_hit.distance <= dynamic_hit.distance ? static_hit : dynamic_hit;
		return true;
	}
	if (hit_static) {
		r_hit = static_hit;
		return true;
	}
	if (hit_dynamic) {
		r_hit = dynamic_hit;
		return true;
	}

	r_hit = LocalGIRayHit();
	return false;
}

bool LocalGIVolume3D::is_gpu_available() {
	return gpu_tracer.ensure_available();
}

bool LocalGIVolume3D::upload_gpu() {
	if (!gpu_tracer.ensure_available()) {
		return false;
	}
	if (!gpu_dirty && gpu_tracer.is_uploaded()) {
		return true;
	}
	if (!gpu_tracer.upload(static_bvh, dynamic_bvh)) {
		return false;
	}
	gpu_dirty = false;
	_update_debug_mesh();
	return true;
}

bool LocalGIVolume3D::intersect_gpu_rays(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions, Vector<LocalGIRayHit> &r_hits) {
	if (!upload_gpu()) {
		r_hits.clear();
		return false;
	}
	return gpu_tracer.trace(p_origins, p_directions, r_hits);
}

bool LocalGIVolume3D::intersect_gpu_ray(const Vector3 &p_origin, const Vector3 &p_direction, LocalGIRayHit &r_hit) {
	Vector<Vector3> origins;
	Vector<Vector3> directions;
	origins.push_back(p_origin);
	directions.push_back(p_direction);
	Vector<LocalGIRayHit> hits;
	if (!intersect_gpu_rays(origins, directions, hits) || hits.is_empty()) {
		r_hit = LocalGIRayHit();
		return false;
	}
	r_hit = hits[0];
	return r_hit.hit;
}

LocalGICPUGPUCompareResult LocalGIVolume3D::compare_cpu_gpu_rays(const Vector<Vector3> &p_origins, const Vector<Vector3> &p_directions) {
	Vector<LocalGIRayHit> cpu_hits;
	cpu_hits.resize(p_origins.size());
	for (int i = 0; i < p_origins.size(); i++) {
		intersect_ray(p_origins[i], p_directions[i], cpu_hits.write[i]);
	}

	Vector<LocalGIRayHit> gpu_hits;
	if (!intersect_gpu_rays(p_origins, p_directions, gpu_hits)) {
		LocalGICPUGPUCompareResult failed;
		failed.ray_count = p_origins.size();
		failed.hit_mismatch = p_origins.size();
		return failed;
	}
	return LocalGIGpuTracer::compare_hits(cpu_hits, gpu_hits);
}

void LocalGIVolume3D::collect_debug_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const {
	r_origins.clear();
	r_directions.clear();

	const AABB aabb = get_aabb();
	const int ny = 9;
	const int nz = 9;
	for (int y = 0; y < ny; y++) {
		for (int z = 0; z < nz; z++) {
			const real_t u = (real_t)(y + 0.5) / (real_t)ny;
			const real_t v = (real_t)(z + 0.5) / (real_t)nz;
			const Vector3 origin(
					aabb.position.x + aabb.size.x * 0.05,
					aabb.position.y + aabb.size.y * u,
					aabb.position.z + aabb.size.z * v);
			r_origins.push_back(origin);
			r_directions.push_back(Vector3(1, 0, 0));
		}
	}
}

void LocalGIVolume3D::set_debug_selected_probe(int p_index) {
	if (debug_selected_probe == p_index) {
		return;
	}
	debug_selected_probe = p_index;
	update_gizmos();
	_update_debug_mesh();
}

int LocalGIVolume3D::get_debug_selected_probe() const {
	return debug_selected_probe;
}

void LocalGIVolume3D::build_probes() {
	probes_dirty = true;
	_ensure_probes();
	update_gizmos();
	_update_debug_mesh();
}

int LocalGIVolume3D::get_probe_count() const {
	return probe_grid.get_probe_count();
}

Vector3i LocalGIVolume3D::get_probe_resolution() const {
	return probe_grid.get_resolution();
}

int LocalGIVolume3D::get_probe_ray_budget() const {
	return probe_grid.get_ray_budget();
}

Vector3 LocalGIVolume3D::get_probe_position(int p_index) const {
	return probe_grid.get_position(p_index);
}

PackedVector3Array LocalGIVolume3D::get_probe_positions() const {
	PackedVector3Array out;
	const Vector<Vector3> &positions = probe_grid.get_positions();
	out.resize(positions.size());
	for (int i = 0; i < positions.size(); i++) {
		out.set(i, positions[i]);
	}
	return out;
}

PackedVector3Array LocalGIVolume3D::get_probe_directions() const {
	PackedVector3Array out;
	const Vector<Vector3> &dirs = probe_grid.get_directions();
	out.resize(dirs.size());
	for (int i = 0; i < dirs.size(); i++) {
		out.set(i, dirs[i]);
	}
	return out;
}

void LocalGIVolume3D::collect_probe_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const {
	probe_grid.collect_rays(r_origins, r_directions);
}

void LocalGIVolume3D::collect_selected_probe_rays(Vector<Vector3> &r_origins, Vector<Vector3> &r_directions) const {
	if (probe_grid.get_probe_count() <= 0) {
		r_origins.clear();
		r_directions.clear();
		return;
	}
	probe_grid.collect_probe_rays(_resolved_selected_probe(), r_origins, r_directions);
}

bool LocalGIVolume3D::trace_probe_rays() {
	_ensure_probes();
	Vector<Vector3> origins;
	Vector<Vector3> directions;
	probe_grid.collect_rays(origins, directions);
	if (intersect_gpu_rays(origins, directions, probe_ray_hits)) {
		probe_rays_traced = true;
		return true;
	}
	probe_ray_hits.resize(origins.size());
	for (int i = 0; i < origins.size(); i++) {
		intersect_ray(origins[i], directions[i], probe_ray_hits.write[i]);
	}
	probe_rays_traced = true;
	return !origins.is_empty();
}

Color LocalGIVolume3D::_evaluate_outgoing_radiance(const LocalGIRayHit &p_hit, const Vector3 &p_direction) const {
	if (!p_hit.hit) {
		return Color(0, 0, 0);
	}

	Vector3 normal = p_hit.normal;
	if (normal.length_squared() < (real_t)CMP_EPSILON2) {
		return Color(0, 0, 0);
	}
	normal.normalize();
	if (normal.dot(-p_direction) < 0.0) {
		normal = -normal;
	}

	Color direct_irradiance;
	const Vector3 shadow_origin = p_hit.position + normal * 0.002;
	for (int i = 0; i < collected_lights.size(); i++) {
		const LocalGIDirectLight::Sample sample = collected_lights[i].sample(p_hit.position, normal);
		if (!sample.valid) {
			continue;
		}

		LocalGIRayHit shadow_hit;
		if (intersect_ray(shadow_origin, sample.to_light, shadow_hit) && shadow_hit.distance + 0.002 < sample.distance) {
			continue;
		}
		direct_irradiance += sample.irradiance;
	}

	Color outgoing = p_hit.albedo * direct_irradiance * (1.0f / (float)Math::PI);
	return Color(outgoing.r, outgoing.g, outgoing.b, 1.0f);
}

Color LocalGIVolume3D::_evaluate_previous_indirect_radiance(const LocalGIRayHit &p_hit, const Vector3 &p_direction) const {
	if (!p_hit.hit || !multi_bounce_enabled || !temporal_history_valid || probe_irradiances.is_empty()) {
		return Color(0, 0, 0);
	}

	Vector3 normal = p_hit.normal;
	if (normal.length_squared() < (real_t)CMP_EPSILON2) {
		return Color(0, 0, 0);
	}
	normal.normalize();
	if (normal.dot(-p_direction) < 0.0) {
		normal = -normal;
	}

	// The estimate field is read-only during compute_one_bounce. The current
	// sample is written separately, so this cannot feed back within the pass.
	const LocalGIShadingSample previous = LocalGIProbeSampler::interpolate(
			probe_grid,
			probe_irradiances,
			probe_ray_distance_mean,
			probe_ray_distance_second_moment,
			p_hit.position,
			normal,
			_visibility_bias(),
			&probe_active);
	// Probe irradiance is the spherical integral (4π times mean radiance).
	// Under the diffuse/isotropic approximation, its incident hemispherical
	// irradiance is one quarter of that integral before applying Lambertian BRDF.
	const Color outgoing = p_hit.albedo * previous.irradiance * (1.0f / (4.0f * (float)Math::PI));
	return Color(outgoing.r, outgoing.g, outgoing.b, 1.0f);
}

bool LocalGIVolume3D::compute_one_bounce(Node *p_from_node) {
	_ensure_probes();
	_ensure_classified();
	LocalGIDirectLights::collect(_resolve_from_node(p_from_node), this, collected_lights);

	Vector<Vector3> origins;
	Vector<Vector3> directions;
	probe_grid.collect_rays(origins, directions);

	const int probe_count = probe_grid.get_probe_count();
	const int rays = probe_grid.get_rays_per_probe();
	probe_irradiance_samples.resize(probe_count);
	probe_ray_radiances.resize(origins.size());
	probe_ray_distance_mean_samples.resize(origins.size());
	probe_ray_distance_second_moment_samples.resize(origins.size());

	const float solid_angle = rays > 0 ? (4.0f * (float)Math::PI) / (float)rays : 0.0f;
	const float far_distance = MAX((float)get_aabb().size.length(), 1.0f);
	for (int p = 0; p < probe_count; p++) {
		Color spherical_irradiance;
		for (int r = 0; r < rays; r++) {
			const int index = p * rays + r;
			const Vector3 direction = directions[index].normalized();
			LocalGIRayHit hit;
			intersect_ray(origins[index], direction, hit);
			Color incoming = _evaluate_outgoing_radiance(hit, direction);
			incoming += _evaluate_previous_indirect_radiance(hit, direction);
			incoming.a = 1.0f;
			probe_ray_radiances.write[index] = incoming;
			const float mean = hit.hit ? hit.distance : far_distance;
			probe_ray_distance_mean_samples.write[index] = mean;
			probe_ray_distance_second_moment_samples.write[index] = mean * mean;
			spherical_irradiance += incoming * solid_angle;
		}
		probe_irradiance_samples.write[p] = Color(spherical_irradiance.r, spherical_irradiance.g, spherical_irradiance.b, 1.0f);
	}

	if (!temporal_history_valid || probe_irradiances.size() != probe_count) {
		_copy_samples_to_estimate();
	}
	one_bounce_ready = true;
	if (!updating_debug_mesh) {
		update_gizmos();
		_update_debug_mesh();
	}
	return probe_count > 0;
}

void LocalGIVolume3D::_update_forward_integration() {
	RendererSceneRenderRD *renderer = RendererSceneRenderRD::get_singleton();
	if (!is_inside_tree() || renderer == nullptr || !one_bounce_ready || probe_irradiances.is_empty()) {
		return;
	}
	Vector<float> probe_mean;
	Vector<float> probe_second;
	const int probe_count = probe_grid.get_probe_count();
	const int rays = probe_grid.get_rays_per_probe();
	probe_mean.resize(probe_count);
	probe_second.resize(probe_count);
	for (int probe = 0; probe < probe_count; probe++) {
		float mean = 0.0f;
		float second = 0.0f;
		for (int ray = 0; ray < rays; ray++) {
			const int index = probe * rays + ray;
			if (index < probe_ray_distance_mean.size()) {
				mean += probe_ray_distance_mean[index];
			}
			if (index < probe_ray_distance_second_moment.size()) {
				second += probe_ray_distance_second_moment[index];
			}
		}
		const float inv_rays = rays > 0 ? 1.0f / (float)rays : 0.0f;
		probe_mean.write[probe] = mean * inv_rays;
		probe_second.write[probe] = second * inv_rays;
	}
	renderer->local_gi_set_volume(get_global_transform().affine_inverse(), size, probe_grid.get_resolution(), probe_spacing, probe_irradiances, probe_mean, probe_second, probe_active);
}

bool LocalGIVolume3D::compute_runtime_transport(Node *p_from_node) {
	_ensure_probes();
	LocalGIDirectLights::collect(_resolve_from_node(p_from_node), this, collected_lights);

	RendererRD::LocalGIRuntime::Input input;
	_pack_runtime_bvh(static_bvh, input.static_nodes, input.static_triangles);
	_pack_runtime_bvh(dynamic_bvh, input.dynamic_nodes, input.dynamic_triangles);
	input.probe_positions = probe_grid.get_positions();
	input.ray_directions = probe_grid.get_directions();
	_pack_runtime_lights(collected_lights, input.lights);
	input.irradiance_history = probe_irradiances;
	input.distance_mean_history = probe_ray_distance_mean;
	input.distance_second_moment_history = probe_ray_distance_second_moment;
	input.probe_resolution = probe_grid.get_resolution();
	input.volume_size = size;
	input.visibility_bias = _visibility_bias();
	input.temporal_hysteresis = temporal_hysteresis;
	input.temporal_cursor = temporal_probe_cursor;
	input.update_count = LocalGITemporal::probe_update_count(probe_grid.get_probe_count(), update_fraction);
	input.history_valid = temporal_history_valid;
	input.multi_bounce = multi_bounce_enabled;

	if (runtime_transport == nullptr) {
		runtime_transport = memnew(RendererRD::LocalGIRuntime);
	}
	RendererRD::LocalGIRuntime::Output output;
	if (!runtime_transport->process(input, output)) {
		return false;
	}

	probe_irradiance_samples = output.irradiance_samples;
	probe_irradiances = output.irradiance_estimates;
	probe_ray_radiances = output.ray_radiances;
	probe_ray_distance_mean_samples = output.distance_mean_samples;
	probe_ray_distance_second_moment_samples = output.distance_second_moment_samples;
	probe_ray_distance_mean = output.distance_mean_estimates;
	probe_ray_distance_second_moment = output.distance_second_moment_estimates;
	probe_active = output.probe_active;
	probes_classified = true;
	one_bounce_ready = true;
	temporal_history_valid = true;
	if (input.update_count > 0) {
		temporal_probe_cursor = (temporal_probe_cursor + input.update_count) % probe_grid.get_probe_count();
	}
	_update_forward_integration();
	if (!updating_debug_mesh) {
		update_gizmos();
		_update_debug_mesh();
	}
	return true;
}

void LocalGIVolume3D::reset_temporal_history() {
	if (probe_irradiance_samples.is_empty()) {
		temporal_history_valid = false;
		temporal_probe_cursor = 0;
		return;
	}

	probe_irradiances.resize(probe_irradiance_samples.size());
	for (int i = 0; i < probe_irradiances.size(); i++) {
		probe_irradiances.write[i] = Color(0, 0, 0, 1);
	}
	if (probe_ray_distance_mean.size() != probe_ray_distance_mean_samples.size()) {
		probe_ray_distance_mean = probe_ray_distance_mean_samples;
		probe_ray_distance_second_moment = probe_ray_distance_second_moment_samples;
	}
	temporal_history_valid = true;
	temporal_probe_cursor = 0;
	if (!updating_debug_mesh) {
		update_gizmos();
		_update_debug_mesh();
	}
}

bool LocalGIVolume3D::update_temporal() {
	if (!one_bounce_ready || probe_irradiance_samples.is_empty()) {
		return false;
	}

	const int probe_count = probe_irradiance_samples.size();
	if (!temporal_history_valid || probe_irradiances.size() != probe_count) {
		_copy_samples_to_estimate();
		temporal_history_valid = true;
		temporal_probe_cursor = 0;
		if (!updating_debug_mesh) {
			update_gizmos();
			_update_debug_mesh();
		}
		return true;
	}

	const int rays = probe_grid.get_rays_per_probe();
	const int to_update = LocalGITemporal::probe_update_count(probe_count, update_fraction);
	if (to_update <= 0) {
		return true;
	}

	LocalGITemporal::blend(
			probe_irradiances,
			probe_ray_distance_mean,
			probe_ray_distance_second_moment,
			probe_irradiance_samples,
			probe_ray_distance_mean_samples,
			probe_ray_distance_second_moment_samples,
			&probe_active,
			probe_count,
			rays,
			temporal_probe_cursor,
			to_update,
			temporal_hysteresis);
	temporal_probe_cursor = (temporal_probe_cursor + to_update) % probe_count;

	if (!updating_debug_mesh) {
		update_gizmos();
		_update_debug_mesh();
	}
	return true;
}

bool LocalGIVolume3D::step_temporal(Node *p_from_node) {
	if (!compute_one_bounce(p_from_node)) {
		return false;
	}
	return update_temporal();
}

void LocalGIVolume3D::classify_probes() {
	_ensure_probes();
	LocalGIProbeClassifier::classify(probe_grid, static_bvh, dynamic_bvh, probe_active);
	probes_classified = true;
}

bool LocalGIVolume3D::is_probe_active(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, probe_grid.get_probe_count(), false);
	if (p_index < 0 || p_index >= probe_active.size()) {
		return true;
	}
	return probe_active[p_index] != 0;
}

int LocalGIVolume3D::get_active_probe_count() const {
	int count = 0;
	for (int i = 0; i < probe_active.size(); i++) {
		if (probe_active[i] != 0) {
			count++;
		}
	}
	return count;
}

PackedByteArray LocalGIVolume3D::get_probe_active_states() const {
	PackedByteArray out;
	out.resize(probe_active.size());
	for (int i = 0; i < probe_active.size(); i++) {
		out.set(i, probe_active[i]);
	}
	return out;
}

int LocalGIVolume3D::get_collected_light_count() const {
	return collected_lights.size();
}

bool LocalGIVolume3D::probe_irradiance_is_finite() const {
	for (int i = 0; i < probe_irradiances.size(); i++) {
		const Color &c = probe_irradiances[i];
		if (!Math::is_finite(c.r) || !Math::is_finite(c.g) || !Math::is_finite(c.b)) {
			return false;
		}
	}
	for (int i = 0; i < probe_irradiance_samples.size(); i++) {
		const Color &c = probe_irradiance_samples[i];
		if (!Math::is_finite(c.r) || !Math::is_finite(c.g) || !Math::is_finite(c.b)) {
			return false;
		}
	}
	for (int i = 0; i < probe_ray_radiances.size(); i++) {
		const Color &c = probe_ray_radiances[i];
		if (!Math::is_finite(c.r) || !Math::is_finite(c.g) || !Math::is_finite(c.b)) {
			return false;
		}
	}
	for (int i = 0; i < probe_ray_distance_mean.size(); i++) {
		if (!Math::is_finite(probe_ray_distance_mean[i])) {
			return false;
		}
	}
	for (int i = 0; i < probe_ray_distance_second_moment.size(); i++) {
		if (!Math::is_finite(probe_ray_distance_second_moment[i])) {
			return false;
		}
	}
	return one_bounce_ready;
}

Color LocalGIVolume3D::get_probe_irradiance(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, probe_irradiances.size(), Color());
	return probe_irradiances[p_index];
}

PackedColorArray LocalGIVolume3D::get_probe_irradiances() const {
	PackedColorArray out;
	out.resize(probe_irradiances.size());
	for (int i = 0; i < probe_irradiances.size(); i++) {
		out.set(i, probe_irradiances[i]);
	}
	return out;
}

Color LocalGIVolume3D::get_mean_probe_irradiance() const {
	if (probe_irradiances.is_empty()) {
		return Color();
	}
	Color sum;
	for (int i = 0; i < probe_irradiances.size(); i++) {
		sum += probe_irradiances[i];
	}
	return sum * (1.0f / (float)probe_irradiances.size());
}

Color LocalGIVolume3D::get_probe_irradiance_sample(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, probe_irradiance_samples.size(), Color());
	return probe_irradiance_samples[p_index];
}

Color LocalGIVolume3D::get_mean_probe_irradiance_sample() const {
	if (probe_irradiance_samples.is_empty()) {
		return Color();
	}
	Color sum;
	for (int i = 0; i < probe_irradiance_samples.size(); i++) {
		sum += probe_irradiance_samples[i];
	}
	return sum * (1.0f / (float)probe_irradiance_samples.size());
}

Color LocalGIVolume3D::get_probe_ray_radiance(int p_probe_index, int p_ray_index) const {
	const int rays = probe_grid.get_rays_per_probe();
	ERR_FAIL_INDEX_V(p_probe_index, probe_grid.get_probe_count(), Color());
	ERR_FAIL_INDEX_V(p_ray_index, rays, Color());
	const int index = p_probe_index * rays + p_ray_index;
	ERR_FAIL_INDEX_V(index, probe_ray_radiances.size(), Color());
	return probe_ray_radiances[index];
}

float LocalGIVolume3D::get_probe_ray_distance_mean(int p_probe_index, int p_ray_index) const {
	const int rays = probe_grid.get_rays_per_probe();
	ERR_FAIL_INDEX_V(p_probe_index, probe_grid.get_probe_count(), 0.0f);
	ERR_FAIL_INDEX_V(p_ray_index, rays, 0.0f);
	const int index = p_probe_index * rays + p_ray_index;
	ERR_FAIL_INDEX_V(index, probe_ray_distance_mean.size(), 0.0f);
	return probe_ray_distance_mean[index];
}

float LocalGIVolume3D::get_probe_ray_distance_second_moment(int p_probe_index, int p_ray_index) const {
	const int rays = probe_grid.get_rays_per_probe();
	ERR_FAIL_INDEX_V(p_probe_index, probe_grid.get_probe_count(), 0.0f);
	ERR_FAIL_INDEX_V(p_ray_index, rays, 0.0f);
	const int index = p_probe_index * rays + p_ray_index;
	ERR_FAIL_INDEX_V(index, probe_ray_distance_second_moment.size(), 0.0f);
	return probe_ray_distance_second_moment[index];
}

float LocalGIVolume3D::_visibility_bias() const {
	return 0.01f + 0.02f * probe_spacing;
}

LocalGIShadingSample LocalGIVolume3D::sample_shading(const Vector3 &p_local_position, const Vector3 &p_local_normal) const {
	if (!one_bounce_ready) {
		LocalGIShadingSample empty;
		empty.irradiance.a = 1.0f;
		return empty;
	}
	return LocalGIProbeSampler::interpolate(probe_grid, probe_irradiances, probe_ray_distance_mean, probe_ray_distance_second_moment, p_local_position, p_local_normal, _visibility_bias(), &probe_active);
}

Color LocalGIVolume3D::sample_indirect_irradiance(const Vector3 &p_local_position, const Vector3 &p_local_normal) const {
	return sample_shading(p_local_position, p_local_normal).irradiance;
}

Color LocalGIVolume3D::sample_indirect_radiance(const Vector3 &p_local_position, const Vector3 &p_local_normal, const Color &p_albedo) const {
	const Color irradiance = sample_indirect_irradiance(p_local_position, p_local_normal);
	const Color radiance = p_albedo * irradiance * (1.0f / (float)Math::PI);
	return Color(radiance.r, radiance.g, radiance.b, 1.0f);
}

Dictionary LocalGIVolume3D::_sample_shading_bind(const Vector3 &p_position, const Vector3 &p_normal) const {
	const LocalGIShadingSample sample = sample_shading(p_position, p_normal);
	Dictionary result;
	result["irradiance"] = sample.irradiance;
	result["weight_sum"] = sample.weight_sum;
	result["visibility_mean"] = sample.visibility_mean;
	result["finite"] = sample.finite;
	Array corners;
	corners.resize(sample.corner_count);
	for (int i = 0; i < sample.corner_count; i++) {
		Dictionary corner;
		corner["index"] = sample.corners[i].index;
		corner["trilinear_weight"] = sample.corners[i].trilinear_weight;
		corner["normal_weight"] = sample.corners[i].normal_weight;
		corner["visibility_weight"] = sample.corners[i].visibility_weight;
		corner["weight"] = sample.corners[i].weight;
		corner["active"] = sample.corners[i].active;
		corners[i] = corner;
	}
	result["corners"] = corners;
	return result;
}

void LocalGIVolume3D::_update_debug_mesh() {
	if (updating_debug_mesh) {
		return;
	}
	updating_debug_mesh = true;

	const bool show_rays = debug_mode == DEBUG_STATIC_BVH_HIT ||
			debug_mode == DEBUG_DYNAMIC_BVH_HIT ||
			debug_mode == DEBUG_RAY_HIT_MISS ||
			debug_mode == DEBUG_HIT_NORMAL ||
			debug_mode == DEBUG_HIT_DISTANCE;
	const bool show_probes = debug_mode == DEBUG_PROBE_POSITIONS ||
			debug_mode == DEBUG_SELECTED_PROBE_RAYS ||
			debug_mode == DEBUG_RAW_PROBE_RADIANCE ||
			debug_mode == DEBUG_PROBE_IRRADIANCE ||
			debug_mode == DEBUG_PROBE_CLASSIFICATION;
	const bool show_shading = debug_mode == DEBUG_VISIBILITY ||
			debug_mode == DEBUG_PROBE_WEIGHTS ||
			debug_mode == DEBUG_FINAL_LOCAL_GI;
	if ((!show_rays && !show_probes && !show_shading) || !is_inside_tree()) {
		_set_debug_mesh_visible(false);
		updating_debug_mesh = false;
		return;
	}

	if (debug_mesh.is_null()) {
		debug_mesh.instantiate();
	}
	if (debug_material.is_null()) {
		debug_material.instantiate();
		debug_material->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
		debug_material->set_flag(StandardMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		debug_material->set_flag(StandardMaterial3D::FLAG_SRGB_VERTEX_COLOR, true);
		debug_material->set_flag(StandardMaterial3D::FLAG_DISABLE_FOG, true);
		debug_material->set_cull_mode(StandardMaterial3D::CULL_BACK);
		debug_material->set_transparency(StandardMaterial3D::TRANSPARENCY_DISABLED);
	}
	RenderingServer::get_singleton()->instance_geometry_set_cast_shadows_setting(get_instance(), RSE::SHADOW_CASTING_SETTING_OFF);

	if (show_probes) {
		_draw_probe_debug_mesh();
		updating_debug_mesh = false;
		return;
	}
	if (show_shading) {
		_draw_shading_debug_mesh();
		updating_debug_mesh = false;
		return;
	}

	Vector<Vector3> origins;
	Vector<Vector3> directions;
	collect_debug_rays(origins, directions);

	Vector<LocalGIRayHit> hits;
	const bool use_gpu = upload_gpu() && intersect_gpu_rays(origins, directions, hits);
	if (!use_gpu) {
		hits.resize(origins.size());
		for (int i = 0; i < origins.size(); i++) {
			if (debug_mode == DEBUG_STATIC_BVH_HIT) {
				intersect_static_ray(origins[i], directions[i], hits.write[i]);
			} else if (debug_mode == DEBUG_DYNAMIC_BVH_HIT) {
				intersect_dynamic_ray(origins[i], directions[i], hits.write[i]);
			} else {
				intersect_ray(origins[i], directions[i], hits.write[i]);
			}
		}
	} else if (debug_mode == DEBUG_STATIC_BVH_HIT || debug_mode == DEBUG_DYNAMIC_BVH_HIT) {
		for (int i = 0; i < origins.size(); i++) {
			if (debug_mode == DEBUG_STATIC_BVH_HIT) {
				intersect_static_ray(origins[i], directions[i], hits.write[i]);
			} else {
				intersect_dynamic_ray(origins[i], directions[i], hits.write[i]);
			}
		}
	}

	debug_mesh->clear_surfaces();
	debug_mesh->surface_begin(Mesh::PRIMITIVE_LINES, debug_material);

	real_t max_distance = 0.001;
	for (int i = 0; i < hits.size(); i++) {
		if (hits[i].hit) {
			max_distance = MAX(max_distance, hits[i].distance);
		}
	}

	for (int i = 0; i < origins.size(); i++) {
		const Vector3 dir = directions[i].normalized();
		const LocalGIRayHit &hit = hits[i];
		const Vector3 end = hit.hit ? hit.position : (origins[i] + dir * get_aabb().size.length());
		Color color;
		if (!hit.hit) {
			color = Color(0.25, 0.3, 0.4, 1.0);
		} else if (debug_mode == DEBUG_HIT_NORMAL) {
			color = Color(hit.normal.x * 0.5 + 0.5, hit.normal.y * 0.5 + 0.5, hit.normal.z * 0.5 + 0.5);
		} else if (debug_mode == DEBUG_HIT_DISTANCE) {
			const float t = CLAMP((float)(hit.distance / max_distance), 0.0f, 1.0f);
			color = Color(t, 1.0f - t, 0.15f);
		} else {
			color = Color(0.15, 0.95, 0.35);
		}
		debug_mesh->surface_set_color(color);
		debug_mesh->surface_add_vertex(origins[i]);
		debug_mesh->surface_add_vertex(end);

		if (hit.hit && (debug_mode == DEBUG_HIT_NORMAL || debug_mode == DEBUG_RAY_HIT_MISS)) {
			debug_mesh->surface_set_color(Color(0.2, 0.55, 1.0));
			debug_mesh->surface_add_vertex(hit.position);
			debug_mesh->surface_add_vertex(hit.position + hit.normal * 0.12);
		}
	}

	debug_mesh->surface_end();
	_set_debug_mesh_visible(true);
	updating_debug_mesh = false;
}

void LocalGIVolume3D::_draw_probe_debug_mesh() {
	_ensure_probes();
	if (!static_has_snapshot) {
		bake();
	}
	if (!dynamic_has_snapshot) {
		update_dynamic();
	}
	if ((debug_mode == DEBUG_PROBE_IRRADIANCE || debug_mode == DEBUG_RAW_PROBE_RADIANCE) && !one_bounce_ready) {
		compute_one_bounce();
	}
	if (debug_mode == DEBUG_PROBE_CLASSIFICATION) {
		_ensure_classified();
	}

	debug_mesh->clear_surfaces();

	const int selected = _resolved_selected_probe();
	const float radius = MIN(0.16f, probe_spacing * 0.28f);
	const Vector<Vector3> &positions = probe_grid.get_positions();
	const Vector<Vector3> &sample_dirs = probe_grid.get_directions();
	const int rays = sample_dirs.size();
	const bool show_directional_gi = (debug_mode == DEBUG_RAW_PROBE_RADIANCE || (debug_mode == DEBUG_PROBE_IRRADIANCE && !temporal_history_valid)) &&
			rays > 0 && probe_ray_radiances.size() == positions.size() * rays;

	if (!positions.is_empty()) {
		debug_mesh->surface_begin(Mesh::PRIMITIVE_TRIANGLES, debug_material);
		for (int i = 0; i < positions.size(); i++) {
			const bool is_selected = i == selected;
			Color color = is_selected ? Color(1.0, 0.85, 0.2) : Color(0.25, 0.75, 1.0);
			if (debug_mode == DEBUG_PROBE_CLASSIFICATION) {
				const bool active = is_probe_active(i);
				color = active ? Color(0.2, 0.95, 0.35) : Color(0.95, 0.2, 0.18);
			} else if (debug_mode == DEBUG_PROBE_IRRADIANCE && i < probe_irradiances.size()) {
				color = _debug_tonemap_color(probe_irradiances[i]);
			}
			const Color *radiance = show_directional_gi ? probe_ray_radiances.ptr() + i * rays : nullptr;
			_add_debug_sphere(debug_mesh.ptr(), positions[i], is_selected ? radius * 1.35f : radius, color, show_directional_gi ? &sample_dirs : nullptr, radiance, show_directional_gi ? rays : 0);
		}
		debug_mesh->surface_end();
	}

	if ((debug_mode == DEBUG_SELECTED_PROBE_RAYS || debug_mode == DEBUG_RAW_PROBE_RADIANCE) && !positions.is_empty()) {
		Vector<Vector3> origins;
		Vector<Vector3> directions;
		collect_selected_probe_rays(origins, directions);
		Vector<LocalGIRayHit> hits;
		if (!intersect_gpu_rays(origins, directions, hits)) {
			hits.resize(origins.size());
			for (int i = 0; i < origins.size(); i++) {
				intersect_ray(origins[i], directions[i], hits.write[i]);
			}
		}

		const real_t miss_length = get_aabb().size.length();
		const int selected_probe = _resolved_selected_probe();
		const int rays = probe_grid.get_rays_per_probe();
		debug_mesh->surface_begin(Mesh::PRIMITIVE_LINES, debug_material);
		for (int i = 0; i < origins.size(); i++) {
			const Vector3 dir = directions[i].normalized();
			const LocalGIRayHit &hit = hits[i];
			const Vector3 end = hit.hit ? hit.position : (origins[i] + dir * miss_length);
			Color color = hit.hit ? Color(0.15, 0.95, 0.35) : Color(0.25, 0.3, 0.4, 1.0);
			if (debug_mode == DEBUG_RAW_PROBE_RADIANCE) {
				const int index = selected_probe * rays + i;
				if (index < probe_ray_radiances.size()) {
					color = _debug_tonemap_color(probe_ray_radiances[index]);
				}
			}
			debug_mesh->surface_set_color(color);
			debug_mesh->surface_add_vertex(origins[i]);
			debug_mesh->surface_add_vertex(end);
			if (hit.hit && debug_mode == DEBUG_SELECTED_PROBE_RAYS) {
				debug_mesh->surface_set_color(Color(0.2, 0.55, 1.0));
				debug_mesh->surface_add_vertex(hit.position);
				debug_mesh->surface_add_vertex(hit.position + hit.normal * 0.12);
			}
		}
		debug_mesh->surface_end();
	}

	_set_debug_mesh_visible(true);
}

void LocalGIVolume3D::_draw_shading_debug_mesh() {
	_ensure_probes();
	if (!static_has_snapshot) {
		bake();
	}
	if (!dynamic_has_snapshot) {
		update_dynamic();
	}
	if (!one_bounce_ready) {
		compute_one_bounce();
	}

	debug_mesh->clear_surfaces();
	if (!one_bounce_ready) {
		_set_debug_mesh_visible(false);
		return;
	}

	const int nx = 16;
	const int nz = 16;
	const Vector3 half = size * 0.41f;
	const float radius = MIN(0.05f, probe_spacing * 0.12f);
	const Vector3 normal(0, 1, 0);

	debug_mesh->surface_begin(Mesh::PRIMITIVE_TRIANGLES, debug_material);
	for (int ix = 0; ix < nx; ix++) {
		for (int iz = 0; iz < nz; iz++) {
			const float u = nx <= 1 ? 0.5f : (float)ix / (float)(nx - 1);
			const float v = nz <= 1 ? 0.5f : (float)iz / (float)(nz - 1);
			const Vector3 pos(-half.x + 2.0f * half.x * u, 0.0f, -half.z + 2.0f * half.z * v);
			const LocalGIShadingSample sample = sample_shading(pos, normal);
			Color color = _debug_tonemap_color(sample.irradiance);
			if (debug_mode == DEBUG_VISIBILITY) {
				const float vis = CLAMP(sample.visibility_mean, 0.0f, 1.0f);
				color = Color(vis, vis, vis);
			} else if (debug_mode == DEBUG_PROBE_WEIGHTS) {
				const float w = CLAMP(sample.weight_sum, 0.0f, 1.0f);
				color = Color(w, 1.0f - w, 0.15f);
			}
			color.a = 1.0f;
			_add_debug_sphere(debug_mesh.ptr(), pos, radius, color);
		}
	}
	debug_mesh->surface_end();
	_set_debug_mesh_visible(true);
}

void LocalGIVolume3D::_set_debug_mesh_visible(bool p_visible) {
	if (!p_visible || debug_mesh.is_null()) {
		set_base(RID());
		return;
	}
	set_base(debug_mesh->get_rid());
}

void LocalGIVolume3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			_set_editor_preview_enabled(true);
			_queue_editor_preview_tick();
			_update_debug_mesh();
			break;
		case NOTIFICATION_EXIT_TREE:
			_set_editor_preview_enabled(false);
			if (RendererSceneRenderRD::get_singleton() != nullptr) {
				RendererSceneRenderRD::get_singleton()->local_gi_clear_volume();
			}
			break;
		case NOTIFICATION_TRANSFORM_CHANGED:
			_update_forward_integration();
			break;
		case NOTIFICATION_VISIBILITY_CHANGED:
			_update_debug_mesh();
			break;
		case NOTIFICATION_INTERNAL_PROCESS:
			_editor_preview_tick();
			break;
		default:
			break;
	}
}

Dictionary LocalGIVolume3D::_hit_to_dictionary(const LocalGIRayHit &p_hit) const {
	Dictionary result;
	result["hit"] = p_hit.hit;
	result["distance"] = p_hit.distance;
	result["position"] = p_hit.position;
	result["normal"] = p_hit.normal;
	result["triangle_index"] = p_hit.triangle_index;
	return result;
}

Dictionary LocalGIVolume3D::_intersect_static_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) const {
	LocalGIRayHit hit;
	static_bvh.intersect_ray(p_origin, p_direction, hit);
	return _hit_to_dictionary(hit);
}

Dictionary LocalGIVolume3D::_intersect_dynamic_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) const {
	LocalGIRayHit hit;
	dynamic_bvh.intersect_ray(p_origin, p_direction, hit);
	return _hit_to_dictionary(hit);
}

Dictionary LocalGIVolume3D::_intersect_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) const {
	LocalGIRayHit hit;
	intersect_ray(p_origin, p_direction, hit);
	return _hit_to_dictionary(hit);
}

Dictionary LocalGIVolume3D::_intersect_gpu_ray_bind(const Vector3 &p_origin, const Vector3 &p_direction) {
	LocalGIRayHit hit;
	intersect_gpu_ray(p_origin, p_direction, hit);
	return _hit_to_dictionary(hit);
}

TypedArray<Dictionary> LocalGIVolume3D::_intersect_gpu_rays_bind(const PackedVector3Array &p_origins, const PackedVector3Array &p_directions) {
	Vector<Vector3> origins;
	Vector<Vector3> directions;
	origins.resize(p_origins.size());
	directions.resize(p_directions.size());
	for (int i = 0; i < p_origins.size(); i++) {
		origins.write[i] = p_origins[i];
	}
	for (int i = 0; i < p_directions.size(); i++) {
		directions.write[i] = p_directions[i];
	}

	Vector<LocalGIRayHit> hits;
	intersect_gpu_rays(origins, directions, hits);
	TypedArray<Dictionary> result;
	result.resize(hits.size());
	for (int i = 0; i < hits.size(); i++) {
		result[i] = _hit_to_dictionary(hits[i]);
	}
	return result;
}

Dictionary LocalGIVolume3D::_compare_cpu_gpu_rays_bind(const PackedVector3Array &p_origins, const PackedVector3Array &p_directions) {
	Vector<Vector3> origins;
	Vector<Vector3> directions;
	origins.resize(p_origins.size());
	directions.resize(p_directions.size());
	for (int i = 0; i < p_origins.size(); i++) {
		origins.write[i] = p_origins[i];
	}
	for (int i = 0; i < p_directions.size(); i++) {
		directions.write[i] = p_directions[i];
	}

	const LocalGICPUGPUCompareResult compare = compare_cpu_gpu_rays(origins, directions);
	Dictionary result;
	result["ray_count"] = compare.ray_count;
	result["hit_mismatch"] = compare.hit_mismatch;
	result["nearest_mismatch"] = compare.nearest_mismatch;
	result["identity_mismatch"] = compare.identity_mismatch;
	result["max_distance_error"] = compare.max_distance_error;
	result["max_normal_error"] = compare.max_normal_error;
	result["passed"] = compare.passed;
	return result;
}

AABB LocalGIVolume3D::get_aabb() const {
	return AABB(-size / 2, size);
}

void LocalGIVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LocalGIVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LocalGIVolume3D::get_size);

	ClassDB::bind_method(D_METHOD("set_probe_spacing", "spacing"), &LocalGIVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LocalGIVolume3D::get_probe_spacing);

	ClassDB::bind_method(D_METHOD("set_rays_per_probe", "rays"), &LocalGIVolume3D::set_rays_per_probe);
	ClassDB::bind_method(D_METHOD("get_rays_per_probe"), &LocalGIVolume3D::get_rays_per_probe);

	ClassDB::bind_method(D_METHOD("set_update_fraction", "fraction"), &LocalGIVolume3D::set_update_fraction);
	ClassDB::bind_method(D_METHOD("get_update_fraction"), &LocalGIVolume3D::get_update_fraction);

	ClassDB::bind_method(D_METHOD("set_temporal_hysteresis", "hysteresis"), &LocalGIVolume3D::set_temporal_hysteresis);
	ClassDB::bind_method(D_METHOD("get_temporal_hysteresis"), &LocalGIVolume3D::get_temporal_hysteresis);

	ClassDB::bind_method(D_METHOD("set_multi_bounce_enabled", "enabled"), &LocalGIVolume3D::set_multi_bounce_enabled);
	ClassDB::bind_method(D_METHOD("is_multi_bounce_enabled"), &LocalGIVolume3D::is_multi_bounce_enabled);

	ClassDB::bind_method(D_METHOD("set_debug_mode", "mode"), &LocalGIVolume3D::set_debug_mode);
	ClassDB::bind_method(D_METHOD("get_debug_mode"), &LocalGIVolume3D::get_debug_mode);

	ClassDB::bind_method(D_METHOD("bake", "from_node"), &LocalGIVolume3D::bake, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("get_static_rebuild_count"), &LocalGIVolume3D::get_static_rebuild_count);
	ClassDB::bind_method(D_METHOD("get_baked_triangle_count"), &LocalGIVolume3D::get_baked_triangle_count);
	ClassDB::bind_method(D_METHOD("is_static_dirty", "from_node"), &LocalGIVolume3D::is_static_dirty, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("intersect_static_ray", "origin", "direction"), &LocalGIVolume3D::_intersect_static_ray_bind);
	ClassDB::bind_method(D_METHOD("is_dynamic_dirty", "from_node"), &LocalGIVolume3D::is_dynamic_dirty, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("update_dynamic", "from_node"), &LocalGIVolume3D::update_dynamic, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("get_dynamic_rebuild_count"), &LocalGIVolume3D::get_dynamic_rebuild_count);
	ClassDB::bind_method(D_METHOD("get_dynamic_triangle_count"), &LocalGIVolume3D::get_dynamic_triangle_count);
	ClassDB::bind_method(D_METHOD("get_dynamic_contributor_count"), &LocalGIVolume3D::get_dynamic_contributor_count);
	ClassDB::bind_method(D_METHOD("intersect_dynamic_ray", "origin", "direction"), &LocalGIVolume3D::_intersect_dynamic_ray_bind);
	ClassDB::bind_method(D_METHOD("intersect_ray", "origin", "direction"), &LocalGIVolume3D::_intersect_ray_bind);
	ClassDB::bind_method(D_METHOD("is_gpu_available"), &LocalGIVolume3D::is_gpu_available);
	ClassDB::bind_method(D_METHOD("upload_gpu"), &LocalGIVolume3D::upload_gpu);
	ClassDB::bind_method(D_METHOD("intersect_gpu_ray", "origin", "direction"), &LocalGIVolume3D::_intersect_gpu_ray_bind);
	ClassDB::bind_method(D_METHOD("intersect_gpu_rays", "origins", "directions"), &LocalGIVolume3D::_intersect_gpu_rays_bind);
	ClassDB::bind_method(D_METHOD("compare_cpu_gpu_rays", "origins", "directions"), &LocalGIVolume3D::_compare_cpu_gpu_rays_bind);
	ClassDB::bind_method(D_METHOD("set_debug_selected_probe", "index"), &LocalGIVolume3D::set_debug_selected_probe);
	ClassDB::bind_method(D_METHOD("get_debug_selected_probe"), &LocalGIVolume3D::get_debug_selected_probe);
	ClassDB::bind_method(D_METHOD("build_probes"), &LocalGIVolume3D::build_probes);
	ClassDB::bind_method(D_METHOD("get_probe_count"), &LocalGIVolume3D::get_probe_count);
	ClassDB::bind_method(D_METHOD("get_probe_resolution"), &LocalGIVolume3D::get_probe_resolution);
	ClassDB::bind_method(D_METHOD("get_probe_ray_budget"), &LocalGIVolume3D::get_probe_ray_budget);
	ClassDB::bind_method(D_METHOD("get_probe_position", "index"), &LocalGIVolume3D::get_probe_position);
	ClassDB::bind_method(D_METHOD("get_probe_positions"), &LocalGIVolume3D::get_probe_positions);
	ClassDB::bind_method(D_METHOD("get_probe_directions"), &LocalGIVolume3D::get_probe_directions);
	ClassDB::bind_method(D_METHOD("trace_probe_rays"), &LocalGIVolume3D::trace_probe_rays);
	ClassDB::bind_method(D_METHOD("compute_one_bounce", "from_node"), &LocalGIVolume3D::compute_one_bounce, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("compute_runtime_transport", "from_node"), &LocalGIVolume3D::compute_runtime_transport, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("update_temporal"), &LocalGIVolume3D::update_temporal);
	ClassDB::bind_method(D_METHOD("step_temporal", "from_node"), &LocalGIVolume3D::step_temporal, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("reset_temporal_history"), &LocalGIVolume3D::reset_temporal_history);
	ClassDB::bind_method(D_METHOD("has_temporal_history"), &LocalGIVolume3D::has_temporal_history);
	ClassDB::bind_method(D_METHOD("classify_probes"), &LocalGIVolume3D::classify_probes);
	ClassDB::bind_method(D_METHOD("is_probe_active", "index"), &LocalGIVolume3D::is_probe_active);
	ClassDB::bind_method(D_METHOD("get_active_probe_count"), &LocalGIVolume3D::get_active_probe_count);
	ClassDB::bind_method(D_METHOD("get_probe_active_states"), &LocalGIVolume3D::get_probe_active_states);
	ClassDB::bind_method(D_METHOD("get_collected_light_count"), &LocalGIVolume3D::get_collected_light_count);
	ClassDB::bind_method(D_METHOD("has_one_bounce"), &LocalGIVolume3D::has_one_bounce);
	ClassDB::bind_method(D_METHOD("probe_irradiance_is_finite"), &LocalGIVolume3D::probe_irradiance_is_finite);
	ClassDB::bind_method(D_METHOD("get_probe_irradiance", "index"), &LocalGIVolume3D::get_probe_irradiance);
	ClassDB::bind_method(D_METHOD("get_probe_irradiances"), &LocalGIVolume3D::get_probe_irradiances);
	ClassDB::bind_method(D_METHOD("get_mean_probe_irradiance"), &LocalGIVolume3D::get_mean_probe_irradiance);
	ClassDB::bind_method(D_METHOD("get_probe_irradiance_sample", "index"), &LocalGIVolume3D::get_probe_irradiance_sample);
	ClassDB::bind_method(D_METHOD("get_mean_probe_irradiance_sample"), &LocalGIVolume3D::get_mean_probe_irradiance_sample);
	ClassDB::bind_method(D_METHOD("get_probe_ray_radiance", "probe_index", "ray_index"), &LocalGIVolume3D::get_probe_ray_radiance);
	ClassDB::bind_method(D_METHOD("get_probe_ray_distance_mean", "probe_index", "ray_index"), &LocalGIVolume3D::get_probe_ray_distance_mean);
	ClassDB::bind_method(D_METHOD("get_probe_ray_distance_second_moment", "probe_index", "ray_index"), &LocalGIVolume3D::get_probe_ray_distance_second_moment);
	ClassDB::bind_method(D_METHOD("sample_shading", "position", "normal"), &LocalGIVolume3D::_sample_shading_bind);
	ClassDB::bind_method(D_METHOD("sample_indirect_irradiance", "position", "normal"), &LocalGIVolume3D::sample_indirect_irradiance);
	ClassDB::bind_method(D_METHOD("sample_indirect_radiance", "position", "normal", "albedo"), &LocalGIVolume3D::sample_indirect_radiance);

	ADD_GROUP("Volume", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");

	ADD_GROUP("Probe", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.05,8,0.01,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rays_per_probe", PROPERTY_HINT_RANGE, "1,1024,1"), "set_rays_per_probe", "get_rays_per_probe");

	ADD_GROUP("Runtime", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_fraction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_update_fraction", "get_update_fraction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temporal_hysteresis", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_temporal_hysteresis", "get_temporal_hysteresis");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "multi_bounce_enabled"), "set_multi_bounce_enabled", "is_multi_bounce_enabled");

	ADD_GROUP("Debug", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Disabled,Local Geometry,Static BVH Hit,Dynamic BVH Hit,Ray Hit/Miss,Hit Normal,Hit Distance,Probe Positions,Selected Probe Rays,Raw Probe Radiance,Probe Irradiance,Visibility,Probe Weights,Global Indirect Cache,Final Local GI,Global GI,Final Selected GI,Probe Classification"), "set_debug_mode", "get_debug_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_selected_probe", PROPERTY_HINT_RANGE, "-1,4096,1"), "set_debug_selected_probe", "get_debug_selected_probe");

	BIND_ENUM_CONSTANT(DEBUG_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_LOCAL_GEOMETRY);
	BIND_ENUM_CONSTANT(DEBUG_STATIC_BVH_HIT);
	BIND_ENUM_CONSTANT(DEBUG_DYNAMIC_BVH_HIT);
	BIND_ENUM_CONSTANT(DEBUG_RAY_HIT_MISS);
	BIND_ENUM_CONSTANT(DEBUG_HIT_NORMAL);
	BIND_ENUM_CONSTANT(DEBUG_HIT_DISTANCE);
	BIND_ENUM_CONSTANT(DEBUG_PROBE_POSITIONS);
	BIND_ENUM_CONSTANT(DEBUG_SELECTED_PROBE_RAYS);
	BIND_ENUM_CONSTANT(DEBUG_RAW_PROBE_RADIANCE);
	BIND_ENUM_CONSTANT(DEBUG_PROBE_IRRADIANCE);
	BIND_ENUM_CONSTANT(DEBUG_VISIBILITY);
	BIND_ENUM_CONSTANT(DEBUG_PROBE_WEIGHTS);
	BIND_ENUM_CONSTANT(DEBUG_GLOBAL_INDIRECT_CACHE);
	BIND_ENUM_CONSTANT(DEBUG_FINAL_LOCAL_GI);
	BIND_ENUM_CONSTANT(DEBUG_GLOBAL_GI);
	BIND_ENUM_CONSTANT(DEBUG_FINAL_SELECTED_GI);
	BIND_ENUM_CONSTANT(DEBUG_PROBE_CLASSIFICATION);
	BIND_ENUM_CONSTANT(DEBUG_MAX);
}

LocalGIVolume3D::LocalGIVolume3D() {
	set_disable_scale(true);
}

LocalGIVolume3D::~LocalGIVolume3D() {
	if (RendererSceneRenderRD::get_singleton() != nullptr) {
		RendererSceneRenderRD::get_singleton()->local_gi_clear_volume();
	}
	if (runtime_transport != nullptr) {
		memdelete(runtime_transport);
	}
	set_base(RID());
}
