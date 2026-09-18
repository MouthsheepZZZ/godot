#[compute]

#version 450

// Resolves one native Forward+ receiver atlas directly into the inactive per-light
// irradiance field. The CPU never reads or decodes the capture texture.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D capture_texture;

layout(set = 0, binding = 1, std430) restrict readonly buffer ReceiverBuffer {
	vec4 data[];
}
receivers;

layout(set = 0, binding = 2, std430) restrict buffer NativeLightUnitBuffer {
	vec4 data[];
}
native_light_units;

layout(set = 0, binding = 3) uniform sampler2D area_light_atlas;

layout(push_constant, std430) uniform Params {
	mat4 volume_to_source;
	vec4 ranges;
	ivec4 layout_data;
	ivec4 kind;
	vec4 atlas_rect;
}
params;

float range_window(float distance_to_light, float light_range) {
	float normalized = distance_to_light / max(light_range, 1e-6);
	float normalized_squared = normalized * normalized;
	float window = max(1.0 - normalized_squared * normalized_squared, 0.0);
	return window * window;
}

float get_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(distance, 0.0001), -decay);
}

const float PI = 3.141592653589793;
const float TAU = 6.28318530718;
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

vec3 receiver_transport_normal(int receiver_base) {
	int direction_index = int(receivers.data[receiver_base].w);
	return -normalize(vec3(OFFSETS[direction_index]));
}

float integrate_edge_hill_y(vec3 p0, vec3 p1) {
	float cos_theta = dot(p0, p1);
	float x = cos_theta;
	float y = abs(x);
	float a = 5.42031 + (3.12829 + 0.0902326 * y) * y;
	float b = 3.45068 + (4.18814 + y) * y;
	float theta_sintheta = a / b;
	if (x < 0.0) {
		theta_sintheta = PI * inversesqrt(1.0 - x * x) - theta_sintheta;
	}
	return (theta_sintheta * cross(p0, p1)).y;
}

float integrate_edge(vec3 p_proj0, vec3 p_proj1, vec3 p0, vec3 p1) {
	float epsilon = 0.00001;
	if (dot(p_proj0, p_proj1) < -1.0 + epsilon) {
		vec3 half_point = normalize(p0 + normalize(p1 - p0) * dot(p0, normalize(p0 - p1)));
		return integrate_edge_hill_y(p_proj0, half_point) + integrate_edge_hill_y(half_point, p_proj1);
	}
	return integrate_edge_hill_y(p_proj0, p_proj1);
}

void clip_quad_to_horizon(inout vec3 L[5], out int vertex_count) {
	int config = 0;
	if (L[0].y > 0.0) {
		config += 1;
	}
	if (L[1].y > 0.0) {
		config += 2;
	}
	if (L[2].y > 0.0) {
		config += 4;
	}
	if (L[3].y > 0.0) {
		config += 8;
	}
	vertex_count = 0;
	if (config == 1) {
		vertex_count = 3;
		L[1] = -L[1].y * L[0] + L[0].y * L[1];
		L[2] = -L[3].y * L[0] + L[0].y * L[3];
	} else if (config == 2) {
		vertex_count = 3;
		L[0] = -L[0].y * L[1] + L[1].y * L[0];
		L[2] = -L[2].y * L[1] + L[1].y * L[2];
	} else if (config == 3) {
		vertex_count = 4;
		L[2] = -L[2].y * L[1] + L[1].y * L[2];
		L[3] = -L[3].y * L[0] + L[0].y * L[3];
	} else if (config == 4) {
		vertex_count = 3;
		L[0] = -L[3].y * L[2] + L[2].y * L[3];
		L[1] = -L[1].y * L[2] + L[2].y * L[1];
	} else if (config == 6) {
		vertex_count = 4;
		L[0] = -L[0].y * L[1] + L[1].y * L[0];
		L[3] = -L[3].y * L[2] + L[2].y * L[3];
	} else if (config == 7) {
		vertex_count = 5;
		L[4] = -L[3].y * L[0] + L[0].y * L[3];
		L[3] = -L[3].y * L[2] + L[2].y * L[3];
	} else if (config == 8) {
		vertex_count = 3;
		L[0] = -L[0].y * L[3] + L[3].y * L[0];
		L[1] = -L[2].y * L[3] + L[3].y * L[2];
		L[2] = L[3];
	} else if (config == 9) {
		vertex_count = 4;
		L[1] = -L[1].y * L[0] + L[0].y * L[1];
		L[2] = -L[2].y * L[3] + L[3].y * L[2];
	} else if (config == 11) {
		vertex_count = 5;
		L[4] = L[3];
		L[3] = -L[2].y * L[3] + L[3].y * L[2];
		L[2] = -L[2].y * L[1] + L[1].y * L[2];
	} else if (config == 12) {
		vertex_count = 4;
		L[1] = -L[1].y * L[2] + L[2].y * L[1];
		L[0] = -L[0].y * L[3] + L[3].y * L[0];
	} else if (config == 13) {
		vertex_count = 5;
		L[4] = L[3];
		L[3] = L[2];
		L[2] = -L[1].y * L[2] + L[2].y * L[1];
		L[1] = -L[1].y * L[0] + L[0].y * L[1];
	} else if (config == 14) {
		vertex_count = 5;
		L[4] = -L[0].y * L[3] + L[3].y * L[0];
		L[0] = -L[0].y * L[1] + L[1].y * L[0];
	} else if (config == 15) {
		vertex_count = 4;
	}
	if (vertex_count == 3) {
		L[3] = L[0];
	}
	if (vertex_count == 4) {
		L[4] = L[0];
	}
}

float ltc_integrate_clipped_quad(vec3 L[5], vec3 L_proj[5], int vertices_above_horizon) {
	float I = integrate_edge(L_proj[0], L_proj[1], L[0], L[1]);
	I += integrate_edge(L_proj[1], L_proj[2], L[1], L[2]);
	I += integrate_edge(L_proj[2], L_proj[3], L[2], L[3]);
	if (vertices_above_horizon >= 4) {
		I += integrate_edge(L_proj[3], L_proj[4], L[3], L[4]);
	}
	if (vertices_above_horizon == 5) {
		I += integrate_edge(L_proj[4], L_proj[0], L[4], L[0]);
	}
	return abs(I);
}

vec3 integrate_edge_hill(vec3 p0, vec3 p1) {
	float cos_theta = dot(p0, p1);
	float x = cos_theta;
	float y = abs(x);
	float a = 5.42031 + (3.12829 + 0.0902326 * y) * y;
	float b = 3.45068 + (4.18814 + y) * y;
	float theta_sintheta = a / b;
	if (x < 0.0) {
		theta_sintheta = PI * inversesqrt(1.0 - x * x) - theta_sintheta;
	}
	return theta_sintheta * cross(p0, p1);
}

vec3 fetch_ltc_lod(vec2 uv, vec4 texture_rect, float lod, float max_mipmap) {
	float low = min(max(floor(lod), 0.0), max_mipmap - 1.0);
	float high = min(max(floor(lod + 1.0), 1.0), max_mipmap);
	vec2 sample_pos = texture_rect.xy + clamp(uv, 0.0, 1.0) * texture_rect.zw;
	vec4 sample_col_low = textureLod(area_light_atlas, sample_pos, low);
	vec4 sample_col_high = textureLod(area_light_atlas, sample_pos, high);
	float blend = high - clamp(lod, high - 1.0, high);
	vec4 sample_col = mix(sample_col_high, sample_col_low, blend);
	return sample_col.rgb * sample_col.a;
}

vec3 fetch_ltc_filtered_texture(vec4 texture_rect, vec3 L[4], float max_mipmap) {
	vec3 L0 = normalize(L[0]);
	vec3 L1 = normalize(L[1]);
	vec3 L2 = normalize(L[2]);
	vec3 L3 = normalize(L[3]);
	vec3 F = vec3(0.0);
	F += integrate_edge_hill(L0, L1);
	F += integrate_edge_hill(L1, L2);
	F += integrate_edge_hill(L2, L3);
	F += integrate_edge_hill(L3, L0);
	vec2 uv;
	float lod = 0.0;
	if (dot(F, F) < 1e-16) {
		uv = vec2(0.5);
		lod = max_mipmap;
	} else {
		vec3 lx = L[1] - L[0];
		vec3 ly = L[3] - L[0];
		vec3 ln = cross(lx, ly);
		float dist_x_area = dot(L[0], ln);
		float d = dist_x_area / dot(F, ln);
		vec3 isec = d * F;
		vec3 li = isec - L[0];
		float dot_lxy = dot(lx, ly);
		float inv_dot_lxlx = 1.0 / dot(lx, lx);
		vec3 ly_ = ly - lx * dot_lxy * inv_dot_lxlx;
		uv.y = dot(li, ly_) / dot(ly_, ly_);
		uv.x = dot(li, lx) * inv_dot_lxlx - dot_lxy * inv_dot_lxlx * uv.y;
		lod = abs(dist_x_area) / pow(dot(ln, ln), 0.75);
		lod = log(2048.0 * lod) / log(3.0);
	}
	return fetch_ltc_lod(vec2(1.0) - uv, texture_rect, lod, max_mipmap);
}

float ltc_evaluate_diff(vec3 normal, vec3 points[4], vec4 texture_rect, out vec3 tex_color) {
	tex_color = vec3(1.0);
	vec3 eye_vec = abs(normal.z) < 0.7 ? vec3(0.0, 0.0, -1.0) : vec3(1.0, 0.0, 0.0);
	vec3 z = -normalize(eye_vec - normal * dot(eye_vec, normal));
	vec3 x = cross(normal, z);
	mat3 M_inv = transpose(mat3(x, normal, z));
	vec3 L[5];
	L[0] = M_inv * points[0];
	L[1] = M_inv * points[1];
	L[2] = M_inv * points[2];
	L[3] = M_inv * points[3];
	vec3 L_unclipped[4];
	L_unclipped[0] = L[0];
	L_unclipped[1] = L[1];
	L_unclipped[2] = L[2];
	L_unclipped[3] = L[3];
	int n;
	clip_quad_to_horizon(L, n);
	if (n == 0) {
		return 0.0;
	}
	vec3 L_proj[5];
	L_proj[0] = normalize(L[0]);
	L_proj[1] = normalize(L[1]);
	L_proj[2] = normalize(L[2]);
	L_proj[3] = normalize(L[3]);
	L_proj[4] = normalize(L[4]);
	vec3 pnorm = normalize(cross(L_proj[0] - L_proj[1], L_proj[2] - L_proj[1]));
	if (abs(dot(pnorm, L_proj[0])) < 1e-10) {
		return 0.0;
	}
	if (texture_rect != vec4(0.0)) {
		tex_color = fetch_ltc_filtered_texture(texture_rect, L_unclipped, intBitsToFloat(params.kind.w));
	}
	return ltc_integrate_clipped_quad(L, L_proj, n);
}

void main() {
	int local_index = int(gl_GlobalInvocationID.x);
	if (local_index >= params.layout_data.y) {
		return;
	}
	int receiver_index = params.layout_data.x + local_index;
	int light_slot = params.kind.x & 4095;
	vec3 value = vec3(0.0);
	if (params.kind.y >= 2) {
		uint cull_mask = uint(params.kind.x) >> 12u;
		uint layer = floatBitsToUint(receivers.data[receiver_index * 3 + 1].w);
		if ((layer & cull_mask) == 0u) {
			native_light_units.data[light_slot * params.layout_data.w + receiver_index] = vec4(0.0);
			return;
		}
	}
	if (params.kind.y == 2) {
		int receiver_base = receiver_index * 3;
		vec3 receiver_position = receivers.data[receiver_base].xyz;
		vec3 surface_normal = normalize(receivers.data[receiver_base + 1].xyz);
		vec3 transport_normal = receiver_transport_normal(receiver_base);
		vec3 light_direction = normalize(params.ranges.xyz);
		float visibility = 1.0;
		if (params.ranges.w > 0.5) {
			vec3 sample_pos = receiver_position + surface_normal * 0.02;
			vec4 coord = params.volume_to_source * vec4(sample_pos, 1.0);
			float inv_w = 1.0 / max(abs(coord.w), 1e-6);
			vec3 uvz = coord.xyz * inv_w;
			if (uvz.x >= 0.0 && uvz.x <= 1.0 && uvz.y >= 0.0 && uvz.y <= 1.0) {
				float closest = textureLod(capture_texture, uvz.xy, 0.0).r;
				// The map clears to zero, so an empty texel reads as "nothing in front" and the
				// receiver stays lit. A stored value nearer the light than the receiver occludes.
				visibility = uvz.z > closest ? 1.0 : 0.0;
			}
		}
		value = vec3(max(dot(transport_normal, light_direction), 0.0) * visibility);
	} else if (params.kind.y == 3 || params.kind.y == 4) {
		int receiver_base = receiver_index * 3;
		vec3 receiver_position = receivers.data[receiver_base].xyz;
		vec3 surface_normal = normalize(receivers.data[receiver_base + 1].xyz);
		vec3 transport_normal = receiver_transport_normal(receiver_base);
		vec3 light_position;
		vec3 light_vector;
		float source_range;
		float decay;
		if (params.kind.y == 3) {
			vec3 local_vert = (params.volume_to_source * vec4(receiver_position, 1.0)).xyz;
			light_vector = transpose(mat3(params.volume_to_source)) * (-local_vert);
			source_range = params.ranges.x;
			decay = params.ranges.y;
		} else {
			light_position = params.ranges.xyz;
			light_vector = light_position - receiver_position;
			source_range = params.ranges.w;
			decay = unpackHalf2x16(uint(params.kind.w)).y;
		}
		float distance_to_light = length(light_vector);
		vec3 light_direction = light_vector / max(distance_to_light, 1e-6);
		float attenuation = get_omni_attenuation(distance_to_light, 1.0 / max(source_range, 1e-6), decay);
		if (params.kind.y == 4) {
			vec2 dir_xy = unpackHalf2x16(uint(params.kind.z));
			float dir_z = unpackHalf2x16(uint(params.kind.w)).x;
			vec3 spot_dir = normalize(vec3(dir_xy, dir_z));
			vec2 cone = unpackHalf2x16(uint(params.layout_data.z));
			float cone_angle = cone.x;
			float scos = max(dot(-light_direction, spot_dir), cone_angle);
			float spot_rim = max(1e-4, (1.0 - scos) / max(1.0 - cone_angle, 1e-6));
			attenuation *= 1.0 - pow(spot_rim, cone.y);
		}
		float visibility = 1.0;
		bool shadowed = params.kind.y == 3 ? params.ranges.w > 0.5 : params.atlas_rect.z > 0.0;
		if (shadowed && attenuation > 0.0) {
			vec3 sample_pos = receiver_position + surface_normal * 0.02;
			if (params.kind.y == 3) {
				vec3 local_vert = (params.volume_to_source * vec4(sample_pos, 1.0)).xyz;
				float shadow_len = length(local_vert);
				vec3 shadow_dir = normalize(local_vert);
				vec3 local_normal = normalize(mat3(params.volume_to_source) * surface_normal);
				vec3 normal_bias = local_normal * 0.02 * (1.0 - abs(dot(local_normal, shadow_dir)));
				vec4 uv_rect = params.atlas_rect;
				vec2 flip_offset = unpackHalf2x16(uint(params.kind.z));
				vec3 shadow_sample = normalize(shadow_dir + normal_bias);
				if (shadow_sample.z >= 0.0) {
					uv_rect.xy += flip_offset;
				}
				shadow_sample.z = 1.0 + abs(shadow_sample.z);
				vec2 pos = shadow_sample.xy / max(shadow_sample.z, 1e-6);
				pos = pos * 0.5 + 0.5;
				pos = uv_rect.xy + pos * uv_rect.zw;
				float depth = (shadow_len - params.ranges.z) * (1.0 / max(source_range, 1e-6));
				depth = 1.0 - depth;
				float closest = textureLod(capture_texture, pos, 0.0).r;
				visibility = depth > closest ? 1.0 : 0.0;
			} else {
				vec4 splane = params.volume_to_source * vec4(sample_pos, 1.0);
				splane.z += 0.0002;
				float inv_w = 1.0 / max(abs(splane.w), 1e-6);
				splane.xyz *= inv_w;
				if (splane.x < 0.0 || splane.x > 1.0 || splane.y < 0.0 || splane.y > 1.0) {
					visibility = 0.0;
				} else {
					vec2 uv = splane.xy * params.atlas_rect.zw + params.atlas_rect.xy;
					float closest = textureLod(capture_texture, uv, 0.0).r;
					visibility = splane.z > closest ? 1.0 : 0.0;
				}
			}
		}
		value = vec3(max(dot(transport_normal, light_direction), 0.0) * attenuation * visibility);
	} else if (params.kind.y == 5) {
		int receiver_base = receiver_index * 3;
		vec3 receiver_position = receivers.data[receiver_base].xyz;
		vec3 surface_normal = normalize(receivers.data[receiver_base + 1].xyz);
		vec3 transport_normal = receiver_transport_normal(receiver_base);
		mat4 source_to_volume = inverse(params.volume_to_source);
		vec3 light_center = source_to_volume[3].xyz;
		vec3 axis_x = normalize(source_to_volume[0].xyz);
		vec3 axis_y = normalize(source_to_volume[1].xyz);
		vec3 area_direction = -normalize(source_to_volume[2].xyz);
		vec2 half_size = unpackHalf2x16(uint(params.layout_data.z));
		vec3 area_width = axis_x * half_size.x * 2.0;
		vec3 area_height = axis_y * half_size.y * 2.0;
		vec4 packed_atlas = vec4(unpackHalf2x16(floatBitsToUint(params.atlas_rect.x)), unpackHalf2x16(floatBitsToUint(params.atlas_rect.y)));
		vec4 packed_projector = vec4(unpackHalf2x16(floatBitsToUint(params.atlas_rect.z)), unpackHalf2x16(floatBitsToUint(params.atlas_rect.w)));
		if (dot(area_width, area_width) < 1e-12 || dot(area_height, area_height) < 1e-12) {
			value = vec3(0.0);
		} else if (dot(area_direction, receiver_position - light_center) <= 0.0) {
			value = vec3(0.0);
		} else {
			vec3 light_to_vert = receiver_position - light_center;
			vec3 pos_local = vec3(dot(light_to_vert, axis_x), dot(light_to_vert, axis_y), dot(light_to_vert, -area_direction));
			vec3 closest_local = vec3(clamp(pos_local.x, -half_size.x, half_size.x), clamp(pos_local.y, -half_size.y, half_size.y), 0.0);
			vec3 closest_point = light_center + closest_local.x * axis_x + closest_local.y * axis_y;
			vec3 light_vector = closest_point - receiver_position;
			float distance_to_light = length(light_vector);
			float source_range = params.ranges.x;
			float decay = params.ranges.y;
			float attenuation = get_omni_attenuation(distance_to_light, 1.0 / max(source_range, 1e-6), decay - 2.0);
			vec3 h_area_width = area_width * 0.5;
			vec3 h_area_height = area_height * 0.5;
			vec3 points[4];
			points[0] = light_center - h_area_width - h_area_height - receiver_position;
			points[1] = light_center + h_area_width - h_area_height - receiver_position;
			points[2] = light_center + h_area_width + h_area_height - receiver_position;
			points[3] = light_center - h_area_width + h_area_height - receiver_position;
			vec3 tex_color = vec3(1.0);
			// Surface capture uses ltc_evaluate (I / 2π). Unit non-physical lights carry π in
			// LIGHT_COLOR, which cancels the capture shader's Lambert /π term.
			float ltc_diffuse = ltc_evaluate_diff(transport_normal, points, packed_projector, tex_color) / (2.0 * PI);
			attenuation *= ltc_diffuse;
			if (params.ranges.w >= 1.5) {
				float surface_area = max(4.0 * half_size.x * half_size.y, 1e-8);
				attenuation /= surface_area;
			}
			float visibility = 1.0;
			bool shadowed = mod(params.ranges.w, 2.0) >= 0.5;
			if (shadowed && attenuation > 0.0) {
				vec3 sample_pos = receiver_position + surface_normal * 0.02;
				vec3 local_vert = (params.volume_to_source * vec4(sample_pos, 1.0)).xyz;
				float shadow_len = length(local_vert);
				vec3 shadow_dir = normalize(local_vert);
				vec3 local_normal = normalize(mat3(params.volume_to_source) * surface_normal);
				vec3 normal_bias = local_normal * 0.02 * (1.0 - abs(dot(local_normal, shadow_dir)));
				vec3 shadow_sample = normalize(shadow_dir + normal_bias);
				shadow_sample.z = 1.0 + abs(shadow_sample.z);
				vec2 pos = shadow_sample.xy / max(shadow_sample.z, 1e-6);
				pos = pos * 0.5 + 0.5;
				pos = packed_atlas.xy + pos * packed_atlas.zw;
				float inv_center_range = 1.0 / max(source_range + length(half_size), 1e-6);
				float depth = (shadow_len - params.ranges.z) * inv_center_range;
				depth = 1.0 - depth;
				float closest = textureLod(capture_texture, pos, 0.0).r;
				visibility = depth > closest ? 1.0 : 0.0;
			}
			vec3 light_direction = light_vector / max(distance_to_light, 1e-6);
			value = max(attenuation, 0.0) * visibility * tex_color * max(dot(transport_normal, light_direction), 0.0);
		}
	} else {
		int pixel_y = params.kind.w - 1 - local_index / max(params.layout_data.z, 1);
		ivec2 pixel = ivec2(local_index % max(params.layout_data.z, 1), pixel_y);
		value = texelFetch(capture_texture, pixel, 0).rgb;
		if (params.kind.y == 0) {
			int receiver_base = receiver_index * 3;
			vec3 receiver_position = receivers.data[receiver_base].xyz;
			vec3 surface_normal = receivers.data[receiver_base + 1].xyz;
			vec3 local_point = (params.volume_to_source * vec4(receiver_position + surface_normal * 0.001, 1.0)).xyz;
			float distance_to_light = length(local_point);
			if (params.kind.z != 0) {
				vec2 half_size = params.ranges.zw;
				vec3 closest_point = vec3(clamp(local_point.xy, -half_size, half_size), 0.0);
				distance_to_light = distance(local_point, closest_point);
			}
			float capture_window = range_window(distance_to_light, params.ranges.y);
			value *= range_window(distance_to_light, params.ranges.x) / max(capture_window, 1e-8);
		}
	}
	int target_index = light_slot * params.layout_data.w + receiver_index;
	native_light_units.data[target_index] = vec4(value, 0.0);
}
