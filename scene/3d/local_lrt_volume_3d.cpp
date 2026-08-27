/**************************************************************************/
/*  local_lrt_volume_3d.cpp                                               */
/**************************************************************************/

#include "local_lrt_volume_3d.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

void LocalLRTVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LocalLRTVolume3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LocalLRTVolume3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LocalLRTVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LocalLRTVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "probe_spacing"), &LocalLRTVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LocalLRTVolume3D::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_resolution"), &LocalLRTVolume3D::get_resolution);
	ClassDB::bind_method(D_METHOD("get_actual_probe_spacing"), &LocalLRTVolume3D::get_actual_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_position", "grid_position"), &LocalLRTVolume3D::get_probe_position);
	ClassDB::bind_method(D_METHOD("set_propagation_iterations", "iterations"), &LocalLRTVolume3D::set_propagation_iterations);
	ClassDB::bind_method(D_METHOD("get_propagation_iterations"), &LocalLRTVolume3D::get_propagation_iterations);
	ClassDB::bind_method(D_METHOD("set_energy", "energy"), &LocalLRTVolume3D::set_energy);
	ClassDB::bind_method(D_METHOD("get_energy"), &LocalLRTVolume3D::get_energy);
	ClassDB::bind_method(D_METHOD("set_edge_blend_distance", "distance"), &LocalLRTVolume3D::set_edge_blend_distance);
	ClassDB::bind_method(D_METHOD("get_edge_blend_distance"), &LocalLRTVolume3D::get_edge_blend_distance);
	ClassDB::bind_method(D_METHOD("set_debug_draw", "enabled"), &LocalLRTVolume3D::set_debug_draw);
	ClassDB::bind_method(D_METHOD("is_debug_draw_enabled"), &LocalLRTVolume3D::is_debug_draw_enabled);
	ClassDB::bind_method(D_METHOD("set_debug_probe_scale", "scale"), &LocalLRTVolume3D::set_debug_probe_scale);
	ClassDB::bind_method(D_METHOD("get_debug_probe_scale"), &LocalLRTVolume3D::get_debug_probe_scale);
	ClassDB::bind_method(D_METHOD("get_bounds"), &LocalLRTVolume3D::get_bounds);
	ClassDB::bind_method(D_METHOD("get_rid"), &LocalLRTVolume3D::get_rid);
	ClassDB::bind_method(D_METHOD("rebuild"), &LocalLRTVolume3D::rebuild);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.01,64,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "resolution", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_iterations", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_propagation_iterations", "get_propagation_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_blend_distance", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_edge_blend_distance", "get_edge_blend_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_draw"), "set_debug_draw", "is_debug_draw_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_probe_scale", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater,suffix:m"), "set_debug_probe_scale", "get_debug_probe_scale");
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		RS::get_singleton()->local_lrt_volume_set_transform(volume, get_global_transform());
	}
}

Vector3i LocalLRTVolume3D::_calculate_resolution() const {
	return Vector3i(
			MAX(2, (int)Math::ceil(size.x / probe_spacing) + 1),
			MAX(2, (int)Math::ceil(size.y / probe_spacing) + 1),
			MAX(2, (int)Math::ceil(size.z / probe_spacing) + 1));
}

void LocalLRTVolume3D::_sync_grid() {
	RS::get_singleton()->local_lrt_volume_set_grid(volume, size, get_resolution());
	update_gizmos();
	notify_property_list_changed();
}

void LocalLRTVolume3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	RS::get_singleton()->local_lrt_volume_set_enabled(volume, enabled);
}

bool LocalLRTVolume3D::is_enabled() const {
	return enabled;
}

void LocalLRTVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.01);
	_sync_grid();
}

Vector3 LocalLRTVolume3D::get_size() const {
	return size;
}

void LocalLRTVolume3D::set_probe_spacing(float p_spacing) {
	probe_spacing = MAX(p_spacing, 0.01f);
	_sync_grid();
}

float LocalLRTVolume3D::get_probe_spacing() const {
	return probe_spacing;
}

Vector3i LocalLRTVolume3D::get_resolution() const {
	return _calculate_resolution();
}

Vector3 LocalLRTVolume3D::get_actual_probe_spacing() const {
	const Vector3i resolution = get_resolution();
	return size / Vector3(resolution - Vector3i(1, 1, 1));
}

Vector3 LocalLRTVolume3D::get_probe_position(const Vector3i &p_grid_position) const {
	const Vector3i resolution = get_resolution();
	ERR_FAIL_COND_V(p_grid_position.x < 0 || p_grid_position.y < 0 || p_grid_position.z < 0, Vector3());
	ERR_FAIL_COND_V(p_grid_position.x >= resolution.x || p_grid_position.y >= resolution.y || p_grid_position.z >= resolution.z, Vector3());
	return -size * 0.5 + Vector3(p_grid_position) * get_actual_probe_spacing();
}

void LocalLRTVolume3D::set_propagation_iterations(int p_iterations) {
	propagation_iterations = MAX(p_iterations, 1);
	RS::get_singleton()->local_lrt_volume_set_propagation_iterations(volume, propagation_iterations);
}

int LocalLRTVolume3D::get_propagation_iterations() const {
	return propagation_iterations;
}

void LocalLRTVolume3D::set_energy(float p_energy) {
	energy = MAX(p_energy, 0.0f);
	RS::get_singleton()->local_lrt_volume_set_energy(volume, energy);
}

float LocalLRTVolume3D::get_energy() const {
	return energy;
}

void LocalLRTVolume3D::set_edge_blend_distance(float p_distance) {
	edge_blend_distance = MAX(p_distance, 0.0f);
	RS::get_singleton()->local_lrt_volume_set_edge_blend_distance(volume, edge_blend_distance);
}

float LocalLRTVolume3D::get_edge_blend_distance() const {
	return edge_blend_distance;
}

void LocalLRTVolume3D::set_debug_draw(bool p_enabled) {
	debug_draw = p_enabled;
	update_gizmos();
}

bool LocalLRTVolume3D::is_debug_draw_enabled() const {
	return debug_draw;
}

void LocalLRTVolume3D::set_debug_probe_scale(float p_scale) {
	debug_probe_scale = MAX(p_scale, 0.01f);
	update_gizmos();
}

float LocalLRTVolume3D::get_debug_probe_scale() const {
	return debug_probe_scale;
}

AABB LocalLRTVolume3D::get_bounds() const {
	return AABB(-size * 0.5, size);
}

RID LocalLRTVolume3D::get_rid() const {
	return volume;
}

void LocalLRTVolume3D::rebuild() {
	_sync_grid();
}

LocalLRTVolume3D::LocalLRTVolume3D() {
	volume = RS::get_singleton()->local_lrt_volume_create();
	set_notify_transform(true);
	set_disable_scale(true);
	RS::get_singleton()->local_lrt_volume_set_enabled(volume, enabled);
	RS::get_singleton()->local_lrt_volume_set_propagation_iterations(volume, propagation_iterations);
	RS::get_singleton()->local_lrt_volume_set_energy(volume, energy);
	RS::get_singleton()->local_lrt_volume_set_edge_blend_distance(volume, edge_blend_distance);
	_sync_grid();
}

LocalLRTVolume3D::~LocalLRTVolume3D() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(volume);
}
