/**************************************************************************/
/*  lrt_transfer_geometry.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be        */
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


#include "lrt_transfer.h"

#include <algorithm>
#include <limits>

namespace lrt {
namespace {

bool ray_bounds(const Vec3 &p_origin, const Vec3 &p_direction, const Vec3 &p_minimum, const Vec3 &p_maximum, double p_limit) {
	double first = 0.0;
	double last = p_limit;
	for (int axis = 0; axis < 3; axis++) {
		if (p_direction[axis] == 0.0) {
			if (p_origin[axis] < p_minimum[axis] || p_origin[axis] > p_maximum[axis]) {
				return false;
			}
			continue;
		}
		double a = (p_minimum[axis] - p_origin[axis]) / p_direction[axis];
		double b = (p_maximum[axis] - p_origin[axis]) / p_direction[axis];
		if (a > b) {
			std::swap(a, b);
		}
		first = std::max(first, a);
		last = std::min(last, b);
		if (first > last) {
			return false;
		}
	}
	return true;
}

double bounds_distance_squared(const Vec3 &p_point, const Vec3 &p_minimum, const Vec3 &p_maximum) {
	double result = 0.0;
	for (int axis = 0; axis < 3; axis++) {
		const double distance = p_point[axis] - std::clamp(p_point[axis], p_minimum[axis], p_maximum[axis]);
		result += distance * distance;
	}
	return result;
}

bool ray_triangle(const MeshTriangle &p_triangle, const Vec3 &p_origin, const Vec3 &p_direction,
		double p_limit, double &r_distance, Vec3 &r_barycentric) {
	const Vec3 a = p_triangle.position[1] - p_triangle.position[0];
	const Vec3 b = p_triangle.position[2] - p_triangle.position[0];
	const Vec3 h = cross(p_direction, b);
	const double determinant = dot(a, h);
	if (std::abs(determinant) <= 1e-12 * length(a) * length(b)) {
		return false;
	}
	const double inverse = 1.0 / determinant;
	const Vec3 s = p_origin - p_triangle.position[0];
	const double u = inverse * dot(s, h);
	if (u < 0.0 || u > 1.0) {
		return false;
	}
	const Vec3 q = cross(s, a);
	const double v = inverse * dot(p_direction, q);
	if (v < 0.0 || u + v > 1.0) {
		return false;
	}
	const double distance = inverse * dot(b, q);
	if (distance <= 0.0 || distance > p_limit) {
		return false;
	}
	r_distance = distance;
	r_barycentric = Vec3(1.0 - u - v, u, v);
	return true;
}

// Ericson's closest-point regions, retaining barycentrics for coverage evaluation.
Vec3 closest_barycentric(const MeshTriangle &p_triangle, const Vec3 &p_point) {
	const Vec3 &a = p_triangle.position[0];
	const Vec3 ab = p_triangle.position[1] - a;
	const Vec3 ac = p_triangle.position[2] - a;
	const Vec3 ap = p_point - a;
	const double d1 = dot(ab, ap);
	const double d2 = dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) {
		return Vec3(1, 0, 0);
	}
	const Vec3 bp = p_point - p_triangle.position[1];
	const double d3 = dot(ab, bp);
	const double d4 = dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) {
		return Vec3(0, 1, 0);
	}
	const double vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
		const double v = d1 / (d1 - d3);
		return Vec3(1.0 - v, v, 0);
	}
	const Vec3 cp = p_point - p_triangle.position[2];
	const double d5 = dot(ab, cp);
	const double d6 = dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) {
		return Vec3(0, 0, 1);
	}
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
		const double w = d2 / (d2 - d6);
		return Vec3(1.0 - w, 0, w);
	}
	const double va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) {
		const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return Vec3(0, 1.0 - w, w);
	}
	const double inverse = 1.0 / (va + vb + vc);
	const double v = vb * inverse;
	const double w = vc * inverse;
	return Vec3(1.0 - v - w, v, w);
}

Vec3 triangle_point(const MeshTriangle &p_triangle, const Vec3 &p_barycentric) {
	return p_triangle.position[0] * p_barycentric.x + p_triangle.position[1] * p_barycentric.y + p_triangle.position[2] * p_barycentric.z;
}

Vec3 triangle_normal(const MeshTriangle &p_triangle) {
	return normalized(cross(p_triangle.position[2] - p_triangle.position[0], p_triangle.position[1] - p_triangle.position[0]));
}

} // namespace

TransferGeometry::TransferGeometry(std::shared_ptr<const std::vector<MeshTriangle>> p_triangles) :
		triangles(std::move(p_triangles)) {
	if (!triangles) {
		return;
	}
	for (int i = 0; i < int(triangles->size()); i++) {
		const MeshTriangle &triangle = (*triangles)[i];
		if (length_squared(cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0])) > 0.0) {
			order.push_back(i);
		}
	}
	if (!order.empty()) {
		nodes.reserve(order.size() * 2);
		build_node(0, int(order.size()));
	}
}

int TransferGeometry::build_node(int p_first, int p_count) {
	Node node;
	node.first = p_first;
	node.count = p_count;
	node.minimum = (*triangles)[order[p_first]].position[0];
	node.maximum = node.minimum;
	for (int i = p_first; i < p_first + p_count; i++) {
		for (const Vec3 &vertex : (*triangles)[order[i]].position) {
			for (int axis = 0; axis < 3; axis++) {
				node.minimum[axis] = std::min(node.minimum[axis], vertex[axis]);
				node.maximum[axis] = std::max(node.maximum[axis], vertex[axis]);
			}
		}
	}
	const int result = int(nodes.size());
	nodes.push_back(node);
	if (p_count <= 8) {
		return result;
	}
	const Vec3 extent = node.maximum - node.minimum;
	int axis = extent.y > extent.x ? 1 : 0;
	if (extent.z > extent[axis]) {
		axis = 2;
	}
	const int middle = p_first + p_count / 2;
	std::nth_element(order.begin() + p_first, order.begin() + middle, order.begin() + p_first + p_count,
			[&](int p_a, int p_b) {
				const MeshTriangle &a = (*triangles)[p_a];
				const MeshTriangle &b = (*triangles)[p_b];
				const double ca = a.position[0][axis] + a.position[1][axis] + a.position[2][axis];
				const double cb = b.position[0][axis] + b.position[1][axis] + b.position[2][axis];
				return ca == cb ? p_a < p_b : ca < cb;
			});
	const int left = build_node(p_first, middle - p_first);
	const int right = build_node(middle, p_first + p_count - middle);
	nodes[result].left = left;
	nodes[result].right = right;
	nodes[result].count = 0;
	return result;
}

uint64_t TransferGeometry::payload_bytes() const {
	return sizeof(TransferGeometry) + order.capacity() * sizeof(int) + nodes.capacity() * sizeof(Node);
}

Vec3 TransferGeometry::get_minimum() const {
	return nodes.empty() ? Vec3() : nodes[0].minimum;
}

Vec3 TransferGeometry::get_maximum() const {
	return nodes.empty() ? Vec3() : nodes[0].maximum;
}

bool TransferGeometry::trace(const Vec3 &p_origin, const Vec3 &p_direction, double p_radius,
		const TransferSurfaceQuery &p_surface, TransferSurface &r_surface) const {
	if (nodes.empty()) {
		return false;
	}
	bool found = false;
	double closest = p_radius;
	int closest_triangle = std::numeric_limits<int>::max();
	// Median splits bound traversal depth by the bit width of the triangle count.
	std::array<int, 64> stack;
	int pending = 1;
	stack[0] = 0;
	while (pending > 0) {
		const Node &node = nodes[stack[--pending]];
		if (!ray_bounds(p_origin, p_direction, node.minimum, node.maximum, closest)) {
			continue;
		}
		if (node.count == 0) {
			stack[pending++] = node.right;
			stack[pending++] = node.left;
			continue;
		}
		for (int i = node.first; i < node.first + node.count; i++) {
			const int triangle_index = order[i];
			const MeshTriangle &triangle = (*triangles)[triangle_index];
			double distance;
			Vec3 barycentric;
			if (!ray_triangle(triangle, p_origin, p_direction, closest, distance, barycentric) ||
					(distance == closest && triangle_index >= closest_triangle)) {
				continue;
			}
			TransferSurface surface;
			surface.normal = triangle_normal(triangle);
			if (dot(surface.normal, p_direction) > 0.0) {
				surface.normal = -surface.normal;
			}
			if (!p_surface(triangle_index, triangle_point(triangle, barycentric), barycentric, surface)) {
				continue;
			}
			closest = distance;
			closest_triangle = triangle_index;
			r_surface = surface;
			found = true;
		}
	}
	return found;
}

bool TransferGeometry::within_surface_band(const Vec3 &p_point, double p_half_width, const TransferSurfaceQuery &p_surface) const {
	if (nodes.empty()) {
		return false;
	}
	const double distance_squared = p_half_width * p_half_width;
	std::array<int, 64> stack;
	int pending = 1;
	stack[0] = 0;
	while (pending > 0) {
		const Node &node = nodes[stack[--pending]];
		if (bounds_distance_squared(p_point, node.minimum, node.maximum) > distance_squared) {
			continue;
		}
		if (node.count == 0) {
			stack[pending++] = node.right;
			stack[pending++] = node.left;
			continue;
		}
		for (int i = node.first; i < node.first + node.count; i++) {
			const int triangle_index = order[i];
			const MeshTriangle &triangle = (*triangles)[triangle_index];
			const Vec3 barycentric = closest_barycentric(triangle, p_point);
			const Vec3 point = triangle_point(triangle, barycentric);
			if (length_squared(point - p_point) > distance_squared) {
				continue;
			}
			TransferSurface surface;
			surface.normal = triangle_normal(triangle);
			if (p_surface(triangle_index, point, barycentric, surface)) {
				return true;
			}
		}
	}
	return false;
}

TransferBakeResult bake_transfer_field(const TransferGeometry &p_geometry, const TransferSurfaceQuery &p_surface,
		const TransferBakeSettings &p_settings, const std::atomic<bool> *p_cancel, int p_threads, std::atomic<uint64_t> *p_evaluated) {
	TransferBakeResult result;
	if (!std::isfinite(p_settings.support_radius) || p_settings.support_radius <= 0.0 ||
			!std::isfinite(p_settings.spacing) || p_settings.spacing <= 0.0 || p_settings.angular_samples < 1) {
		result.error = TRANSFER_BAKE_INVALID_SETTINGS;
		return result;
	}
	if (p_geometry.empty()) {
		result.error = TRANSFER_BAKE_EMPTY_GEOMETRY;
		return result;
	}
	const Vec3 margin(p_settings.support_radius + p_settings.spacing, p_settings.support_radius + p_settings.spacing, p_settings.support_radius + p_settings.spacing);
	const Vec3 minimum = p_geometry.get_minimum() - margin;
	const Vec3 maximum = p_geometry.get_maximum() + margin;
	int size[3];
	int64_t count = 1;
	for (int axis = 0; axis < 3; axis++) {
		const double dimension = std::ceil((maximum[axis] - minimum[axis]) / p_settings.spacing) + 1.0;
		if (!std::isfinite(dimension) || dimension > INT32_MAX || count * int64_t(dimension) > INT32_MAX) {
			result.error = TRANSFER_BAKE_TOO_LARGE;
			return result;
		}
		size[axis] = int(dimension);
		count *= size[axis];
	}
	result.field.origin = minimum;
	result.field.spacing = p_settings.spacing;
	result.field.support_radius = p_settings.support_radius;
	result.field.angular_samples = p_settings.angular_samples;
	// Each worker slice owns complete brick layers, avoiding eight copies of the
	// same brick when neighboring sample rows are evaluated concurrently.
	const int layers = (size[1] + TransferField::BRICK_SIZE - 1) / TransferField::BRICK_SIZE;
	std::vector<TransferField> fields(layers);
	std::atomic<uint64_t> evaluated{ 0 };
	std::vector<Vec3> directions(size_t(p_settings.angular_samples));
	for (int i = 0; i < p_settings.angular_samples; i++) {
		directions[i] = transfer_sample_direction(i, p_settings.angular_samples);
	}
	const TransferSurfaceQuery geometry_coverage = [](int, const Vec3 &, const Vec3 &, TransferSurface &) { return true; };
	parallel_for(layers, p_threads, [&](int p_layer) {
		for (int y = p_layer * TransferField::BRICK_SIZE; y < std::min((p_layer + 1) * TransferField::BRICK_SIZE, size[1]); y++) {
			for (int z = 0; z < size[2]; z++) {
				for (int x = 0; x < size[0]; x++) {
					if (p_cancel && p_cancel->load()) {
						return;
					}
					const Vec3 point = minimum + Vec3(x, y, z) * p_settings.spacing;
					// This is only broad-phase rejection: alpha at the closest triangle
					// point must not discard other valid hits within the support sphere.
					if (!p_geometry.within_surface_band(point, p_settings.support_radius, geometry_coverage)) {
						continue;
					}
					TransferSample sample;
					for (const Vec3 &direction : directions) {
						if (p_cancel && p_cancel->load()) {
							return;
						}
						TransferSurface surface;
						const bool hit = p_geometry.trace(point, direction, p_settings.support_radius, p_surface, surface);
						integrate_transfer_direction(sample, direction, 4.0 * PI / p_settings.angular_samples,
								hit, surface.reflectance, surface.emission);
					}
					fields[p_layer].set(x, y, z, sample);
					evaluated.fetch_add(1, std::memory_order_relaxed);
					if (p_evaluated != nullptr) {
						p_evaluated->fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
		}
	});
	if (p_cancel && p_cancel->load()) {
		result.error = TRANSFER_BAKE_CANCELLED;
		result.field.bricks.clear();
		return result;
	}
	for (TransferField &field : fields) {
		result.field.bricks.merge(field.bricks);
	}
	result.evaluated_samples = evaluated.load();
	return result;
}

} // namespace lrt
