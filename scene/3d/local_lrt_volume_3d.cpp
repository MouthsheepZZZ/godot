/**************************************************************************/
/*  local_lrt_volume_3d.cpp                                               */
/**************************************************************************/

#include "local_lrt_volume_3d.h"

#include "core/config/engine.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/os/time.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_default.h"

#include <cstring>

// Calibrates BaseMaterial3D emission units to the v0 Cycles Cornell reference.
static constexpr float LOCAL_LRT_MESH_LIGHT_ENERGY_SCALE = 2.0f;

void LocalLRTVolumeData::_set_data(const Dictionary &p_data) {
	ERR_FAIL_COND(!p_data.has("version"));
	ERR_FAIL_COND(!p_data.has("size"));
	ERR_FAIL_COND(!p_data.has("resolution"));
	ERR_FAIL_COND(!p_data.has("payload"));
	ERR_FAIL_COND(!p_data.has("checksum"));
	data_format_version = p_data["version"];
	size = p_data["size"];
	resolution = p_data["resolution"];
	payload = p_data["payload"];
	payload_checksum = uint32_t(int64_t(p_data["checksum"]));
	ERR_FAIL_COND_MSG(!_validate_data(), "Invalid Local LRT bake data.");
}

Dictionary LocalLRTVolumeData::_get_data() const {
	Dictionary data;
	data["version"] = data_format_version;
	data["size"] = size;
	data["resolution"] = resolution;
	data["payload"] = payload;
	data["checksum"] = int64_t(payload_checksum);
	return data;
}

Vector3i LocalLRTVolumeData::_get_trunk_resolution() const {
	return Vector3i(
			(resolution.x + TRUNK_PROBE_SIZE - 1) / TRUNK_PROBE_SIZE,
			(resolution.y + TRUNK_PROBE_SIZE - 1) / TRUNK_PROBE_SIZE,
			(resolution.z + TRUNK_PROBE_SIZE - 1) / TRUNK_PROBE_SIZE);
}

void LocalLRTVolumeData::_get_trunk_bounds(int p_trunk_index, Vector3i &r_begin, Vector3i &r_end) const {
	const Vector3i trunk_resolution = _get_trunk_resolution();
	const Vector3i trunk_position(
			p_trunk_index % trunk_resolution.x,
			(p_trunk_index / trunk_resolution.x) % trunk_resolution.y,
			p_trunk_index / (trunk_resolution.x * trunk_resolution.y));
	r_begin = trunk_position * TRUNK_PROBE_SIZE;
	r_end = (r_begin + Vector3i(TRUNK_PROBE_SIZE, TRUNK_PROBE_SIZE, TRUNK_PROBE_SIZE)).min(resolution);
}

int LocalLRTVolumeData::_get_trunk_probe_count(int p_trunk_index) const {
	Vector3i begin;
	Vector3i end;
	_get_trunk_bounds(p_trunk_index, begin, end);
	const Vector3i trunk_size = end - begin;
	return trunk_size.x * trunk_size.y * trunk_size.z;
}

bool LocalLRTVolumeData::_validate_data() const {
	if (data_format_version != DATA_FORMAT_VERSION || !Math::is_finite(size.x) || !Math::is_finite(size.y) || !Math::is_finite(size.z) || size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0) {
		return false;
	}
	if (resolution.x < 2 || resolution.y < 2 || resolution.z < 2 || payload.size() < 4) {
		return false;
	}
	const int64_t probe_count = int64_t(resolution.x) * resolution.y * resolution.z;
	const Vector3i trunk_resolution = _get_trunk_resolution();
	const int64_t trunk_count = int64_t(trunk_resolution.x) * trunk_resolution.y * trunk_resolution.z;
	if (probe_count > INT32_MAX / 12 || trunk_count > INT32_MAX) {
		return false;
	}
	if (hash_murmur3_buffer(payload.ptr(), payload.size()) != payload_checksum) {
		return false;
	}

	const uint8_t *read = payload.ptr();
	const uint32_t active_trunk_count = decode_uint32(read);
	read += 4;
	if (active_trunk_count > uint64_t(trunk_count)) {
		return false;
	}
	int previous_trunk_index = -1;
	int64_t bytes_read = 4;
	for (uint32_t active_trunk = 0; active_trunk < active_trunk_count; active_trunk++) {
		if (bytes_read + 4 > payload.size()) {
			return false;
		}
		const uint32_t trunk_index = decode_uint32(read);
		read += 4;
		bytes_read += 4;
		if ((previous_trunk_index >= 0 && trunk_index <= uint32_t(previous_trunk_index)) || trunk_index >= uint64_t(trunk_count)) {
			return false;
		}
		previous_trunk_index = trunk_index;
		const int trunk_probe_count = _get_trunk_probe_count(trunk_index);
		const int64_t trunk_bytes = int64_t(trunk_probe_count) * (4 * sizeof(float) + LocalLRTMath::PACKED_TRANSFER_UINTS_PER_PROBE * sizeof(uint32_t) + 12 * sizeof(float)) + (trunk_probe_count + 7) / 8;
		if (bytes_read + trunk_bytes > payload.size()) {
			return false;
		}
		read += trunk_bytes;
		bytes_read += trunk_bytes;
	}
	return bytes_read == payload.size();
}

void LocalLRTVolumeData::allocate(const Vector3 &p_size, const Vector3i &p_resolution, const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light, const Vector<int> &p_inside_solid) {
	ERR_FAIL_COND(!Math::is_finite(p_size.x) || !Math::is_finite(p_size.y) || !Math::is_finite(p_size.z) || p_size.x <= 0.0 || p_size.y <= 0.0 || p_size.z <= 0.0);
	ERR_FAIL_COND(p_resolution.x < 2 || p_resolution.y < 2 || p_resolution.z < 2);
	const int64_t probe_count_64 = int64_t(p_resolution.x) * p_resolution.y * p_resolution.z;
	ERR_FAIL_COND(probe_count_64 > INT32_MAX / 12);
	const int probe_count = probe_count_64;
	ERR_FAIL_COND(p_local_visibility.size() != probe_count);
	ERR_FAIL_COND(p_local_transfer.size() != probe_count * 12);
	ERR_FAIL_COND(p_mesh_light.size() != probe_count * 3);
	ERR_FAIL_COND(p_inside_solid.size() != probe_count);

	size = p_size;
	resolution = p_resolution;
	data_format_version = DATA_FORMAT_VERSION;
	const Vector3i trunk_resolution = _get_trunk_resolution();
	const int trunk_count = trunk_resolution.x * trunk_resolution.y * trunk_resolution.z;
	const Vector4 fully_visible = LocalLRTMath::encode_constant(1.0);
	Vector<int> active_trunks;
	int64_t payload_size = 4;
	for (int trunk_index = 0; trunk_index < trunk_count; trunk_index++) {
		Vector3i begin;
		Vector3i end;
		_get_trunk_bounds(trunk_index, begin, end);
		bool active = false;
		for (int z = begin.z; z < end.z && !active; z++) {
			for (int y = begin.y; y < end.y && !active; y++) {
				for (int x = begin.x; x < end.x; x++) {
					const int probe_index = LocalLRTMath::probe_index(Vector3i(x, y, z), resolution);
					active = p_local_visibility[probe_index] != fully_visible || p_inside_solid[probe_index] != 0;
					for (int value = 0; value < 12 && !active; value++) {
						active = p_local_transfer[probe_index * 12 + value] != Vector4();
					}
					for (int value = 0; value < 3 && !active; value++) {
						active = p_mesh_light[probe_index * 3 + value] != Vector4();
					}
					if (active) {
						break;
					}
				}
			}
		}
		if (!active) {
			continue;
		}
		active_trunks.push_back(trunk_index);
		const int trunk_probe_count = _get_trunk_probe_count(trunk_index);
		payload_size += 4 + int64_t(trunk_probe_count) * (4 * sizeof(float) + LocalLRTMath::PACKED_TRANSFER_UINTS_PER_PROBE * sizeof(uint32_t) + 12 * sizeof(float)) + (trunk_probe_count + 7) / 8;
	}
	ERR_FAIL_COND(payload_size > INT32_MAX);

	payload.resize(int(payload_size));
	uint8_t *write = payload.ptrw();
	encode_uint32(active_trunks.size(), write);
	write += sizeof(uint32_t);
	for (int trunk_index : active_trunks) {
		encode_uint32(trunk_index, write);
		write += sizeof(uint32_t);
		Vector3i begin;
		Vector3i end;
		_get_trunk_bounds(trunk_index, begin, end);
		Vector<int> probe_indices;
		for (int z = begin.z; z < end.z; z++) {
			for (int y = begin.y; y < end.y; y++) {
				for (int x = begin.x; x < end.x; x++) {
					probe_indices.push_back(LocalLRTMath::probe_index(Vector3i(x, y, z), resolution));
				}
			}
		}
		for (int probe_index : probe_indices) {
			const Vector4 &visibility = p_local_visibility[probe_index];
			for (int component = 0; component < 4; component++) {
				write += encode_float(visibility[component], write);
			}
		}
		for (int probe_index : probe_indices) {
			uint32_t packed_transfer[LocalLRTMath::PACKED_TRANSFER_UINTS_PER_PROBE];
			LocalLRTMath::pack_transfer_luminance_fp16_rgb8(&p_local_transfer[probe_index * 12], packed_transfer);
			for (uint32_t value : packed_transfer) {
				write += encode_uint32(value, write);
			}
		}
		for (int probe_index : probe_indices) {
			for (int value = 0; value < 3; value++) {
				const Vector4 &mesh_light = p_mesh_light[probe_index * 3 + value];
				for (int component = 0; component < 4; component++) {
					write += encode_float(mesh_light[component], write);
				}
			}
		}
		const int bitset_bytes = (probe_indices.size() + 7) / 8;
		memset(write, 0, bitset_bytes);
		for (int probe = 0; probe < probe_indices.size(); probe++) {
			if (p_inside_solid[probe_indices[probe]] != 0) {
				write[probe / 8] |= 1u << (probe % 8);
			}
		}
		write += bitset_bytes;
	}
	payload_checksum = hash_murmur3_buffer(payload.ptr(), payload.size());
}

bool LocalLRTVolumeData::decode(Vector<Vector4> &r_local_visibility, Vector<Vector4> &r_local_transfer, Vector<Vector4> &r_mesh_light, Vector<int> &r_inside_solid) const {
	if (!_validate_data()) {
		return false;
	}
	const int probe_count = resolution.x * resolution.y * resolution.z;
	r_local_visibility.resize(probe_count);
	r_local_visibility.fill(LocalLRTMath::encode_constant(1.0));
	r_local_transfer.resize(probe_count * 12);
	r_local_transfer.fill(Vector4());
	r_mesh_light.resize(probe_count * 3);
	r_mesh_light.fill(Vector4());
	r_inside_solid.resize(probe_count);
	r_inside_solid.fill(0);

	const uint8_t *read = payload.ptr();
	const uint32_t active_trunk_count = decode_uint32(read);
	read += 4;
	for (uint32_t active_trunk = 0; active_trunk < active_trunk_count; active_trunk++) {
		const int trunk_index = decode_uint32(read);
		read += 4;
		Vector3i begin;
		Vector3i end;
		_get_trunk_bounds(trunk_index, begin, end);
		Vector<int> probe_indices;
		for (int z = begin.z; z < end.z; z++) {
			for (int y = begin.y; y < end.y; y++) {
				for (int x = begin.x; x < end.x; x++) {
					probe_indices.push_back(LocalLRTMath::probe_index(Vector3i(x, y, z), resolution));
				}
			}
		}
		for (int probe_index : probe_indices) {
			Vector4 &visibility = r_local_visibility.write[probe_index];
			for (int component = 0; component < 4; component++) {
				visibility[component] = decode_float(read);
				read += sizeof(float);
			}
		}
		for (int probe_index : probe_indices) {
			uint32_t packed_transfer[LocalLRTMath::PACKED_TRANSFER_UINTS_PER_PROBE];
			for (uint32_t &value : packed_transfer) {
				value = decode_uint32(read);
				read += sizeof(uint32_t);
			}
			LocalLRTMath::unpack_transfer_luminance_fp16_rgb8(packed_transfer, r_local_transfer.ptrw() + probe_index * 12);
		}
		for (int probe_index : probe_indices) {
			for (int value = 0; value < 3; value++) {
				Vector4 &mesh_light = r_mesh_light.write[probe_index * 3 + value];
				for (int component = 0; component < 4; component++) {
					mesh_light[component] = decode_float(read);
					read += sizeof(float);
				}
			}
		}
		for (int probe = 0; probe < probe_indices.size(); probe++) {
			r_inside_solid.write[probe_indices[probe]] = (read[probe / 8] >> (probe % 8)) & 1u;
		}
		read += (probe_indices.size() + 7) / 8;
	}
	return true;
}

bool LocalLRTVolumeData::is_valid() const {
	return _validate_data();
}

void LocalLRTVolumeData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_valid"), &LocalLRTVolumeData::is_valid);
	ClassDB::bind_method(D_METHOD("_set_data", "data"), &LocalLRTVolumeData::_set_data);
	ClassDB::bind_method(D_METHOD("_get_data"), &LocalLRTVolumeData::_get_data);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_data", "_get_data");
}

void LocalLRTVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LocalLRTVolume3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LocalLRTVolume3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LocalLRTVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LocalLRTVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "probe_spacing"), &LocalLRTVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &LocalLRTVolume3D::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("set_geometry_voxel_size", "voxel_size"), &LocalLRTVolume3D::set_geometry_voxel_size);
	ClassDB::bind_method(D_METHOD("get_geometry_voxel_size"), &LocalLRTVolume3D::get_geometry_voxel_size);
	ClassDB::bind_method(D_METHOD("set_dynamic_update_probe_budget", "probe_budget"), &LocalLRTVolume3D::set_dynamic_update_probe_budget);
	ClassDB::bind_method(D_METHOD("get_dynamic_update_probe_budget"), &LocalLRTVolume3D::get_dynamic_update_probe_budget);
	ClassDB::bind_method(D_METHOD("get_resolution"), &LocalLRTVolume3D::get_resolution);
	ClassDB::bind_method(D_METHOD("get_actual_probe_spacing"), &LocalLRTVolume3D::get_actual_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_position", "grid_position"), &LocalLRTVolume3D::get_probe_position);
	ClassDB::bind_method(D_METHOD("set_visibility_iterations", "iterations"), &LocalLRTVolume3D::set_visibility_iterations);
	ClassDB::bind_method(D_METHOD("get_visibility_iterations"), &LocalLRTVolume3D::get_visibility_iterations);
	ClassDB::bind_method(D_METHOD("set_propagation_iterations", "iterations"), &LocalLRTVolume3D::set_propagation_iterations);
	ClassDB::bind_method(D_METHOD("get_propagation_iterations"), &LocalLRTVolume3D::get_propagation_iterations);
	ClassDB::bind_method(D_METHOD("set_visibility_probe_budget", "probe_budget"), &LocalLRTVolume3D::set_visibility_probe_budget);
	ClassDB::bind_method(D_METHOD("get_visibility_probe_budget"), &LocalLRTVolume3D::get_visibility_probe_budget);
	ClassDB::bind_method(D_METHOD("set_radiance_probe_budget", "probe_budget"), &LocalLRTVolume3D::set_radiance_probe_budget);
	ClassDB::bind_method(D_METHOD("get_radiance_probe_budget"), &LocalLRTVolume3D::get_radiance_probe_budget);
	ClassDB::bind_method(D_METHOD("set_radiance_neighbor_pattern", "pattern"), &LocalLRTVolume3D::set_radiance_neighbor_pattern);
	ClassDB::bind_method(D_METHOD("get_radiance_neighbor_pattern"), &LocalLRTVolume3D::get_radiance_neighbor_pattern);
	ClassDB::bind_method(D_METHOD("set_energy", "energy"), &LocalLRTVolume3D::set_energy);
	ClassDB::bind_method(D_METHOD("get_energy"), &LocalLRTVolume3D::get_energy);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &LocalLRTVolume3D::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &LocalLRTVolume3D::get_priority);
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
	ClassDB::bind_method(D_METHOD("get_sdf_build_count"), &LocalLRTVolume3D::get_sdf_build_count);
	ClassDB::bind_method(D_METHOD("get_last_geometry_update_probe_count"), &LocalLRTVolume3D::get_last_geometry_update_probe_count);
	ClassDB::bind_method(D_METHOD("get_last_geometry_update_usec"), &LocalLRTVolume3D::get_last_geometry_update_usec);
	ClassDB::bind_method(D_METHOD("get_last_geometry_build_usec"), &LocalLRTVolume3D::get_last_geometry_build_usec);
	ClassDB::bind_method(D_METHOD("get_last_geometry_pack_usec"), &LocalLRTVolume3D::get_last_geometry_pack_usec);
	ClassDB::bind_method(D_METHOD("get_last_geometry_upload_usec"), &LocalLRTVolume3D::get_last_geometry_upload_usec);
	ClassDB::bind_method(D_METHOD("get_last_geometry_update_frame_count"), &LocalLRTVolume3D::get_last_geometry_update_frame_count);
	ClassDB::bind_method(D_METHOD("get_last_geometry_max_build_slice_usec"), &LocalLRTVolume3D::get_last_geometry_max_build_slice_usec);
	ClassDB::bind_method(D_METHOD("is_geometry_update_pending"), &LocalLRTVolume3D::is_geometry_update_pending);
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
	ClassDB::bind_method(D_METHOD("get_probe_environment_injection", "grid_position", "channel"), &LocalLRTVolume3D::get_probe_environment_injection);
	ClassDB::bind_method(D_METHOD("get_probe_injection_color", "grid_position"), &LocalLRTVolume3D::get_probe_injection_color);
	ClassDB::bind_method(D_METHOD("get_probe_radiance", "grid_position", "channel"), &LocalLRTVolume3D::get_probe_radiance);
	ClassDB::bind_method(D_METHOD("get_probe_radiance_color", "grid_position"), &LocalLRTVolume3D::get_probe_radiance_color);
	ClassDB::bind_method(D_METHOD("get_probe_shadow_visibility", "grid_position"), &LocalLRTVolume3D::get_probe_shadow_visibility);
	ClassDB::bind_method(D_METHOD("has_gpu_data"), &LocalLRTVolume3D::has_gpu_data);
	ClassDB::bind_method(D_METHOD("update_light_injection"), &LocalLRTVolume3D::update_light_injection);
	ClassDB::bind_method(D_METHOD("rebuild"), &LocalLRTVolume3D::rebuild);
	ClassDB::bind_method(D_METHOD("set_bake_data", "data"), &LocalLRTVolume3D::set_bake_data);
	ClassDB::bind_method(D_METHOD("get_bake_data"), &LocalLRTVolume3D::get_bake_data);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, LocalLRTVolumeData::get_class_static(), PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_ALWAYS_DUPLICATE), "set_bake_data", "get_bake_data");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0.01,1024,0.01,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.01,64,0.01,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "geometry_voxel_size", PROPERTY_HINT_RANGE, "0.01,4,0.001,or_greater,suffix:m"), "set_geometry_voxel_size", "get_geometry_voxel_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dynamic_update_probe_budget"), "set_dynamic_update_probe_budget", "get_dynamic_update_probe_budget");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "resolution", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_iterations", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_visibility_iterations", "get_visibility_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "propagation_iterations", PROPERTY_HINT_RANGE, "1,64,1,or_greater"), "set_propagation_iterations", "get_propagation_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "visibility_probe_budget", PROPERTY_HINT_RANGE, "0,1048576,1,or_greater"), "set_visibility_probe_budget", "get_visibility_probe_budget");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radiance_probe_budget", PROPERTY_HINT_RANGE, "0,1048576,1,or_greater"), "set_radiance_probe_budget", "get_radiance_probe_budget");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radiance_neighbor_pattern", PROPERTY_HINT_ENUM, "Reference 26,Dithered 4"), "set_radiance_neighbor_pattern", "get_radiance_neighbor_pattern");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-1000,1000,1"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_blend_distance", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater,suffix:m"), "set_edge_blend_distance", "get_edge_blend_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_draw"), "set_debug_draw", "is_debug_draw_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_mode", PROPERTY_HINT_ENUM, "Occupancy,Local Visibility,Local Transfer,Global Visibility,Injection,Radiance,Geometry Distance,Geometry Coverage,Inside Solid,Directional Shadow,Omni Shadow,Area Shadow,Spot Shadow,Shadowed Injection,Environment Injection"), "set_debug_mode", "get_debug_mode");
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
	BIND_ENUM_CONSTANT(DEBUG_MODE_AREA_SHADOW);
	BIND_ENUM_CONSTANT(DEBUG_MODE_SPOT_SHADOW);
	BIND_ENUM_CONSTANT(DEBUG_MODE_SHADOWED_INJECTION);
	BIND_ENUM_CONSTANT(DEBUG_MODE_ENVIRONMENT_INJECTION);
	BIND_ENUM_CONSTANT(RADIANCE_NEIGHBOR_PATTERN_REFERENCE_26);
	BIND_ENUM_CONSTANT(RADIANCE_NEIGHBOR_PATTERN_DITHERED_4);
}

void LocalLRTVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		RS::get_singleton()->local_lrt_volume_set_enabled(volume, enabled);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		RS::get_singleton()->local_lrt_volume_set_enabled(volume, false);
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		const Transform3D global_transform = is_inside_tree() ? get_global_transform() : get_transform();
		RS::get_singleton()->local_lrt_volume_set_transform(volume, global_transform);
		if (builder) {
			builder->set_transform(global_transform);
			force_light_injection_update = true;
		}
	} else if (p_what == NOTIFICATION_READY) {
		_ensure_debug_probe_instance();
		if (!builder) {
			if (bake_data.is_valid() && bake_data->is_valid()) {
				_apply_bake_data();
			} else if (!Engine::get_singleton()->is_editor_hint()) {
				rebuild();
			}
		}
	} else if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
		if (gizmo_size_edit_active) {
			return;
		}
		if (!Engine::get_singleton()->is_editor_hint()) {
			_update_geometry_sources();
		}
		if (!geometry_update_pending && debug_draw && debug_mode == DEBUG_MODE_INJECTION) {
			update_light_injection();
		}
		if (builder && enabled && debug_draw) {
			if (debug_mode == DEBUG_MODE_DIRECTIONAL_SHADOW || debug_mode == DEBUG_MODE_OMNI_SHADOW || debug_mode == DEBUG_MODE_AREA_SHADOW || debug_mode == DEBUG_MODE_SPOT_SHADOW) {
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
			} else if (debug_mode == DEBUG_MODE_ENVIRONMENT_INJECTION) {
				environment_injection = RS::get_singleton()->local_lrt_volume_get_environment_injection(volume);
				if (environment_injection.size() == builder->get_probe_count() * 3) {
					_update_debug_probe_instances();
				}
			} else if (debug_mode == DEBUG_MODE_GLOBAL_VISIBILITY) {
				global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
				if (global_visibility.size() == builder->get_probe_count()) {
					_sync_global_visibility_to_builder();
					_update_debug_probe_instances();
				}
			} else if (debug_mode == DEBUG_MODE_RADIANCE) {
				radiance = RS::get_singleton()->local_lrt_volume_get_radiance(volume);
				_update_debug_probe_instances();
			}
		}
		if (geometry_update_pending || (builder && enabled)) {
			RenderingServerDefault::redraw_request();
		}
	}
}

Vector3i LocalLRTVolume3D::_calculate_resolution() const {
	return Vector3i(
			MAX(2, (int)Math::ceil(size.x / probe_spacing) + 1),
			MAX(2, (int)Math::ceil(size.y / probe_spacing) + 1),
			MAX(2, (int)Math::ceil(size.z / probe_spacing) + 1));
}

Vector3 LocalLRTVolume3D::_get_active_size() const {
	return builder ? builder->get_size() : size;
}

Vector3i LocalLRTVolume3D::_get_active_resolution() const {
	return builder ? builder->get_resolution() : _calculate_resolution();
}

Vector3 LocalLRTVolume3D::_get_active_probe_spacing() const {
	return _get_active_size() / Vector3(_get_active_resolution() - Vector3i(1, 1, 1));
}

bool LocalLRTVolume3D::_is_valid_probe_position(const Vector3i &p_grid_position) const {
	const Vector3i resolution = _get_active_resolution();
	return p_grid_position.x >= 0 && p_grid_position.y >= 0 && p_grid_position.z >= 0 &&
			p_grid_position.x < resolution.x && p_grid_position.y < resolution.y && p_grid_position.z < resolution.z;
}

void LocalLRTVolume3D::_sync_grid() {
	if (!builder) {
		RS::get_singleton()->local_lrt_volume_set_grid(volume, size, get_resolution());
	}
	_update_debug_probe_instances();
	update_gizmos();
	notify_property_list_changed();
}

void LocalLRTVolume3D::_clear_built_data() {
	geometry_sources.clear();
	if (builder) {
		memdelete(builder);
		builder = nullptr;
	}
	global_visibility.clear();
	injection.clear();
	analytic_lights.clear();
	shadowed_injection.clear();
	environment_injection.clear();
	shadow_visibility.clear();
	radiance.clear();
	built_geometry_count = 0;
	sdf_build_count = 0;
	last_geometry_update_probe_count = 0;
	last_geometry_update_usec = 0;
	last_geometry_build_usec = 0;
	last_geometry_pack_usec = 0;
	last_geometry_upload_usec = 0;
	last_geometry_update_frame_count = 0;
	last_geometry_max_build_slice_usec = 0;
	geometry_update_pending = false;
	pending_geometry_regions.clear();
	pending_geometry_region_index = 0;
	pending_geometry_region_probe_index = 0;
	pending_geometry_upload_begin = Vector3i();
	pending_geometry_upload_end = Vector3i();
	pending_geometry_probe_count = 0;
	pending_geometry_update_frame_count = 0;
	pending_geometry_source_usec = 0;
	pending_geometry_build_usec = 0;
	pending_geometry_max_build_slice_usec = 0;
	force_light_injection_update = false;
}

static bool local_lrt_mesh_is_visible(const MeshInstance3D *p_mesh_instance) {
	return p_mesh_instance->is_inside_tree() ? p_mesh_instance->is_visible_in_tree() : p_mesh_instance->is_visible();
}

AABB LocalLRTVolume3D::_get_collection_bounds() const {
	const Vector3 spacing = _get_active_probe_spacing();
	const Vector3 active_size = _get_active_size();
	AABB bounds(-active_size * 0.5, active_size);
	bounds.position -= spacing;
	bounds.size += spacing * 2.0;
	return bounds;
}

static void local_lrt_extract_surface_color(MeshInstance3D *p_mesh_instance, int p_surface, Color &r_albedo, Color &r_emission, Color &r_transfer_emission) {
	r_albedo = Color(1.0, 1.0, 1.0);
	r_emission = Color();
	r_transfer_emission = Color();
	const Ref<Material> material = p_mesh_instance->get_active_material(p_surface);
	const Ref<BaseMaterial3D> base_material = material;
	if (base_material.is_null()) {
		return;
	}
	r_albedo = base_material->get_albedo();
	if (!base_material->get_feature(BaseMaterial3D::FEATURE_EMISSION)) {
		return;
	}
	r_transfer_emission = base_material->get_emission();
	r_emission = r_transfer_emission;
	const float emission_energy = base_material->get_emission_energy_multiplier() * LOCAL_LRT_MESH_LIGHT_ENERGY_SCALE;
	r_emission.r *= emission_energy;
	r_emission.g *= emission_energy;
	r_emission.b *= emission_energy;
}

static LocalLRTColorSDF local_lrt_make_mesh_sdf(const Ref<Mesh> &p_mesh, int p_surface, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission, const Color &p_transfer_emission) {
	if (p_surface == 0) {
		if (Object::cast_to<PlaneMesh>(p_mesh.ptr())) {
			return LocalLRTColorSDF();
		}
		if (const BoxMesh *box = Object::cast_to<BoxMesh>(p_mesh.ptr())) {
			return LocalLRTColorSDF::make_box(box->get_size() * 0.5, p_voxel_size, p_albedo, p_emission, p_transfer_emission);
		}
		if (const SphereMesh *sphere = Object::cast_to<SphereMesh>(p_mesh.ptr())) {
			if (!sphere->get_is_hemisphere()) {
				return LocalLRTColorSDF::make_sphere(sphere->get_radius(), p_voxel_size, p_albedo, p_emission, p_transfer_emission);
			}
		}
	}
	return LocalLRTColorSDF::from_mesh_surface(p_mesh, p_surface, p_voxel_size, p_albedo, p_emission, p_transfer_emission);
}

int LocalLRTVolume3D::_find_geometry_source(ObjectID p_object_id, int p_surface) const {
	for (int index = 0; index < geometry_sources.size(); index++) {
		if (geometry_sources[index].object_id == p_object_id && geometry_sources[index].surface == p_surface) {
			return index;
		}
	}
	return -1;
}

AABB LocalLRTVolume3D::_get_source_influence_bounds(const LocalLRTColorSDF &p_sdf, const Transform3D &p_object_to_volume) const {
	AABB bounds = p_object_to_volume.xform(p_sdf.get_bounds());
	const Vector3 spacing = _get_active_probe_spacing();
	const Vector3 scale = p_object_to_volume.basis.get_scale().abs();
	const real_t scale_max = MAX(scale.x, MAX(scale.y, scale.z));
	const real_t margin = MAX(spacing.x, MAX(spacing.y, spacing.z)) + geometry_voxel_size * scale_max;
	return bounds.grow(margin);
}

bool LocalLRTVolume3D::_geometry_sdf_input_matches(const GeometrySourceState &p_a, const GeometrySourceState &p_b) const {
	return p_a.surface == p_b.surface && p_a.mesh == p_b.mesh && p_a.albedo == p_b.albedo && p_a.emission == p_b.emission && p_a.transfer_emission == p_b.transfer_emission;
}

bool LocalLRTVolume3D::_geometry_world_state_matches(const GeometrySourceState &p_a, const GeometrySourceState &p_b) const {
	return p_a.object_world_transform.is_equal_approx(p_b.object_world_transform) && p_a.visible == p_b.visible && p_a.gi_mode == p_b.gi_mode && _geometry_sdf_input_matches(p_a, p_b);
}

bool LocalLRTVolume3D::_geometry_source_voxel_size_matches(const GeometrySourceState &p_state) const {
	if (!p_state.sdf_ready || p_state.sdf.is_empty()) {
		return true;
	}
	return Math::is_equal_approx((float)p_state.sdf.get_voxel_size(), geometry_voxel_size);
}

bool LocalLRTVolume3D::_geometry_output_matches(const GeometrySourceState &p_a, const GeometrySourceState &p_b) const {
	if (p_a.active != p_b.active) {
		return false;
	}
	if (!p_a.active) {
		return true;
	}
	if (p_a.object_to_volume != p_b.object_to_volume || !_geometry_sdf_input_matches(p_a, p_b)) {
		return false;
	}
	if (p_a.sdf.is_empty() && p_b.sdf.is_empty()) {
		return true;
	}
	return Math::is_equal_approx((float)p_a.sdf.get_voxel_size(), (float)p_b.sdf.get_voxel_size());
}

void LocalLRTVolume3D::_collect_geometry_sources(Node *p_node, const Transform3D &p_world_to_volume, Vector<GeometrySourceState> &r_geometry) {
	MeshInstance3D *mesh_instance = Object::cast_to<MeshInstance3D>(p_node);
	const bool contributes_gi = mesh_instance && (mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC || mesh_instance->get_gi_mode() == GeometryInstance3D::GI_MODE_DYNAMIC);
	if (contributes_gi) {
		const Ref<Mesh> mesh = mesh_instance->get_mesh();
		if (mesh.is_valid()) {
			const Transform3D mesh_transform = mesh_instance->is_inside_tree() ? mesh_instance->get_global_transform() : mesh_instance->get_transform();
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				GeometrySourceState state;
				state.object_id = mesh_instance->get_instance_id();
				state.surface = surface;
				state.gi_mode = mesh_instance->get_gi_mode();
				state.visible = local_lrt_mesh_is_visible(mesh_instance);
				state.mesh = mesh;
				state.object_world_transform = mesh_transform;
				state.object_to_volume = p_world_to_volume * mesh_transform;
				local_lrt_extract_surface_color(mesh_instance, surface, state.albedo, state.emission, state.transfer_emission);
				const int previous_index = _find_geometry_source(state.object_id, state.surface);
				bool copied_unmoved_source = false;
				if (previous_index >= 0) {
					const GeometrySourceState &previous = geometry_sources[previous_index];
					if (_geometry_world_state_matches(state, previous) && _geometry_source_voxel_size_matches(previous)) {
						state.sdf = previous.sdf;
						state.sdf_ready = previous.sdf_ready;
						state.active = previous.active;
						state.influence_bounds = previous.influence_bounds;
						copied_unmoved_source = true;
					} else if (_geometry_sdf_input_matches(state, previous) && _geometry_source_voxel_size_matches(previous)) {
						state.sdf = previous.sdf;
						state.sdf_ready = previous.sdf_ready;
					}
				}
				if (!copied_unmoved_source) {
					bool intersects_volume = _get_collection_bounds().intersects(state.object_to_volume.xform(state.mesh->get_aabb()));
					if (state.visible && intersects_volume && !state.sdf_ready) {
						state.sdf = local_lrt_make_mesh_sdf(state.mesh, state.surface, geometry_voxel_size, state.albedo, state.emission, state.transfer_emission);
						sdf_build_count++;
						state.sdf_ready = true;
					}
					if (state.sdf_ready && !state.sdf.is_empty()) {
						intersects_volume = _get_collection_bounds().intersects(state.object_to_volume.xform(state.sdf.get_bounds()));
					}
					state.active = state.visible && intersects_volume && !state.sdf.is_empty();
					if (state.active) {
						state.influence_bounds = _get_source_influence_bounds(state.sdf, state.object_to_volume);
					}
				}
				r_geometry.push_back(state);
			}
		}
	}

	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_geometry_sources(p_node->get_child(child), p_world_to_volume, r_geometry);
	}
}

void LocalLRTVolume3D::_install_geometry_sources() {
	builder->clear_geometry_sources();
	built_geometry_count = 0;
	for (const GeometrySourceState &state : geometry_sources) {
		if (!state.active) {
			continue;
		}
		builder->add_geometry_source(state.sdf, state.object_to_volume);
		built_geometry_count++;
	}
}

void LocalLRTVolume3D::_upload_geometry_region(const Vector3i &p_begin, const Vector3i &p_end, uint64_t &r_pack_usec, uint64_t &r_upload_usec) {
	const uint64_t pack_begin = Time::get_singleton()->get_ticks_usec();
	const Vector3i region_size = p_end - p_begin + Vector3i(1, 1, 1);
	const int region_probe_count = region_size.x * region_size.y * region_size.z;
	Vector<Vector4> local_visibility;
	Vector<Vector4> local_transfer;
	Vector<Vector4> mesh_light;
	Vector<int> inside_solid;
	local_visibility.resize(region_probe_count);
	local_transfer.resize(region_probe_count * 12);
	mesh_light.resize(region_probe_count * 3);
	inside_solid.resize(region_probe_count);

	int region_index = 0;
	for (int z = p_begin.z; z <= p_end.z; z++) {
		for (int y = p_begin.y; y <= p_end.y; y++) {
			for (int x = p_begin.x; x <= p_end.x; x++) {
				const LocalLRTBuilder::Probe &probe = builder->get_probe(Vector3i(x, y, z));
				local_visibility.write[region_index] = probe.local_visibility;
				inside_solid.write[region_index] = probe.inside_solid ? 1 : 0;
				mesh_light.write[region_index * 3] = probe.mesh_light.r;
				mesh_light.write[region_index * 3 + 1] = probe.mesh_light.g;
				mesh_light.write[region_index * 3 + 2] = probe.mesh_light.b;
				const LocalLRTMath::SH2Matrix *channels[] = { &probe.local_transfer.r, &probe.local_transfer.g, &probe.local_transfer.b };
				for (int channel = 0; channel < 3; channel++) {
					for (int row = 0; row < 4; row++) {
						local_transfer.write[region_index * 12 + channel * 4 + row] = channels[channel]->rows[row];
					}
				}
				region_index++;
			}
		}
	}
	r_pack_usec += Time::get_singleton()->get_ticks_usec() - pack_begin;
	const uint64_t upload_begin = Time::get_singleton()->get_ticks_usec();
	RS::get_singleton()->local_lrt_volume_update_static_data(volume, p_begin, region_size, local_visibility, local_transfer, mesh_light, inside_solid);
	r_upload_usec += Time::get_singleton()->get_ticks_usec() - upload_begin;
}

bool LocalLRTVolume3D::_process_pending_geometry_update() {
	ERR_FAIL_COND_V(!geometry_update_pending, false);
	int remaining_frame_budget = dynamic_update_probe_budget > 0 ? dynamic_update_probe_budget : pending_geometry_probe_count;
	const uint64_t build_begin = Time::get_singleton()->get_ticks_usec();
	while (remaining_frame_budget > 0 && pending_geometry_region_index < pending_geometry_regions.size()) {
		const LocalLRTBuilder::TrunkRegion &region = pending_geometry_regions[pending_geometry_region_index];
		const int region_remaining = region.get_probe_count() - pending_geometry_region_probe_index;
		const int build_probe_count = MIN(remaining_frame_budget, region_remaining);
		builder->build_local_data_region_slice(region.begin, region.end, pending_geometry_region_probe_index, build_probe_count);
		pending_geometry_region_probe_index += build_probe_count;
		remaining_frame_budget -= build_probe_count;
		if (pending_geometry_region_probe_index == region.get_probe_count()) {
			builder->mark_geometry_trunk_clean(region.trunk_index);
			pending_geometry_region_index++;
			pending_geometry_region_probe_index = 0;
		}
	}
	const uint64_t build_usec = Time::get_singleton()->get_ticks_usec() - build_begin;
	pending_geometry_update_frame_count++;
	pending_geometry_build_usec += build_usec;
	pending_geometry_max_build_slice_usec = MAX(pending_geometry_max_build_slice_usec, build_usec);
	if (pending_geometry_region_index < pending_geometry_regions.size()) {
		return false;
	}

	last_geometry_pack_usec = 0;
	last_geometry_upload_usec = 0;
	_upload_geometry_region(pending_geometry_upload_begin, pending_geometry_upload_end, last_geometry_pack_usec, last_geometry_upload_usec);
	last_geometry_update_probe_count = pending_geometry_probe_count;
	last_geometry_build_usec = pending_geometry_build_usec;
	last_geometry_update_usec = pending_geometry_source_usec + last_geometry_build_usec + last_geometry_pack_usec + last_geometry_upload_usec;
	last_geometry_update_frame_count = pending_geometry_update_frame_count;
	last_geometry_max_build_slice_usec = pending_geometry_max_build_slice_usec;
	geometry_update_pending = false;
	pending_geometry_regions.clear();
	force_light_injection_update = true;
	if (debug_draw) {
		if (debug_mode == DEBUG_MODE_GLOBAL_VISIBILITY) {
			global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
			_sync_global_visibility_to_builder();
		}
		_update_debug_probe_instances();
	}
	return true;
}

bool LocalLRTVolume3D::_update_geometry_sources() {
	if (!builder) {
		return false;
	}
	if (geometry_update_pending) {
		return _process_pending_geometry_update();
	}
	const uint64_t update_begin = Time::get_singleton()->get_ticks_usec();
	const Transform3D volume_transform = is_inside_tree() ? get_global_transform() : get_transform();
	Node *root = get_parent();
	if (is_inside_tree() && get_tree()->get_current_scene()) {
		root = get_tree()->get_current_scene();
	}
	Vector<GeometrySourceState> current_geometry;
	if (root) {
		_collect_geometry_sources(root, volume_transform.affine_inverse(), current_geometry);
	}

	AABB dirty_bounds;
	bool dirty = false;
	Vector<uint8_t> previous_matched;
	previous_matched.resize_initialized(geometry_sources.size());
	bool source_order_changed = false;
	if (current_geometry.size() == geometry_sources.size()) {
		for (int index = 0; index < current_geometry.size(); index++) {
			if (current_geometry[index].object_id != geometry_sources[index].object_id || current_geometry[index].surface != geometry_sources[index].surface) {
				source_order_changed = true;
				break;
			}
		}
	}
	for (const GeometrySourceState &current : current_geometry) {
		const int previous_index = _find_geometry_source(current.object_id, current.surface);
		if (previous_index >= 0) {
			previous_matched.write[previous_index] = true;
			const GeometrySourceState &previous = geometry_sources[previous_index];
			if (_geometry_world_state_matches(current, previous) || _geometry_output_matches(current, previous)) {
				continue;
			}
			if (previous.active) {
				dirty_bounds = dirty ? dirty_bounds.merge(previous.influence_bounds) : previous.influence_bounds;
				dirty = true;
			}
		} else if (!current.active) {
			continue;
		}
		if (current.active) {
			dirty_bounds = dirty ? dirty_bounds.merge(current.influence_bounds) : current.influence_bounds;
			dirty = true;
		}
	}
	if (source_order_changed) {
		for (const GeometrySourceState &previous : geometry_sources) {
			if (previous.active) {
				dirty_bounds = dirty ? dirty_bounds.merge(previous.influence_bounds) : previous.influence_bounds;
				dirty = true;
			}
		}
		for (const GeometrySourceState &current : current_geometry) {
			if (current.active) {
				dirty_bounds = dirty ? dirty_bounds.merge(current.influence_bounds) : current.influence_bounds;
				dirty = true;
			}
		}
	}
	for (int index = 0; index < geometry_sources.size(); index++) {
		if (!previous_matched[index] && geometry_sources[index].active) {
			dirty_bounds = dirty ? dirty_bounds.merge(geometry_sources[index].influence_bounds) : geometry_sources[index].influence_bounds;
			dirty = true;
		}
	}

	const Vector3 active_size = _get_active_size();
	const AABB active_bounds(-active_size * 0.5, active_size);
	if (!dirty || !dirty_bounds.intersects(active_bounds)) {
		geometry_sources = current_geometry;
		return false;
	}
	geometry_sources = current_geometry;
	_install_geometry_sources();
	pending_geometry_regions = builder->mark_geometry_trunks_dirty(dirty_bounds);
	if (pending_geometry_regions.is_empty()) {
		return false;
	}
	geometry_update_pending = true;
	pending_geometry_region_index = 0;
	pending_geometry_region_probe_index = 0;
	pending_geometry_probe_count = 0;
	pending_geometry_upload_begin = pending_geometry_regions[0].begin;
	pending_geometry_upload_end = pending_geometry_regions[0].end;
	for (const LocalLRTBuilder::TrunkRegion &region : pending_geometry_regions) {
		pending_geometry_probe_count += region.get_probe_count();
		pending_geometry_upload_begin = pending_geometry_upload_begin.min(region.begin);
		pending_geometry_upload_end = pending_geometry_upload_end.max(region.end);
	}
	pending_geometry_update_frame_count = 0;
	pending_geometry_source_usec = Time::get_singleton()->get_ticks_usec() - update_begin;
	pending_geometry_build_usec = 0;
	pending_geometry_max_build_slice_usec = 0;
	return _process_pending_geometry_update();
}

static void local_lrt_pack_analytic_light(Vector<Vector4> &r_lights, int p_type, const Color &p_color, real_t p_energy, const Vector3 &p_vector, real_t p_range = 0.0, real_t p_attenuation = 1.0, const Vector3 &p_spot_direction = Vector3(), real_t p_cone_limit = 0.0, real_t p_shadow = 0.0, real_t p_cone_exponent = 0.0) {
	r_lights.push_back(Vector4((real_t)p_type, p_energy, p_range, p_cone_limit));
	r_lights.push_back(Vector4(p_color.r, p_color.g, p_color.b, p_shadow));
	r_lights.push_back(Vector4(p_vector.x, p_vector.y, p_vector.z, p_attenuation));
	r_lights.push_back(Vector4(p_spot_direction.x, p_spot_direction.y, p_spot_direction.z, p_cone_exponent));
	r_lights.push_back(Vector4(1.0, 0.0, 0.0, 0.0));
	r_lights.push_back(Vector4(0.0, 1.0, 0.0, 0.0));
	r_lights.push_back(Vector4(0.0, 0.0, 1.0, 0.0));
	r_lights.push_back(Vector4());
	r_lights.push_back(Vector4());
}

void LocalLRTVolume3D::_collect_light_injection(Node *p_node, Vector<Vector4> &r_lights, bool p_inject_builder) {
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
				if (p_inject_builder) {
					builder->inject_directional_light(source);
				}
				local_lrt_pack_analytic_light(r_lights, 1, source.color, source.energy, source.direction_to_light, 0.0, 1.0, Vector3(), 0.0, directional->has_shadow() ? 1.0 : 0.0);
			}
		} else if (Object::cast_to<OmniLight3D>(light)) {
			LocalLRTBuilder::OmniLight source;
			source.position = light_transform.origin;
			source.color = color;
			source.energy = light_energy;
			source.range = light->get_param(Light3D::PARAM_RANGE);
			source.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
			if (p_inject_builder) {
				builder->inject_omni_light(source);
			}
			local_lrt_pack_analytic_light(r_lights, 2, source.color, source.energy, source.position, source.range, source.attenuation);
		} else if (AreaLight3D *area = Object::cast_to<AreaLight3D>(light)) {
			LocalLRTBuilder::AreaLight source;
			source.position = light_transform.origin;
			source.direction = -light_transform.basis.get_column(Vector3::AXIS_Z).normalized();
			const Vector2 area_size = area->get_area_size();
			source.width = light_transform.basis.get_column(Vector3::AXIS_X).normalized() * area_size.x;
			source.height = light_transform.basis.get_column(Vector3::AXIS_Y).normalized() * area_size.y;
			source.color = color;
			source.energy = light_energy;
			source.range = light->get_param(Light3D::PARAM_RANGE);
			source.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
			source.normalize_energy = area->is_area_normalizing_energy();
			if (p_inject_builder) {
				builder->inject_area_light(source);
			}
			r_lights.push_back(Vector4(4, source.energy, source.range, source.normalize_energy ? 1.0 : 0.0));
			r_lights.push_back(Vector4(source.color.r, source.color.g, source.color.b, 0));
			r_lights.push_back(Vector4(source.position.x, source.position.y, source.position.z, source.attenuation));
			r_lights.push_back(Vector4(source.direction.x, source.direction.y, source.direction.z, 0));
			r_lights.push_back(Vector4(source.width.x, source.width.y, source.width.z, 0));
			r_lights.push_back(Vector4(source.height.x, source.height.y, source.height.z, 0));
			r_lights.push_back(Vector4());
			r_lights.push_back(Vector4());
			r_lights.push_back(Vector4());
		} else if (Object::cast_to<SpotLight3D>(light)) {
			LocalLRTBuilder::SpotLight source;
			source.position = light_transform.origin;
			source.direction = -light_transform.basis.get_column(Vector3::AXIS_Z).normalized();
			source.color = color;
			source.energy = light_energy;
			source.range = light->get_param(Light3D::PARAM_RANGE);
			source.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
			source.angle = Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE));
			source.angle_attenuation = light->get_param(Light3D::PARAM_SPOT_ATTENUATION);
			if (p_inject_builder) {
				builder->inject_spot_light(source);
			}
			local_lrt_pack_analytic_light(r_lights, 3, source.color, source.energy, source.position, source.range, source.attenuation, source.direction, Math::cos(source.angle), 0.0, 1.0 / source.angle_attenuation);
		}
	}

	for (int child = 0; child < p_node->get_child_count(); child++) {
		_collect_light_injection(p_node->get_child(child), r_lights, p_inject_builder);
	}
}

void LocalLRTVolume3D::_sync_global_visibility_to_builder() {
	if (!builder || global_visibility.size() != builder->get_probe_count()) {
		return;
	}
	const Vector3i resolution = builder->get_resolution();
	for (int index = 0; index < global_visibility.size(); index++) {
		builder->get_probe(LocalLRTMath::probe_position(index, resolution)).global_visibility = global_visibility[index];
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
	if (geometry_update_pending) {
		return;
	}
	if (!debug_draw || !builder) {
		if (debug_probe_multimesh->get_instance_count() != 0) {
			debug_probe_multimesh->set_instance_count(0);
		}
		return;
	}

	const Vector3i resolution = builder->get_resolution();
	const int probe_count = builder->get_probe_count();
	if ((debug_mode == DEBUG_MODE_DIRECTIONAL_SHADOW || debug_mode == DEBUG_MODE_OMNI_SHADOW || debug_mode == DEBUG_MODE_AREA_SHADOW || debug_mode == DEBUG_MODE_SPOT_SHADOW) && shadow_visibility.size() != probe_count) {
		debug_probe_multimesh->set_instance_count(0);
		return;
	}
	if ((debug_mode == DEBUG_MODE_SHADOWED_INJECTION && shadowed_injection.size() != probe_count * 3) ||
			(debug_mode == DEBUG_MODE_ENVIRONMENT_INJECTION && environment_injection.size() != probe_count * 3)) {
		debug_probe_multimesh->set_instance_count(0);
		return;
	}
	if (debug_probe_multimesh->get_instance_count() != probe_count) {
		debug_probe_multimesh->set_instance_count(probe_count);
	}
	const Vector3 spacing = builder->get_size() / Vector3(resolution - Vector3i(1, 1, 1));
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
		} else if (debug_mode == DEBUG_MODE_INJECTION || debug_mode == DEBUG_MODE_SHADOWED_INJECTION || debug_mode == DEBUG_MODE_ENVIRONMENT_INJECTION) {
			const Vector4 red = _get_probe_debug_injection(position, 0);
			const Vector4 green = _get_probe_debug_injection(position, 1);
			const Vector4 blue = _get_probe_debug_injection(position, 2);
			const float unit_energy = LocalLRTMath::encode_direction(Vector3(1.0, 0.0, 0.0), 1.0, Math::TAU).length();
			color = Color(red.length(), green.length(), blue.length()) / unit_energy;
		} else if (debug_mode == DEBUG_MODE_DIRECTIONAL_SHADOW || debug_mode == DEBUG_MODE_OMNI_SHADOW || debug_mode == DEBUG_MODE_AREA_SHADOW || debug_mode == DEBUG_MODE_SPOT_SHADOW) {
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
		if (debug_mode == DEBUG_MODE_INJECTION || debug_mode == DEBUG_MODE_SHADOWED_INJECTION || debug_mode == DEBUG_MODE_ENVIRONMENT_INJECTION) {
			const Vector4 red = _get_probe_debug_injection(position, 0);
			const Vector4 green = _get_probe_debug_injection(position, 1);
			const Vector4 blue = _get_probe_debug_injection(position, 2);
			directional_sh = red * 0.2126 + green * 0.7152 + blue * 0.0722;
		} else if (debug_mode == DEBUG_MODE_RADIANCE && radiance.size() == probe_count * 3) {
			const Vector4 red = get_probe_radiance(position, 0);
			const Vector4 green = get_probe_radiance(position, 1);
			const Vector4 blue = get_probe_radiance(position, 2);
			directional_sh = red * 0.2126 + green * 0.7152 + blue * 0.0722;
		}

		Transform3D probe_transform = probe_scale_transform;
		probe_transform.origin = builder->get_probe_local_position(position);
		debug_probe_multimesh->set_instance_transform(index, probe_transform);
		debug_probe_multimesh->set_instance_color(index, color);
		debug_probe_multimesh->set_instance_custom_data(index, Color(directional_sh.x, directional_sh.y, directional_sh.z, directional_sh.w));
	}
}

Vector4 LocalLRTVolume3D::_get_probe_debug_injection(const Vector3i &p_position, int p_channel) const {
	if (debug_mode == DEBUG_MODE_ENVIRONMENT_INJECTION) {
		return get_probe_environment_injection(p_position, p_channel);
	}
	if (debug_mode == DEBUG_MODE_SHADOWED_INJECTION) {
		return get_probe_shadowed_injection(p_position, p_channel);
	}
	return get_probe_injection(p_position, p_channel);
}

void LocalLRTVolume3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	RS::get_singleton()->local_lrt_volume_set_enabled(volume, enabled && is_inside_tree());
}

bool LocalLRTVolume3D::is_enabled() const {
	return enabled;
}

void LocalLRTVolume3D::set_size(const Vector3 &p_size) {
	const Vector3 next_size = p_size.maxf(0.01);
	if (size.is_equal_approx(next_size)) {
		return;
	}
	size = next_size;
	update_configuration_warnings();
	if (gizmo_size_edit_active) {
		update_gizmos();
		notify_property_list_changed();
		return;
	}
	_sync_grid();
}

Vector3 LocalLRTVolume3D::get_size() const {
	return size;
}

void LocalLRTVolume3D::begin_gizmo_size_edit() {
	gizmo_size_edit_active = true;
}

void LocalLRTVolume3D::end_gizmo_size_edit() {
	if (!gizmo_size_edit_active) {
		return;
	}
	gizmo_size_edit_active = false;
	update_gizmos();
	notify_property_list_changed();
}

void LocalLRTVolume3D::set_probe_spacing(float p_spacing) {
	probe_spacing = MAX(p_spacing, 0.01f);
	update_configuration_warnings();
	_sync_grid();
}

float LocalLRTVolume3D::get_probe_spacing() const {
	return probe_spacing;
}

void LocalLRTVolume3D::set_geometry_voxel_size(float p_voxel_size) {
	const float voxel_size = MAX(p_voxel_size, 0.001f);
	if (Math::is_equal_approx(geometry_voxel_size, voxel_size)) {
		return;
	}
	geometry_voxel_size = voxel_size;
}

float LocalLRTVolume3D::get_geometry_voxel_size() const {
	return geometry_voxel_size;
}

void LocalLRTVolume3D::set_dynamic_update_probe_budget(int p_probe_budget) {
	dynamic_update_probe_budget = MAX(p_probe_budget, 0);
}

int LocalLRTVolume3D::get_dynamic_update_probe_budget() const {
	return dynamic_update_probe_budget;
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
	ERR_FAIL_COND_V(p_grid_position.x < 0 || p_grid_position.y < 0 || p_grid_position.z < 0 || p_grid_position.x >= resolution.x || p_grid_position.y >= resolution.y || p_grid_position.z >= resolution.z, Vector3());
	return -size * 0.5 + Vector3(p_grid_position) * get_actual_probe_spacing();
}

void LocalLRTVolume3D::set_visibility_iterations(int p_iterations) {
	const int iterations = MAX(p_iterations, 1);
	if (visibility_iterations == iterations) {
		return;
	}
	visibility_iterations = iterations;
	RS::get_singleton()->local_lrt_volume_set_visibility_iterations(volume, visibility_iterations);
	if (builder) {
		global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
		_sync_global_visibility_to_builder();
		radiance = RS::get_singleton()->local_lrt_volume_get_radiance(volume);
		_update_debug_probe_instances();
		update_gizmos();
	}
	RenderingServerDefault::redraw_request();
}

int LocalLRTVolume3D::get_visibility_iterations() const {
	return visibility_iterations;
}

void LocalLRTVolume3D::set_propagation_iterations(int p_iterations) {
	const int iterations = MAX(p_iterations, 1);
	if (propagation_iterations == iterations) {
		return;
	}
	propagation_iterations = iterations;
	RS::get_singleton()->local_lrt_volume_set_propagation_iterations(volume, propagation_iterations);
	if (builder) {
		radiance = RS::get_singleton()->local_lrt_volume_get_radiance(volume);
		_update_debug_probe_instances();
		update_gizmos();
	}
	RenderingServerDefault::redraw_request();
}

int LocalLRTVolume3D::get_propagation_iterations() const {
	return propagation_iterations;
}

void LocalLRTVolume3D::set_visibility_probe_budget(int p_probe_budget) {
	visibility_probe_budget = MAX(p_probe_budget, 0);
	RS::get_singleton()->local_lrt_volume_set_visibility_probe_budget(volume, visibility_probe_budget);
}

int LocalLRTVolume3D::get_visibility_probe_budget() const {
	return visibility_probe_budget;
}

void LocalLRTVolume3D::set_radiance_probe_budget(int p_probe_budget) {
	radiance_probe_budget = MAX(p_probe_budget, 0);
	RS::get_singleton()->local_lrt_volume_set_radiance_probe_budget(volume, radiance_probe_budget);
}

int LocalLRTVolume3D::get_radiance_probe_budget() const {
	return radiance_probe_budget;
}

void LocalLRTVolume3D::set_radiance_neighbor_pattern(RadianceNeighborPattern p_pattern) {
	radiance_neighbor_pattern = p_pattern;
	RS::get_singleton()->local_lrt_volume_set_radiance_neighbor_pattern(volume, radiance_neighbor_pattern);
}

LocalLRTVolume3D::RadianceNeighborPattern LocalLRTVolume3D::get_radiance_neighbor_pattern() const {
	return radiance_neighbor_pattern;
}

void LocalLRTVolume3D::set_energy(float p_energy) {
	energy = MAX(p_energy, 0.0f);
	RS::get_singleton()->local_lrt_volume_set_energy(volume, energy);
}

float LocalLRTVolume3D::get_energy() const {
	return energy;
}

void LocalLRTVolume3D::set_priority(int p_priority) {
	priority = p_priority;
	RS::get_singleton()->local_lrt_volume_set_priority(volume, priority);
}

int LocalLRTVolume3D::get_priority() const {
	return priority;
}

void LocalLRTVolume3D::set_edge_blend_distance(float p_distance) {
	edge_blend_distance = MAX(p_distance, 0.0f);
	RS::get_singleton()->local_lrt_volume_set_edge_blend_distance(volume, edge_blend_distance);
	update_gizmos();
}

float LocalLRTVolume3D::get_edge_blend_distance() const {
	return edge_blend_distance;
}

void LocalLRTVolume3D::set_debug_draw(bool p_enabled) {
	debug_draw = p_enabled;
	if (debug_draw && debug_mode == DEBUG_MODE_INJECTION && builder) {
		force_light_injection_update = true;
		update_light_injection();
	}
	_update_debug_probe_instances();
	update_gizmos();
}

bool LocalLRTVolume3D::is_debug_draw_enabled() const {
	return debug_draw;
}

void LocalLRTVolume3D::set_debug_mode(DebugMode p_mode) {
	ERR_FAIL_INDEX(p_mode, DEBUG_MODE_ENVIRONMENT_INJECTION + 1);
	debug_mode = p_mode;
	if (debug_draw && debug_mode == DEBUG_MODE_INJECTION && builder) {
		force_light_injection_update = true;
		update_light_injection();
	}
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

int LocalLRTVolume3D::get_sdf_build_count() const {
	return sdf_build_count;
}

int LocalLRTVolume3D::get_last_geometry_update_probe_count() const {
	return last_geometry_update_probe_count;
}

uint64_t LocalLRTVolume3D::get_last_geometry_update_usec() const {
	return last_geometry_update_usec;
}

uint64_t LocalLRTVolume3D::get_last_geometry_build_usec() const {
	return last_geometry_build_usec;
}

uint64_t LocalLRTVolume3D::get_last_geometry_pack_usec() const {
	return last_geometry_pack_usec;
}

uint64_t LocalLRTVolume3D::get_last_geometry_upload_usec() const {
	return last_geometry_upload_usec;
}

int LocalLRTVolume3D::get_last_geometry_update_frame_count() const {
	return last_geometry_update_frame_count;
}

uint64_t LocalLRTVolume3D::get_last_geometry_max_build_slice_usec() const {
	return last_geometry_max_build_slice_usec;
}

bool LocalLRTVolume3D::is_geometry_update_pending() const {
	return geometry_update_pending;
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
	return global_visibility[LocalLRTMath::probe_index(p_grid_position, _get_active_resolution())];
}

Vector4 LocalLRTVolume3D::get_probe_injection(const Vector3i &p_grid_position, int p_channel) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_INDEX_V(p_channel, 3, Vector4());
	ERR_FAIL_COND_V(injection.size() != builder->get_probe_count() * 3, Vector4());
	return injection[LocalLRTMath::probe_index(p_grid_position, _get_active_resolution()) * 3 + p_channel];
}

Vector4 LocalLRTVolume3D::get_probe_shadowed_injection(const Vector3i &p_grid_position, int p_channel) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_INDEX_V(p_channel, 3, Vector4());
	ERR_FAIL_COND_V(shadowed_injection.size() != builder->get_probe_count() * 3, Vector4());
	return shadowed_injection[LocalLRTMath::probe_index(p_grid_position, _get_active_resolution()) * 3 + p_channel];
}

Vector4 LocalLRTVolume3D::get_probe_environment_injection(const Vector3i &p_grid_position, int p_channel) const {
	ERR_FAIL_NULL_V(builder, Vector4());
	ERR_FAIL_COND_V(!_is_valid_probe_position(p_grid_position), Vector4());
	ERR_FAIL_INDEX_V(p_channel, 3, Vector4());
	ERR_FAIL_COND_V(environment_injection.size() != builder->get_probe_count() * 3, Vector4());
	return environment_injection[LocalLRTMath::probe_index(p_grid_position, _get_active_resolution()) * 3 + p_channel];
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
	return radiance[LocalLRTMath::probe_index(p_grid_position, _get_active_resolution()) * 3 + p_channel];
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
	const Vector3i resolution = _get_active_resolution();
	ERR_FAIL_COND_V(shadow_visibility.size() != resolution.x * resolution.y * resolution.z, 1.0);
	return shadow_visibility[LocalLRTMath::probe_index(p_grid_position, resolution)];
}

bool LocalLRTVolume3D::has_gpu_data() const {
	return builder && global_visibility.size() == builder->get_probe_count() &&
			injection.size() == builder->get_probe_count() * 3 &&
			radiance.size() == builder->get_probe_count() * 3;
}

void LocalLRTVolume3D::update_light_injection() {
	if (!builder || geometry_update_pending) {
		return;
	}

	Vector<Vector4> next_analytic_lights;
	Node *root = get_parent();
	if (is_inside_tree() && get_tree()->get_current_scene()) {
		root = get_tree()->get_current_scene();
	}
	if (root) {
		_collect_light_injection(root, next_analytic_lights, false);
	}
	const bool lights_changed = next_analytic_lights != analytic_lights;
	if (!lights_changed && !force_light_injection_update) {
		return;
	}

	builder->clear_injection();
	if (root) {
		Vector<Vector4> ignored_lights;
		_collect_light_injection(root, ignored_lights, true);
	}

	Vector<Vector4> next_injection;
	next_injection.resize(builder->get_probe_count() * 3);
	const Vector3i resolution = builder->get_resolution();
	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int x = 0; x < resolution.x; x++) {
				const Vector3i position(x, y, z);
				const int probe_index = LocalLRTMath::probe_index(position, resolution);
				const LocalLRTBuilder::Probe &probe = builder->get_probe(position);
				next_injection.write[probe_index * 3] = probe.injection.r;
				next_injection.write[probe_index * 3 + 1] = probe.injection.g;
				next_injection.write[probe_index * 3 + 2] = probe.injection.b;
			}
		}
	}
	const bool injection_changed = next_injection != injection;
	if (injection_changed) {
		injection = next_injection;
	}
	analytic_lights = next_analytic_lights;
	force_light_injection_update = false;
	if (debug_mode == DEBUG_MODE_INJECTION) {
		_update_debug_probe_instances();
	}
}

void LocalLRTVolume3D::set_bake_data(const Ref<LocalLRTVolumeData> &p_data) {
	bake_data = p_data;
	if (bake_data.is_valid() && bake_data->is_valid()) {
		_apply_bake_data();
	} else if (builder) {
		_clear_built_data();
	}
	update_configuration_warnings();
}

Ref<LocalLRTVolumeData> LocalLRTVolume3D::get_bake_data() const {
	return bake_data;
}

void LocalLRTVolume3D::_capture_bake_data(const Vector<Vector4> &p_local_visibility, const Vector<Vector4> &p_local_transfer, const Vector<Vector4> &p_mesh_light, const Vector<int> &p_inside_solid) {
	if (bake_data.is_null()) {
		bake_data.instantiate();
	}
	bake_data->allocate(size, get_resolution(), p_local_visibility, p_local_transfer, p_mesh_light, p_inside_solid);
	update_configuration_warnings();
#ifdef TOOLS_ENABLED
	bake_data->set_edited(true);
#endif
}

void LocalLRTVolume3D::_apply_bake_data() {
	ERR_FAIL_COND(bake_data.is_null() || !bake_data->is_valid());
	_clear_built_data();
	const Vector3 baked_size = bake_data->get_size();
	const Vector3i baked_resolution = bake_data->get_resolution();
	const Transform3D volume_transform = is_inside_tree() ? get_global_transform() : get_transform();
	RS::get_singleton()->local_lrt_volume_set_grid(volume, baked_size, baked_resolution);
	RS::get_singleton()->local_lrt_volume_set_transform(volume, volume_transform);
	builder = memnew(LocalLRTBuilder(baked_size, baked_resolution, volume_transform, false));
	Vector<Vector4> local_visibility;
	Vector<Vector4> local_transfer;
	Vector<Vector4> mesh_light;
	Vector<int> inside_solid;
	ERR_FAIL_COND_MSG(!bake_data->decode(local_visibility, local_transfer, mesh_light, inside_solid), "Failed to decode Local LRT bake data.");
	for (int z = 0; z < baked_resolution.z; z++) {
		for (int y = 0; y < baked_resolution.y; y++) {
			for (int x = 0; x < baked_resolution.x; x++) {
				const Vector3i position(x, y, z);
				const int probe_index = LocalLRTMath::probe_index(position, baked_resolution);
				LocalLRTBuilder::Probe &probe = builder->get_probe(position);
				probe.local_visibility = local_visibility[probe_index];
				probe.inside_solid = inside_solid[probe_index] != 0;
				probe.occupied = probe.inside_solid;
				probe.mesh_light.r = mesh_light[probe_index * 3];
				probe.mesh_light.g = mesh_light[probe_index * 3 + 1];
				probe.mesh_light.b = mesh_light[probe_index * 3 + 2];
				LocalLRTMath::SH2Matrix *channels[] = { &probe.local_transfer.r, &probe.local_transfer.g, &probe.local_transfer.b };
				for (int channel = 0; channel < 3; channel++) {
					for (int row = 0; row < 4; row++) {
						channels[channel]->rows[row] = local_transfer[probe_index * 12 + channel * 4 + row];
					}
				}
			}
		}
	}
	if (!Engine::get_singleton()->is_editor_hint()) {
		Node *root = get_parent();
		if (is_inside_tree() && get_tree()->get_current_scene()) {
			root = get_tree()->get_current_scene();
		}
		if (root) {
			_collect_geometry_sources(root, volume_transform.affine_inverse(), geometry_sources);
		}
		_install_geometry_sources();
	}
	RS::get_singleton()->local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light);
	RS::get_singleton()->local_lrt_volume_set_inside_solid(volume, inside_solid);
	global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
	_sync_global_visibility_to_builder();
	last_geometry_update_probe_count = builder->get_probe_count();
	if (debug_draw && debug_mode == DEBUG_MODE_INJECTION) {
		update_light_injection();
	}
	_update_debug_probe_instances();
	update_gizmos();
}

PackedStringArray LocalLRTVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (bake_data.is_valid() && bake_data->is_valid() && (!size.is_equal_approx(bake_data->get_size()) || _calculate_resolution() != bake_data->get_resolution())) {
		warnings.push_back(RTR("Local LRT bake data does not match the current size or probe spacing. Bake the volume again."));
	}
	return warnings;
}

void LocalLRTVolume3D::rebuild() {
	const uint64_t rebuild_begin = Time::get_singleton()->get_ticks_usec();
	_clear_built_data();
	const Transform3D volume_transform = is_inside_tree() ? get_global_transform() : get_transform();
	RS::get_singleton()->local_lrt_volume_set_grid(volume, size, get_resolution());
	builder = memnew(LocalLRTBuilder(size, get_resolution(), volume_transform, false));
	RS::get_singleton()->local_lrt_volume_set_transform(volume, volume_transform);
	Node *root = get_parent();
	if (is_inside_tree() && get_tree()->get_current_scene()) {
		root = get_tree()->get_current_scene();
	}
	if (root) {
		_collect_geometry_sources(root, volume_transform.affine_inverse(), geometry_sources);
	}
	_install_geometry_sources();
	const uint64_t build_begin = Time::get_singleton()->get_ticks_usec();
	builder->build_local_data();
	last_geometry_build_usec = Time::get_singleton()->get_ticks_usec() - build_begin;

	const uint64_t pack_begin = Time::get_singleton()->get_ticks_usec();
	Vector<Vector4> local_visibility;
	Vector<Vector4> local_transfer;
	Vector<Vector4> mesh_light;
	Vector<int> inside_solid;
	local_visibility.resize(builder->get_probe_count());
	local_transfer.resize(builder->get_probe_count() * 12);
	mesh_light.resize(builder->get_probe_count() * 3);
	inside_solid.resize(builder->get_probe_count());
	for (int z = 0; z < get_resolution().z; z++) {
		for (int y = 0; y < get_resolution().y; y++) {
			for (int x = 0; x < get_resolution().x; x++) {
				const Vector3i position(x, y, z);
				const int probe_index = LocalLRTMath::probe_index(position, get_resolution());
				const LocalLRTBuilder::Probe &probe = builder->get_probe(position);
				local_visibility.write[probe_index] = probe.local_visibility;
				inside_solid.write[probe_index] = probe.inside_solid ? 1 : 0;
				mesh_light.write[probe_index * 3] = probe.mesh_light.r;
				mesh_light.write[probe_index * 3 + 1] = probe.mesh_light.g;
				mesh_light.write[probe_index * 3 + 2] = probe.mesh_light.b;
				const LocalLRTMath::SH2Matrix *channels[] = { &probe.local_transfer.r, &probe.local_transfer.g, &probe.local_transfer.b };
				for (int channel = 0; channel < 3; channel++) {
					for (int row = 0; row < 4; row++) {
						local_transfer.write[probe_index * 12 + channel * 4 + row] = channels[channel]->rows[row];
					}
				}
			}
		}
	}
	last_geometry_pack_usec = Time::get_singleton()->get_ticks_usec() - pack_begin;
	const uint64_t upload_begin = Time::get_singleton()->get_ticks_usec();
	RS::get_singleton()->local_lrt_volume_set_static_data(volume, local_visibility, local_transfer, mesh_light);
	RS::get_singleton()->local_lrt_volume_set_inside_solid(volume, inside_solid);
	_capture_bake_data(local_visibility, local_transfer, mesh_light, inside_solid);
	last_geometry_upload_usec = Time::get_singleton()->get_ticks_usec() - upload_begin;
	global_visibility = RS::get_singleton()->local_lrt_volume_get_global_visibility(volume);
	_sync_global_visibility_to_builder();
	last_geometry_update_probe_count = builder->get_probe_count();
	last_geometry_update_usec = Time::get_singleton()->get_ticks_usec() - rebuild_begin;
	last_geometry_update_frame_count = 1;
	last_geometry_max_build_slice_usec = last_geometry_build_usec;
	if (debug_draw && debug_mode == DEBUG_MODE_INJECTION) {
		update_light_injection();
	}
	_update_debug_probe_instances();
	update_gizmos();
}

LocalLRTVolume3D::LocalLRTVolume3D() {
	volume = RS::get_singleton()->local_lrt_volume_create();
	set_notify_transform(true);
	set_process_internal(true);
	set_disable_scale(true);
	RS::get_singleton()->local_lrt_volume_set_enabled(volume, false);
	RS::get_singleton()->local_lrt_volume_set_visibility_iterations(volume, visibility_iterations);
	RS::get_singleton()->local_lrt_volume_set_propagation_iterations(volume, propagation_iterations);
	RS::get_singleton()->local_lrt_volume_set_visibility_probe_budget(volume, visibility_probe_budget);
	RS::get_singleton()->local_lrt_volume_set_radiance_probe_budget(volume, radiance_probe_budget);
	RS::get_singleton()->local_lrt_volume_set_radiance_neighbor_pattern(volume, radiance_neighbor_pattern);
	RS::get_singleton()->local_lrt_volume_set_energy(volume, energy);
	RS::get_singleton()->local_lrt_volume_set_priority(volume, priority);
	RS::get_singleton()->local_lrt_volume_set_edge_blend_distance(volume, edge_blend_distance);
	_sync_grid();
}

LocalLRTVolume3D::~LocalLRTVolume3D() {
	_clear_built_data();
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(volume);
}
