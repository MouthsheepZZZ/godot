#include "register_types.h"

#include "lrt_volume.h"

#include "core/object/class_db.h"

void initialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(LRTVolume);
	}
}

void uninitialize_lrt_module(ModuleInitializationLevel p_level) {
}
