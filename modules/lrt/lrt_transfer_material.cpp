/**************************************************************************/
/*  lrt_transfer_material.cpp                                             */
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

#include "lrt_transfer_material.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/rendering_server.h"

#include <algorithm>

namespace lrt {
namespace {

size_t triangle_texel(int p_width, int p_height, int p_tile_size, int p_triangle, const Vec3 &p_barycentric) {
	const int columns = p_width / p_tile_size;
	const double x = (p_triangle % columns) * p_tile_size + 2.0 + p_barycentric.y * (p_tile_size - 4);
	const double y = (p_triangle / columns) * p_tile_size + 2.0 + p_barycentric.z * (p_tile_size - 4);
	const int pixel_x = std::clamp(int(std::floor(x)), 0, p_width - 1);
	const int pixel_y = std::clamp(int(std::floor(y)), 0, p_height - 1);
	return size_t(pixel_x) + size_t(p_width) * pixel_y;
}

} // namespace

bool TransferSurfaceAtlas::query(int p_triangle, const Vec3 &p_barycentric, TransferSurface &r_surface) const {
	const TransferMaterialTexel &texel = texels[triangle_texel(width, height, tile_size, p_triangle, p_barycentric)];
	// Same effective opaque-surface convention as the existing material voxel pass.
	// Alpha-scissor/hash materials produce binary alpha in the material renderer.
	if (texel.coverage < 0.99f) {
		return false;
	}
	const bool front = dot(r_surface.normal, geometric_normals[p_triangle]) >= 0.0;
	Vec3 normal(texel.normal[0], texel.normal[1], texel.normal[2]);
	if (dot(normal, r_surface.normal) < 0.0) {
		normal = -normal;
	}
	r_surface.normal = normal;
	r_surface.reflectance = Vec3(texel.reflectance[0], texel.reflectance[1], texel.reflectance[2]);
	r_surface.emission = Vec3();
	const uint8_t side = front ? EMISSION_FRONT : EMISSION_BACK;
	if ((texel.emission_sides & side) != 0) {
		r_surface.emission = Vec3(texel.emission[0], texel.emission[1], texel.emission[2]);
	}
	return true;
}

bool TransferMaterialCapture::query(int p_triangle, const Vec3 &p_point, const Vec3 &p_barycentric, TransferSurface &r_surface) const {
	const auto &mapping = triangle_mapping[p_triangle];
	return surfaces[mapping.first].query(mapping.second, p_barycentric, r_surface);
}

bool TransferCoverage::query(int p_triangle, const Vec3 &p_point, const Vec3 &p_barycentric, TransferSurface &r_surface) const {
	const auto &mapping = triangle_mapping[p_triangle];
	const TransferCoverageAtlas &atlas = surfaces[mapping.first];
	return atlas.covered[triangle_texel(atlas.width, atlas.height, atlas.tile_size, mapping.second, p_barycentric)] != 0;
}

bool TransferCoverage::occupied(const Vec3 &p_point, double p_half_width) const {
	return geometry->within_surface_band(p_point, p_half_width,
			[this](int p_triangle, const Vec3 &p_position, const Vec3 &p_barycentric, TransferSurface &r_surface) {
				return query(p_triangle, p_position, p_barycentric, r_surface);
			});
}

TransferCoverage TransferMaterialCapture::coverage_snapshot() const {
	TransferCoverage result;
	result.triangles = triangles;
	result.triangle_mapping = triangle_mapping;
	result.geometry = std::make_shared<TransferGeometry>(triangles);
	for (const TransferSurfaceAtlas &surface : surfaces) {
		TransferCoverageAtlas atlas;
		atlas.width = surface.width;
		atlas.height = surface.height;
		atlas.tile_size = surface.tile_size;
		atlas.covered.reserve(surface.texels.size());
		for (const TransferMaterialTexel &texel : surface.texels) {
			atlas.covered.push_back(texel.coverage >= 0.99f);
		}
		result.surfaces.push_back(std::move(atlas));
	}
	return result;
}

namespace {

class TransferAtlasReadback : public RefCounted {
	GDCLASS(TransferAtlasReadback, RefCounted);

protected:
	static void _bind_methods() {}

public:
	Ref<ArrayMesh> mesh;
	TypedArray<Image> images;
	Size2i size;
	uint32_t tile_size = 0;

	void capture() {
		RendererSceneRenderRD *renderer = RendererSceneRenderRD::get_singleton();
		if (renderer != nullptr) {
			images = renderer->bake_render_triangle_material(mesh->get_rid(), {}, size, tile_size);
		}
	}
};

Vec3 from_vector(const Vector3 &p_value) {
	return Vec3(p_value.x, p_value.y, p_value.z);
}

bool decode_atlas(const TypedArray<Image> &p_images, TransferSurfaceAtlas &r_atlas, String &r_error) {
	if (p_images.size() != 4) {
		r_error = "LRT triangle material capture requires a RenderingDevice renderer and valid atlas dimensions.";
		return false;
	}
	const Ref<Image> albedo = p_images[0];
	const Ref<Image> normal = p_images[1];
	const Ref<Image> orm = p_images[2];
	const Ref<Image> emission = p_images[3];
	for (const Ref<Image> &image : { albedo, normal, orm, emission }) {
		if (image.is_null() || image->is_empty() || image->get_width() != r_atlas.width || image->get_height() != r_atlas.height) {
			r_error = "LRT triangle material capture returned missing or mismatched images.";
			return false;
		}
	}
	r_atlas.texels.resize(size_t(r_atlas.width) * r_atlas.height);
	for (int y = 0; y < r_atlas.height; y++) {
		for (int x = 0; x < r_atlas.width; x++) {
			const Color color = albedo->get_pixel(x, y);
			TransferMaterialTexel &texel = r_atlas.texels[size_t(x) + size_t(r_atlas.width) * y];
			texel.coverage = color.a;
			if (color.a < 0.99f) {
				continue;
			}
			const Color encoded_normal = normal->get_pixel(x, y);
			const Vec3 raw_normal(encoded_normal.r * 2.0 - 1.0, encoded_normal.g * 2.0 - 1.0, encoded_normal.b * 2.0 - 1.0);
			if (length_squared(raw_normal) == 0.0) {
				r_error = "LRT triangle material capture returned a zero surface normal.";
				return false;
			}
			const Vec3 decoded_normal = normalized(raw_normal);
			const Color emitted = emission->get_pixel(x, y);
			const float diffuse = 1.0f - orm->get_pixel(x, y).b;
			for (int channel = 0; channel < 3; channel++) {
				texel.reflectance[channel] = color[channel] * diffuse;
				texel.normal[channel] = float(decoded_normal[channel]);
				texel.emission[channel] = emitted[channel];
			}
			texel.emission_sides = uint8_t(std::lround(encoded_normal.a * 3.0f));
		}
	}
	return true;
}

} // namespace

bool capture_transfer_material(const Ref<Mesh> &p_mesh, const Vector<Ref<Material>> &p_materials,
		double p_texel_size, TransferMaterialCapture &r_capture, String &r_error, const Color &p_instance_color) {
	if (p_mesh.is_null() || !std::isfinite(p_texel_size) || p_texel_size <= 0.0 ||
			p_materials.size() != p_mesh->get_surface_count()) {
		r_error = "LRT triangle material capture requires a mesh, one effective material per surface, and positive texel size.";
		return false;
	}
	TransferMaterialCapture result;
	auto triangles = std::make_shared<std::vector<MeshTriangle>>();
	for (int surface = 0; surface < p_mesh->get_surface_count(); surface++) {
		if (p_materials[surface].is_null()) {
			continue;
		}
		if (p_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			r_error = "LRT precomputed transfer requires triangle surfaces.";
			return false;
		}
		Ref<SurfaceTool> tool;
		tool.instantiate();
		tool->create_from(p_mesh, surface);
		tool->deindex();
		Array arrays = tool->commit_to_arrays();
		arrays[Mesh::ARRAY_BONES] = Variant();
		arrays[Mesh::ARRAY_WEIGHTS] = Variant();
		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		if (p_instance_color != Color(1, 1, 1, 1)) {
			PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
			if (colors.is_empty()) {
				colors.resize(positions.size());
				colors.fill(p_instance_color);
			} else {
				for (int i = 0; i < colors.size(); i++) {
					colors.set(i, colors[i] * p_instance_color);
				}
			}
			arrays[Mesh::ARRAY_COLOR] = colors;
		}
		if (positions.is_empty() || positions.size() % 3 != 0) {
			r_error = "LRT triangle material capture received invalid triangle arrays.";
			return false;
		}
		const int triangle_count = positions.size() / 3;
		TransferSurfaceAtlas atlas;
		double longest_edge = 0.0;
		for (int i = 0; i < triangle_count; i++) {
			MeshTriangle triangle;
			for (int vertex = 0; vertex < 3; vertex++) {
				triangle.position[vertex] = from_vector(positions[i * 3 + vertex]);
				longest_edge = std::max(longest_edge, double(positions[i * 3 + vertex].distance_to(positions[i * 3 + (vertex + 1) % 3])));
			}
			const Vec3 normal = cross(triangle.position[2] - triangle.position[0], triangle.position[1] - triangle.position[0]);
			atlas.geometric_normals.push_back(length_squared(normal) == 0.0 ? Vec3() : normalized(normal));
			triangles->push_back(triangle);
			result.triangle_mapping.emplace_back(int(result.surfaces.size()), i);
		}
		const double tile_size = std::max(8.0, std::ceil(longest_edge / p_texel_size) + 4.0);
		const int columns = int(std::ceil(std::sqrt(double(triangle_count))));
		const int rows = (triangle_count + columns - 1) / columns;
		if (!std::isfinite(tile_size) || tile_size * std::max(columns, rows) > INT32_MAX) {
			r_error = "LRT triangle material atlas is too large; increase the sampling spacing or split the mesh.";
			return false;
		}
		atlas.tile_size = int(tile_size);
		atlas.width = columns * atlas.tile_size;
		atlas.height = rows * atlas.tile_size;
		Ref<TransferAtlasReadback> readback;
		readback.instantiate();
		readback->mesh.instantiate();
		readback->mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		readback->mesh->surface_set_material(0, p_materials[surface]);
		readback->size = Size2i(atlas.width, atlas.height);
		readback->tile_size = atlas.tile_size;
		BaseMaterial3D::flush_changes();
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp(readback.ptr(), &TransferAtlasReadback::capture));
		RenderingServer::get_singleton()->sync();
		if (!decode_atlas(readback->images, atlas, r_error)) {
			return false;
		}
		result.surfaces.push_back(std::move(atlas));
	}
	if (triangles->empty()) {
		r_error = "LRT precomputed transfer has no contributing triangle surfaces.";
		return false;
	}
	result.triangles = std::move(triangles);
	r_capture = std::move(result);
	return true;
}

} // namespace lrt
