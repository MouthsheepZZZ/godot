#[compute]

#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer DirectOutgoing {
	vec4 data[];
}
direct_outgoing;

layout(set = 0, binding = 1, std430) restrict readonly buffer Albedo {
	vec4 data[];
}
albedo;

layout(set = 0, binding = 2, std430) restrict readonly buffer Normal {
	vec4 data[];
}
surface_normal;

layout(set = 0, binding = 3, std430) restrict readonly buffer Links {
	vec4 data[];
}
links;

struct SH4 {
	vec4 red;
	vec4 green;
	vec4 blue;
};

layout(set = 0, binding = 4, std430) restrict readonly buffer CurrentRadiance {
	SH4 data[];
}
current_radiance;

layout(set = 0, binding = 5, std430) restrict writeonly buffer NextRadiance {
	SH4 data[];
}
next_radiance;

layout(set = 0, binding = 6, std430) restrict readonly buffer SkyVisibility {
	vec4 data[];
}
sky_visibility;

layout(push_constant, std430) uniform Params {
	uint cell_count;
	uint ray_count;
	float bounce_feedback;
	float sky_energy;
}
params;

const float PI = 3.14159265359;
const float SH_L0 = 0.28209479177;
const float SH_L1 = 0.48860251190;

vec4 sh_basis(vec3 direction) {
	return vec4(SH_L0, SH_L1 * direction.x, SH_L1 * direction.y, SH_L1 * direction.z);
}

vec3 evaluate_irradiance(SH4 value, vec3 normal) {
	vec4 convolved_basis = vec4(SH_L0, (2.0 / 3.0) * SH_L1 * normal.x, (2.0 / 3.0) * SH_L1 * normal.y, (2.0 / 3.0) * SH_L1 * normal.z);
	return max(vec3(dot(value.red, convolved_basis), dot(value.green, convolved_basis), dot(value.blue, convolved_basis)), vec3(0.0));
}

void main() {
	uint receiver = gl_GlobalInvocationID.x;
	if (receiver >= params.cell_count) {
		return;
	}
	vec3 receiver_normal = surface_normal.data[receiver].xyz;
	if (surface_normal.data[receiver].w < 0.5) {
		next_radiance.data[receiver].red = vec4(0.0);
		next_radiance.data[receiver].green = vec4(0.0);
		next_radiance.data[receiver].blue = vec4(0.0);
		return;
	}
	SH4 result;
	result.red = vec4(0.0);
	result.green = vec4(0.0);
	result.blue = vec4(0.0);
	for (uint ray = 0; ray < params.ray_count; ray++) {
		vec4 link = links.data[receiver * params.ray_count + ray];
		uint source = floatBitsToUint(link.w);
		if (source == 0xffffffffu) {
			continue;
		}
		vec3 direction = link.xyz;
		vec3 source_normal_value = surface_normal.data[source].xyz;
		if (dot(source_normal_value, -direction) <= 0.0) {
			continue;
		}
		float source_sky_visibility = max(dot(sky_visibility.data[source], vec4(SH_L0, (2.0 / 3.0) * SH_L1 * source_normal_value)), 0.0);
		vec3 incoming = evaluate_irradiance(current_radiance.data[source], source_normal_value) + vec3(source_sky_visibility * params.sky_energy);
		vec3 outgoing = direct_outgoing.data[source].rgb + albedo.data[source].rgb * incoming * params.bounce_feedback;
		vec4 basis = sh_basis(direction) * (2.0 * PI / float(params.ray_count));
		result.red += basis * outgoing.r;
		result.green += basis * outgoing.g;
		result.blue += basis * outgoing.b;
	}
	next_radiance.data[receiver] = result;
}
