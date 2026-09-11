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

#include "core/string/ustring.h"

namespace lrt {

// Content signature of one baked asset: the triangle soup in the asset's own space plus the
// SDF resolution. Editing a mesh (vertices, normals or colours) changes it, so the derived
// cache below can never serve a stale field.
uint64_t asset_signature(const std::vector<MeshTriangle> &p_triangles, int p_resolution);

// Derived cache of baked Color SDF fields. Files live under the project's .godot/lrt while
// that directory is writable (editor and dev builds) and under user://lrt_cache when it is not
// (an exported game, where res:// is the read-only PCK).
String asset_cache_directory();
bool load_asset_field(uint64_t p_signature, ColorSdfField &r_field);
bool store_asset_field(uint64_t p_signature, const ColorSdfField &p_field);

} // namespace lrt
