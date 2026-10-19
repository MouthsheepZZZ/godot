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
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/screen_gather/normal_bias",
				PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), 0.1);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/voxel/cell_size",
				PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), 0.125);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/directional_shadow/resolution",
				PROPERTY_HINT_RANGE, "64,2048,1"), 512);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/global_illumination/lrt/directional_shadow/bias",
				PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), 0.02);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/propagation/iterations_per_frame",
				PROPERTY_HINT_RANGE, "1,64,1,or_greater"), 1);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/global_illumination/lrt/propagation/iterations_after_change",
				PROPERTY_HINT_RANGE, "1,1024,1,or_greater"), 64);
		GDREGISTER_CLASS(LRTVolume);
		GDREGISTER_CLASS(LRTVolume3D);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(LRTVoxelCellSizeEditor);
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
