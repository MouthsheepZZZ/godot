/**************************************************************************/
/*  lrt_occupancy.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).   */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                     */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining    */
/* a copy of this software and associated documentation files (the          */
/* "Software"), to deal in the Software without restriction, including     */
/* without limitation the rights to use, copy, modify, merge, publish,      */
/* distribute, sublicense, and/or sell copies of the Software, and to       */
/* permit persons to whom the Software is furnished to do so, subject to    */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be          */
/* included in all copies or substantial portions of the Software.         */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,          */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF      */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,     */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                  */
/**************************************************************************/

#include "lrt_core.h"
#include "lrt_transfer.h"

#include <algorithm>

namespace lrt {

int ProbeOccupancy::find_cell(int p_probe) const {
	const auto found = std::lower_bound(cells.begin(), cells.end(), p_probe,
			[](const std::pair<int, int> &p_cell, int p_index) { return p_cell.first < p_index; });
	return found != cells.end() && found->first == p_probe ? found->second : -1;
}

bool TrunkIndex::prepare_probe_occupancy(const Grid &p_grid, const std::vector<GeometryInstance> &p_primitives,
		const std::atomic<bool> *p_cancel, int p_threads) {
	update_stats.occupancy_built = update_stats.occupancy_reused = 0;
	update_stats.occupancy_candidates = 0;
	if (p_cancel && p_cancel->load()) {
		return false;
	}
	struct Job {
		Registration *registration;
		const GeometryInstance *instance;
		std::shared_ptr<ProbeOccupancy> result;
		uint64_t candidates = 0;
	};
	std::vector<Job> jobs;
	for (auto &entry : registrations) {
		Registration &registration = entry.second;
		const GeometryInstance &instance = p_primitives[size_t(registration.inputs[0])];
		if (!instance.precomputed && !instance.static_triangles) {
			continue;
		}
		if (registration.occupancy) {
			update_stats.occupancy_reused++;
			continue;
		}
		jobs.push_back({ &registration, &instance, std::make_shared<ProbeOccupancy>(), 0 });
	}
	parallel_for(int(jobs.size()), p_threads, [&](int p_job) {
		Job &job = jobs[size_t(p_job)];
		const GeometryInstance &instance = *job.instance;
		if (instance.determinant == 0.0) {
			return;
		}
		Vec3 band_extent;
		if (instance.precomputed) {
			const double half_width = instance.occupancy_band_width * 0.5;
			band_extent = instance.mesh_to_volume.xform_extent(Vec3(half_width, half_width, half_width));
		}
		int low[3];
		int high[3];
		for (int axis = 0; axis < 3; axis++) {
			const double first = std::ceil(std::nextafter((instance.bounds_min[axis] - band_extent[axis] - p_grid.min[axis]) / p_grid.spacing - 0.5, -INFINITY));
			const double last = std::floor(std::nextafter((instance.bounds_max[axis] + band_extent[axis] - p_grid.min[axis]) / p_grid.spacing - 0.5, INFINITY));
			if (last < 0 || first >= p_grid.size[axis]) {
				return;
			}
			low[axis] = int(std::max(0.0, first));
			high[axis] = int(std::min(double(p_grid.size[axis] - 1), last));
		}
		for (int y = low[1]; y <= high[1]; y++) {
			for (int z = low[2]; z <= high[2]; z++) {
				if (p_cancel && p_cancel->load()) {
					return;
				}
				for (int x = low[0]; x <= high[0]; x++) {
					const Vec3 point = probe_point(p_grid, x, y, z);
					if (instance.precomputed) {
						job.candidates++;
						if (instance.occupies_probe(point)) {
							job.result->cells.emplace_back(index_of(p_grid, x, y, z), 0);
						}
						continue;
					}
					const SurfaceSample sample = instance.sample(point);
					if (!sample.valid) {
						continue;
					}
					job.candidates++;
					job.result->cells.emplace_back(index_of(p_grid, x, y, z), sample.cell);
				}
			}
		}
	});
	if (p_cancel && p_cancel->load()) {
		return false;
	}
	for (Job &job : jobs) {
		job.registration->occupancy = std::move(job.result);
		update_stats.occupancy_built++;
		update_stats.occupancy_candidates += job.candidates;
	}
	probe_occupancy.assign(p_primitives.size(), nullptr);
	for (const auto &entry : registrations) {
		for (int input : entry.second.inputs) {
			probe_occupancy[size_t(input)] = entry.second.occupancy.get();
		}
	}
	return true;
}

} // namespace lrt
