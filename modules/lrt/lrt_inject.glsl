#[compute]

#version 450

// One Source recomposition. Native incident light has no receiver cosine or albedo;
// the local reflection operator applies those once. No per-light spatial fields exist.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(push_constant, std430) uniform SourceRegion {
	ivec4 minimum;
	ivec4 size;
} region;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size;
	vec4 grid_min;
	ivec4 counts;
	vec4 flags;
} params;
layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer { vec4 data[]; } material;
layout(set = 0, binding = 3, std430) restrict readonly buffer LocalRecordBuffer { vec4 data[]; } records;
layout(set = 0, binding = 4, std430) restrict readonly buffer RecordIndexBuffer { uint data[]; } record_indices;
layout(set = 0, binding = 6, std430) restrict writeonly buffer SourceRBuffer { vec4 data[]; } source_r;
layout(set = 0, binding = 7, std430) restrict writeonly buffer SourceGBuffer { vec4 data[]; } source_g;
layout(set = 0, binding = 8, std430) restrict writeonly buffer SourceBBuffer { vec4 data[]; } source_b;

layout(set = 0, binding = 27, std430) restrict readonly buffer SkyR { vec4 data[]; } sky_r;
layout(set = 0, binding = 28, std430) restrict readonly buffer SkyG { vec4 data[]; } sky_g;
layout(set = 0, binding = 29, std430) restrict readonly buffer SkyB { vec4 data[]; } sky_b;

// Matches NativeLightStateData. These are light parameters, never light-by-Probe results.
struct NativeLight {
	mat4 volume_to_light;
	mat4 shadow_matrix[4];
	vec4 color_kind;
	vec4 position_range;
	vec4 direction_attenuation;
	vec4 cone_area;
	vec4 projector;
	vec4 shadow_rect[4];
	vec4 shadow_parameters;
	vec4 cascade_splits;
	vec4 cascade_blend;
	vec4 camera_depth;
	vec4 shadow_bias;
	vec4 atlas_flags;
	vec4 area_projector;
};
layout(set = 0, binding = 21, std430) restrict readonly buffer NativeLights { NativeLight data[]; } lights;
layout(set = 0, binding = 30) uniform sampler2DArray directional_shadow;
layout(set = 0, binding = 23) uniform sampler2D positional_shadow;
layout(set = 0, binding = 24) uniform sampler2D area_atlas;
layout(set = 0, binding = 25) uniform sampler2D projector_atlas;
// Header: Trunk X/Y/Z counts, directional count; global IDs, absolute Trunk offsets, local IDs.
layout(set = 0, binding = 26, std430) restrict readonly buffer TrunkLights { uint data[]; } trunk_lights;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;

vec4 Y(vec3 direction) {
	return vec4(C0, C1 * direction);
}

const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

uint blocked_connections(uint probe) {
	uvec2 halves = uvec2(material.data[probe].xz);
	return halves.x | (halves.y << 13u);
}

uint blocked_incident_connections(uint probe, ivec3 position) {
	uint blocked = blocked_connections(probe);
	for (int j = 0; j < 26; j++) {
		if ((blocked & (1u << uint(j))) != 0u) {
			continue;
		}
		ivec3 neighbor = position + OFFSETS[j];
		if (any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, params.grid_size.xyz))) {
			continue;
		}
		uint index = uint(neighbor.x + params.grid_size.x * (neighbor.z + params.grid_size.z * neighbor.y));
		if (material.data[index].a > 0.5 || (blocked_connections(index) & (1u << uint(25 - j))) != 0u) {
			blocked |= 1u << uint(j);
		}
	}
	return blocked;
}

// Bilinear interpolation on the dominant cube face uses four of the 26 lattice
// directions. Values agree across face boundaries, avoiding nearest-bin popping.
float local_light_visibility(uint blocked, vec3 direction) {
	if (blocked == 0u) {
		return 1.0;
	}
	vec3 extent = abs(direction);
	int axis = 0;
	if (extent.y > extent.x) {
		axis = 1;
	}
	if (extent.z > extent[axis]) {
		axis = 2;
	}
	float major = extent[axis];
	if (major == 0.0) {
		// At a point source center the incident direction is isotropic.
		return float(26 - bitCount(blocked)) / 26.0;
	}
	vec3 face = direction / major;
	int u = (axis + 1) % 3;
	int v = (axis + 2) % 3;
	float visible = 0.0;
	for (int corner = 0; corner < 4; corner++) {
		ivec3 offset = ivec3(0);
		offset[axis] = int(sign(face[axis]));
		offset[u] = (corner & 1) != 0 ? int(sign(face[u])) : 0;
		offset[v] = (corner & 2) != 0 ? int(sign(face[v])) : 0;
		float weight_u = (corner & 1) != 0 ? abs(face[u]) : 1.0 - abs(face[u]);
		float weight_v = (corner & 2) != 0 ? abs(face[v]) : 1.0 - abs(face[v]);
		int index = (offset.z + 1) * 9 + (offset.y + 1) * 3 + offset.x + 1;
		if (index > 13) {
			index--;
		}
		if ((blocked & (1u << uint(index))) == 0u) {
			visible += weight_u * weight_v;
		}
	}
	return visible;
}

float attenuation(float distance_to_light, float radius, float decay) {
	float window = distance_to_light / max(radius, 1e-6);
	window *= window;
	window *= window;
	window = max(1.0 - window, 0.0);
	return window * window * pow(max(distance_to_light, 0.0001), -decay);
}

bool shadow_covers(vec4 coord) {
	if (abs(coord.w) < 1e-6) {
		return false;
	}
	vec3 uvz = coord.xyz / coord.w;
	return all(greaterThanEqual(uvz, vec3(0.0))) && all(lessThanEqual(uvz, vec3(1.0)));
}

float compare_shadow(sampler2D depth_texture, vec4 coord, vec4 rect, float bias) {
	// Native coverage is finite. Outside it the light still contributes unshadowed.
	if (!shadow_covers(coord)) {
		return 1.0;
	}
	vec3 uvz = coord.xyz / coord.w;
	float depth = textureLod(depth_texture, rect.xy + uvz.xy * rect.zw, 0.0).r;
	return uvz.z + bias >= depth ? 1.0 : 0.0;
}

float visibility(NativeLight light, vec3 position, vec3 local_position) {
	if (light.shadow_parameters.x < 0.5) {
		return 1.0;
	}
	float result;
	if (int(light.color_kind.w) == 2) {
		vec4 coord = light.shadow_matrix[0] * vec4(position, 1.0);
		vec3 uvz = coord.xyz / coord.w;
		float depth = textureLod(directional_shadow, vec3(uvz.xy, light.shadow_rect[0].x), 0.0).r;
		result = uvz.z + light.shadow_parameters.y >= depth ? 1.0 : 0.0;
	} else if (int(light.color_kind.w) == 4) {
		result = compare_shadow(positional_shadow, light.shadow_matrix[0] * vec4(position, 1.0),
				light.shadow_rect[0], light.shadow_parameters.y);
	} else {
		float distance_to_light = length(local_position);
		vec3 direction = local_position / max(distance_to_light, 1e-6);
		vec4 rect = light.shadow_rect[0];
		if (direction.z >= 0.0) {
			rect.xy += light.atlas_flags.xy;
		}
		vec2 uv = direction.xy / (1.0 + abs(direction.z)) * 0.5 + 0.5;
		float depth = 1.0 - (distance_to_light - light.shadow_parameters.y) / light.shadow_parameters.w;
		float closest = textureLod(positional_shadow, rect.xy + uv * rect.zw, 0.0).r;
		result = depth >= closest ? 1.0 : 0.0;
	}
	return mix(1.0, result, light.shadow_parameters.z);
}

vec3 projector_color(NativeLight light, vec3 position, vec3 local_position) {
	if (light.atlas_flags.z < 0.5) {
		return vec3(1.0);
	}
	vec4 rect = light.projector;
	vec2 uv;
	if (int(light.color_kind.w) == 3) {
		vec3 direction = local_position / max(length(local_position), 1e-6);
		if (direction.z >= 0.0) {
			rect.y += rect.w;
		}
		uv = direction.xy / (1.0 + abs(direction.z)) * 0.5 + 0.5;
	} else {
		vec4 coord = light.shadow_matrix[0] * vec4(position, 1.0);
		if (abs(coord.w) < 1e-6) {
			return vec3(0.0);
		}
		uv = coord.xy / coord.w;
	}
	vec2 half_texel = 0.5 / vec2(textureSize(projector_atlas, 0));
	vec2 atlas_uv = rect.xy + uv * rect.zw;
	if (int(light.color_kind.w) == 3) {
		atlas_uv = rect.xy + clamp(uv * rect.zw, half_texel, rect.zw - half_texel);
	}
	vec4 color = textureLod(projector_atlas, atlas_uv, 0.0);
	return color.rgb * color.a;
}

float triangle_solid_angle(vec3 a, vec3 b, vec3 c) {
	return 2.0 * atan(abs(dot(a, cross(b, c))), 1.0 + dot(a, b) + dot(b, c) + dot(c, a));
}

// Exact zeroth and first angular moments of the rectangle, before receiver reflection.
vec4 rectangle_projection(vec3 local_position, vec2 low, vec2 high, mat3 light_to_volume) {
	vec3 corners[4] = vec3[4](vec3(low.x, low.y, 0.0), vec3(high.x, low.y, 0.0),
			vec3(high.x, high.y, 0.0), vec3(low.x, high.y, 0.0));
	vec3 directions[4];
	for (int i = 0; i < 4; i++) {
		directions[i] = normalize(corners[i] - local_position);
	}
	float solid_angle = triangle_solid_angle(directions[0], directions[1], directions[2]) +
			triangle_solid_angle(directions[0], directions[2], directions[3]);
	vec3 moment = vec3(0.0);
	for (int i = 0; i < 4; i++) {
		vec3 edge = cross(directions[i], directions[(i + 1) % 4]);
		float edge_length = length(edge);
		if (edge_length > 1e-8) {
			moment += 0.5 * edge / edge_length * atan(edge_length, dot(directions[i], directions[(i + 1) % 4]));
		}
	}
	return vec4(C0 * solid_angle, C1 * light_to_volume * moment);
}

vec4 area_projection(NativeLight light, vec3 local_position, uint blocked) {
	vec2 half_size = light.cone_area.zw;
	if (local_position.z >= 0.0 || any(lessThanEqual(half_size, vec2(0.0)))) {
		return vec4(0.0);
	}
	mat3 light_to_volume = mat3(light.shadow_matrix[1]);
	if (blocked == 0u) {
		return rectangle_projection(local_position, -half_size, half_size, light_to_volume);
	}
	// Integrate four angular patches, rather than treating an area emitter as a
	// single direction. With no occlusion their exact moments sum to the rectangle.
	vec4 result = vec4(0.0);
	for (int quadrant = 0; quadrant < 4; quadrant++) {
		vec2 low = (vec2(quadrant & 1, (quadrant >> 1) & 1) - 1.0) * half_size;
		vec4 projected = rectangle_projection(local_position, low, low + half_size, light_to_volume);
		result += projected * local_light_visibility(blocked, projected.yzw);
	}
	return result;
}

void add_light(uint light_index, vec3 position, uint blocked, inout vec4 incoming_r, inout vec4 incoming_g, inout vec4 incoming_b) {
	NativeLight light = lights.data[light_index];
	int kind = int(light.color_kind.w);
	if (kind == 0 || all(equal(light.color_kind.rgb, vec3(0.0)))) {
		return;
	}
	vec3 local_position = (light.volume_to_light * vec4(position, 1.0)).xyz;
	vec4 projected;
	vec3 amplitude = light.color_kind.rgb;
	if (kind == 2) {
		projected = Y(light.direction_attenuation.xyz) * local_light_visibility(blocked, light.direction_attenuation.xyz);
	} else if (kind == 5) {
		projected = area_projection(light, local_position, blocked);
		vec3 closest = vec3(clamp(local_position.xy, -light.cone_area.zw, light.cone_area.zw), 0.0);
		amplitude *= attenuation(length(local_position - closest), light.position_range.w, light.direction_attenuation.w - 2.0);
		if (light.atlas_flags.w > 0.5) {
			amplitude /= 4.0 * light.cone_area.z * light.cone_area.w;
		}
		if (light.area_projector.z > 0.0 && light.area_projector.w > 0.0) {
			vec4 color = textureLod(area_atlas, light.area_projector.xy + light.area_projector.zw * 0.5, 0.0);
			amplitude *= color.rgb * color.a;
		}
	} else {
		vec3 delta = light.position_range.xyz - position;
		vec3 direction = delta / max(length(delta), 1e-6);
		projected = Y(direction) * local_light_visibility(blocked, direction);
		amplitude *= attenuation(length(local_position), light.position_range.w, light.direction_attenuation.w);
		if (kind == 4) {
			float cosine = max(dot(local_position / max(length(local_position), 1e-6), vec3(0.0, 0.0, -1.0)), light.cone_area.x);
			float rim = max(1e-4, (1.0 - cosine) / max(1.0 - light.cone_area.x, 1e-6));
			amplitude *= 1.0 - pow(rim, light.cone_area.y);
		}
		if (all(equal(amplitude, vec3(0.0)))) {
			return;
		}
		amplitude *= projector_color(light, position, local_position);
	}
	amplitude *= visibility(light, position, local_position);
	incoming_r += projected * amplitude.r;
	incoming_g += projected * amplitude.g;
	incoming_b += projected * amplitude.b;
}

void main() {
	uint invocation = gl_GlobalInvocationID.x;
	if (invocation >= uint(region.size.w)) {
		return;
	}
	uvec3 local = uvec3(invocation % uint(region.size.x), invocation / uint(region.size.x * region.size.z),
			(invocation / uint(region.size.x)) % uint(region.size.z));
	uvec3 cell = uvec3(region.minimum.xyz) + local;
	uint probe = cell.x + uint(params.grid_size.x) * (cell.z + uint(params.grid_size.z) * cell.y);
	uint slot = record_indices.data[probe];
	if (slot == 0xffffffffu) {
		return;
	}
	uint base = slot * 16u;
	vec4 emitted_r = records.data[base + 13u];
	vec4 emitted_g = records.data[base + 14u];
	vec4 emitted_b = records.data[base + 15u];
	bool reflecting = false;
	for (uint row = 0u; row < 12u; row++) {
		reflecting = reflecting || any(notEqual(records.data[base + row], vec4(0.0)));
	}
	if (reflecting) {
		uvec3 coordinate;
		coordinate.x = probe % uint(params.grid_size.x);
		coordinate.z = (probe / uint(params.grid_size.x)) % uint(params.grid_size.z);
		coordinate.y = probe / uint(params.grid_size.x * params.grid_size.z);
		vec3 position = params.grid_min.xyz + (vec3(coordinate) + 0.5) * params.grid_min.w;
		uint blocked = blocked_incident_connections(probe, ivec3(coordinate));
		vec4 incoming_r = vec4(0.0);
		vec4 incoming_g = vec4(0.0);
		vec4 incoming_b = vec4(0.0);
		for (uint i = 0u; i < trunk_lights.data[3]; i++) {
			add_light(trunk_lights.data[4u + i], position, blocked, incoming_r, incoming_g, incoming_b);
		}
		uvec3 trunk = coordinate / 8u;
		uint trunk_index = trunk.x + trunk_lights.data[0] * (trunk.z + trunk_lights.data[2] * trunk.y);
		uint offset = 4u + trunk_lights.data[3] + trunk_index;
		for (uint i = trunk_lights.data[offset]; i < trunk_lights.data[offset + 1u]; i++) {
			add_light(trunk_lights.data[i], position, blocked, incoming_r, incoming_g, incoming_b);
		}
		// Native direct light and sky already carry their respective shadowing.
		// The raw reflection matrix consumes this incident field without another mask.
		if (params.flags.y > 0.5) {
			incoming_r += sky_r.data[probe];
			incoming_g += sky_g.data[probe];
			incoming_b += sky_b.data[probe];
		}
		for (uint row = 0u; row < 4u; row++) {
			emitted_r[row] += dot(records.data[base + row], incoming_r);
			emitted_g[row] += dot(records.data[base + 4u + row], incoming_g);
			emitted_b[row] += dot(records.data[base + 8u + row], incoming_b);
		}
	}
	// A truncated angular response can ring below zero on a dark source.
	// Reject negative total emission without flattening valid SH directions.
	source_r.data[slot] = emitted_r.x > 0.0 ? emitted_r : vec4(0.0);
	source_g.data[slot] = emitted_g.x > 0.0 ? emitted_g : vec4(0.0);
	source_b.data[slot] = emitted_b.x > 0.0 ? emitted_b : vec4(0.0);
}
