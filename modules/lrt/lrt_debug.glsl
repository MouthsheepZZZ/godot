#[vertex]

#version 450

#VERSION_DEFINES

layout(set = 0, binding = 0) uniform sampler tex_sampler;
layout(set = 0, binding = 1) uniform texture2D radiance_r;
layout(set = 0, binding = 2) uniform texture2D radiance_g;
layout(set = 0, binding = 3) uniform texture2D radiance_b;
layout(set = 0, binding = 4) uniform texture2D source_r;
layout(set = 0, binding = 5) uniform texture2D source_g;
layout(set = 0, binding = 6) uniform texture2D source_b;
layout(set = 0, binding = 7) uniform texture2D visibility_field;
layout(set = 0, binding = 8) uniform texture2D local_visibility_field;
layout(set = 0, binding = 9) uniform texture2D matrix_field;
layout(set = 0, binding = 10) uniform texture2D links_field;
layout(set = 0, binding = 11) uniform texture2D sdf_field;
layout(set = 0, binding = 12) uniform texture2D albedo_field;
layout(set = 0, binding = 13) uniform texture2D emission_field;
layout(set = 0, binding = 14) uniform texture2D dirty_field;
layout(set = 0, binding = 15) uniform texture2D material_field;

layout(set = 0, binding = 16, std430) readonly buffer ExternalRedBuffer {
	float data[];
} external_red;
layout(set = 0, binding = 17, std430) readonly buffer ExternalGreenBuffer {
	float data[];
} external_green;
layout(set = 0, binding = 18, std430) readonly buffer ExternalBlueBuffer {
	float data[];
} external_blue;
layout(set = 0, binding = 19, std430) readonly buffer ReceiverBuffer {
	float data[];
} receivers;

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
const int MODE_LOCAL_VISIBILITY = 2;
const int MODE_GLOBAL_VISIBILITY = 3;
const int MODE_TRANSFER = 4;
const int MODE_SDF_SURFACE = 5;
const int MODE_ALBEDO = 6;
const int MODE_EMISSION = 7;
const int MODE_BOUNDARY = 8;
const int MODE_UPDATE_REGIONS = 9;
const int DRAW_LOBES = 0;
const int DRAW_VOXELS = 1;
const int DRAW_RECEIVERS = 2;
const int DRAW_LINKS = 3;
const int DRAW_BOUNDARY_BOXES = 4;

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

vec3 fetch_rgb_sh(texture2D red_field, texture2D green_field, texture2D blue_field, ivec3 cell, vec4 basis) {
	return vec3(dot(fetch_field(red_field, cell), basis),
			dot(fetch_field(green_field, cell), basis),
			dot(fetch_field(blue_field, cell), basis));
}

vec3 fetch_external_sh(int cell_index, vec4 basis) {
	int offset = cell_index * 4;
	vec4 red = vec4(external_red.data[offset], external_red.data[offset + 1], external_red.data[offset + 2], external_red.data[offset + 3]);
	vec4 green = vec4(external_green.data[offset], external_green.data[offset + 1], external_green.data[offset + 2], external_green.data[offset + 3]);
	vec4 blue = vec4(external_blue.data[offset], external_blue.data[offset + 1], external_blue.data[offset + 2], external_blue.data[offset + 3]);
	return vec3(dot(red, basis), dot(green, basis), dot(blue, basis));
}

vec3 transfer_response(ivec3 cell, vec4 basis) {
	vec3 response = vec3(0.0);
	for (int channel = 0; channel < 3; channel++) {
		vec4 projected;
		for (int row = 0; row < 4; row++) {
			ivec2 coordinate = ivec2(atlas_coord(cell).x,
					cell.y + (channel * 4 + row) * params.grid_size_mode.y);
			projected[row] = dot(texelFetch(sampler2D(matrix_field, tex_sampler), coordinate, 0), basis);
		}
		response[channel] = dot(basis, projected);
	}
	return response;
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
	if (fetch_field(material_field, cell).a > 0.5) {
		hide_vertex();
		return;
	}
	vec3 direction = sphere_direction(gl_VertexIndex);
	vec4 basis = sh_basis(direction);
	vec3 value;
	int mode = params.grid_size_mode.w & 255;
	if (mode == MODE_RADIANCE) {
		value = fetch_rgb_sh(radiance_r, radiance_g, radiance_b, cell, basis);
	} else if (mode == MODE_SOURCE) {
		value = fetch_rgb_sh(source_r, source_g, source_b, cell, basis);
	} else if (mode == MODE_LOCAL_VISIBILITY) {
		value = vec3(clamp(dot(fetch_field(local_visibility_field, cell), basis), 0.0, 1.0)) * vec3(0.35, 1.0, 0.55);
	} else if (mode == MODE_GLOBAL_VISIBILITY) {
		value = vec3(clamp(dot(fetch_field(visibility_field, cell), basis), 0.0, 1.0)) * vec3(0.35, 0.75, 1.0);
	} else if (mode == MODE_TRANSFER) {
		value = transfer_response(cell, basis);
	} else {
		bool boundary = cell.x == 0 || cell.y == 0 || cell.z == 0 ||
				cell.x == params.grid_size_mode.x - 1 || cell.y == params.grid_size_mode.y - 1 || cell.z == params.grid_size_mode.z - 1;
		if (!boundary) {
			hide_vertex();
			return;
		}
		value = fetch_external_sh(cell_index, basis);
	}
	float magnitude = length(value);
	float radius = params.grid_min_spacing.w * (0.08 + 0.40 * magnitude / (1.0 + magnitude));
	vec3 position = probe_position(cell) + direction * radius;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = lobe_color(value);
}

void draw_voxel() {
	ivec3 cell = cell_from_index(gl_InstanceIndex);
	vec3 color;
	int mode = params.grid_size_mode.w & 255;
	if (mode == MODE_SDF_SURFACE) {
		vec4 sdf = fetch_field(sdf_field, cell);
		if (sdf.a < 0.5 || abs(sdf.r) > params.grid_min_spacing.w * 1.5) {
			hide_vertex();
			return;
		}
		float fade = 1.0 - 0.6 * clamp(abs(sdf.r) / max(params.grid_min_spacing.w * 1.5, 0.0001), 0.0, 1.0);
		color = sdf.r < 0.0 ? vec3(0.9, 0.16, 0.08) : vec3(0.08, 0.3, 0.95);
		color *= fade;
	} else if (mode == MODE_ALBEDO) {
		vec4 sample_value = fetch_field(albedo_field, cell);
		if (sample_value.a < 0.5) {
			hide_vertex();
			return;
		}
		color = sample_value.rgb;
	} else if (mode == MODE_EMISSION) {
		vec4 sample_value = fetch_field(emission_field, cell);
		if (sample_value.a < 0.5 || dot(sample_value.rgb, sample_value.rgb) <= 0.000001) {
			hide_vertex();
			return;
		}
		color = sample_value.rgb / (vec3(1.0) + sample_value.rgb);
	} else {
		if (fetch_field(dirty_field, cell).r <= 0.5) {
			hide_vertex();
			return;
		}
		color = vec3(1.0, 0.38, 0.03);
	}
	float scale = mode == MODE_UPDATE_REGIONS ? 0.12 : 0.38;
	vec3 position = probe_position(cell) + cube_triangles[gl_VertexIndex] * params.grid_min_spacing.w * scale;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = vec4(color, 1.0);
}

void draw_receiver() {
	int offset = gl_InstanceIndex * 12;
	vec3 center = vec3(receivers.data[offset], receivers.data[offset + 1], receivers.data[offset + 2]);
	vec3 normal = normalize(vec3(receivers.data[offset + 4], receivers.data[offset + 5], receivers.data[offset + 6]));
	vec3 direction = sphere_direction(gl_VertexIndex);
	float response = max(dot(direction, normal), 0.0);
	vec3 position = center + direction * params.grid_min_spacing.w * (0.05 + 0.38 * response);
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = vec4(mix(vec3(0.08, 0.02, 0.0), vec3(1.0, 0.82, 0.15), response), 1.0);
}

void draw_link() {
	int cell_index = gl_InstanceIndex / 26;
	int direction_index = gl_InstanceIndex % 26;
	ivec3 cell = cell_from_index(cell_index);
	int packed_index = direction_index < 13 ? direction_index : direction_index + 1;
	ivec3 offset = ivec3(packed_index % 3 - 1, (packed_index / 3) % 3 - 1, packed_index / 9 - 1);
	vec2 packed_links = fetch_field(links_field, cell).rg;
	uint bits = direction_index < 13 ? uint(packed_links.r + 0.5) : uint(packed_links.g + 0.5);
	if ((bits & (1u << uint(direction_index % 13))) == 0u) {
		hide_vertex();
		return;
	}
	vec3 direction = normalize(vec3(offset));
	vec3 tangent = normalize(abs(direction.y) < 0.9 ? cross(direction, vec3(0.0, 1.0, 0.0)) : cross(direction, vec3(1.0, 0.0, 0.0)));
	vec3 bitangent = cross(direction, tangent);
	vec3 cube = cube_triangles[gl_VertexIndex];
	vec3 center = probe_position(cell) + direction * params.grid_min_spacing.w * 0.25;
	vec3 position = center + direction * cube.x * params.grid_min_spacing.w * 0.25 +
			tangent * cube.y * params.grid_min_spacing.w * 0.018 + bitangent * cube.z * params.grid_min_spacing.w * 0.018;
	gl_Position = params.projection * vec4(position, 1.0);
	color_interp = vec4(0.12, 1.0, 0.38, 1.0);
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
	} else if (draw_kind == DRAW_RECEIVERS) {
		draw_receiver();
	} else if (draw_kind == DRAW_LINKS) {
		draw_link();
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
