/**************************************************************************/
/*  lrt_render_bridge.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "lrt_render_bridge.h"

namespace {

LRTRenderBridge::State lrt_render_state;

} // namespace

void LRTRenderBridge::set_state(const Dictionary &p_state) {
	State next;
	next.owner = ObjectID(uint64_t(p_state.get("owner", uint64_t(0))));
	next.world_to_volume = p_state.get("world_to_volume", Transform3D());
	next.volume_min = p_state.get("volume_min", Vector3());
	next.volume_max = p_state.get("volume_max", Vector3());
	next.grid_min = p_state.get("grid_min", Vector3());
	next.grid_size = p_state.get("grid_size", Vector3i());
	next.atlas_size = p_state.get("atlas_size", Vector2(1.0, 1.0));
	next.sky_color = p_state.get("sky_color", Vector3());
	next.spacing = float(p_state.get("spacing", 0.25));
	next.mode = int(p_state.get("mode", 0));
	next.blur_sampling = p_state.get("blur_sampling", true);
	next.enabled = p_state.get("enabled", false);
	next.radiance_r = p_state.get("radiance_r", RID());
	next.radiance_g = p_state.get("radiance_g", RID());
	next.radiance_b = p_state.get("radiance_b", RID());
	next.visibility = p_state.get("visibility", RID());
	next.material = p_state.get("material", RID());
	next.links = p_state.get("links", RID());
	next.revision = lrt_render_state.revision + 1;
	lrt_render_state = next;
}

void LRTRenderBridge::clear(ObjectID p_owner) {
	if (lrt_render_state.owner != p_owner) {
		return;
	}
	const uint64_t next_revision = lrt_render_state.revision + 1;
	lrt_render_state = State();
	lrt_render_state.revision = next_revision;
}

const LRTRenderBridge::State &LRTRenderBridge::get_state() {
	return lrt_render_state;
}
