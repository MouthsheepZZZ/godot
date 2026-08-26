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

#include "core/object/class_db.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"
#include "scene/main/viewport.h"

void LocalGIVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.01);
	update_gizmos();
}

Vector3 LocalGIVolume3D::get_size() const {
	return size;
}

void LocalGIVolume3D::set_probe_spacing(float p_spacing) {
	probe_spacing = MAX(p_spacing, 0.05f);
}

float LocalGIVolume3D::get_probe_spacing() const {
	return probe_spacing;
}

void LocalGIVolume3D::set_rays_per_probe(int p_rays) {
	rays_per_probe = MAX(p_rays, 1);
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
	debug_mode = p_mode;
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

void LocalGIVolume3D::bake(Node *p_from_node) {
	Vector<LocalGITriangle> triangles;
	LocalGIStaticGeometry::collect(_resolve_from_node(p_from_node), LocalGIStaticGeometry::get_composed_transform(this), get_aabb(), triangles);
	static_bvh.build(triangles);
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
