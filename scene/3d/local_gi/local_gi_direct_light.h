/**************************************************************************/
/*  local_gi_direct_light.h                                               */
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
/* MERCHANTABILITY, FITNESS FOR A MERCHANTABILITY AND NONINFRINGEMENT.    */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/color.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/vector.h"

class Node;
class Node3D;

// Scene lights copied into LocalGIVolume local space for one-bounce shading.
struct LocalGIDirectLight {
	enum Type {
		TYPE_DIRECTIONAL,
		TYPE_OMNI,
		TYPE_SPOT,
	};

	struct Sample {
		bool valid = false;
		Vector3 to_light;
		float distance = 0.0f;
		Color irradiance;
	};

	Type type = TYPE_OMNI;
	Vector3 position;
	Vector3 direction;
	Color intensity;
	float range = 5.0f;
	float attenuation = 1.0f;
	float spot_angle_cos = 0.0f;
	float spot_attenuation = 1.0f;

	static float omni_attenuation(float p_distance, float p_range, float p_decay);
	Sample sample(const Vector3 &p_position, const Vector3 &p_normal) const;
};

class LocalGIDirectLights {
public:
	static void collect(Node *p_from_node, const Node3D *p_volume, Vector<LocalGIDirectLight> &r_lights);
};
