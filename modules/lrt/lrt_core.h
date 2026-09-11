/**************************************************************************/
/*  lrt_core.h                                                            */
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

#pragma once

// Local Radiance Transfer numeric core.
//
// This file is a direct port of the JavaScript prototype
// (G:\lrt_external_test, transport baseline bef08c9 / visual baseline 524a290).
// It intentionally uses no engine types so it can be compiled and diffed against
// the prototype outside of Godot. Engine-facing code lives in lrt_volume.h and must
// only adapt data layout and platform APIs, never the algorithm.
//
// Source of truth per function:
//   make_grid / index_of / probe_point / basis / positive_basis / cosine_basis
//     src/core.js
//   box contains / trace                        src/geometry-query.js
//   build_local_data                            src/core.js (BVH/analytic backend)
//   bake_box_color_sdf / sample_color_sdf       src/primitive-gi.js
//   build_sdf_local_data                        src/sdf-local.js
//   build_local_visibility / sh_triple_product  src/core.js

#include <cstdint>
#include <cmath>
#include <vector>

namespace lrt {

constexpr double PI = 3.141592653589793;
// src/core.js computes these; the GPU shaders hard-code rounded literals instead.
constexpr double C0 = 0.2820947917738781; // 1 / sqrt(4 * PI)
constexpr double C1 = 0.4886025119029199; // sqrt(3 / (4 * PI))
constexpr double WEIGHT = 4.0 * PI / 26.0;
constexpr int DIRECTION_COUNT = 26;
constexpr int TRUNK = 8;
constexpr double GEOMETRY_EPSILON = 1e-6;

struct Vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;

	Vec3() = default;
	Vec3(double p_x, double p_y, double p_z) :
			x(p_x), y(p_y), z(p_z) {}

	double &operator[](int p_axis) { return p_axis == 0 ? x : (p_axis == 1 ? y : z); }
	double operator[](int p_axis) const { return p_axis == 0 ? x : (p_axis == 1 ? y : z); }
};

inline Vec3 operator+(const Vec3 &p_a, const Vec3 &p_b) { return Vec3(p_a.x + p_b.x, p_a.y + p_b.y, p_a.z + p_b.z); }
inline Vec3 operator-(const Vec3 &p_a, const Vec3 &p_b) { return Vec3(p_a.x - p_b.x, p_a.y - p_b.y, p_a.z - p_b.z); }
inline Vec3 operator-(const Vec3 &p_a) { return Vec3(-p_a.x, -p_a.y, -p_a.z); }
inline Vec3 operator*(const Vec3 &p_a, double p_b) { return Vec3(p_a.x * p_b, p_a.y * p_b, p_a.z * p_b); }
inline Vec3 operator*(double p_b, const Vec3 &p_a) { return p_a * p_b; }
inline Vec3 operator/(const Vec3 &p_a, double p_b) { return Vec3(p_a.x / p_b, p_a.y / p_b, p_a.z / p_b); }
inline double dot(const Vec3 &p_a, const Vec3 &p_b) { return p_a.x * p_b.x + p_a.y * p_b.y + p_a.z * p_b.z; }
inline Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3(p_a.y * p_b.z - p_a.z * p_b.y, p_a.z * p_b.x - p_a.x * p_b.z, p_a.x * p_b.y - p_a.y * p_b.x);
}
inline double length_squared(const Vec3 &p_a) { return dot(p_a, p_a); }
inline double length(const Vec3 &p_a) { return std::sqrt(dot(p_a, p_a)); }
inline Vec3 normalized(const Vec3 &p_a) { return p_a / length(p_a); }
inline double hypot3(double p_x, double p_y, double p_z) { return std::sqrt(p_x * p_x + p_y * p_y + p_z * p_z); }

struct Direction {
	int offset[3];
	Vec3 direction;
};

// src/core.js DIRECTIONS: z outer, y middle, x inner, (0,0,0) skipped.
const Direction *directions();

// SH basis sequences from src/core.js.
void basis(const Vec3 &p_direction, double r_out[4]);
void positive_basis(const Vec3 &p_direction, double r_out[4]);
void cosine_basis(const Vec3 &p_normal, double r_out[4]);
void sh_triple_product(const double p_a[4], const double p_b[4], double r_out[4]);

struct Grid {
	Vec3 min;
	int size[3] = { 0, 0, 0 };
	double spacing = 0.0;
	int width = 0;
	int height = 0;
	int count = 0;
};

// prototype make_grid: fixed region [-3,-0.5,-3] .. [3,3.5,3], optionally expanded by bounds.
Grid make_grid(double p_spacing);
Grid make_grid(double p_spacing, const Vec3 &p_bounds_min, const Vec3 &p_bounds_max);
inline int index_of(const Grid &p_grid, int p_x, int p_y, int p_z) { return p_x + p_z * p_grid.size[0] + p_y * p_grid.width; }
inline Vec3 probe_point(const Grid &p_grid, int p_x, int p_y, int p_z) {
	return Vec3(p_grid.min.x + (p_x + 0.5) * p_grid.spacing,
			p_grid.min.y + (p_y + 0.5) * p_grid.spacing,
			p_grid.min.z + (p_z + 0.5) * p_grid.spacing);
}
inline bool inside(const Grid &p_grid, int p_x, int p_y, int p_z) {
	return p_x >= 0 && p_y >= 0 && p_z >= 0 && p_x < p_grid.size[0] && p_y < p_grid.size[1] && p_z < p_grid.size[2];
}

struct Box {
	Vec3 min;
	Vec3 max;
	Vec3 color;
};

struct Hit {
	bool valid = false;
	double distance = 0.0;
	Vec3 position;
	Vec3 normal;
	Vec3 color;
};

// Analytic box backend: geometry-query.js restricted to the box list used by the fixtures.
class BoxQuery {
	std::vector<Box> boxes;

public:
	explicit BoxQuery(const std::vector<Box> &p_boxes) :
			boxes(p_boxes) {}
	bool contains(const Vec3 &p_point) const;
	bool trace(const Vec3 &p_origin, const Vec3 &p_direction, double p_limit, Hit &r_hit) const;
};

struct ColorSdfField {
	Vec3 min;
	int size[3] = { 0, 0, 0 };
	double cell = 0.0;
	double distance_scale = 0.0;
	std::vector<int16_t> distance;
	int color_size[3] = { 0, 0, 0 };
	std::vector<uint8_t> color;
};

struct ColorSdfSample {
	bool valid = false;
	double distance = 0.0;
	Vec3 normal;
	Vec3 color;
};

// primitive-gi.js bakeBoxSDF -> bakeColorSDF with the analytic box closure.
ColorSdfField bake_box_color_sdf(const Vec3 &p_extent, const Vec3 &p_color, int p_resolution = 24);
ColorSdfSample sample_color_sdf(const ColorSdfField &p_field, const Vec3 &p_point);

struct SdfPrimitive {
	Vec3 position;
	ColorSdfField field;
	Vec3 bounds_min;
	Vec3 bounds_max;
};

SdfPrimitive make_sdf_primitive(const Vec3 &p_position, ColorSdfField p_field);

// ---------------------------------------------------------------------------
// Triangle meshes (prototype src/model-geometry.js / src/geometry-query.js).
//
// The driver supplies world-space triangles, which is exactly the frame the
// prototype bakes and samples in: gltf-import.js centers the asset and lab.js
// keeps position [0,0,0] with scale 1, so no primitive transform is needed.
// ---------------------------------------------------------------------------

struct MeshTriangle {
	Vec3 position[3];
	Vec3 normal[3]; // geometric normals are used for winding; see mesh_contains
	Vec3 color[3];
};

struct TriangleMesh {
	std::vector<MeshTriangle> triangles;
	// Preorder BVH with escape indices, mirroring prototype buildBVH().
	std::vector<Vec3> node_min;
	std::vector<Vec3> node_max;
	std::vector<int> node_escape;
	std::vector<int> node_leaf; // triangle start * 4 + count - 1, or -1 for interior nodes
	std::vector<int> order; // triangle indices in leaf order
	std::vector<int> shell; // per triangle: welded shell root
	std::vector<uint8_t> shell_closed; // per triangle: 1 when its shell is watertight
	bool has_closed_shell = false;
};

struct MeshSample {
	bool valid = false;
	double distance = 0.0;
	Vec3 normal;
	Vec3 color;
};

TriangleMesh build_triangle_mesh(std::vector<MeshTriangle> p_triangles);

// geometry-query.js closest(): only the distance is needed for the SDF bake, and
// attributes=true additionally interpolates the vertex color and the triangle normal.
MeshSample mesh_closest(const TriangleMesh &p_mesh, const Vec3 &p_point, bool p_attributes);

// geometry-query.js contains(): ray winding over watertight, consistently oriented shells.
bool mesh_contains(const TriangleMesh &p_mesh, const Vec3 &p_point);

// primitive-gi.js bakeMeshSDF(): Color SDF in the mesh's own bounds.
ColorSdfField bake_mesh_color_sdf(const TriangleMesh &p_mesh, int p_resolution = 128);

// Display layout used by the fragment shader's traceMesh (float4 per index).
std::vector<float> mesh_node_data(const TriangleMesh &p_mesh);
std::vector<float> mesh_triangle_data(const TriangleMesh &p_mesh);
std::vector<float> mesh_material_data();

struct LocalField {
	std::vector<float> material; // count * 4
	std::vector<float> matrices; // count * 48
	std::vector<uint32_t> links; // count
	std::vector<float> local_visibility; // count * 4
	std::vector<float> receivers; // variable length, vec4 slots
	int solid_count = 0;
	int surface_count = 0;
	int classification_mismatches = 0;
	int trunk_count = 0;
};

// src/core.js buildLocalData (BVH backend) and src/sdf-local.js buildSDFLocalData.
LocalField build_local_data(const Grid &p_grid, const BoxQuery &p_query);
LocalField build_sdf_local_data(const Grid &p_grid, const std::vector<SdfPrimitive> &p_primitives);

// src/core.js buildLocalVisibility.
void build_local_visibility(LocalField &r_field);

} // namespace lrt
