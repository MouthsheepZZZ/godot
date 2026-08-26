/**************************************************************************/
/*  local_gi_runtime.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/os/semaphore.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/shaders/environment/local_gi/local_gi_transport.glsl.gen.h"

namespace RendererRD {

class LocalGIRuntime {
public:
	struct Triangle {
		float v0[4] = {};
		float v1[4] = {};
		float v2[4] = {};
		float normal[4] = {};
		float albedo[4] = {};
	};

	struct Node {
		float bounds_min[3] = {};
		int32_t left = -1;
		float bounds_max[3] = {};
		int32_t right = -1;
		int32_t first_triangle = -1;
		int32_t triangle_count = 0;
		int32_t pad0 = 0;
		int32_t pad1 = 0;
	};

	struct Light {
		float position_type[4] = {};
		float direction_range[4] = {};
		float intensity_attenuation[4] = {};
		float spot[4] = {};
	};

	struct Input {
		Vector<Node> static_nodes;
		Vector<Triangle> static_triangles;
		Vector<Node> dynamic_nodes;
		Vector<Triangle> dynamic_triangles;
		Vector<Vector3> probe_positions;
		Vector<Vector3> ray_directions;
		Vector<Light> lights;
		Vector<Color> irradiance_history;
		Vector<float> distance_mean_history;
		Vector<float> distance_second_moment_history;
		Vector3i probe_resolution;
		Vector3 volume_size;
		float visibility_bias = 0.0f;
		float temporal_hysteresis = 0.0f;
		int temporal_cursor = 0;
		int update_count = 0;
		bool history_valid = false;
		bool multi_bounce = false;
	};

	struct Output {
		Vector<Color> irradiance_samples;
		Vector<Color> irradiance_estimates;
		Vector<Color> ray_radiances;
		Vector<float> distance_mean_samples;
		Vector<float> distance_second_moment_samples;
		Vector<float> distance_mean_estimates;
		Vector<float> distance_second_moment_estimates;
		Vector<uint8_t> probe_active;
	};

private:
	LocalGiTransportShaderRD *shader = nullptr;
	RID shader_version;
	RID pipeline;
	bool initialized = false;
	const Input *pending_input = nullptr;
	Output *pending_output = nullptr;
	bool pending_result = false;
	Semaphore render_thread_done;

	static RID _create_storage_buffer(const void *p_data, uint32_t p_bytes);
	static void _free_rid(RID &r_rid);
	void _process_render_thread();
	void _free_render_thread();
	static void _process_callback(uint64_t p_instance);
	static void _free_callback(uint64_t p_instance);

public:
	bool ensure_initialized();
	bool process(const Input &p_input, Output &r_output);
	void free();

	~LocalGIRuntime();
};

} // namespace RendererRD
