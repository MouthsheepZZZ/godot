/**************************************************************************/
/*  lrt_transfer.cpp                                                      */
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

namespace lrt {

SH4 full_visibility() {
	return { float(1.0 / C0), 0.0f, 0.0f, 0.0f };
}

SH4 visibility_product(const SH4 &p_a, const SH4 &p_b) {
	double a[4], b[4], result[4];
	for (int i = 0; i < 4; i++) {
		a[i] = p_a[i];
		b[i] = p_b[i];
	}
	sh_triple_product(a, b, result);
	return { float(result[0]), float(result[1]), float(result[2]), float(result[3]) };
}

SH4 rotate_sh(const SH4 &p_sh, const SHRotation &p_rotation) {
	const Vec3 direction = p_rotation[0] * p_sh[1] + p_rotation[1] * p_sh[2] + p_rotation[2] * p_sh[3];
	return { p_sh[0], float(direction.x), float(direction.y), float(direction.z) };
}

TransferMatrix rotate_transfer(const TransferMatrix &p_matrix, const SHRotation &p_rotation) {
	// D = diag(1, R); both input and output must change basis.
	double rotation[4][4] = {};
	rotation[0][0] = 1.0;
	for (int column = 0; column < 3; column++) {
		for (int row = 0; row < 3; row++) {
			rotation[row + 1][column + 1] = p_rotation[column][row];
		}
	}
	double left[4][4] = {};
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			for (int k = 0; k < 4; k++) {
				left[row][column] += rotation[row][k] * p_matrix[k * 4 + column];
			}
		}
	}
	TransferMatrix result{};
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			double value = 0.0;
			for (int k = 0; k < 4; k++) {
				value += left[row][k] * rotation[column][k];
			}
			result[row * 4 + column] = float(value);
		}
	}
	return result;
}

SH4 apply_transfer(const TransferMatrix &p_matrix, const SH4 &p_incoming) {
	SH4 result{};
	for (int row = 0; row < 4; row++) {
		double value = 0.0;
		for (int column = 0; column < 4; column++) {
			value += double(p_matrix[row * 4 + column]) * p_incoming[column];
		}
		result[row] = float(value);
	}
	return result;
}

LocalRecord build_transfer_record(const TransferSample &p_sample) {
	LocalRecord record;
	record.reflection = p_sample.reflection;
	for (int channel = 0; channel < 3; channel++) {
		for (int row = 0; row < 4; row++) {
			record.emission[size_t(channel * 4 + row)] = p_sample.emission[channel][row];
		}
	}
	return record;
}

void TransferSample::compose(const TransferSample &p_other) {
	for (int channel = 0; channel < 3; channel++) {
		for (int i = 0; i < 16; i++) {
			reflection[channel][i] += p_other.reflection[channel][i];
		}
		for (int i = 0; i < 4; i++) {
			emission[channel][i] += p_other.emission[channel][i];
		}
	}
	visibility = visibility_product(visibility, p_other.visibility);
}

void TransferSample::bound_local_operator() {
	// Bound the projected multiplication operator to [0, I]. SH reconstruction
	// may ring; clipping the function itself would erase valid low-order contrast.
	const double visible = std::clamp(double(visibility[0]) * C0, 0.0, 1.0);
	visibility[0] = float(visible / C0);
	const double amplitude = C0 * length(Vec3(visibility[1], visibility[2], visibility[3]));
	const double limit = std::min(visible, 1.0 - visible);
	const double scale = amplitude > limit ? limit / amplitude : 1.0;
	for (int row = 1; row < 4; row++) {
		visibility[row] *= float(scale);
	}
	for (int channel = 0; channel < 3; channel++) {
		// Cap the isotropic material response to the intercepted solid angle.
		const double reflected = reflection[channel][0];
		const double energy_scale = reflected > 1.0 - visible ? (1.0 - visible) / reflected : 1.0;
		for (int row = 0; row < 4; row++) {
			for (int column = 0; column < 4; column++) {
				reflection[channel][row * 4 + column] *= float(energy_scale);
			}
		}
	}
}

TransferSample TransferSample::rotated(const SHRotation &p_rotation) const {
	TransferSample result;
	for (int channel = 0; channel < 3; channel++) {
		result.reflection[channel] = rotate_transfer(reflection[channel], p_rotation);
		result.emission[channel] = rotate_sh(emission[channel], p_rotation);
	}
	result.visibility = rotate_sh(visibility, p_rotation);
	return result;
}

bool TransferSample::is_identity() const {
	if (visibility != full_visibility()) {
		return false;
	}
	for (int channel = 0; channel < 3; channel++) {
		if (std::any_of(reflection[channel].begin(), reflection[channel].end(), [](float p_value) { return p_value != 0.0f; }) ||
				std::any_of(emission[channel].begin(), emission[channel].end(), [](float p_value) { return p_value != 0.0f; })) {
			return false;
		}
	}
	return true;
}

void integrate_transfer_direction(TransferSample &r_sample, const Vec3 &p_direction, double p_weight,
		bool p_hit, const Vec3 &p_reflectance, const Vec3 &p_emission) {
	// Start from analytic full visibility, subtract blocked solid angle. An empty
	// neighborhood remains exactly identity, even for a finite angular sample set.
	if (!p_hit) {
		return;
	}
	double outgoing[4], incoming[4];
	basis(p_direction, outgoing);
	// Reference p.28 uses the inward normal of the local sphere, -SampleDir.
	// Use the Lambert SH kernel (PI, 2*PI/3), divided by PI below.
	// The reference does not publish GetSH2PIDivDFT's implementation.
	cosine_basis(-p_direction, incoming);
	for (int row = 0; row < 4; row++) {
		r_sample.visibility[row] -= float(p_weight * outgoing[row]);
		for (int channel = 0; channel < 3; channel++) {
			r_sample.emission[channel][row] += float(p_weight * outgoing[row] * p_emission[channel]);
			for (int column = 0; column < 4; column++) {
				r_sample.reflection[channel][row * 4 + column] +=
						float((p_weight / PI) * outgoing[row] * incoming[column] * p_reflectance[channel]);
			}
		}
	}
}

Vec3 transfer_sample_direction(int p_index, int p_count) {
	const double z = 1.0 - 2.0 * (double(p_index) + 0.5) / double(p_count);
	const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
	const double angle = double(p_index) * PI * (3.0 - std::sqrt(5.0));
	return Vec3(radius * std::cos(angle), radius * std::sin(angle), z);
}

namespace {

int brick_axis(int p_value) {
	const int quotient = p_value / TransferField::BRICK_SIZE;
	return quotient - (p_value % TransferField::BRICK_SIZE < 0 ? 1 : 0);
}

TransferField::BrickKey brick_key(int p_x, int p_y, int p_z) {
	return { brick_axis(p_x), brick_axis(p_y), brick_axis(p_z) };
}

int brick_offset(int p_x, int p_y, int p_z, const TransferField::BrickKey &p_key) {
	const int x = p_x - p_key[0] * TransferField::BRICK_SIZE;
	const int y = p_y - p_key[1] * TransferField::BRICK_SIZE;
	const int z = p_z - p_key[2] * TransferField::BRICK_SIZE;
	return x + TransferField::BRICK_SIZE * (z + TransferField::BRICK_SIZE * y);
}

template <typename F>
void interpolate_corners(const Vec3 &p_origin, double p_spacing, const Vec3 &p_point, F p_accumulate) {
	const Vec3 coordinate = (p_point - p_origin) / p_spacing;
	int first[3];
	double fraction[3];
	for (int axis = 0; axis < 3; axis++) {
		first[axis] = int(std::floor(coordinate[axis]));
		fraction[axis] = coordinate[axis] - first[axis];
	}
	for (int corner = 0; corner < 8; corner++) {
		const int x = corner & 1;
		const int y = (corner >> 1) & 1;
		const int z = (corner >> 2) & 1;
		const double weight = (x ? fraction[0] : 1.0 - fraction[0]) *
				(y ? fraction[1] : 1.0 - fraction[1]) * (z ? fraction[2] : 1.0 - fraction[2]);
		if (weight != 0.0) {
			p_accumulate(first[0] + x, first[1] + y, first[2] + z, weight);
		}
	}
}

} // namespace

const TransferSample &TransferField::get(int p_x, int p_y, int p_z) const {
	static const TransferSample identity;
	const BrickKey key = brick_key(p_x, p_y, p_z);
	const auto found = bricks.find(key);
	return found == bricks.end() ? identity : found->second[brick_offset(p_x, p_y, p_z, key)];
}

void TransferField::set(int p_x, int p_y, int p_z, const TransferSample &p_sample) {
	const BrickKey key = brick_key(p_x, p_y, p_z);
	auto found = bricks.find(key);
	if (found == bricks.end()) {
		if (p_sample.is_identity()) {
			return;
		}
		found = bricks.try_emplace(key).first;
	}
	found->second[brick_offset(p_x, p_y, p_z, key)] = p_sample;
}

TransferSample TransferField::sample(const Vec3 &p_point) const {
	TransferSample result;
	result.visibility.fill(0.0f);
	interpolate_corners(origin, spacing, p_point, [&](int p_x, int p_y, int p_z, double weight) {
		const TransferSample &value = get(p_x, p_y, p_z);
		for (int i = 0; i < 4; i++) {
			result.visibility[i] += float(weight * value.visibility[i]);
		}
		for (int channel = 0; channel < 3; channel++) {
			for (int i = 0; i < 16; i++) {
				result.reflection[channel][i] += float(weight * value.reflection[channel][i]);
			}
			for (int i = 0; i < 4; i++) {
				result.emission[channel][i] += float(weight * value.emission[channel][i]);
			}
		}
	});
	return result;
}

uint64_t TransferField::sample_count() const {
	return uint64_t(bricks.size()) * BRICK_SAMPLES;
}

TransferVisibilityField::TransferVisibilityField(const TransferField &p_field) :
		origin(p_field.origin), spacing(p_field.spacing) {
	for (const auto &entry : p_field.bricks) {
		Brick &brick = bricks.try_emplace(entry.first).first->second;
		for (int i = 0; i < TransferField::BRICK_SAMPLES; i++) {
			brick[i] = entry.second[i].visibility;
		}
	}
}

SH4 TransferVisibilityField::sample(const Vec3 &p_point) const {
	SH4 result{};
	const SH4 identity = full_visibility();
	interpolate_corners(origin, spacing, p_point, [&](int p_x, int p_y, int p_z, double weight) {
		const TransferField::BrickKey key = brick_key(p_x, p_y, p_z);
		const auto found = bricks.find(key);
		const SH4 &value = found == bricks.end() ? identity : found->second[brick_offset(p_x, p_y, p_z, key)];
		for (int i = 0; i < 4; i++) {
			result[i] += float(weight * value[i]);
		}
	});
	return result;
}

TransferMaterialField::TransferMaterialField(const TransferField &p_field) :
		origin(p_field.origin), spacing(p_field.spacing) {
	auto mix = [](uint64_t &r_hash, const void *p_data, size_t p_bytes) {
		const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
		for (size_t i = 0; i < p_bytes; i++) {
			r_hash = (r_hash ^ bytes[i]) * 1099511628211ull;
		}
	};
	for (uint64_t *signature : { &reflection_signature, &emission_signature }) {
		mix(*signature, &origin, sizeof(origin));
		mix(*signature, &spacing, sizeof(spacing));
	}
	for (const auto &entry : p_field.bricks) {
		Brick &brick = bricks.try_emplace(entry.first).first->second;
		for (int i = 0; i < TransferField::BRICK_SAMPLES; i++) {
			brick[i].reflection = entry.second[i].reflection;
			brick[i].emission = entry.second[i].emission;
			auto mix_component = [&](uint64_t &r_signature, const auto &p_channels) {
				const bool nonzero = std::any_of(p_channels.begin(), p_channels.end(), [](const auto &p_coefficients) {
					return std::any_of(p_coefficients.begin(), p_coefficients.end(), [](float p_value) { return p_value != 0.0f; });
				});
				if (nonzero) {
					// Zero payloads do not change identity when another channel grows a brick.
					mix(r_signature, entry.first.data(), sizeof(entry.first));
					mix(r_signature, &i, sizeof(i));
					mix(r_signature, p_channels.data(), sizeof(p_channels));
				}
			};
			mix_component(reflection_signature, brick[i].reflection);
			mix_component(emission_signature, brick[i].emission);
		}
	}
}

TransferMaterialSample TransferMaterialField::sample(const Vec3 &p_point) const {
	TransferMaterialSample result;
	interpolate_corners(origin, spacing, p_point, [&](int p_x, int p_y, int p_z, double weight) {
		const TransferField::BrickKey key = brick_key(p_x, p_y, p_z);
		const auto found = bricks.find(key);
		if (found == bricks.end()) {
			return;
		}
		const TransferMaterialSample &value = found->second[brick_offset(p_x, p_y, p_z, key)];
		for (int channel = 0; channel < 3; channel++) {
			for (int i = 0; i < 16; i++) {
				result.reflection[channel][i] += float(weight * value.reflection[channel][i]);
			}
			for (int i = 0; i < 4; i++) {
				result.emission[channel][i] += float(weight * value.emission[channel][i]);
			}
		}
	});
	return result;
}

TransferSample TransferInstance::sample(const Vec3 &p_volume_point) const {
	const Vec3 point = volume_to_asset.xform(p_volume_point);
	const TransferMaterialSample appearance = material->sample(point);
	TransferSample result;
	result.reflection = appearance.reflection;
	result.emission = appearance.emission;
	result.visibility = visibility->sample(point);
	return result.rotated(asset_to_volume);
}

bool make_precomputed_instance(const TransferInstance &p_asset, const PrimitiveTransform &p_transform,
		double p_support_radius, double p_occupancy_band_width, const LocalInputSignatures &p_signatures,
		GeometryInstance &r_instance, uint32_t p_layer_mask) {
	if (!p_asset.visibility || !p_asset.material || !p_asset.coverage || !p_asset.coverage->triangles ||
			!p_asset.coverage->geometry || p_asset.coverage->geometry->empty() ||
			!std::isfinite(p_support_radius) || p_support_radius <= 0.0 || !std::isfinite(p_occupancy_band_width) || p_occupancy_band_width < 0.0) {
		return false;
	}
	const TransferVisibilityField &visibility = *p_asset.visibility;
	const TransferMaterialField &material = *p_asset.material;
	if (!std::isfinite(visibility.spacing) || visibility.spacing <= 0.0 || visibility.spacing != material.spacing) {
		return false;
	}
	for (int axis = 0; axis < 3; axis++) {
		if (!std::isfinite(visibility.origin[axis]) || visibility.origin[axis] != material.origin[axis] ||
				!std::isfinite(p_transform.origin[axis]) || !std::isfinite(p_transform.basis_x[axis]) ||
				!std::isfinite(p_transform.basis_y[axis]) || !std::isfinite(p_transform.basis_z[axis])) {
			return false;
		}
	}
	GeometryInstance instance;
	instance.origin = p_transform.origin;
	instance.mesh_to_volume = p_transform;
	instance.basis_x = p_transform.basis_x;
	instance.basis_y = p_transform.basis_y;
	instance.basis_z = p_transform.basis_z;
	instance.cofactor_x = cross(instance.basis_y, instance.basis_z);
	instance.cofactor_y = cross(instance.basis_z, instance.basis_x);
	instance.cofactor_z = cross(instance.basis_x, instance.basis_y);
	instance.determinant = dot(instance.basis_x, instance.cofactor_x);
	if (!std::isfinite(instance.determinant) || instance.determinant == 0.0) {
		return false;
	}
	auto transfer = std::make_shared<TransferInstance>(p_asset);
	PrimitiveTransform &inverse = transfer->volume_to_asset;
	inverse.basis_x = Vec3(instance.cofactor_x.x, instance.cofactor_y.x, instance.cofactor_z.x) / instance.determinant;
	inverse.basis_y = Vec3(instance.cofactor_x.y, instance.cofactor_y.y, instance.cofactor_z.y) / instance.determinant;
	inverse.basis_z = Vec3(instance.cofactor_x.z, instance.cofactor_y.z, instance.cofactor_z.z) / instance.determinant;
	inverse.origin = -(inverse.basis_x * instance.origin.x + inverse.basis_y * instance.origin.y + inverse.basis_z * instance.origin.z);
	// Match the engine's column-wise orthonormalization. Spatial resampling retains
	// the exact affine inverse; angular distortion under scale/shear is approximate.
	const Vec3 x = normalized(instance.basis_x);
	const Vec3 y = normalized(instance.basis_y - x * dot(x, instance.basis_y));
	transfer->asset_to_volume = { x, y, cross(x, y) * (instance.determinant < 0.0 ? -1.0 : 1.0) };
	instance.precomputed = std::move(transfer);
	instance.support_radius = p_support_radius;
	instance.occupancy_band_width = p_occupancy_band_width;
	instance.asset_signature = p_signatures.geometry;
	instance.material_signature = primitive_signature(p_signatures.reflectance, p_signatures.emission, PrimitiveTransform());
	instance.signature = primitive_signature(instance.asset_signature, instance.material_signature, p_transform);
	instance.input_signatures = p_signatures;
	instance.input_signatures.geometry = primitive_signature(p_signatures.geometry, 0, p_transform);
	instance.layer_mask = p_layer_mask;
	const Vec3 minimum = p_asset.coverage->geometry->get_minimum();
	const Vec3 maximum = p_asset.coverage->geometry->get_maximum();
	for (int corner = 0; corner < 8; corner++) {
		const Vec3 point = p_transform.xform(Vec3((corner & 1) ? maximum.x : minimum.x,
				(corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z));
		for (int axis = 0; axis < 3; axis++) {
			instance.bounds_min[axis] = corner == 0 ? point[axis] : std::min(instance.bounds_min[axis], point[axis]);
			instance.bounds_max[axis] = corner == 0 ? point[axis] : std::max(instance.bounds_max[axis], point[axis]);
		}
	}
	r_instance = std::move(instance);
	return true;
}

Vec3 GeometryInstance::transfer_support_extent(double p_spacing) const {
	// Trilinear filtering extends a lattice node's influence by one cell per axis.
	const double radius = precomputed ? std::max(support_radius + precomputed->visibility->spacing, occupancy_band_width * 0.5) : support_radius;
	const double half = precomputed ? 0.0 : p_spacing * 0.5;
	const Vec3 extent = mesh_to_volume.xform_extent(Vec3(radius, radius, radius)) + Vec3(half, half, half);
	// Connection footprints reach one neighbor even when the LTM support is smaller.
	return Vec3(std::max(extent.x, p_spacing), std::max(extent.y, p_spacing), std::max(extent.z, p_spacing));
}

bool GeometryInstance::occupies_probe(const Vec3 &p_point) const {
	if (precomputed) {
		return precomputed->coverage->occupied(precomputed->volume_to_asset.xform(p_point), occupancy_band_width * 0.5);
	}
	const SurfaceSample surface = sample(p_point);
	return surface.valid;
}

uint32_t TrunkSurfaceWorkspace::blocked_connections(const Vec3 &p_point) const {
	uint32_t blocked = 0;
	const Direction *dirs = directions();
	for (int i = 0; i < DIRECTION_COUNT; i++) {
		const Vec3 offset = Vec3(dirs[i].offset[0], dirs[i].offset[1], dirs[i].offset[2]) * spacing;
		const Vec3 midpoint = p_point + offset * 0.5;
		const Vec3 half_extent(std::abs(offset.x) * 0.5, std::abs(offset.y) * 0.5, std::abs(offset.z) * 0.5);
		for (const Entry &entry : instances) {
			const GeometryInstance &instance = *entry.instance;
			bool covered;
			if (instance.precomputed) {
				const PrimitiveTransform &to_asset = instance.precomputed->volume_to_asset;
				covered = instance.precomputed->coverage->overlaps_box(to_asset.xform(midpoint), to_asset.xform_extent(half_extent));
			} else {
				// A fixed connection AABB also conservatively closes diagonal corner cuts.
				covered = instance.sample(midpoint, half_extent).valid;
			}
			if (covered) {
				blocked |= 1u << i;
				break;
			}
		}
	}
	return blocked;
}

TransferSample TrunkSurfaceWorkspace::sample_transfer(int p_probe, const Vec3 &p_point, bool &r_occupied) const {
	TransferSample result;
	r_occupied = false;
	for (const Entry &entry : instances) {
		const bool occupied = entry.occupancy != nullptr ?
				entry.occupancy->find_cell(p_probe) >= 0 : entry.instance->occupies_probe(p_point);
		if (occupied) {
			r_occupied = true;
			return result;
		}
	}
	TransferSample voxel;
	bool has_precomputed = false;
	uint32_t covered_directions = 0;
	const Direction *directions = lrt::directions();
	for (const Entry &entry : instances) {
		const GeometryInstance &instance = *entry.instance;
		if (instance.precomputed) {
			has_precomputed = true;
			result.compose(instance.precomputed->sample(p_point));
			continue;
		}
		// Support is measured in authored mesh space, not the normalized capture
		// lattice used by GeometryInstance's cached query transform.
		const PrimitiveTransform &transform = instance.mesh_to_volume;
		const Vec3 cofactor[3] = { cross(transform.basis_y, transform.basis_z),
			cross(transform.basis_z, transform.basis_x), cross(transform.basis_x, transform.basis_y) };
		const double determinant = dot(transform.basis_x, cofactor[0]);
		if (determinant == 0.0) {
			continue;
		}
		for (int i = 0; i < DIRECTION_COUNT; i++) {
			const uint32_t bit = uint32_t(1) << i;
			if (covered_directions & bit) {
				continue;
			}
			const Vec3 offset = Vec3(directions[i].offset[0], directions[i].offset[1], directions[i].offset[2]) * spacing;
			const Vec3 local_offset(dot(cofactor[0], offset), dot(cofactor[1], offset), dot(cofactor[2], offset));
			if (length_squared(local_offset) > instance.support_radius * instance.support_radius * determinant * determinant) {
				continue;
			}
			// Use the same connection footprint as blocked_connections(). A full
			// neighbor cell would count nearby walls even when this link is open.
			const Vec3 point = p_point + offset * 0.5;
			const Vec3 half_extent(std::abs(offset.x) * 0.5, std::abs(offset.y) * 0.5, std::abs(offset.z) * 0.5);
			SurfaceSample surface = instance.sample(point, half_extent);
			if (!surface.valid) {
				continue;
			}
			surface = instance.sample_cell(surface.cell, point, &p_point);
			integrate_transfer_direction(voxel, directions[i].direction, WEIGHT, true, surface.color, surface.emission);
			covered_directions |= bit;
		}
	}
	// Keep the jointly integrated Voxel visibility and reflection complementary.
	if (!has_precomputed) {
		return voxel;
	}
	if (covered_directions != 0) {
		result.compose(voxel);
	}
	result.bound_local_operator();
	return result;
}

} // namespace lrt
