/**************************************************************************/
/*  lrt_core.cpp                                                          */
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

#include "lrt_core.h"

#include "core/object/worker_thread_pool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace lrt {

void parallel_for(int p_count, int p_threads, const std::function<void(int)> &p_body) {
	if (p_count <= 0) {
		return;
	}
	const int threads = std::max(1, std::min(p_threads, p_count));
	if (threads == 1) {
		for (int i = 0; i < p_count; i++) {
			p_body(i);
		}
		return;
	}
	// Dynamic edits run several parallel passes per build. Reuse the engine workers instead
	// of creating and joining operating-system threads for every pass. Individual task waits
	// are collaborative, so nested bakes cannot exhaust the pool waiting on child passes.
	struct Pass {
		std::atomic<int> next{ 0 };
		int count;
		const std::function<void(int)> &body;
	} pass{ {}, p_count, p_body };
	auto run = [](void *p_userdata) {
		Pass &work = *static_cast<Pass *>(p_userdata);
		for (;;) {
			const int index = work.next.fetch_add(1, std::memory_order_relaxed);
			if (index >= work.count) {
				return;
			}
			work.body(index);
		}
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	std::vector<WorkerThreadPool::TaskID> tasks;
	tasks.reserve(size_t(threads - 1));
	for (int i = 1; i < threads; i++) {
		tasks.push_back(pool->add_native_task(run, &pass, false, "LRT parallel pass"));
	}
	run(&pass);
	for (WorkerThreadPool::TaskID task : tasks) {
		pool->wait_for_task_completion(task);
	}
}

namespace {

// JavaScript Math.round: ties go towards +Infinity (floor(x + 0.5)).
inline double js_round(double p_value) {
	return std::floor(p_value + 0.5);
}

inline double max3(double p_a, double p_b, double p_c) {
	return std::max(p_a, std::max(p_b, p_c));
}

struct DirectionTable {
	Direction items[DIRECTION_COUNT];
	double projection[DIRECTION_COUNT][4];
	double geometry[DIRECTION_COUNT][16];
	std::array<int, 6> faces;
	DirectionTable() {
		int slot = 0;
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					if (x == 0 && y == 0 && z == 0) {
						continue;
					}
					const double len = hypot3(x, y, z);
					items[slot].offset[0] = x;
					items[slot].offset[1] = y;
					items[slot].offset[2] = z;
					items[slot].direction = Vec3(x / len, y / len, z / len);
					positive_basis(items[slot].direction, projection[slot]);
					double incident[4];
					cosine_basis(-items[slot].direction, incident);
					for (int row = 0; row < 4; row++) {
						for (int column = 0; column < 4; column++) {
							geometry[slot][row * 4 + column] = (WEIGHT / PI) * projection[slot][row] * incident[column];
						}
					}
					slot++;
				}
			}
		}
		int face = 0;
		for (int j = 0; j < DIRECTION_COUNT; j++) {
			const int *offset = items[j].offset;
			if (offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] == 1) {
				faces[face++] = j;
			}
		}
	}
};

const DirectionTable &direction_table() {
	static const DirectionTable table;
	return table;
}

} // namespace

const Direction *directions() {
	return direction_table().items;
}

const std::array<int, 6> &face_directions() {
	return direction_table().faces;
}

void basis(const Vec3 &p_direction, double r_out[4]) {
	r_out[0] = C0;
	r_out[1] = C1 * p_direction.x;
	r_out[2] = C1 * p_direction.y;
	r_out[3] = C1 * p_direction.z;
}

void positive_basis(const Vec3 &p_direction, double r_out[4]) {
	r_out[0] = C0;
	r_out[1] = C1 * p_direction.x / 3.0;
	r_out[2] = C1 * p_direction.y / 3.0;
	r_out[3] = C1 * p_direction.z / 3.0;
}

void cosine_basis(const Vec3 &p_normal, double r_out[4]) {
	r_out[0] = PI * C0;
	r_out[1] = 2.0 * PI / 3.0 * C1 * p_normal.x;
	r_out[2] = 2.0 * PI / 3.0 * C1 * p_normal.y;
	r_out[3] = 2.0 * PI / 3.0 * C1 * p_normal.z;
}

Vec3 LocalRecord::get_reflectance() const {
	return Vec3(reflectance & 0xffu, (reflectance >> 8) & 0xffu, (reflectance >> 16) & 0xffu) / 255.0;
}

bool LocalRecord::operator==(const LocalRecord &p_other) const {
	return geometry == p_other.geometry && reflectance == p_other.reflectance &&
			transport_weight == p_other.transport_weight && emission == p_other.emission;
}

bool LocalRecord::has_contribution() const {
	return (reflectance != 0 && geometry[0] != 0.0f) ||
			std::any_of(emission.begin(), emission.end(), [](float p_value) { return p_value != 0.0f; });
}

const LocalRecord &LocalField::get_local_record(size_t p_probe) const {
	static const LocalRecord empty;
	const uint32_t slot = local_record_indices[p_probe];
	return slot == NO_RECORD ? empty : local_records[slot];
}

uint32_t LocalField::set_local_record(size_t p_probe, const LocalRecord &p_record) {
	uint32_t &slot = local_record_indices[p_probe];
	if (!p_record.has_contribution()) {
		if (slot != NO_RECORD) {
			local_records[slot] = LocalRecord();
			local_record_owners[slot] = NO_RECORD;
			free_local_records.push_back(slot);
			slot = NO_RECORD;
		}
		return slot;
	}
	if (slot == NO_RECORD) {
		if (free_local_records.empty()) {
			slot = uint32_t(local_records.size());
			local_records.emplace_back();
			local_record_owners.push_back(uint32_t(p_probe));
		} else {
			slot = free_local_records.back();
			free_local_records.pop_back();
			local_record_owners[slot] = uint32_t(p_probe);
		}
	}
	local_records[slot] = p_record;
	return slot;
}

bool LocalField::validate_local_records() const {
	if (local_records.size() > local_record_indices.size() || local_record_owners.size() != local_records.size()) {
		return false;
	}
	std::vector<uint8_t> seen(local_records.size(), 0);
	for (size_t probe = 0; probe < local_record_indices.size(); probe++) {
		const uint32_t slot = local_record_indices[probe];
		if (slot == NO_RECORD) {
			continue;
		}
		if (slot >= local_records.size() || seen[slot] || local_record_owners[slot] != probe ||
				!local_records[slot].has_contribution()) {
			return false;
		}
		seen[slot] = 1;
	}
	for (uint32_t slot : free_local_records) {
		if (slot >= local_records.size() || seen[slot] || local_record_owners[slot] != NO_RECORD ||
				local_records[slot].has_contribution()) {
			return false;
		}
		seen[slot] = 1;
	}
	return std::all_of(seen.begin(), seen.end(), [](uint8_t p_seen) { return p_seen != 0; });
}

LocalRecord build_local_record(uint32_t p_open_mask, const std::array<LocalSurface, DIRECTION_COUNT> &p_surfaces) {
	return update_local_record(p_open_mask, p_surfaces, LocalRecord(), LOCAL_ALL);
}

LocalRecord update_local_record(uint32_t p_open_mask, const std::array<LocalSurface, DIRECTION_COUNT> &p_surfaces,
		const LocalRecord &p_previous, uint8_t p_changes) {
	LocalRecord record = p_previous;
	if (p_changes & LOCAL_GEOMETRY) {
		record.geometry.fill(0.0f);
	}
	if (p_changes & LOCAL_REFLECTANCE) {
		record.reflectance = 0;
	}
	if (p_changes & LOCAL_EMISSION) {
		record.emission.fill(0.0f);
	}
	Vec3 reflectance_sum;
	int blocked_count = 0;
	const DirectionTable &kernels = direction_table();
	for (int j = 0; j < DIRECTION_COUNT; j++) {
		if (p_open_mask & (1u << uint32_t(j))) {
			continue;
		}
		const LocalSurface &surface = p_surfaces[size_t(j)];
		const double *outgoing = kernels.projection[j];
		for (int row = 0; row < 4; row++) {
			if (p_changes & LOCAL_GEOMETRY) {
				for (int column = 0; column < 4; column++) {
					float &value = record.geometry[size_t(row * 4 + column)];
					value = float(double(value) + kernels.geometry[j][row * 4 + column]);
				}
			}
			for (int channel = 0; (p_changes & LOCAL_EMISSION) && channel < 3; channel++) {
				float &value = record.emission[size_t(channel * 4 + row)];
				value = float(double(value) + WEIGHT * outgoing[row] * surface.emission[channel]);
			}
		}
		if (p_changes & LOCAL_REFLECTANCE) {
			reflectance_sum = reflectance_sum + surface.reflectance;
		}
		blocked_count++;
	}
	if ((p_changes & LOCAL_REFLECTANCE) && blocked_count > 0) {
		for (int channel = 0; channel < 3; channel++) {
			const double average = std::clamp(reflectance_sum[channel] / blocked_count, 0.0, 1.0);
			record.reflectance |= uint32_t(std::lround(average * 255.0)) << (channel * 8);
		}
	}
	if (!(p_changes & (LOCAL_GEOMETRY | LOCAL_REFLECTANCE))) {
		return record;
	}
	const Vec3 rho = record.get_reflectance();
	const double rho_max = std::max({ rho.x, rho.y, rho.z });
	double response[4] = {};
	for (int j : kernels.faces) {
		if (!(p_open_mask & (1u << uint32_t(j)))) {
			continue;
		}
		for (int coefficient = 0; coefficient < 4; coefficient++) {
			response[coefficient] += (4.0 * PI / 6.0) * kernels.projection[j][coefficient];
		}
	}
	double combined[4];
	for (int row = 0; row < 4; row++) {
		double reflected = 0.0;
		for (int column = 0; column < 4; column++) {
			reflected += double(record.geometry[size_t(row * 4 + column)]) * response[column];
		}
		combined[row] = response[row] + rho_max * reflected;
	}
	const double peak = C0 * combined[0] + C1 * std::sqrt(combined[1] * combined[1] +
			combined[2] * combined[2] + combined[3] * combined[3]);
	record.transport_weight = float(1.0 / std::max(1.0, peak));
	return record;
}

void sh_triple_product(const double p_a[4], const double p_b[4], double r_out[4]) {
	double dc = 0.0;
	for (int i = 0; i < 4; i++) {
		dc += p_a[i] * p_b[i];
	}
	r_out[0] = C0 * dc;
	for (int i = 1; i < 4; i++) {
		r_out[i] = C0 * (p_a[0] * p_b[i] + p_b[0] * p_a[i]);
	}
}

Grid make_grid(double p_spacing) {
	Vec3 lo(-3, -0.5, -3);
	Vec3 hi(3, 3.5, 3);
	Grid grid;
	grid.min = lo;
	grid.spacing = p_spacing;
	for (int axis = 0; axis < 3; axis++) {
		grid.size[axis] = int(std::ceil((hi[axis] - lo[axis]) / p_spacing - 1e-9));
	}
	grid.width = grid.size[0] * grid.size[2];
	grid.height = grid.size[1];
	grid.count = grid.size[0] * grid.size[1] * grid.size[2];
	return grid;
}

Grid make_grid(double p_spacing, const Vec3 &p_bounds_min, const Vec3 &p_bounds_max) {
	// Prototype: start from the default region, then keep two air cells around geometry.
	Vec3 lo(-3, -0.5, -3);
	Vec3 hi(3, 3.5, 3);
	for (int axis = 0; axis < 3; axis++) {
		lo[axis] = std::min(lo[axis], std::floor(p_bounds_min[axis] / p_spacing) * p_spacing - 2.0 * p_spacing);
		hi[axis] = std::max(hi[axis], std::ceil(p_bounds_max[axis] / p_spacing) * p_spacing + 2.0 * p_spacing);
	}
	Grid grid;
	grid.min = lo;
	grid.spacing = p_spacing;
	for (int axis = 0; axis < 3; axis++) {
		grid.size[axis] = int(std::ceil((hi[axis] - lo[axis]) / p_spacing - 1e-9));
	}
	grid.width = grid.size[0] * grid.size[2];
	grid.height = grid.size[1];
	grid.count = grid.size[0] * grid.size[1] * grid.size[2];
	return grid;
}

static Grid make_grid_from_region(double p_spacing, const Vec3 &p_lo, const Vec3 &p_hi) {
	Grid grid;
	grid.min = p_lo;
	grid.spacing = p_spacing;
	for (int axis = 0; axis < 3; axis++) {
		grid.size[axis] = int(std::ceil((p_hi[axis] - p_lo[axis]) / p_spacing - 1e-9));
	}
	grid.width = grid.size[0] * grid.size[2];
	grid.height = grid.size[1];
	grid.count = grid.size[0] * grid.size[1] * grid.size[2];
	return grid;
}

Grid make_grid_sized(double p_spacing, const Vec3 &p_min, const Vec3 &p_size) {
	return make_grid_from_region(p_spacing, p_min, p_min + p_size);
}

Grid make_grid_sized(double p_spacing, const Vec3 &p_min, const Vec3 &p_size, const Vec3 &p_bounds_min, const Vec3 &p_bounds_max) {
	// Same rule as make_grid(spacing, bounds): two air cells around the geometry.
	Vec3 lo = p_min;
	Vec3 hi = p_min + p_size;
	for (int axis = 0; axis < 3; axis++) {
		lo[axis] = std::min(lo[axis], std::floor(p_bounds_min[axis] / p_spacing) * p_spacing - 2.0 * p_spacing);
		hi[axis] = std::max(hi[axis], std::ceil(p_bounds_max[axis] / p_spacing) * p_spacing + 2.0 * p_spacing);
	}
	return make_grid_from_region(p_spacing, lo, hi);
}

bool BoxQuery::contains(const Vec3 &p_point) const {
	for (const Box &box : boxes) {
		bool within = true;
		for (int axis = 0; axis < 3; axis++) {
			if (p_point[axis] < box.min[axis] - GEOMETRY_EPSILON || p_point[axis] > box.max[axis] + GEOMETRY_EPSILON) {
				within = false;
				break;
			}
		}
		if (within) {
			return true;
		}
	}
	return false;
}

CoarseGeometryField bake_box_sdf(const Vec3 &p_extent, int p_resolution, const std::atomic<bool> *p_cancel) {
	CoarseGeometryField field;
	field.cell = max3(p_extent.x, p_extent.y, p_extent.z) / double(p_resolution);
	const Vec3 half = p_extent * 0.5;
	field.min = -half - Vec3(field.cell, field.cell, field.cell);
	int64_t count = 1;
	for (int axis = 0; axis < 3; axis++) {
		field.size[axis] = int(std::ceil(p_extent[axis] / field.cell)) + 3;
		count *= field.size[axis];
		if (count > INT32_MAX) {
			return CoarseGeometryField();
		}
	}
	field.cells.assign(size_t(count), 0u);
	const Vec3 voxel_half(field.cell * 0.5, field.cell * 0.5, field.cell * 0.5);
	for (int z = 0; z < field.size[2]; z++) {
		if (p_cancel && p_cancel->load()) {
			return CoarseGeometryField();
		}
		for (int y = 0; y < field.size[1]; y++) {
			for (int x = 0; x < field.size[0]; x++) {
				const Vec3 center = field.min + Vec3(x, y, z) * field.cell;
				bool overlap = true;
				uint32_t mask = 1u << DIRECTION_COUNT;
				for (int axis = 0; axis < 3; axis++) {
					const double low = center[axis] - voxel_half[axis];
					const double high = center[axis] + voxel_half[axis];
					if (low > half[axis] || high < -half[axis]) {
						overlap = false;
						break;
					}
					for (int side : { -1, 1 }) {
						const double plane = side * half[axis];
						if (low <= plane && high >= plane) {
							Vec3 normal;
							normal[axis] = side;
							mask |= coarse_normal_mask(normal);
						}
					}
				}
				if (overlap) {
					field.cells[size_t(x + field.size[0] * (y + field.size[1] * z))] = mask;
					field.surface_voxels += (mask & ((1u << DIRECTION_COUNT) - 1u)) != 0;
				}
			}
		}
	}
	field.closed_shell_count = 1;
	return field;
}

SurfaceMaterialField bake_constant_instance_field(const Vec3 &p_albedo, const Vec3 &p_emission, uint8_t p_emission_sides) {
	SurfaceMaterialField field;
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = 1;
	}
	if (p_emission_sides != EMISSION_FRONT) {
		field.emission_sides.push_back(p_emission_sides);
	}
	field.albedo.resize(3);
	const bool has_emission = p_emission.x > 0.0 || p_emission.y > 0.0 || p_emission.z > 0.0;
	if (has_emission) {
		field.emission.resize(3);
	}
	for (int channel = 0; channel < 3; channel++) {
		field.albedo[channel] = uint8_t(js_round(std::clamp(p_albedo[channel], 0.0, 1.0) * 255.0));
		if (has_emission) {
			field.emission[channel] = float(std::max(0.0, p_emission[channel]));
		}
	}
	return field;
}

uint32_t coarse_normal_mask(const Vec3 &p_normal) {
	int selected = 0;
	double best = -std::numeric_limits<double>::infinity();
	for (int direction = 0; direction < DIRECTION_COUNT; direction++) {
		const double alignment = dot(p_normal, directions()[direction].direction);
		if (alignment > best) {
			best = alignment;
			selected = direction;
		}
	}
	return 1u << uint32_t(selected);
}

namespace {

uint8_t sample_material_cell(const CoarseGeometryField &p_geometry, const SurfaceMaterialField &p_material,
		int p_index, SurfaceSample &r_sample) {
	const int coordinate[3] = {
		p_index % p_geometry.size[0],
		(p_index / p_geometry.size[0]) % p_geometry.size[1],
		p_index / (p_geometry.size[0] * p_geometry.size[1]),
	};
	int material_coordinate[3];
	for (int axis = 0; axis < 3; axis++) {
		material_coordinate[axis] = std::min(p_material.color_size[axis] - 1, coordinate[axis] / p_material.cell_stride);
	}
	const size_t index = size_t(material_coordinate[0] + p_material.color_size[0] *
			(material_coordinate[1] + p_material.color_size[1] * material_coordinate[2]));
	for (int channel = 0; channel < 3; channel++) {
		r_sample.color[channel] = double(p_material.albedo[index * 3 + channel]) / 255.0;
		if (!p_material.emission.empty()) {
			r_sample.emission[channel] = p_material.emission[index * 3 + channel];
		}
	}
	return p_material.emission_sides.empty() ? uint8_t(EMISSION_FRONT) : p_material.emission_sides[index];
}

bool segment_box(const Vec3 &p_from, const Vec3 &p_delta, const Vec3 &p_low, const Vec3 &p_high, double &r_enter, double &r_exit) {
	r_enter = 0.0;
	r_exit = 1.0;
	for (int axis = 0; axis < 3; axis++) {
		if (p_delta[axis] == 0.0) {
			if (p_from[axis] < p_low[axis] || p_from[axis] > p_high[axis]) {
				return false;
			}
			continue;
		}
		double first = (p_low[axis] - p_from[axis]) / p_delta[axis];
		double last = (p_high[axis] - p_from[axis]) / p_delta[axis];
		if (first > last) {
			std::swap(first, last);
		}
		r_enter = std::max(r_enter, first);
		r_exit = std::min(r_exit, last);
		if (r_enter > r_exit) {
			return false;
		}
	}
	return true;
}

Vec3 instance_local_point(const GeometryInstance &p_instance, const Vec3 &p_point) {
	const Vec3 delta = p_point - p_instance.origin;
	return Vec3(dot(delta, p_instance.cofactor_x), dot(delta, p_instance.cofactor_y), dot(delta, p_instance.cofactor_z)) / p_instance.determinant;
}

Vec3 instance_normal(const GeometryInstance &p_instance, const Vec3 &p_normal) {
	return normalized((p_instance.cofactor_x * p_normal.x + p_instance.cofactor_y * p_normal.y +
			p_instance.cofactor_z * p_normal.z) / p_instance.determinant);
}

double rounded_down(double p_value) {
	return std::nextafter(p_value, -std::numeric_limits<double>::infinity());
}

double rounded_up(double p_value) {
	return std::nextafter(p_value, std::numeric_limits<double>::infinity());
}

// Outward rounding affects broad-phase bounds only, never the exact covered-cell tests.
void affine_bounds(const GeometryInstance &p_instance, const Vec3 &p_low, const Vec3 &p_high,
		bool p_inverse, Vec3 &r_low, Vec3 &r_high) {
	Vec3 low = p_low;
	Vec3 high = p_high;
	if (p_inverse) {
		for (int axis = 0; axis < 3; axis++) {
			low[axis] = rounded_down(low[axis] - p_instance.origin[axis]);
			high[axis] = rounded_up(high[axis] - p_instance.origin[axis]);
		}
	}
	const Vec3 rows[3] = {
		p_inverse ? p_instance.cofactor_x : Vec3(p_instance.basis_x.x, p_instance.basis_y.x, p_instance.basis_z.x),
		p_inverse ? p_instance.cofactor_y : Vec3(p_instance.basis_x.y, p_instance.basis_y.y, p_instance.basis_z.y),
		p_inverse ? p_instance.cofactor_z : Vec3(p_instance.basis_x.z, p_instance.basis_y.z, p_instance.basis_z.z),
	};
	for (int axis = 0; axis < 3; axis++) {
		double first = p_inverse ? 0.0 : p_instance.origin[axis];
		double last = first;
		for (int component = 0; component < 3; component++) {
			const double a = rows[axis][component] * low[component];
			const double b = rows[axis][component] * high[component];
			first = rounded_down(first + rounded_down(std::min(a, b)));
			last = rounded_up(last + rounded_up(std::max(a, b)));
		}
		if (p_inverse) {
			const double a = first / p_instance.determinant;
			const double b = last / p_instance.determinant;
			first = rounded_down(std::min(a, b));
			last = rounded_up(std::max(a, b));
		}
		r_low[axis] = first;
		r_high[axis] = last;
	}
}

void geometry_cell_bounds(const CoarseGeometryField &p_field, int p_index, bool p_segment, Vec3 &r_low, Vec3 &r_high) {
	const Vec3 coordinate(p_index % p_field.size[0], (p_index / p_field.size[0]) % p_field.size[1],
			p_index / (p_field.size[0] * p_field.size[1]));
	const Vec3 half(p_field.cell * 0.5, p_field.cell * 0.5, p_field.cell * 0.5);
	// Preserve the arithmetic of the point and segment reference queries at closed boundaries.
	r_low = p_segment ? p_field.min - half + coordinate * p_field.cell : p_field.min + coordinate * p_field.cell - half;
	r_high = r_low + half * 2.0;
}

SurfaceSample instance_cell_sample(const GeometryInstance &p_instance, int p_index,
		const Vec3 &p_from, const Vec3 &p_to, double p_fraction, bool p_segment) {
	SurfaceSample sample;
	sample.valid = true;
	sample.fraction = p_fraction;
	sample.position = p_segment ? p_from + (p_to - p_from) * p_fraction : p_from;
	sample.layer_mask = p_instance.layer_mask;
	const uint8_t emission_sides = sample_material_cell(*p_instance.geometry, *p_instance.instance, p_index, sample);
	bool emits_towards_origin = false;
	const uint32_t mask = p_instance.geometry->cells[size_t(p_index)];
	double facing = -std::numeric_limits<double>::infinity();
	for (int direction = 0; direction < DIRECTION_COUNT; direction++) {
		if (!(mask & (1u << uint32_t(direction)))) {
			continue;
		}
		const Vec3 normal = instance_normal(p_instance, directions()[direction].direction);
		const double alignment = p_segment ? dot(normal, p_from - p_to) : 0.0;
		emits_towards_origin |= ((emission_sides & EMISSION_FRONT) && alignment > 0.0) ||
				((emission_sides & EMISSION_BACK) && alignment < 0.0);
		if (alignment > facing) {
			facing = alignment;
			sample.normal = normal;
		}
	}
	if (p_segment && !emits_towards_origin) {
		sample.emission = Vec3();
	}
	return sample;
}

} // namespace

SurfaceSample sample_sdf_fields(const CoarseGeometryField &p_geometry, const SurfaceMaterialField &p_instance, const Vec3 &p_point) {
	SurfaceSample sample;
	int coordinate[3];
	for (int axis = 0; axis < 3; axis++) {
		const double value = std::floor((p_point[axis] - p_geometry.min[axis]) / p_geometry.cell + 0.5);
		if (value < -1 || value > p_geometry.size[axis]) {
			return sample;
		}
		coordinate[axis] = int(value);
	}
	const Vec3 half(p_geometry.cell * 0.5, p_geometry.cell * 0.5, p_geometry.cell * 0.5);
	for (int z = std::max(0, coordinate[2] - 1); z <= std::min(p_geometry.size[2] - 1, coordinate[2] + 1); z++) {
		for (int y = std::max(0, coordinate[1] - 1); y <= std::min(p_geometry.size[1] - 1, coordinate[1] + 1); y++) {
			for (int x = std::max(0, coordinate[0] - 1); x <= std::min(p_geometry.size[0] - 1, coordinate[0] + 1); x++) {
				const int index = x + p_geometry.size[0] * (y + p_geometry.size[1] * z);
				const uint32_t mask = p_geometry.cells[size_t(index)];
				if (mask == 0) {
					continue;
				}
				const Vec3 cell_min = p_geometry.min + Vec3(x, y, z) * p_geometry.cell - half;
				double enter;
				double exit;
				if (!segment_box(p_point, Vec3(), cell_min, cell_min + half * 2.0, enter, exit)) {
					continue;
				}
				sample.valid = true;
				sample.position = p_point;
				for (int direction = 0; direction < DIRECTION_COUNT; direction++) {
					if (mask & (1u << uint32_t(direction))) {
						sample.normal = directions()[direction].direction;
						break;
					}
				}
				sample_material_cell(p_geometry, p_instance, index, sample);
				return sample;
			}
		}
	}
	return sample;
}

Vec3 PrimitiveTransform::xform(const Vec3 &p_point) const {
	return origin + basis_x * p_point.x + basis_y * p_point.y + basis_z * p_point.z;
}

PrimitiveTransform PrimitiveTransform::operator*(const PrimitiveTransform &p_transform) const {
	PrimitiveTransform result;
	result.origin = xform(p_transform.origin);
	auto basis_xform = [this](const Vec3 &p_vector) {
		return basis_x * p_vector.x + basis_y * p_vector.y + basis_z * p_vector.z;
	};
	result.basis_x = basis_xform(p_transform.basis_x);
	result.basis_y = basis_xform(p_transform.basis_y);
	result.basis_z = basis_xform(p_transform.basis_z);
	return result;
}

bool PrimitiveTransform::is_identity() const {
	return origin.x == 0.0 && origin.y == 0.0 && origin.z == 0.0 &&
			basis_x.x == 1.0 && basis_x.y == 0.0 && basis_x.z == 0.0 &&
			basis_y.x == 0.0 && basis_y.y == 1.0 && basis_y.z == 0.0 &&
			basis_z.x == 0.0 && basis_z.y == 0.0 && basis_z.z == 1.0;
}

SurfaceSample GeometryInstance::sample(const Vec3 &p_point) const {
	if (determinant == 0.0) {
		return SurfaceSample();
	}
	SurfaceSample value = sample_sdf_fields(*geometry, *instance, instance_local_point(*this, p_point));
	value.position = p_point;
	value.layer_mask = layer_mask;
	if (value.valid && length_squared(value.normal) > 0.0) {
		value.normal = instance_normal(*this, value.normal);
	}
	return value;
}

bool GeometryInstance::trace_segment(const Vec3 &p_from, const Vec3 &p_to, SurfaceSample &r_sample) const {
	if (determinant == 0.0) {
		return false;
	}
	// Both directions traverse the same canonical segment with identical arithmetic.
	bool reversed = false;
	for (int axis = 0; axis < 3; axis++) {
		if (p_from[axis] != p_to[axis]) {
			reversed = p_from[axis] > p_to[axis];
			break;
		}
	}
	const Vec3 from = instance_local_point(*this, reversed ? p_to : p_from);
	const Vec3 to = instance_local_point(*this, reversed ? p_from : p_to);
	const Vec3 delta = to - from;
	const CoarseGeometryField &field = *geometry;
	const Vec3 half(field.cell * 0.5, field.cell * 0.5, field.cell * 0.5);
	const Vec3 low = field.min - half;
	const Vec3 high = low + Vec3(field.size[0], field.size[1], field.size[2]) * field.cell;
	double enter;
	double exit;
	if (!segment_box(from, delta, low, high, enter, exit)) {
		return false;
	}
	int cell[3];
	int step[3];
	double next[3];
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = std::clamp(int(std::floor((from[axis] + delta[axis] * enter - low[axis]) / field.cell)), 0, field.size[axis] - 1);
		step[axis] = delta[axis] > 0.0 ? 1 : -1;
		next[axis] = delta[axis] == 0.0 ? std::numeric_limits<double>::infinity() :
				(low[axis] + (cell[axis] + (step[axis] > 0 ? 1 : 0)) * field.cell - from[axis]) / delta[axis];
	}
	int selected = -1;
	double nearest = std::numeric_limits<double>::infinity();
	while (true) {
		// Supercover candidates include cells touched only on a face, edge or corner.
		// Exact closed-box intersections reject the surrounding non-intersected cells;
		// there is no epsilon, radius expansion or signed-distance threshold.
		for (int z = std::max(0, cell[2] - 1); z <= std::min(field.size[2] - 1, cell[2] + 1); z++) {
			for (int y = std::max(0, cell[1] - 1); y <= std::min(field.size[1] - 1, cell[1] + 1); y++) {
				for (int x = std::max(0, cell[0] - 1); x <= std::min(field.size[0] - 1, cell[0] + 1); x++) {
					const int index = x + field.size[0] * (y + field.size[1] * z);
					if (field.cells[size_t(index)] == 0) {
						continue;
					}
					const Vec3 cell_low = low + Vec3(x, y, z) * field.cell;
					double first;
					double last;
					if (!segment_box(from, delta, cell_low, cell_low + half * 2.0, first, last)) {
						continue;
					}
					const double fraction = reversed ? 1.0 - last : first;
					if (fraction < nearest || (fraction == nearest && index < selected)) {
						nearest = fraction;
						selected = index;
					}
				}
			}
		}
		const double crossing = std::min(next[0], std::min(next[1], next[2]));
		if (crossing > exit) {
			break;
		}
		bool outside = false;
		for (int axis = 0; axis < 3; axis++) {
			if (next[axis] != crossing) {
				continue;
			}
			cell[axis] += step[axis];
			outside = outside || cell[axis] < 0 || cell[axis] >= field.size[axis];
			next[axis] = (low[axis] + (cell[axis] + (step[axis] > 0 ? 1 : 0)) * field.cell - from[axis]) / delta[axis];
		}
		if (outside) {
			break;
		}
	}
	if (selected < 0) {
		return false;
	}
	r_sample = instance_cell_sample(*this, selected, p_from, p_to, nearest, true);
	return true;
}

GeometryInstance make_sdf_primitive(std::shared_ptr<const CoarseGeometryField> p_geometry, std::shared_ptr<const SurfaceMaterialField> p_instance,
		const PrimitiveTransform &p_transform, uint64_t p_signature, uint32_t p_layer_mask,
		uint64_t p_asset_signature, uint64_t p_material_signature) {
	GeometryInstance primitive;
	primitive.geometry = std::move(p_geometry);
	primitive.instance = std::move(p_instance);
	primitive.signature = p_signature;
	primitive.asset_signature = p_asset_signature;
	primitive.material_signature = p_material_signature;
	// Real asset identities are stable across cache reloads; anonymous in-memory fields use
	// their immutable allocation identity and are never serialized as production assets.
	const uint64_t geometry_identity = p_asset_signature != 0 ? p_asset_signature : uint64_t(reinterpret_cast<uintptr_t>(primitive.geometry.get()));
	primitive.input_signatures.geometry = primitive_signature(geometry_identity, 0, p_transform);
	primitive.input_signatures.reflectance = material_component_signature(*primitive.instance, false);
	primitive.input_signatures.emission = material_component_signature(*primitive.instance, true);
	primitive.layer_mask = p_layer_mask;
	primitive.origin = p_transform.origin;
	primitive.basis_x = p_transform.basis_x;
	primitive.basis_y = p_transform.basis_y;
	primitive.basis_z = p_transform.basis_z;
	primitive.cofactor_x = cross(primitive.basis_y, primitive.basis_z);
	primitive.cofactor_y = cross(primitive.basis_z, primitive.basis_x);
	primitive.cofactor_z = cross(primitive.basis_x, primitive.basis_y);
	primitive.determinant = dot(primitive.basis_x, primitive.cofactor_x);
	// PrimitiveGI bounds: the local field box transformed and re-boxed (Box3.applyMatrix4).
	const CoarseGeometryField &field = *primitive.geometry;
	const Vec3 half(field.cell * 0.5, field.cell * 0.5, field.cell * 0.5);
	const Vec3 local_low = field.min - half;
	const Vec3 local_high = local_low + Vec3(field.size[0], field.size[1], field.size[2]) * field.cell;
	for (int corner = 0; corner < 8; corner++) {
		const Vec3 local((corner & 1) ? local_high.x : local_low.x,
				(corner & 2) ? local_high.y : local_low.y,
				(corner & 4) ? local_high.z : local_low.z);
		const Vec3 world = p_transform.origin +
				p_transform.basis_x * local.x +
				p_transform.basis_y * local.y +
				p_transform.basis_z * local.z;
		for (int axis = 0; axis < 3; axis++) {
			if (corner == 0 || world[axis] < primitive.bounds_min[axis]) {
				primitive.bounds_min[axis] = world[axis];
			}
			if (corner == 0 || world[axis] > primitive.bounds_max[axis]) {
				primitive.bounds_max[axis] = world[axis];
			}
		}
	}
	return primitive;
}

namespace {

// FNV-1a over raw doubles: the signature only has to be stable within a run and change whenever
// a primitive's field or matrix changes.
void mix_bytes(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = reinterpret_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; i++) {
		r_hash ^= uint64_t(bytes[i]);
		r_hash *= 1099511628211ull;
	}
}

void mix_value(uint64_t &r_hash, double p_value) {
	mix_bytes(r_hash, &p_value, sizeof(double));
}

} // namespace

// Prototype gridKey (JSON of min/size/spacing): an incremental build is only valid on its own
// grid, so a spacing or box change falls back to the full bake.
uint64_t grid_signature(const Grid &p_grid) {
	uint64_t hash = 1469598103934665603ull;
	mix_value(hash, p_grid.min.x);
	mix_value(hash, p_grid.min.y);
	mix_value(hash, p_grid.min.z);
	mix_value(hash, double(p_grid.size[0]));
	mix_value(hash, double(p_grid.size[1]));
	mix_value(hash, double(p_grid.size[2]));
	mix_value(hash, p_grid.spacing);
	return hash;
}

// One digest per Trunk over the primitives that overlap its 26-neighbour support box. This is the
// whole input of the incremental test, so a caller can compare it with the previous build and
// learn that nothing has to be solved before paying for any field storage.
TrunkIndex::PrimitiveKey TrunkIndex::make_key(const GeometryInstance &p_primitive) {
	PrimitiveKey key{};
	key[0] = p_primitive.input_signatures.geometry != 0 ? p_primitive.input_signatures.geometry : p_primitive.signature;
	key[1] = p_primitive.layer_mask;
	for (int axis = 0; axis < 3; axis++) {
		const double low = p_primitive.bounds_min[axis];
		const double high = p_primitive.bounds_max[axis];
		memcpy(&key[size_t(2 + axis)], &low, sizeof(low));
		memcpy(&key[size_t(5 + axis)], &high, sizeof(high));
	}
	return key;
}

void TrunkIndex::update(const Grid &p_grid, const std::vector<GeometryInstance> &p_primitives) {
	update_stats = UpdateStats();
	const int trunk_size[3] = {
		(p_grid.size[0] + TRUNK - 1) / TRUNK,
		(p_grid.size[1] + TRUNK - 1) / TRUNK,
		(p_grid.size[2] + TRUNK - 1) / TRUNK,
	};
	const int trunk_count = trunk_size[0] * trunk_size[1] * trunk_size[2];
	const uint64_t next_grid_key = grid_signature(p_grid);
	const bool grid_changed = grid_key != next_grid_key || signatures.size() != size_t(trunk_count);
	if (grid_changed) {
		update_stats.removed = int(registrations.size());
		trunk_members.clear();
		registrations.clear();
		candidates.clear();
		trunk_members.resize(size_t(trunk_count));
		candidates.resize(size_t(trunk_count));
		signatures.resize(size_t(trunk_count));
		input_signatures.resize(size_t(trunk_count));
		grid_key = next_grid_key;
	}
	std::vector<PrimitiveKey> keys;
	std::vector<std::array<uint64_t, 3>> material_inputs;
	material_inputs.reserve(p_primitives.size());
	keys.reserve(p_primitives.size());
	for (const GeometryInstance &primitive : p_primitives) {
		keys.push_back(make_key(primitive));
		material_inputs.push_back({ primitive.signature, primitive.input_signatures.reflectance, primitive.input_signatures.emission });
	}
	if (!grid_changed && keys == previous_inputs && material_inputs == previous_material_inputs) {
		return;
	}
	for (auto &entry : registrations) {
		entry.second.inputs.clear();
	}
	for (int input = 0; input < int(p_primitives.size()); input++) {
		auto inserted = registrations.try_emplace(keys[size_t(input)]);
		Registration &registration = inserted.first->second;
		registration.inputs.push_back(input);
		if (!inserted.second) {
			continue;
		}
		update_stats.added++;
		const GeometryInstance &primitive = p_primitives[size_t(input)];
		int low[3];
		int high[3];
		bool outside = false;
		for (int axis = 0; axis < 3; axis++) {
			const double first = std::floor((primitive.bounds_min[axis] - p_grid.min[axis]) / (TRUNK * p_grid.spacing)) - 1;
			const double last = std::floor((primitive.bounds_max[axis] - p_grid.min[axis]) / (TRUNK * p_grid.spacing)) + 1;
			if (last < 0 || first >= trunk_size[axis]) {
				outside = true;
				break;
			}
			low[axis] = int(std::max(0.0, first));
			high[axis] = int(std::min(double(trunk_size[axis] - 1), last));
		}
		if (outside) {
			continue;
		}
		// The broad range includes one neighbouring Trunk. This exact support-box test
		// matches sampling at both ends of a 26-neighbour link, including partial Trunks.
		for (int z = low[2]; z <= high[2]; z++) {
			for (int y = low[1]; y <= high[1]; y++) {
				for (int x = low[0]; x <= high[0]; x++) {
					const int coordinate[3] = { x, y, z };
					bool overlap = true;
					update_stats.overlap_tests++;
					for (int axis = 0; axis < 3; axis++) {
						const double support_min = p_grid.min[axis] + (coordinate[axis] * TRUNK - 1) * p_grid.spacing;
						const double support_max = p_grid.min[axis] + (coordinate[axis] * TRUNK + TRUNK + 1) * p_grid.spacing;
						if (primitive.bounds_min[axis] > support_max || primitive.bounds_max[axis] < support_min) {
							overlap = false;
							break;
						}
					}
					if (!overlap) {
						continue;
					}
					const int trunk = x + trunk_size[0] * (y + trunk_size[1] * z);
					registration.trunks.push_back(trunk);
					trunk_members[size_t(trunk)].push_back(&registration);
				}
			}
		}
	}
	for (auto entry = registrations.begin(); entry != registrations.end();) {
		Registration &registration = entry->second;
		if (!registration.inputs.empty()) {
			++entry;
			continue;
		}
		for (int trunk : registration.trunks) {
			std::vector<Registration *> &members = trunk_members[size_t(trunk)];
			members.erase(std::remove(members.begin(), members.end(), &registration), members.end());
		}
		entry = registrations.erase(entry);
		update_stats.removed++;
	}
	candidate_references = 0;
	for (int trunk = 0; trunk < trunk_count; trunk++) {
		std::vector<int> &inputs = candidates[size_t(trunk)];
		inputs.clear();
		for (const Registration *registration : trunk_members[size_t(trunk)]) {
			inputs.insert(inputs.end(), registration->inputs.begin(), registration->inputs.end());
		}
		// Preserve the caller's order, including duplicates, for nearest-surface ties.
		// Reordering inputs can change a digest without rebuilding any registration.
		std::sort(inputs.begin(), inputs.end());
		candidate_references += inputs.size();
		uint64_t signature = 1469598103934665603ull;
		LocalInputSignatures components{ signature, signature, signature };
		for (int input : inputs) {
			const GeometryInstance &primitive = p_primitives[size_t(input)];
			mix_bytes(signature, &primitive.signature, sizeof(primitive.signature));
			mix_bytes(signature, &primitive.layer_mask, sizeof(primitive.layer_mask));
			const PrimitiveKey &key = keys[size_t(input)];
			mix_bytes(components.geometry, key.data(), key.size() * sizeof(uint64_t));
			mix_bytes(components.reflectance, &primitive.input_signatures.reflectance, sizeof(uint64_t));
			mix_bytes(components.emission, &primitive.input_signatures.emission, sizeof(uint64_t));
		}
		mix_bytes(signature, &components, sizeof(components));
		signatures[size_t(trunk)] = signature;
		input_signatures[size_t(trunk)] = components;
	}
	previous_inputs = std::move(keys);
	previous_material_inputs = std::move(material_inputs);
}

uint64_t TrunkIndex::payload_bytes() const {
	uint64_t bytes = sizeof(TrunkIndex) + registrations.size() * sizeof(decltype(registrations)::value_type) +
			previous_inputs.capacity() * sizeof(PrimitiveKey) + previous_material_inputs.capacity() * sizeof(decltype(previous_material_inputs)::value_type) +
			signatures.capacity() * sizeof(uint64_t) + input_signatures.capacity() * sizeof(LocalInputSignatures) +
			trunk_members.capacity() * sizeof(decltype(trunk_members)::value_type) +
			candidates.capacity() * sizeof(decltype(candidates)::value_type);
	for (const auto &entry : registrations) {
		bytes += (entry.second.trunks.capacity() + entry.second.inputs.capacity()) * sizeof(int);
	}
	for (const auto &members : trunk_members) {
		bytes += members.capacity() * sizeof(Registration *);
	}
	for (const auto &inputs : candidates) {
		bytes += inputs.capacity() * sizeof(int);
	}
	return bytes;
}

int TrunkSurfaceWorkspace::build_node(int p_begin, int p_end) {
	const int slot = int(nodes.size());
	nodes.emplace_back();
	Node node;
	node.begin = p_begin;
	node.end = p_end;
	node.low = cells[size_t(order[size_t(p_begin)])].low;
	node.high = cells[size_t(order[size_t(p_begin)])].high;
	for (int i = p_begin + 1; i < p_end; i++) {
		const Cell &cell = cells[size_t(order[size_t(i)])];
		for (int axis = 0; axis < 3; axis++) {
			node.low[axis] = std::min(node.low[axis], cell.low[axis]);
			node.high[axis] = std::max(node.high[axis], cell.high[axis]);
		}
	}
	if (p_end - p_begin > 8) {
		int axis = 0;
		for (int i = 1; i < 3; i++) {
			if (node.high[i] - node.low[i] > node.high[axis] - node.low[axis]) {
				axis = i;
			}
		}
		const int middle = p_begin + (p_end - p_begin) / 2;
		std::nth_element(order.begin() + p_begin, order.begin() + middle, order.begin() + p_end, [&](int p_a, int p_b) {
			const Cell &a = cells[size_t(p_a)];
			const Cell &b = cells[size_t(p_b)];
			const double first = a.low[axis] + a.high[axis];
			const double second = b.low[axis] + b.high[axis];
			return first == second ? p_a < p_b : first < second;
		});
		node.left = build_node(p_begin, middle);
		node.right = build_node(middle, p_end);
	}
	nodes[size_t(slot)] = node;
	return slot;
}

bool TrunkSurfaceWorkspace::build(const Grid &p_grid, int p_trunk,
		const std::vector<GeometryInstance> &p_instances, const std::vector<int> &p_candidates, const std::atomic<bool> *p_cancel) {
	cells.clear();
	order.clear();
	nodes.clear();
	const int width = (p_grid.size[0] + TRUNK - 1) / TRUNK;
	const int height = (p_grid.size[1] + TRUNK - 1) / TRUNK;
	const int coordinate[3] = { p_trunk % width, (p_trunk / width) % height, p_trunk / (width * height) };
	int first_probe[3];
	int last_probe[3];
	for (int axis = 0; axis < 3; axis++) {
		first_probe[axis] = coordinate[axis] * TRUNK - 1;
		last_probe[axis] = std::min((coordinate[axis] + 1) * TRUNK, p_grid.size[axis]);
	}
	const Vec3 support_low = probe_point(p_grid, first_probe[0], first_probe[1], first_probe[2]);
	const Vec3 support_high = probe_point(p_grid, last_probe[0], last_probe[1], last_probe[2]);
	for (int input : p_candidates) {
		if (p_cancel && p_cancel->load()) {
			return false;
		}
		const GeometryInstance &instance = p_instances[size_t(input)];
		if (instance.determinant == 0.0) {
			continue;
		}
		const CoarseGeometryField &geometry = *instance.geometry;
		Vec3 local_low;
		Vec3 local_high;
		affine_bounds(instance, support_low, support_high, true, local_low, local_high);
		int first[3];
		int last[3];
		bool outside = false;
		for (int axis = 0; axis < 3; axis++) {
			const double low = std::ceil(rounded_down(rounded_down(rounded_down(local_low[axis] - geometry.min[axis]) / geometry.cell) - 0.5));
			const double high = std::floor(rounded_up(rounded_up(rounded_up(local_high[axis] - geometry.min[axis]) / geometry.cell) + 0.5));
			if (low >= geometry.size[axis] || high < 0) {
				outside = true;
				break;
			}
			first[axis] = int(std::max(0.0, low));
			last[axis] = int(std::min(double(geometry.size[axis] - 1), high));
		}
		if (outside) {
			continue;
		}
		for (int z = first[2]; z <= last[2]; z++) {
			for (int y = first[1]; y <= last[1]; y++) {
				if (p_cancel && p_cancel->load()) {
					return false;
				}
				for (int x = first[0]; x <= last[0]; x++) {
					const int index = x + geometry.size[0] * (y + geometry.size[1] * z);
					if (geometry.cells[size_t(index)] == 0) {
						continue;
					}
					Vec3 low;
					Vec3 high;
					Vec3 point_low;
					Vec3 point_high;
					geometry_cell_bounds(geometry, index, true, low, high);
					geometry_cell_bounds(geometry, index, false, point_low, point_high);
					for (int axis = 0; axis < 3; axis++) {
						low[axis] = std::min(low[axis], point_low[axis]);
						high[axis] = std::max(high[axis], point_high[axis]);
					}
					Cell cell;
					cell.instance = &instance;
					cell.index = index;
					affine_bounds(instance, low, high, false, cell.low, cell.high);
					bool overlaps = true;
					for (int axis = 0; axis < 3; axis++) {
						overlaps &= cell.low[axis] <= support_high[axis] && cell.high[axis] >= support_low[axis];
					}
					if (overlaps) {
						cells.push_back(cell);
					}
				}
			}
		}
	}
	order.resize(cells.size());
	for (int i = 0; i < int(order.size()); i++) {
		order[size_t(i)] = i;
	}
	if (!cells.empty()) {
		build_node(0, int(cells.size()));
	}
	return !(p_cancel && p_cancel->load());
}

void TrunkSurfaceWorkspace::query_node(int p_node, Query &r_query) const {
	const Node &node = nodes[size_t(p_node)];
	double enter;
	double exit;
	if (!segment_box(r_query.canonical_from, r_query.canonical_to - r_query.canonical_from, node.low, node.high, enter, exit)) {
		return;
	}
	if (node.left >= 0) {
		query_node(node.left, r_query);
		query_node(node.right, r_query);
		return;
	}
	const GeometryInstance *previous_instance = nullptr;
	Vec3 local_from;
	Vec3 local_delta;
	for (int i = node.begin; i < node.end; i++) {
		const int ordinal = order[size_t(i)];
		const Cell &cell = cells[size_t(ordinal)];
		const GeometryInstance &instance = *cell.instance;
		if (previous_instance != &instance) {
			local_from = instance_local_point(instance, r_query.canonical_from);
			local_delta = instance_local_point(instance, r_query.canonical_to) - local_from;
			previous_instance = &instance;
		}
		Vec3 low;
		Vec3 high;
		geometry_cell_bounds(*instance.geometry, cell.index, r_query.segment, low, high);
		double first;
		double last;
		if (!segment_box(local_from, local_delta, low, high, first, last)) {
			continue;
		}
		const double fraction = r_query.reversed ? 1.0 - last : first;
		if (r_query.selected >= 0 && (fraction > r_query.sample.fraction || (fraction == r_query.sample.fraction && ordinal >= r_query.selected))) {
			continue;
		}
		r_query.selected = ordinal;
		r_query.sample = instance_cell_sample(instance, cell.index, r_query.from, r_query.to, fraction, r_query.segment);
	}
}

bool TrunkSurfaceWorkspace::query(const Vec3 &p_from, const Vec3 &p_to, bool p_segment, SurfaceSample &r_sample) const {
	if (nodes.empty()) {
		return false;
	}
	Query query;
	query.from = p_from;
	query.to = p_to;
	query.segment = p_segment;
	for (int axis = 0; axis < 3; axis++) {
		if (p_from[axis] != p_to[axis]) {
			query.reversed = p_from[axis] > p_to[axis];
			break;
		}
	}
	query.canonical_from = query.reversed ? p_to : p_from;
	query.canonical_to = query.reversed ? p_from : p_to;
	query_node(0, query);
	if (query.selected < 0) {
		return false;
	}
	r_sample = query.sample;
	return true;
}

bool TrunkSurfaceWorkspace::sample(const Vec3 &p_point, SurfaceSample &r_sample) const {
	return query(p_point, p_point, false, r_sample);
}

bool TrunkSurfaceWorkspace::trace_segment(const Vec3 &p_from, const Vec3 &p_to, SurfaceSample &r_sample) const {
	return query(p_from, p_to, true, r_sample);
}

uint64_t TrunkSurfaceWorkspace::payload_bytes() const {
	return cells.capacity() * sizeof(Cell) + order.capacity() * sizeof(int) + nodes.capacity() * sizeof(Node);
}

uint64_t box_field_signature(const Vec3 &p_extent, int p_resolution) {
	uint64_t hash = 1469598103934665603ull;
	mix_value(hash, p_extent.x);
	mix_value(hash, p_extent.y);
	mix_value(hash, p_extent.z);
	mix_value(hash, double(p_resolution));
	return hash;
}

uint64_t instance_field_signature(const SurfaceMaterialField &p_field) {
	uint64_t hash = 1469598103934665603ull;
	mix_value(hash, p_field.cell_stride);
	for (int axis = 0; axis < 3; axis++) {
		mix_value(hash, double(p_field.color_size[axis]));
	}
	if (!p_field.albedo.empty()) {
		mix_bytes(hash, p_field.albedo.data(), p_field.albedo.size());
	}
	if (!p_field.emission.empty()) {
		mix_bytes(hash, p_field.emission.data(), p_field.emission.size() * sizeof(float));
	}
	mix_bytes(hash, p_field.emission_sides.data(), p_field.emission_sides.size());
	return hash;
}

uint64_t material_component_signature(const SurfaceMaterialField &p_field, bool p_emission) {
	uint64_t hash = 1469598103934665603ull;
	mix_value(hash, p_field.cell_stride);
	for (int axis = 0; axis < 3; axis++) {
		mix_value(hash, double(p_field.color_size[axis]));
	}
	if (p_emission) {
		mix_bytes(hash, p_field.emission.data(), p_field.emission.size() * sizeof(float));
		mix_bytes(hash, p_field.emission_sides.data(), p_field.emission_sides.size());
	} else {
		mix_bytes(hash, p_field.albedo.data(), p_field.albedo.size());
	}
	return hash;
}

uint64_t material_field_input_signature(const std::vector<MeshTriangle> &p_triangles, const MaterialCapture *p_material) {
	uint64_t hash = 1469598103934665603ull;
	if (p_material == nullptr) {
		for (const MeshTriangle &triangle : p_triangles) {
			for (const Vec3 &color : triangle.color) {
				mix_bytes(hash, &color, sizeof(Vec3));
			}
		}
		return hash;
	}
	for (int axis = 0; axis < 3; axis++) {
		mix_value(hash, double(p_material->size[axis]));
	}
	mix_bytes(hash, &p_material->uvw_offset, sizeof(Vec3));
	mix_bytes(hash, &p_material->uvw_basis_x, sizeof(Vec3));
	mix_bytes(hash, &p_material->uvw_basis_y, sizeof(Vec3));
	mix_bytes(hash, &p_material->uvw_basis_z, sizeof(Vec3));
	if (!p_material->albedo.empty()) {
		mix_bytes(hash, p_material->albedo.data(), p_material->albedo.size() * sizeof(float));
	}
	if (!p_material->emission.empty()) {
		mix_bytes(hash, p_material->emission.data(), p_material->emission.size() * sizeof(float));
	}
	mix_bytes(hash, p_material->emission_sides.data(), p_material->emission_sides.size());
	if (!p_material->occupied.empty()) {
		mix_bytes(hash, p_material->occupied.data(), p_material->occupied.size());
	}
	return hash;
}

uint64_t primitive_signature(uint64_t p_field_signature, uint64_t p_instance_signature, const PrimitiveTransform &p_transform) {
	uint64_t hash = 1469598103934665603ull;
	mix_bytes(hash, &p_field_signature, sizeof(uint64_t));
	mix_bytes(hash, &p_instance_signature, sizeof(uint64_t));
	mix_value(hash, p_transform.origin.x);
	mix_value(hash, p_transform.origin.y);
	mix_value(hash, p_transform.origin.z);
	mix_value(hash, p_transform.basis_x.x);
	mix_value(hash, p_transform.basis_x.y);
	mix_value(hash, p_transform.basis_x.z);
	mix_value(hash, p_transform.basis_y.x);
	mix_value(hash, p_transform.basis_y.y);
	mix_value(hash, p_transform.basis_y.z);
	mix_value(hash, p_transform.basis_z.x);
	mix_value(hash, p_transform.basis_z.y);
	mix_value(hash, p_transform.basis_z.z);
	return hash;
}

LocalField build_local_data(const Grid &p_grid, const BoxQuery &p_query, const std::atomic<bool> *p_cancel, int p_threads) {
	LocalField field;
	field.material.assign(size_t(p_grid.count) * 4, 0.0f);
	field.local_record_indices.assign(size_t(p_grid.count), LocalField::NO_RECORD);
	field.links.assign(size_t(p_grid.count), 0u);
	field.dirty_mask.assign(size_t(p_grid.count), 0);
	// One row of probes per parallel unit; every counter is summed in y order afterwards, so the
	// field is identical to the serial sweep.
	const int rows = p_grid.size[1];
	std::vector<int> row_solid(size_t(rows), 0);
	std::vector<int> row_surface(size_t(rows), 0);
	std::vector<std::vector<std::pair<int, LocalRecord>>> row_records(size_t(rows), std::vector<std::pair<int, LocalRecord>>{});
	std::atomic<bool> cancelled(false);
	parallel_for(rows, p_threads, [&](int y) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				if (!p_query.contains(probe_point(p_grid, x, y, z))) {
					continue;
				}
				const int index = index_of(p_grid, x, y, z);
				field.material[index * 4 + 3] = 1.0f;
				row_solid[size_t(y)]++;
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	const Direction *dirs = directions();
	parallel_for(rows, p_threads, [&](int y) {
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				if (field.material[index * 4 + 3] != 0.0f) {
					continue;
				}
				int blocked_count = 0;
				std::array<LocalSurface, DIRECTION_COUNT> surfaces;
				for (int j = 0; j < DIRECTION_COUNT; j++) {
					const int qx = x + dirs[j].offset[0];
					const int qy = y + dirs[j].offset[1];
					const int qz = z + dirs[j].offset[2];
					if (inside(p_grid, qx, qy, qz) && field.material[index_of(p_grid, qx, qy, qz) * 4 + 3] != 0.0f) {
						blocked_count++;
						surfaces[size_t(j)].reflectance = Vec3(0.5, 0.5, 0.5);
					} else {
						field.links[index] |= 1u << uint32_t(j);
					}
				}
				LocalRecord record = build_local_record(field.links[size_t(index)], surfaces);
				if (record.has_contribution()) {
					row_records[size_t(y)].emplace_back(index, record);
				}
				field.material[size_t(index) * 4 + 1] = float(blocked_count);
				if (blocked_count > 0) {
					row_surface[size_t(y)]++;
				}
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	for (int y = 0; y < rows; y++) {
		for (const auto &update : row_records[size_t(y)]) {
			field.set_local_record(size_t(update.first), update.second);
		}
		field.solid_count += row_solid[size_t(y)];
		field.surface_count += row_surface[size_t(y)];
	}
	return field;
}

// src/sdf-local.js buildSDFLocalData, including its incremental cache: a trunk of 8^3 probes is
// recomputed only when its candidate primitives changed. A recycled bank catches up by copying
// only Trunks changed since that bank's snapshot; an unseeded bank starts with a full copy.
LocalField build_sdf_local_data(const Grid &p_grid, const std::vector<GeometryInstance> &p_primitives,
		const std::atomic<bool> *p_cancel, int p_threads, const LocalCache *p_previous, LocalCache *r_cache, LocalField *p_reuse,
		const TrunkIndex *p_index) {
	using Clock = std::chrono::steady_clock;
	const auto started = Clock::now();
	LocalField field;
	if (p_reuse != nullptr) {
		field = std::move(*p_reuse);
	}
	field.sdf_setup_ms = field.sdf_sample_ms = field.sdf_transfer_ms = field.sdf_merge_ms = 0.0;
	const size_t probe_count = size_t(p_grid.count);
	// Prototype dirty test: reuse the previous field only when it describes this very grid. The
	// digest is read before anything is written, because a reusable previous field also seeds
	// every array the dirty Trunks do not touch.
	const uint64_t grid_key = grid_signature(p_grid);
	TrunkIndex owned_index;
	if (p_index == nullptr) {
		owned_index.update(p_grid, p_primitives);
		p_index = &owned_index;
	}
	std::vector<uint64_t> signatures = p_index->get_signatures();
	const std::vector<LocalInputSignatures> &input_signatures = p_index->get_input_signatures();
	const bool have_previous = p_previous != nullptr && p_previous->local != nullptr &&
			p_previous->grid_key == grid_key &&
			p_previous->local->material.size() == probe_count * 4 &&
			p_previous->trunk_signatures.size() == signatures.size() &&
			p_previous->input_signatures.size() == input_signatures.size() &&
			p_previous->samples.size() == probe_count &&
			p_previous->sampled.size() == probe_count;
	const LocalField *previous_local = have_previous ? p_previous->local : nullptr;
	// The whole-field arrays can only seed from a previous field with the same probe count.
	const bool can_seed = previous_local != nullptr &&
			previous_local->local_record_indices.size() == probe_count &&
			previous_local->links.size() == probe_count;
	const LocalField &previous_field = can_seed ? *previous_local : field;
	const bool reuse_snapshot = can_seed && r_cache != nullptr && p_reuse != nullptr &&
			r_cache->local == p_reuse && r_cache->grid_key == grid_key &&
			r_cache->trunk_signatures.size() == signatures.size() &&
			r_cache->samples.size() == probe_count && r_cache->sampled.size() == probe_count &&
			field.material.size() == probe_count * 4 && field.local_record_indices.size() == probe_count &&
			field.links.size() == probe_count;
	field.snapshot_reused = reuse_snapshot;
	field.snapshot_sync_probes = 0;
	if (can_seed) {
		// Both CPU banks share slot identities, not storage. Copy only the sparse slot payload;
		// the recycled bank's dense Probe map catches up by changed Trunks below.
		field.local_records = previous_field.local_records;
		field.local_record_owners = previous_field.local_record_owners;
		field.free_local_records = previous_field.free_local_records;
	}
	if (reuse_snapshot) {
		const int trunk_size[3] = {
			(p_grid.size[0] + TRUNK - 1) / TRUNK,
			(p_grid.size[1] + TRUNK - 1) / TRUNK,
			(p_grid.size[2] + TRUNK - 1) / TRUNK,
		};
		for (size_t trunk = 0; trunk < signatures.size(); trunk++) {
			if (r_cache->trunk_signatures[trunk] == p_previous->trunk_signatures[trunk]) {
				continue;
			}
			const int tx = int(trunk) % trunk_size[0];
			const int ty = (int(trunk) / trunk_size[0]) % trunk_size[1];
			const int tz = int(trunk) / (trunk_size[0] * trunk_size[1]);
			for (int y = ty * TRUNK; y < MIN((ty + 1) * TRUNK, p_grid.size[1]); y++) {
				for (int z = tz * TRUNK; z < MIN((tz + 1) * TRUNK, p_grid.size[2]); z++) {
					for (int x = tx * TRUNK; x < MIN((tx + 1) * TRUNK, p_grid.size[0]); x++) {
						const size_t probe = size_t(index_of(p_grid, x, y, z));
						std::copy_n(previous_field.material.data() + probe * 4, 4, field.material.data() + probe * 4);
						field.local_record_indices[probe] = previous_field.local_record_indices[probe];
						field.links[probe] = previous_field.links[probe];
						r_cache->samples[probe] = p_previous->samples[probe];
						r_cache->sampled[probe] = p_previous->sampled[probe];
						field.snapshot_sync_probes++;
					}
				}
			}
		}
	}
	auto seed_floats = [&](std::vector<float> &r_target, const std::vector<float> &p_source, size_t p_size, float p_value) {
		if (can_seed && p_source.size() == p_size) {
			r_target = p_source;
		} else {
			r_target.assign(p_size, p_value);
		}
	};
	// The published field remains readable while the worker updates a separate snapshot.
	if (!reuse_snapshot) {
		seed_floats(field.material, previous_field.material, probe_count * 4, 0.0f);
		if (can_seed) {
			field.local_record_indices = previous_field.local_record_indices;
		} else {
			field.local_record_indices.assign(probe_count, LocalField::NO_RECORD);
			field.local_records.clear();
			field.local_record_owners.clear();
			field.free_local_records.clear();
		}
		if (can_seed) {
			field.links = previous_field.links;
		} else {
			field.links.assign(probe_count, 0u);
		}
	}
	// Keep the allocation; sparse updates clear the mask before visiting active rows.
	field.dirty_mask.resize(probe_count);
	field.dirty_probes.clear();
	field.changed_occupancy.clear();
	field.changed_occupancy_valid = false;
	field.links_changed = true;
	field.solid_count = 0;
	field.surface_count = 0;
	field.classification_mismatches = 0;
	field.trunk_count = 0;
	field.dirty_trunk_count = 0;
	field.geometry_dirty_trunks = field.reflectance_dirty_trunks = field.emission_dirty_trunks = 0;
	field.occupancy_probes_updated = field.geometry_records_built = 0;
	field.reflectance_records_updated = field.emission_records_updated = field.transport_weights_updated = 0;

	// Only dirty Trunks assemble a temporary union of covered cells for both query passes.
	// The trunk lattice is flat and indexed, not a map: probing needs one lookup per probe, and
	// the incremental test below is a plain compare over the same index space.
	const int trunk_size[3] = {
		(p_grid.size[0] + TRUNK - 1) / TRUNK,
		(p_grid.size[1] + TRUNK - 1) / TRUNK,
		(p_grid.size[2] + TRUNK - 1) / TRUNK,
	};
	const int trunk_count = trunk_size[0] * trunk_size[1] * trunk_size[2];
	auto trunk_of = [&trunk_size](int p_x, int p_y, int p_z) {
		return (p_x / TRUNK) + trunk_size[0] * ((p_y / TRUNK) + trunk_size[1] * (p_z / TRUNK));
	};
	std::vector<TrunkSurfaceWorkspace> trunks;
	trunks.resize(size_t(trunk_count));
	std::vector<uint8_t> dirty(size_t(trunk_count), LOCAL_ALL);
	std::vector<int> dirty_trunks;
	for (int trunk = 0; trunk < trunk_count; trunk++) {
		uint8_t changes = LOCAL_ALL;
		if (can_seed) {
			const LocalInputSignatures &before = p_previous->input_signatures[size_t(trunk)];
			const LocalInputSignatures &after = input_signatures[size_t(trunk)];
			if (before.geometry == after.geometry) {
				changes = (before.reflectance != after.reflectance ? LOCAL_REFLECTANCE : 0) |
						(before.emission != after.emission ? LOCAL_EMISSION : 0);
			}
		}
		dirty[size_t(trunk)] = changes;
		field.geometry_dirty_trunks += (changes & LOCAL_GEOMETRY) != 0;
		field.reflectance_dirty_trunks += (changes & LOCAL_REFLECTANCE) != 0;
		field.emission_dirty_trunks += (changes & LOCAL_EMISSION) != 0;
		if (changes != 0) {
			dirty_trunks.push_back(trunk);
		}
	}
	field.dirty_trunk_count = int(dirty_trunks.size());
	std::atomic<bool> cancelled(false);
	parallel_for(int(dirty_trunks.size()), p_threads, [&](int p_work) {
		const int trunk = dirty_trunks[size_t(p_work)];
		if (!trunks[size_t(trunk)].build(p_grid, trunk, p_primitives, p_index->get_candidates(trunk), p_cancel)) {
			cancelled.store(true);
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	field.surface_workspaces_built = int(dirty_trunks.size());
	field.surface_workspace_cells = 0;
	field.surface_workspace_payload_bytes = 0;
	for (int trunk : dirty_trunks) {
		field.surface_workspace_cells += trunks[size_t(trunk)].get_cell_count();
		field.surface_workspace_payload_bytes += trunks[size_t(trunk)].payload_bytes();
	}
	field.trunk_count = trunk_count;

	std::vector<SurfaceSample> samples;
	std::vector<uint8_t> sampled;
	if (reuse_snapshot) {
		samples = std::move(r_cache->samples);
		sampled = std::move(r_cache->sampled);
	} else if (can_seed) {
		// Clean Trunks reproduce the previous samples exactly, so the previous cache carries them.
		samples = p_previous->samples;
		sampled = p_previous->sampled;
	} else {
		samples.assign(probe_count, SurfaceSample());
		sampled.assign(probe_count, 0);
	}
	// Each row owns its probe outputs. Only compact records survive the worker passes;
	// surface samples used for integration are transient stack data.
	struct SdfRow {
		std::vector<std::pair<int, LocalRecord>> records;
		std::vector<int> dirty_probes;
		int solid = 0;
		int surface = 0;
		int occupancy = 0;
		int geometry = 0;
		int reflectance = 0;
		int emission = 0;
		int weights = 0;
	};
	const int rows = p_grid.size[1];
	std::vector<SdfRow> row_data;
	row_data.resize(size_t(rows));
	std::vector<int> active_rows;
	for (int y = 0; y < rows; y++) {
		bool active = false;
		for (int z = 0; !active && z < trunk_size[2]; z++) {
			for (int x = 0; x < trunk_size[0]; x++) {
				active |= dirty[size_t(x + trunk_size[0] * (y / TRUNK + trunk_size[1] * z))] != 0;
			}
		}
		if (active) {
			active_rows.push_back(y);
		}
	}
	std::fill(field.dirty_mask.begin(), field.dirty_mask.end(), 0);
	if (can_seed) {
		field.solid_count = previous_field.solid_count;
		field.surface_count = previous_field.surface_count;
	}
	// Match nested row fan-out to the actual dirty regions. A single bounded trunk
	// update stays on its bake worker instead of waking the entire pool twice.
	const int update_threads = can_seed ? std::min(p_threads, field.dirty_trunk_count) : p_threads;
	const auto sampling_started = Clock::now();
	parallel_for(int(active_rows.size()), update_threads, [&](int p_row) {
		const int y = active_rows[size_t(p_row)];
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				const size_t trunk = size_t(trunk_of(x, y, z));
				field.dirty_mask[size_t(index)] = dirty[trunk] != 0;
				if (!dirty[trunk]) {
					// Clean Trunk: samples and material are already seeded from the previous field.
					continue;
				}
				// Geometry replaces the complete local answer; appearance keeps its seeded topology.
				row_data[size_t(y)].dirty_probes.push_back(index);
				if (!(dirty[trunk] & LOCAL_GEOMETRY)) {
					// Only refresh captured appearance; occupancy remains the published answer.
					if (sampled[size_t(index)]) {
						SurfaceSample value;
						const bool covered = trunks[trunk].sample(probe_point(p_grid, x, y, z), value);
						DEV_ASSERT(covered);
						samples[size_t(index)] = value;
					}
					continue;
				}
				row_data[size_t(y)].occupancy++;
				if (can_seed) {
					const bool was_solid = previous_field.material[size_t(index) * 4 + 3] > 0.5f;
					row_data[size_t(y)].solid -= was_solid ? 1 : 0;
					row_data[size_t(y)].surface -= !was_solid && previous_field.material[size_t(index) * 4 + 1] > 0.0f ? 1 : 0;
				}
				field.material[index * 4 + 0] = 0.0f;
				field.material[index * 4 + 1] = 0.0f;
				field.material[index * 4 + 2] = 0.0f;
				field.material[index * 4 + 3] = 0.0f;
				// A query that finds nothing must clear the seeded sample, or the previous answer
				// would be re-read by this probe and by its neighbours during local integration.
				samples[size_t(index)] = SurfaceSample();
				sampled[size_t(index)] = 0;
				SurfaceSample value;
				if (trunks[trunk].sample(probe_point(p_grid, x, y, z), value)) {
					samples[size_t(index)] = value;
					sampled[size_t(index)] = 1;
				}
				if (sampled[size_t(index)]) {
					field.material[index * 4 + 3] = 1.0f;
					row_data[size_t(y)].solid++;
				}
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	const auto transfer_started = Clock::now();
	const Direction *dirs = directions();
	parallel_for(int(active_rows.size()), update_threads, [&](int p_row) {
		const int y = active_rows[size_t(p_row)];
		if (p_cancel && p_cancel->load()) {
			cancelled.store(true);
			return;
		}
		SdfRow &row = row_data[size_t(y)];
		for (int z = 0; z < p_grid.size[2]; z++) {
			for (int x = 0; x < p_grid.size[0]; x++) {
				const int index = index_of(p_grid, x, y, z);
				const size_t trunk_index = size_t(trunk_of(x, y, z));
				if (!dirty[trunk_index]) {
					continue;
				}
				const uint8_t changes = dirty[trunk_index];
				const bool geometry_changed = (changes & LOCAL_GEOMETRY) != 0;
				if (geometry_changed) {
					field.links[index] = 0u;
				}
				if (field.material[index * 4 + 3] != 0.0f ||
						(!geometry_changed && field.material[index * 4 + 1] == 0.0f)) {
					continue;
				}
				const Vec3 origin = probe_point(p_grid, x, y, z);
				const TrunkSurfaceWorkspace &trunk = trunks[trunk_index];
				int blocked_count = 0;

				std::array<LocalSurface, DIRECTION_COUNT> surfaces;
				for (int j = 0; j < DIRECTION_COUNT; j++) {
					if (!geometry_changed && (field.links[size_t(index)] & (1u << uint32_t(j)))) {
						continue;
					}
					const int qx = x + dirs[j].offset[0];
					const int qy = y + dirs[j].offset[1];
					const int qz = z + dirs[j].offset[2];
					SurfaceSample value;
					const Vec3 neighbor_position = probe_point(p_grid, qx, qy, qz);
					const bool blocked = trunk.trace_segment(origin, neighbor_position, value);
					if (geometry_changed && !blocked) {
						field.links[index] |= 1u << uint32_t(j);
						continue;
					}
					DEV_ASSERT(blocked);
					surfaces[size_t(j)] = { value.color, value.emission };
					blocked_count++;
				}
				LocalRecord record = update_local_record(field.links[size_t(index)], surfaces, field.get_local_record(size_t(index)), changes);
				bool new_geometry = false;
				if (!geometry_changed && field.local_record_indices[size_t(index)] == LocalField::NO_RECORD && record.has_contribution()) {
					// Newly contributing black occluders derive G from the existing OpenMask.
					// They do not trigger an occupancy or visibility solve.
					record = update_local_record(field.links[size_t(index)], surfaces, record, LOCAL_GEOMETRY);
					new_geometry = true;
				}
				row.geometry += geometry_changed || new_geometry;
				row.reflectance += (changes & LOCAL_REFLECTANCE) != 0;
				row.emission += (changes & LOCAL_EMISSION) != 0;
				row.weights += (changes & (LOCAL_GEOMETRY | LOCAL_REFLECTANCE)) != 0 || new_geometry;
				if (record.has_contribution()) {
					row.records.emplace_back(index, record);
				}
				if (geometry_changed) {
					field.material[size_t(index) * 4 + 1] = float(blocked_count);
					if (blocked_count > 0) {
						row.surface++;
					}
				}
			}
		}
	});
	if (cancelled.load()) {
		return LocalField();
	}
	// Release all borrowed-cell acceleration data before staging the publication arrays.
	trunks.clear();
	// Release all removed contributions before assigning new slots. Retained Probes keep
	// their identities, and new contributions reuse this batch's freed slots before growing.
	for (const SdfRow &row : row_data) {
		size_t next = 0;
		for (int probe : row.dirty_probes) {
			if (next < row.records.size() && row.records[next].first == probe) {
				next++;
			} else {
				field.set_local_record(size_t(probe), LocalRecord());
			}
		}
	}
	for (const SdfRow &row : row_data) {
		for (const auto &update : row.records) {
			field.set_local_record(size_t(update.first), update.second);
		}
	}
	const auto merge_started = Clock::now();
	// Deterministic row order preserves counters and the sparse dirty-probe list.
	for (const SdfRow &row : row_data) {
		field.solid_count += row.solid;
		field.surface_count += row.surface;
		field.occupancy_probes_updated += row.occupancy;
		field.geometry_records_built += row.geometry;
		field.reflectance_records_updated += row.reflectance;
		field.emission_records_updated += row.emission;
		field.transport_weights_updated += row.weights;
		if (can_seed) {
			field.dirty_probes.insert(field.dirty_probes.end(), row.dirty_probes.begin(), row.dirty_probes.end());
		}
	}
	field.changed_occupancy_valid = true;
	if (have_previous) {
		const LocalField &previous = *p_previous->local;
		field.links_changed = false;
		for (int index : field.dirty_probes) {
			if (previous.material[size_t(index) * 4 + 3] != field.material[size_t(index) * 4 + 3]) {
				field.changed_occupancy.push_back(index);
			}
			if (previous.links[size_t(index)] != field.links[size_t(index)]) {
				field.links_changed = true;
			}
		}
	}
	if (r_cache != nullptr) {
		// The next build's previous state. `local` is filled in by the caller, which owns the
		// field these entries belong to.
		r_cache->grid_key = grid_key;
		r_cache->trunk_signatures = std::move(signatures);
		r_cache->input_signatures = input_signatures;
		r_cache->samples = std::move(samples);
		r_cache->sampled = std::move(sampled);
		r_cache->local = nullptr;
	}
	field.sdf_setup_ms = std::chrono::duration<double, std::milli>(sampling_started - started).count();
	field.sdf_sample_ms = std::chrono::duration<double, std::milli>(transfer_started - sampling_started).count();
	field.sdf_transfer_ms = std::chrono::duration<double, std::milli>(merge_started - transfer_started).count();
	field.sdf_merge_ms = std::chrono::duration<double, std::milli>(Clock::now() - merge_started).count();
	return field;
}

// ---------------------------------------------------------------------------
// Triangle meshes: conservative voxelization and sampled distance fields.
// ---------------------------------------------------------------------------

namespace {

constexpr double MESH_EPSILON = 1e-6;
constexpr double MESH_WELD = 1e-6;



bool overlaps_on_axis(const Vec3 &p_axis, const Vec3 p_triangle[3], const Vec3 &p_half) {
	if (length_squared(p_axis) < 1e-24) {
		return true;
	}
	const double a = dot(p_triangle[0], p_axis);
	const double b = dot(p_triangle[1], p_axis);
	const double c = dot(p_triangle[2], p_axis);
	const double triangle_min = std::min(a, std::min(b, c));
	const double triangle_max = std::max(a, std::max(b, c));
	const double box_radius = p_half.x * std::fabs(p_axis.x) + p_half.y * std::fabs(p_axis.y) + p_half.z * std::fabs(p_axis.z);
	return triangle_min <= box_radius && triangle_max >= -box_radius;
}

// Same conservative SAT test used by Godot's Voxelizer through
// Geometry3D::triangle_box_overlap, expressed with the engine-independent Vec3 type.
bool triangle_box_overlap(const MeshTriangle &p_triangle, const Vec3 &p_center, const Vec3 &p_half) {
	Vec3 vertices[3] = {
		p_triangle.position[0] - p_center,
		p_triangle.position[1] - p_center,
		p_triangle.position[2] - p_center,
	};
	const Vec3 edges[3] = {
		vertices[1] - vertices[0],
		vertices[2] - vertices[1],
		vertices[0] - vertices[2],
	};
	const Vec3 box_axes[3] = {
		Vec3(1.0, 0.0, 0.0),
		Vec3(0.0, 1.0, 0.0),
		Vec3(0.0, 0.0, 1.0),
	};
	for (const Vec3 &axis : box_axes) {
		if (!overlaps_on_axis(axis, vertices, p_half)) {
			return false;
		}
	}
	if (!overlaps_on_axis(cross(edges[0], edges[1]), vertices, p_half)) {
		return false;
	}
	for (const Vec3 &edge : edges) {
		for (const Vec3 &axis : box_axes) {
			if (!overlaps_on_axis(cross(edge, axis), vertices, p_half)) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

TriangleMesh build_triangle_mesh(std::vector<MeshTriangle> p_triangles) {
	TriangleMesh mesh;
	mesh.triangles = std::move(p_triangles);
	const int triangle_count = int(mesh.triangles.size());
	if (triangle_count == 0) {
		return mesh;
	}
	// classifyVolumes(): weld attribute seams at the intersection query tolerance and
	// keep only shells whose every edge has two opposite halves.
	std::vector<int> parents(triangle_count);
	for (int i = 0; i < triangle_count; i++) {
		parents[i] = i;
	}
	std::function<int(int)> root = [&](int p_index) {
		while (parents[p_index] != p_index) {
			parents[p_index] = parents[parents[p_index]];
			p_index = parents[p_index];
		}
		return p_index;
	};
	struct EdgeKey {
		int64_t a[3];
		int64_t b[3];
		bool operator<(const EdgeKey &p_other) const {
			for (int i = 0; i < 3; i++) {
				if (a[i] != p_other.a[i]) {
					return a[i] < p_other.a[i];
				}
			}
			for (int i = 0; i < 3; i++) {
				if (b[i] != p_other.b[i]) {
					return b[i] < p_other.b[i];
				}
			}
			return false;
		}
	};
	struct EdgeEntry {
		std::vector<int> triangles;
		int balance = 0;
	};
	std::map<EdgeKey, EdgeEntry> edges;
	for (int i = 0; i < triangle_count; i++) {
		int64_t points[3][3];
		for (int v = 0; v < 3; v++) {
			for (int axis = 0; axis < 3; axis++) {
				points[v][axis] = int64_t(js_round(mesh.triangles[i].position[v][axis] / MESH_WELD));
			}
		}
		for (int j = 0; j < 3; j++) {
			const int64_t *first = points[j];
			const int64_t *second = points[(j + 1) % 3];
			bool forward = false;
			bool ordered = false;
			for (int axis = 0; axis < 3 && !ordered; axis++) {
				if (first[axis] != second[axis]) {
					ordered = true;
					forward = first[axis] < second[axis];
				}
			}
			EdgeKey key;
			for (int axis = 0; axis < 3; axis++) {
				key.a[axis] = forward ? first[axis] : second[axis];
				key.b[axis] = forward ? second[axis] : first[axis];
			}
			EdgeEntry &edge = edges[key];
			if (!edge.triangles.empty()) {
				parents[root(i)] = root(edge.triangles[0]);
			}
			edge.triangles.push_back(i);
			edge.balance += forward ? 1 : -1;
		}
	}
	std::set<int> shells;
	std::set<int> closed;
	for (int i = 0; i < triangle_count; i++) {
		shells.insert(root(i));
		closed.insert(root(i));
	}
	for (const auto &entry : edges) {
		if (entry.second.triangles.size() != 2 || entry.second.balance != 0) {
			closed.erase(root(entry.second.triangles[0]));
		}
	}
	mesh.shell.resize(triangle_count);
	mesh.shell_closed.assign(triangle_count, 0);
	mesh.shell_outward_sign.assign(triangle_count, 1);
	std::map<int, double> shell_volume;
	for (int i = 0; i < triangle_count; i++) {
		mesh.shell[i] = root(i);
		mesh.shell_closed[i] = closed.count(mesh.shell[i]) ? 1 : 0;
		const MeshTriangle &triangle = mesh.triangles[size_t(i)];
		shell_volume[mesh.shell[i]] += dot(triangle.position[0], cross(triangle.position[1], triangle.position[2])) / 6.0;
	}
	for (int i = 0; i < triangle_count; i++) {
		mesh.shell_outward_sign[i] = shell_volume[mesh.shell[i]] < 0.0 ? -1 : 1;
	}
	mesh.closed_shell_count = int(closed.size());
	mesh.open_shell_count = int(shells.size() - closed.size());
	mesh.has_closed_shell = !closed.empty();
	return mesh;
}

MeshSdfBakeResult bake_mesh_sdf(const TriangleMesh &p_mesh, int p_resolution, const std::atomic<bool> *p_cancel, int p_threads) {
	using Clock = std::chrono::steady_clock;
	MeshSdfBakeResult result;
	if (p_mesh.triangles.empty()) {
		result.error = MESH_SDF_BAKE_EMPTY;
		return result;
	}
	Vec3 bounds_min = p_mesh.triangles[0].position[0];
	Vec3 bounds_max = bounds_min;
	for (const MeshTriangle &triangle : p_mesh.triangles) {
		for (const Vec3 &position : triangle.position) {
			for (int axis = 0; axis < 3; axis++) {
				bounds_min[axis] = std::min(bounds_min[axis], position[axis]);
				bounds_max[axis] = std::max(bounds_max[axis], position[axis]);
			}
		}
	}
	const double longest = max3(bounds_max.x - bounds_min.x, bounds_max.y - bounds_min.y, bounds_max.z - bounds_min.z);
	if (p_resolution < 1 || longest == 0.0) {
		result.error = MESH_SDF_BAKE_DEGENERATE_BOUNDS;
		return result;
	}
	CoarseGeometryField &field = result.field;
	field.cell = longest / p_resolution;
	field.min = bounds_min - Vec3(field.cell, field.cell, field.cell);
	int64_t count = 1;
	for (int axis = 0; axis < 3; axis++) {
		const double dimension = std::ceil((bounds_max[axis] - bounds_min[axis]) / field.cell) + 3;
		if (dimension > INT32_MAX || count * int64_t(dimension) > INT32_MAX) {
			result.error = MESH_SDF_BAKE_TOO_LARGE;
			return result;
		}
		field.size[axis] = int(dimension);
		count *= field.size[axis];
	}
	result.sample_count = uint64_t(count);
	field.cells.assign(size_t(count), 0u);
	const auto started = Clock::now();
	const Vec3 half(field.cell * 0.5, field.cell * 0.5, field.cell * 0.5);
	for (const MeshTriangle &triangle : p_mesh.triangles) {
		if (p_cancel && p_cancel->load()) {
			result.error = MESH_SDF_BAKE_CANCELLED;
			return result;
		}
		// Godot front faces use clockwise winding.
		const Vec3 normal = cross(triangle.position[2] - triangle.position[0], triangle.position[1] - triangle.position[0]);
		if (length_squared(normal) == 0.0) {
			continue;
		}
		const uint32_t mask = coarse_normal_mask(normal);
		int low[3];
		int high[3];
		for (int axis = 0; axis < 3; axis++) {
			const double first = std::min(triangle.position[0][axis], std::min(triangle.position[1][axis], triangle.position[2][axis]));
			const double last = std::max(triangle.position[0][axis], std::max(triangle.position[1][axis], triangle.position[2][axis]));
			low[axis] = std::max(0, int(std::ceil((first - half[axis] - field.min[axis]) / field.cell)));
			high[axis] = std::min(field.size[axis] - 1, int(std::floor((last + half[axis] - field.min[axis]) / field.cell)));
		}
		for (int z = low[2]; z <= high[2]; z++) {
			for (int y = low[1]; y <= high[1]; y++) {
				for (int x = low[0]; x <= high[0]; x++) {
					const Vec3 center = field.min + Vec3(x, y, z) * field.cell;
					if (triangle_box_overlap(triangle, center, half)) {
						field.cells[size_t(x + field.size[0] * (y + field.size[1] * z))] |= mask;
					}
				}
			}
		}
	}
	field.surface_voxels = int(std::count_if(field.cells.begin(), field.cells.end(), [](uint32_t p_cell) { return p_cell != 0; }));
	if (field.surface_voxels == 0) {
		result.error = MESH_SDF_BAKE_NO_SURFACE;
		return result;
	}
	// Surface occupancy never floods the interior of a room or invents an inside for open sheets.
	field.closed_shell_count = p_mesh.closed_shell_count;
	field.open_shell_count = p_mesh.open_shell_count;
	result.voxelize_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
	return result;
}

static int material_capture_cell(const MaterialCapture &p_capture, const Vec3 &p_point) {
	const Vec3 uvw = p_capture.uvw_offset + p_capture.uvw_basis_x * p_point.x +
			p_capture.uvw_basis_y * p_point.y + p_capture.uvw_basis_z * p_point.z;
	int cell[3];
	for (int axis = 0; axis < 3; axis++) {
		cell[axis] = std::min(p_capture.size[axis] - 1,
				int(std::floor(std::clamp(uvw[axis], 0.0, 1.0) * p_capture.size[axis])));
	}
	const size_t index = size_t(cell[0] + p_capture.size[0] * (cell[1] + p_capture.size[1] * cell[2]));
	if (!p_capture.occupied[index]) {
		return -1;
	}
	return int(index);
}

static Vec3 sample_material_capture(const std::vector<float> &p_values, const MaterialCapture &p_capture, const Vec3 &p_point) {
	const int index = material_capture_cell(p_capture, p_point);
	if (index < 0) {
		return Vec3();
	}
	return Vec3(p_values[size_t(index) * 3], p_values[size_t(index) * 3 + 1], p_values[size_t(index) * 3 + 2]);
}

bool bake_raster_geometry_capture(const std::vector<uint32_t> &p_surface, const int *p_size,
		const MaterialCapture &p_material, const PrimitiveTransform &p_transform, RasterGeometryCapture &r_capture,
		const std::atomic<bool> *p_cancel, int p_threads) {
	int64_t count = 1;
	int64_t material_count = 1;
	for (int axis = 0; axis < 3; axis++) {
		if (p_size[axis] <= 0 || p_material.size[axis] != (p_size[axis] + 1) / 2) {
			return false;
		}
		count *= p_size[axis];
		material_count *= p_material.size[axis];
	}
	if (uint64_t(count) != p_surface.size() || p_material.albedo.size() != uint64_t(material_count) * 3 ||
			(!p_material.emission.empty() && p_material.emission.size() != uint64_t(material_count) * 3) ||
			(!p_material.emission_sides.empty() && p_material.emission_sides.size() != uint64_t(material_count)) ||
			(p_cancel && p_cancel->load())) {
		return false;
	}
	RasterGeometryCapture capture;
	capture.transform = p_transform;
	capture.geometry.min = Vec3(0.5, 0.5, 0.5);
	capture.geometry.cell = 1.0;
	for (int axis = 0; axis < 3; axis++) {
		capture.geometry.size[axis] = p_size[axis];
		capture.material.color_size[axis] = p_material.size[axis];
	}
	capture.geometry.cells = p_surface;
	capture.geometry.surface_voxels = int(std::count_if(p_surface.begin(), p_surface.end(), [](uint32_t p_cell) { return p_cell != 0; }));
	uint64_t signature = 1469598103934665603ull;
	mix_bytes(signature, p_size, 3 * sizeof(int));
	mix_bytes(signature, p_surface.data(), p_surface.size() * sizeof(uint32_t));
	capture.signature = signature;
	if (capture.geometry.surface_voxels == 0) {
		capture.geometry.cells.clear();
		r_capture = std::move(capture);
		return true;
	}
	capture.geometry.open_shell_count = 1;
	capture.material.cell_stride = 2;
	capture.material.albedo.resize(p_material.albedo.size());
	for (size_t value = 0; value < p_material.albedo.size(); value++) {
		capture.material.albedo[value] = uint8_t(js_round(std::clamp(double(p_material.albedo[value]), 0.0, 1.0) * 255.0));
	}
	capture.material.emission = p_material.emission;
	capture.material.emission_sides = p_material.emission_sides;
	r_capture = std::move(capture);
	return true;
}

SurfaceMaterialField bake_mesh_instance_field(const TriangleMesh &p_mesh, const CoarseGeometryField &p_geometry,
		const MaterialCapture *p_material, const std::atomic<bool> *p_cancel, int p_threads, const PrimitiveTransform &p_material_transform) {
	SurfaceMaterialField field;
	if (p_material != nullptr) {
		const int64_t material_count = int64_t(p_material->size[0]) * p_material->size[1] * p_material->size[2];
		if (material_count <= 0 || p_material->albedo.size() != uint64_t(material_count) * 3 ||
				p_material->occupied.size() != uint64_t(material_count) ||
				(!p_material->emission.empty() && p_material->emission.size() != uint64_t(material_count) * 3) ||
				(!p_material->emission_sides.empty() && p_material->emission_sides.size() != uint64_t(material_count))) {
			return field;
		}
	}
	int count = 1;
	for (int axis = 0; axis < 3; axis++) {
		field.color_size[axis] = (p_geometry.size[axis] + field.cell_stride - 1) / field.cell_stride;
		count *= field.color_size[axis];
	}
	field.albedo.assign(size_t(count) * 3, 0);
	const bool has_emission = p_material != nullptr && !p_material->emission.empty();
	if (has_emission) {
		field.emission.assign(size_t(count) * 3, 0.0f);
	}
	if (p_material != nullptr && !p_material->emission_sides.empty()) {
		field.emission_sides.assign(size_t(count), 0);
	}
	std::vector<Vec3> colors(size_t(count), Vec3{});
	std::vector<Vec3> emissions(has_emission ? size_t(count) : 0, Vec3());
	std::vector<uint32_t> contributions(size_t(count), 0);
	const double cell = p_geometry.cell * field.cell_stride;
	const Vec3 origin = p_geometry.min - Vec3(0.5, 0.5, 0.5) * p_geometry.cell;
	struct Vertex {
		Vec3 position;
		Vec3 color;
	};
	for (const MeshTriangle &triangle : p_mesh.triangles) {
		if (p_cancel && p_cancel->load()) {
			return SurfaceMaterialField();
		}
		int low[3];
		int high[3];
		for (int axis = 0; axis < 3; axis++) {
			const double first = std::min(triangle.position[0][axis], std::min(triangle.position[1][axis], triangle.position[2][axis]));
			const double last = std::max(triangle.position[0][axis], std::max(triangle.position[1][axis], triangle.position[2][axis]));
			low[axis] = std::max(0, int(std::ceil((first - origin[axis]) / cell)) - 1);
			high[axis] = std::min(field.color_size[axis] - 1, int(std::floor((last - origin[axis]) / cell)));
		}
		for (int z = low[2]; z <= high[2]; z++) {
			for (int y = low[1]; y <= high[1]; y++) {
				for (int x = low[0]; x <= high[0]; x++) {
					const Vec3 tile_min = origin + Vec3(x, y, z) * cell;
					std::array<Vertex, 12> vertices;
					std::array<Vertex, 12> clipped;
					for (int i = 0; i < 3; i++) {
						vertices[size_t(i)] = { triangle.position[i], triangle.color[i] };
					}
					int vertex_count = 3;
					for (int axis = 0; axis < 3 && vertex_count > 0; axis++) {
						for (int side = 0; side < 2 && vertex_count > 0; side++) {
							const double plane = tile_min[axis] + side * cell;
							int output = 0;
							Vertex previous = vertices[size_t(vertex_count - 1)];
							bool previous_inside = side == 0 ? previous.position[axis] >= plane : previous.position[axis] <= plane;
							for (int i = 0; i < vertex_count; i++) {
								const Vertex current = vertices[size_t(i)];
								const bool current_inside = side == 0 ? current.position[axis] >= plane : current.position[axis] <= plane;
								if (current_inside != previous_inside) {
									const double fraction = (plane - previous.position[axis]) / (current.position[axis] - previous.position[axis]);
									clipped[size_t(output++)] = { previous.position + (current.position - previous.position) * fraction,
										previous.color + (current.color - previous.color) * fraction };
								}
								if (current_inside) {
									clipped[size_t(output++)] = current;
								}
								previous = current;
								previous_inside = current_inside;
							}
							vertex_count = output;
							vertices.swap(clipped);
						}
					}
					if (vertex_count == 0) {
						continue;
					}
					Vec3 point;
					Vec3 color;
					for (int i = 0; i < vertex_count; i++) {
						point = point + vertices[size_t(i)].position / double(vertex_count);
						color = color + vertices[size_t(i)].color / double(vertex_count);
					}
					const int index = x + field.color_size[0] * (y + field.color_size[1] * z);
					if (p_material != nullptr) {
						point = p_material_transform.xform(point);
						color = sample_material_capture(p_material->albedo, *p_material, point);
						if (!field.emission_sides.empty()) {
							const int capture_cell = material_capture_cell(*p_material, point);
							if (capture_cell >= 0) {
								field.emission_sides[size_t(index)] |= p_material->emission_sides[size_t(capture_cell)];
							}
						}
						if (has_emission) {
							emissions[size_t(index)] = emissions[size_t(index)] + sample_material_capture(p_material->emission, *p_material, point);
						}
					}
					colors[size_t(index)] = colors[size_t(index)] + color;
					contributions[size_t(index)]++;
				}
			}
		}
	}
	for (int index = 0; index < count; index++) {
		if (contributions[size_t(index)] == 0) {
			continue;
		}
		const Vec3 color = colors[size_t(index)] / double(contributions[size_t(index)]);
		for (int channel = 0; channel < 3; channel++) {
			field.albedo[size_t(index) * 3 + channel] = uint8_t(js_round(std::clamp(color[channel], 0.0, 1.0) * 255.0));
			if (has_emission) {
				field.emission[size_t(index) * 3 + channel] = float(emissions[size_t(index)][channel] / double(contributions[size_t(index)]));
			}
		}
	}
	return field;
}

} // namespace lrt
