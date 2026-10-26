/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "register_types.h"

#include "local_lrt_volume_3d.h"
#include "lrt_data.h"
#include "lrt_geometry_binding.h"
#include "lrt_renderer.h"
#include "lrt_runtime.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/lrt_editor_plugin.h"
#endif

void initialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(LocalLRTVolumeData);
		GDREGISTER_CLASS(LRTInstanceGeometryData);
		GDREGISTER_CLASS(LocalLRTVolume3D);
		GDREGISTER_CLASS(LRTGeometryBinding);
		GDREGISTER_INTERNAL_CLASS(LRTRuntime);
		GDREGISTER_INTERNAL_CLASS(LRTRenderer);

		GLOBAL_DEF_RST("rendering/lrt/enabled", false);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/lrt/propagation_iterations", PROPERTY_HINT_RANGE, "1,16,1,or_greater"), 2);
		GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/lrt/propagation_relaxation", PROPERTY_HINT_RANGE, "0.001,1,0.001"), 0.5);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/lrt/geometry_trunks_per_frame", PROPERTY_HINT_RANGE, "1,128,1,or_greater"), 8);
		GLOBAL_DEF("rendering/lrt/screen_gather_enabled", true);
		GLOBAL_DEF(PropertyInfo(Variant::STRING, "rendering/lrt/screen_gather_scale", PROPERTY_HINT_ENUM, "1,0.5,0.25"), "0.5");
		memnew(LRTRuntime);
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<LRTEditorPlugin>();
	}
#endif
}

void uninitialize_lrt_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		memdelete(LRTRuntime::get_singleton());
	}
}
