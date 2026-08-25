/**************************************************************************/
/*  local_dynamic_gi.cpp                                                  */
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

#include "core/error/error_macros.h"
#include "core/math/aabb.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/rid_owner.h"
#include "core/templates/vector.h"
#include "servers/rendering/environment/renderer_gi.h"

namespace {
struct LocalDynamicGI {
	bool enabled = true;
	Vector3 extend = Vector3(0.25, 0.25, 0.25);
	float blend_distance = 0.5;
	AABB local_bounds;
	Transform3D transform;
	RID scenario;
};

RID_Owner<LocalDynamicGI> local_dynamic_gi_owner;
} // namespace

RID RendererGI::local_dynamic_gi_allocate() {
	return local_dynamic_gi_owner.allocate_rid();
}

void RendererGI::local_dynamic_gi_free(RID p_rid) {
	ERR_FAIL_COND(!local_dynamic_gi_owner.owns(p_rid));
	local_dynamic_gi_owner.free(p_rid);
}

void RendererGI::local_dynamic_gi_initialize(RID p_rid) {
	local_dynamic_gi_owner.initialize_rid(p_rid, LocalDynamicGI());
}

bool RendererGI::owns_local_dynamic_gi(RID p_rid) const {
	return local_dynamic_gi_owner.owns(p_rid);
}

void RendererGI::local_dynamic_gi_set_enabled(RID p_local_dynamic_gi, bool p_enabled) {
	LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL(local_gi);
	local_gi->enabled = p_enabled;
}

bool RendererGI::local_dynamic_gi_is_enabled(RID p_local_dynamic_gi) const {
	const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL_V(local_gi, false);
	return local_gi->enabled;
}

void RendererGI::local_dynamic_gi_set_extend(RID p_local_dynamic_gi, const Vector3 &p_extend) {
	LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL(local_gi);
	local_gi->extend = p_extend.maxf(0.0);
}

Vector3 RendererGI::local_dynamic_gi_get_extend(RID p_local_dynamic_gi) const {
	const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL_V(local_gi, Vector3());
	return local_gi->extend;
}

void RendererGI::local_dynamic_gi_set_blend_distance(RID p_local_dynamic_gi, float p_blend_distance) {
	LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL(local_gi);
	local_gi->blend_distance = MAX(p_blend_distance, 0.0f);
}

float RendererGI::local_dynamic_gi_get_blend_distance(RID p_local_dynamic_gi) const {
	const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL_V(local_gi, 0.0f);
	return local_gi->blend_distance;
}

void RendererGI::local_dynamic_gi_set_local_bounds(RID p_local_dynamic_gi, const AABB &p_bounds) {
	LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL(local_gi);
	local_gi->local_bounds = p_bounds;
}

AABB RendererGI::local_dynamic_gi_get_local_bounds(RID p_local_dynamic_gi) const {
	const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL_V(local_gi, AABB());
	return local_gi->local_bounds;
}

void RendererGI::local_dynamic_gi_set_transform(RID p_local_dynamic_gi, const Transform3D &p_transform) {
	LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL(local_gi);
	local_gi->transform = p_transform;
}

Transform3D RendererGI::local_dynamic_gi_get_transform(RID p_local_dynamic_gi) const {
	const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL_V(local_gi, Transform3D());
	return local_gi->transform;
}

void RendererGI::local_dynamic_gi_set_scenario(RID p_local_dynamic_gi, RID p_scenario) {
	LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL(local_gi);
	local_gi->scenario = p_scenario;
}

RID RendererGI::local_dynamic_gi_get_scenario(RID p_local_dynamic_gi) const {
	const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(p_local_dynamic_gi);
	ERR_FAIL_NULL_V(local_gi, RID());
	return local_gi->scenario;
}

Vector<RID> RendererGI::local_dynamic_gi_get_registered(RID p_scenario) const {
	const LocalVector<RID> owned = local_dynamic_gi_owner.get_owned_list();

	Vector<RID> registered;
	for (const RID &rid : owned) {
		const LocalDynamicGI *local_gi = local_dynamic_gi_owner.get_or_null(rid);
		if (local_gi && local_gi->scenario == p_scenario) {
			registered.push_back(rid);
		}
	}
	return registered;
}
