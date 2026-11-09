#pragma once

#include "core/math/vector4.h"
#include "core/templates/rid.h"

// Scene/renderer adapter. RID keys never retain scene objects on the render thread.
namespace FirstPerson {
struct CameraSettings {
	bool enabled = false;
	float fov = 75.0f;
	float scale = 0.1f;
};

void set_instance(RID p_instance, bool p_enabled);
bool is_instance(RID p_instance);
void set_camera(RID p_camera, const CameraSettings &p_settings);
void remove_camera(RID p_camera);
Vector4 camera_parameters(RID p_camera, float p_world_fov);

constexpr uint32_t INSTANCE_FLAG = 1u << 1;
} //namespace FirstPerson
