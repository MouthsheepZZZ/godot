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

#include <atomic>
#include <map>

#include "core/object/ref_counted.h"
#include "core/math/vector3.h"
#include "core/math/vector2i.h"
#include "core/math/vector3i.h"
#include "core/templates/rid.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class Environment;
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

public:
	// Engine-facing geometry inputs. The GDScript setters below build the same records
	// from plain dictionaries; LRTVolume3D builds them straight from MeshInstance3D.
	struct BoxInstance {
		// Analytic backend: the world axis-aligned box (prototype geometry-query.js boxes).
		lrt::Vec3 world_min;
		lrt::Vec3 world_max;
		// SDF backend: the box baked in the instance frame (prototype bakeBoxSDF) plus the
		// world transform that places it (prototype PrimitiveGI).
		lrt::Vec3 local_extent;
		lrt::Vec3 color;
		lrt::PrimitiveTransform transform;
		bool axis_aligned = true;
	};

	// One instance of a mesh asset: the shared local triangle soup plus its world transform.
	struct MeshInstance {
		lrt::PrimitiveTransform transform;
		// Bake cache key for the shared asset (mesh RID + surface); 0 disables sharing.
		int64_t asset_key = 0;
		std::vector<lrt::MeshTriangle> triangles;
	};

	// Plain-data result of the CPU half of the bake, so the whole bake can run on a worker
	// thread without touching engine objects.
	struct LocalBakeResult {
		bool ok = false;
		bool cancelled = false;
		bool needs_axis_aligned = false;
		int solid = 0;
		int surface = 0;
		int receivers = 0;
		int trunks = 0;
		int dirty_trunks = 0;
		int mismatches = 0;
		int mesh_volumes = 0;
		int mesh_triangles = 0;
		double build_ms = 0.0;
		// Phase breakdown of build_ms, kept because N5 tunes these separately.
		double assets_ms = 0.0;
		double local_ms = 0.0;
		double visibility_ms = 0.0;
		double display_ms = 0.0;
		// Derived-cache accounting: how many assets came from disk and how many were baked.
		int assets_loaded = 0;
		int assets_baked = 0;
	};

private:
	lrt::Grid grid;
	std::vector<BoxInstance> box_instances;
	std::vector<MeshInstance> mesh_instances;
	// Per-volume asset cache: one baked Color SDF is shared by every instance of a mesh.
	std::map<int64_t, lrt::ColorSdfField> mesh_sdf_cache;
	// Counters of the bake currently running, reported through LocalBakeResult.
	int assets_loaded = 0;
	int assets_baked = 0;
	lrt::LocalField local;
	lrt::TriangleMesh display_mesh;
	// CPU result of the last bake, waiting for apply_local_field() to upload it.
	lrt::LocalField staged_local;
	// Incremental state of the applied field and of the bake being staged: trunk signatures plus
	// per-probe samples, so an edit only re-solves the trunks it touched.
	lrt::LocalCache local_cache;
	lrt::LocalCache staged_cache;
	lrt::TriangleMesh staged_display_mesh;
	bool has_staged = false;
	std::atomic<bool> cancel_flag{ false };
	String local_backend = "sdf";
	int mesh_sdf_resolution = 128;

	struct Light {
		// Godot light data, in the same shape as the engine's own GI light buffers
		// (servers/rendering/renderer_rd/environment/gi.cpp), so the migrated source term
		// falls off exactly like the light that renders the engine's direct term.
		// type keeps the prototype's numbering: 0 omni, 1 directional, 2 spot.
		int type = 0;
		bool enabled = true;
		bool casts_shadow = true;
		Vector3 position;
		Vector3 direction;
		Vector3 color = Vector3(1, 1, 1);
		// Radiometric scale applied to color: PI * light_energy * light_indirect_energy in
		// Godot's non-physical light units, matching Light3D's direct radiance term.
		float intensity = 0.0f;
		float range = 1.0f;
		float attenuation = 1.0f;
		float spot_angle_deg = 45.0f;
		float spot_attenuation = 1.0f;
	};
	std::vector<Light> lights;
	// Environment radiance replacing the prototype's uniform white sky input.
	Vector3 sky;
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
	bool _build_primitives(const String &p_backend, int p_threads, std::vector<lrt::SdfPrimitive> &r_primitives,
			std::vector<lrt::Box> &r_boxes);
	LocalBakeResult _bake_local_field_data(bool p_analytic);
	void _build_display_mesh();
	int _mesh_instance_count() const;

protected:
	static void _bind_methods();

public:
	LRTVolume();
	~LRTVolume();

	void configure(double p_spacing);
	void configure_with_bounds(double p_spacing, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max);
	void configure_sized(double p_spacing, const Vector3 &p_min, const Vector3 &p_size);
	void configure_sized_with_bounds(double p_spacing, const Vector3 &p_min, const Vector3 &p_size, const Vector3 &p_bounds_min, const Vector3 &p_bounds_max);
	void set_boxes(const Array &p_boxes);
	void set_meshes(const Array &p_meshes);
	void set_box_instances(const std::vector<BoxInstance> &p_boxes);
	void set_mesh_instances(const std::vector<MeshInstance> &p_meshes);
	void set_mesh_sdf_resolution(int p_resolution);
	void set_lights(const Array &p_lights);
	void set_sky(const Vector3 &p_sky);
	void set_multi_bounce(bool p_enabled);
	void set_sh_visibility(bool p_enabled);
	Vector3 read_environment_radiance(const Ref<Environment> &p_environment, const Vector2i &p_size);

	void request_cancel();
	void clear_cancel();
	bool is_cancel_requested() const;
	bool has_local_field() const;
	// CPU-only half of the bake; safe to call on a worker thread. apply_local_field() must
	// run on the main thread afterwards to upload the staged result.
	LocalBakeResult bake_local_field_data(bool p_analytic);
	Dictionary bake_local_field(const String &p_backend);
	Dictionary apply_local_field();
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
