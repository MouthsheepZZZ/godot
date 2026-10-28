const float LRT_C0 = 0.28209479177387814;
const float LRT_C1 = 0.4886025119029199;

vec4 lrt_product(vec4 a, vec4 b) {
	return LRT_C0 * vec4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
}

vec4 lrt_flip(vec4 a) {
	return a * vec4(1.0, -1.0, -1.0, -1.0);
}

// Preserve projected directionality; zero mean energy has no light contribution.
vec4 lrt_constrain_light(vec4 a) {
	if (a.x <= 0.0) {
		return vec4(0.0);
	}
	return a;
}

// Bound Lambert response, not radiance: C0*a.x - (2/3)*C1*|a.yzw| >= 0.
vec4 lrt_constrain_diffuse(vec4 a) {
	a = lrt_constrain_light(a);
	float directional = length(a.yzw);
	float limit = (1.5 * LRT_C0 / LRT_C1) * a.x;
	if (directional > limit) {
		a.yzw *= limit / directional;
	}
	return a;
}

vec4 lrt_constrain_visibility(vec4 a) {
	float mean = clamp(LRT_C0 * a.x, 0.0, 1.0);
	a.x = mean / LRT_C0;
	float directional = length(a.yzw);
	float limit = min(mean, 1.0 - mean) / LRT_C1;
	if (directional > limit) {
		a.yzw *= limit / directional;
	}
	return a;
}

vec4 lrt_visible_sky(vec4 sky, vec4 visibility) {
	return lrt_flip(lrt_constrain_light(lrt_product(lrt_constrain_light(sky), lrt_constrain_visibility(visibility))));
}

vec4 lrt_reflect(mat4 transfer, vec4 incoming) {
	return lrt_constrain_diffuse(transfer * lrt_constrain_light(incoming));
}

// Match reflected outgoing moments to the neighbor transport kernel, preserving DC.
// Do not apply this directional factor to free-space transport or injected sources.
vec4 lrt_filter_radiance(vec4 light) {
	return lrt_constrain_light(light) * vec4(1.0, 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
}
