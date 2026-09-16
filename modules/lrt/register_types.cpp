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
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/sdf/default_resolution",
				PROPERTY_HINT_RANGE, "8,256,1"), 128);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/cache/memory_budget_mb",
				PROPERTY_HINT_RANGE, "16,4096,1,suffix:MiB"), 256);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/propagation/frames_to_converge",
				PROPERTY_HINT_ENUM, "6 Frames:6,12 Frames:12,18 Frames:18,24 Frames:24,32 Frames:32"), 18);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/dynamic_objects/update_interval",
				PROPERTY_HINT_ENUM, "Every Frame:1,Every 2 Frames:2,Every 4 Frames:4,Every 8 Frames:8"), 1);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/limits/max_volume_gpu_memory_mb",
				PROPERTY_HINT_RANGE, "64,8192,1,suffix:MiB"), 512);
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
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		LRTVolume3D::clear_shared_mesh_capture_cache();
		LRTVolume::free_shared_gpu_resources();
	}
}
