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

#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"

struct RenderSceneCullFrame;
struct MacrameViewportSnapshot; // renderer_viewport.h: a viewport as the cull saw it, for the record node.
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
	// A run's cull takes a handful of frames: one per visible viewport, six per reflection probe
	// face, one per voxel GI, one per particle heightfield. The pool is per cull set and only
	// grows, so a run that asks for more than a scene can legitimately need is a runaway: without
	// a bound each extra frame is a full `RenderSceneCullFrame` (paged arrays for every culled
	// instance, megabytes at this scale) and the process fills memory in seconds and dies with a
	// fail-fast and no message. Name it here instead.
	static constexpr uint32_t MAX_CULL_FRAMES = 64;
	static constexpr uint32_t MAX_JOBS = 256;

	LocalVector<Entry> entries; // The frames culled this run.
	LocalVector<RenderSceneCullFrame *> pool; // Every frame this value owns; `entries` point into it.
	uint32_t frames_used = 0; // How many of `pool` this run's cull took.

	// Every viewport the record node draws this run, in draw order, each with the copy of the
	// viewport the cull node took after the scene update (the record node reads the copy, never
	// the viewport: the next scene update is rewriting that beside it). The 3D ones also have a
	// frame in `entries`.
	struct ViewportEntry {
		RID viewport;
		MacrameViewportSnapshot *snapshot = nullptr;
		RenderSceneCullFrame *frame = nullptr;
	};
	LocalVector<ViewportEntry> viewports;
	LocalVector<MacrameViewportSnapshot *> snapshot_pool; // Owned like `pool`; managed by the viewport server.

	// The device work the record node does for the scene besides the viewports, each culled by
	// the cull node into frames of this value: a reflection probe's six faces (or one
	// post-process step), a voxel GI's dynamic geometry, a particle collider's heightfield. What
	// the record node did with them travels back to the scene update by mailbox.
	struct ProbeJob {
		RID instance; // The probe's scene instance.
		RID probe_instance; // The light storage's probe instance.
		RID atlas;
		int step = 0; // 0: render the faces (culled below); >0: a post-process step.
		bool always = false; // UPDATE_ALWAYS: the record runs every step at once.
		RenderSceneCullFrame *faces[6] = {};
	};
	struct VoxelGIJob {
		RID instance;
		RID probe_instance;
		bool update_lights = false;
		Vector<RID> light_instances;
		RenderSceneCullFrame *geometry = nullptr; // `cull_result.geometry_instances`: the dynamic geometry in view.
	};
	struct HeightfieldJob {
		RID instance;
		RID base;
		Transform3D transform;
		RenderSceneCullFrame *geometry = nullptr;
	};
	LocalVector<ProbeJob> probe_jobs;
	LocalVector<VoxelGIJob> voxel_gi_jobs;
	LocalVector<HeightfieldJob> heightfield_jobs;
	void *run_data = nullptr; // The renderer's update -> record hand-off of the run (opaque).
	bool owns_run_data = false; // A cull set owns its run data; a published copy only names it.
	uint64_t frame_number = 0;
	// What the blue thread posted with the frame, carried to the record node with the lists.
	bool present = false;
	double step = 0.0;
	int render_slot = -1; // The device hand-off slot of the frame.
	int set_index = -1; // The cull set the entries live in.
	bool valid = false; // A frame is here to record.

	// The published copy: names the set's frames and run data, owns nothing.
	MacrameRenderLists published_copy() const {
		MacrameRenderLists c;
		c.entries = entries;
		c.viewports = viewports;
		c.probe_jobs = probe_jobs;
		c.voxel_gi_jobs = voxel_gi_jobs;
		c.heightfield_jobs = heightfield_jobs;
		c.frames_used = frames_used;
		c.run_data = run_data;
		c.owns_run_data = false;
		c.frame_number = frame_number;
		c.present = present;
		c.step = step;
		c.render_slot = render_slot;
		c.set_index = set_index;
		c.valid = valid;
		return c;
	}

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
	// Free every pooled frame through `p_scene`; call before the scene renderer goes away. The
	// snapshots are the viewport server's to free (`RendererViewport::macrame_snapshots_release`).
	void release(RenderingMethod *p_scene);
	void clear_jobs() {
		viewports.clear();
		probe_jobs.clear();
		voxel_gi_jobs.clear();
		heightfield_jobs.clear();
	}
};
