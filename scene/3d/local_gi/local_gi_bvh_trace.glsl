#[versions]

trace = "";

#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct GpuTriangle {
	vec3 v0;
	float pad0;
	vec3 v1;
	float pad1;
	vec3 v2;
	float pad2;
	vec3 normal;
	int index;
};

struct GpuNode {
	vec3 bounds_min;
	int left;
	vec3 bounds_max;
	int right;
	int first_triangle;
	int triangle_count;
	int pad0;
	int pad1;
};

struct GpuRay {
	vec3 origin;
	float pad0;
	vec3 direction;
	float pad1;
};

struct GpuHit {
	uint hit;
	float distance;
	int triangle_index;
	uint pad0;
	vec3 position;
	float pad1;
	vec3 normal;
	float pad2;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer StaticNodes {
	GpuNode data[];
}
static_nodes;

layout(set = 0, binding = 1, std430) restrict readonly buffer StaticTris {
	GpuTriangle data[];
}
static_tris;

layout(set = 0, binding = 2, std430) restrict readonly buffer DynamicNodes {
	GpuNode data[];
}
dynamic_nodes;

layout(set = 0, binding = 3, std430) restrict readonly buffer DynamicTris {
	GpuTriangle data[];
}
dynamic_tris;

layout(set = 1, binding = 0, std430) restrict readonly buffer Rays {
	GpuRay data[];
}
rays;

layout(set = 1, binding = 1, std430) restrict writeonly buffer Hits {
	GpuHit data[];
}
hits;

layout(push_constant, std430) uniform Params {
	int static_node_count;
	int dynamic_node_count;
	int ray_count;
	int pad;
}
params;

const float TRI_T_MIN = 0.00001;
const int STACK_SIZE = 64;

bool aabb_intersects_ray(vec3 p_from, vec3 p_dir, vec3 p_min, vec3 p_max) {
	float tmin = -1.0e20;
	float tmax = 1.0e20;

	for (int i = 0; i < 3; i++) {
		if (p_dir[i] == 0.0) {
			if ((p_from[i] < p_min[i]) || (p_from[i] > p_max[i])) {
				return false;
			}
		} else {
			float t1 = (p_min[i] - p_from[i]) / p_dir[i];
			float t2 = (p_max[i] - p_from[i]) / p_dir[i];
			if (t1 > t2) {
				float tmp = t1;
				t1 = t2;
				t2 = tmp;
			}
			if (t1 >= tmin) {
				tmin = t1;
			}
			if (t2 < tmax) {
				if (t2 < 0.0) {
					return false;
				}
				tmax = t2;
			}
			if (tmin > tmax) {
				return false;
			}
		}
	}

	return true;
}

bool ray_intersects_triangle(vec3 p_from, vec3 p_dir, vec3 p_v0, vec3 p_v1, vec3 p_v2, out vec3 r_point) {
	vec3 e1 = p_v1 - p_v0;
	vec3 e2 = p_v2 - p_v0;
	vec3 h = cross(p_dir, e2);
	float a = dot(e1, h);
	if (abs(a) < 1e-6) {
		return false;
	}

	float f = 1.0 / a;
	vec3 s = p_from - p_v0;
	float u = f * dot(s, h);
	if ((u < 0.0) || (u > 1.0)) {
		return false;
	}

	vec3 q = cross(s, e1);
	float v = f * dot(p_dir, q);
	if ((v < 0.0) || (u + v > 1.0)) {
		return false;
	}

	float t = f * dot(e2, q);
	if (t > TRI_T_MIN) {
		r_point = p_from + p_dir * t;
		return true;
	}
	return false;
}

bool is_leaf(GpuNode p_node) {
	return p_node.triangle_count > 0;
}

bool trace_tree(vec3 p_origin, vec3 p_dir, bool p_dynamic, out GpuHit r_hit) {
	r_hit.hit = 0u;
	r_hit.distance = 0.0;
	r_hit.triangle_index = -1;
	r_hit.pad0 = 0u;
	r_hit.position = vec3(0.0);
	r_hit.pad1 = 0.0;
	r_hit.normal = vec3(0.0);
	r_hit.pad2 = 0.0;

	int node_count = p_dynamic ? params.dynamic_node_count : params.static_node_count;
	if (node_count <= 0) {
		return false;
	}

	float closest_t = 1.0e20;
	int closest_tri = -1;
	vec3 closest_point = vec3(0.0);

	int stack[STACK_SIZE];
	int sp = 0;
	stack[sp++] = 0;

	while (sp > 0) {
		int node_index = stack[--sp];
		GpuNode node = p_dynamic ? dynamic_nodes.data[node_index] : static_nodes.data[node_index];
		if (!aabb_intersects_ray(p_origin, p_dir, node.bounds_min, node.bounds_max)) {
			continue;
		}

		if (is_leaf(node)) {
			for (int i = 0; i < node.triangle_count; i++) {
				int tri_index = node.first_triangle + i;
				GpuTriangle tri = p_dynamic ? dynamic_tris.data[tri_index] : static_tris.data[tri_index];
				vec3 point;
				if (!ray_intersects_triangle(p_origin, p_dir, tri.v0, tri.v1, tri.v2, point)) {
					continue;
				}
				float t = length(point - p_origin);
				if (t < closest_t) {
					closest_t = t;
					closest_tri = tri_index;
					closest_point = point;
				}
			}
			continue;
		}

		if (node.right >= 0 && sp < STACK_SIZE) {
			stack[sp++] = node.right;
		}
		if (node.left >= 0 && sp < STACK_SIZE) {
			stack[sp++] = node.left;
		}
	}

	if (closest_tri < 0) {
		return false;
	}

	GpuTriangle closest = p_dynamic ? dynamic_tris.data[closest_tri] : static_tris.data[closest_tri];
	r_hit.hit = 1u;
	r_hit.distance = closest_t;
	r_hit.triangle_index = closest.index;
	r_hit.position = closest_point;
	r_hit.normal = closest.normal;
	return true;
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= uint(params.ray_count)) {
		return;
	}

	GpuRay ray = rays.data[idx];
	float dir_len_sq = dot(ray.direction, ray.direction);
	GpuHit result;
	result.hit = 0u;
	result.distance = 0.0;
	result.triangle_index = -1;
	result.pad0 = 0u;
	result.position = vec3(0.0);
	result.pad1 = 0.0;
	result.normal = vec3(0.0);
	result.pad2 = 0.0;

	if (dir_len_sq < 1e-12) {
		hits.data[idx] = result;
		return;
	}

	vec3 dir = ray.direction * inversesqrt(dir_len_sq);
	GpuHit static_hit;
	GpuHit dynamic_hit;
	bool hit_static = trace_tree(ray.origin, dir, false, static_hit);
	bool hit_dynamic = trace_tree(ray.origin, dir, true, dynamic_hit);

	if (hit_static && hit_dynamic) {
		result = static_hit.distance <= dynamic_hit.distance ? static_hit : dynamic_hit;
	} else if (hit_static) {
		result = static_hit;
	} else if (hit_dynamic) {
		result = dynamic_hit;
	}

	hits.data[idx] = result;
}
