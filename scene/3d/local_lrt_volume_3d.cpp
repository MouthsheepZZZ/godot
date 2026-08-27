/**************************************************************************/
/*  local_lrt_volume_3d.cpp                                               */
/**************************************************************************/

#include "local_lrt_volume_3d.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
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
	ClassDB::bind_method(D_METHOD("set_debug_mode", "mode"), &LocalLRTVolume3D::set_debug_mode);
	ClassDB::bind_method(D_METHOD("get_debug_mode"), &LocalLRTVolume3D::get_debug_mode);
	ClassDB::bind_method(D_METHOD("set_debug_probe_scale", "scale"), &LocalLRTVolume3D::set_debug_probe_scale);
	ClassDB::bind_method(D_METHOD("get_debug_probe_scale"), &LocalLRTVolume3D::get_debug_probe_scale);
	ClassDB::bind_method(D_METHOD("get_bounds"), &LocalLRTVolume3D::get_bounds);
	ClassDB::bind_method(D_METHOD("get_rid"), &LocalLRTVolume3D::get_rid);
	ClassDB::bind_method(D_METHOD("has_built_data"), &LocalLRTVolume3D::has_built_data);
	ClassDB::bind_method(D_METHOD("get_built_geometry_count"), &LocalLRTVolume3D::get_built_geometry_count);
	ClassDB::bind_method(D_METHOD("is_probe_occupied", "grid_position"), &LocalLRTVolume3D::is_probe_occupied);
	ClassDB::bind_method(D_METHOD("get_probe_albedo", "grid_position"), &LocalLRTVolume3D::get_probe_albedo);
	ClassDB::bind_method(D_METHOD("get_probe_emission", "grid_position"), &LocalLRTVolume3D::get_probe_emission);
	ClassDB::bind_method(D_METHOD("get_probe_local_visibility", "grid_position"), &LocalLRTVolume3D::get_probe_local_visibility);
	ClassDB::bind_method(D_METHOD("get_probe_transfer_color", "grid_position"), &LocalLRTVolume3D::get_probe_transfer_color);
	ClassDB::bind_method(D_METHOD("get_probe_global_visibility", "grid_position"), &LocalLRTVolume3D::get_probe_global_visibility);
	ClassDB::bind_method(D_METHOD("has_gpu_data"), &LocalLRTVolume3D::has_gpu_data);
	ClassDB::bind_method(D_METHOD("rebuild"), &LocalLRTVolume3D::rebuild);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.01,64,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "resolution", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_iterations", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_propagation_iterations", "get_propagation_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_blend_distance", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_edge_blend_distance", "get_edge_blend_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_draw"), "set_debug_draw", "is_debug_draw_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Occupancy,Local Visibility,Local Transfer,Global Visibility"), "set_debug_mode", "get_debug_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_probe_scale", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater,suffix:m"), "set_debug_probe_scale", "get_debug_probe_scale");

	BIND_ENUM_CONSTANT(DEBUG_MODE_OCCUPANCY);
	BIND_ENUM_CONSTANT(DEBUG_MODE_LOCAL_VISIBILITY);
	BIND_ENUM_CONSTANT(DEBUG_MODE_LOCAL_TRANSFER);
	BIND_ENUM_CONSTANT(DEBUG_MODE_GLOBAL_VISIBILITY);
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		const Transform3D global_transform = is_inside_tree() ? get_global_transform() : get_transform();
		RS::get_singleton()->local_lrt_volume_set_transform(volume, global_transform);
		if (builder) {
			builder->set_transform(global_transform);
		}
	} else if (p_what == NOTIFICATION_READY) {
		rebuild();
	}
}

Vector3i LocalLRTVolume3D::_calculate_resolution() const {
	return Vector3i(
			MAX(2, (int)Math::ceil(size.x / probe_spacing) + 1),
			MAX(2, (int)Math::ceil(size.y / probe_spacing) + 1),
			MAX(2, (int)Math::ceil(size.z / probe_spacing) + 1));
}

bool LocalLRTVolume3D::_is_valid_probe_position(const Vector3i &p_grid_position) const {
	const Vector3i resolution = get_resolution();
	return p_grid_position.x >= 0 && p_grid_position.y >= 0 && p_grid_position.z >= 0 &&
			p_grid_position.x < resolution.x && p_grid_position.y < resolution.y && p_grid_position.z < resolution.z;
}

void LocalLRTVolume3D::_sync_grid() {
	RS::get_singleton()->local_lrt_volume_set_grid(volume, size, get_resolution());
	_clear_built_data();
	update_gizmos();
	notify_property_list_changed();
}

void LocalLRTVolume3D::_clear_built_data() {
	if (builder) {
		memdelete(builder);
		builder = nullptr;
	}
	global_visibility.clear();
	built_geometry_count = 0;
}

void LocalLRTVolume3D::_collect_static_geometry(Node *p_node, const Transform3D &p_world_to_volume) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh_instance && mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && (!mesh_instance->is_inside_tree() || mesh_instance->is_visible_in_tree())) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			const Transform3D mesh_transform = mesh_instance->is_inside_tree() ? mesh_instance->get_global_transform() : mesh_instance->get_transform();
			const Transform3D mesh_to_volume = p_world_to_volume * mesh_transform;
			if (get_bounds().intersects(mesh_to_volume.xform(mesh->get_aabb()))) {
				built_geometry_count++;
				for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
					if (mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
						continue;
					}

					Color albedo(1.0, 1.0, 1.0);
					Color emission;
					const Ref<Material> material = mesh_instance->get_active_material(surface);
					const Ref<BaseMaterial3D> base_material = material;
					if (base_material.is_valid()) {
						albedo = base_material->get_albedo();
						if (base_material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
							emission = base_material->get_emission();
							const float emission_energy = base_material->get_emission_energy_multiplier();
							emission.r *= emission_energy;
							emission.g *= emission_energy;
							emission.b *= emission_energy;
						}
					}

					const Array arrays = mesh->surface_get_arrays(surface);
					if (arrays.is_empty()) {
						continue;
					}
					const Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
					const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
					const int triangle_vertex_count = indices.is_empty() ? vertices.size() : indices.size();
					for (int index = 0; index + 2 < triangle_vertex_count; index += 3) {
						Vector3 triangle[3];
						for (int vertex = 0; vertex < 3; vertex++) {
							const int vertex_index = indices.is_empty() ? index + vertex : indices[index + vertex];
							triangle[vertex] = mesh_to_volume.xform(vertices[vertex_index]);
						}
						builder->rasterize_triangle(triangle[0], triangle[1], triangle[2], albedo, emission);
					}
				}
			}
		}
	}

	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_static_geometry(p_node->get_child(child), p_world_to_volume);
	}
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
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector3());
	return -size * 0.5 + Vector3(p_grid_position) * get_actual_probe_spacing();
}

void LocalLRTVolume3D::set_propagation_iterations(int p_iterations) {
	propagation_iterations = MAX(p_iterations, 1);
	RS::get_singleton()->local_lrt_volume_set_propagation_iterations(volume, propagation_iterations);
	if (builder) {
		global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
		update_gizmos();
	}
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

void LocalLRTVolume3D::set_debug_mode(DebugMode p_mode) {
	ERR_FAIL_INDEX(p_mode, DEBUG_MODE_GLOBAL_VISIBILITY + 1);
	debug_mode = p_mode;
	update_gizmos();
}

LocalLRTVolume3D::DebugMode LocalLRTVolume3D::get_debug_mode() const {
	return debug_mode;
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

bool LocalLRTVolume3D::has_built_data() const {
	return builder != nullptr;
}

int LocalLRTVolume3D::get_built_geometry_count() const {
	return built_geometry_count;
}

bool LocalLRTVolume3D::is_probe_occupied(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, false);
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), false);
	return builder->get_probe(p_grid_position).occupied;
}

Color LocalLRTVolume3D::get_probe_albedo(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Color());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Color());
	return builder->get_probe(p_grid_position).albedo;
}

Color LocalLRTVolume3D::get_probe_emission(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Color());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Color());
	return builder->get_probe(p_grid_position).emission;
}

Vector4 LocalLRTVolume3D::get_probe_local_visibility(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	return builder->get_probe(p_grid_position).local_visibility;
}

Color LocalLRTVolume3D::get_probe_transfer_color(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Color());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Color());
	const LocalLRTBuilder::TransferRGB &transfer = builder->get_probe(p_grid_position).local_transfer;
	Color color;
	for (int coefficient = 0; coefficient < 4; coefficient++) {
		color.r += transfer.r.rows[coefficient][coefficient] * 0.25;
		color.g += transfer.g.rows[coefficient][coefficient] * 0.25;
		color.b += transfer.b.rows[coefficient][coefficient] * 0.25;
	}
	return color;
}

Vector4 LocalLRTVolume3D::get_probe_global_visibility(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_COND_V(global_visibility.size() != builder->get_probe_count(), Vector4());
	return global_visibility[LocalLRTMath::probe_index(p_grid_position, get_resolution())];
}

bool LocalLRTVolume3D::has_gpu_data() const {
	return builder && global_visibility.size() == builder->get_probe_count();
}

void LocalLRTVolume3D::rebuild() {
	_clear_built_data();
	const Transform3D volume_transform = is_inside_tree() ? get_global_transform() : get_transform();
	builder = memnew(LocalLRTBuilder(size, get_resolution(), volume_transform));
	Node *root = get_parent();
	if (is_inside_tree() && get_tree()->get_current_scene()) {
		root = get_tree()->get_current_scene();
	}
	if (root) {
		_collect_static_geometry(root, volume_transform.affine_inverse());
	}
	builder->build_local_data();

	Vector<Vector4> local_visibility;
	Vector<Vector4> local_transfer;
	local_visibility.resize(builder->get_probe_count());
	local_transfer.resize(builder->get_probe_count() * 12);
	for (int z = 0; z < get_resolution().z; z++) {
		for (int y = 0; y < get_resolution().y; y++) {
			for (int x = 0; x < get_resolution().x; x++) {
				const Vector3i position(x, y, z);
				const int probe_index = LocalLRTMath::probe_index(position, get_resolution());
				const LocalLRTBuilder::Probe &probe = builder->get_probe(position);
				local_visibility.write[probe_index] = probe.local_visibility;
				const LocalLRTMath::SH2Matrix *channels[] = { &probe.local_transfer.r, &probe.local_transfer.g, &probe.local_transfer.b };
				for (int channel = 0; channel < 3; channel++) {
					for (int row = 0; row < 4; row++) {
						local_transfer.write[probe_index * 12 + channel * 4 + row] = channels[channel]->rows[row];
					}
				}
			}
		}
	}
	RS::get_singleton()->local_lrt_volume_set_static_data(volume, local_visibility, local_transfer);
	global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
	update_gizmos();
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
	_clear_built_data();
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(volume);
}
