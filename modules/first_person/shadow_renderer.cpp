#include "shadow_renderer.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace FirstPerson {
void ShadowBuffers::configure(RenderSceneBuffersRD *p_buffers) {
	p_buffers->clear_context(SNAME("first_person_shadow"));
	size = Size2i();
	layers = 0;
}
void ShadowBuffers::free_data() {
	if (mapping.is_valid()) {
		RD::get_singleton()->free_rid(mapping);
		mapping = RID();
		mapping_bytes = 0;
	}
	if (lights.is_valid()) {
		RD::get_singleton()->free_rid(lights);
		lights = RID();
		light_bytes = 0;
	}
}
ShadowBuffers::~ShadowBuffers() {
	free_data();
}

ShadowRenderer::ShadowRenderer() {
	Vector<String> modes;
	modes.push_back("#define MODE_BLOCKS\n");
	modes.push_back("#define MODE_SURFACE\n");
	modes.push_back("#define MODE_TRACE\n");
	String defines;
#ifdef REAL_T_IS_DOUBLE
	defines = "#define USE_DOUBLE_PRECISION\n";
#endif
	shader.initialize(modes, defines);
	version = shader.version_create();
	for (int i = 0; i < 3; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(version, i));
	}
	uint32_t zero[4] = {};
	empty_mapping = RD::get_singleton()->storage_buffer_create(sizeof(zero), Span((uint8_t *)zero, sizeof(zero)));
}
ShadowRenderer::~ShadowRenderer() {
	for (int i = 0; i < 3; i++) {
		pipelines[i].free();
	}
	shader.version_free(version);
	RD::get_singleton()->free_rid(empty_mapping);
}

void ShadowRenderer::render(const Ref<RenderSceneBuffersRD> &rb, const RenderSceneDataRD *scene, RID scene_uniform, uint32_t directional_count, const AABB &bounds) {
	RD *rd = RD::get_singleton();
	auto *storage = RendererRD::LightStorage::get_singleton();
	Ref<ShadowBuffers> data;
	if (rb->has_custom_data(SNAME("first_person_shadow"))) {
		data = rb->get_custom_data(SNAME("first_person_shadow"));
	}
	if (data.is_null()) {
		data.instantiate();
		rb->set_custom_data(SNAME("first_person_shadow"), data);
	}
	uint32_t divisor = bool(GLOBAL_GET("rendering/lights_and_shadows/first_person_shadow/half_resolution")) ? 2 : 1;
	uint32_t entries = MAX(MAX(storage->get_omni_light_count(), storage->get_spot_light_count()), MAX(storage->get_area_light_count(), directional_count));
	Vector<uint32_t> mapping;
	mapping.resize(4 + entries * 4);
	mapping.fill(0);
	struct Light {
		float position_range[4] = {};
		float direction_type[4] = {};
		float cone[4] = {};
	};
	Vector<Light> lights;
	if (scene->first_person.z > 0.0f) {
		for (uint32_t i = 0; i < directional_count; i++) {
			const auto &source = storage->get_directional_light_data()[i];
			if (source.shadow_opacity <= 0.0f) {
				continue;
			}
			Light light;
			for (int axis = 0; axis < 3; axis++) {
				light.direction_type[axis] = source.direction[axis];
			}
			lights.push_back(light);
			mapping.write[4 + i * 4] = lights.size();
		}
		for (uint32_t type = 1; type < 4; type++) {
			uint32_t count = type == 1 ? storage->get_omni_light_count() : (type == 2 ? storage->get_spot_light_count() : storage->get_area_light_count());
			const auto *sources = type == 1 ? storage->get_omni_light_data() : (type == 2 ? storage->get_spot_light_data() : storage->get_area_light_data());
			for (uint32_t i = 0; i < count; i++) {
				const auto &source = sources[i];
				if (source.shadow_opacity <= 0.0f) {
					continue;
				}
				float range = 1.0f / (type == 3 ? source.inv_spot_attenuation : source.inv_radius);
				Vector3 position(source.position[0], source.position[1], source.position[2]);
				Vector3 extent(range, range, range);
				if (!bounds.intersects(AABB(position - extent, extent * 2.0f))) {
					continue;
				}
				Light light;
				for (int axis = 0; axis < 3; axis++) {
					light.position_range[axis] = source.position[axis];
					light.direction_type[axis] = source.direction[axis];
				}
				light.position_range[3] = range;
				light.direction_type[3] = type;
				light.cone[0] = source.cos_spot_angle;
				lights.push_back(light);
				mapping.write[4 + i * 4 + type] = lights.size();
			}
		}
	}
	mapping.write[0] = entries;
	mapping.write[1] = lights.size();
	mapping.write[2] = rb->get_view_count();
	mapping.write[3] = divisor;
	auto upload = [rd](RID &buffer, uint32_t &capacity, const void *bytes, uint32_t size) {
		if (capacity < size) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
			capacity = Math::next_power_of_2(size);
			buffer = rd->storage_buffer_create(capacity);
		}
		rd->buffer_update(buffer, 0, size, bytes);
	};
	upload(data->mapping, data->mapping_bytes, mapping.ptr(), mapping.size() * sizeof(uint32_t));
	if (lights.is_empty()) {
		rb->clear_context(SNAME("first_person_shadow"));
		data->size = Size2i();
		data->layers = 0;
		return;
	}
	upload(data->lights, data->light_bytes, lights.ptr(), lights.size() * sizeof(Light));
	Size2i full_size = rb->get_internal_size();
	Size2i size((full_size.x + divisor - 1) / divisor, (full_size.y + divisor - 1) / divisor);
	uint32_t layers = lights.size() * rb->get_view_count();
	if (data->size != size || data->layers != layers) {
		rb->clear_context(SNAME("first_person_shadow"));
		data->size = size;
		data->layers = layers;
	}
	uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	rb->create_texture(SNAME("first_person_shadow"), SNAME("blocks"), RD::DATA_FORMAT_R32G32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, Size2i((full_size.x + 7) / 8, (full_size.y + 7) / 8));
	rb->create_texture(SNAME("first_person_shadow"), SNAME("receivers"), RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size);
	rb->create_texture(SNAME("first_person_shadow"), SNAME("normals"), RD::DATA_FORMAT_R16G16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size);
	RD::TextureFormat format;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.format = RD::DATA_FORMAT_R8_UNORM;
	format.width = size.x;
	format.height = size.y;
	format.array_layers = layers;
	format.usage_bits = usage;
	RID masks = rb->create_texture_from_format(SNAME("first_person_shadow"), SNAME("masks"), format);
	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	struct Params {
		float distance;
		float thickness;
		uint32_t block_skipping;
		uint32_t divisor;
		uint32_t view;
		uint32_t views;
		uint32_t lights;
		uint32_t pad = 0;
	} params = { float(scene->first_person.z), float(scene->first_person.w), uint32_t(bool(GLOBAL_GET("rendering/lights_and_shadows/first_person_shadow/block_skipping"))), divisor, 0, rb->get_view_count(), uint32_t(lights.size()) };

	for (uint32_t view = 0; view < rb->get_view_count(); view++) {
		params.view = view;
		RID depth = rb->get_texture_slice(SNAME("first_person"), SNAME("depth"), view, 0);
		RID blocks = rb->get_texture_slice(SNAME("first_person_shadow"), SNAME("blocks"), view, 0);
		RID receivers = rb->get_texture_slice(SNAME("first_person_shadow"), SNAME("receivers"), view, 0);
		RID normals = rb->get_texture_slice(SNAME("first_person_shadow"), SNAME("normals"), view, 0);
		RD::ComputeListID list = rd->compute_list_begin();
		for (int mode = 0; mode < 3; mode++) {
			if (mode == 0 && !params.block_skipping) {
				continue;
			}
			RID shader_rid = shader.version_get_shader(version, mode);
			LocalVector<RD::Uniform> inputs;
			inputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, scene_uniform));
			inputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { sampler, depth }));
			LocalVector<RD::Uniform> outputs;
			if (mode == 0) {
				outputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, blocks));
			} else if (mode == 1) {
				outputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, receivers));
				outputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, normals));
			} else {
				inputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { sampler, blocks }));
				inputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { sampler, receivers }));
				inputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { sampler, normals }));
				inputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, data->lights));
				outputs.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, masks));
			}
			rd->compute_list_bind_compute_pipeline(list, pipelines[mode].get_rid());
			rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, inputs), 0);
			rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 1, outputs), 1);
			rd->compute_list_set_push_constant(list, &params, sizeof(params));
			if (mode == 0) {
				rd->compute_list_dispatch(list, (full_size.x + 7) / 8, (full_size.y + 7) / 8, 1);
			} else {
				rd->compute_list_dispatch_threads(list, size.x, size.y, mode == 2 ? lights.size() : 1);
			}
			rd->compute_list_add_barrier(list);
		}
		rd->compute_list_end();
	}
}
} //namespace FirstPerson
