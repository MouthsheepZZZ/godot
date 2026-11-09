#pragma once

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

#include "modules/first_person/shadow_compute.glsl.gen.h"

namespace FirstPerson {
class ShadowBuffers : public RenderBufferCustomDataRD {
	GDCLASS(ShadowBuffers, RenderBufferCustomDataRD);

public:
	RID mapping;
	RID lights;
	uint32_t mapping_bytes = 0;
	uint32_t light_bytes = 0;
	Size2i size;
	uint32_t layers = 0;
	virtual void configure(RenderSceneBuffersRD *p_buffers) override;
	virtual void free_data() override;
	~ShadowBuffers();
};

class ShadowRenderer {
	ShadowComputeShaderRD shader;
	RID version;
	PipelineDeferredRD pipelines[3];
	RID empty_mapping;

public:
	ShadowRenderer();
	~ShadowRenderer();
	RID get_empty_mapping() const { return empty_mapping; }
	void render(const Ref<RenderSceneBuffersRD> &p_buffers, const RenderSceneDataRD *p_scene, RID p_scene_uniform, uint32_t directional_count, const AABB &p_bounds);
};
} //namespace FirstPerson
