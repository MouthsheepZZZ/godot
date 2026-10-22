/**************************************************************************/
/*  lrt_cache.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "lrt_core.h"
#include "lrt_transfer.h"

#include "core/string/ustring.h"
#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_server_enums.h"

#include <memory>

class Mesh;
class MeshInstance3D;

namespace lrt {

struct TransferCoverage;

// Reads texture content for persistent keys; live global uniforms use their revision instead.
uint64_t texture_content_signature(RID p_texture, RSE::TextureType p_type);

// Content signature of one baked asset: positions in asset space, effective precision and
// algorithm version. Material data is deliberately excluded and lives in the instance field.
uint64_t asset_signature(const std::vector<MeshTriangle> &p_triangles, int p_resolution);

// Asset-local geometry precision: an optional node override, otherwise the project default.
inline constexpr char VOXEL_CELL_SIZE_META[] = "_lrt_voxel_cell_size";
double geometry_cell_size(const Object *p_instance = nullptr);
int estimate_mesh_geometry_resolution(const std::vector<MeshTriangle> &p_triangles);

// Packed dependencies are immutable. Runtime-generated data in an exported project always
// goes to user://, even when the directory containing the executable is writable.
String asset_cache_directory();
String asset_cache_path(uint64_t p_signature);
String packed_asset_path(uint64_t p_signature);
// Persist native mesh-local capture bytes so runtime edits can reuse prebuilt voxels.
String static_voxel_capture_path(uint64_t p_signature);
bool load_static_voxel_capture(uint64_t p_signature, Dictionary &r_images);
bool store_static_voxel_capture(uint64_t p_signature, const Dictionary &p_images);
bool mesh_triangles(const Ref<Mesh> &p_mesh, std::vector<MeshTriangle> &r_triangles, const Vector<int> *p_surfaces = nullptr);
uint64_t mesh_deformation_signature(MeshInstance3D *p_instance);
Dictionary prepare_mesh_asset(const Ref<Mesh> &p_mesh, int p_resolution);
bool load_asset_field(uint64_t p_signature, CoarseGeometryField &r_field);
bool store_asset_field(uint64_t p_signature, const CoarseGeometryField &p_field);
String instance_cache_path(uint64_t p_signature);
String packed_instance_path(uint64_t p_signature);
bool load_instance_field(uint64_t p_signature, SurfaceMaterialField &r_field);
bool store_instance_field(uint64_t p_signature, const SurfaceMaterialField &p_field);

enum GeometryRepresentation {
	GEOMETRY_VOXEL,
	GEOMETRY_PRECOMPUTED_TRANSFER,
};
inline constexpr char REPRESENTATION_META[] = "_lrt_representation";
inline constexpr char TRANSFER_MANIFEST_META[] = "_lrt_transfer_manifest";
inline constexpr char TRANSFER_RADIUS_META[] = "_lrt_transfer_radius";
inline constexpr char TRANSFER_SPACING_META[] = "_lrt_transfer_spacing";
inline constexpr char TRANSFER_ANGULAR_SAMPLES_META[] = "_lrt_transfer_angular_samples";
inline constexpr char OCCUPANCY_BAND_WIDTH_META[] = "_lrt_occupancy_band_width";
GeometryRepresentation geometry_representation(const Object *p_instance);
TransferBakeSettings transfer_bake_settings(const Object *p_instance = nullptr);
double occupancy_band_width(const Object *p_instance = nullptr);
uint64_t transfer_configuration_signature(const Object *p_instance);
uint64_t transfer_bake_signature(const TransferBakeSettings &p_settings);
uint64_t transfer_visibility_signature(uint64_t p_geometry, uint64_t p_coverage, const TransferBakeSettings &p_settings);
uint64_t transfer_material_signature(uint64_t p_visibility, uint64_t p_reflectance, uint64_t p_emission);

// Precomputed transfer dependencies have independent effective-coverage and material
// identities. A missing or stale payload is an error, never a request to rebake.
String transfer_cache_path(uint64_t p_signature, bool p_visibility);
String packed_transfer_path(uint64_t p_signature, bool p_visibility);
bool load_transfer_field(uint64_t p_signature, bool p_visibility, TransferField &r_field);
bool load_transfer_instance(uint64_t p_visibility, uint64_t p_material, const TransferBakeSettings &p_settings, TransferInstance &r_instance);
bool store_transfer_field(uint64_t p_signature, bool p_visibility, const TransferField &p_field);
String transfer_coverage_path(uint64_t p_signature);
String packed_transfer_coverage_path(uint64_t p_signature);
bool store_transfer_coverage(uint64_t p_signature, const TransferCoverage &p_coverage);
std::shared_ptr<const TransferCoverage> load_transfer_coverage(uint64_t p_signature);
std::shared_ptr<const TransferVisibilityField> find_shared_transfer_visibility(uint64_t p_signature);
std::shared_ptr<const TransferVisibilityField> share_transfer_visibility(uint64_t p_signature, const TransferField &p_field);
std::shared_ptr<const TransferMaterialField> find_shared_transfer_material(uint64_t p_signature);
std::shared_ptr<const TransferMaterialField> share_transfer_material(uint64_t p_signature, const TransferField &p_field);

// Process-wide immutable storage shared by every LRTVolume. The returned pointer is the
// canonical allocation for a specification, including when two volumes prepare it at once.
std::shared_ptr<const CoarseGeometryField> find_shared_asset_field(uint64_t p_signature);
std::shared_ptr<const CoarseGeometryField> share_asset_field(uint64_t p_signature, CoarseGeometryField p_field);
uint64_t instance_field_cache_signature(uint64_t p_geometry_signature, uint64_t p_material_signature);
std::shared_ptr<const SurfaceMaterialField> find_shared_instance_field(uint64_t p_signature);
std::shared_ptr<const SurfaceMaterialField> share_instance_field(uint64_t p_signature, SurfaceMaterialField p_field);
void clear_shared_asset_fields();
uint64_t asset_field_bytes(const CoarseGeometryField &p_field);

// Native asset-lattice samples within half a voxel diagonal of the zero surface.
// Each record is asset-local XYZ plus signed distance in voxel units.
std::vector<float> sdf_surface_cells(const CoarseGeometryField &p_field);

// Surface Probe diagnostics derived from RGB transfer DC or mean HDR EmissionSH DC.
std::vector<float> local_material_rgba(const LocalField &p_field, bool p_emission);

} // namespace lrt
