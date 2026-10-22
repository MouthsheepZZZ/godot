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
// Local records retain independent RGB single-bounce operators and HDR EmissionSH.
// Engine-facing data layout and RenderingDevice APIs live in lrt_volume.h.
//
// Geometry and grid utilities originated in the JavaScript prototype
// (G:\lrt_external_test, transport baseline bef08c9 / visual baseline 524a290):
//   make_grid / index_of / probe_point / basis / cosine_basis
//     src/core.js
//   box contains                                src/geometry-query.js
//   build_local_data                            src/core.js (analytic backend)
//   bake_box_color_sdf / sample_color_sdf       src/primitive-gi.js
//   build_sdf_local_data                        src/sdf-local.js
//   sh_triple_product                          src/core.js

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace lrt {

constexpr double PI = 3.141592653589793;
// src/core.js computes these; the GPU shaders hard-code rounded literals instead.
constexpr double C0 = 0.2820947917738781; // 1 / sqrt(4 * PI)
constexpr double C1 = 0.4886025119029199; // sqrt(3 / (4 * PI))
constexpr double WEIGHT = 4.0 * PI / 26.0;
constexpr int DIRECTION_COUNT = 26;
constexpr int TRUNK = 8;
inline uint64_t boundary_connection_count(int p_x, int p_y, int p_z) {
	return 2 * (uint64_t(p_x) * p_y + uint64_t(p_x) * p_z + uint64_t(p_y) * p_z);
}
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

// Analytic box backend: geometry-query.js restricted to the box list used by the fixtures.
class BoxQuery {
	std::vector<Box> boxes;

public:
	explicit BoxQuery(const std::vector<Box> &p_boxes) :
			boxes(p_boxes) {}
	bool contains(const Vec3 &p_point) const;
};

// Sparse 4x4x4 surface coverage inside captured parent cells. An empty index array
// denotes a deliberately unrefined field; otherwise every covered parent has a mask.
struct SubcellCoverage {
	static constexpr int RESOLUTION = 4;
	static constexpr uint32_t NO_MASK = UINT32_MAX;
	std::vector<uint32_t> indices;
	std::vector<uint64_t> masks;

	bool valid_for(const std::vector<uint32_t> &p_cells) const;
};

// Asset-owned geometry field. It contains no material data, so instances with different
// colours or emission can share the same allocation and derived-cache entry.
struct CoarseGeometryField {
	Vec3 min;
	int size[3] = { 0, 0, 0 };
	double cell = 0.0;
	// Low 26 bits are covered surface orientations; bit 26 marks a known solid cell.
	std::vector<uint32_t> cells;
	SubcellCoverage subcells;
	int closed_shell_count = 0;
	int open_shell_count = 0;
	int surface_voxels = 0;
};

// Instance-owned materials on covered geometry cells; native raster captures use stride two.
// Emission is a separate HDR source and never changes the shared geometry field.
enum EmissionSides : uint8_t {
	EMISSION_FRONT = 1,
	EMISSION_BACK = 2,
	EMISSION_BOTH = EMISSION_FRONT | EMISSION_BACK,
};

struct SurfaceMaterialField {
	int cell_stride = 1;
	int color_size[3] = { 0, 0, 0 };
	std::vector<uint8_t> albedo;
	std::vector<float> emission;
	// Empty means front-facing emission; otherwise one side mask per material cell.
	std::vector<uint8_t> emission_sides;
};

struct SurfaceSample {
	bool valid = false;
	int cell = -1;
	double fraction = 0.0;
	Vec3 position;
	Vec3 normal;
	Vec3 color;
	Vec3 emission;
	uint32_t layer_mask = 1;
};

// Conservative box coverage is shared independently of constant instance appearance.
CoarseGeometryField bake_box_sdf(const Vec3 &p_extent, int p_resolution = 24, const std::atomic<bool> *p_cancel = nullptr);
SurfaceMaterialField bake_constant_instance_field(const Vec3 &p_albedo, const Vec3 &p_emission = Vec3(), uint8_t p_emission_sides = EMISSION_FRONT);
SurfaceSample sample_sdf_fields(const CoarseGeometryField &p_geometry, const SurfaceMaterialField &p_instance,
		const Vec3 &p_point, const Vec3 &p_half_extent = Vec3());

struct LocalInputSignatures {
	uint64_t geometry = 0;
	uint64_t reflectance = 0;
	uint64_t emission = 0;
};
static_assert(sizeof(LocalInputSignatures) == 24);

struct PrimitiveTransform {
	Vec3 origin;
	Vec3 basis_x = Vec3(1.0, 0.0, 0.0);
	Vec3 basis_y = Vec3(0.0, 1.0, 0.0);
	Vec3 basis_z = Vec3(0.0, 0.0, 1.0);

	bool is_identity() const;
	Vec3 xform(const Vec3 &p_point) const;
	Vec3 xform_extent(const Vec3 &p_extent) const;
	PrimitiveTransform operator*(const PrimitiveTransform &p_transform) const;
};

struct MeshTriangle;
struct TransferInstance;
struct TransferSample;

// Cached occupied Probe -> covered parent-cell relation. Precomputed inputs use
// membership only, with zero as the cell token; appearance is always separate.
struct ProbeOccupancy {
	std::vector<std::pair<int, int>> cells;
	int find_cell(int p_probe) const;
};

struct GeometryInstance {
	std::shared_ptr<const TransferInstance> precomputed;
	double support_radius = 1.0; // Mesh-local, independent of the voxel capture lattice.
	double occupancy_band_width = 0.05; // Full asset-local band width; precomputed only.
	std::shared_ptr<const CoarseGeometryField> geometry;
	std::shared_ptr<const SurfaceMaterialField> instance;
	// Asset-local coverage to Volume-local affine transform. Keeping the full basis allows a
	// non-uniformly scaled instance to reuse the same asset field.
	Vec3 origin;
	Vec3 basis_x = Vec3(1.0, 0.0, 0.0);
	Vec3 basis_y = Vec3(0.0, 1.0, 0.0);
	Vec3 basis_z = Vec3(0.0, 0.0, 1.0);
	// Transform-only terms are prepared once per instance, not once per geometry query.
	Vec3 cofactor_x = Vec3(1.0, 0.0, 0.0);
	Vec3 cofactor_y = Vec3(0.0, 1.0, 0.0);
	Vec3 cofactor_z = Vec3(0.0, 0.0, 1.0);
	double determinant = 1.0;
	Vec3 bounds_min;
	Vec3 bounds_max;
	// Combined content and transform identity for whole-field cache invalidation.
	uint64_t signature = 0;
	// Content identities exclude the instance transform and identify shared asset/material inputs.
	uint64_t asset_signature = 0;
	uint64_t material_signature = 0;
	uint64_t instance_cache_signature = 0;
	LocalInputSignatures input_signatures;
	uint32_t layer_mask = 1;
	// Present only for Static GI. Dynamic GI keeps direct subcell queries.
	std::shared_ptr<const std::vector<MeshTriangle>> static_triangles;
	PrimitiveTransform mesh_to_volume;
	uint64_t static_mesh_signature = 0;

	Vec3 transfer_support_extent(double p_spacing = 0.0) const;
	bool occupies_probe(const Vec3 &p_point) const;

	// Voxel-only queries; precomputed inputs never enter voxel capture or tracing.
	SurfaceSample sample(const Vec3 &p_point, double p_footprint = 0.0) const;
	SurfaceSample sample_cell(int p_cell, const Vec3 &p_point, const Vec3 *p_towards = nullptr) const;
};

GeometryInstance make_sdf_primitive(std::shared_ptr<const CoarseGeometryField> p_geometry, std::shared_ptr<const SurfaceMaterialField> p_instance,
		const PrimitiveTransform &p_transform, uint64_t p_signature = 0, uint32_t p_layer_mask = 1,
		uint64_t p_asset_signature = 0, uint64_t p_material_signature = 0,
		const LocalInputSignatures *p_material_signatures = nullptr);

// A volume-owned bidirectional spatial index. Static registrations also retain a sparse
// Probe occupancy relation; neither registration kind owns geometry or previous versions.
class TrunkIndex {
	using PrimitiveKey = std::array<uint64_t, 15>;
	struct Registration {
		std::vector<int> trunks;
		std::vector<int> inputs;
		std::shared_ptr<const ProbeOccupancy> occupancy;
	};
	uint64_t grid_key = 0;
	std::map<PrimitiveKey, Registration> registrations;
	std::vector<PrimitiveKey> previous_inputs;
	std::vector<std::array<uint64_t, 3>> previous_material_inputs;
	std::vector<std::vector<Registration *>> trunk_members;
	std::vector<std::vector<int>> candidates;
	std::vector<uint64_t> signatures;
	std::vector<LocalInputSignatures> input_signatures;
	uint64_t candidate_references = 0;
	std::vector<const ProbeOccupancy *> probe_occupancy;

	static PrimitiveKey make_key(const GeometryInstance &p_primitive);

public:
	struct UpdateStats {
		int added = 0;
		int removed = 0;
		uint64_t overlap_tests = 0;
		int occupancy_built = 0;
		int occupancy_reused = 0;
		uint64_t occupancy_candidates = 0;
	};

private:
	UpdateStats update_stats;

public:
	TrunkIndex() = default;
	TrunkIndex(const TrunkIndex &) = delete;
	TrunkIndex &operator=(const TrunkIndex &) = delete;

	void update(const Grid &p_grid, const std::vector<GeometryInstance> &p_primitives);
	bool prepare_probe_occupancy(const Grid &p_grid, const std::vector<GeometryInstance> &p_primitives,
			const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);
	const ProbeOccupancy *get_probe_occupancy(int p_input) const { return probe_occupancy.empty() ? nullptr : probe_occupancy[size_t(p_input)]; }
	const std::vector<int> &get_candidates(int p_trunk) const { return candidates[size_t(p_trunk)]; }
	const std::vector<uint64_t> &get_signatures() const { return signatures; }
	const std::vector<LocalInputSignatures> &get_input_signatures() const { return input_signatures; }
	const UpdateStats &get_update_stats() const { return update_stats; }
	size_t get_registration_count() const { return registrations.size(); }
	uint64_t get_candidate_references() const { return candidate_references; }
	uint64_t payload_bytes() const;
};

// Borrows candidate assets and cached occupancy. Voxel transfer uses 26 fixed
// neighbor queries; precomputed transfer uses asset-local lookup and SH rotation.
class TrunkSurfaceWorkspace {
	struct Entry {
		const GeometryInstance *instance;
		const ProbeOccupancy *occupancy;
	};
	std::vector<Entry> instances;
	double spacing = 0.0;

public:
	bool build(const Grid &p_grid, int p_trunk, const std::vector<GeometryInstance> &p_instances,
			const std::vector<int> &p_candidates, const std::atomic<bool> *p_cancel = nullptr, const TrunkIndex *p_index = nullptr);
	// p_probe and p_point describe the same lattice point; occupancy may be cached.
	TransferSample sample_transfer(int p_probe, const Vec3 &p_point, bool &r_occupied) const;
	bool sample(const Vec3 &p_point, SurfaceSample &r_sample) const;
	bool sample_probe(int p_probe, const Vec3 &p_point, SurfaceSample &r_sample) const;
	size_t get_instance_count() const { return instances.size(); }
	uint64_t payload_bytes() const;
};

// Content identities of the baked fields and instance transform.
uint64_t box_field_signature(const Vec3 &p_extent, int p_resolution);
uint64_t instance_field_signature(const SurfaceMaterialField &p_field);
uint64_t material_component_signature(const SurfaceMaterialField &p_field, bool p_emission);
uint64_t primitive_signature(uint64_t p_field_signature, uint64_t p_instance_signature, const PrimitiveTransform &p_transform);

// ---------------------------------------------------------------------------
// Triangle meshes (prototype src/model-geometry.js).
//
// The driver supplies world-space triangles, which is exactly the frame the
// prototype bakes and samples in: gltf-import.js centers the asset and lab.js
// keeps position [0,0,0] with scale 1, so no primitive transform is needed.
// ---------------------------------------------------------------------------

// Positions use Godot's clockwise front-face winding.
struct MeshTriangle {
	Vec3 position[3];
	Vec3 color[3];
};

struct MeshCopy {
	PrimitiveTransform transform;
	std::shared_ptr<const std::vector<MeshTriangle>> triangles;
};

// Renderer-executed static material output in an instance-local 3D lookup. The transform maps
// asset-local positions to normalized capture coordinates without consuming any mesh attribute.
struct MaterialCapture {
	// Authored capture dependencies; unchanged channels keep their immutable snapshot.
	LocalInputSignatures input_signatures;
	int size[3] = { 0, 0, 0 };
	Vec3 uvw_offset;
	Vec3 uvw_basis_x;
	Vec3 uvw_basis_y;
	Vec3 uvw_basis_z;
	std::vector<float> albedo;
	std::vector<float> emission;
	std::vector<uint8_t> occupied;
	std::vector<uint8_t> emission_sides;
};

// A fragment-evaluated surface in capture-grid coordinates. The transform maps grid units
// back to the render instance's local coordinates; it is shared with the captured material.
struct RasterGeometryCapture {
	CoarseGeometryField geometry;
	SurfaceMaterialField material;
	PrimitiveTransform transform;
	uint64_t signature = 0;
	uint64_t material_signature = 0;
	LocalInputSignatures material_signatures;
};

uint32_t coarse_normal_mask(const Vec3 &p_normal);

// Remove GPU-covered cells that do not intersect any authored triangle. Coordinates are
// transformed to the capture's unit-cell lattice; zero GPU cells remain zero (alpha holes).
uint64_t filter_raster_geometry_coverage(const std::vector<MeshTriangle> &p_triangles,
		const PrimitiveTransform &p_mesh_to_grid, const int *p_size, std::vector<uint32_t> &r_surface,
		SubcellCoverage *r_subcells = nullptr);

bool bake_raster_geometry_capture(const std::vector<uint32_t> &p_surface, const int *p_size,
		const MaterialCapture &p_material, const PrimitiveTransform &p_transform, RasterGeometryCapture &r_capture,
		const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1, const SubcellCoverage *p_subcells = nullptr);

uint64_t material_field_input_signature(const std::vector<MeshTriangle> &p_triangles, const MaterialCapture *p_material);

struct TriangleMesh {
	std::vector<MeshTriangle> triangles;
	std::vector<int> shell; // per triangle: welded shell root
	std::vector<uint8_t> shell_closed; // per triangle: 1 when its shell is watertight
	std::vector<int8_t> shell_outward_sign; // corrects either consistent winding to outward
	int closed_shell_count = 0;
	int open_shell_count = 0;
	bool has_closed_shell = false;
};

TriangleMesh build_triangle_mesh(std::vector<MeshTriangle> p_triangles);

enum MeshSdfBakeError {
	MESH_SDF_BAKE_OK,
	MESH_SDF_BAKE_EMPTY,
	MESH_SDF_BAKE_DEGENERATE_BOUNDS,
	MESH_SDF_BAKE_TOO_LARGE,
	MESH_SDF_BAKE_NO_SURFACE,
	MESH_SDF_BAKE_CANCELLED,
};

struct MeshSdfBakeResult {
	CoarseGeometryField field;
	MeshSdfBakeError error = MESH_SDF_BAKE_OK;
	double voxelize_ms = 0.0;
	double flood_fill_ms = 0.0;
	double distance_ms = 0.0;
	uint64_t sample_count = 0;
};

// Conservative triangle coverage only; closed interiors are not filled and no distances are built.
MeshSdfBakeResult bake_mesh_sdf(const TriangleMesh &p_mesh, int p_resolution = 128,
		const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);
SurfaceMaterialField bake_mesh_instance_field(const TriangleMesh &p_mesh, const CoarseGeometryField &p_geometry,
		const MaterialCapture *p_material = nullptr, const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1,
		const PrimitiveTransform &p_material_transform = PrimitiveTransform());

struct LocalSurface {
	Vec3 reflectance;
	Vec3 emission;
	Vec3 normal;
};

struct LocalRecord {
	std::array<std::array<float, 16>, 3> reflection{}; // RGB row-major single-bounce operators.
	std::array<float, 4> reserved{};
	std::array<float, 12> emission{}; // RGB SH4, channel-major.

	Vec3 reflection_dc() const;
	bool has_contribution() const;
	bool operator==(const LocalRecord &p_other) const;
};

static_assert(sizeof(LocalRecord) == 256);

enum LocalChange : uint8_t {
	LOCAL_GEOMETRY = 1,
	LOCAL_REFLECTANCE = 2,
	LOCAL_EMISSION = 4,
	LOCAL_ALL = LOCAL_GEOMETRY | LOCAL_REFLECTANCE | LOCAL_EMISSION,
};

struct LocalField {
	std::vector<float> material; // Probe topology: reserved, local-contribution indicator, reserved, occupied.
	static constexpr uint32_t NO_RECORD = UINT32_MAX;
	std::vector<uint32_t> local_record_indices; // Probe -> stable slot, or NO_RECORD.
	std::vector<LocalRecord> local_records; // Stable slots: RGB transfer matrices and HDR EmissionSH.
	std::vector<uint32_t> local_record_owners; // Slot -> Probe, or NO_RECORD for a reusable slot.
	std::vector<uint32_t> free_local_records;
	std::vector<float> gpu_local_records; // Transient AoS upload: sixteen vec4 values per slot, including EmissionSH.
	const LocalRecord &get_local_record(size_t p_probe) const;
	uint32_t set_local_record(size_t p_probe, const LocalRecord &p_record);
	bool validate_local_records() const;
	std::vector<std::array<float, 4>> local_visibility; // Probe-local SH4; identity outside support.
	std::vector<uint8_t> dirty_mask; // one byte per probe; diagnostic RGBA is generated on demand
	std::vector<int> dirty_probes; // only probes rebuilt by the latest incremental bake
	std::vector<int> changed_occupancy; // incremental solid/air changes, prepared on the bake worker
	bool changed_occupancy_valid = false;
	bool visibility_changed = true;
	// Wall time of the CPU SDF build phases, including cooperative worker waits.
	double sdf_setup_ms = 0.0;
	double sdf_sample_ms = 0.0;
	double sdf_transfer_ms = 0.0;
	double sdf_merge_ms = 0.0;
	int solid_count = 0;
	int surface_count = 0;
	int classification_mismatches = 0;
	int trunk_count = 0;
	// Prototype's dirtyTrunkCount: how many trunks the incremental build had to recompute.
	int dirty_trunk_count = 0;
	int geometry_dirty_trunks = 0;
	int reflectance_dirty_trunks = 0;
	int emission_dirty_trunks = 0;
	int occupancy_probes_updated = 0;
	int geometry_records_built = 0;
	int reflectance_records_updated = 0;
	int emission_records_updated = 0;
	int surface_workspaces_built = 0;
	uint64_t surface_workspace_instances = 0;
	uint64_t surface_workspace_payload_bytes = 0; // Last build's simultaneous temporary payload.
	bool snapshot_reused = false;
	int snapshot_sync_probes = 0; // probes copied to catch the recycled bank up to the published field
};

// Prototype src/sdf-local.js cache: the trunk signatures and the per-probe samples of a built
// field. The next build synchronizes a recycled bank by changed Trunks, then recomputes the
// current dirty Trunks. `local` points at the field these entries describe and stays owned by
// the volume.
struct LocalCache {
	uint64_t grid_key = 0;
	std::vector<uint64_t> trunk_signatures;
	std::vector<LocalInputSignatures> input_signatures;
	std::vector<SurfaceSample> samples;
	std::vector<uint8_t> sampled;
	const LocalField *local = nullptr;
};

// Analytic box local data and sampled SDF local data.
LocalField build_local_data(const Grid &p_grid, const BoxQuery &p_query, const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1);
LocalField build_sdf_local_data(const Grid &p_grid, const std::vector<GeometryInstance> &p_primitives,
		const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1,
		const LocalCache *p_previous = nullptr, LocalCache *r_cache = nullptr, LocalField *p_reuse = nullptr,
		const TrunkIndex *p_index = nullptr);

// Comparing spatial-index digests with the published build allows unchanged inputs to return
// before allocating or clearing any field arrays.
uint64_t grid_signature(const Grid &p_grid);

} // namespace lrt
