#include "register_types.h"

#include "local_lrt_volume_3d.h"

#include "core/object/class_db.h"

void initialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(LocalLRTVolume3D);
	}
}

void uninitialize_lrt_module(ModuleInitializationLevel p_level) {
}
