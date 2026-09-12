#include "register_types.h"

#include "lrt_volume.h"
#include "lrt_volume_3d.h"

#ifdef TOOLS_ENABLED
#include "lrt_editor_plugin.h"

#include "editor/plugins/editor_plugin.h"
#endif

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/default_sdf_resolution",
				PROPERTY_HINT_RANGE, "8,256,1,or_greater"), 128);
		GDREGISTER_CLASS(LRTVolume);
		GDREGISTER_CLASS(LRTVolume3D);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<LRTEditorPlugin>();
	}
#endif
}

void uninitialize_lrt_module(ModuleInitializationLevel p_level) {
}
