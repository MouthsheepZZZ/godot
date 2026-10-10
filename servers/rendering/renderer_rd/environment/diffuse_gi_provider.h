#pragma once

#include "core/math/transform_3d.h"
#include "core/math/vector3i.h"
#include "core/templates/rid.h"

namespace RendererRD {
// Optional GPU diffuse-field provider. Coefficients are unexposed linear RGB
// incident radiance in real SH order (Y00, Y1x, Y1y, Y1z), in sample_to_world's
// local basis. Providers must exclude sky when requested; they must never
// silently return a field with different energy ownership.
class DiffuseGIProvider {
public:
	struct BoundaryRequest {
		Transform3D sample_to_world;
		Vector3 grid_min;
		Vector3i grid_size;
		float spacing = 1.0f;
		RID coefficients[3];
		RID validity; // One uint per probe: 0 is uncovered, 1 includes valid black.
		bool exclude_sky = true;
	};
	virtual bool includes_sky() const = 0;
	virtual bool sample_boundary(const BoundaryRequest &p_request, const Vector3 &p_camera_origin) = 0;
	virtual ~DiffuseGIProvider() = default;
};
} // namespace RendererRD
