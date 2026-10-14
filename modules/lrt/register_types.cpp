#include "register_types.h"

#include "lrt_volume.h"
#include "lrt_volume_3d.h"
#include "lrt_render_bridge.h"

#ifdef TOOLS_ENABLED
#include "lrt_editor_plugin.h"
#include "lrt_export_plugin.h"

#include "editor/plugins/editor_plugin.h"
#endif

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

void initialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/screen_gather/resolution",
				PROPERTY_HINT_ENUM, "25% Pixels (Half Width and Height):0,100% Pixels:1"), 0);
		GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/propagation/sampling",
				PROPERTY_HINT_ENUM, "Full 26:0,Four Point Dithered:1"), 1);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/sdf/target_cell_size",
				PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), 0.125);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/sdf/minimum_feature_size_percent",
				PROPERTY_HINT_RANGE, "0,100,0.01,suffix:%"), 0.78125);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/cache/memory_budget_mb",
				PROPERTY_HINT_RANGE, "16,4096,1,suffix:MiB"), 256);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/propagation/update_budget_ms",
				PROPERTY_HINT_RANGE, "0.01,16,0.01,suffix:ms"), 0.5);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/propagation/response_frames",
				PROPERTY_HINT_ENUM, "6 Frames:6,12 Frames:12,18 Frames:18,24 Frames:24,32 Frames:32"), 18);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/propagation/display_response_time",
				PROPERTY_HINT_RANGE, "0,1,0.01,suffix:s"), 0.15);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/dynamic_objects/cpu_budget_ms",
				PROPERTY_HINT_RANGE, "0.01,16,0.01,suffix:ms"), 0.5);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/dynamic_objects/update_interval",
				PROPERTY_HINT_ENUM, "Every Frame:1,Every 2 Frames:2,Every 4 Frames:4,Every 8 Frames:8"), 1);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/limits/max_volume_gpu_memory_mb",
				PROPERTY_HINT_RANGE, "64,8192,1,suffix:MiB"), 512);
		GDREGISTER_CLASS(LRTVolume);
		GDREGISTER_CLASS(LRTVolume3D);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(LRTExportPlugin);
		EditorPlugins::add_by_type<LRTEditorPlugin>();
	}
#endif
}

void uninitialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		LRTVolume3D::clear_shared_mesh_capture_cache();
		LRTRenderBridge::free_external_gi_resources();
		LRTVolume::free_shared_gpu_resources();
	}
}
