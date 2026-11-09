#include "first_person.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/visual_instance_3d.h"

void GeometryInstance3D::set_first_person_enabled(bool p_enabled) {
	first_person_enabled = p_enabled;
	FirstPerson::set_instance(get_instance(), p_enabled);
	set_cast_shadows_setting(get_cast_shadows_setting());
	set_gi_mode(get_gi_mode());
	update_configuration_warnings();
}

void GeometryInstance3D::_bind_first_person_methods() {
	ClassDB::bind_method(D_METHOD("set_first_person_enabled", "enabled"), &GeometryInstance3D::set_first_person_enabled);
	ClassDB::bind_method(D_METHOD("is_first_person_enabled"), &GeometryInstance3D::is_first_person_enabled);
	ADD_GROUP("First Person", "first_person_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "first_person_enabled"), "set_first_person_enabled", "is_first_person_enabled");
	ADD_GROUP("", "");
}

void Camera3D::set_first_person_enabled(bool p_enabled) {
	first_person.enabled = p_enabled;
	FirstPerson::set_camera(camera, first_person);
}

void Camera3D::set_first_person_fov(float p_fov) {
	ERR_FAIL_COND(!Math::is_finite(p_fov) || p_fov < 1.0f || p_fov > 179.0f);
	first_person.fov = p_fov;
	FirstPerson::set_camera(camera, first_person);
}

void Camera3D::set_first_person_scale(float p_scale) {
	ERR_FAIL_COND(!Math::is_finite(p_scale) || p_scale <= 0.0f || p_scale > 1.0f);
	first_person.scale = p_scale;
	FirstPerson::set_camera(camera, first_person);
}

void Camera3D::_bind_first_person_methods() {
	ClassDB::bind_method(D_METHOD("set_first_person_enabled", "enabled"), &Camera3D::set_first_person_enabled);
	ClassDB::bind_method(D_METHOD("is_first_person_enabled"), &Camera3D::is_first_person_enabled);
	ClassDB::bind_method(D_METHOD("set_first_person_fov", "fov"), &Camera3D::set_first_person_fov);
	ClassDB::bind_method(D_METHOD("get_first_person_fov"), &Camera3D::get_first_person_fov);
	ClassDB::bind_method(D_METHOD("set_first_person_scale", "scale"), &Camera3D::set_first_person_scale);
	ClassDB::bind_method(D_METHOD("get_first_person_scale"), &Camera3D::get_first_person_scale);
	ADD_GROUP("First Person", "first_person_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "first_person_enabled"), "set_first_person_enabled", "is_first_person_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "first_person_fov", PROPERTY_HINT_RANGE, "1,179,0.1,degrees"), "set_first_person_fov", "get_first_person_fov");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "first_person_scale", PROPERTY_HINT_RANGE, "0.001,1,0.001"), "set_first_person_scale", "get_first_person_scale");
	ADD_GROUP("", "");
}
