/**************************************************************************/
/*  lrt_material_baker.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
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
/* The above copyright notice and this permission notice shall be         */
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

#include "core/io/image.h"
#include "core/math/vector3.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"

class GeometryInstance3D;

// Uses the native Forward+ material pass; no material language is interpreted here.
class LRTMaterialBaker {
public:
	class Context : public RefCounted {
		GDSOFTCLASS(Context, RefCounted);

		friend class LRTMaterialBaker;
		struct Data;
		Data *data = nullptr;

	public:
		Context();
		~Context();
	};

	class Request : public RefCounted {
		GDSOFTCLASS(Request, RefCounted);

		friend class LRTMaterialBaker;
		struct Data;
		Data *data = nullptr;

	public:
		Request();
		~Request();
	};

private:
	struct Readback;
	static void _capture_context(const Ref<Context> &p_context);
	static void _free_context(int64_t p_data);
	static void _capture(const Ref<Request> &p_request);
	static bool _advance_capture(const Ref<Request> &p_request, bool p_wait);
	static void _capture_sync(const Ref<Request> &p_request);
	static Error _prepare(const Ref<Request> &p_request);
	static Error _snapshot_samplers(const Ref<Request> &p_request);
	static void _update_texture_signature(const Ref<Request> &p_request);
	static Error _render_page(const Ref<Request> &p_request);
	static bool _check_programs(const Ref<Request> &p_request);
	static void _release_gpu(const Ref<Request> &p_request);
	static void _render_sync(const Ref<Request> &p_request);
	static void _begin_page(const Ref<Request> &p_request);
	static void _begin_vertices(const Ref<Request> &p_request);
	static void _begin_source(const Ref<Request> &p_request);
	static void _process_pending();
	static void _readback(const Vector<uint8_t> &p_bytes, const Ref<Readback> &p_readback);
	static Error _validate(const Ref<Request> &p_request);
	static void _wait(const Ref<Request> &p_request);
	static void _complete(const Ref<Request> &p_request);

public:
	struct Surface {
		Vector<Vector3> positions;
		Vector<Vector2> atlas_uv;
		static constexpr int PAGE_SIZE = 2048;
		struct Page {
			Ref<Image> albedo;
			Ref<Image> emission;
		};
		Size2i atlas_size;
		Size2i page_size;
		Vector<Page> pages;
		uint64_t source_read_bytes = 0;
		bool has_emission = false;
		bool transparent = false;
		Array issues;

		void sample(const Vector2 &p_uv, Color &r_albedo, Color &r_emission) const;
	};

	static Ref<Context> capture_context();
	static Ref<Request> capture(GeometryInstance3D *p_instance, int p_surface, float p_texel_size, int p_instance_index = 0, const Ref<Context> &p_context = Ref<Context>());
	// Must run in a WorkerThreadPool pump task; cooperative waits keep other work progressing.
	static Error bake_on_worker(const Ref<Request> &p_request, Surface &r_surface, const SafeFlag *p_cancel = nullptr, const Ref<RefCounted> &p_source = Ref<RefCounted>());
	static Ref<RefCounted> get_source_snapshot(const Ref<Request> &p_request);
	static void clear_source_cache(const Ref<Context> &p_context);
	static Dictionary get_render_timings(const Ref<Context> &p_context);
	static Error get_signature(const Ref<Request> &p_request, Dictionary &r_signature, const SafeFlag *p_cancel);
	static void cancel(const Ref<Request> &p_request);
	static void process();
	static bool has_pending();
	static void shutdown();

	static Error inspect_surface(GeometryInstance3D *p_instance, int p_surface, float p_texel_size, Surface &r_surface, int p_instance_index = 0);
};
