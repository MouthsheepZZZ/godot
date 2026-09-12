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

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
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

// Deterministic parallel loop for the bake: runs p_body(i) for every i in [0, p_count)
// spread over at most p_threads OS threads (p_threads <= 1 stays serial). Every body must
// write only to the slots it owns, which keeps the result bit-identical to the serial loop;
// this is an implementation-level change to the prototype's cost, not to its algorithm.
void parallel_for(int p_count, int p_threads, const std::function<void(int)> &p_body);

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

inline Vec3 operator+(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3(p_a.x + p_b.x, p_a.y + p_b.y, p_a.z + p_b.z);
}
inline Vec3 operator-(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3(p_a.x - p_b.x, p_a.y - p_b.y, p_a.z - p_b.z);
}
inline Vec3 operator-(const Vec3 &p_a) {
	return Vec3(-p_a.x, -p_a.y, -p_a.z);
}
inline Vec3 operator*(const Vec3 &p_a, double p_b) {
	return Vec3(p_a.x * p_b, p_a.y * p_b, p_a.z * p_b);
}
inline Vec3 operator*(double p_b, const Vec3 &p_a) {
	return p_a * p_b;
}
inline Vec3 operator/(const Vec3 &p_a, double p_b) {
	return Vec3(p_a.x / p_b, p_a.y / p_b, p_a.z / p_b);
}
inline double dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a.x * p_b.x + p_a.y * p_b.y + p_a.z * p_b.z;
}
inline Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3(p_a.y * p_b.z - p_a.z * p_b.y, p_a.z * p_b.x - p_a.x * p_b.z, p_a.x * p_b.y - p_a.y * p_b.x);
}
inline double length_squared(const Vec3 &p_a) {
	return dot(p_a, p_a);
}
inline double length(const Vec3 &p_a) {
	return std::sqrt(dot(p_a, p_a));
}
inline Vec3 normalized(const Vec3 &p_a) {
	return p_a / length(p_a);
}
inline double hypot3(double p_x, double p_y, double p_z) {
	return std::sqrt(p_x * p_x + p_y * p_y + p_z * p_z);
}

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
// Same lattice rule as make_grid, with the region supplied by the caller (LRTVolume3D
// derives it from the node size). The two-argument form additionally expands the region to
// cover geometry with the prototype's two air cells.
Grid make_grid_sized(double p_spacing, const Vec3 &p_min, const Vec3 &p_size);
Grid make_grid_sized(double p_spacing, const Vec3 &p_min, const Vec3 &p_size, const Vec3 &p_bounds_min, const Vec3 &p_bounds_max);
inline int index_of(const Grid &p_grid, int p_x, int p_y, int p_z) {
	return p_x + p_z * p_grid.size[0] + p_y * p_grid.width;
}
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

// Asset-owned geometry field. It contains no material data, so instances with different
// colours or emission can share the same allocation and derived-cache entry.
struct SdfGeometryField {
	Vec3 min;
	int size[3] = { 0, 0, 0 };
	double cell = 0.0;
	double distance_scale = 0.0;
	std::vector<int16_t> distance;
	int closed_shell_count = 0;
	int open_shell_count = 0;
	int surface_voxels = 0;
	uint64_t ray_queries = 0;
};

// Instance-owned material field sampled on the geometry field's coarser 4-cell lattice.
// Emission is a separate HDR source and never changes the shared geometry field.
struct SdfInstanceField {
	int color_size[3] = { 0, 0, 0 };
	std::vector<uint8_t> albedo;
	std::vector<float> emission;
};

struct ColorSdfSample {
	bool valid = false;
	double distance = 0.0;
	Vec3 normal;
	Vec3 color;
	Vec3 emission;
};

// primitive-gi.js bakeBoxSDF -> bakeColorSDF, split into the shared distance field and
// instance material field without changing their sample lattices.
SdfGeometryField bake_box_sdf(const Vec3 &p_extent, int p_resolution = 24, const std::atomic<bool> *p_cancel = nullptr);
SdfInstanceField bake_constant_instance_field(const SdfGeometryField &p_geometry, const Vec3 &p_albedo, const Vec3 &p_emission = Vec3());
ColorSdfSample sample_sdf_fields(const SdfGeometryField &p_geometry, const SdfInstanceField &p_instance, const Vec3 &p_point);

struct SdfPrimitive {
	std::shared_ptr<const SdfGeometryField> geometry;
	SdfInstanceField instance;
	// Asset-local SDF to volume-local affine transform. Keeping the full basis allows a
	// non-uniformly scaled instance to reuse the same asset field.
	Vec3 origin;
	Vec3 basis_x = Vec3(1.0, 0.0, 0.0);
	Vec3 basis_y = Vec3(0.0, 1.0, 0.0);
	Vec3 basis_z = Vec3(0.0, 0.0, 1.0);
	Vec3 bounds_min;
	Vec3 bounds_max;
	// Prototype PrimitiveGI.signature: which baked field this is plus its world matrix. The
	// incremental trunk test below is the only consumer.
	uint64_t signature = 0;

	// PrimitiveGI.sample: world point -> local field sample -> world units.
	ColorSdfSample sample(const Vec3 &p_point) const;
};

struct PrimitiveTransform {
	Vec3 origin;
	Vec3 basis_x = Vec3(1.0, 0.0, 0.0);
	Vec3 basis_y = Vec3(0.0, 1.0, 0.0);
	Vec3 basis_z = Vec3(0.0, 0.0, 1.0);

	bool is_identity() const;
};

SdfPrimitive make_sdf_primitive(std::shared_ptr<const SdfGeometryField> p_geometry, SdfInstanceField p_instance,
		const PrimitiveTransform &p_transform, uint64_t p_signature = 0);

// Prototype PrimitiveGI.signature inputs: the baked field's own content plus the transform.
uint64_t box_field_signature(const Vec3 &p_extent, int p_resolution);
uint64_t instance_field_signature(const SdfInstanceField &p_field);
uint64_t primitive_signature(uint64_t p_field_signature, uint64_t p_instance_signature, const PrimitiveTransform &p_transform);

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

// Renderer-executed static material output in an instance-local 3D lookup. The transform maps
// asset-local positions to normalized capture coordinates without consuming any mesh attribute.
struct MaterialCapture {
	int size[3] = { 0, 0, 0 };
	Vec3 uvw_offset;
	Vec3 uvw_basis_x;
	Vec3 uvw_basis_y;
	Vec3 uvw_basis_z;
	std::vector<float> albedo;
	std::vector<float> emission;
	std::vector<uint8_t> occupied;
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
	std::vector<int8_t> shell_outward_sign; // corrects either consistent winding to outward
	int closed_shell_count = 0;
	int open_shell_count = 0;
	bool has_closed_shell = false;
};

struct MeshSample {
	bool valid = false;
	double distance = 0.0;
	Vec3 position;
	Vec3 normal;
	Vec3 color;
};

TriangleMesh build_triangle_mesh(std::vector<MeshTriangle> p_triangles);

// geometry-query.js closest(): only the distance is needed for the SDF bake, and
// attributes=true additionally interpolates the vertex color and the triangle normal.
MeshSample mesh_closest(const TriangleMesh &p_mesh, const Vec3 &p_point, bool p_attributes);

// geometry-query.js contains(): ray winding over watertight, consistently oriented shells.
bool mesh_contains(const TriangleMesh &p_mesh, const Vec3 &p_point);

enum MeshSdfBakeError {
	MESH_SDF_BAKE_OK,
	MESH_SDF_BAKE_EMPTY,
	MESH_SDF_BAKE_DEGENERATE_BOUNDS,
	MESH_SDF_BAKE_TOO_LARGE,
	MESH_SDF_BAKE_NO_SURFACE,
	MESH_SDF_BAKE_CANCELLED,
};

struct MeshSdfBakeResult {
	SdfGeometryField field;
	MeshSdfBakeError error = MESH_SDF_BAKE_OK;
};

// R4 production generator: conservative triangle voxelization and outside flood fill determine
// the sign without rays; BVH closest-point queries retain continuous-surface distance magnitude.
// Open shells remain unsigned two-sided surfaces.
MeshSdfBakeResult bake_mesh_sdf(const TriangleMesh &p_mesh, int p_resolution = 128,
		const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);

// Prototype src/primitive-gi.js reference path. It retains closest-point and winding-ray
// queries for offline numerical comparison, but runtime preparation never calls it.
SdfGeometryField bake_mesh_sdf_reference(const TriangleMesh &p_mesh, int p_resolution = 128,
		const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);
SdfInstanceField bake_mesh_instance_field(const TriangleMesh &p_mesh, const SdfGeometryField &p_geometry,
		const MaterialCapture *p_material = nullptr, const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);

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
	std::vector<float> receiver_emission; // one vec4 per receiver, HDR RGB
	int solid_count = 0;
	int surface_count = 0;
	int classification_mismatches = 0;
	int trunk_count = 0;
	// Prototype's dirtyTrunkCount: how many trunks the incremental build had to recompute.
	int dirty_trunk_count = 0;
};

// Prototype src/sdf-local.js cache: the trunk signatures and the per-probe samples of a built
// field. The next build copies every probe whose trunk signature did not change, which is what
// makes an edit cost the edited region instead of the whole volume. `local` points at the field
// these entries describe and stays owned by the volume, so nothing is duplicated.
struct LocalCache {
	uint64_t grid_key = 0;
	std::vector<uint64_t> trunk_signatures;
	std::vector<ColorSdfSample> samples;
	std::vector<uint8_t> sampled;
	const LocalField *local = nullptr;
};

// src/core.js buildLocalData (BVH backend) and src/sdf-local.js buildSDFLocalData.
LocalField build_local_data(const Grid &p_grid, const BoxQuery &p_query, const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);
LocalField build_sdf_local_data(const Grid &p_grid, const std::vector<SdfPrimitive> &p_primitives,
		const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1,
		const LocalCache *p_previous = nullptr, LocalCache *r_cache = nullptr);

// src/core.js buildLocalVisibility.
void build_local_visibility(LocalField &r_field, int p_threads = 1);

} // namespace lrt
