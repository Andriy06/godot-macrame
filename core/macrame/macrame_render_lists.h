/**************************************************************************/
/*  macrame_render_lists.h                                                */
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

#include "core/templates/local_vector.h"
#include "core/templates/rid.h"

struct RenderSceneCullFrame;
class RenderingMethod;

// The value the cull node produces and the record node consumes: one culled frame per 3D
// viewport, in draw order. The frames are opaque to everything but the scene renderer that
// created them (`RenderingMethod::cull_frame_create`); this struct owns them, pooled, so a run
// allocates nothing in steady state.
//
// Guarded (write by `cull`, read by `record`) in the same-run shape; the versioned shape keeps
// two of these behind one front, one per replica, so the record node's frame and the cull node's
// frame are never the same object.
struct MacrameRenderLists {
	struct Entry {
		RID viewport;
		RenderSceneCullFrame *frame = nullptr;
	};
	LocalVector<Entry> entries; // The frames culled this run.
	LocalVector<RenderSceneCullFrame *> pool; // Every frame this value owns; `entries` point into it.
	void *run_data = nullptr; // The renderer's update -> record hand-off of the run (opaque).
	bool owns_run_data = false; // The value that created its run data frees it.
	uint64_t frame_number = 0;
	RenderSceneCullFrame *find(RID p_viewport) const {
		for (const Entry &e : entries) {
			if (e.viewport == p_viewport) {
				return e.frame;
			}
		}
		return nullptr;
	}

	// The next pooled frame, creating one through `p_scene` if the pool is short.
	RenderSceneCullFrame *acquire(RenderingMethod *p_scene, uint32_t p_index);
	// Free every pooled frame through `p_scene`; call before the scene renderer goes away.
	void release(RenderingMethod *p_scene);
};
