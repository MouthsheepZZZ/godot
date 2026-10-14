#[vertex]

#version 450

#VERSION_DEFINES

layout(set = 0, binding = 0, std430) readonly buffer SurfaceCells {
	vec4 data[];
} surface_cells;

layout(push_constant, std430) uniform Params {
	mat4 projection;
	vec4 cell_size;
} params;

layout(location = 0) out vec3 voxel_position;
layout(location = 1) flat out vec3 voxel_color;

// Clockwise faces, matching the engine's front-face culling convention.
const vec3 cube_triangles[36] = vec3[](
		vec3(-1, -1, -1), vec3(-1, -1, 1), vec3(-1, 1, 1),
		vec3(1, 1, -1), vec3(-1, -1, -1), vec3(-1, 1, -1),
		vec3(1, -1, 1), vec3(-1, -1, -1), vec3(1, -1, -1),
		vec3(1, 1, -1), vec3(1, -1, -1), vec3(-1, -1, -1),
		vec3(-1, -1, -1), vec3(-1, 1, 1), vec3(-1, 1, -1),
		vec3(1, -1, 1), vec3(-1, -1, 1), vec3(-1, -1, -1),
		vec3(-1, 1, 1), vec3(-1, -1, 1), vec3(1, -1, 1),
		vec3(1, 1, 1), vec3(-1, 1, 1), vec3(1, -1, 1),
		vec3(1, 1, 1), vec3(1, -1, -1), vec3(1, 1, -1),
		vec3(1, -1, -1), vec3(1, 1, 1), vec3(1, -1, 1),
		vec3(1, 1, 1), vec3(1, 1, -1), vec3(-1, 1, -1),
		vec3(1, 1, 1), vec3(-1, 1, -1), vec3(-1, 1, 1));

void main() {
	vec4 sample_value = surface_cells.data[gl_InstanceIndex];
	voxel_position = cube_triangles[gl_VertexIndex];
	vec3 position = sample_value.xyz + voxel_position * params.cell_size.x * 0.5;
	gl_Position = params.projection * vec4(position, 1.0);
	vec3 tint = sample_value.w < 0.0 ? vec3(0.95, 0.22, 0.08) : vec3(0.08, 0.4, 0.95);
	voxel_color = mix(vec3(1.0, 0.85, 0.25), tint, clamp(abs(sample_value.w) * 2.0, 0.0, 1.0));
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec3 voxel_position;
layout(location = 1) flat in vec3 voxel_color;
layout(location = 0) out vec4 frag_color;

void main() {
	vec3 a = abs(voxel_position);
	float edge = min(max(a.x, a.y), min(max(a.x, a.z), max(a.y, a.z)));
	float border = smoothstep(0.94, 0.98, edge);
	vec3 normal = normalize(cross(dFdx(voxel_position), dFdy(voxel_position)));
	float shade = 0.65 + 0.35 * abs(dot(normal, normalize(vec3(0.4, 0.8, 0.3))));
	frag_color = vec4(mix(voxel_color * shade, vec3(0.035), border), 1.0);
}
