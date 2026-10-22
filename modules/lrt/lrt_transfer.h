/**************************************************************************/
/*  lrt_transfer.h                                                        */
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

#include "lrt_core.h"

namespace lrt {

using SH4 = std::array<float, 4>;
using TransferMatrix = std::array<float, 16>;
using SHRotation = std::array<Vec3, 3>; // Columns, including an optional orthogonal reflection.

SH4 full_visibility();
SH4 visibility_product(const SH4 &p_a, const SH4 &p_b);
SH4 rotate_sh(const SH4 &p_sh, const SHRotation &p_rotation);
TransferMatrix rotate_transfer(const TransferMatrix &p_matrix, const SHRotation &p_rotation);
SH4 apply_transfer(const TransferMatrix &p_matrix, const SH4 &p_incoming);

// Column-vector SH convention. Assets and runtime records contain single-bounce matrices.
// Visibility composes by projected function product, not addition.
struct TransferSample {
	std::array<TransferMatrix, 3> reflection{};
	SH4 visibility = full_visibility();
	std::array<SH4, 3> emission{};

	void compose(const TransferSample &p_other);
	// Bound overlapping Precomputed composition only; never rescale pure Voxel fields.
	void bound_local_operator();
	TransferSample rotated(const SHRotation &p_rotation) const;
	bool is_identity() const;
};
static_assert(sizeof(TransferSample) == 256);

// Pack composed reflection and emission without changing their energy.
LocalRecord build_transfer_record(const TransferSample &p_sample);

// One angular quadrature sample using the local sphere's inward normal.
// Misses retain visibility; material emission has already applied its side policy.
void integrate_transfer_direction(TransferSample &r_sample, const Vec3 &p_direction, double p_weight,
		bool p_hit, const Vec3 &p_reflectance = Vec3(), const Vec3 &p_emission = Vec3());

// Deterministic equal-area sphere sequence; count is part of the bake identity.
Vec3 transfer_sample_direction(int p_index, int p_count);

// Surface-local sparse lattice. Missing nodes are exactly zero reflection/emission
// and full visibility. Occupancy is deliberately not inferred from these coefficients.
class TransferField {
public:
	static constexpr int BRICK_SIZE = 8;
	static constexpr int BRICK_SAMPLES = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
	using BrickKey = std::array<int, 3>;
	using Brick = std::array<TransferSample, BRICK_SAMPLES>;

	Vec3 origin;
	double spacing = 0.0;
	double support_radius = 0.0;
	int angular_samples = 0;
	std::map<BrickKey, Brick> bricks;

	const TransferSample &get(int p_x, int p_y, int p_z) const;
	void set(int p_x, int p_y, int p_z, const TransferSample &p_sample);
	TransferSample sample(const Vec3 &p_point) const;
	uint64_t sample_count() const;
};

// Runtime asset channels are independent allocations: many material variants can
// reference one visibility field without duplicating its coefficients.
struct TransferVisibilityField {
	using Brick = std::array<SH4, TransferField::BRICK_SAMPLES>;
	Vec3 origin;
	double spacing = 0.0;
	std::map<TransferField::BrickKey, Brick> bricks;

	explicit TransferVisibilityField(const TransferField &p_field);
	SH4 sample(const Vec3 &p_point) const;
};

struct TransferMaterialSample {
	std::array<TransferMatrix, 3> reflection{};
	std::array<SH4, 3> emission{};
};
static_assert(sizeof(TransferMaterialSample) == 240);

struct TransferMaterialField {
	uint64_t reflection_signature = 1469598103934665603ull;
	uint64_t emission_signature = 1469598103934665603ull;
	using Brick = std::array<TransferMaterialSample, TransferField::BRICK_SAMPLES>;
	Vec3 origin;
	double spacing = 0.0;
	std::map<TransferField::BrickKey, Brick> bricks;

	explicit TransferMaterialField(const TransferField &p_field);
	TransferMaterialSample sample(const Vec3 &p_point) const;
};

struct TransferCoverage;

struct TransferInstance {
	std::shared_ptr<const TransferCoverage> coverage;
	std::shared_ptr<const TransferVisibilityField> visibility;
	std::shared_ptr<const TransferMaterialField> material;
	PrimitiveTransform volume_to_asset;
	SHRotation asset_to_volume = { Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1) };

	TransferSample sample(const Vec3 &p_volume_point) const;
};

// Geometry is shared independently of material. A callback evaluates effective
// coverage and shading at a barycentric hit; false means an alpha/discard hole.
struct TransferSurface {
	Vec3 normal;
	Vec3 reflectance;
	Vec3 emission;
};
using TransferSurfaceQuery = std::function<bool(int, const Vec3 &, const Vec3 &, TransferSurface &)>;

class TransferGeometry {
	struct Node {
		Vec3 minimum;
		Vec3 maximum;
		int first = 0;
		int count = 0;
		int left = -1;
		int right = -1;
	};
	std::shared_ptr<const std::vector<MeshTriangle>> triangles;
	std::vector<int> order;
	std::vector<Node> nodes;
	int build_node(int p_first, int p_count);

public:
	explicit TransferGeometry(std::shared_ptr<const std::vector<MeshTriangle>> p_triangles);
	bool empty() const { return nodes.empty(); }
	uint64_t payload_bytes() const; // Excludes the independently shared triangle array.
	Vec3 get_minimum() const;
	Vec3 get_maximum() const;
	bool trace(const Vec3 &p_origin, const Vec3 &p_direction, double p_radius,
			const TransferSurfaceQuery &p_surface, TransferSurface &r_surface) const;
	bool within_surface_band(const Vec3 &p_point, double p_half_width, const TransferSurfaceQuery &p_surface) const;
	bool overlaps_box(const Vec3 &p_point, const Vec3 &p_half_extent, const TransferSurfaceQuery &p_surface) const;
};

struct TransferCoverageAtlas {
	int width = 0;
	int height = 0;
	int tile_size = 0;
	std::vector<uint8_t> covered;
};

// Material-independent effective triangle coverage, shared by all appearance variants.
// Retains its BVH so occupancy can be queried without rasterizing at runtime.
struct TransferCoverage {
	std::vector<TransferCoverageAtlas> surfaces;
	std::vector<std::pair<int, int>> triangle_mapping;
	std::shared_ptr<const std::vector<MeshTriangle>> triangles;
	std::shared_ptr<const TransferGeometry> geometry;

	bool query(int p_triangle, const Vec3 &p_point, const Vec3 &p_barycentric, TransferSurface &r_surface) const;
	bool occupied(const Vec3 &p_point, double p_half_width) const;
	bool overlaps_box(const Vec3 &p_point, const Vec3 &p_half_extent) const;
};

// Construct immutable precomputed inputs without voxel fields or runtime baking.
// A failed validation leaves r_instance untouched. Radius and band width are asset-local.
bool make_precomputed_instance(const TransferInstance &p_asset, const PrimitiveTransform &p_transform,
		double p_support_radius, double p_occupancy_band_width, const LocalInputSignatures &p_signatures,
		GeometryInstance &r_instance, uint32_t p_layer_mask = 1);

// Separate payloads let coverage-identical material variants reuse visibility.
// A failed decode leaves the destination unchanged; no legacy format is accepted.
std::vector<uint8_t> encode_transfer_field(const TransferField &p_field, uint64_t p_signature, bool p_visibility);
bool decode_transfer_field(const std::vector<uint8_t> &p_data, uint64_t p_signature, bool p_visibility, TransferField &r_field);

struct TransferBakeSettings {
	double support_radius = 1.0;
	double spacing = 0.25;
	int angular_samples = 256;
};

enum TransferBakeError {
	TRANSFER_BAKE_OK,
	TRANSFER_BAKE_INVALID_SETTINGS,
	TRANSFER_BAKE_EMPTY_GEOMETRY,
	TRANSFER_BAKE_TOO_LARGE,
	TRANSFER_BAKE_CANCELLED,
};

struct TransferBakeResult {
	TransferBakeError error = TRANSFER_BAKE_OK;
	TransferField field;
	uint64_t evaluated_samples = 0;
};

TransferBakeResult bake_transfer_field(const TransferGeometry &p_geometry, const TransferSurfaceQuery &p_surface,
		const TransferBakeSettings &p_settings, const std::atomic<bool> *p_cancel = nullptr, int p_threads = 1,
		std::atomic<uint64_t> *p_evaluated = nullptr);

} // namespace lrt
