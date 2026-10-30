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
#include "lrt_bake_task.h"
#include "lrt_geometry.h"
#include "lrt_geometry_binding.h"
#include "lrt_material_baker.h"
#include "lrt_runtime.h"

#include "core/io/marshalls.h"
#include "core/math/dynamic_bvh.h"
#include "core/math/triangle_mesh.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server_default.h"

#include <cfloat>
#include <cmath>

namespace {

using BakeMaterial = LRTMaterialBaker::Surface;

String scene_node_path(const Node *p_node) {
	const Node *root = nullptr;
	if (p_node->is_inside_tree()) {
#ifdef TOOLS_ENABLED
		root = p_node->get_tree()->get_edited_scene_root();
#endif
		if (!root) {
			root = p_node->get_tree()->get_current_scene();
		}
	}
	if (!root || (root != p_node && !root->is_ancestor_of(p_node))) {
		root = p_node;
		while (root->get_owner()) {
			root = root->get_owner();
		}
	}
	return root == p_node ? String(root->get_name()) : String(root->get_name()) + "/" + String(root->get_path_to(p_node));
}

void report_bake_issues(const Array &p_issues) {
	HashMap<String, HashMap<String, int>> meshes;
	for (const Dictionary &issue : p_issues) {
		meshes[String(issue["node_path"])][String(issue["reason"])]++;
	}
	for (const KeyValue<String, HashMap<String, int>> &mesh : meshes) {
		PackedStringArray reasons;
		int count = 0;
		for (const KeyValue<String, int> &reason : mesh.value) {
			reasons.push_back(vformat("%s (%d)", reason.key, reason.value));
			count += reason.value;
		}
		WARN_PRINT(vformat("LRT: %s: %d bake issues: %s See bake_issues for instance, surface, pass and triangle details.", mesh.key, count, String("; ").join(reasons)));
	}
}

struct FaceAttributes {
	Vector2 uv[3];
	Vector3 normal;
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
	Double3(const Vector3 &p_value) :
			x(p_value.x), y(p_value.y), z(p_value.z) {}
	Double3 operator+(const Double3 &p_other) const { return Double3(x + p_other.x, y + p_other.y, z + p_other.z); }
	Double3 operator-(const Double3 &p_other) const { return Double3(x - p_other.x, y - p_other.y, z - p_other.z); }
	Double3 operator*(double p_value) const { return Double3(x * p_value, y * p_value, z * p_value); }
	double dot(const Double3 &p_other) const { return x * p_other.x + y * p_other.y + z * p_other.z; }
	Double3 cross(const Double3 &p_other) const { return Double3(y * p_other.z - z * p_other.y, z * p_other.x - x * p_other.z, x * p_other.y - y * p_other.x); }
};

struct DoubleTriangle {
	Double3 vertex[3];
};

struct DoubleBox {
	Double3 minimum;
	Double3 maximum;
	bool valid = false;

	DoubleBox() = default;
	explicit DoubleBox(const AABB &p_box) {
		minimum = Double3(p_box.position);
		// Native AABB sizes can round down; keep a conservative double upper bound.
		maximum = minimum + Double3(std::nextafter(p_box.size.x, real_t(INFINITY)), std::nextafter(p_box.size.y, real_t(INFINITY)), std::nextafter(p_box.size.z, real_t(INFINITY)));
		valid = true;
	}
	void include(const Double3 &p_point) {
		if (!valid) {
			minimum = maximum = p_point;
			valid = true;
			return;
		}
		minimum = Double3(MIN(minimum.x, p_point.x), MIN(minimum.y, p_point.y), MIN(minimum.z, p_point.z));
		maximum = Double3(MAX(maximum.x, p_point.x), MAX(maximum.y, p_point.y), MAX(maximum.z, p_point.z));
	}
	void merge(const DoubleBox &p_box) {
		if (p_box.valid) {
			include(p_box.minimum);
			include(p_box.maximum);
		}
	}
	double distance_squared(const Double3 &p_point) const {
		if (!valid) {
			return INFINITY;
		}
		const double x = MAX(minimum.x - p_point.x, 0.0) + MAX(p_point.x - maximum.x, 0.0);
		const double y = MAX(minimum.y - p_point.y, 0.0) + MAX(p_point.y - maximum.y, 0.0);
		const double z = MAX(minimum.z - p_point.z, 0.0) + MAX(p_point.z - maximum.z, 0.0);
		return x * x + y * y + z * z;
	}
	bool intersects_segment(const Double3 &p_from, const Double3 &p_to) const {
		if (!valid) {
			return false;
		}
		const double from[3] = { p_from.x, p_from.y, p_from.z };
		const double to[3] = { p_to.x, p_to.y, p_to.z };
		const double low[3] = { minimum.x, minimum.y, minimum.z };
		const double high[3] = { maximum.x, maximum.y, maximum.z };
		double enter = 0, leave = 1;
		for (int axis = 0; axis < 3; axis++) {
			const double delta = to[axis] - from[axis];
			if (delta == 0) {
				if (from[axis] < low[axis] || from[axis] > high[axis]) {
					return false;
				}
				continue;
			}
			double first = (low[axis] - from[axis]) / delta;
			double last = (high[axis] - from[axis]) / delta;
			if (first > last) {
				SWAP(first, last);
			}
			enter = MAX(enter, first);
			leave = MIN(leave, last);
			if (enter > leave) {
				return false;
			}
		}
		return true;
	}
	bool overlaps(const AABB &p_domain) const {
		const Vector3 end = p_domain.get_end();
		return valid && minimum.x <= end.x && minimum.y <= end.y && minimum.z <= end.z && maximum.x >= p_domain.position.x && maximum.y >= p_domain.position.y && maximum.z >= p_domain.position.z;
	}
	AABB clipped(const AABB &p_domain) const {
		if (!valid) {
			return AABB();
		}
		const Vector3 end = p_domain.get_end();
		const Double3 low(MAX(minimum.x, double(p_domain.position.x)), MAX(minimum.y, double(p_domain.position.y)), MAX(minimum.z, double(p_domain.position.z)));
		const Double3 high(MIN(maximum.x, double(end.x)), MIN(maximum.y, double(end.y)), MIN(maximum.z, double(end.z)));
		if (low.x > high.x || low.y > high.y || low.z > high.z) {
			return AABB();
		}
		const Vector3 first(std::nextafter(real_t(low.x), real_t(-INFINITY)), std::nextafter(real_t(low.y), real_t(-INFINITY)), std::nextafter(real_t(low.z), real_t(-INFINITY)));
		const Vector3 last(std::nextafter(real_t(high.x), real_t(INFINITY)), std::nextafter(real_t(high.y), real_t(INFINITY)), std::nextafter(real_t(high.z), real_t(INFINITY)));
		Vector3 size = last - first;
		for (int axis = 0; axis < 3; axis++) {
			size[axis] = std::nextafter(size[axis], real_t(INFINITY));
		}
		return AABB(first, size);
	}
};

struct SurfaceHit {
	int face = -1;
	real_t b = 0;
	real_t c = 0;
	uint32_t source = 0;
};

struct CapturedGeometry {
	ObjectID owner;
	String path;
	int instance_index = 0;
	Ref<Mesh> source_mesh;
	AABB source_bounds;
	Transform3D transform;
	Vector<Ref<LRTMaterialBaker::Request>> surfaces;
	Array issues;
};

struct CachedSurface {
	bool valid = false;
	Dictionary signature;
	Ref<RefCounted> source_snapshot;
	BakeMaterial material;
};

struct Geometry {
	Vector<CachedSurface> cached_surfaces;
	Transform3D bake_transform;
	uint64_t input_read_bytes = 0;
	uint32_t material_reuses = 0;
	uint32_t material_evaluations = 0;
	bool bvh_reused = false;
	bool range_filtered = false;
	String source_path;
	Ref<TriangleMesh> mesh;
	Vector<FaceAttributes> attributes;
	Vector<BakeMaterial> materials;
	Vector<double> node_area;
	Vector<Double3> posed_vertices;
	Vector<DoubleBox> posed_bounds;
	DoubleBox posed_inside;
	Transform3D pose;
	AABB containment_bounds;
	Array issues;
	bool closed = true;

	void report(GeometryInstance3D *p_instance, int p_instance_index, int p_surface, int p_triangle, const String &p_reason, const Dictionary &p_issue = Dictionary()) {
		Dictionary issue = p_issue.duplicate();
		issue["node_path"] = p_instance ? scene_node_path(p_instance) : source_path;
		issue["instance_index"] = p_instance_index;
		issue["surface"] = p_surface;
		issue["triangle"] = p_triangle;
		issue["reason"] = p_reason;
		issues.push_back(issue);
	}

	static bool outside_domain(const CapturedGeometry &p_input, const AABB &p_domain, const SafeFlag *p_cancel) {
		DoubleBox bounds;
		double area_bound = 0;
		for (const Ref<LRTMaterialBaker::Request> &surface : p_input.surfaces) {
			if (surface.is_null()) {
				continue;
			}
			Dictionary signature;
			if (LRTMaterialBaker::get_signature(surface, signature, p_cancel) != OK) {
				return false;
			}
			const Dictionary source = signature["source"];
			const uint64_t format = int64_t(source["format"]);
			const Array buffers = source["buffers"];
			const Dictionary vertices = buffers[0];
			// GPU/CPU writes and dynamic vertices invalidate the original AABB.
			if ((format & RSE::ARRAY_FLAG_USE_DYNAMIC_UPDATE) || int64_t(vertices["revision"]) != 0) {
				return false;
			}
			const Array passes = signature["passes"];
			for (const Dictionary &pass : passes) {
				if (pass.get("vertex_deformation", false)) {
					return false;
				}
			}
			const AABB box = source["bounds"];
			bounds.merge(DoubleBox(box));
			const Double3 diagonal(box.size);
			const int64_t indices = source["indices"];
			const int64_t count = indices ? indices : int64_t(source["vertices"]);
			// Every triangle lies within this AABB, so area <= diagonal^2 / 2.
			area_bound += diagonal.dot(diagonal) * 0.5 * (count / 3);
		}
		if (!bounds.valid || !Math::is_finite(area_bound)) {
			return false;
		}
		// This also covers generalized winding influence of open/nonmanifold meshes.
		const double radius = Math::sqrt(area_bound / (2.0 * Math::PI)) * (1.0 + 8.0 * CMP_EPSILON);
		const Double3 padding(radius, radius, radius);
		bounds.minimum = bounds.minimum - padding;
		bounds.maximum = bounds.maximum + padding;
		const Transform3D &transform = p_input.transform;
		const Double3 columns[3] = { transform.basis.get_column(0), transform.basis.get_column(1), transform.basis.get_column(2) };
		DoubleBox transformed;
		for (int corner = 0; corner < 8; corner++) {
			const Double3 point(corner & 1 ? bounds.maximum.x : bounds.minimum.x,
					corner & 2 ? bounds.maximum.y : bounds.minimum.y, corner & 4 ? bounds.maximum.z : bounds.minimum.z);
			transformed.include(Double3(transform.origin) + columns[0] * point.x + columns[1] * point.y + columns[2] * point.z);
		}
		return !transformed.overlaps(p_domain);
	}

	Error prepare(GeometryInstance3D *p_instance, const Transform3D &p_transform = Transform3D(), float p_texel_size = 0, int p_instance_index = 0, const CapturedGeometry *p_captured = nullptr, const SafeFlag *p_cancel = nullptr, const Geometry *p_previous = nullptr, const AABB *p_domain = nullptr) {
		bake_transform = p_transform;
		if (p_captured) {
			source_path = p_captured->path;
			issues = p_captured->issues;
			if (p_captured->surfaces.is_empty() && !issues.is_empty()) {
				return OK;
			}
		}
		ERR_FAIL_COND_V(!p_transform.is_finite(), ERR_INVALID_DATA);
		const Ref<Mesh> source = p_captured ? p_captured->source_mesh : LRTGeometryBinding::get_mesh(p_instance);
		ERR_FAIL_COND_V_MSG(source.is_null(), ERR_INVALID_DATA, "LRT cannot bake an instance without a mesh.");
		if (MeshInstance3D *instance = p_captured ? nullptr : Object::cast_to<MeshInstance3D>(p_instance)) {
			if (instance->get_skin().is_valid()) {
				report(p_instance, p_instance_index, -1, -1, "Skeletal deformation is not supported by LRT baking.");
				return ERR_INVALID_DATA;
			}
			for (int shape = 0; shape < source->get_blend_shape_count(); shape++) {
				if (instance->get_blend_shape_value(shape) != 0) {
					report(p_instance, p_instance_index, -1, -1, vformat("Active blend shape %d is not supported by LRT baking.", shape));
					return ERR_INVALID_DATA;
				}
			}
		}

		if (p_captured && p_domain && outside_domain(*p_captured, *p_domain, p_cancel)) {
			range_filtered = true;
			bvh_reused = p_previous && p_previous->range_filtered;
			return OK;
		}
		if (!p_captured && p_texel_size == 0) {
			p_texel_size = source->get_aabb().get_longest_axis_size() / (32 - 5) * 0.25f;
		}
		Vector<Vector3> faces;
		bool has_triangles = false;
		const int surface_count = p_captured ? p_captured->surfaces.size() : source->get_surface_count();
		ERR_FAIL_COND_V(cached_surfaces.resize(surface_count) != OK, ERR_OUT_OF_MEMORY);
		bool all_reused = p_previous && p_previous->issues.is_empty() && p_previous->cached_surfaces.size() == surface_count && p_previous->bake_transform == p_transform;
		for (int surface = 0; surface < surface_count; surface++) {
			if (p_cancel && p_cancel->is_set()) {
				return ERR_SKIP;
			}
			if (p_captured ? p_captured->surfaces[surface].is_null() : source->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			has_triangles = true;
			BakeMaterial material;
			Dictionary signature;
			Error error = OK;
			if (p_captured) {
				error = LRTMaterialBaker::get_signature(p_captured->surfaces[surface], signature, p_cancel);
				const bool reuse = error == OK && p_previous && surface < p_previous->cached_surfaces.size() &&
						p_previous->cached_surfaces[surface].valid && p_previous->cached_surfaces[surface].material.issues.is_empty() && p_previous->cached_surfaces[surface].signature == signature;
				if (reuse) {
					material = p_previous->cached_surfaces[surface].material;
					cached_surfaces.write[surface].source_snapshot = p_previous->cached_surfaces[surface].source_snapshot;
					material_reuses++;
				} else {
					all_reused = false;
					if (error == OK) {
						error = LRTMaterialBaker::bake_on_worker(p_captured->surfaces[surface], material, p_cancel,
								p_previous && surface < p_previous->cached_surfaces.size() ? p_previous->cached_surfaces[surface].source_snapshot : Ref<RefCounted>());
						cached_surfaces.write[surface].source_snapshot = LRTMaterialBaker::get_source_snapshot(p_captured->surfaces[surface]);
						input_read_bytes += material.source_read_bytes;
						material_evaluations++;
						if (error == OK) {
							error = LRTMaterialBaker::get_signature(p_captured->surfaces[surface], signature, p_cancel);
						}
					}
				}
				cached_surfaces.write[surface].signature = signature;
				cached_surfaces.write[surface].material = material;
			} else {
				error = LRTMaterialBaker::inspect_surface(p_instance, surface, p_texel_size, material, p_instance_index);
			}
			if (error == ERR_SKIP) {
				return error;
			}
			for (const Variant &entry : material.issues) {
				const Dictionary issue = entry;
				report(p_instance, p_instance_index, surface, -1, issue["reason"], issue);
			}
			if (error != OK) {
				if (error == ERR_OUT_OF_MEMORY || error == ERR_UNAVAILABLE) {
					return error;
				}
				report(p_instance, p_instance_index, surface, -1, vformat("Material evaluation failed: %s.", error_names[error]));
				continue;
			}
			cached_surfaces.write[surface].valid = true;
			cached_surfaces.write[surface].material = material;
		}
		if (all_reused) {
			const uint32_t reused = material_reuses;
			*this = *p_previous;
			source_path = p_captured->path;
			material_reuses = reused;
			material_evaluations = 0;
			input_read_bytes = 0;
			bvh_reused = true;
			return OK;
		}
		if (!has_triangles) {
			report(p_instance, p_instance_index, -1, -1, "LRT baking requires triangle surfaces.");
			return ERR_INVALID_DATA;
		}
		for (int surface = 0; surface < cached_surfaces.size(); surface++) {
			if (!cached_surfaces[surface].valid) {
				continue;
			}
			const BakeMaterial &material = cached_surfaces[surface].material;
			if (material.transparent) {
				continue;
			}
			const int material_index = materials.size();
			materials.push_back(material);
			for (int face = 0; face < material.positions.size(); face += 3) {
				Vector3 positions[3];
				bool finite = true;
				for (int corner = 0; corner < 3; corner++) {
					positions[corner] = p_transform.xform(material.positions[face + corner]);
					finite &= material.positions[face + corner].is_finite() && positions[corner].is_finite();
				}
				if (!finite) {
					report(p_instance, p_instance_index, surface, face / 3, "Triangle has non-finite evaluated positions.");
					continue;
				}
				const Double3 a(positions[0]), b(positions[1]), c(positions[2]);
				const Double3 normal = (b - a).cross(c - a);
				// Remove only exact degenerates, preserving thin imported triangles.
				if (normal.dot(normal) == 0) {
					report(p_instance, p_instance_index, surface, face / 3, "Triangle is exactly degenerate after vertex evaluation.");
					continue;
				}
				FaceAttributes values;
				// Godot uses clockwise front faces; normalize before narrowing to real_t.
				const double normal_length = Math::sqrt(normal.dot(normal));
				values.normal = Vector3(-normal.x / normal_length, -normal.y / normal_length, -normal.z / normal_length);
				values.material = material_index;
				for (int corner = 0; corner < 3; corner++) {
					faces.push_back(positions[corner]);
					values.uv[corner] = material.atlas_uv[face + corner];
				}
				attributes.push_back(values);
			}
		}
		if (faces.is_empty()) {
			return OK; // Transparent or fully degenerate geometry has no contribution.
		}
		if (p_previous && p_previous->mesh.is_valid() && p_previous->attributes.size() == attributes.size()) {
			bool equal = true;
			for (int face = 0; face < attributes.size() && equal; face++) {
				const Face3 previous = p_previous->face(face);
				for (int corner = 0; corner < 3; corner++) {
					equal &= previous.vertex[corner] == faces[face * 3 + corner];
				}
			}
			if (equal) {
				mesh = p_previous->mesh;
				closed = p_previous->closed;
				node_area = p_previous->node_area;
				containment_bounds = p_previous->containment_bounds;
				bvh_reused = true;
				return OK;
			}
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
		containment_bounds = mesh->get_aabb();
		if (!closed) {
			ERR_FAIL_COND_V(node_area.resize(mesh->get_bvh().size()) != OK, ERR_OUT_OF_MEMORY);
			double *areas = node_area.ptrw();
			// TriangleMesh stores children before their parent.
			for (int index = 0; index < node_area.size(); index++) {
				const TriangleMesh::BVH &node = mesh->get_bvh()[index];
				if (node.face_index < 0) {
					areas[index] = areas[node.left] + areas[node.right];
					continue;
				}
				const Face3 triangle = face(node.face_index);
				const Double3 normal = (Double3(triangle.vertex[1]) - Double3(triangle.vertex[0])).cross(Double3(triangle.vertex[2]) - Double3(triangle.vertex[0]));
				areas[index] = 0.5 * Math::sqrt(normal.dot(normal));
			}
			// Open overlapping sheets can exceed winding 0.5 outside the mesh AABB.
			// Area / distance^2 bounds that influence; pad the broad phase outwards.
			const double radius = Math::sqrt(areas[node_area.size() - 1] / (2.0 * Math::PI));
			containment_bounds = containment_bounds.grow(radius * (1.0 + 4.0 * CMP_EPSILON));
		}
		return OK;
	}

	Face3 face(int p_index) const {
		const TriangleMesh::Triangle &triangle = mesh->get_triangles()[p_index];
		const Vector<Vector3> &vertices = mesh->get_vertices();
		return Face3(vertices[triangle.indices[0]], vertices[triangle.indices[1]], vertices[triangle.indices[2]]);
	}

	bool has_geometry() const {
		return mesh.is_valid() && (posed_bounds.is_empty() || posed_bounds[posed_bounds.size() - 1].valid);
	}

	DoubleTriangle query_face(int p_index) const {
		const TriangleMesh::Triangle &triangle = mesh->get_triangles()[p_index];
		DoubleTriangle result;
		for (int corner = 0; corner < 3; corner++) {
			const int vertex = triangle.indices[corner];
			result.vertex[corner] = posed_vertices.is_empty() ? Double3(mesh->get_vertices()[vertex]) : posed_vertices[vertex];
		}
		return result;
	}

	DoubleBox node_bounds(int p_index) const {
		return posed_bounds.is_empty() ? DoubleBox(mesh->get_bvh()[p_index].aabb) : posed_bounds[p_index];
	}

	Vector3 normal(const SurfaceHit &p_hit) const {
		const DoubleTriangle triangle = query_face(p_hit.face);
		const Double3 normal = (triangle.vertex[1] - triangle.vertex[0]).cross(triangle.vertex[2] - triangle.vertex[0]);
		const double length = Math::sqrt(normal.dot(normal));
		return Vector3(-normal.x / length, -normal.y / length, -normal.z / length);
	}

	Array pose_issues;
	Error set_pose(const Transform3D &p_transform, int p_instance, const Geometry *p_previous) {
		pose = p_transform;
		if (mesh.is_null()) {
			return OK;
		}
		if (p_previous && mesh == p_previous->mesh && pose == p_previous->pose) {
			posed_vertices = p_previous->posed_vertices;
			posed_bounds = p_previous->posed_bounds;
			posed_inside = p_previous->posed_inside;
			node_area = p_previous->node_area;
			closed = p_previous->closed;
			pose_issues = p_previous->pose_issues;
			issues.append_array(pose_issues);
			return OK;
		}
		ERR_FAIL_COND_V(posed_vertices.resize(mesh->get_vertices().size()) != OK ||
						posed_bounds.resize(mesh->get_bvh().size()) != OK ||
						node_area.resize(mesh->get_bvh().size()) != OK,
				ERR_OUT_OF_MEMORY);
		const Double3 columns[3] = { p_transform.basis.get_column(0), p_transform.basis.get_column(1), p_transform.basis.get_column(2) };
		const Double3 origin(p_transform.origin);
		Double3 *vertices = posed_vertices.ptrw();
		for (int index = 0; index < posed_vertices.size(); index++) {
			const Vector3 &point = mesh->get_vertices()[index];
			vertices[index] = origin + columns[0] * point.x + columns[1] * point.y + columns[2] * point.z;
		}
		DoubleBox *bounds = posed_bounds.ptrw();
		double *areas = node_area.ptrw();
		for (int index = 0; index < posed_bounds.size(); index++) {
			const TriangleMesh::BVH &node = mesh->get_bvh()[index];
			if (node.face_index < 0) {
				bounds[index].merge(bounds[node.left]);
				bounds[index].merge(bounds[node.right]);
				areas[index] = areas[node.left] + areas[node.right];
				continue;
			}
			const DoubleTriangle triangle = query_face(node.face_index);
			const Double3 cross = (triangle.vertex[1] - triangle.vertex[0]).cross(triangle.vertex[2] - triangle.vertex[0]);
			const double area_squared = cross.dot(cross);
			if (!Math::is_finite(area_squared) || area_squared == 0) {
				closed = false;
				areas[index] = 0;
				report(nullptr, p_instance, -1, node.face_index, "Triangle is non-finite or exactly degenerate after instance transformation.");
				pose_issues.push_back(issues[issues.size() - 1]);
				continue;
			}
			for (const Double3 &point : triangle.vertex) {
				bounds[index].include(point);
			}
			areas[index] = 0.5 * Math::sqrt(area_squared);
		}
		posed_inside = posed_bounds[posed_bounds.size() - 1];
		if (!closed && posed_inside.valid) {
			const double radius = Math::sqrt(areas[node_area.size() - 1] / (2.0 * Math::PI)) * (1.0 + 4.0 * CMP_EPSILON);
			const Double3 padding(radius, radius, radius);
			posed_inside.minimum = posed_inside.minimum - padding;
			posed_inside.maximum = posed_inside.maximum + padding;
		}
		return OK;
	}

	static Double3 closest_point(const DoubleTriangle &p_triangle, const Double3 &p_point) {
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

	bool intersect_node(int p_node, const Double3 &p_from, const Double3 &p_to, double &r_t, int &r_face, Double3 &r_point, bool p_any_hit) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (!node_bounds(p_node).intersects_segment(p_from, p_to)) {
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
		const DoubleTriangle triangle = query_face(node.face_index);
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
		if (t <= 0 || t > r_t || (t == r_t && r_face >= 0 && node.face_index >= r_face)) {
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

	void nearest(int p_node, const Double3 &p_point, double &r_distance_squared, int &r_face, Double3 &r_closest) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (node_bounds(p_node).distance_squared(p_point) > r_distance_squared) {
			return;
		}
		if (node.face_index >= 0) {
			const Double3 point = closest_point(query_face(node.face_index), Double3(p_point));
			const Double3 delta = point - Double3(p_point);
			const double distance_squared = delta.dot(delta);
			if (distance_squared < r_distance_squared) {
				r_distance_squared = distance_squared;
				r_face = node.face_index;
				r_closest = point;
			}
			return;
		}
		const bool left_first = node_bounds(node.left).distance_squared(p_point) <= node_bounds(node.right).distance_squared(p_point);
		nearest(left_first ? node.left : node.right, p_point, r_distance_squared, r_face, r_closest);
		nearest(left_first ? node.right : node.left, p_point, r_distance_squared, r_face, r_closest);
	}

	static double edge(const Double3 &p_a, const Double3 &p_b, const Double3 &p_point) {
		return (double(p_b.y) - p_a.y) * (double(p_point.z) - p_a.z) - (double(p_b.z) - p_a.z) * (double(p_point.y) - p_a.y);
	}

	static bool covered(double p_edge, const Double3 &p_a, const Double3 &p_b) {
		// A half-open projected triangle counts shared edges exactly once.
		return p_edge > 0 || (p_edge == 0 && (p_b.z > p_a.z || (p_b.z == p_a.z && p_b.y < p_a.y)));
	}

	int winding(int p_node, const Double3 &p_point) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		const DoubleBox box = node_bounds(p_node);
		if (!box.valid || box.maximum.x < p_point.x || p_point.y < box.minimum.y || p_point.y > box.maximum.y || p_point.z < box.minimum.z || p_point.z > box.maximum.z) {
			return 0;
		}
		if (node.face_index < 0) {
			return winding(node.left, p_point) + winding(node.right, p_point);
		}
		const DoubleTriangle triangle = query_face(node.face_index);
		Double3 a = triangle.vertex[0], b = triangle.vertex[1], c = triangle.vertex[2];
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

	bool on_surface(int p_node, const Double3 &p_point) const {
		const TriangleMesh::BVH &node = mesh->get_bvh()[p_node];
		if (node_bounds(p_node).distance_squared(p_point) != 0) {
			return false;
		}
		if (node.face_index < 0) {
			return on_surface(node.left, p_point) || on_surface(node.right, p_point);
		}
		const Double3 delta = closest_point(query_face(node.face_index), Double3(p_point)) - Double3(p_point);
		return delta.dot(delta) == 0;
	}

	AABB inside_bounds() const {
		return containment_bounds;
	}

	bool contains(const Double3 &p_point) const {
		if (!has_geometry() || (posed_bounds.is_empty() ? DoubleBox(inside_bounds()) : posed_inside).distance_squared(p_point) != 0) {
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
			struct PendingNode {
				int index;
				double bound;
			};
			auto angle_bound = [&](int p_node) {
				const double distance_squared = node_bounds(p_node).distance_squared(p_point);
				// |integral(n dot r / |r|^3)| <= area / minimum_distance^2.
				return distance_squared > 0 ? node_area[p_node] / distance_squared : double(INFINITY);
			};
			LocalVector<PendingNode> pending;
			pending.push_back({ root, angle_bound(root) });
			while (!pending.is_empty()) {
				const PendingNode current = pending[pending.size() - 1];
				pending.resize(pending.size() - 1);
				const TriangleMesh::BVH &node = mesh->get_bvh()[current.index];
				if (node.face_index < 0) {
					PendingNode left{ node.left, angle_bound(node.left) };
					PendingNode right{ node.right, angle_bound(node.right) };
					if (left.bound > right.bound) {
						SWAP(left, right);
					}
					pending.push_back(left);
					pending.push_back(right);
					continue;
				}
				if (!node_bounds(current.index).valid) {
					continue;
				}
				const DoubleTriangle triangle = query_face(node.face_index);
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
				double remaining = 0;
				for (const PendingNode &entry : pending) {
					remaining += entry.bound;
				}
				// Only classify early when *every* remaining triangle contribution
				// fits on the same side of the threshold. This is not a far-field
				// approximation of the winding number; ambiguous points visit leaves.
				const double margin = 64.0 * DBL_EPSILON * mesh->get_triangles().size() * (2.0 * Math::PI + Math::abs(solid_angle));
				if (Math::abs(solid_angle) - remaining > 2.0 * Math::PI + margin) {
					return true;
				}
				if (Math::abs(solid_angle) + remaining < 2.0 * Math::PI - margin) {
					return on_surface(root, p_point);
				}
			}
			if (Math::abs(solid_angle) > 2.0 * Math::PI) {
				return true;
			}
		}
		// Only exact surface membership is needed, not the globally nearest face.
		return on_surface(root, p_point);
	}

	SurfaceHit surface_hit(int p_face, const Double3 &p_closest) const {
		const DoubleTriangle triangle = query_face(p_face);
		const Double3 u = Double3(triangle.vertex[1]) - Double3(triangle.vertex[0]);
		const Double3 v = Double3(triangle.vertex[2]) - Double3(triangle.vertex[0]);
		const Double3 normal = u.cross(v);
		const Double3 point = p_closest - Double3(triangle.vertex[0]);
		const double denominator = normal.dot(normal);
		const real_t b = point.cross(v).dot(normal) / denominator;
		const real_t c = u.cross(point).dot(normal) / denominator;
		return { p_face, b, c };
	}

	void sample_material(const SurfaceHit &p_hit, Color &r_albedo, Color &r_emission) const {
		const FaceAttributes &values = attributes[p_hit.face];
		materials[values.material].sample(values.uv[0] * (1 - p_hit.b - p_hit.c) + values.uv[1] * p_hit.b + values.uv[2] * p_hit.c, r_albedo, r_emission);
	}

	void sample_material(int p_face, const Double3 &p_closest, Color &r_albedo, Color &r_emission) const {
		sample_material(surface_hit(p_face, p_closest), r_albedo, r_emission);
	}
};

struct SharedGeometryRecord;
Mutex shared_geometry_mutex;
HashMap<uint32_t, Vector<SharedGeometryRecord *>> shared_geometry_records;

struct SharedGeometryRecord : RefCounted {
	GDSOFTCLASS(SharedGeometryRecord, RefCounted);

public:
	Dictionary key;
	Geometry geometry;
	uint32_t hash = 0;
	bool registered = false;
	struct SDFSnapshot {
		Dictionary data;
		Vector<SurfaceHit> hits;
	};
	Mutex sdf_mutex;
	HashMap<int, SDFSnapshot> sdfs;

	bool get_sdf(int p_resolution, Ref<LRTInstanceGeometryData> &r_data, Vector<SurfaceHit> &r_hits) {
		Dictionary data;
		{
			MutexLock lock(sdf_mutex);
			const SDFSnapshot *snapshot = sdfs.getptr(p_resolution);
			if (!snapshot) {
				return false;
			}
			data = snapshot->data.duplicate(true);
			r_hits = snapshot->hits;
		}
		r_data.instantiate();
		r_data->set_data(data);
		return true;
	}

	void store_sdf(int p_resolution, const Ref<LRTInstanceGeometryData> &p_data, const Vector<SurfaceHit> &p_hits) {
		SDFSnapshot snapshot;
		// Published Resources remain editable; never retain their mutable Dictionary.
		snapshot.data = p_data->get_data().duplicate(true);
		snapshot.hits = p_hits;
		MutexLock lock(sdf_mutex);
		sdfs.insert(p_resolution, snapshot);
	}

	~SharedGeometryRecord() {
		if (!registered) {
			return;
		}
		MutexLock lock(shared_geometry_mutex);
		Vector<SharedGeometryRecord *> *bucket = shared_geometry_records.getptr(hash);
		bucket->erase(this);
		if (bucket->is_empty()) {
			shared_geometry_records.erase(hash);
		}
	}

	static Ref<SharedGeometryRecord> find(const Dictionary &p_key) {
		Vector<Ref<SharedGeometryRecord>> candidates;
		{
			MutexLock lock(shared_geometry_mutex);
			const Vector<SharedGeometryRecord *> *bucket = shared_geometry_records.getptr(p_key.hash());
			if (bucket) {
				for (SharedGeometryRecord *record : *bucket) {
					// A zero-reference record is still alive while its destructor
					// waits for this lock. Native Ref refuses to resurrect it.
					const Ref<SharedGeometryRecord> candidate(record);
					if (candidate.is_valid()) {
						candidates.push_back(candidate);
					}
				}
			}
		}
		for (const Ref<SharedGeometryRecord> &candidate : candidates) {
			if (candidate->key == p_key) {
				return candidate;
			}
		}
		return Ref<SharedGeometryRecord>();
	}

	Error publish() {
		hash = key.hash();
		MutexLock lock(shared_geometry_mutex);
		if (shared_geometry_records[hash].push_back(this)) {
			return ERR_OUT_OF_MEMORY;
		}
		registered = true;
		return OK;
	}
};

struct CachedGeometryEntry {
	ObjectID owner;
	uint32_t source_id = 0;
	bool material_changed = true;
	bool static_contribution = true;
	int subinstance = 0;
	Transform3D transform;
	Geometry geometry;
	Ref<SharedGeometryRecord> shared_geometry;
	Geometry posed;
	Ref<LRTInstanceGeometryData> sdf;
	int sdf_resolution = 0;
	Vector<SurfaceHit> sdf_hits;
};

// TriangleMesh topology stays in object space. Instance poses only refit double
// query bounds/vertices; no merged scene TriangleMesh or topology rebuild.
struct SceneGeometry {
	mutable DynamicBVH surfaces;
	DynamicBVH interiors;
	HashMap<uint32_t, const CachedGeometryEntry *> sources;
	Array issues;
	bool emission = false;
	bool active = false;

	SceneGeometry(Vector<CachedGeometryEntry> &p_entries, const AABB &p_domain, bool p_build) {
		for (CachedGeometryEntry &entry : p_entries) {
			sources.insert(entry.source_id, &entry);
			if (!entry.static_contribution || !entry.posed.has_geometry() || !entry.posed.posed_inside.overlaps(p_domain)) {
				continue;
			}
			active = true;
			for (const BakeMaterial &material : entry.posed.materials) {
				emission |= material.has_emission;
			}
			if (p_build) {
				interiors.insert(entry.posed.posed_inside.clipped(p_domain), &entry);
				const DoubleBox bounds = entry.posed.node_bounds(entry.posed.mesh->get_bvh().size() - 1);
				if (bounds.overlaps(p_domain)) {
					surfaces.insert(bounds.clipped(p_domain), &entry);
				}
			}
		}
		if (p_build) {
			surfaces.optimize_top_down();
			interiors.optimize_top_down();
		}
	}
	bool has_geometry() const { return active; }

	bool contains(const Vector3 &p_point) {
		bool inside = false;
		auto query = [&](void *p_data) {
			inside = static_cast<const CachedGeometryEntry *>(p_data)->posed.contains(p_point);
			return inside;
		};
		interiors.aabb_query(AABB(p_point, Vector3()), query);
		return inside;
	}

	bool intersect_segment(const Vector3 &p_from, const Vector3 &p_to, SurfaceHit *r_hit = nullptr) const {
		double nearest = 1;
		const CachedGeometryEntry *selected = nullptr;
		int selected_face = -1;
		Double3 selected_point;
		auto query = [&](void *p_data) {
			const CachedGeometryEntry &entry = *static_cast<const CachedGeometryEntry *>(p_data);
			double t = nearest;
			int face = -1;
			Double3 point;
			if (!entry.posed.intersect_node(entry.posed.mesh->get_bvh().size() - 1, p_from, p_to, t, face, point, !r_hit)) {
				return false;
			}
			if (t == nearest && selected &&
					(uint64_t(entry.owner) > uint64_t(selected->owner) || (entry.owner == selected->owner && entry.subinstance > selected->subinstance))) {
				return false;
			}
			nearest = t;
			selected = &entry;
			selected_face = face;
			selected_point = point;
			return !r_hit;
		};
		AABB bounds(p_from, Vector3());
		bounds.expand_to(p_to);
		surfaces.aabb_query(bounds, query);
		if (!selected) {
			return false;
		}
		if (r_hit) {
			*r_hit = selected->posed.surface_hit(selected_face, selected_point);
			r_hit->source = selected->source_id;
		}
		return true;
	}

	void sample_material(const SurfaceHit &p_hit, Color &r_albedo, Color &r_emission) const {
		sources[p_hit.source]->posed.sample_material(p_hit, r_albedo, r_emission);
	}
	Vector3 normal(const SurfaceHit &p_hit) const {
		return sources[p_hit.source]->posed.normal(p_hit);
	}
};

// Broad phase over instance bounds; the referenced vector stays fixed during queries.
struct SolidBVH {
	DynamicBVH tree;

	explicit SolidBVH(Vector<Geometry> &p_solids, bool p_build = true) {
		if (!p_build) {
			return;
		}
		Geometry *solids = p_solids.ptrw();
		for (int index = 0; index < p_solids.size(); index++) {
			tree.insert(solids[index].inside_bounds(), &solids[index]);
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

Error collect_static_geometry(LRTVolume3D *p_volume, Node *p_from_node, Geometry &r_combined, Vector<Geometry> &r_solids, Array &r_sources) {
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
		if (!LRTGeometryBinding::supports(instance) || !p_volume->collects_static(instance) || !instance->is_visible_in_tree() || instance->get_world_3d() != p_volume->get_world_3d()) {
			continue;
		}
		const Ref<Mesh> mesh = LRTGeometryBinding::get_mesh(instance);
		if (mesh.is_null()) {
			continue;
		}
		if (MultiMeshInstance3D *multi = Object::cast_to<MultiMeshInstance3D>(instance)) {
			if (multi->get_multimesh()->get_transform_format() != MultiMesh::TRANSFORM_3D) {
				r_combined.report(instance, -1, -1, -1, "LRT requires a 3D MultiMesh.");
				continue;
			}
		}
		const Transform3D node_transform = volume_frame.relative_transform(instance);
		for (int subinstance = 0; subinstance < LRTGeometryBinding::get_instance_count(instance); subinstance++) {
			const Transform3D transform = node_transform * LRTGeometryBinding::get_instance_transform(instance, subinstance);
			if (!transform.is_finite() || !transform.basis.is_conformal() || transform.basis.determinant() <= 0) {
				r_combined.report(instance, subinstance, -1, -1, "LRT instances require a positive uniform scale without shear.");
				continue;
			}
			Geometry geometry;
			const Error error = geometry.prepare(instance, transform, p_volume->get_probe_spacing() * 0.25f / transform.basis.get_scale().x, subinstance);
			r_combined.issues.append_array(geometry.issues);
			if (error != OK) {
				if (error == ERR_OUT_OF_MEMORY || error == ERR_UNAVAILABLE) {
					return error;
				}
				if (geometry.issues.is_empty()) {
					r_combined.report(instance, subinstance, -1, -1, vformat("Geometry evaluation failed: %s.", error_names[error]));
				}
				continue;
			}
			if (geometry.mesh.is_null() || !query_bounds.intersects_inclusive(geometry.inside_bounds())) {
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

template <typename Query, typename Inside>
uint8_t bake_edge_mask(const Query &p_geometry, Inside &p_solids, const AABB &p_bounds, float p_spacing, const Vector3i &p_probe, int p_direction) {
	if (!p_geometry.has_geometry()) {
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

struct DirtyRegion {
	DoubleBox bounds;
	uint8_t flags = 0; // 1: material, 2: geometry and visibility.
};

struct EdgeBake {
	const SceneGeometry &geometry;
	SceneGeometry &solids;
	const AABB &bounds;
	float spacing;
	Vector3i grid;
	const Vector3i *neighbors;
	uint8_t *output;
	SafeFlag cancelled{ false };
	const SafeFlag *external_cancel = nullptr;
	SafeNumeric<uint32_t> *progress = nullptr;
	const uint8_t *dirty = nullptr;

	void bake_slice(uint32_t p_z, void *) {
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				if (cancelled.is_set() || (external_cancel && external_cancel->is_set())) {
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
					if (dirty && dirty[index] != 2 && (!in_grid || dirty[adjacent_index] != 2)) {
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
		if (progress) {
			progress->increment();
		}
	}
};

struct SDFBake {
	const Geometry &geometry;
	AABB bounds;
	Vector3i grid;
	real_t cell_size;
	float distance_scale;
	int root;
	uint8_t *distance_write;
	uint8_t *color_write;
	float *emission_write;
	const SafeFlag *cancel;
	SafeNumeric<uint32_t> *progress;
	const SurfaceHit *previous_hits = nullptr;
	SurfaceHit *output_hits = nullptr;
	SafeFlag invalid{ false };

	void bake_slice(uint32_t z, void *) {
		if (cancel && cancel->is_set()) {
			return;
		}
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				const int64_t index = (int64_t(z) * grid.y + y) * grid.x + x;
				const Vector3 point = bounds.position + Vector3(x, y, z) * cell_size;
				SurfaceHit hit;
				if (previous_hits) {
					hit = previous_hits[index];
				} else {
					double distance_squared = INFINITY;
					int face = -1;
					Double3 closest;
					geometry.nearest(root, point, distance_squared, face, closest);
					if (face < 0) {
						invalid.set();
						return;
					}
					hit = geometry.surface_hit(face, closest);
					if (output_hits) {
						output_hits[index] = hit;
					}
					const bool inside = geometry.contains(point);
					const real_t signed_distance = Math::sqrt(distance_squared) * (inside ? -1 : 1);
					const int quantized = CLAMP(int(Math::round(signed_distance / distance_scale * 32767.0)), -32767, 32767);
					encode_uint16(uint16_t(int16_t(quantized)), distance_write + index * 2);
				}

				Color albedo;
				Color light;
				geometry.sample_material(hit, albedo, light);
				color_write[index * 4 + 0] = uint8_t(CLAMP(Math::round(albedo.r * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 1] = uint8_t(CLAMP(Math::round(albedo.g * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 2] = uint8_t(CLAMP(Math::round(albedo.b * 255.0f), 0.0f, 255.0f));
				color_write[index * 4 + 3] = 255;
				if (emission_write) {
					emission_write[index * 3 + 0] = light.r;
					emission_write[index * 3 + 1] = light.g;
					emission_write[index * 3 + 2] = light.b;
				}
			}
		}
		if (progress) {
			progress->increment();
		}
	}
};

Error bake_sdf(Geometry &geometry, const Ref<Mesh> &p_mesh, const AABB &p_source_bounds, int p_resolution, Ref<LRTInstanceGeometryData> &r_data, const SafeFlag *p_cancel = nullptr, SafeNumeric<uint32_t> *p_progress = nullptr, const Ref<LRTInstanceGeometryData> &p_previous = Ref<LRTInstanceGeometryData>(), const Vector<SurfaceHit> *p_hits = nullptr, Vector<SurfaceHit> *r_hits = nullptr) {
	if (geometry.mesh.is_null()) {
		// Persist an explicit receiver-only bake, so Dynamic bindings can be
		// excluded without querying GPU shader classification every runtime frame.
		Dictionary data;
		const AABB bounds = p_source_bounds.grow(0.001f);
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
		data["bake_complete"] = geometry.issues.is_empty();
		data["bake_issues"] = geometry.issues;
		data["source_mesh"] = p_mesh;
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

	const bool reuse_distance = p_previous.is_valid() && p_hits && p_hits->size() == count && AABB(p_previous->get_data()["bounds"]) == bounds && Vector3i(p_previous->get_data()["grid_size"]) == grid;
	Vector<uint8_t> distance = reuse_distance ? Vector<uint8_t>(p_previous->get_data()["distance"]) : Vector<uint8_t>();
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
	uint8_t *distance_write = reuse_distance ? nullptr : distance.ptrw();
	uint8_t *color_write = color.ptrw();
	float *emission_write = emission.ptrw();
	const int root = geometry.mesh->get_bvh().size() - 1;
	SDFBake job{ geometry, bounds, grid, cell_size, distance_scale, root, distance_write, color_write, emission_write, p_cancel, p_progress };
	if (reuse_distance) {
		job.previous_hits = p_hits->ptr();
		if (r_hits) {
			*r_hits = *p_hits;
		}
	} else if (r_hits) {
		ERR_FAIL_COND_V(r_hits->resize(count) != OK, ERR_OUT_OF_MEMORY);
		job.output_hits = r_hits->ptrw();
	}
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const WorkerThreadPool::GroupID task = pool->add_template_group_task(&job, &SDFBake::bake_slice, nullptr, grid.z);
	pool->wait_for_group_task_completion(task);
	if (p_cancel && p_cancel->is_set()) {
		return ERR_SKIP;
	}
	ERR_FAIL_COND_V(job.invalid.is_set(), ERR_INVALID_DATA);

	Dictionary data;
	data["bounds"] = bounds;
	data["grid_size"] = grid;
	data["distance_scale"] = distance_scale;
	data["distance"] = distance;
	data["color"] = color;
	data["emission"] = emission;
	data["source_mesh"] = p_mesh;
	data["source_resolution"] = p_resolution;
	data["bake_complete"] = geometry.issues.is_empty();
	data["bake_issues"] = geometry.issues;
	Ref<LRTInstanceGeometryData> baked;
	baked.instantiate();
	baked->set_data(data);
	ERR_FAIL_COND_V(!baked->has_data(), ERR_INVALID_DATA);
	r_data = baked;
	return OK;
}

struct ProbeBake {
	const SceneGeometry &combined;
	SceneGeometry &solid_bvh;
	AABB bounds;
	float spacing;
	Vector3i grid;
	const Vector3i *neighbors;
	const float (*sh)[4];
	uint8_t *ltm_write;
	uint8_t *vis_write;
	float *emission_write;
	const SafeFlag *cancel;
	SafeNumeric<uint32_t> *progress;
	const SurfaceHit *previous_hits = nullptr;
	SurfaceHit *output_hits = nullptr;
	const uint8_t *dirty = nullptr;
	SafeNumeric<uint32_t> *updated = nullptr;

	void bake_slice(uint32_t z, void *) {
		if (cancel && cancel->is_set()) {
			return;
		}
		for (int y = 0; y < grid.y; y++) {
			for (int x = 0; x < grid.x; x++) {
				const int64_t index = (int64_t(z) * grid.y + y) * grid.x + x;
				if (dirty && !dirty[index]) {
					continue;
				}
				if (updated) {
					updated->increment();
				}
				const Vector3 point = bounds.position + Vector3(x, y, z) * spacing;
				const bool recolor = previous_hits && dirty[index] == 1;
				const bool inside = recolor ? ltm_write[index * 36 + 35] != 0 : solid_bvh.contains(point);
				if (!recolor && output_hits) {
					for (int direction = 0; direction < 26; direction++) {
						output_hits[index * 26 + direction] = SurfaceHit();
					}
				}
				float matrix[3][16] = {};
				float visibility[4] = {};
				float source[12] = {};
				if (!inside) {
					visibility[0] = 1.0f / SH_C0;
					if (combined.has_geometry()) {
						for (int neighbor = 0; neighbor < 26; neighbor++) {
							const int64_t hit_index = index * 26 + neighbor;
							SurfaceHit hit;
							if (recolor) {
								hit = previous_hits[hit_index];
							} else {
								const Vector3 end = point + Vector3(neighbors[neighbor]) * spacing;
								combined.intersect_segment(point, end, &hit);
								if (output_hits) {
									output_hits[hit_index] = hit;
								}
							}
							if (hit.face < 0) {
								continue;
							}
							Color albedo;
							Color light;
							combined.sample_material(hit, albedo, light);
							for (int row = 0; row < 4; row++) {
								visibility[row] -= DIRECTION_WEIGHT * sh[neighbor][row];
								for (int channel = 0; channel < 3; channel++) {
									const float flip = row == 0 ? 1.0f : -1.0f;
									source[channel * 4 + row] += DIRECTION_WEIGHT * light[channel] * sh[neighbor][row] * flip;
								}
							}
							const Vector3 normal = combined.normal(hit);
							if (normal.dot(Vector3(neighbors[neighbor])) >= 0) {
								continue;
							}
							const float incident_diffuse[4] = { SH_C0, -(2.0f / 3.0f) * SH_C1 * normal.x, -(2.0f / 3.0f) * SH_C1 * normal.y, -(2.0f / 3.0f) * SH_C1 * normal.z };
							// Lambert radiance is view-independent: geometry selects outgoing rays,
							// while the hit normal alone determines the incident cosine response.
							for (int channel = 0; channel < 3; channel++) {
								for (int row = 0; row < 4; row++) {
									const float outgoing = sh[neighbor][row] * (row == 0 ? 1.0f : -1.0f);
									for (int column = 0; column < 4; column++) {
										matrix[channel][row + column * 4] += DIRECTION_WEIGHT * albedo[channel] * outgoing * incident_diffuse[column];
									}
								}
							}
						}
					}
				}
				encode_ltm(matrix, inside, ltm_write + index * 36);
				if (vis_write && !recolor) {
					for (int coefficient = 0; coefficient < 4; coefficient++) {
						encode_half(visibility[coefficient], vis_write + index * 8 + coefficient * 2);
					}
				}
				if (emission_write != nullptr) {
					for (int coefficient = 0; coefficient < 12; coefficient++) {
						emission_write[index * 12 + coefficient] = source[coefficient];
					}
				}
			}
		}
		if (progress) {
			progress->increment();
		}
	}
};

Error bake_volume_data(SceneGeometry &combined, const Array &sources, const AABB &bounds, float spacing, const Vector3i &grid, Ref<LocalLRTVolumeData> &r_data, const SafeFlag *p_cancel = nullptr, SafeNumeric<uint32_t> *p_progress = nullptr, const Ref<LocalLRTVolumeData> &p_previous = Ref<LocalLRTVolumeData>(), const Vector<SurfaceHit> *p_hits = nullptr, Vector<SurfaceHit> *r_hits = nullptr, const Vector<DirtyRegion> *p_changed = nullptr, SafeNumeric<uint32_t> *p_updated = nullptr, bool p_geometry_changed = true) {
	ERR_FAIL_COND_V(grid.x < 2 || grid.y < 2 || grid.z < 2, ERR_INVALID_DATA);
	int64_t count = grid.x;
	ERR_FAIL_COND_V(count > INT32_MAX / 36 / grid.y, ERR_OUT_OF_MEMORY);
	count *= grid.y;
	ERR_FAIL_COND_V(count > INT32_MAX / 36 / grid.z, ERR_OUT_OF_MEMORY);
	count *= grid.z;

	const bool incremental = p_previous.is_valid() && p_hits && p_hits->size() == count * 26;
	const Dictionary previous = incremental ? p_previous->get_data() : Dictionary();
	SceneGeometry &solid_bvh = combined;
	Vector<uint8_t> ltm = previous.get("ltm", Vector<uint8_t>());
	Vector<uint8_t> local_vis = previous.get("local_vis", Vector<uint8_t>());
	Vector<uint8_t> edge_masks = previous.get("edge_masks", Vector<uint8_t>());
	Vector<float> emission = previous.get("emission", Vector<float>());
	Vector<uint8_t> dirty;
	if (incremental) {
		ERR_FAIL_COND_V(dirty.resize(count) != OK, ERR_OUT_OF_MEMORY);
		dirty.fill(0);
		uint8_t *mark = dirty.ptrw();
		for (const DirtyRegion &changed : *p_changed) {
			const AABB domain = bounds.grow(spacing * 2.05f);
			if (!changed.bounds.overlaps(domain)) {
				continue;
			}
			const AABB affected = changed.bounds.clipped(domain).grow(spacing * (changed.flags == 2 ? 1.05f : 1.0f));
			if (!bounds.intersects_inclusive(affected)) {
				continue;
			}
			const Vector3 begin = (affected.position - bounds.position) / spacing;
			const Vector3 end = (affected.get_end() - bounds.position) / spacing;
			Vector3i first, last;
			for (int axis = 0; axis < 3; axis++) {
				first[axis] = CLAMP(int(Math::floor(begin[axis])), 0, grid[axis] - 1);
				last[axis] = CLAMP(int(Math::ceil(end[axis])), 0, grid[axis] - 1);
			}
			for (int z = first.z; z <= last.z; z++) {
				for (int y = first.y; y <= last.y; y++) {
					for (int x = first.x; x <= last.x; x++) {
						const int64_t index = (int64_t(z) * grid.y + y) * grid.x + x;
						mark[index] = MAX(mark[index], changed.flags);
					}
				}
			}
		}
	}
	ERR_FAIL_COND_V(ltm.resize(count * 36) != OK || local_vis.resize(count * 8) != OK || edge_masks.resize(count * 26) != OK, ERR_OUT_OF_MEMORY);
	const bool has_emission = combined.emission;
	if (has_emission) {
		const bool initialize = emission.is_empty();
		ERR_FAIL_COND_V(emission.resize(count * 12) != OK, ERR_OUT_OF_MEMORY);
		if (initialize) {
			emission.fill(0);
		}
	} else {
		emission.clear();
	}
	uint8_t *ltm_write = ltm.ptrw();
	uint8_t *vis_write = p_geometry_changed ? local_vis.ptrw() : nullptr;
	uint8_t *edge_write = p_geometry_changed ? edge_masks.ptrw() : nullptr;
	float *emission_write = emission.ptrw();

	Vector3i neighbors[26];
	float sh[26][4];
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
				for (int axis = 0; axis < 3; axis++) {
					sh[direction][axis + 1] = SH_C1 * normal[axis];
				}
				direction++;
			}
		}
	}

	ProbeBake probe_bake{ combined, solid_bvh, bounds, spacing, grid, neighbors, sh, ltm_write, vis_write, emission_write, p_cancel, p_progress };
	probe_bake.updated = p_updated;
	if (incremental) {
		probe_bake.previous_hits = p_hits->ptr();
		probe_bake.dirty = dirty.ptr();
		if (r_hits) {
			*r_hits = *p_hits;
			if (p_geometry_changed) {
				probe_bake.output_hits = r_hits->ptrw();
			}
		}
	} else if (r_hits) {
		ERR_FAIL_COND_V(r_hits->resize(count * 26) != OK, ERR_OUT_OF_MEMORY);
		probe_bake.output_hits = r_hits->ptrw();
	}
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const WorkerThreadPool::GroupID probes = pool->add_template_group_task(&probe_bake, &ProbeBake::bake_slice, nullptr, grid.z);
	pool->wait_for_group_task_completion(probes);
	if (p_cancel && p_cancel->is_set()) {
		return ERR_SKIP;
	}
	if (p_geometry_changed) {
		EdgeBake edge_bake{ combined, solid_bvh, bounds, spacing, grid, neighbors, edge_write };
		edge_bake.external_cancel = p_cancel;
		edge_bake.progress = p_progress;
		edge_bake.dirty = incremental ? dirty.ptr() : nullptr;
		const WorkerThreadPool::GroupID edges = pool->add_template_group_task(&edge_bake, &EdgeBake::bake_slice, nullptr, grid.z);
		pool->wait_for_group_task_completion(edges);
	}
	if (p_cancel && p_cancel->is_set()) {
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
	data["bake_complete"] = combined.issues.is_empty();
	data["bake_issues"] = combined.issues;
	Ref<LocalLRTVolumeData> baked;
	baked.instantiate();
	baked->set_data(data);
	ERR_FAIL_COND_V(!baked->has_data(), ERR_INVALID_DATA);
	r_data = baked;
	return OK;
}

struct BakeCache : RefCounted {
	GDSOFTCLASS(BakeCache, RefCounted);

public:
	Vector<CachedGeometryEntry> entries;
	uint32_t next_source_id = 1;
	Vector<SurfaceHit> probe_hits;
	HashMap<ObjectID, HashMap<int, int>> source_index;
	AABB bounds;
	Vector3i grid;
	float spacing = 0;
	int resolution = 0;

	const CachedGeometryEntry *find(ObjectID p_owner, int p_subinstance) const {
		const HashMap<int, int> *instances = source_index.getptr(p_owner);
		const int *index = instances ? instances->getptr(p_subinstance) : nullptr;
		return index ? &entries[*index] : nullptr;
	}

	Error add(const CachedGeometryEntry &p_entry) {
		if (entries.push_back(p_entry)) {
			return ERR_OUT_OF_MEMORY;
		}
		source_index[p_entry.owner].insert(p_entry.subinstance, entries.size() - 1);
		return OK;
	}
};

} // namespace

struct LRTBakeTask::Data {
	struct Walk {
		ObjectID node;
		int child = -1;
	};
	struct Detail {
		ObjectID target;
		Transform3D from_parent;
		AABB bounds;
		float spacing = 0;
		Ref<LocalLRTVolumeData> result;
	};
	struct Source {
		bool is_static = true;
		int visible_count = 0;
		Ref<LRTGeometryBinding> binding;
		int resolution = 32;
		bool multi = false;
		Array results;
		Ref<LRTInstanceGeometryData> result;
	};
	Vector<Detail> details;
	HashMap<ObjectID, Source> sources;
	bool auto_bounds = false;
	float margin = 0;
	float capture_spacing = 0;
	ObjectID target;
	ObjectID scene_root;
	Ref<LRTGeometryBinding> binding;
	Ref<Resource> previous;
	Ref<BakeCache> previous_cache;
	Ref<BakeCache> next_cache;
	Ref<Resource> result;
	Array failure_issues;
	Dictionary render_timings;
	Ref<LRTMaterialBaker::Context> context;
	Vector<Walk> pending;
	Vector<CapturedGeometry> inputs;
	CapturedGeometry building;
	ObjectID current;
	int subinstance = 0;
	int surface = 0;
	int instance_count = 0;
	bool building_started = false;
	bool volume = false;
	bool multi = false;
	bool cleanup = false;
	AABB bounds;
	Vector3i grid;
	float spacing = 0;
	int resolution = 32;
	SafeFlag cancelled;
	Status status = COLLECTING;
	Error error = OK;
	SafeNumeric<uint32_t> phase;
	SafeNumeric<uint32_t> completed_sources;
	SafeNumeric<uint32_t> completed_slices;
	SafeNumeric<uint32_t> filtered_sources;
	SafeNumeric<uint64_t> input_read_bytes;
	SafeNumeric<uint32_t> material_reuses;
	SafeNumeric<uint32_t> material_evaluations;
	SafeNumeric<uint32_t> bvh_reuses;
	SafeNumeric<uint32_t> bvh_builds;
	SafeNumeric<uint32_t> sdf_distance_reuses;
	SafeNumeric<uint32_t> updated_probes;
	bool volume_recolor = false;
	bool unchanged = false;
	WorkerThreadPool::TaskID worker = WorkerThreadPool::INVALID_TASK_ID;
	uint64_t started_usec = 0;
	uint64_t capture_usec = 0;
	uint64_t finished_usec = 0;
	uint64_t worker_usec = 0;
	uint64_t geometry_usec = 0;
	uint64_t sampling_usec = 0;
};

namespace {
Vector<Ref<LRTBakeTask>> active_bakes;
bool bake_shutdown = false;

bool bake_workers_available() {
#ifdef THREADS_ENABLED
	return WorkerThreadPool::get_singleton()->get_thread_count() > 0;
#else
	return false;
#endif
}
} //namespace

LRTBakeTask::LRTBakeTask() {
	data = memnew(Data);
	data->started_usec = OS::get_singleton()->get_ticks_usec();
}

LRTBakeTask::~LRTBakeTask() {
	memdelete(data);
}

Ref<LRTBakeTask> LRTBakeTask::start_volume(LRTVolume3D *p_volume, Node *p_root) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), Ref<LRTBakeTask>());
	Ref<LRTBakeTask> task;
	task.instantiate();
	Data &d = *task->data;
	d.volume = true;
	d.target = p_volume->get_instance_id();
	d.scene_root = p_root ? p_root->get_instance_id() : ObjectID();
	d.previous = p_volume->get_bake_data();
	d.previous_cache = p_volume->bake_cache;
	d.bounds = p_volume->get_bounds();
	d.grid = p_volume->get_grid_size();
	d.spacing = p_volume->get_probe_spacing();
	d.capture_spacing = d.spacing;
	Vector<Node *> pending_details;
	pending_details.push_back(p_volume);
	while (!pending_details.is_empty()) {
		Node *node = pending_details[pending_details.size() - 1];
		pending_details.resize(pending_details.size() - 1);
		if (auto *detail = Object::cast_to<LRTDetailVolume3D>(node)) {
			if (detail->get_group() == p_volume) {
				Data::Detail snapshot;
				snapshot.target = detail->get_instance_id();
				snapshot.from_parent = detail->get_global_transform().affine_inverse() * p_volume->get_global_transform();
				snapshot.bounds = detail->get_bounds();
				snapshot.spacing = detail->get_probe_spacing();
				d.details.push_back(snapshot);
				d.capture_spacing = MIN(d.capture_spacing, snapshot.spacing / snapshot.from_parent.basis.get_scale().x);
			}
		}
		for (int child = node->get_child_count() - 1; child >= 0; child--) {
			pending_details.push_back(node->get_child(child));
		}
	}
	if (LRTGI *gi = Object::cast_to<LRTGI>(p_volume)) {
		d.auto_bounds = true;
		d.margin = gi->get_extend_margin() / gi->get_global_transform().basis.get_scale().x;
	}
	for (const Data::Detail &detail : d.details) {
		const Transform3D to_parent = detail.from_parent.affine_inverse();
		if (!to_parent.is_finite() || !to_parent.basis.is_conformal() || to_parent.basis.determinant() <= 0 ||
				detail.spacing * to_parent.basis.get_scale().x >= d.spacing || (!d.auto_bounds && !d.bounds.encloses(to_parent.xform(detail.bounds)))) {
			d.error = ERR_INVALID_PARAMETER;
			Dictionary issue;
			issue["node_path"] = scene_node_path(Object::cast_to<Node>(ObjectDB::get_instance(detail.target)));
			issue["reason"] = "Detail requires a supported transform, containment in its parent and finer world-space probe spacing.";
			d.failure_issues.push_back(issue);
			break;
		}
	}
	const Transform3D transform = p_volume->is_inside_tree() ? p_volume->get_global_transform() : Transform3D();
	if (Object::cast_to<LRTDetailVolume3D>(p_volume) || (Object::cast_to<LRTGI>(p_volume) && LRTVolume3D::find_geometry_owner(p_volume))) {
		d.error = ERR_INVALID_PARAMETER;
	} else if (!p_volume->is_inside_tree() || !p_root || !p_root->is_inside_tree()) {
		d.error = ERR_UNCONFIGURED;
	} else if (!transform.is_finite() || !transform.basis.is_conformal() || transform.basis.determinant() <= 0) {
		d.error = ERR_INVALID_PARAMETER;
	} else if (d.error == OK) {
		if (!bake_workers_available()) {
			d.error = ERR_UNAVAILABLE;
			active_bakes.push_back(task);
			RenderingServerDefault::redraw_request();
			return task;
		}
		d.pending.push_back({ p_root->get_instance_id() });
		d.context = LRTMaterialBaker::capture_context();
	}
	for (const Ref<LRTBakeTask> &previous : active_bakes) {
		if (previous->data->volume && previous->data->target == d.target) {
			previous->cancel();
		}
	}
	active_bakes.push_back(task);
	RenderingServerDefault::redraw_request();
	return task;
}

Ref<LRTBakeTask> LRTBakeTask::start_geometry(const Ref<LRTGeometryBinding> &p_binding, GeometryInstance3D *p_instance) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), Ref<LRTBakeTask>());
	Ref<LRTBakeTask> task;
	task.instantiate();
	Data &d = *task->data;
	d.binding = p_binding;
	d.target = p_instance->get_instance_id();
	d.previous = p_binding->get_bake_data();
	d.previous_cache = p_binding->bake_cache;
	d.resolution = p_binding->get_sdf_resolution();
	d.multi = Object::cast_to<MultiMeshInstance3D>(p_instance) != nullptr;
	if (!LRTGeometryBinding::supports(p_instance) || LRTGeometryBinding::get_mesh(p_instance).is_null()) {
		d.error = ERR_INVALID_PARAMETER;
	} else {
		if (!bake_workers_available()) {
			d.error = ERR_UNAVAILABLE;
			active_bakes.push_back(task);
			RenderingServerDefault::redraw_request();
			return task;
		}
		d.pending.push_back({ p_instance->get_instance_id() });
		d.context = LRTMaterialBaker::capture_context();
	}
	for (const Ref<LRTBakeTask> &previous : active_bakes) {
		if (!previous->data->volume && previous->data->binding == p_binding) {
			previous->cancel();
		}
	}
	active_bakes.push_back(task);
	RenderingServerDefault::redraw_request();
	return task;
}

void LRTBakeTask::_collect() {
	Data &d = *data;
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	auto report = [&](const String &p_reason) {
		Dictionary issue;
		issue["node_path"] = d.building.path;
		issue["instance_index"] = d.building.instance_index;
		issue["surface"] = -1;
		issue["triangle"] = -1;
		issue["reason"] = p_reason;
		d.building.issues.push_back(issue);
	};
	while (d.error == OK && !d.cancelled.is_set() && OS::get_singleton()->get_ticks_usec() - start < 2000) {
		if (d.current.is_valid()) {
			auto *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(d.current));
			if (!instance) {
				d.cancelled.set();
				break;
			}
			if (!d.building_started) {
				d.building = CapturedGeometry();
				d.building.owner = instance->get_instance_id();
				d.building.path = scene_node_path(instance);
				d.building.instance_index = d.subinstance;
				d.building.source_mesh = LRTGeometryBinding::get_mesh(instance);
				d.building.source_bounds = d.building.source_mesh->get_aabb();
				d.surface = 0;
				if (d.volume) {
					auto *volume = Object::cast_to<LRTVolume3D>(ObjectDB::get_instance(d.target));
					if (!volume) {
						d.cancelled.set();
						break;
					}
					d.building.transform = LRTGeometry::VolumeFrame(volume).relative_transform(instance) * LRTGeometryBinding::get_instance_transform(instance, d.subinstance);
					const Transform3D &transform = d.building.transform;
					if (!transform.is_finite() || !transform.basis.is_conformal() || transform.basis.determinant() <= 0) {
						report("LRT instances require a positive uniform scale without shear.");
					}
				}
				if (auto *mesh_instance = Object::cast_to<MeshInstance3D>(instance)) {
					if (mesh_instance->get_skin().is_valid()) {
						report("Skeletal deformation is not supported by LRT baking.");
					}
					for (int shape = 0; shape < d.building.source_mesh->get_blend_shape_count(); shape++) {
						if (mesh_instance->get_blend_shape_value(shape) != 0) {
							report(vformat("Active blend shape %d is not supported by LRT baking.", shape));
							break;
						}
					}
				}
				if (auto *multi = Object::cast_to<MultiMeshInstance3D>(instance)) {
					if (multi->get_multimesh()->get_transform_format() != MultiMesh::TRANSFORM_3D) {
						report("LRT requires a 3D MultiMesh.");
					}
				}
				if (d.building.issues.is_empty() && d.building.surfaces.resize(d.building.source_mesh->get_surface_count()) != OK) {
					d.error = ERR_OUT_OF_MEMORY;
					break;
				}
				d.building_started = true;
			}
			if (d.surface < d.building.surfaces.size()) {
				if (d.building.source_mesh->surface_get_primitive_type(d.surface) == Mesh::PRIMITIVE_TRIANGLES) {
					const float texel = d.volume ? d.capture_spacing * 0.25f / d.building.transform.basis.get_scale().x : d.building.source_bounds.get_longest_axis_size() / (d.resolution - 5) * 0.25f;
					d.building.surfaces.write[d.surface] = LRTMaterialBaker::capture(instance, d.surface, texel, d.subinstance, d.context);
					if (d.building.surfaces[d.surface].is_null()) {
						d.error = ERR_OUT_OF_MEMORY;
						break;
					}
				}
				d.surface++;
				continue;
			}
			if (d.inputs.push_back(d.building) != OK) {
				d.error = ERR_OUT_OF_MEMORY;
				break;
			}
			d.building = CapturedGeometry();
			d.building_started = false;
			d.subinstance++;
			if (d.subinstance == d.instance_count) {
				d.current = ObjectID();
			}
			continue;
		}
		if (d.pending.is_empty()) {
			break;
		}
		Data::Walk &walk = d.pending.write[d.pending.size() - 1];
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(walk.node));
		if (!node) {
			d.pending.resize(d.pending.size() - 1);
			continue;
		}
		if (walk.child < 0) {
			walk.child = 0;
			auto *instance = Object::cast_to<GeometryInstance3D>(node);
			if (LRTGeometryBinding::supports(instance) && LRTGeometryBinding::get_mesh(instance).is_valid()) {
				bool include = !d.volume;
				if (d.volume) {
					auto *volume = Object::cast_to<LRTVolume3D>(ObjectDB::get_instance(d.target));
					include = volume && instance->is_visible_in_tree() && instance->get_world_3d() == volume->get_world_3d() &&
							(volume->collects_static(instance) || (d.auto_bounds && LRTVolume3D::find_geometry_owner(instance) == volume && instance->get_gi_mode() == GeometryInstance3D::GI_MODE_DYNAMIC));
					if (include) {
						Data::Source source;
						source.is_static = instance->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC;
						source.visible_count = LRTGeometryBinding::get_instance_count(instance);
						if (d.auto_bounds && LRTGeometryBinding::uses_sdf(instance)) {
							const Ref<LRTGeometryBinding> binding = LRTGeometryBinding::get_binding(instance);
							source.binding = binding.is_valid() ? Ref<LRTGeometryBinding>(binding->duplicate()) : Ref<LRTGeometryBinding>(memnew(LRTGeometryBinding));
							source.resolution = source.binding->get_sdf_resolution();
							source.multi = Object::cast_to<MultiMeshInstance3D>(instance) != nullptr;
						}
						d.sources.insert(instance->get_instance_id(), source);
					}
				}
				if (include) {
					d.instance_count = LRTGeometryBinding::get_instance_count(instance, d.volume && d.sources[instance->get_instance_id()].binding.is_null());
					if (d.instance_count > 0) {
						d.current = instance->get_instance_id();
						d.subinstance = 0;
					}
				}
			}
			continue;
		}
		if (d.volume && walk.child < node->get_child_count()) {
			const ObjectID child = node->get_child(walk.child++)->get_instance_id();
			d.pending.push_back({ child });
		} else {
			d.pending.resize(d.pending.size() - 1);
		}
	}
	d.capture_usec += OS::get_singleton()->get_ticks_usec() - start;
	if (d.error != OK || d.cancelled.is_set() || (d.pending.is_empty() && !d.current.is_valid())) {
		d.status = RUNNING;
		d.worker = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &LRTBakeTask::_work), false, "LRT explicit bake", true);
	}
}

void LRTBakeTask::_work() {
	Data &d = *data;
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	d.phase.set(1);
	Geometry combined;
	Array sources;
	Array instances;
	d.next_cache.instantiate();
	d.next_cache->next_source_id = d.previous_cache.is_valid() ? d.previous_cache->next_source_id : 1;
	d.next_cache->bounds = d.bounds;
	d.next_cache->grid = d.grid;
	d.next_cache->spacing = d.spacing;
	d.next_cache->resolution = d.resolution;
	bool unchanged = d.previous_cache.is_valid() && d.previous.is_valid() &&
			d.previous_cache->entries.size() == d.inputs.size() &&
			d.previous_cache->bounds == d.bounds && d.previous_cache->grid == d.grid &&
			d.previous_cache->spacing == d.spacing && d.previous_cache->resolution == d.resolution;
	for (const CapturedGeometry &input : d.inputs) {
		if (d.error != OK || d.cancelled.is_set()) {
			break;
		}
		const CachedGeometryEntry *previous = d.previous_cache.is_valid() ? d.previous_cache->find(input.owner, input.instance_index) : nullptr;
		CachedGeometryEntry entry;
		entry.owner = input.owner;
		if (d.volume) {
			entry.static_contribution = d.sources[input.owner].is_static && input.instance_index < d.sources[input.owner].visible_count;
		}
		entry.source_id = previous ? previous->source_id : d.next_cache->next_source_id++;
		entry.subinstance = input.instance_index;
		entry.transform = input.transform;
		Geometry &geometry = entry.geometry;
		const uint64_t geometry_started = OS::get_singleton()->get_ticks_usec();
		const AABB query_domain = d.bounds.grow(d.spacing * 2.05f);
		Array signatures;
		bool shareable = input.issues.is_empty();
		for (const Ref<LRTMaterialBaker::Request> &request : input.surfaces) {
			if (request.is_null()) {
				signatures.push_back(Variant());
				continue;
			}
			Dictionary signature;
			if (LRTMaterialBaker::get_signature(request, signature, &d.cancelled) != OK) {
				shareable = false;
				break;
			}
			signatures.push_back(signature);
		}
		Dictionary key;
		key["surfaces"] = signatures;
		if (shareable) {
			if (previous && previous->shared_geometry.is_valid() && previous->shared_geometry->key == key) {
				entry.shared_geometry = previous->shared_geometry;
			} else {
				entry.shared_geometry = SharedGeometryRecord::find(key);
			}
		}
		Error error = OK;
		if (entry.shared_geometry.is_valid()) {
			geometry = previous && previous->shared_geometry == entry.shared_geometry ? previous->geometry : entry.shared_geometry->geometry;
			if (previous && geometry.mesh.is_valid() && previous->geometry.mesh.is_valid() &&
					geometry.mesh != previous->geometry.mesh && geometry.attributes.size() == previous->geometry.attributes.size()) {
				bool equal = true;
				for (int face = 0; face < geometry.attributes.size() && equal; face++) {
					const Face3 current_face = geometry.face(face);
					const Face3 previous_face = previous->geometry.face(face);
					for (int corner = 0; corner < 3; corner++) {
						equal &= current_face.vertex[corner] == previous_face.vertex[corner];
					}
				}
				if (equal) {
					geometry.mesh = previous->geometry.mesh;
					geometry.closed = previous->geometry.closed;
					geometry.node_area = previous->geometry.node_area;
					geometry.containment_bounds = previous->geometry.containment_bounds;
				}
			}
			geometry.source_path = input.path;
			geometry.input_read_bytes = 0;
			geometry.material_evaluations = 0;
			geometry.material_reuses = 0;
			for (const CachedSurface &surface : geometry.cached_surfaces) {
				geometry.material_reuses += surface.valid;
			}
			geometry.bvh_reused = true;
		} else {
			error = geometry.prepare(nullptr, Transform3D(), 0, input.instance_index, &input, &d.cancelled, previous ? &previous->geometry : nullptr, d.volume && !d.auto_bounds ? &query_domain : nullptr);
			if (error == OK && shareable && geometry.issues.is_empty() && !geometry.range_filtered) {
				signatures.clear();
				for (int surface = 0; surface < input.surfaces.size(); surface++) {
					signatures.push_back(input.surfaces[surface].is_valid() ? Variant(geometry.cached_surfaces[surface].signature) : Variant());
				}
				key["surfaces"] = signatures;
				entry.shared_geometry.instantiate();
				entry.shared_geometry->key = key;
				entry.shared_geometry->geometry = geometry;
				error = entry.shared_geometry->publish();
			}
		}
		if (geometry.range_filtered) {
			d.filtered_sources.increment();
		}
		if (d.volume && error == OK) {
			entry.posed = geometry;
			error = entry.posed.set_pose(input.transform, input.instance_index, previous ? &previous->posed : nullptr);
		}
		d.geometry_usec += OS::get_singleton()->get_ticks_usec() - geometry_started;
		d.input_read_bytes.add(geometry.input_read_bytes);
		d.material_reuses.add(geometry.material_reuses);
		d.material_evaluations.add(geometry.material_evaluations);
		if (geometry.bvh_reused) {
			d.bvh_reuses.increment();
		} else if (geometry.mesh.is_valid()) {
			d.bvh_builds.increment();
		}
		if (error == ERR_OUT_OF_MEMORY || error == ERR_UNAVAILABLE || error == ERR_SKIP) {
			d.failure_issues.append_array(geometry.issues);
			d.error = error;
			break;
		}
		if (error != OK && geometry.issues.is_empty()) {
			geometry.report(nullptr, input.instance_index, -1, -1, vformat("Geometry evaluation failed: %s.", error_names[error]));
		}
		bool same_material = previous && geometry.cached_surfaces.size() == previous->geometry.cached_surfaces.size();
		for (int surface = 0; same_material && surface < geometry.cached_surfaces.size(); surface++) {
			same_material = geometry.cached_surfaces[surface].signature == previous->geometry.cached_surfaces[surface].signature;
		}
		entry.material_changed = !same_material;
		const bool source_unchanged = previous && geometry.bvh_reused && same_material &&
				geometry.issues == previous->geometry.issues && entry.transform == previous->transform && entry.static_contribution == previous->static_contribution;
		unchanged &= source_unchanged;
		combined.issues.append_array(d.volume && error == OK ? entry.posed.issues : geometry.issues);
		d.completed_sources.increment();
		if (!d.volume || d.sources[input.owner].binding.is_valid()) {
			d.phase.set(2);
			const int resolution = d.volume ? d.sources[input.owner].resolution : d.resolution;
			entry.sdf_resolution = resolution;
			const bool reuse_distance = previous && geometry.mesh == previous->geometry.mesh && previous->sdf.is_valid() &&
					previous->sdf_resolution == resolution;
			bool shared_sdf = false;
			if (source_unchanged && reuse_distance) {
				entry.sdf = previous->sdf;
				entry.sdf_hits = previous->sdf_hits;
			} else if (entry.shared_geometry.is_valid() && geometry.mesh.is_valid() && entry.shared_geometry->get_sdf(resolution, entry.sdf, entry.sdf_hits)) {
				shared_sdf = true;
			} else {
				const uint64_t sample_started = OS::get_singleton()->get_ticks_usec();
				d.error = bake_sdf(geometry, input.source_mesh, input.source_bounds, resolution, entry.sdf, &d.cancelled, &d.completed_slices,
						reuse_distance ? previous->sdf : Ref<LRTInstanceGeometryData>(), reuse_distance ? &previous->sdf_hits : nullptr, &entry.sdf_hits);
				d.sampling_usec += OS::get_singleton()->get_ticks_usec() - sample_started;
				if (d.error == OK && entry.shared_geometry.is_valid() && geometry.mesh.is_valid()) {
					entry.shared_geometry->store_sdf(resolution, entry.sdf, entry.sdf_hits);
				}
			}
			if (reuse_distance || shared_sdf) {
				d.sdf_distance_reuses.increment();
			}
			if (d.error != OK) {
				break;
			}
			if (d.volume) {
				d.sources[input.owner].results.push_back(entry.sdf);
			} else {
				instances.push_back(entry.sdf);
			}
			d.phase.set(1);
		}
		d.error = d.next_cache->add(entry);
	}
	if (d.volume && d.auto_bounds && d.error == OK && !d.cancelled.is_set()) {
		bool found = false;
		for (const CachedGeometryEntry &entry : d.next_cache->entries) {
			if (!entry.geometry.has_geometry() || entry.subinstance >= d.sources[entry.owner].visible_count) {
				continue;
			}
			const AABB bounds = entry.transform.xform(entry.geometry.mesh->get_aabb());
			d.bounds = found ? d.bounds.merge(bounds) : bounds;
			found = true;
		}
		if (!found) {
			d.error = ERR_INVALID_DATA;
			Dictionary issue;
			issue["reason"] = "LRTGI has no participating geometry from which to derive bounds.";
			issue["node_path"] = String();
			d.failure_issues.push_back(issue);
		} else {
			d.bounds = d.bounds.grow(d.margin);
			Vector3 end = d.bounds.get_end();
			for (int axis = 0; axis < 3; axis++) {
				d.bounds.position[axis] = Math::floor(d.bounds.position[axis] / d.spacing) * d.spacing;
				end[axis] = Math::ceil(end[axis] / d.spacing) * d.spacing;
				const double cells = Math::round((end[axis] - d.bounds.position[axis]) / d.spacing);
				if (cells >= INT32_MAX) {
					d.error = ERR_OUT_OF_MEMORY;
					break;
				}
				d.grid[axis] = int(cells) + 1;
			}
			d.bounds.size = end - d.bounds.position;
			d.next_cache->bounds = d.bounds;
			d.next_cache->grid = d.grid;
			unchanged &= d.previous_cache.is_valid() && d.previous_cache->bounds == d.bounds && d.previous_cache->grid == d.grid;
		}
	}
	d.unchanged = unchanged && d.error == OK;
	const bool same_layout = d.volume && d.previous_cache.is_valid() && d.previous.is_valid() &&
			d.previous_cache->bounds == d.bounds && d.previous_cache->grid == d.grid && d.previous_cache->spacing == d.spacing;
	bool same_geometry = same_layout;
	Vector<DirtyRegion> changed_bounds;
	for (const CachedGeometryEntry &entry : d.next_cache->entries) {
		const CachedGeometryEntry *previous = d.previous_cache.is_valid() ? d.previous_cache->find(entry.owner, entry.subinstance) : nullptr;
		const bool changed = !previous || entry.geometry.mesh != previous->geometry.mesh || entry.transform != previous->transform || entry.static_contribution != previous->static_contribution;
		if (changed) {
			same_geometry = false;
			if (previous && previous->posed.has_geometry()) {
				changed_bounds.push_back({ previous->posed.posed_inside, 2 });
			}
			if (entry.posed.has_geometry()) {
				changed_bounds.push_back({ entry.posed.posed_inside, 2 });
			}
		} else if (entry.material_changed && entry.posed.has_geometry()) {
			changed_bounds.push_back({ entry.posed.node_bounds(entry.posed.mesh->get_bvh().size() - 1), 1 });
		}
		if (d.volume && entry.static_contribution && entry.posed.has_geometry() && entry.posed.posed_inside.overlaps(d.bounds.grow(d.spacing * 2.05f))) {
			Dictionary source;
			source["node_path"] = entry.geometry.source_path;
			source["instance_index"] = entry.subinstance;
			source["transform"] = entry.transform;
			sources.push_back(source);
		}
	}
	if (d.volume && d.previous_cache.is_valid()) {
		for (const CachedGeometryEntry &entry : d.previous_cache->entries) {
			if (!d.next_cache->find(entry.owner, entry.subinstance)) {
				same_geometry = false;
				if (entry.posed.has_geometry()) {
					changed_bounds.push_back({ entry.posed.posed_inside, 2 });
				}
			}
		}
	}
	if (d.error == OK && !d.cancelled.is_set()) {
		d.phase.set(2);
		if (d.unchanged) {
			d.result = d.previous;
			d.next_cache->probe_hits = d.previous_cache->probe_hits;
		} else if (d.volume) {
			SceneGeometry scene(d.next_cache->entries, d.bounds.grow(d.spacing * 2.05f), !same_geometry);
			scene.issues = combined.issues;
			Ref<LocalLRTVolumeData> result;
			const uint64_t sample_started = OS::get_singleton()->get_ticks_usec();
			d.volume_recolor = same_geometry;
			d.error = bake_volume_data(scene, sources, d.bounds, d.spacing, d.grid, result, &d.cancelled, &d.completed_slices,
					same_layout ? Ref<LocalLRTVolumeData>(d.previous) : Ref<LocalLRTVolumeData>(), same_layout ? &d.previous_cache->probe_hits : nullptr,
					&d.next_cache->probe_hits, &changed_bounds, &d.updated_probes, !same_geometry);
			d.sampling_usec += OS::get_singleton()->get_ticks_usec() - sample_started;
			d.result = result;
		} else if (!d.multi && instances.size() == 1) {
			d.result = instances[0];
		} else {
			Dictionary value;
			value["instances"] = instances;
			value["bake_complete"] = combined.issues.is_empty();
			value["bake_issues"] = combined.issues;
			Ref<LRTInstanceGeometryData> result;
			result.instantiate();
			result->set_data(value);
			d.result = result;
		}
	}
	if (d.volume && d.error == OK && !d.cancelled.is_set()) {
		for (Data::Detail &detail : d.details) {
			const Transform3D to_parent = detail.from_parent.affine_inverse();
			const AABB parent_bounds = to_parent.xform(detail.bounds);
			if (!detail.from_parent.is_finite() || !detail.from_parent.basis.is_conformal() || detail.from_parent.basis.determinant() <= 0 ||
					!d.bounds.encloses(parent_bounds) || detail.spacing * to_parent.basis.get_scale().x >= d.spacing) {
				d.error = ERR_INVALID_PARAMETER;
				Dictionary issue;
				issue["node_path"] = String();
				issue["reason"] = "Detail must be fully contained in its parent and use finer world-space probe spacing with a supported transform.";
				d.failure_issues.push_back(issue);
				break;
			}
			Vector<CachedGeometryEntry> entries = d.next_cache->entries;
			for (CachedGeometryEntry &entry : entries) {
				if (entry.geometry.has_geometry()) {
					d.error = entry.posed.set_pose(detail.from_parent * entry.transform, entry.subinstance, nullptr);
					if (d.error != OK) {
						break;
					}
				}
			}
			if (d.error != OK) {
				break;
			}
			Vector3i grid;
			for (int axis = 0; axis < 3; axis++) {
				grid[axis] = int(Math::ceil(detail.bounds.size[axis] / detail.spacing)) + 1;
			}
			SceneGeometry scene(entries, detail.bounds.grow(detail.spacing * 2.05f), true);
			scene.issues = combined.issues;
			d.error = bake_volume_data(scene, sources, detail.bounds, detail.spacing, grid, detail.result, &d.cancelled, &d.completed_slices);
			if (d.error != OK || d.cancelled.is_set()) {
				break;
			}
			Dictionary value = detail.result->get_data();
			value["group_transform"] = to_parent;
			detail.result->set_data(value);
		}
		if (d.error == OK && !d.cancelled.is_set()) {
			Ref<LocalLRTVolumeData> parent_result = d.result;
			Dictionary value = parent_result->get_data();
			Array details;
			Array sdfs;
			for (const Data::Detail &detail : d.details) {
				details.push_back(detail.result);
			}
			for (KeyValue<ObjectID, Data::Source> &source : d.sources) {
				if (source.value.results.is_empty()) {
					continue;
				}
				if (source.value.multi) {
					source.value.result.instantiate();
					Dictionary value;
					value["instances"] = source.value.results;
					value["bake_complete"] = true;
					source.value.result->set_data(value);
				} else {
					source.value.result = source.value.results[0];
				}
				sdfs.push_back(source.value.result);
			}
			value["group_details"] = details;
			value["group_sdfs"] = sdfs;
			// Do not mutate the previous resource on an otherwise unchanged bake.
			Ref<LocalLRTVolumeData> result;
			result.instantiate();
			result->set_data(value);
			d.result = result;
		}
	}
	if (d.cancelled.is_set()) {
		d.error = ERR_SKIP;
		d.result.unref();
	}
	if (d.cancelled.is_set() || d.error != OK) {
		for (const CapturedGeometry &input : d.inputs) {
			for (const Ref<LRTMaterialBaker::Request> &request : input.surfaces) {
				if (request.is_valid()) {
					LRTMaterialBaker::cancel(request);
				}
			}
		}
		for (const Ref<LRTMaterialBaker::Request> &request : d.building.surfaces) {
			if (request.is_valid()) {
				LRTMaterialBaker::cancel(request);
			}
		}
	}
	if (d.cancelled.is_set() || d.error != OK) {
		for (const CapturedGeometry &input : d.inputs) {
			for (const Ref<LRTMaterialBaker::Request> &request : input.surfaces) {
				if (request.is_valid()) {
					Dictionary ignored;
					LRTMaterialBaker::get_signature(request, ignored, &d.cancelled);
				}
			}
		}
		for (const Ref<LRTMaterialBaker::Request> &request : d.building.surfaces) {
			if (request.is_valid()) {
				Dictionary ignored;
				LRTMaterialBaker::get_signature(request, ignored, &d.cancelled);
			}
		}
	}
	if (d.context.is_valid()) {
		d.render_timings = LRTMaterialBaker::get_render_timings(d.context);
		LRTMaterialBaker::clear_source_cache(d.context);
	}
	d.building = CapturedGeometry();
	d.inputs.clear();
	d.context.unref();
	d.pending.clear();
	d.worker_usec = OS::get_singleton()->get_ticks_usec() - started;
}

void LRTBakeTask::_finish() {
	Data &d = *data;
	if (d.volume) {
		for (const Data::Detail &detail : d.details) {
			if (!ObjectDB::get_instance(detail.target)) {
				d.cancelled.set();
			}
		}
		for (const KeyValue<ObjectID, Data::Source> &source : d.sources) {
			if (source.value.binding.is_valid() && !ObjectDB::get_instance(source.key)) {
				d.cancelled.set();
			}
		}
	}
	const Ref<LocalLRTVolumeData> volume_result = d.result;
	const Array volume_issues = volume_result.is_valid() ? Array(volume_result->get_data().get("bake_issues", Array())) : Array();
	if (!volume_issues.is_empty() && d.error == OK) {
		d.error = ERR_INVALID_DATA;
	}
	if (d.cancelled.is_set() || d.error == ERR_SKIP || !ObjectDB::get_instance(d.target)) {
		d.status = CANCELLED;
	} else if (d.error != OK || d.result.is_null() || !volume_issues.is_empty()) {
		d.status = FAILED;
	} else {
		d.status = SUCCEEDED;
		if (d.volume) {
			auto *volume = Object::cast_to<LRTVolume3D>(ObjectDB::get_instance(d.target));
			// The worker cleanup still owns the old snapshot during this commit.
			volume->bake_cache.unref();
			volume->set_bake_data(d.result);
			volume->bake_cache = d.next_cache;
			volume->notify_property_list_changed();
			for (const Data::Detail &detail : d.details) {
				auto *node = Object::cast_to<LRTDetailVolume3D>(ObjectDB::get_instance(detail.target));
				node->set_bake_data(detail.result);
			}
			for (KeyValue<ObjectID, Data::Source> &source : d.sources) {
				if (source.value.binding.is_null() || source.value.results.is_empty()) {
					continue;
				}
				auto *instance = Object::cast_to<GeometryInstance3D>(ObjectDB::get_instance(source.key));
				source.value.binding->set_bake_data(source.value.result);
				instance->set_meta(LRTGeometryBinding::get_meta_name(), source.value.binding);
				// Native instance overrides preserve bindings on imported/nested scenes.
				// Node.owner is used only for serialization, never geometry membership.
				Node *root = Object::cast_to<Node>(ObjectDB::get_instance(d.scene_root));
				for (Node *owner = instance->get_owner(); root && owner && owner != root && root->is_ancestor_of(owner); owner = owner->get_owner()) {
					root->set_editable_instance(owner, true);
				}
			}
		} else {
			d.binding->bake_cache.unref();
			d.binding->set_bake_data(d.result);
			d.binding->bake_cache = d.next_cache;
		}
		d.status = get_issues().is_empty() ? SUCCEEDED : COMPLETED_WITH_ISSUES;
	}
	if (d.status == COMPLETED_WITH_ISSUES || d.status == FAILED) {
		report_bake_issues(get_issues());
	}
	d.phase.set(3);
	d.finished_usec = OS::get_singleton()->get_ticks_usec();
	emit_signal(SNAME("finished"));
}

void LRTBakeTask::process() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	for (int index = active_bakes.size() - 1; index >= 0; index--) {
		const Ref<LRTBakeTask> task = active_bakes[index];
		Data &d = *task->data;
		if (!task->is_completed() && !ObjectDB::get_instance(d.target)) {
			d.cancelled.set();
		}
		if (d.cleanup) {
			if (WorkerThreadPool::get_singleton()->is_task_completed(d.worker)) {
				WorkerThreadPool::get_singleton()->wait_for_task_completion(d.worker);
				active_bakes.remove_at(index);
				if (!task->is_completed()) {
					task->_finish();
				}
			}
			continue;
		}
		if (d.status == COLLECTING && !bake_workers_available()) {
			d.error = ERR_UNAVAILABLE;
			active_bakes.remove_at(index);
			task->_finish();
			continue;
		}
		if (d.status == COLLECTING) {
			task->_collect();
		}
		if (d.status == RUNNING && WorkerThreadPool::get_singleton()->is_task_completed(d.worker)) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(d.worker);
			if (!d.cancelled.is_set()) {
				task->_finish();
			}
			d.cleanup = true;
			d.worker = WorkerThreadPool::get_singleton()->add_task(callable_mp(task.ptr(), &LRTBakeTask::_cleanup), false, "LRT bake data cleanup");
		}
	}
}

void LRTBakeTask::_cleanup() {
	data->previous.unref();
	data->previous_cache.unref();
	data->next_cache.unref();
	data->binding.unref();
	if (data->cancelled.is_set() || data->error != OK) {
		data->result.unref();
	}
}

void LRTBakeTask::retire_cache(Ref<RefCounted> &r_cache) {
	if (r_cache.is_null()) {
		return;
	}
	if (Thread::is_main_thread() && !bake_shutdown && bake_workers_available()) {
		Ref<LRTBakeTask> retirement;
		retirement.instantiate();
		retirement->data->previous_cache = r_cache;
		retirement->data->status = CANCELLED;
		retirement->data->cleanup = true;
		active_bakes.push_back(retirement);
		retirement->data->worker = WorkerThreadPool::get_singleton()->add_task(callable_mp(retirement.ptr(), &LRTBakeTask::_cleanup), false, "LRT snapshot retirement");
		RenderingServerDefault::redraw_request();
	}
	r_cache.unref();
}

bool LRTBakeTask::has_pending() {
	return !active_bakes.is_empty();
}

void LRTBakeTask::cancel_all() {
	for (const Ref<LRTBakeTask> &task : active_bakes) {
		task->cancel();
	}
}

void LRTBakeTask::finish_shutdown() {
	bake_shutdown = true;
	while (!active_bakes.is_empty() || LRTMaterialBaker::has_pending()) {
		LRTMaterialBaker::process();
		RenderingServer::get_singleton()->sync();
		process();
		OS::get_singleton()->delay_usec(1000);
	}
}

bool LRTBakeTask::cancel() {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), false);
	if (is_completed()) {
		return false;
	}
	data->cancelled.set();
	return true;
}

bool LRTBakeTask::is_completed() const {
	return data->status >= SUCCEEDED;
}

LRTBakeTask::Status LRTBakeTask::get_status() const {
	return data->status;
}

Dictionary LRTBakeTask::get_progress() const {
	const Data &d = *data;
	Dictionary progress;
	progress["status"] = d.status;
	progress["phase"] = d.phase.get();
	progress["completed_sources"] = d.completed_sources.get();
	progress["completed_slices"] = d.completed_slices.get();
	progress["filtered_sources"] = d.filtered_sources.get();
	progress["input_read_bytes"] = int64_t(d.input_read_bytes.get());
	progress["material_reuses"] = d.material_reuses.get();
	progress["material_evaluations"] = d.material_evaluations.get();
	progress["bvh_reuses"] = d.bvh_reuses.get();
	progress["bvh_builds"] = d.bvh_builds.get();
	progress["sdf_distance_reuses"] = d.sdf_distance_reuses.get();
	progress["unchanged"] = is_completed() && d.unchanged;
	progress["volume_recolor"] = is_completed() && d.volume_recolor;
	progress["updated_probes"] = d.updated_probes.get();
	progress["capture_usec"] = d.capture_usec;
	progress["elapsed_usec"] = (is_completed() ? d.finished_usec : OS::get_singleton()->get_ticks_usec()) - d.started_usec;
	if (is_completed()) {
		progress["render_thread"] = d.render_timings;
		progress["worker_usec"] = d.worker_usec;
		progress["geometry_usec"] = d.geometry_usec;
		progress["sampling_usec"] = d.sampling_usec;
		progress["error"] = int(d.error);
		progress["error_message"] = String(error_names[d.error]);
	}
	return progress;
}

Ref<Resource> LRTBakeTask::get_result() const {
	return is_completed() && (data->status == SUCCEEDED || data->status == COMPLETED_WITH_ISSUES) ? data->result : Ref<Resource>();
}

Array LRTBakeTask::get_issues() const {
	if (!is_completed()) {
		return Array();
	}
	if (!data->failure_issues.is_empty() || data->result.is_null()) {
		return data->failure_issues;
	}
	if (const Ref<LocalLRTVolumeData> volume = data->result; volume.is_valid()) {
		return volume->get_data().get("bake_issues", Array());
	}
	const Ref<LRTInstanceGeometryData> geometry = data->result;
	return geometry->get_data().get("bake_issues", Array());
}

void LRTBakeTask::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &LRTBakeTask::cancel);
	ClassDB::bind_method(D_METHOD("is_completed"), &LRTBakeTask::is_completed);
	ClassDB::bind_method(D_METHOD("get_status"), &LRTBakeTask::get_status);
	ClassDB::bind_method(D_METHOD("get_progress"), &LRTBakeTask::get_progress);
	ClassDB::bind_method(D_METHOD("get_result"), &LRTBakeTask::get_result);
	ClassDB::bind_method(D_METHOD("get_issues"), &LRTBakeTask::get_issues);
	BIND_ENUM_CONSTANT(COLLECTING);
	BIND_ENUM_CONSTANT(RUNNING);
	BIND_ENUM_CONSTANT(SUCCEEDED);
	BIND_ENUM_CONSTANT(COMPLETED_WITH_ISSUES);
	BIND_ENUM_CONSTANT(FAILED);
	BIND_ENUM_CONSTANT(CANCELLED);
	ADD_SIGNAL(MethodInfo("finished"));
}

Dictionary LRTGeometryBaker::inspect_probe(LRTVolume3D *p_volume, const Vector3i &p_coordinate) {
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
	Error error = collect_static_geometry(p_volume, p_volume->get_tree()->get_edited_scene_root() ? p_volume->get_tree()->get_edited_scene_root() : p_volume->get_tree()->get_current_scene(), combined, solids, sources);
	report_bake_issues(combined.issues);
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
