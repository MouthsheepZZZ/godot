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

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/variant/typed_array.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"

namespace {

Color _opaque_rgb(const Color &p_color) {
	return Color(p_color.r, p_color.g, p_color.b, 1.0f);
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
	return _opaque_rgb(p_radiance[best]);
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
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), LocalGIStaticGeometry::get_composed_transform(this), get_aabb(), nullptr, &r_keys, GeometryInstance3D::GI_MODE_DYNAMIC);
}

void LocalGIVolume3D::_mark_one_bounce_dirty() {
	one_bounce_ready = false;
	collected_lights.clear();
	probe_irradiances.clear();
	probe_ray_radiances.clear();
}

void LocalGIVolume3D::_mark_gpu_dirty() {
	gpu_dirty = true;
	probe_rays_traced = false;
	probe_ray_hits.clear();
	_mark_one_bounce_dirty();
	update_gizmos();
	_update_debug_mesh();
}

void LocalGIVolume3D::_mark_probes_dirty() {
	probes_dirty = true;
	probe_rays_traced = false;
	probe_ray_hits.clear();
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
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), LocalGIStaticGeometry::get_composed_transform(this), get_aabb(), triangles);
	static_bvh.build(triangles);
	_mark_gpu_dirty();
}

int LocalGIVolume3D::get_baked_triangle_count() const {
	return static_bvh.get_triangles().size();
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
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), LocalGIStaticGeometry::get_composed_transform(this), get_aabb(), &triangles, &keys, GeometryInstance3D::GI_MODE_DYNAMIC);
	dynamic_bvh.build(triangles);
	dynamic_snapshot = keys;
	dynamic_snapshot_bounds = get_aabb();
	dynamic_has_snapshot = true;
	dynamic_rebuild_count++;
	_mark_gpu_dirty();
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

bool LocalGIVolume3D::compute_one_bounce(Node *p_from_node) {
	_ensure_probes();
	LocalGIDirectLights::collect(_resolve_from_node(p_from_node), LocalGIStaticGeometry::get_composed_transform(this), collected_lights);

	Vector<Vector3> origins;
	Vector<Vector3> directions;
	probe_grid.collect_rays(origins, directions);

	const int probe_count = probe_grid.get_probe_count();
	const int rays = probe_grid.get_rays_per_probe();
	probe_irradiances.resize(probe_count);
	probe_ray_radiances.resize(origins.size());

	const float solid_angle = rays > 0 ? (4.0f * (float)Math::PI) / (float)rays : 0.0f;
	for (int p = 0; p < probe_count; p++) {
		Color spherical_irradiance;
		for (int r = 0; r < rays; r++) {
			const int index = p * rays + r;
			const Vector3 direction = directions[index].normalized();
			LocalGIRayHit hit;
			intersect_ray(origins[index], direction, hit);
			const Color incoming = _evaluate_outgoing_radiance(hit, direction);
			probe_ray_radiances.write[index] = incoming;
			spherical_irradiance += incoming * solid_angle;
		}
		probe_irradiances.write[p] = Color(spherical_irradiance.r, spherical_irradiance.g, spherical_irradiance.b, 1.0f);
	}

	one_bounce_ready = true;
	if (!updating_debug_mesh) {
		update_gizmos();
		_update_debug_mesh();
	}
	return probe_count > 0;
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
	for (int i = 0; i < probe_ray_radiances.size(); i++) {
		const Color &c = probe_ray_radiances[i];
		if (!Math::is_finite(c.r) || !Math::is_finite(c.g) || !Math::is_finite(c.b)) {
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

Color LocalGIVolume3D::get_probe_ray_radiance(int p_probe_index, int p_ray_index) const {
	const int rays = probe_grid.get_rays_per_probe();
	ERR_FAIL_INDEX_V(p_probe_index, probe_grid.get_probe_count(), Color());
	ERR_FAIL_INDEX_V(p_ray_index, rays, Color());
	const int index = p_probe_index * rays + p_ray_index;
	ERR_FAIL_INDEX_V(index, probe_ray_radiances.size(), Color());
	return probe_ray_radiances[index];
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
			debug_mode == DEBUG_PROBE_IRRADIANCE;
	if ((!show_rays && !show_probes) || !is_inside_tree()) {
		if (debug_mesh_instance) {
			debug_mesh_instance->hide();
		}
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
	if (debug_mesh_instance == nullptr) {
		debug_mesh_instance = memnew(MeshInstance3D);
		debug_mesh_instance->set_mesh(debug_mesh);
		debug_mesh_instance->set_material_override(debug_material);
		debug_mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		debug_mesh_instance->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
		add_child(debug_mesh_instance, false, INTERNAL_MODE_BACK);
	}

	if (show_probes) {
		_draw_probe_debug_mesh();
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
	debug_mesh_instance->show();
	updating_debug_mesh = false;
}

void LocalGIVolume3D::_draw_probe_debug_mesh() {
	_ensure_probes();
	if ((debug_mode == DEBUG_PROBE_IRRADIANCE || debug_mode == DEBUG_RAW_PROBE_RADIANCE) && !one_bounce_ready) {
		if (get_baked_triangle_count() == 0) {
			bake();
			update_dynamic();
		}
		compute_one_bounce();
	}

	debug_mesh->clear_surfaces();

	const int selected = _resolved_selected_probe();
	const float radius = MIN(0.16f, probe_spacing * 0.28f);
	const Vector<Vector3> &positions = probe_grid.get_positions();
	const Vector<Vector3> &sample_dirs = probe_grid.get_directions();
	const int rays = sample_dirs.size();
	const bool show_directional_gi = (debug_mode == DEBUG_PROBE_IRRADIANCE || debug_mode == DEBUG_RAW_PROBE_RADIANCE) &&
			rays > 0 && probe_ray_radiances.size() == positions.size() * rays;

	if (!positions.is_empty()) {
		debug_mesh->surface_begin(Mesh::PRIMITIVE_TRIANGLES, debug_material);
		for (int i = 0; i < positions.size(); i++) {
			const bool is_selected = i == selected;
			Color color = is_selected ? Color(1.0, 0.85, 0.2) : Color(0.25, 0.75, 1.0);
			if (debug_mode == DEBUG_PROBE_IRRADIANCE && i < probe_irradiances.size()) {
				color = probe_irradiances[i];
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
					color = Color(probe_ray_radiances[index].r, probe_ray_radiances[index].g, probe_ray_radiances[index].b, 1.0);
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

	debug_mesh_instance->show();
}

void LocalGIVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		_update_debug_mesh();
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
	ClassDB::bind_method(D_METHOD("get_baked_triangle_count"), &LocalGIVolume3D::get_baked_triangle_count);
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
	ClassDB::bind_method(D_METHOD("get_collected_light_count"), &LocalGIVolume3D::get_collected_light_count);
	ClassDB::bind_method(D_METHOD("has_one_bounce"), &LocalGIVolume3D::has_one_bounce);
	ClassDB::bind_method(D_METHOD("probe_irradiance_is_finite"), &LocalGIVolume3D::probe_irradiance_is_finite);
	ClassDB::bind_method(D_METHOD("get_probe_irradiance", "index"), &LocalGIVolume3D::get_probe_irradiance);
	ClassDB::bind_method(D_METHOD("get_probe_irradiances"), &LocalGIVolume3D::get_probe_irradiances);
	ClassDB::bind_method(D_METHOD("get_mean_probe_irradiance"), &LocalGIVolume3D::get_mean_probe_irradiance);
	ClassDB::bind_method(D_METHOD("get_probe_ray_radiance", "probe_index", "ray_index"), &LocalGIVolume3D::get_probe_ray_radiance);

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
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Disabled,Local Geometry,Static BVH Hit,Dynamic BVH Hit,Ray Hit/Miss,Hit Normal,Hit Distance,Probe Positions,Selected Probe Rays,Raw Probe Radiance,Probe Irradiance,Visibility,Probe Weights,Global Indirect Cache,Final Local GI,Global GI,Final Selected GI"), "set_debug_mode", "get_debug_mode");
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
	BIND_ENUM_CONSTANT(DEBUG_MAX);
}

LocalGIVolume3D::LocalGIVolume3D() {
	set_disable_scale(true);
}

LocalGIVolume3D::~LocalGIVolume3D() {
	if (debug_mesh_instance) {
		debug_mesh_instance = nullptr;
	}
}
