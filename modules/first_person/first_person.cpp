#include "first_person.h"

#include "core/config/project_settings.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

namespace FirstPerson {
static Mutex mutex;
static HashSet<RID> instances;
static HashMap<RID, CameraSettings> cameras;

void set_instance(RID p_instance, bool p_enabled) {
	MutexLock lock(mutex);
	if (p_enabled) {
		instances.insert(p_instance);
	} else {
		instances.erase(p_instance);
	}
}

bool is_instance(RID p_instance) {
	MutexLock lock(mutex);
	return instances.has(p_instance);
}

void set_camera(RID p_camera, const CameraSettings &p_settings) {
	MutexLock lock(mutex);
	cameras[p_camera] = p_settings;
}

void remove_camera(RID p_camera) {
	MutexLock lock(mutex);
	cameras.erase(p_camera);
}

Vector4 camera_parameters(RID p_camera, float p_world_fov) {
	MutexLock lock(mutex);
	const CameraSettings *settings = cameras.getptr(p_camera);
	if (!settings || !settings->enabled) {
		return Vector4();
	}
	float distance = GLOBAL_GET_CACHED(float, "rendering/lights_and_shadows/first_person_shadow/distance");
	float thickness = GLOBAL_GET_CACHED(float, "rendering/lights_and_shadows/first_person_shadow/thickness");
	bool shadows = GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/first_person_shadow/enabled");
	return Vector4(
			Math::tan(Math::deg_to_rad(p_world_fov * 0.5f)) / Math::tan(Math::deg_to_rad(settings->fov * 0.5f)),
			settings->scale,
			(shadows ? MAX(distance, 0.0f) : 0.0f) * settings->scale,
			MAX(thickness, 0.001f) * settings->scale);
}
} //namespace FirstPerson
