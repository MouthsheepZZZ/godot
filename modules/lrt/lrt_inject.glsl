#[compute]

#version 450

// Local Radiance Transfer first-bounce injection.
// Ported from the prototype's src/shaders.js "injection" pass (G:\lrt_external_test @ 524a290).
// Only the data layout changed: the prototype's 2D atlas textures became storage buffers.
// PDF pp.19-20 only outlines source injection; light formula, shadow test and receiver
// reconstruction below are the prototype's own design and stay unchanged.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform Params {
	ivec4 grid_size; // xyz probe counts, w probe count
	vec4 grid_min; // xyz origin, w probe spacing
	ivec4 counts; // x light count, y box count, z direction count
	vec4 flags; // x sky, y multi bounce, z SH visibility, w color SDF
	vec4 light_position[8];
	vec4 light_direction[8];
	vec4 light_color[8];
	vec4 light_data[8]; // power, type, outer cos, inner cos
	vec4 box_min[16];
	vec4 box_max[16];
	vec4 box_color[16];
} params;

layout(set = 0, binding = 1, std430) restrict readonly buffer MaterialBuffer {
	vec4 data[];
}
material;

layout(set = 0, binding = 2, std430) restrict readonly buffer LinksBuffer {
	uint data[];
}
links;

layout(set = 0, binding = 5, std430) restrict readonly buffer ReceiverBuffer {
	vec4 data[];
}
receivers;

layout(set = 0, binding = 6, std430) restrict buffer SourceRBuffer {
	vec4 data[];
}
source_r;

layout(set = 0, binding = 7, std430) restrict buffer SourceGBuffer {
	vec4 data[];
}
source_g;

layout(set = 0, binding = 8, std430) restrict buffer SourceBBuffer {
	vec4 data[];
}
source_b;

const float PI = 3.141592653589793;
const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const float W = 4.0 * PI / 26.0;

// Substituted from lrt::directions() so the CPU local field and the GPU passes always agree.
const ivec3 OFFSETS[26] = ivec3[26](%LRT_DIRECTIONS%);

vec4 P(vec3 d) {
	return vec4(C0, (C1 / 3.0) * d);
}

ivec3 decode_coord(uint p_index) {
	int width = params.grid_size.x * params.grid_size.z;
	int index = int(p_index);
	int y = index / width;
	int rest = index % width;
	return ivec3(rest % params.grid_size.x, y, rest / params.grid_size.x);
}

uint probe_index(ivec3 p) {
	int width = params.grid_size.x * params.grid_size.z;
	return uint(p.x + p.z * params.grid_size.x + p.y * width);
}

bool outside(ivec3 p) {
	return any(lessThan(p, ivec3(0))) || any(greaterThanEqual(p, params.grid_size.xyz));
}

vec4 material_at(ivec3 p) {
	if (outside(p)) {
		return vec4(0.0);
	}
	return material.data[probe_index(p)];
}

vec3 position_of(ivec3 p) {
	return params.grid_min.xyz + (vec3(p) + 0.5) * params.grid_min.w;
}

// Analytic box intersection, identical to the prototype's hitBox helper.
float hit_box(vec3 origin, vec3 direction, vec3 low, vec3 high, out vec3 normal) {
	float near_t = -1e20;
	float far_t = 1e20;
	vec3 near_normal = vec3(0.0);
	vec3 far_normal = vec3(0.0);
	for (int axis = 0; axis < 3; axis++) {
		if (abs(direction[axis]) < 1e-7) {
			if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
				return -1.0;
			}
			continue;
		}
		float a = (low[axis] - origin[axis]) / direction[axis];
		float b = (high[axis] - origin[axis]) / direction[axis];
		vec3 n = vec3(0.0);
		n[axis] = -sign(direction[axis]);
		if (min(a, b) > near_t) {
			near_t = min(a, b);
			near_normal = n;
		}
		if (max(a, b) < far_t) {
			far_t = max(a, b);
			far_normal = -n;
		}
	}
	if (far_t < max(near_t, 0.0)) {
		return -1.0;
	}
	normal = near_t > 0.0 ? near_normal : far_normal;
	return near_t > 0.0 ? near_t : far_t;
}

float shadow_ray(vec3 p, vec3 direction, float distance_to_light) {
	for (int i = 0; i < 16; i++) {
		if (i >= params.counts.y) {
			break;
		}
		vec3 normal;
		float t = hit_box(p, direction, params.box_min[i].xyz, params.box_max[i].xyz, normal);
		if (t > 0.0001 && t < distance_to_light) {
			return 0.0;
		}
	}
	// N1 fixtures are box-only. Mesh occlusion is added together with model support in N2.
	return 1.0;
}

vec3 sample_light(int i, vec3 p, out vec3 incoming_direction) {
	float intensity = params.light_data[i].x;
	float distance_to_light = 1e6;
	if (params.light_data[i].y == 1.0) {
		incoming_direction = -params.light_direction[i].xyz;
	} else {
		vec3 delta = params.light_position[i].xyz - p;
		distance_to_light = length(delta);
		incoming_direction = delta / max(distance_to_light, 0.00001);
		intensity /= max(dot(delta, delta), 0.04);
		if (params.light_data[i].y == 2.0) {
			float cone_cos = dot(-incoming_direction, params.light_direction[i].xyz);
			intensity *= smoothstep(params.light_data[i].z, params.light_data[i].w, cone_cos);
		}
	}
	if (intensity <= 0.0) {
		return vec3(0.0);
	}
	return params.light_color[i].xyz * intensity * shadow_ray(p, incoming_direction, distance_to_light);
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= uint(params.grid_size.w)) {
		return;
	}
	source_r.data[index] = vec4(0.0);
	source_g.data[index] = vec4(0.0);
	source_b.data[index] = vec4(0.0);
	if (params.counts.x == 0 || material.data[index].a > 0.5) {
		return;
	}
	ivec3 p = decode_coord(index);

	if (params.flags.w > 0.5) {
		vec4 header = material.data[index];
		for (int j = 0; j < params.counts.z; j++) {
			if (j >= int(header.g)) {
				break;
			}
			int base = int(header.r) + j * 3;
			vec4 receiver_data = receivers.data[base];
			vec3 surface_normal = receivers.data[base + 1].xyz;
			vec3 albedo = receivers.data[base + 2].rgb;
			vec3 d = normalize(vec3(OFFSETS[int(receiver_data.w)]));
			vec3 receiver = receiver_data.xyz + surface_normal * 0.001;
			vec4 b = W * P(d);
			for (int i = 0; i < 8; i++) {
				if (i >= params.counts.x) {
					break;
				}
				vec3 l;
				vec3 incident = sample_light(i, receiver, l);
				vec3 reflected = albedo * incident * (max(dot(-d, l), 0.0) / PI);
				source_r.data[index] += b * reflected.r;
				source_g.data[index] += b * reflected.g;
				source_b.data[index] += b * reflected.b;
			}
		}
		return;
	}

	uint transport_mask = links.data[index];
	for (int j = 0; j < params.counts.z; j++) {
		if ((transport_mask & (1u << uint(j))) != 0u) {
			continue;
		}
		vec3 d = normalize(vec3(OFFSETS[j]));
		vec3 origin = position_of(p);
		float nearest = length(vec3(OFFSETS[j])) * params.grid_min.w + 0.000001;
		vec3 normal = vec3(0.0);
		vec3 albedo = vec3(0.0);
		vec3 surface_normal = vec3(0.0);
		bool found = false;
		for (int k = 0; k < 16; k++) {
			if (k >= params.counts.y) {
				break;
			}
			vec3 n;
			float t = hit_box(origin, d, params.box_min[k].xyz, params.box_max[k].xyz, n);
			if (t > 0.0 && t < nearest) {
				nearest = t;
				normal = n;
				surface_normal = n;
				albedo = params.box_color[k].rgb;
				found = true;
			}
		}
		if (!found) {
			continue;
		}
		vec3 receiver = origin + d * nearest + surface_normal * 0.001;
		vec4 b = W * P(d);
		for (int i = 0; i < 8; i++) {
			if (i >= params.counts.x) {
				break;
			}
			vec3 l;
			vec3 incident = sample_light(i, receiver, l);
			vec3 reflected = albedo * incident * (max(dot(normal, l), 0.0) / PI);
			source_r.data[index] += b * reflected.r;
			source_g.data[index] += b * reflected.g;
			source_b.data[index] += b * reflected.b;
		}
	}
}
