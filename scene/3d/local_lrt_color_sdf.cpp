/**************************************************************************/
/*  local_lrt_color_sdf.cpp                                               */
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

#include "local_lrt_color_sdf.h"

#include "core/math/math_funcs.h"

real_t LocalLRTColorSDF::signed_distance_box(const Vector3 &p_local_position, const Vector3 &p_half_extents) {
	const Vector3 q = p_local_position.abs() - p_half_extents;
	return Vector3(MAX(q.x, (real_t)0.0), MAX(q.y, (real_t)0.0), MAX(q.z, (real_t)0.0)).length() + MIN(MAX(q.x, MAX(q.y, q.z)), (real_t)0.0);
}

real_t LocalLRTColorSDF::signed_distance_sphere(const Vector3 &p_local_position, real_t p_radius) {
	return p_local_position.length() - p_radius;
}

Vector3 LocalLRTColorSDF::normal_box(const Vector3 &p_local_position, const Vector3 &p_half_extents) {
	const Vector3 q = p_local_position.abs() - p_half_extents;
	if (q.x >= q.y && q.x >= q.z) {
		return Vector3(SIGN(p_local_position.x), 0.0, 0.0);
	}
	if (q.y >= q.z) {
		return Vector3(0.0, SIGN(p_local_position.y), 0.0);
	}
	return Vector3(0.0, 0.0, SIGN(p_local_position.z));
}

Vector3 LocalLRTColorSDF::normal_sphere(const Vector3 &p_local_position) {
	if (p_local_position.length_squared() <= CMP_EPSILON) {
		return Vector3(0.0, 1.0, 0.0);
	}
	return p_local_position.normalized();
}

real_t LocalLRTColorSDF::coverage_from_distance(real_t p_signed_distance, real_t p_voxel_size) {
	if (p_voxel_size <= 0.0) {
		return p_signed_distance < 0.0 ? 1.0 : 0.0;
	}
	return CLAMP((real_t)0.5 - p_signed_distance / p_voxel_size, (real_t)0.0, (real_t)1.0);
}

LocalLRTColorSDF LocalLRTColorSDF::make_box(const Vector3 &p_half_extents, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission) {
	LocalLRTColorSDF sdf;
	ERR_FAIL_COND_V(p_voxel_size <= 0.0, sdf);
	ERR_FAIL_COND_V(p_half_extents.x <= 0.0 || p_half_extents.y <= 0.0 || p_half_extents.z <= 0.0, sdf);
	sdf.type = TYPE_BOX;
	sdf.voxel_size = p_voxel_size;
	sdf.box_half_extents = p_half_extents;
	sdf.bounds = AABB(-p_half_extents, p_half_extents * 2.0);
	sdf.albedo = p_albedo;
	sdf.emission = p_emission;
	return sdf;
}

LocalLRTColorSDF LocalLRTColorSDF::make_sphere(real_t p_radius, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission) {
	LocalLRTColorSDF sdf;
	ERR_FAIL_COND_V(p_voxel_size <= 0.0, sdf);
	ERR_FAIL_COND_V(p_radius <= 0.0, sdf);
	sdf.type = TYPE_SPHERE;
	sdf.voxel_size = p_voxel_size;
	sdf.sphere_radius = p_radius;
	sdf.bounds = AABB(Vector3(-p_radius, -p_radius, -p_radius), Vector3(p_radius, p_radius, p_radius) * 2.0);
	sdf.albedo = p_albedo;
	sdf.emission = p_emission;
	return sdf;
}

Vector<Face3> LocalLRTColorSDF::_faces_from_triangles(const Vector<Vector3> &p_vertices, const Vector<int> &p_indices) {
	Vector<Face3> faces;
	const int count = p_indices.is_empty() ? p_vertices.size() : p_indices.size();
	for (int index = 0; index + 2 < count; index += 3) {
		Face3 face;
		bool valid = true;
		for (int vertex = 0; vertex < 3; vertex++) {
			const int vertex_index = p_indices.is_empty() ? index + vertex : p_indices[index + vertex];
			if (vertex_index < 0 || vertex_index >= p_vertices.size()) {
				valid = false;
				break;
			}
			face.vertex[vertex] = p_vertices[vertex_index];
		}
		if (valid && !face.is_degenerate()) {
			faces.push_back(face);
		}
	}
	return faces;
}

Vector<Face3> LocalLRTColorSDF::_faces_from_mesh(const Ref<Mesh> &p_mesh) {
	Vector<Face3> faces;
	if (p_mesh.is_null()) {
		return faces;
	}
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(surface);
		if (arrays.is_empty()) {
			continue;
		}
		const Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
		const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
		faces.append_array(_faces_from_triangles(vertices, indices));
	}
	return faces;
}

real_t LocalLRTColorSDF::_closest_signed_distance(const Vector3 &p_position, const Vector<Face3> &p_faces) {
	real_t best_distance_squared = 1.0e20;
	Vector3 best_closest;
	Vector3 best_normal;
	for (int index = 0; index < p_faces.size(); index++) {
		const Face3 &face = p_faces[index];
		const Vector3 closest = face.get_closest_point_to(p_position);
		const real_t distance_squared = closest.distance_squared_to(p_position);
		if (distance_squared < best_distance_squared) {
			best_distance_squared = distance_squared;
			best_closest = closest;
			best_normal = face.get_plane().normal;
		}
	}
	if (best_distance_squared >= 1.0e19) {
		return 1.0e20;
	}
	const real_t unsigned_distance = Math::sqrt(best_distance_squared);
	if (unsigned_distance <= CMP_EPSILON) {
		return 0.0;
	}
	if (best_normal.length_squared() <= CMP_EPSILON) {
		return unsigned_distance;
	}
	return best_normal.dot(p_position - best_closest) < 0.0 ? -unsigned_distance : unsigned_distance;
}

void LocalLRTColorSDF::_build_voxel_field(const Vector<Face3> &p_faces) {
	type = TYPE_EMPTY;
	distances.clear();
	resolution = Vector3i();
	actual_voxel_size = Vector3();
	if (p_faces.is_empty() || voxel_size <= 0.0) {
		return;
	}

	AABB aabb = p_faces[0].get_aabb();
	for (int index = 1; index < p_faces.size(); index++) {
		aabb.merge_with(p_faces[index].get_aabb());
	}
	aabb.grow_by(voxel_size);
	if (aabb.size.x <= 0.0 || aabb.size.y <= 0.0 || aabb.size.z <= 0.0) {
		return;
	}

	resolution = Vector3i(
			MAX(2, (int)Math::ceil(aabb.size.x / voxel_size) + 1),
			MAX(2, (int)Math::ceil(aabb.size.y / voxel_size) + 1),
			MAX(2, (int)Math::ceil(aabb.size.z / voxel_size) + 1));
	actual_voxel_size = aabb.size / Vector3(resolution - Vector3i(1, 1, 1));
	bounds = aabb;
	type = TYPE_VOXEL;
	distances.resize(resolution.x * resolution.y * resolution.z);

	for (int z = 0; z < resolution.z; z++) {
		for (int y = 0; y < resolution.y; y++) {
			for (int x = 0; x < resolution.x; x++) {
				const Vector3i position(x, y, z);
				distances.write[_voxel_index(position)] = _closest_signed_distance(get_voxel_local_position(position), p_faces);
			}
		}
	}
}

LocalLRTColorSDF LocalLRTColorSDF::from_triangles(const Vector<Vector3> &p_vertices, const Vector<int> &p_indices, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission) {
	LocalLRTColorSDF sdf;
	ERR_FAIL_COND_V(p_voxel_size <= 0.0, sdf);
	sdf.voxel_size = p_voxel_size;
	sdf.albedo = p_albedo;
	sdf.emission = p_emission;
	sdf._build_voxel_field(_faces_from_triangles(p_vertices, p_indices));
	return sdf;
}

LocalLRTColorSDF LocalLRTColorSDF::from_mesh(const Ref<Mesh> &p_mesh, real_t p_voxel_size, const Color &p_albedo, const Color &p_emission) {
	LocalLRTColorSDF sdf;
	ERR_FAIL_COND_V(p_voxel_size <= 0.0, sdf);
	sdf.voxel_size = p_voxel_size;
	sdf.albedo = p_albedo;
	sdf.emission = p_emission;
	sdf._build_voxel_field(_faces_from_mesh(p_mesh));
	return sdf;
}

int LocalLRTColorSDF::_voxel_index(const Vector3i &p_position) const {
	return p_position.x + resolution.x * (p_position.y + resolution.y * p_position.z);
}

Vector3 LocalLRTColorSDF::get_voxel_local_position(const Vector3i &p_position) const {
	return bounds.position + Vector3(p_position) * actual_voxel_size;
}

real_t LocalLRTColorSDF::_distance_at_voxel(const Vector3i &p_position) const {
	const Vector3i clamped(
			CLAMP(p_position.x, 0, resolution.x - 1),
			CLAMP(p_position.y, 0, resolution.y - 1),
			CLAMP(p_position.z, 0, resolution.z - 1));
	return distances[_voxel_index(clamped)];
}

real_t LocalLRTColorSDF::_interpolate_distance(const Vector3 &p_object_local) const {
	if (distances.is_empty() || resolution.x < 2 || resolution.y < 2 || resolution.z < 2) {
		return 1.0e20;
	}
	const Vector3 grid = (p_object_local - bounds.position) / actual_voxel_size;
	const Vector3i max_index = resolution - Vector3i(1, 1, 1);
	const Vector3i base(
			CLAMP((int)Math::floor(grid.x), 0, max_index.x - 1),
			CLAMP((int)Math::floor(grid.y), 0, max_index.y - 1),
			CLAMP((int)Math::floor(grid.z), 0, max_index.z - 1));
	const Vector3 frac(
			CLAMP(grid.x - (real_t)base.x, (real_t)0.0, (real_t)1.0),
			CLAMP(grid.y - (real_t)base.y, (real_t)0.0, (real_t)1.0),
			CLAMP(grid.z - (real_t)base.z, (real_t)0.0, (real_t)1.0));
	const real_t d000 = _distance_at_voxel(base);
	const real_t d100 = _distance_at_voxel(base + Vector3i(1, 0, 0));
	const real_t d010 = _distance_at_voxel(base + Vector3i(0, 1, 0));
	const real_t d110 = _distance_at_voxel(base + Vector3i(1, 1, 0));
	const real_t d001 = _distance_at_voxel(base + Vector3i(0, 0, 1));
	const real_t d101 = _distance_at_voxel(base + Vector3i(1, 0, 1));
	const real_t d011 = _distance_at_voxel(base + Vector3i(0, 1, 1));
	const real_t d111 = _distance_at_voxel(base + Vector3i(1, 1, 1));
	const real_t c00 = Math::lerp(d000, d100, frac.x);
	const real_t c10 = Math::lerp(d010, d110, frac.x);
	const real_t c01 = Math::lerp(d001, d101, frac.x);
	const real_t c11 = Math::lerp(d011, d111, frac.x);
	return Math::lerp(Math::lerp(c00, c10, frac.y), Math::lerp(c01, c11, frac.y), frac.z);
}

Vector3 LocalLRTColorSDF::_gradient_normal(const Vector3 &p_object_local) const {
	const Vector3 epsilon = actual_voxel_size * 0.5;
	const Vector3 gradient(
			_interpolate_distance(p_object_local + Vector3(epsilon.x, 0.0, 0.0)) - _interpolate_distance(p_object_local - Vector3(epsilon.x, 0.0, 0.0)),
			_interpolate_distance(p_object_local + Vector3(0.0, epsilon.y, 0.0)) - _interpolate_distance(p_object_local - Vector3(0.0, epsilon.y, 0.0)),
			_interpolate_distance(p_object_local + Vector3(0.0, 0.0, epsilon.z)) - _interpolate_distance(p_object_local - Vector3(0.0, 0.0, epsilon.z)));
	if (gradient.length_squared() <= CMP_EPSILON) {
		return Vector3(0.0, 1.0, 0.0);
	}
	return gradient.normalized();
}

LocalLRTColorSDF::Sample LocalLRTColorSDF::_sample_material(real_t p_signed_distance, const Vector3 &p_normal) const {
	Sample sample;
	sample.signed_distance = p_signed_distance;
	sample.coverage = coverage_from_distance(p_signed_distance, voxel_size);
	sample.albedo = albedo;
	sample.emission = emission;
	sample.normal = p_normal;
	return sample;
}

LocalLRTColorSDF::Sample LocalLRTColorSDF::sample(const Vector3 &p_object_local) const {
	if (type == TYPE_BOX) {
		return _sample_material(signed_distance_box(p_object_local, box_half_extents), normal_box(p_object_local, box_half_extents));
	}
	if (type == TYPE_SPHERE) {
		return _sample_material(signed_distance_sphere(p_object_local, sphere_radius), normal_sphere(p_object_local));
	}
	if (type != TYPE_VOXEL) {
		return Sample();
	}
	if (!bounds.has_point(p_object_local)) {
		const Vector3 closest = p_object_local.clamp(bounds.position, bounds.position + bounds.size);
		const real_t outside = p_object_local.distance_to(closest);
		Sample sample = _sample_material(_interpolate_distance(closest) + outside, _gradient_normal(closest));
		if (outside > voxel_size * 0.5) {
			sample.coverage = 0.0;
		}
		return sample;
	}
	return _sample_material(_interpolate_distance(p_object_local), _gradient_normal(p_object_local));
}
