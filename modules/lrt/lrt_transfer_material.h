/**************************************************************************/
/*  lrt_transfer_material.h                                               */
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

#include "lrt_transfer.h"

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

namespace lrt {

struct TransferMaterialTexel {
	float reflectance[3] = {};
	float normal[3] = {};
	float emission[3] = {};
	float coverage = 0.0f;
	uint8_t emission_sides = EMISSION_FRONT;
};

// Plain CPU snapshot; workers never touch Image, Mesh, Material, or renderer objects.
struct TransferSurfaceAtlas {
	int width = 0;
	int height = 0;
	int tile_size = 0;
	std::vector<TransferMaterialTexel> texels;
	std::vector<Vec3> geometric_normals;

	bool query(int p_triangle, const Vec3 &p_barycentric, TransferSurface &r_surface) const;
};

struct TransferMaterialCapture {
	std::vector<TransferSurfaceAtlas> surfaces;
	std::vector<std::pair<int, int>> triangle_mapping;
	std::shared_ptr<const std::vector<MeshTriangle>> triangles;

	bool query(int p_triangle, const Vec3 &p_point, const Vec3 &p_barycentric, TransferSurface &r_surface) const;
	TransferCoverage coverage_snapshot() const;
};

struct TransferBakeInput {
	TransferMaterialCapture capture;
	TransferBakeSettings settings;
	Color instance_color = Color(1, 1, 1, 1);
	double capture_ms = 0.0;
};

struct TransferInstanceBake {
	uint64_t input_signature = 0;
	std::vector<TransferBakeInput> variants;
};

bool prepare_transfer_bake_input(const Ref<Mesh> &p_mesh, const Vector<Ref<Material>> &p_materials,
		const TransferBakeSettings &p_settings, const Color &p_instance_color, TransferBakeInput &r_input, String &r_error);
Dictionary bake_prepared_transfer(const TransferBakeInput &p_input, const std::atomic<bool> *p_cancel = nullptr, std::atomic<uint64_t> *p_evaluated = nullptr);
Dictionary bake_transfer_instance(const TransferInstanceBake &p_input, const std::atomic<bool> *p_cancel = nullptr,
		std::atomic<uint64_t> *p_evaluated = nullptr, std::atomic<int> *p_completed_variants = nullptr);

// Main-thread preparation. Effective static materials have already been filtered by
// the caller. Each source surface is captured with procedural atlas raster coordinates
// so shader UV/UV2, tangents, vertex colors, and normal maps remain authored inputs.
bool capture_transfer_material(const Ref<Mesh> &p_mesh, const Vector<Ref<Material>> &p_materials,
		double p_texel_size, TransferMaterialCapture &r_capture, String &r_error, const Color &p_instance_color = Color(1, 1, 1, 1));

} // namespace lrt
