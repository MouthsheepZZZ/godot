/**************************************************************************/
/*  local_gi_direct_light.cpp                                             */
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
/* MERCHANTABILITY AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR  */
/* COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, */
/* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,     */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER          */
/* DEALINGS IN THE SOFTWARE.                                              */
/**************************************************************************/

#include "local_gi_direct_light.h"

#include "scene/3d/light_3d.h"
#include "scene/3d/local_gi/local_gi_static_geometry.h"

float LocalGIDirectLight::omni_attenuation(float p_distance, float p_range, float p_decay) {
	const float range = MAX(p_range, 0.0001f);
	float nd = p_distance / range;
	nd *= nd;
	nd *= nd;
	nd = MAX(1.0f - nd, 0.0f);
	nd *= nd;
	return nd * Math::pow(MAX(p_distance, 0.0001f), -p_decay);
}

LocalGIDirectLight::Sample LocalGIDirectLight::sample(const Vector3 &p_position, const Vector3 &p_normal) const {
	Sample result;
	Vector3 to_light;
	float distance = 0.0f;
	float distance_atten = 1.0f;

	if (type == TYPE_DIRECTIONAL) {
		to_light = -direction;
		const float dir_len_sq = to_light.length_squared();
		if (dir_len_sq < (float)CMP_EPSILON2) {
			return result;
		}
		to_light /= Math::sqrt(dir_len_sq);
		distance = range;
	} else {
		to_light = position - p_position;
		distance = to_light.length();
		if (distance > range || distance < 1e-5f) {
			return result;
		}
		to_light /= distance;
		distance_atten = omni_attenuation(distance, range, attenuation);
		if (type == TYPE_SPOT) {
			const float scos = MAX((-to_light).dot(direction), spot_angle_cos);
			const float denom = MAX(1.0f - spot_angle_cos, 1e-4f);
			const float spot_rim = MAX(1e-4f, (1.0f - scos) / denom);
			distance_atten *= 1.0f - Math::pow(spot_rim, spot_attenuation);
		}
	}

	const float n_dot_l = p_normal.dot(to_light);
	if (n_dot_l <= 0.0f || distance_atten <= 0.0f) {
		return result;
	}

	result.valid = true;
	result.to_light = to_light;
	result.distance = distance;
	result.irradiance = intensity * (distance_atten * n_dot_l);
	result.irradiance.a = 1.0f;
	return result;
}

void LocalGIDirectLights::collect(Node *p_from_node, const Node3D *p_volume, Vector<LocalGIDirectLight> &r_lights) {
	r_lights.clear();
	ERR_FAIL_NULL(p_from_node);
	ERR_FAIL_NULL(p_volume);

	Vector<Node *> stack;
	stack.push_back(p_from_node);

	while (!stack.is_empty()) {
		Node *node = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);

		Light3D *light = Object::cast_to<Light3D>(node);
		if (light && light->is_visible_in_tree() && !light->is_editor_only() && !light->is_negative() && light->get_bake_mode() != Light3D::BAKE_DISABLED) {
			const Transform3D local = LocalGIStaticGeometry::get_relative_transform(light, p_volume);
			LocalGIDirectLight packed;
			packed.position = local.origin;
			packed.direction = -local.basis.get_column(Vector3::AXIS_Z);
			if (packed.direction.length_squared() > (real_t)CMP_EPSILON2) {
				packed.direction.normalize();
			}
			packed.intensity = light->get_color() * light->get_param(Light3D::PARAM_ENERGY) * light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
			packed.intensity.a = 1.0f;
			packed.range = light->get_param(Light3D::PARAM_RANGE);
			packed.attenuation = light->get_param(Light3D::PARAM_ATTENUATION);
			packed.spot_angle_cos = Math::cos(Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE)));
			packed.spot_attenuation = light->get_param(Light3D::PARAM_SPOT_ATTENUATION);

			switch (light->get_light_type()) {
				case RSE::LIGHT_DIRECTIONAL: {
					packed.type = LocalGIDirectLight::TYPE_DIRECTIONAL;
					packed.range = MAX(packed.range, 1000.0f);
				} break;
				case RSE::LIGHT_SPOT: {
					packed.type = LocalGIDirectLight::TYPE_SPOT;
				} break;
				default: {
					packed.type = LocalGIDirectLight::TYPE_OMNI;
				} break;
			}
			r_lights.push_back(packed);
		}

		for (int i = 0; i < node->get_child_count(); i++) {
			stack.push_back(node->get_child(i));
		}
	}
}
