/**************************************************************************/
/*  lrt_transfer_baker.cpp                                                */
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


#include "lrt_cache.h"
#include "lrt_transfer_material.h"
#include "lrt_volume_3d.h"

#include "core/config/engine.h"
#include "core/os/os.h"

#include <cstring>

namespace {

void hash_value(uint64_t &r_hash, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		r_hash = (r_hash ^ (p_value & 255u)) * 1099511628211ull;
		p_value >>= 8;
	}
}

void hash_float(uint64_t &r_hash, float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	hash_value(r_hash, bits);
}

struct CaptureIdentities {
	uint64_t geometry;
	uint64_t coverage = 1469598103934665603ull;
	uint64_t reflectance = 1469598103934665603ull;
	uint64_t emission = 1469598103934665603ull;
};

CaptureIdentities capture_identities(const lrt::TransferMaterialCapture &p_capture) {
	CaptureIdentities result;
	result.geometry = lrt::asset_signature(*p_capture.triangles, 1);
	for (const lrt::TransferSurfaceAtlas &atlas : p_capture.surfaces) {
		for (int size : { atlas.width, atlas.height, atlas.tile_size }) {
			hash_value(result.coverage, size);
		}
		for (const lrt::TransferMaterialTexel &texel : atlas.texels) {
			const bool covered = texel.coverage >= 0.99f;
			hash_value(result.coverage, covered);
			if (!covered) {
				continue;
			}
			for (int channel = 0; channel < 3; channel++) {
				hash_float(result.reflectance, texel.reflectance[channel]);
				hash_float(result.reflectance, texel.normal[channel]);
				hash_float(result.emission, texel.emission[channel]);
			}
			hash_value(result.emission, texel.emission_sides);
		}
	}
	return result;
}

} // namespace

namespace lrt {

bool prepare_transfer_bake_input(const Ref<Mesh> &p_mesh, const Vector<Ref<Material>> &p_materials,
		const TransferBakeSettings &p_settings, const Color &p_instance_color, TransferBakeInput &r_input, String &r_error) {
	if (!Engine::get_singleton()->is_editor_hint()) {
		r_error = "Precomputed LRT assets must be explicitly baked in the editor; runtime baking is disabled.";
		return false;
	}
	if (!std::isfinite(p_settings.support_radius) || p_settings.support_radius <= 0.0 ||
			!std::isfinite(p_settings.spacing) || p_settings.spacing <= 0.0 || p_settings.angular_samples < 1) {
		r_error = "LRT support radius, sample spacing and angular sample count must be positive and finite.";
		return false;
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	TransferBakeInput input;
	input.settings = p_settings;
	input.instance_color = p_instance_color;
	if (!capture_transfer_material(p_mesh, p_materials, p_settings.spacing * 0.25, input.capture, r_error, p_instance_color)) {
		return false;
	}
	input.capture_ms = double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
	r_input = std::move(input);
	return true;
}

Dictionary bake_prepared_transfer(const TransferBakeInput &p_input, const std::atomic<bool> *p_cancel, std::atomic<uint64_t> *p_evaluated) {
	Dictionary report;
	report["ok"] = false;
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	const auto cancelled = [p_cancel]() { return p_cancel != nullptr && p_cancel->load(); };
	if (cancelled()) {
		report["cancelled"] = true;
		report["error"] = "LRT transfer bake cancelled.";
		return report;
	}
	const TransferMaterialCapture &capture = p_input.capture;
	const TransferBakeSettings &settings = p_input.settings;
	const CaptureIdentities identities = capture_identities(capture);
	const uint64_t visibility_signature = transfer_visibility_signature(identities.geometry, identities.coverage, settings);
	const uint64_t material_signature = transfer_material_signature(visibility_signature, identities.reflectance, identities.emission);
	const TransferGeometry geometry(capture.triangles);
	const TransferSurfaceQuery surface = [&capture](int p_triangle, const Vec3 &p_point, const Vec3 &p_barycentric, TransferSurface &r_surface) {
		return capture.query(p_triangle, p_point, p_barycentric, r_surface);
	};
	const int threads = CLAMP(OS::get_singleton()->get_processor_count() - 1, 1, 16);
	TransferBakeResult baked = bake_transfer_field(geometry, surface, settings, p_cancel, threads, p_evaluated);
	if (baked.error != TRANSFER_BAKE_OK) {
		report["cancelled"] = baked.error == TRANSFER_BAKE_CANCELLED;
		report["error"] = vformat("LRT transfer bake failed (error %d).", int(baked.error));
		return report;
	}
	if (cancelled()) {
		report["cancelled"] = true;
		report["error"] = "LRT transfer bake cancelled.";
		return report;
	}
	if (!store_transfer_field(visibility_signature, true, baked.field) ||
			!store_transfer_field(material_signature, false, baked.field) ||
			!store_transfer_coverage(visibility_signature, capture.coverage_snapshot())) {
		report["error"] = "Cannot persist the LRT transfer dependencies; check cache permissions and FP16 coefficient range.";
		return report;
	}
	if (cancelled()) {
		report["cancelled"] = true;
		report["error"] = "LRT transfer bake cancelled; no new manifest was published.";
		return report;
	}
	report["ok"] = true;
	report["geometry_signature"] = int64_t(identities.geometry);
	report["coverage_signature"] = int64_t(identities.coverage);
	report["reflectance_signature"] = int64_t(identities.reflectance);
	report["emission_signature"] = int64_t(identities.emission);
	report["specification_signature"] = int64_t(transfer_bake_signature(settings));
	report["visibility_signature"] = int64_t(visibility_signature);
	report["material_signature"] = int64_t(material_signature);
	report["visibility_path"] = transfer_cache_path(visibility_signature, true);
	report["material_path"] = transfer_cache_path(material_signature, false);
	report["packed_visibility_path"] = packed_transfer_path(visibility_signature, true);
	report["packed_material_path"] = packed_transfer_path(material_signature, false);
	report["coverage_path"] = transfer_coverage_path(visibility_signature);
	report["packed_coverage_path"] = packed_transfer_coverage_path(visibility_signature);
	report["instance_color"] = p_input.instance_color;
	report["support_radius"] = settings.support_radius;
	report["sample_spacing"] = settings.spacing;
	report["angular_samples"] = settings.angular_samples;
	report["evaluated_samples"] = int64_t(baked.evaluated_samples);
	report["stored_samples"] = int64_t(baked.field.sample_count());
	report["visibility_bytes"] = int64_t(baked.field.sample_count() * 8);
	report["material_bytes"] = int64_t(baked.field.sample_count() * 120);
	report["bake_ms"] = p_input.capture_ms + double(OS::get_singleton()->get_ticks_usec() - start) / 1000.0;
	return report;
}

Dictionary bake_transfer_instance(const TransferInstanceBake &p_input, const std::atomic<bool> *p_cancel,
		std::atomic<uint64_t> *p_evaluated, std::atomic<int> *p_completed_variants) {
	Array variants;
	double bake_ms = 0.0;
	for (const TransferBakeInput &input : p_input.variants) {
		const Dictionary baked = bake_prepared_transfer(input, p_cancel, p_evaluated);
		if (!bool(baked.get("ok", false))) {
			return baked;
		}
		Dictionary variant;
		variant["color"] = input.instance_color;
		for (const char *key : { "geometry_signature", "coverage_signature", "reflectance_signature", "emission_signature",
				 "specification_signature", "visibility_signature", "material_signature" }) {
			variant[key] = baked[key];
		}
		variants.push_back(variant);
		bake_ms += double(baked["bake_ms"]);
		if (p_completed_variants != nullptr) {
			p_completed_variants->fetch_add(1);
		}
	}
	Dictionary manifest;
	manifest["version"] = 2;
	manifest["input_signature"] = int64_t(p_input.input_signature);
	manifest["variants"] = variants;
	Dictionary report;
	report["ok"] = true;
	report["manifest"] = manifest;
	report["variant_count"] = variants.size();
	report["bake_ms"] = bake_ms;
	return report;
}

} // namespace lrt

Dictionary LRTVolume3D::prepare_transfer_mesh(const Ref<Mesh> &p_mesh, const TypedArray<Material> &p_materials,
		double p_support_radius, double p_sample_spacing, int p_angular_samples, const Color &p_instance_color) {
	Vector<Ref<Material>> materials;
	for (int i = 0; i < p_materials.size(); i++) {
		materials.push_back(p_materials[i]);
	}
	lrt::TransferBakeSettings settings;
	settings.support_radius = p_support_radius;
	settings.spacing = p_sample_spacing;
	settings.angular_samples = p_angular_samples;
	lrt::TransferBakeInput input;
	String error;
	if (!lrt::prepare_transfer_bake_input(p_mesh, materials, settings, p_instance_color, input, error)) {
		Dictionary report;
		report["ok"] = false;
		report["error"] = error;
		return report;
	}
	return lrt::bake_prepared_transfer(input);
}
