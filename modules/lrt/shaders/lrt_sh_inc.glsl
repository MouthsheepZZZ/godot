const float LRT_C0 = 0.28209479177387814;
const float LRT_C1 = 0.4886025119029199;

vec4 lrt_product(vec4 a, vec4 b) {
	return LRT_C0 * vec4(dot(a, b), a.x * b.yzw + b.x * a.yzw);
}

vec4 lrt_flip(vec4 a) {
	return a * vec4(1.0, -1.0, -1.0, -1.0);
}
