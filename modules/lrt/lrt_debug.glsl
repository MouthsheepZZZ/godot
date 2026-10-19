#[vertex]

#version 450

#VERSION_DEFINES

layout(set = 0, binding = 0) uniform sampler tex_sampler;
layout(set = 0, binding = 1, std430) readonly buffer RadianceRed { vec4 data[]; } radiance_r;
layout(set = 0, binding = 2, std430) readonly buffer RadianceGreen { vec4 data[]; } radiance_g;
layout(set = 0, binding = 3, std430) readonly buffer RadianceBlue { vec4 data[]; } radiance_b;
layout(set = 0, binding = 4, std430) readonly buffer SourceRed { vec4 data[]; } source_r;
layout(set = 0, binding = 5, std430) readonly buffer SourceGreen { vec4 data[]; } source_g;
layout(set = 0, binding = 6, std430) readonly buffer SourceBlue { vec4 data[]; } source_b;
layout(set = 0, binding = 7, std430) readonly buffer SkyVisibility { vec4 data[]; } visibility_field;
layout(set = 0, binding = 10, std430) readonly buffer LinksBuffer {
	uint data[];
} links;
layout(set = 0, binding = 14) uniform texture2D dirty_field;
layout(set = 0, binding = 15, std430) readonly buffer Material { vec4 data[]; } material_field;
layout(set = 0, binding = 17, std430) readonly buffer SkyMask { uint data[]; } sky_mask;
layout(set = 0, binding = 19, std430) readonly buffer RecordIndices { uint data[]; } record_indices;

layout(set = 0, binding = 16, std430) readonly buffer Boundary { vec4 data[]; } boundary;

%LRT_BOUNDARY%

layout(push_constant, std430) uniform Params {
	mat4 projection;
	ivec4 grid_size_mode;
	vec4 grid_min_spacing;
	vec4 volume_min;
	vec4 volume_max;
} params;

layout(location = 0) out vec4 color_interp;

const float C0 = 0.2820947918;
const float C1 = 0.4886025119;
const int MODE_RADIANCE = 0;
const int MODE_SOURCE = 1;
const int MODE_SKY_VISIBILITY = 3;
const int MODE_BOUNDARY = 4;
const int MODE_UPDATE_REGIONS = 5;
const int DRAW_LOBES = 0;
const int DRAW_VOXELS = 1;
const int DRAW_LINKS = 2;
const int DRAW_BOUNDARY_BOXES = 3;
const int DRAW_BOUNDARY_SAMPLES = 4;

const vec3 cube_triangles[36] = vec3[](
		vec3(-1.0, -1.0, -1.0), vec3(-1.0, -1.0, 1.0), vec3(-1.0, 1.0, 1.0),
		vec3(1.0, 1.0, -1.0), vec3(-1.0, -1.0, -1.0), vec3(-1.0, 1.0, -1.0),
		vec3(1.0, -1.0, 1.0), vec3(-1.0, -1.0, -1.0), vec3(1.0, -1.0, -1.0),
		vec3(1.0, 1.0, -1.0), vec3(1.0, -1.0, -1.0), vec3(-1.0, -1.0, -1.0),
		vec3(-1.0, -1.0, -1.0), vec3(-1.0, 1.0, 1.0), vec3(-1.0, 1.0, -1.0),
		vec3(1.0, -1.0, 1.0), vec3(-1.0, -1.0, 1.0), vec3(-1.0, -1.0, -1.0),
		vec3(-1.0, 1.0, 1.0), vec3(-1.0, -1.0, 1.0), vec3(1.0, -1.0, 1.0),
		vec3(1.0, 1.0, 1.0), vec3(1.0, -1.0, -1.0), vec3(1.0, 1.0, -1.0),
		vec3(1.0, -1.0, -1.0), vec3(1.0, 1.0, 1.0), vec3(1.0, -1.0, 1.0),
		vec3(1.0, 1.0, 1.0), vec3(1.0, 1.0, -1.0), vec3(-1.0, 1.0, -1.0),
		vec3(1.0, 1.0, 1.0), vec3(-1.0, 1.0, -1.0), vec3(-1.0, 1.0, 1.0),
		vec3(1.0, 1.0, 1.0), vec3(-1.0, 1.0, 1.0), vec3(1.0, -1.0, 1.0));

ivec3 cell_from_index(int index) {
	return ivec3(index % params.grid_size_mode.x,
			(index / params.grid_size_mode.x) % params.grid_size_mode.y,
			index / (params.grid_size_mode.x * params.grid_size_mode.y));
}

ivec2 atlas_coord(ivec3 cell) {
	return ivec2(cell.x + cell.z * params.grid_size_mode.x, cell.y);
}

vec4 fetch_field(texture2D field, ivec3 cell) {
	return texelFetch(sampler2D(field, tex_sampler), atlas_coord(cell), 0);
}

vec3 probe_position(ivec3 cell) {
	return params.grid_min_spacing.xyz + (vec3(cell) + 0.5) * params.grid_min_spacing.w;
}

vec3 sphere_direction(int vertex_index) {
	const int segments = 8;
	const int rings = 6;
	int triangle = vertex_index / 3;
	int corner = vertex_index % 3;
	int quad = triangle / 2;
	int triangle_half = triangle % 2;
	int longitude = quad % segments;
	int latitude = quad / segments;
	ivec2 offset = triangle_half == 0 ?
			(corner == 0 ? ivec2(0, 0) : (corner == 1 ? ivec2(1, 0) : ivec2(1, 1))) :
			(corner == 0 ? ivec2(0, 0) : (corner == 1 ? ivec2(1, 1) : ivec2(0, 1)));
	float u = float(longitude + offset.x) / float(segments);
	float v = float(latitude + offset.y) / float(rings);
	float theta = v * 3.14159265359;
	float phi = u * 6.28318530718;
	return vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

vec4 sh_basis(vec3 direction) {
	return vec4(C0, C1 * direction);
}

int field_index(ivec3 cell) {
	return cell.x + params.grid_size_mode.x * (cell.z + params.grid_size_mode.z * cell.y);
}

vec4 fetch_material(ivec3 cell) {
	return material_field.data[field_index(cell)];
}

vec3 fetch_rgb_sh(ivec3 cell, vec4 basis, bool source) {
	int probe = field_index(cell);
	if (source) {
		uint slot = record_indices.data[probe];
		if (slot == 0xffffffffu) {
			return vec3(0.0);
		}
		return vec3(dot(source_r.data[slot], basis), dot(source_g.data[slot], basis), dot(source_b.data[slot], basis));
	}
	return vec3(dot(radiance_r.data[probe], basis), dot(radiance_g.data[probe], basis), dot(radiance_b.data[probe], basis));
}

vec4 lobe_color(vec3 value) {
	vec3 positive = max(value, vec3(0.0));
	float negative = max(-min(value.r, min(value.g, value.b)), 0.0);
	vec3 mapped = positive / (vec3(1.0) + positive);
	return vec4(mapped + vec3(negative / (1.0 + negative), 0.0, negative / (1.0 + negative)), 1.0);
}

void hide_vertex() {
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
	color_interp = vec4(0.0);
}

void draw_lobe() {
	int cell_index = gl_InstanceIndex;
	ivec3 cell = cell_from_index(cell_index);
	if (fetch_material(cell).a > 0.5) {
		hide_vertex();
		return;
	}
	vec3 direction = sphere_direction(gl_VertexIndex);
	vec4 basis = sh_basis(direction);
	vec3 value;
	int mode = params.grid_size_mode.w & 255;
	if (mode == MODE_RADIANCE) {
		value = fetch_rgb_sh(cell, basis, false);
	} else if (mode == MODE_SOURCE) {
		value = fetch_rgb_sh(cell, basis, true);
	} else if (mode == MODE_SKY_VISIBILITY) {
		value = vec3(clamp(dot(visibility_field.data[field_index(cell)], vec4(3.14159265359 * C0, (2.0 * 3.14159265359 / 3.0) * C1 * direction)) / 3.14159265359, 0.0, 1.0)) * vec3(0.35, 0.75, 1.0);

	} else {
		value = vec3(0.0);
	}
	float magnitude = length(value);
	float radius = params.grid_min_spacing.w * (0.08 + 0.40 * magnitude / (1.0 + magnitude));
	vec3 position = probe_position(cell) + direction * radius;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = lobe_color(value);
}

void draw_voxel() {
	ivec3 cell = cell_from_index(gl_InstanceIndex);
	int activity = int(params.volume_max.w);
	bool local_update = (activity & 1) != 0 && fetch_field(dirty_field, cell).r > 0.5;
	bool source_update = (activity & 2) != 0 && record_indices.data[field_index(cell)] != 0xffffffffu &&
			all(greaterThanEqual(vec3(cell), params.volume_min.xyz)) && all(lessThan(vec3(cell), params.volume_max.xyz));
	bool solving = (activity & 12) != 0 && fetch_material(cell).a <= 0.5;
	if (!local_update && !source_update && !solving) {
		hide_vertex();
		return;
	}
	vec3 color = local_update ? vec3(1.0, 0.38, 0.03) : (source_update ? vec3(0.1, 1.0, 0.3) : vec3(0.15, 0.4, 1.0));
	float scale = 0.12;
	vec3 position = probe_position(cell) + cube_triangles[gl_VertexIndex] * params.grid_min_spacing.w * scale;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = vec4(color, 1.0);
}

void draw_link() {
	int cell_index = gl_InstanceIndex / 26;
	int direction_index = gl_InstanceIndex % 26;
	ivec3 cell = cell_from_index(cell_index);
	int packed_index = direction_index < 13 ? direction_index : direction_index + 1;
	ivec3 offset = ivec3(packed_index % 3 - 1, (packed_index / 3) % 3 - 1, packed_index / 9 - 1);
	int probe_index = cell.x + params.grid_size_mode.x * (cell.z + params.grid_size_mode.z * cell.y);
	uint bits = (params.grid_size_mode.w & 255) == MODE_SKY_VISIBILITY ? sky_mask.data[probe_index] : links.data[probe_index];
	if (fetch_material(cell).a > 0.5) {
		hide_vertex();
		return;
	}
	vec3 direction = normalize(vec3(offset));
	vec3 tangent = normalize(abs(direction.y) < 0.9 ? cross(direction, vec3(0.0, 1.0, 0.0)) : cross(direction, vec3(1.0, 0.0, 0.0)));
	vec3 bitangent = cross(direction, tangent);
	vec3 cube = cube_triangles[gl_VertexIndex];
	bool open = (bits & (1u << uint(direction_index))) != 0u;
	// Closed sky directions are represented by the derived dark visibility lobe.
	if ((params.grid_size_mode.w & 255) == MODE_SKY_VISIBILITY && !open) {
		hide_vertex();
		return;
	}
	float half_length = params.grid_min_spacing.w * (open ? 0.06 : 0.08);
	float half_width = params.grid_min_spacing.w * (open ? 0.009 : 0.022);
	vec3 center = probe_position(cell) + direction * params.grid_min_spacing.w * 0.24;
	vec3 position = center + direction * cube.x * half_length +
			tangent * cube.y * half_width + bitangent * cube.z * half_width;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = open ? vec4(0.12, 1.0, 0.38, 1.0) : vec4(1.0, 0.18, 0.08, 1.0);
}

void draw_boundary_sample() {
	ivec3 cell;
	ivec3 offset;
	boundary_connection(gl_InstanceIndex, params.grid_size_mode.xyz, cell, offset);
	vec4 sample_value = boundary.data[gl_InstanceIndex];
	vec3 color = sample_value.a > 0.5 ? sample_value.rgb / (vec3(1.0) + sample_value.rgb) : vec3(0.9, 0.15, 0.4);
	if (params.volume_max.w < 0.5) {
		color = vec3(1.0, 0.6, 0.05); // No complete native field is ready.
	}
	if (params.volume_max.w > 0.5 && sample_value.a > 0.5 && dot(sample_value.rgb, sample_value.rgb) == 0.0) {
		color = vec3(0.08); // Valid black differs from uncovered pink.
	}
	vec3 position = probe_position(cell) + vec3(offset) * params.grid_min_spacing.w * 0.35 +
			cube_triangles[gl_VertexIndex] * params.grid_min_spacing.w * 0.06;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = vec4(color, 1.0);
}

void draw_boundary_box() {
	vec3 minimum = params.volume_min.xyz;
	vec3 maximum = params.volume_max.xyz;
	if (gl_InstanceIndex == 1) {
		minimum += vec3(params.volume_min.w);
		maximum -= vec3(params.volume_min.w);
	}
	int edge = gl_VertexIndex / 36;
	vec3 cube = cube_triangles[gl_VertexIndex % 36];
	int axis = edge / 4;
	int corner = edge % 4;
	vec3 center = (minimum + maximum) * 0.5;
	vec3 scale = vec3(params.grid_min_spacing.w * 0.035);
	scale[axis] = (maximum[axis] - minimum[axis]) * 0.5;
	int axis_1 = (axis + 1) % 3;
	int axis_2 = (axis + 2) % 3;
	center[axis_1] = (corner & 1) == 0 ? minimum[axis_1] : maximum[axis_1];
	center[axis_2] = (corner & 2) == 0 ? minimum[axis_2] : maximum[axis_2];
	vec3 position = center + cube * scale;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = gl_InstanceIndex == 0 ? vec4(0.1, 0.85, 1.0, 1.0) : vec4(1.0, 0.78, 0.1, 1.0);
}

void main() {
	int draw_kind = params.grid_size_mode.w >> 8;
	if (draw_kind == DRAW_LOBES) {
		draw_lobe();
	} else if (draw_kind == DRAW_VOXELS) {
		draw_voxel();
	} else if (draw_kind == DRAW_LINKS) {
		draw_link();
	} else if (draw_kind == DRAW_BOUNDARY_SAMPLES) {
		draw_boundary_sample();
	} else {
		draw_boundary_box();
	}
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec4 color_interp;
layout(location = 0) out vec4 frag_color;

void main() {
	frag_color = color_interp;
}
