/**************************************************************************/
/*  lrt_geometry_baker.cpp                                                */
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

#include "lrt_geometry_baker.h"

#include "local_lrt_volume_3d.h"
#include "lrt_geometry.h"
#include "lrt_geometry_binding.h"
#include "lrt_material_baker.h"

#include "core/io/marshalls.h"
#include "core/math/dynamic_bvh.h"
#include "core/math/triangle_mesh.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/material.h"

namespace {

using BakeMaterial = LRTMaterialBaker::Surface;

struct FaceAttributes {
	Vector2 uv[3];
	int material = 0;
};

// Geometry predicates must not lose the thin triangles present in imported meshes.
struct Double3 {
	double x = 0;
	double y = 0;
	double z = 0;

	Double3() = default;
	Double3(double p_x, double p_y, double p_z) :
			x(p_x), y(p_y), z(p_z) {}
	explicit Double3(const Vector3 &p_value) :
			x(p_value.x), y(p_value.y), z(p_value.z) {}
	Double3 operator+(const Double3 &p_other) const { return Double3(x + p_other.x, y + p_other.y, z + p_other.z); }
	Double3 operator-(const Double3 &p_other) const { return Double3(x - p_other.x, y - p_other.y, z - p_other.z); }
	Double3 operator*(double p_value) const { return Double3(x * p_value, y * p_value, z * p_value); }
	double dot(const Double3 &p_other) const { return x * p_other.x + y * p_other.y + z * p_other.z; }
	Double3 cross(const Double3 &p_other) const { return Double3(y * p_other.z - z * p_other.y, z * p_other.x - x * p_other.z, x * p_other.y - y * p_other.x); }
};

struct Geometry {
	Ref<TriangleMesh> mesh;
	Vector<FaceAttributes> attributes;
	Vector<BakeMaterial> materials;
	bool closed = true;

	Error prepare(GeometryInstance3D *p_instance, const Transform3D &p_transform = Transform3D(), float p_texel_size = 0, int p_instance_index = 0) {
		ERR_FAIL_COND_V(!p_transform.is_finite(), ERR_INVALID_DATA);
		const Ref<Mesh> source = LRTGeometryBinding::get_mesh(p_instance);
		ERR_FAIL_COND_V_MSG(source.is_null(), ERR_INVALID_DATA, "LRT cannot bake an instance without a mesh.");
		if (MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_instance)) {
			ERR_FAIL_COND_V_MSG(instance->get_skin().is_valid(), ERR_UNAVAILABLE, "LRT geometry baking requires a mesh without skeletal deformation.");
			for (int shape = 0; shape < source->get_blend_shape_count(); shape++) {
				ERR_FAIL_COND_V_MSG(instance->get_blend_shape_value(shape) != 0, ERR_UNAVAILABLE, "LRT geometry baking requires undeformed mesh geometry.");
			}
		}

		if (p_texel_size == 0) {
			p_texel_size = source->get_aabb().get_longest_axis_size() / (32 - 5) * 0.25f;
		}
		Vector<Vector3> faces;
		bool has_triangles = false;
		for (int surface = 0; surface < source->get_surface_count(); surface++) {
			if (source->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			has_triangles = true;
			BakeMaterial material;
			const Error error = LRTMaterialBaker::bake(p_instance, surface, p_texel_size, material, p_instance_index);
			if (error != OK) {
				return error;
			}
			if (material.transparent) {
				continue;
			}
			const int material_index = materials.size();
			materials.push_back(material);
			for (int face = 0; face < material.positions.size(); face += 3) {
				Vector3 positions[3];
				for (int corner = 0; corner < 3; corner++) {
					ERR_FAIL_COND_V(!material.positions[face + corner].is_finite(), ERR_INVALID_DATA);
					positions[corner] = p_transform.xform(material.positions[face + corner]);
					ERR_FAIL_COND_V(!positions[corner].is_finite(), ERR_INVALID_DATA);
				}
				const Double3 a(positions[0]), b(positions[1]), c(positions[2]);
				const Double3 normal = (b - a).cross(c - a);
				// Remove only exact degenerates, preserving thin imported triangles.
				if (normal.dot(normal) == 0) {
					continue;
				}
				FaceAttributes values;
				values.material = material_index;
				for (int corner = 0; corner < 3; corner++) {
					faces.push_back(positions[corner]);
					values.uv[corner] = material.atlas_uv[face + corner];
				}
				attributes.push_back(values);
			}
		}
		ERR_FAIL_COND_V_MSG(!has_triangles, ERR_INVALID_DATA, "LRT baking requires triangle surfaces.");
		if (faces.is_empty()) {
			return OK; // Transparent or fully degenerate geometry has no contribution.
		}
		mesh.instantiate();
		mesh->create(faces, Vector<int32_t>(), false);

		struct Edge {
			int count = 0;
			int orientation = 0;
		};
		HashMap<uint64_t, Edge> edges;
		for (const TriangleMesh::Triangle &triangle : mesh->get_triangles()) {
			for (int edge = 0; edge < 3; edge++) {
				const int from = triangle.indices[edge];
				const int to = triangle.indices[(edge + 1) % 3];
				const uint64_t key = (uint64_t(MIN(from, to)) << 32) | uint32_t(MAX(from, to));
				Edge &value = edges[key];
				value.count++;
				value.orientation += from < to ? 1 : -1;
			}
		}
		for (const KeyValue<uint64_t, Edge> &edge : edges) {
			if (edge.value.count != 2 || edge.value.orientation != 0) {
				closed = false;
				break;
			}
		}
		return OK;
	}

	Face3 face(int p_index) const {
		const TriangleMesh::Triangle &triangle = mesh->get_triangles()[p_index];
		const Vector<Vector3> &vertices = mesh->get_vertices();
		return Face3(vertices[triangle.indices[0]], vertices[triangle.indices[1]], vertices[triangle.indices[2]]);
	}

	static Double3 closest_point(const Face3 &p_triangle, const Double3 &p_point) {
		const Double3 vertices[3] = { Double3(p_triangle.vertex[0]), Double3(p_triangle.vertex[1]), Double3(p_triangle.vertex[2]) };
		const Double3 ab = vertices[1] - vertices[0];
		const Double3 ac = vertices[2] - vertices[0];
		const Double3 normal = ab.cross(ac);
		const double area_squared = normal.dot(normal);
		const Double3 projection = p_point - normal * (normal.dot(p_point - vertices[0]) / area_squared);
		const Double3 local = projection - vertices[0];
		const double b = local.cross(ac).dot(normal) / area_squared;
		const double c = ab.cross(local).dot(normal) / area_squared;
		if (b >= 0 && c >= 0 && b + c <= 1) {
			return projection;
		}
		Double3 closest;
		double best = INFINITY;
		for (int edge = 0; edge < 3; edge++) {
			const Double3 start = vertices[edge];
			const Double3 direction = vertices[(edge + 1) % 3] - start;
			const double t = CLAMP((p_point - start).dot(direction) / direction.dot(direction), 0.0, 1.0);
			const Double3 point = start + direction * t;
			const Double3 offset = p_point - point;
			const double distance_squared = offset.dot(offset);
			if (distance_squared < best) {
				best = distance_squared;
				closest = point;
			}
		}
		return closest;
	}

	bool intersect_node(int p_node, const Vector3 &p_from, const Vector3 &p_to, double &r_t, int &r_face, Double3 &r_point, bool p_any_hit) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (!node.aabb.intersects_segment(p_from, p_to)) {
			return false;
		}
		if (node.face_index < 0) {
			const bool left = intersect_node(node.left, p_from, p_to, r_t, r_face, r_point, p_any_hit);
			if (left && p_any_hit) {
				return true;
			}
			const bool right = intersect_node(node.right, p_from, p_to, r_t, r_face, r_point, p_any_hit);
			return left || right;
		}
		const Face3 triangle = face(node.face_index);
		const Double3 origin(p_from);
		const Double3 direction = Double3(p_to) - origin;
		const Double3 ab = Double3(triangle.vertex[1]) - Double3(triangle.vertex[0]);
		const Double3 ac = Double3(triangle.vertex[2]) - Double3(triangle.vertex[0]);
		const Double3 h = direction.cross(ac);
		const double determinant = ab.dot(h);
		if (determinant == 0) {
			return false;
		}
		const Double3 relative = origin - Double3(triangle.vertex[0]);
		const double u = relative.dot(h) / determinant;
		if (u < 0 || u > 1) {
			return false;
		}
		const Double3 q = relative.cross(ab);
		const double v = direction.dot(q) / determinant;
		if (v < 0 || u + v > 1) {
			return false;
		}
		const double t = ac.dot(q) / determinant;
		// Use (0, 1]: a probe on a surface does not re-hit its own origin.
		if (t <= 0 || t > r_t) {
			return false;
		}
		r_t = t;
		r_face = node.face_index;
		r_point = origin + direction * t;
		return true;
	}

	bool intersect_segment(const Vector3 &p_from, const Vector3 &p_to, int *r_face = nullptr, Double3 *r_point = nullptr) const {
		double t = 1;
		int face_index = -1;
		Double3 point;
		if (!intersect_node(mesh->get_bvh().size() - 1, p_from, p_to, t, face_index, point, !r_face && !r_point)) {
			return false;
		}
		if (r_face) {
			*r_face = face_index;
		}
		if (r_point) {
			*r_point = point;
		}
		return true;
	}

	static double aabb_distance_squared(const AABB &p_bounds, const Vector3 &p_point) {
		const Vector3 end = p_bounds.get_end();
		double result = 0;
		for (int axis = 0; axis < 3; axis++) {
			const double delta = MAX(double(p_bounds.position[axis]) - p_point[axis], 0.0) + MAX(double(p_point[axis]) - end[axis], 0.0);
			result += delta * delta;
		}
		return result;
	}

	void nearest(int p_node, const Vector3 &p_point, double &r_distance_squared, int &r_face, Double3 &r_closest) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (aabb_distance_squared(node.aabb, p_point) > r_distance_squared) {
			return;
		}
		if (node.face_index >= 0) {
			const Double3 point = closest_point(face(node.face_index), Double3(p_point));
			const Double3 delta = point - Double3(p_point);
			const double distance_squared = delta.dot(delta);
			if (distance_squared < r_distance_squared) {
				r_distance_squared = distance_squared;
				r_face = node.face_index;
				r_closest = point;
			}
			return;
		}
		const bool left_first = aabb_distance_squared(mesh->get_bvh()[node.left].aabb, p_point) <= aabb_distance_squared(mesh->get_bvh()[node.right].aabb, p_point);
		nearest(left_first ? node.left : node.right, p_point, r_distance_squared, r_face, r_closest);
		nearest(left_first ? node.right : node.left, p_point, r_distance_squared, r_face, r_closest);
	}

	static double edge(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_point) {
		return (double(p_b.y) - p_a.y) * (double(p_point.z) - p_a.z) - (double(p_b.z) - p_a.z) * (double(p_point.y) - p_a.y);
	}

	static bool covered(double p_edge, const Vector3 &p_a, const Vector3 &p_b) {
		// A half-open projected triangle counts shared edges exactly once.
		return p_edge > 0 || (p_edge == 0 && (p_b.z > p_a.z || (p_b.z == p_a.z && p_b.y < p_a.y)));
	}

	int winding(int p_node, const Vector3 &p_point) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (!node.aabb.intersects_ray(p_point, Vector3(1, 0, 0))) {
			return 0;
		}
		if (node.face_index < 0) {
			return winding(node.left, p_point) + winding(node.right, p_point);
		}
		const Face3 triangle = face(node.face_index);
		Vector3 a = triangle.vertex[0], b = triangle.vertex[1], c = triangle.vertex[2];
		double area = edge(a, b, c);
		if (area == 0) {
			return 0;
		}
		const int sign = area > 0 ? 1 : -1;
		if (area < 0) {
			SWAP(b, c);
			area = -area;
		}
		const double e0 = edge(b, c, p_point), e1 = edge(c, a, p_point), e2 = edge(a, b, p_point);
		if (!covered(e0, b, c) || !covered(e1, c, a) || !covered(e2, a, b)) {
			return 0;
		}
		const double x = (a.x * e0 + b.x * e1 + c.x * e2) / area;
		return x >= p_point.x ? sign : 0;
	}

	bool on_surface(int p_node, const Vector3 &p_point) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (aabb_distance_squared(node.aabb, p_point) != 0) {
			return false;
		}
		if (node.face_index < 0) {
			return on_surface(node.left, p_point) || on_surface(node.right, p_point);
		}
		const Double3 delta = closest_point(face(node.face_index), Double3(p_point)) - Double3(p_point);
		return delta.dot(delta) == 0;
	}

	bool contains(const Vector3 &p_point) const {
		if (!mesh->get_aabb().has_point(p_point)) {
			return false;
		}
		const int root = mesh->get_bvh().size() - 1;
		if (closed) {
			// For closed manifolds, a BVH ray gives the exact integer winding number.
			if (winding(root, p_point) != 0) {
				return true;
			}
		} else {
			// Generalized winding number: more than half the sphere is enclosed.
			// Open and nonmanifold meshes must not use a single ray's integer count.
			double solid_angle = 0;
			for (int index = 0; index < mesh->get_triangles().size(); index++) {
				const Face3 triangle = face(index);
				const Double3 a = Double3(triangle.vertex[0]) - Double3(p_point);
				const Double3 b = Double3(triangle.vertex[1]) - Double3(p_point);
				const Double3 c = Double3(triangle.vertex[2]) - Double3(p_point);
				const double numerator = a.dot(b.cross(c));
				if (numerator == 0) {
					continue; // Coplanar points are handled by the surface check below.
				}
				const double la = Math::sqrt(a.dot(a));
				const double lb = Math::sqrt(b.dot(b));
				const double lc = Math::sqrt(c.dot(c));
				const double denominator = la * lb * lc + a.dot(b) * lc + b.dot(c) * la + c.dot(a) * lb;
				solid_angle += 2.0 * Math::atan2(numerator, denominator);
			}
			if (Math::abs(solid_angle) > 2.0 * Math::PI) {
				return true;
			}
		}
		// Only exact surface membership is needed, not the globally nearest face.
		return on_surface(root, p_point);
	}

	void sample_material(int p_face, const Double3 &p_closest, Color &r_albedo, Color &r_emission) const {
		const Face3 triangle = face(p_face);
		const Double3 u = Double3(triangle.vertex[1]) - Double3(triangle.vertex[0]);
		const Double3 v = Double3(triangle.vertex[2]) - Double3(triangle.vertex[0]);
		const Double3 normal = u.cross(v);
		const Double3 point = p_closest - Double3(triangle.vertex[0]);
		const double denominator = normal.dot(normal);
		const real_t b = point.cross(v).dot(normal) / denominator;
		const real_t c = u.cross(point).dot(normal) / denominator;
		const real_t a = 1 - b - c;
		const FaceAttributes &values = attributes[p_face];
		materials[values.material].sample(values.uv[0] * a + values.uv[1] * b + values.uv[2] * c, r_albedo, r_emission);
	}
};

// Broad phase over instance bounds; the referenced vector stays fixed during queries.
struct SolidBVH {
	DynamicBVH tree;

	explicit SolidBVH(Vector<Geometry> &p_solids) {
		Geometry *solids = p_solids.ptrw();
		for (int index = 0; index < p_solids.size(); index++) {
			tree.insert(solids[index].mesh->get_aabb(), &solids[index]);
		}
		tree.optimize_top_down();
	}

	bool contains(const Vector3 &p_point) {
		bool inside = false;
		auto query = [&](void *p_data) {
			inside = static_cast<const Geometry *>(p_data)->contains(p_point);
			return inside;
		};
		tree.aabb_query(AABB(p_point, Vector3()), query);
		return inside;
	}
};

constexpr float SH_C0 = 0.28209479177387814f;
constexpr float SH_C1 = 0.4886025119029199f;
constexpr float DIRECTION_WEIGHT = 4.0f * Math::PI / 26.0f;

Error collect_static_geometry(LocalLRTVolume3D *p_volume, Node *p_from_node, Geometry &r_combined, Vector<Geometry> &r_solids, Array &r_sources, LRTGeometryBaker::BakeStepFunc p_step) {
	const LRTGeometry::VolumeFrame volume_frame(p_volume);
	// The last probe can extend one cell beyond coverage; its neighbors extend another.
	const AABB query_bounds = p_volume->get_bounds().grow(p_volume->get_probe_spacing() * 2.05f);
	Vector<Node *> pending;
	pending.push_back(p_from_node);
	Vector<Vector3> faces;
	while (!pending.is_empty()) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		for (int child = node->get_child_count() - 1; child >= 0; child--) {
			pending.push_back(node->get_child(child));
		}
		GeometryInstance3D *instance = Object::cast_to<GeometryInstance3D>(node);
		if (!LRTGeometryBinding::supports(instance) || instance->get_gi_mode() != GeometryInstance3D::GI_MODE_STATIC || !instance->is_visible_in_tree()) {
			continue;
		}
		const Ref<Mesh> mesh = LRTGeometryBinding::get_mesh(instance);
		if (mesh.is_null()) {
			continue;
		}
		if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(instance)) {
			ERR_FAIL_COND_V_MSG(multi->get_multimesh()->get_transform_format() != MultiMesh::TRANSFORM_3D, ERR_UNAVAILABLE, "LRT requires a 3D MultiMesh.");
		}
		const Transform3D node_transform = volume_frame.relative_transform(instance);
		for (int subinstance = 0; subinstance < LRTGeometryBinding::get_instance_count(instance); subinstance++) {
			const Transform3D transform = node_transform * LRTGeometryBinding::get_instance_transform(instance, subinstance);
			ERR_FAIL_COND_V_MSG(!transform.is_finite() || !transform.basis.is_conformal() || transform.basis.determinant() <= 0, ERR_INVALID_DATA, "LRT instances require a positive uniform scale without shear.");
			if (p_step && p_step(0, 1)) {
				return ERR_SKIP;
			}
			Geometry geometry;
			const Error error = geometry.prepare(instance, transform, p_volume->get_probe_spacing() * 0.25f / transform.basis.get_scale().x, subinstance);
			if (error != OK) {
				ERR_PRINT(vformat("LRT could not bake static geometry at %s, instance %d.", instance->get_path(), subinstance));
				return error;
			}
			if (geometry.mesh.is_null() || !query_bounds.intersects_inclusive(geometry.mesh->get_aabb())) {
				continue;
			}
			const int material_offset = r_combined.materials.size();
			r_combined.materials.append_array(geometry.materials);
			for (int index = 0; index < geometry.attributes.size(); index++) {
				const Face3 face = geometry.face(index);
				for (int corner = 0; corner < 3; corner++) {
					faces.push_back(face.vertex[corner]);
				}
				FaceAttributes attributes = geometry.attributes[index];
				attributes.material += material_offset;
				r_combined.attributes.push_back(attributes);
			}
			r_solids.push_back(geometry);
			Dictionary source;
			source["node_path"] = p_volume->get_path_to(instance);
			source["instance_index"] = subinstance;
			source["mesh"] = mesh;
			source["transform"] = transform;
			Array materials;
			for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
				materials.push_back(LRTGeometryBinding::get_material(instance, surface));
			}
			source["materials"] = materials;
			r_sources.push_back(source);
		}
	}
	if (!faces.is_empty()) {
		r_combined.mesh.instantiate();
		r_combined.mesh->create(faces, Vector<int32_t>(), false);
	}
	return OK;
}

void encode_ltm(const float p_matrix[3][16], bool p_inside, uint8_t *r_data) {
	float magnitude[3];
	for (int channel = 0; channel < 3; channel++) {
		magnitude[channel] = MAX(p_matrix[channel][0], 0.0f);
	}
	const float maximum = MAX(magnitude[0], MAX(magnitude[1], magnitude[2]));
	const float weights[3] = { 0.2126f, 0.7152f, 0.0722f };
	float color[3] = {};
	float luminance = 0;
	if (maximum > 0) {
		for (int channel = 0; channel < 3; channel++) {
			color[channel] = magnitude[channel] / maximum;
			luminance += weights[channel] * color[channel];
		}
	}
	for (int coefficient = 0; coefficient < 16; coefficient++) {
		float value = 0;
		if (luminance > 0) {
			for (int channel = 0; channel < 3; channel++) {
				value += weights[channel] * p_matrix[channel][coefficient];
			}
			value /= luminance;
		}
		encode_half(value, r_data + coefficient * 2);
	}
	for (int channel = 0; channel < 3; channel++) {
		r_data[32 + channel] = uint8_t(Math::round(color[channel] * 255.0f));
	}
	r_data[35] = p_inside ? 1 : 0;
}

uint8_t bake_edge_mask(const Geometry &p_geometry, SolidBVH &p_solids, const AABB &p_bounds, float p_spacing, const Vector3i &p_probe, int p_direction) {
	if (p_geometry.mesh.is_null()) {
		return 15;
	}
	Vector3 from[4], to[4];
	LRTGeometry::edge_segments(p_bounds, p_spacing, p_probe, p_direction, from, to);
	uint8_t mask = 0;
	for (int sample = 0; sample < 4; sample++) {
		bool blocked = p_geometry.intersect_segment(from[sample], to[sample]);
		if (!blocked) {
			blocked = p_solids.contains(from[sample]);
		}
		if (!blocked) {
			mask |= 1 << sample;
		}
	}
	return mask;
}

struct EdgeBake {
	const Geometry &geometry;
	SolidBVH &solids;
	const AABB &bounds;
	float spacing;
	Vector3i grid;
	const Vector3i *neighbors;
	uint8_t *output;
	SafeFlag cancelled{ false };

	void bake_slice(uint32_t p_z, void *) {
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				if (cancelled.is_set()) {
					return;
				}
				const Vector3i probe(x, y, p_z);
				const int64_t index = (int64_t(p_z) * grid.y + y) * grid.x + x;
				for (int neighbor = 0; neighbor < 26; neighbor++) {
					const Vector3i adjacent = probe + neighbors[neighbor];
					const bool in_grid = adjacent.x >= 0 && adjacent.y >= 0 && adjacent.z >= 0 && adjacent.x < grid.x && adjacent.y < grid.y && adjacent.z < grid.z;
					const int64_t adjacent_index = (int64_t(adjacent.z) * grid.y + adjacent.y) * grid.x + adjacent.x;
					if (in_grid && adjacent_index < index) {
						continue;
					}
					const uint8_t mask = bake_edge_mask(geometry, solids, bounds, spacing, probe, neighbor);
					// One task owns both bytes of a shared edge; no worker reads another's output.
					output[index * 26 + neighbor] = mask;
					if (in_grid) {
						output[adjacent_index * 26 + (25 - neighbor)] = mask;
					}
				}
			}
		}
	}
};

} // namespace

Error LRTGeometryBaker::bake_geometry(GeometryInstance3D *p_instance, int p_resolution, Ref<LRTInstanceGeometryData> &r_data, BakeStepFunc p_step) {
	ERR_FAIL_COND_V(!LRTGeometryBinding::supports(p_instance), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(LRTGeometryBinding::get_mesh(p_instance).is_null(), ERR_INVALID_DATA);
	MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(p_instance);
	if (!multi) {
		return bake_instance(p_instance, p_resolution, r_data, p_step);
	}
	ERR_FAIL_COND_V_MSG(multi->get_multimesh()->get_transform_format() != MultiMesh::TRANSFORM_3D, ERR_UNAVAILABLE, "LRT requires a 3D MultiMesh.");
	// Bake every allocated instance, including those hidden by visible_instance_count.
	Array instances;
	for (int index = 0; index < LRTGeometryBinding::get_instance_count(p_instance, false); index++) {
		if (p_step && p_step(0, 1)) {
			return ERR_SKIP;
		}
		Ref<LRTInstanceGeometryData> baked;
		const Error error = bake_instance(p_instance, p_resolution, baked, p_step, index);
		if (error != OK) {
			return error;
		}
		// Equal evaluated snapshots share their payload, not merely their source mesh.
		for (int previous = 0; previous < instances.size(); previous++) {
			const Ref<LRTInstanceGeometryData> candidate = instances[previous];
			if (candidate->get_data() == baked->get_data()) {
				baked = candidate;
				break;
			}
		}
		instances.push_back(baked);
	}
	Dictionary data;
	data["instances"] = instances;
	r_data.instantiate();
	r_data->set_data(data);
	return OK;
}

Error LRTGeometryBaker::bake_instance(GeometryInstance3D *p_instance, int p_resolution, Ref<LRTInstanceGeometryData> &r_data, BakeStepFunc p_step, int p_instance_index) {
	ERR_FAIL_NULL_V(p_instance, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_resolution < 6, ERR_INVALID_PARAMETER);

	const Ref<Mesh> mesh = LRTGeometryBinding::get_mesh(p_instance);
	ERR_FAIL_COND_V(mesh.is_null(), ERR_INVALID_DATA);
	Geometry geometry;
	Error error = geometry.prepare(p_instance, Transform3D(), mesh->get_aabb().get_longest_axis_size() / (p_resolution - 5) * 0.25f, p_instance_index);
	if (error != OK) {
		return error;
	}
	if (geometry.mesh.is_null()) {
		// Persist an explicit receiver-only bake, so Dynamic bindings can be
		// excluded without querying GPU shader classification every runtime frame.
		Dictionary data;
		const AABB bounds = mesh->get_aabb().grow(0.001f);
		data["bounds"] = bounds;
		data["grid_size"] = Vector3i(2, 2, 2);
		data["distance_scale"] = bounds.size.length();
		PackedByteArray distance;
		distance.resize(16);
		for (int index = 0; index < 8; index++) {
			encode_uint16(32767, distance.ptrw() + index * 2);
		}
		PackedByteArray color;
		color.resize(32);
		color.fill(0);
		data["distance"] = distance;
		data["color"] = color;
		data["emission"] = PackedFloat32Array();
		data["receiver_only"] = true;
		data["source_mesh"] = mesh;
		r_data.instantiate();
		r_data->set_data(data);
		return OK;
	}
	const AABB mesh_bounds = geometry.mesh->get_aabb();
	const int longest_axis = mesh_bounds.get_longest_axis_index();
	const real_t cell_size = mesh_bounds.size[longest_axis] / (p_resolution - 5);
	ERR_FAIL_COND_V(cell_size <= 0, ERR_INVALID_DATA);
	Vector3i grid;
	for (int axis = 0; axis < 3; axis++) {
		grid[axis] = axis == longest_axis ? p_resolution : int(Math::ceil(double(mesh_bounds.size[axis]) / cell_size)) + 5;
	}
	int64_t count = grid.x;
	ERR_FAIL_COND_V(count > INT32_MAX / 4 / grid.y, ERR_OUT_OF_MEMORY);
	count *= grid.y;
	ERR_FAIL_COND_V(count > INT32_MAX / 4 / grid.z, ERR_OUT_OF_MEMORY);
	count *= grid.z;
	const AABB bounds(mesh_bounds.position - Vector3(1, 1, 1) * cell_size * 2, Vector3(grid - Vector3i(1, 1, 1)) * cell_size);
	const float distance_scale = bounds.size.length();

	Vector<uint8_t> distance;
	Vector<uint8_t> color;
	Vector<float> emission;
	ERR_FAIL_COND_V(distance.resize(count * 2) != OK || color.resize(count * 4) != OK, ERR_OUT_OF_MEMORY);
	bool has_emission = false;
	for (const BakeMaterial &material : geometry.materials) {
		has_emission |= material.has_emission;
	}
	if (has_emission) {
		ERR_FAIL_COND_V(emission.resize(count * 3) != OK, ERR_OUT_OF_MEMORY);
	}
	uint8_t *distance_write = distance.ptrw();
	uint8_t *color_write = color.ptrw();
	float *emission_write = emission.ptrw();
	const int root = geometry.mesh->get_bvh().size() - 1;
	for (int z = 0; z < grid.z; z++) {
		if (p_step && p_step(z, grid.z)) {
			return ERR_SKIP;
		}
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				const int64_t index = (int64_t(z) * grid.y + y) * grid.x + x;
				const Vector3 point = bounds.position + Vector3(x, y, z) * cell_size;
				double distance_squared = INFINITY;
				int face = -1;
				Double3 closest;
				geometry.nearest(root, point, distance_squared, face, closest);
				ERR_FAIL_COND_V(face < 0, ERR_INVALID_DATA);
				const bool inside = geometry.contains(point);
				const real_t signed_distance = Math::sqrt(distance_squared) * (inside ? -1 : 1);
				const int quantized = CLAMP(int(Math::round(signed_distance / distance_scale * 32767.0)), -32767, 32767);
				encode_uint16(uint16_t(int16_t(quantized)), distance_write + index * 2);

				Color albedo;
				Color light;
				geometry.sample_material(face, closest, albedo, light);
				color_write[index * 4 + 0] = uint8_t(CLAMP(Math::round(albedo.r * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 1] = uint8_t(CLAMP(Math::round(albedo.g * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 2] = uint8_t(CLAMP(Math::round(albedo.b * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 3] = 255;
				if (has_emission) {
					emission_write[index * 3 + 0] = light.r;
					emission_write[index * 3 + 1] = light.g;
					emission_write[index * 3 + 2] = light.b;
				}
			}
		}
	}

	Dictionary data;
	data["bounds"] = bounds;
	data["grid_size"] = grid;
	data["distance_scale"] = distance_scale;
	data["distance"] = distance;
	data["color"] = color;
	data["emission"] = emission;
	data["source_mesh"] = mesh;
	Array source_materials;
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		source_materials.push_back(LRTGeometryBinding::get_material(p_instance, surface));
	}
	data["source_materials"] = source_materials;
	data["source_resolution"] = p_resolution;
	Ref<LRTInstanceGeometryData> baked;
	baked.instantiate();
	baked->set_data(data);
	ERR_FAIL_COND_V(!baked->has_data(), ERR_INVALID_DATA);
	r_data = baked;
	return OK;
}

Dictionary LRTGeometryBaker::inspect_probe(LocalLRTVolume3D *p_volume, const Vector3i &p_coordinate) {
	Dictionary result;
	ERR_FAIL_NULL_V(p_volume, result);
	Vector3i grid = p_volume->get_grid_size();
	if (!p_volume->is_inside_tree() || p_coordinate.x < 0 || p_coordinate.y < 0 || p_coordinate.z < 0 || p_coordinate.x >= grid.x || p_coordinate.y >= grid.y || p_coordinate.z >= grid.z) {
		result["error"] = "Probe coordinate is outside the volume grid.";
		return result;
	}
	Geometry combined;
	Vector<Geometry> solids;
	Array sources;
	Error error = collect_static_geometry(p_volume, p_volume->get_tree()->get_edited_scene_root() ? p_volume->get_tree()->get_edited_scene_root() : p_volume->get_tree()->get_current_scene(), combined, solids, sources, nullptr);
	if (error != OK) {
		result["error"] = error;
		return result;
	}
	SolidBVH solid_bvh(solids);
	Array samples;
	AABB bounds = p_volume->get_bounds();
	float spacing = p_volume->get_probe_spacing();
	Vector3 point = bounds.position + Vector3(p_coordinate) * spacing;
	for (int d = 0; d < 26; d++) {
		Dictionary sample;
		Vector3 q = point + Vector3(LRTGeometry::direction(d).offset) * spacing;
		Double3 hit;
		int face = -1;
		bool occupied = combined.mesh.is_valid() && combined.intersect_segment(point, q, &face, &hit);
		Color color;
		Color emission;
		if (occupied) {
			combined.sample_material(face, hit, color, emission);
		}
		sample["position"] = q;
		sample["hit"] = occupied;
		sample["albedo"] = color;
		sample["emission"] = emission;
		sample["edge_mask"] = bake_edge_mask(combined, solid_bvh, bounds, spacing, p_coordinate, d);
		samples.push_back(sample);
	}
	result["coordinate"] = p_coordinate;
	result["point"] = point;
	result["samples"] = samples;
	return result;
}

Error LRTGeometryBaker::bake_volume(LocalLRTVolume3D *p_volume, Node *p_from_node, Ref<LocalLRTVolumeData> &r_data, BakeStepFunc p_step) {
	ERR_FAIL_NULL_V(p_volume, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_from_node, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!p_volume->is_inside_tree() || !p_from_node->is_inside_tree(), ERR_UNCONFIGURED);
	const Transform3D transform = p_volume->get_global_transform();
	ERR_FAIL_COND_V_MSG(!transform.is_finite() || !transform.basis.is_conformal() || transform.basis.determinant() <= 0,
			ERR_INVALID_PARAMETER, "LRT volumes require a rotation and positive uniform scale.");
	const Vector3i grid = p_volume->get_grid_size();
	ERR_FAIL_COND_V(grid.x < 2 || grid.y < 2 || grid.z < 2, ERR_INVALID_DATA);
	int64_t count = grid.x;
	ERR_FAIL_COND_V(count > INT32_MAX / 36 / grid.y, ERR_OUT_OF_MEMORY);
	count *= grid.y;
	ERR_FAIL_COND_V(count > INT32_MAX / 36 / grid.z, ERR_OUT_OF_MEMORY);
	count *= grid.z;

	Geometry combined;
	Vector<Geometry> solids;
	Array sources;
	Error error = collect_static_geometry(p_volume, p_from_node, combined, solids, sources, p_step);
	if (error != OK) {
		return error;
	}
	SolidBVH solid_bvh(solids);
	Vector<uint8_t> ltm;
	Vector<uint8_t> local_vis;
	Vector<uint8_t> edge_masks;
	Vector<float> emission;
	ERR_FAIL_COND_V(ltm.resize(count * 36) != OK || local_vis.resize(count * 8) != OK || edge_masks.resize(count * 26) != OK, ERR_OUT_OF_MEMORY);
	bool has_emission = false;
	for (const BakeMaterial &material : combined.materials) {
		has_emission |= material.has_emission;
	}
	if (has_emission) {
		ERR_FAIL_COND_V(emission.resize(count * 12) != OK, ERR_OUT_OF_MEMORY);
	}
	uint8_t *ltm_write = ltm.ptrw();
	uint8_t *vis_write = local_vis.ptrw();
	uint8_t *edge_write = edge_masks.ptrw();
	float *emission_write = emission.ptrw();

	Vector3i neighbors[26];
	float sh[26][4];
	float diffuse[26][4];
	int direction = 0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				if (x == 0 && y == 0 && z == 0) {
					continue;
				}
				neighbors[direction] = Vector3i(x, y, z);
				const Vector3 normal = Vector3(x, y, z).normalized();
				sh[direction][0] = SH_C0;
				diffuse[direction][0] = SH_C0;
				for (int axis = 0; axis < 3; axis++) {
					sh[direction][axis + 1] = SH_C1 * normal[axis];
					diffuse[direction][axis + 1] = -(2.0f / 3.0f) * SH_C1 * normal[axis];
				}
				direction++;
			}
		}
	}

	const AABB bounds = p_volume->get_bounds();
	const float spacing = p_volume->get_probe_spacing();
	for (int z = 0; z < grid.z; z++) {
		if (p_step && p_step(z, grid.z * 2)) {
			return ERR_SKIP;
		}
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				const int64_t index = (int64_t(z) * grid.y + y) * grid.x + x;
				const Vector3 point = bounds.position + Vector3(x, y, z) * spacing;
				const bool inside = solid_bvh.contains(point);
				float matrix[3][16] = {};
				float visibility[4] = {};
				float source[12] = {};
				if (!inside) {
					visibility[0] = 1.0f / SH_C0;
					if (combined.mesh.is_valid()) {
						for (int neighbor = 0; neighbor < 26; neighbor++) {
							const Vector3 end = point + Vector3(neighbors[neighbor]) * spacing;
							Double3 hit;
							int face = -1;
							if (!combined.intersect_segment(point, end, &face, &hit)) {
								continue;
							}
							Color albedo;
							Color light;
							combined.sample_material(face, hit, albedo, light);
							for (int row = 0; row < 4; row++) {
								visibility[row] -= DIRECTION_WEIGHT * sh[neighbor][row];
								for (int channel = 0; channel < 3; channel++) {
									const float flip = row == 0 ? 1.0f : -1.0f;
									source[channel * 4 + row] += DIRECTION_WEIGHT * light[channel] * sh[neighbor][row] * flip;
									for (int column = 0; column < 4; column++) {
										matrix[channel][row + column * 4] += DIRECTION_WEIGHT * albedo[channel] * diffuse[neighbor][row] * sh[neighbor][column];
									}
								}
							}
						}
					}
				}
				encode_ltm(matrix, inside, ltm_write + index * 36);
				for (int coefficient = 0; coefficient < 4; coefficient++) {
					encode_half(visibility[coefficient], vis_write + index * 8 + coefficient * 2);
				}
				if (has_emission) {
					for (int coefficient = 0; coefficient < 12; coefficient++) {
						emission_write[index * 12 + coefficient] = source[coefficient];
					}
				}
			}
		}
	}
	EdgeBake edge_bake{ combined, solid_bvh, bounds, spacing, grid, neighbors, edge_write };
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const WorkerThreadPool::GroupID task = pool->add_template_group_task(&edge_bake, &EdgeBake::bake_slice, nullptr, grid.z);
	while (!pool->is_group_task_completed(task)) {
		if (p_step && p_step(grid.z + pool->get_group_processed_element_count(task), grid.z * 2)) {
			edge_bake.cancelled.set();
		}
		OS::get_singleton()->delay_usec(10000);
	}
	pool->wait_for_group_task_completion(task);
	if (edge_bake.cancelled.is_set()) {
		return ERR_SKIP;
	}
	Dictionary data;
	data["bounds"] = bounds;
	data["probe_spacing"] = spacing;
	data["grid_size"] = grid;
	data["ltm"] = ltm;
	data["local_vis"] = local_vis;
	data["edge_masks"] = edge_masks;
	data["emission"] = emission;
	data["source_instances"] = sources;
	Ref<LocalLRTVolumeData> baked;
	baked.instantiate();
	baked->set_data(data);
	ERR_FAIL_COND_V(!baked->has_data(), ERR_INVALID_DATA);
	r_data = baked;
	return OK;
}
