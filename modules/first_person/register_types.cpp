#include "register_types.h"

#include "core/config/project_settings.h"

void initialize_first_person_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	GLOBAL_DEF_BASIC("rendering/lights_and_shadows/first_person_shadow/enabled", true);
	GLOBAL_DEF_BASIC("rendering/lights_and_shadows/first_person_shadow/half_resolution", true);
	GLOBAL_DEF_BASIC("rendering/lights_and_shadows/first_person_shadow/block_skipping", true);
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::FLOAT, "rendering/lights_and_shadows/first_person_shadow/distance", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), 1.0);
	GLOBAL_DEF_BASIC(PropertyInfo(Variant::FLOAT, "rendering/lights_and_shadows/first_person_shadow/thickness", PROPERTY_HINT_RANGE, "0.001,1,0.001,or_greater,suffix:m"), 0.05);
}

void uninitialize_first_person_module(ModuleInitializationLevel p_level) {
}
