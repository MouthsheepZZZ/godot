/**************************************************************************/
/*  local_lrt_volume_3d.cpp                                               */
/**************************************************************************/

#include "local_lrt_volume_3d.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/primitive_meshes.h"
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
	ClassDB::bind_method(D_METHOD("set_geometry_voxel_size", "voxel_size"), &LocalLRTVolume3D::set_geometry_voxel_size);
	ClassDB::bind_method(D_METHOD("get_geometry_voxel_size"), &LocalLRTVolume3D::get_geometry_voxel_size);
	ClassDB::bind_method(D_METHOD("get_resolution"), &LocalLRTVolume3D::get_resolution);
	ClassDB::bind_method(D_METHOD("get_actual_probe_spacing"), &LocalLRTVolume3D::get_actual_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_position", "grid_position"), &LocalLRTVolume3D::get_probe_position);
	ClassDB::bind_method(D_METHOD("set_visibility_iterations", "iterations"), &LocalLRTVolume3D::set_visibility_iterations);
	ClassDB::bind_method(D_METHOD("get_visibility_iterations"), &LocalLRTVolume3D::get_visibility_iterations);
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
	ClassDB::bind_method(D_METHOD("is_probe_inside_solid", "grid_position"), &LocalLRTVolume3D::is_probe_inside_solid);
	ClassDB::bind_method(D_METHOD("get_probe_signed_distance", "grid_position"), &LocalLRTVolume3D::get_probe_signed_distance);
	ClassDB::bind_method(D_METHOD("get_probe_coverage", "grid_position"), &LocalLRTVolume3D::get_probe_coverage);
	ClassDB::bind_method(D_METHOD("get_probe_surface_normal", "grid_position"), &LocalLRTVolume3D::get_probe_surface_normal);
	ClassDB::bind_method(D_METHOD("get_probe_albedo", "grid_position"), &LocalLRTVolume3D::get_probe_albedo);
	ClassDB::bind_method(D_METHOD("get_probe_emission", "grid_position"), &LocalLRTVolume3D::get_probe_emission);
	ClassDB::bind_method(D_METHOD("get_probe_local_visibility", "grid_position"), &LocalLRTVolume3D::get_probe_local_visibility);
	ClassDB::bind_method(D_METHOD("get_probe_transfer_color", "grid_position"), &LocalLRTVolume3D::get_probe_transfer_color);
	ClassDB::bind_method(D_METHOD("get_probe_global_visibility", "grid_position"), &LocalLRTVolume3D::get_probe_global_visibility);
	ClassDB::bind_method(D_METHOD("get_probe_injection", "grid_position", "channel"), &LocalLRTVolume3D::get_probe_injection);
	ClassDB::bind_method(D_METHOD("get_probe_shadowed_injection", "grid_position", "channel"), &LocalLRTVolume3D::get_probe_shadowed_injection);
	ClassDB::bind_method(D_METHOD("get_probe_injection_color", "grid_position"), &LocalLRTVolume3D::get_probe_injection_color);
	ClassDB::bind_method(D_METHOD("get_probe_radiance", "grid_position", "channel"), &LocalLRTVolume3D::get_probe_radiance);
	ClassDB::bind_method(D_METHOD("get_probe_radiance_color", "grid_position"), &LocalLRTVolume3D::get_probe_radiance_color);
	ClassDB::bind_method(D_METHOD("get_probe_shadow_visibility", "grid_position"), &LocalLRTVolume3D::get_probe_shadow_visibility);
	ClassDB::bind_method(D_METHOD("has_gpu_data"), &LocalLRTVolume3D::has_gpu_data);
	ClassDB::bind_method(D_METHOD("update_light_injection"), &LocalLRTVolume3D::update_light_injection);
	ClassDB::bind_method(D_METHOD("rebuild"), &LocalLRTVolume3D::rebuild);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.01,64,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "geometry_voxel_size", PROPERTY_HINT_RANGE, "0.01,4,0.001,or_greater,suffix:m"), "set_geometry_voxel_size", "get_geometry_voxel_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "resolution", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_iterations", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_visibility_iterations", "get_visibility_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_iterations", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_propagation_iterations", "get_propagation_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_blend_distance", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_edge_blend_distance", "get_edge_blend_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_draw"), "set_debug_draw", "is_debug_draw_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Occupancy,Local Visibility,Local Transfer,Global Visibility,Injection,Radiance,Geometry Distance,Geometry Coverage,Inside Solid,Directional Shadow,Omni Shadow,Shadowed Injection"), "set_debug_mode", "get_debug_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_probe_scale", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater,suffix:m"), "set_debug_probe_scale", "get_debug_probe_scale");

	BIND_ENUM_CONSTANT(DEBUG_MODE_OCCUPANCY);
	BIND_ENUM_CONSTANT(DEBUG_MODE_LOCAL_VISIBILITY);
	BIND_ENUM_CONSTANT(DEBUG_MODE_LOCAL_TRANSFER);
	BIND_ENUM_CONSTANT(DEBUG_MODE_GLOBAL_VISIBILITY);
	BIND_ENUM_CONSTANT(DEBUG_MODE_INJECTION);
	BIND_ENUM_CONSTANT(DEBUG_MODE_RADIANCE);
	BIND_ENUM_CONSTANT(DEBUG_MODE_GEOMETRY_DISTANCE);
	BIND_ENUM_CONSTANT(DEBUG_MODE_GEOMETRY_COVERAGE);
	BIND_ENUM_CONSTANT(DEBUG_MODE_INSIDE_SOLID);
	BIND_ENUM_CONSTANT(DEBUG_MODE_DIRECTIONAL_SHADOW);
	BIND_ENUM_CONSTANT(DEBUG_MODE_OMNI_SHADOW);
	BIND_ENUM_CONSTANT(DEBUG_MODE_SHADOWED_INJECTION);
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		const Transform3D global_transform = is_inside_tree() ? get_global_transform() : get_transform();
		RS::get_singleton()->local_lrt_volume_set_transform(volume, global_transform);
		if (builder) {
			builder->set_transform(global_transform);
		}
	} else if (p_what == NOTIFICATION_READY) {
		_ensure_debug_probe_instance();
		rebuild();
	} else if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
		update_light_injection();
		if (builder && enabled && debug_draw) {
			if (debug_mode == DEBUG_MODE_DIRECTIONAL_SHADOW || debug_mode == DEBUG_MODE_OMNI_SHADOW) {
				shadow_visibility = RS::get_singleton()->local_lrt_volume_get_shadow_visibility(volume);
				if (shadow_visibility.size() == builder->get_probe_count()) {
					_update_debug_probe_instances();
				}
			} else if (debug_mode == DEBUG_MODE_INJECTION) {
				_update_debug_probe_instances();
			} else if (debug_mode == DEBUG_MODE_SHADOWED_INJECTION) {
				shadowed_injection = RS::get_singleton()->local_lrt_volume_get_injection(volume);
				if (shadowed_injection.size() == builder->get_probe_count() * 3) {
					_update_debug_probe_instances();
				}
			} else if (debug_mode == DEBUG_MODE_RADIANCE) {
				radiance = RS::get_singleton()->local_lrt_volume_get_radiance(volume);
				_update_debug_probe_instances();
			}
		}
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
	const bool rebuild_existing_data = builder != nullptr;
	RS::get_singleton()->local_lrt_volume_set_grid(volume, size, get_resolution());
	if (rebuild_existing_data) {
		rebuild();
	} else {
		_clear_built_data();
		_update_debug_probe_instances();
		update_gizmos();
	}
	notify_property_list_changed();
}

void LocalLRTVolume3D::_clear_built_data() {
	if (builder) {
		memdelete(builder);
		builder = nullptr;
	}
	global_visibility.clear();
	injection.clear();
	shadowed_injection.clear();
	shadow_visibility.clear();
	emissive_injection.clear();
	radiance.clear();
	built_geometry_count = 0;
}

AABB LocalLRTVolume3D::_get_collection_bounds() const {
	const Vector3 spacing = get_actual_probe_spacing();
	AABB bounds = get_bounds();
	bounds.position -= spacing;
	bounds.size += spacing * 2.0;
	return bounds;
}

static void local_lrt_extract_surface_color(MeshInstance3D *p_mesh_instance, int p_surface, Color &r_albedo, Color &r_emission) {
	r_albedo = Color(1.0, 1.0, 1.0);
	r_emission = Color();
	const Ref<Material> material = p_mesh_instance->get_active_material(p_surface);
	const Ref<BaseMaterial3D> base_material = material;
	if (base_material.is_null()) {
		return;
	}
	r_albedo = base_material->get_albedo();
	if (!base_material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
		return;
	}
	r_emission = base_material->get_emission();
	const float emission_energy = base_material->get_emission_energy_multiplier();
	r_emission.r *= emission_energy;
	r_emission.g *= emission_energy;
	r_emission.b *= emission_energy;
}

static LocalLRTColorSDF local_lrt_make_mesh_sdf(const Ref<Mesh> &p_mesh, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission) {
	if (Object::cast_to<PlaneMesh>(p_mesh.ptr())) {
		return LocalLRTColorSDF();
	}
	if (const BoxMesh *box = Object::cast_to<BoxMesh>(p_mesh.ptr())) {
		return LocalLRTColorSDF::make_box(box->get_size() * 0.5, p_voxel_size, p_albedo, p_emission);
	}
	if (const SphereMesh *sphere = Object::cast_to<SphereMesh>(p_mesh.ptr())) {
		if (!sphere->get_is_hemisphere()) {
			return LocalLRTColorSDF::make_sphere(sphere->get_radius(), p_voxel_size, p_albedo, p_emission);
		}
	}
	return LocalLRTColorSDF::from_mesh(p_mesh, p_voxel_size, p_albedo, p_emission);
}

void LocalLRTVolume3D::_collect_static_geometry(Node *p_node, const Transform3D &p_world_to_volume) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh_instance && mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && (!mesh_instance->is_inside_tree() || mesh_instance->is_visible_in_tree())) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			const Transform3D mesh_transform = mesh_instance->is_inside_tree() ? mesh_instance->get_global_transform() : mesh_instance->get_transform();
			const Transform3D mesh_to_volume = p_world_to_volume * mesh_transform;
			if (_get_collection_bounds().intersects(mesh_to_volume.xform(mesh->get_aabb()))) {
				Color albedo;
				Color emission;
				local_lrt_extract_surface_color(mesh_instance, 0, albedo, emission);
				const LocalLRTColorSDF sdf = local_lrt_make_mesh_sdf(mesh, geometry_voxel_size, albedo, emission);
				if (!sdf.is_empty()) {
					built_geometry_count++;
					builder->add_geometry_source(sdf, mesh_to_volume);
				}
			}
		}
	}

	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_static_geometry(p_node->get_child(child), p_world_to_volume);
	}
}

static void local_lrt_pack_analytic_light(Vector<Vector4> &r_lights, int p_type, const Color &p_color, real_t p_energy, const Vector3 &p_vector, real_t p_range = 0.0, real_t p_attenuation = 1.0, const Vector3 &p_spot_direction = Vector3(), real_t p_cone_limit = 0.0, real_t p_shadow = 0.0) {
	r_lights.push_back(Vector4((real_t)p_type, p_energy, p_range, p_cone_limit));
	r_lights.push_back(Vector4(p_color.r, p_color.g, p_color.b, p_shadow));
	r_lights.push_back(Vector4(p_vector.x, p_vector.y, p_vector.z, p_attenuation));
	r_lights.push_back(Vector4(p_spot_direction.x, p_spot_direction.y, p_spot_direction.z, 0.0));
	r_lights.push_back(Vector4(1.0, 0.0, 0.0, 0.0));
	r_lights.push_back(Vector4(0.0, 1.0, 0.0, 0.0));
	r_lights.push_back(Vector4(0.0, 0.0, 1.0, 0.0));
	r_lights.push_back(Vector4());
	r_lights.push_back(Vector4());
}

void LocalLRTVolume3D::_collect_light_injection(Node *p_node, Vector<Vector4> &r_lights) {
	Light3D *light = Object::cast_to<Light3D>(p_node);
	if (light && light->is_visible() && (!light->is_inside_tree() || light->is_visible_in_tree())) {
		const Transform3D light_transform = light->is_inside_tree() ? light->get_global_transform() : light->get_transform();
		const Color color = light->get_color().srgb_to_linear();
		const real_t light_energy = light->get_param(Light3D::PARAM_ENERGY) * light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
		if (DirectionalLight3D *directional = Object::cast_to<DirectionalLight3D>(light)) {
			if (directional->get_sky_mode() != DirectionalLight3D::SKY_MODE_SKY_ONLY) {
				LocalLRTBuilder::DirectionalLight source;
				source.direction_to_light = light_transform.basis.get_column(Vector3::AXIS_Z).normalized();
				source.color = color;
				source.energy = light_energy;
				builder->inject_directional_light(source);
				local_lrt_pack_analytic_light(r_lights, 1, source.color, source.energy, source.direction_to_light, 0.0, 1.0, Vector3(), 0.0, directional->has_shadow() ? 1.0 : 0.0);
			}
		} else if (Object::cast_to<OmniLight3D>(light)) {
			LocalLRTBuilder::OmniLight source;
			source.position = light_transform.origin;
			source.color = color;
			source.energy = light_energy;
			source.range = light->get_param(Light3D::PARAM_RANGE);
			source.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
			builder->inject_omni_light(source);
			local_lrt_pack_analytic_light(r_lights, 2, source.color, source.energy, source.position, source.range, source.attenuation);
		} else if (Object::cast_to<SpotLight3D>(light)) {
			LocalLRTBuilder::SpotLight source;
			source.position = light_transform.origin;
			source.direction = -light_transform.basis.get_column(Vector3::AXIS_Z).normalized();
			source.color = color;
			source.energy = light_energy;
			source.range = light->get_param(Light3D::PARAM_RANGE);
			source.angle = Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE));
			builder->inject_spot_light(source);
			local_lrt_pack_analytic_light(r_lights, 3, source.color, source.energy, source.position, source.range, light->get_param(Light3D::PARAM_ATTENUATION), source.direction, Math::cos(source.angle));
		}
	}

	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_light_injection(p_node->get_child(child), r_lights);
	}
}

void LocalLRTVolume3D::_sync_global_visibility_to_builder() {
	if (!builder || global_visibility.size() != builder->get_probe_count()) {
		return;
	}
	for (int index = 0; index < global_visibility.size(); index++) {
		builder->get_probe(LocalLRTMath::probe_position(index, get_resolution())).global_visibility = global_visibility[index];
	}
}

void LocalLRTVolume3D::_ensure_debug_probe_instance() {
	if (debug_probe_instance) {
		return;
	}

	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(1.0);
	sphere->set_height(2.0);
	sphere->set_radial_segments(8);
	sphere->set_rings(4);

	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(R"(
shader_type spatial;
render_mode unshaded, cull_disabled;

varying vec3 probe_normal;
varying vec4 probe_color;
varying vec4 probe_sh;

void vertex() {
	probe_normal = NORMAL;
	probe_color = COLOR;
	probe_sh = INSTANCE_CUSTOM;
}

void fragment() {
	float modulation = 1.0;
	if (dot(probe_sh, probe_sh) > 0.000001) {
		vec3 direction = normalize(probe_normal);
		vec4 basis = vec4(0.2820947918, 0.4886025119 * direction);
		modulation = clamp(0.2 + 1.6 * dot(normalize(probe_sh), basis), 0.08, 1.0);
	}
	ALBEDO = probe_color.rgb * modulation;
	ALPHA = probe_color.a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)");
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(shader);

	debug_probe_multimesh.instantiate();
	debug_probe_multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	debug_probe_multimesh->set_use_colors(true);
	debug_probe_multimesh->set_use_custom_data(true);
	debug_probe_multimesh->set_mesh(sphere);

	debug_probe_instance = memnew(MultiMeshInstance3D);
	debug_probe_instance->set_name("DebugProbes");
	debug_probe_instance->set_multimesh(debug_probe_multimesh);
	debug_probe_instance->set_material_override(material);
	debug_probe_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	add_child(debug_probe_instance, false, INTERNAL_MODE_BACK);
}

void LocalLRTVolume3D::_update_debug_probe_instances() {
	if (!debug_probe_instance) {
		return;
	}
	debug_probe_instance->set_visible(debug_draw);
	if (!debug_draw || !builder) {
		if (debug_probe_multimesh->get_instance_count() != 0) {
			debug_probe_multimesh->set_instance_count(0);
		}
		return;
	}

	const Vector3i resolution = get_resolution();
	const int probe_count = builder->get_probe_count();
	if ((debug_mode == DEBUG_MODE_DIRECTIONAL_SHADOW || debug_mode == DEBUG_MODE_OMNI_SHADOW) && shadow_visibility.size() != probe_count) {
		debug_probe_multimesh->set_instance_count(0);
		return;
	}
	if (debug_mode == DEBUG_MODE_SHADOWED_INJECTION && shadowed_injection.size() != probe_count * 3) {
		debug_probe_multimesh->set_instance_count(0);
		return;
	}
	if (debug_probe_multimesh->get_instance_count() != probe_count) {
		debug_probe_multimesh->set_instance_count(probe_count);
	}
	const Vector3 spacing = get_actual_probe_spacing();
	const float probe_radius = MIN(debug_probe_scale, MIN(spacing.x, MIN(spacing.y, spacing.z)) * 0.35f);
	const Transform3D probe_scale_transform(Basis().scaled(Vector3(probe_radius, probe_radius, probe_radius)));
	const float fully_visible_constant = LocalLRTMath::encode_constant(1.0).x;
	for (int index = 0; index < probe_count; index++) {
		const Vector3i position = LocalLRTMath::probe_position(index, resolution);
		const LocalLRTBuilder::Probe &probe = builder->get_probe(position);
		Color color(1.0, 0.75, 0.2, 0.65);
		if (debug_mode == DEBUG_MODE_INSIDE_SOLID) {
			color = probe.inside_solid ? Color(1.0, 0.2, 0.8, 0.85) : Color(0.2, 0.55, 1.0, 0.12);
		} else if (debug_mode == DEBUG_MODE_GEOMETRY_COVERAGE || debug_mode == DEBUG_MODE_OCCUPANCY) {
			const float coverage = CLAMP((float)probe.coverage, 0.0f, 1.0f);
			color = Color(coverage, coverage * 0.35f, 1.0f - coverage, 0.2f + 0.7f * coverage);
		} else if (debug_mode == DEBUG_MODE_GEOMETRY_DISTANCE) {
			const float t = CLAMP((float)(0.5 - probe.signed_distance / MAX(geometry_voxel_size, 0.001f)), 0.0f, 1.0f);
			color = probe.inside_solid ? Color(1.0, 0.15f * (1.0f - t), 0.2, 0.8) : Color(0.2, 0.45 + 0.4 * t, 1.0, 0.2 + 0.5 * t);
		} else if (debug_mode == DEBUG_MODE_LOCAL_VISIBILITY) {
			const float visibility = CLAMP(get_probe_local_visibility(position).x / fully_visible_constant, 0.0, 1.0);
			color = Color(visibility, visibility, visibility, 0.9);
		} else if (debug_mode == DEBUG_MODE_LOCAL_TRANSFER) {
			color = get_probe_transfer_color(position);
		} else if (debug_mode == DEBUG_MODE_GLOBAL_VISIBILITY) {
			const float visibility = CLAMP(get_probe_global_visibility(position).x / fully_visible_constant, 0.0, 1.0);
			color = Color(visibility, visibility, visibility, 0.9);
		} else if (debug_mode == DEBUG_MODE_INJECTION || debug_mode == DEBUG_MODE_SHADOWED_INJECTION) {
			const Vector4 red = debug_mode == DEBUG_MODE_SHADOWED_INJECTION ? get_probe_shadowed_injection(position, 0) : get_probe_injection(position, 0);
			const Vector4 green = debug_mode == DEBUG_MODE_SHADOWED_INJECTION ? get_probe_shadowed_injection(position, 1) : get_probe_injection(position, 1);
			const Vector4 blue = debug_mode == DEBUG_MODE_SHADOWED_INJECTION ? get_probe_shadowed_injection(position, 2) : get_probe_injection(position, 2);
			const float unit_energy = LocalLRTMath::encode_direction(Vector3(1.0, 0.0, 0.0), 1.0, Math::TAU).length();
			color = Color(red.length(), green.length(), blue.length()) / unit_energy;
		} else if (debug_mode == DEBUG_MODE_DIRECTIONAL_SHADOW || debug_mode == DEBUG_MODE_OMNI_SHADOW) {
			const float visibility = CLAMP((float)get_probe_shadow_visibility(position), 0.0f, 1.0f);
			color = Color(visibility, visibility, visibility, 0.9);
		} else if (debug_mode == DEBUG_MODE_RADIANCE && radiance.size() == probe_count * 3) {
			color = get_probe_radiance_color(position);
		} else if (debug_mode == DEBUG_MODE_RADIANCE) {
			color = Color(0.2, 0.55, 1.0, 0.65);
		}
		if (MAX(color.r, MAX(color.g, color.b)) <= 0.0001) {
			color = Color(0.2, 0.55, 1.0, 0.65);
		} else {
			color.r = CLAMP(color.r, 0.0, 1.0);
			color.g = CLAMP(color.g, 0.0, 1.0);
			color.b = CLAMP(color.b, 0.0, 1.0);
			color.a = MIN(color.a, 0.9f);
		}

		Vector4 directional_sh;
		if (debug_mode == DEBUG_MODE_INJECTION || debug_mode == DEBUG_MODE_SHADOWED_INJECTION) {
			const Vector4 red = debug_mode == DEBUG_MODE_SHADOWED_INJECTION ? get_probe_shadowed_injection(position, 0) : get_probe_injection(position, 0);
			const Vector4 green = debug_mode == DEBUG_MODE_SHADOWED_INJECTION ? get_probe_shadowed_injection(position, 1) : get_probe_injection(position, 1);
			const Vector4 blue = debug_mode == DEBUG_MODE_SHADOWED_INJECTION ? get_probe_shadowed_injection(position, 2) : get_probe_injection(position, 2);
			directional_sh = red * 0.2126 + green * 0.7152 + blue * 0.0722;
		} else if (debug_mode == DEBUG_MODE_RADIANCE && radiance.size() == probe_count * 3) {
			const Vector4 red = get_probe_radiance(position, 0);
			const Vector4 green = get_probe_radiance(position, 1);
			const Vector4 blue = get_probe_radiance(position, 2);
			directional_sh = red * 0.2126 + green * 0.7152 + blue * 0.0722;
		}

		Transform3D probe_transform = probe_scale_transform;
		probe_transform.origin = get_probe_position(position);
		debug_probe_multimesh->set_instance_transform(index, probe_transform);
		debug_probe_multimesh->set_instance_color(index, color);
		debug_probe_multimesh->set_instance_custom_data(index, Color(directional_sh.x, directional_sh.y, directional_sh.z, directional_sh.w));
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

void LocalLRTVolume3D::set_geometry_voxel_size(float p_voxel_size) {
	geometry_voxel_size = MAX(p_voxel_size, 0.001f);
	if (builder) {
		rebuild();
	}
}

float LocalLRTVolume3D::get_geometry_voxel_size() const {
	return geometry_voxel_size;
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

void LocalLRTVolume3D::set_visibility_iterations(int p_iterations) {
	visibility_iterations = MAX(p_iterations, 1);
	RS::get_singleton()->local_lrt_volume_set_visibility_iterations(volume, visibility_iterations);
	if (builder) {
		global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
		_sync_global_visibility_to_builder();
		radiance = RS::get_singleton()->local_lrt_volume_get_radiance(volume);
		_update_debug_probe_instances();
		update_gizmos();
	}
}

int LocalLRTVolume3D::get_visibility_iterations() const {
	return visibility_iterations;
}

void LocalLRTVolume3D::set_propagation_iterations(int p_iterations) {
	propagation_iterations = MAX(p_iterations, 1);
	RS::get_singleton()->local_lrt_volume_set_propagation_iterations(volume, propagation_iterations);
	if (builder) {
		radiance = RS::get_singleton()->local_lrt_volume_get_radiance(volume);
		_update_debug_probe_instances();
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
	_update_debug_probe_instances();
	update_gizmos();
}

bool LocalLRTVolume3D::is_debug_draw_enabled() const {
	return debug_draw;
}

void LocalLRTVolume3D::set_debug_mode(DebugMode p_mode) {
	ERR_FAIL_INDEX(p_mode, DEBUG_MODE_SHADOWED_INJECTION + 1);
	debug_mode = p_mode;
	_update_debug_probe_instances();
	update_gizmos();
}

LocalLRTVolume3D::DebugMode LocalLRTVolume3D::get_debug_mode() const {
	return debug_mode;
}

void LocalLRTVolume3D::set_debug_probe_scale(float p_scale) {
	debug_probe_scale = MAX(p_scale, 0.01f);
	_update_debug_probe_instances();
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
	return is_probe_inside_solid(p_grid_position);
}

bool LocalLRTVolume3D::is_probe_inside_solid(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, false);
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), false);
	return builder->get_probe(p_grid_position).inside_solid;
}

real_t LocalLRTVolume3D::get_probe_signed_distance(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, 1.0e20);
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), 1.0e20);
	return builder->get_probe(p_grid_position).signed_distance;
}

real_t LocalLRTVolume3D::get_probe_coverage(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, 0.0);
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), 0.0);
	return builder->get_probe(p_grid_position).coverage;
}

Vector3 LocalLRTVolume3D::get_probe_surface_normal(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Vector3());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector3());
	return builder->get_probe(p_grid_position).surface_normal;
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
	const Vector4 constant_radiance = LocalLRTMath::encode_constant(1.0);
	return Color(
			MAX(transfer.r.xform(constant_radiance).x * LocalLRTMath::SH_Y00, (real_t)0.0),
			MAX(transfer.g.xform(constant_radiance).x * LocalLRTMath::SH_Y00, (real_t)0.0),
			MAX(transfer.b.xform(constant_radiance).x * LocalLRTMath::SH_Y00, (real_t)0.0));
}

Vector4 LocalLRTVolume3D::get_probe_global_visibility(const Vector3i &p_grid_position) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_COND_V(global_visibility.size() != builder->get_probe_count(), Vector4());
	return global_visibility[LocalLRTMath::probe_index(p_grid_position, get_resolution())];
}

Vector4 LocalLRTVolume3D::get_probe_injection(const Vector3i &p_grid_position, int p_channel) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_INDEX_V(p_channel, 3, Vector4());
	ERR_FAIL_COND_V(injection.size() != builder->get_probe_count() * 3, Vector4());
	return injection[LocalLRTMath::probe_index(p_grid_position, get_resolution()) * 3 + p_channel];
}

Vector4 LocalLRTVolume3D::get_probe_shadowed_injection(const Vector3i &p_grid_position, int p_channel) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_INDEX_V(p_channel, 3, Vector4());
	ERR_FAIL_COND_V(shadowed_injection.size() != builder->get_probe_count() * 3, Vector4());
	return shadowed_injection[LocalLRTMath::probe_index(p_grid_position, get_resolution()) * 3 + p_channel];
}

Color LocalLRTVolume3D::get_probe_injection_color(const Vector3i &p_grid_position) const {
	const float unit_energy = LocalLRTMath::encode_direction(Vector3(1.0, 0.0, 0.0), 1.0, Math::TAU).length();
	return Color(
			get_probe_injection(p_grid_position, 0).length() / unit_energy,
			get_probe_injection(p_grid_position, 1).length() / unit_energy,
			get_probe_injection(p_grid_position, 2).length() / unit_energy);
}

Vector4 LocalLRTVolume3D::get_probe_radiance(const Vector3i &p_grid_position, int p_channel) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_INDEX_V(p_channel, 3, Vector4());
	ERR_FAIL_COND_V(radiance.size() != builder->get_probe_count() * 3, Vector4());
	return radiance[LocalLRTMath::probe_index(p_grid_position, get_resolution()) * 3 + p_channel];
}

Color LocalLRTVolume3D::get_probe_radiance_color(const Vector3i &p_grid_position) const {
	const float unit_energy = LocalLRTMath::encode_direction(Vector3(1.0, 0.0, 0.0), 1.0, Math::TAU).length();
	return Color(
			get_probe_radiance(p_grid_position, 0).length() / unit_energy,
			get_probe_radiance(p_grid_position, 1).length() / unit_energy,
			get_probe_radiance(p_grid_position, 2).length() / unit_energy);
}

real_t LocalLRTVolume3D::get_probe_shadow_visibility(const Vector3i &p_grid_position) const {
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), 1.0);
	ERR_FAIL_COND_V(shadow_visibility.size() != get_resolution().x * get_resolution().y * get_resolution().z, 1.0);
	return shadow_visibility[LocalLRTMath::probe_index(p_grid_position, get_resolution())];
}

bool LocalLRTVolume3D::has_gpu_data() const {
	return builder && global_visibility.size() == builder->get_probe_count() &&
			injection.size() == builder->get_probe_count() * 3 &&
			emissive_injection.size() == builder->get_probe_count() * 3 &&
			radiance.size() == builder->get_probe_count() * 3;
}

void LocalLRTVolume3D::update_light_injection() {
	if (!builder) {
		return;
	}

	builder->clear_injection();
	Vector<Vector4> analytic_lights;
	Node *root = get_parent();
	if (is_inside_tree() && get_tree()->get_current_scene()) {
		root = get_tree()->get_current_scene();
	}
	if (root) {
		_collect_light_injection(root, analytic_lights);
	}

	Vector<Vector4> next_injection;
	Vector<Vector4> next_emissive_injection;
	next_injection.resize(builder->get_probe_count() * 3);
	next_emissive_injection.resize(builder->get_probe_count() * 3);
	for (int z = 0; z < get_resolution().z; z++) {
		for (int y = 0; y < get_resolution().y; y++) {
			for (int x = 0; x < get_resolution().x; x++) {
				const Vector3i position(x, y, z);
				const int probe_index = LocalLRTMath::probe_index(position, get_resolution());
				const LocalLRTBuilder::Probe &probe = builder->get_probe(position);
				next_injection.write[probe_index * 3] = probe.injection.r;
				next_injection.write[probe_index * 3 + 1] = probe.injection.g;
				next_injection.write[probe_index * 3 + 2] = probe.injection.b;
				next_emissive_injection.write[probe_index * 3] = probe.emissive_injection.r;
				next_emissive_injection.write[probe_index * 3 + 1] = probe.emissive_injection.g;
				next_emissive_injection.write[probe_index * 3 + 2] = probe.emissive_injection.b;
			}
		}
	}
	if (next_injection == injection && next_emissive_injection == emissive_injection) {
		return;
	}
	injection = next_injection;
	emissive_injection = next_emissive_injection;
	RS::get_singleton()->local_lrt_volume_set_injection(volume, injection, emissive_injection);
	RS::get_singleton()->local_lrt_volume_inject_analytic_lights(volume, analytic_lights);
	if (debug_mode == DEBUG_MODE_INJECTION) {
		_update_debug_probe_instances();
	}
}

void LocalLRTVolume3D::rebuild() {
	_clear_built_data();
	const Transform3D volume_transform = is_inside_tree() ? get_global_transform() : get_transform();
	builder = memnew(LocalLRTBuilder(size, get_resolution(), volume_transform));
	RS::get_singleton()->local_lrt_volume_set_transform(volume, volume_transform);
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
	Vector<int> inside_solid;
	local_visibility.resize(builder->get_probe_count());
	local_transfer.resize(builder->get_probe_count() * 12);
	inside_solid.resize(builder->get_probe_count());
	for (int z = 0; z < get_resolution().z; z++) {
		for (int y = 0; y < get_resolution().y; y++) {
			for (int x = 0; x < get_resolution().x; x++) {
				const Vector3i position(x, y, z);
				const int probe_index = LocalLRTMath::probe_index(position, get_resolution());
				const LocalLRTBuilder::Probe &probe = builder->get_probe(position);
				local_visibility.write[probe_index] = probe.local_visibility;
				inside_solid.write[probe_index] = probe.inside_solid ? 1 : 0;
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
	RS::get_singleton()->local_lrt_volume_set_inside_solid(volume, inside_solid);
	global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
	_sync_global_visibility_to_builder();
	update_light_injection();
	_update_debug_probe_instances();
	update_gizmos();
}

LocalLRTVolume3D::LocalLRTVolume3D() {
	volume = RS::get_singleton()->local_lrt_volume_create();
	set_notify_transform(true);
	set_process_internal(true);
	set_disable_scale(true);
	RS::get_singleton()->local_lrt_volume_set_enabled(volume, enabled);
	RS::get_singleton()->local_lrt_volume_set_visibility_iterations(volume, visibility_iterations);
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
