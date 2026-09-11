/**************************************************************************/
/*  lrt_volume.h                                                          */
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

#include "core/object/ref_counted.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/rid.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class RenderingDevice;
class Image;
class ImageTexture;
class Texture2D;

// N1 driver for the Local Radiance Transfer core.
//
// The numeric algorithm lives in lrt_core.cpp (local field) and the two compute
// shaders (injection, propagation). This class only adapts platform APIs and data
// layout: analytic box inputs from the scene, RenderingDevice storage buffers, and
// CPU-visible copies of the production fields for display and parity checks.
class LRTVolume : public RefCounted {
	GDCLASS(LRTVolume, RefCounted);

	lrt::Grid grid;
	std::vector<lrt::Box> boxes;
	std::vector<lrt::MeshTriangle> mesh_triangles;
	std::vector<lrt::TriangleMesh> mesh_volumes;
	lrt::TriangleMesh display_mesh;
	lrt::LocalField local;
	String local_backend = "sdf";
	int mesh_sdf_resolution = 128;

	struct Light {
		int type = 0;
		bool enabled = true;
		Vector3 position;
		Vector3 direction;
		Vector3 color = Vector3(1, 1, 1);
		float power = 0.0f;
		float angle_deg = 35.0f;
	};
	std::vector<Light> lights;
	float sky = 0.0f;
	bool multi_bounce = true;
	bool sh_visibility = true;
	bool configured = false;
	bool has_local = false;

	RenderingDevice *device = nullptr;
	RID shader_inject;
	RID shader_propagate;
	RID pipeline_inject;
	RID pipeline_propagate;
	RID params_buffer;
	RID material_buffer;
	RID links_buffer;
	RID matrix_buffer;
	RID local_visibility_buffer;
	RID receiver_buffer;
	RID mesh_node_buffer;
	RID mesh_triangle_buffer;
	RID mesh_material_buffer;
	RID source_buffers[3];
	RID radiance_buffers[2][3];
	RID visibility_buffers[2];
	RID uniform_set_inject;
	RID uniform_set_propagate[2];
	int current = 0;
	int iteration = 0;
	double last_gpu_ms = 0.0;

	// CPU-visible copies of the production fields (always the current A/B buffer).
	std::vector<float> radiance_cpu[3];
	std::vector<float> visibility_cpu;
	std::vector<float> source_cpu[3];
	Ref<Image> field_images[3];
	Ref<Image> visibility_image;
	Ref<Image> source_images[3];
	Ref<Image> material_image;
	Ref<Image> matrix_image;
	Ref<Image> local_visibility_image;
	Ref<ImageTexture> field_textures[3];
	Ref<ImageTexture> visibility_texture;
	Ref<ImageTexture> source_textures[3];
	Ref<ImageTexture> material_texture;
	Ref<ImageTexture> matrix_texture;
	Ref<ImageTexture> local_visibility_texture;

	Error _ensure_device();
	Error _create_shaders();
	Error _create_buffers();
	Error _create_uniform_sets();
	void _free_gpu_resources();
	bool _upload_params();
	void _upload_local_buffers();
	Error _read_back_fields();

protected:
	static void _bind_methods();

public:
	LRTVolume();
	~LRTVolume();

	void configure(double p_spacing);
	void configure_with_bounds(double p_spacing, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max);
	void set_boxes(const Array &p_boxes);
	void set_meshes(const Array &p_meshes);
	void set_mesh_sdf_resolution(int p_resolution);
	void set_lights(const Array &p_lights);
	void set_sky(double p_sky);
	void set_multi_bounce(bool p_enabled);
	void set_sh_visibility(bool p_enabled);

	Dictionary build_local_field(const String &p_backend);
	void inject();
	void step(int p_iterations);
	void reset();
	int get_iteration() const;
	Dictionary get_grid() const;

	void refresh_display();
	Ref<Texture2D> get_texture(const String &p_name) const;
	PackedFloat32Array read_field(const String &p_name) const;
	PackedInt32Array read_links() const;
	Dictionary get_mesh_bvh() const;
	Dictionary get_stats() const;
};
