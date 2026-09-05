/**************************************************************************/
/*  rendering_server_default.cpp                                          */
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

#include "rendering_server_default.h"

#include "core/macrame/macrame_phase_probe.h"
#include "core/macrame/macrame_runtime.h"
#include "core/macrame/macrame_scene.h"

#ifdef MACRAME_ENABLED
#include "ts/priority.h"
#include "ts/static_task_graph.h"
#endif

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "servers/display/display_server.h"
#include "servers/rendering/renderer_canvas_cull.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

#ifndef XR_DISABLED
#include "servers/xr/xr_server.h"
#endif

// careful, these may run in different threads than the rendering server

int RenderingServerDefault::changes = 0;

/* FREE */

void RenderingServerDefault::_free(RID p_rid) {
	if (unlikely(p_rid.is_null())) {
		return;
	}
	if (RSG::utilities->free(p_rid)) {
		return;
	}
	if (RSG::canvas->free(p_rid)) {
		return;
	}
	if (RSG::viewport->free(p_rid)) {
		return;
	}
	if (RSG::scene->free(p_rid)) {
		return;
	}
}

/* EVENT QUEUING */

void RenderingServerDefault::request_frame_drawn_callback(const Callable &p_callable) {
	frame_drawn_callbacks.push_back(p_callable);
}

#ifdef MACRAME_ENABLED
// --- The two frame-graph nodes -----------------------------------------------------------
//
// `render` writes the render server's guarded object, `submit` writes the device's, and neither
// declares anything else, so `compile()` derives no edge between them or to the shard, step and
// navigation nodes. That is the point: the three chains of a run work on different frames.

// The render node. Declared access: write on `MacrameCommandQueue::get_guarded()` (the render
// server). It applies the command journal cut at the last frame boundary - everything the shards
// and the blue thread staged during the previous run - records that frame's draw, and leaves the
// recorded frame in the hand-off ring for the next run's submit node.
void RenderingServerDefault::_macrame_render_node() {
	if (!render_request) {
		// Start-up only: the first run of each graph happens before `draw()` has posted anything
		// (the graph runs in the middle of the iteration, the post is at its end), and an
		// iteration that draws nothing posts nothing. Never a "the node has no work" no-op in
		// steady state.
		return;
	}
	render_request = false;
	const int slot = split_draw ? render_slot : -1;
	MacrameRender::set_holds_grant(true);
	MacrameRuntime::long_task_begin();
	MacramePhaseProbe::frame_begin(MacrameScene::frame_graph_kind());
	command_queue.commit_previous_under_grant();
	MACRAME_PHASE("journal apply");
	_draw(render_present, render_step, slot);
	MacramePhaseProbe::frame_end();
	MacrameRuntime::long_task_end();
	MacrameRender::set_holds_grant(false);
	// Read by the blue thread at the next frame boundary, which is the only other reader.
	last_render_slot = slot;
}

// The submit node. Declared access: write on `Guarded<RenderingDeviceSubmit>` (the device
// payload). It does one thing: take the value the previous run's render node staged and submit it
// - compile the graph, hand it to the queue, present.
void RenderingServerDefault::_macrame_submit_node() {
	if (submit_slot < 0 || !handoff_ring[submit_slot].valid) {
		return; // The first two runs, before a frame has travelled the ring.
	}
	Handoff &h = handoff_ring[submit_slot];
	h.valid = false;
	MacrameRenderDevice::set_holds_grant(true);
	MacrameRuntime::long_task_begin();
	RSG::rasterizer->submit_staged(h.staged, h.present);
	MacrameRuntime::long_task_end();
	MacrameRenderDevice::set_holds_grant(false);
}

void RenderingServerDefault::_macrame_add_frame_nodes(void *p_graph) {
	ts::Static_task_graph &graph = *static_cast<ts::Static_task_graph *>(p_graph);
	RenderingServerDefault *rs = static_cast<RenderingServerDefault *>(RenderingServer::get_singleton());
	// High priority on both: they are the frame's two longest single-threaded bodies, and at
	// equal priority a worker would pop a shard node ahead of them.
	graph.add_node("render", [rs](RenderGrantToken &) { rs->_macrame_render_node(); },
					rs->command_queue.get_guarded())
			.set_priority(ts::Priority::high);
	if (rs->split_draw) {
		graph.add_node("submit", [rs](RenderingDeviceSubmit &) { rs->_macrame_submit_node(); },
						*rs->device_guarded)
				.set_priority(ts::Priority::high);
	}
}

// Shutdown: the last run's render node staged a frame no submit node will consume, and `draw()`
// may have posted one more that no render node will record. The staged one still owns a device
// graph and a command buffer, so submit it here under the grant, as an ordinary access.
void RenderingServerDefault::_macrame_drain_handoff() {
	if (!split_draw) {
		return;
	}
	for (int i = 0; i < HANDOFF_SLOTS; i++) {
		// At most one slot is ever valid here, so the order does not matter; the loop is so that
		// no frame is left behind whichever way the main loop exited.
		Handoff &h = handoff_ring[i];
		if (!h.valid) {
			continue;
		}
		h.valid = false;
		device_guarded->access([&](RenderingDeviceSubmit &) {
					MacrameRenderDevice::set_holds_grant(true);
					RSG::rasterizer->submit_staged(h.staged, h.present);
					MacrameRenderDevice::set_holds_grant(false);
				})
				.sync();
	}
}

void RenderingServerDefault::macrame_drain_commands() {
	_macrame_drain_handoff();
	command_queue.wait();
	command_queue.sync();
}
#endif

void RenderingServerDefault::_draw(bool p_swap_buffers, double frame_step, int p_handoff_slot) {
	GodotProfileZoneGroupedFirst(_profile_zone, "rasterizer->begin_frame");
	RSG::rasterizer->begin_frame(frame_step);
	MACRAME_PHASE("begin_frame");

	TIMESTAMP_BEGIN()

	uint64_t time_usec = OS::get_singleton()->get_ticks_usec();

	RENDER_TIMESTAMP("Prepare Render Frame");

#ifndef XR_DISABLED
	GodotProfileZoneGrouped(_profile_zone, "xr_server->pre_render");
	XRServer *xr_server = XRServer::get_singleton();
	if (xr_server != nullptr) {
		// Let XR server know we're about to render a frame.
		xr_server->pre_render();
	}
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "scene->update");
	RSG::scene->update(); //update scenes stuff before updating instances
	GodotProfileZoneGrouped(_profile_zone, "canvas->update");
	RSG::canvas->update();
	MACRAME_PHASE("canvas update");

	frame_setup_time = double(OS::get_singleton()->get_ticks_usec() - time_usec) / 1000.0;

	GodotProfileZoneGrouped(_profile_zone, "particles_storage->update_particles");
	RSG::particles_storage->update_particles(); //need to be done after instances are updated (colliders and particle transforms), and colliders are rendered
	MACRAME_PHASE("particles update");

	GodotProfileZoneGrouped(_profile_zone, "scene->render_probes");
	RSG::scene->render_probes();
	MACRAME_PHASE("render probes");

	GodotProfileZoneGrouped(_profile_zone, "viewport->draw_viewports");
	RSG::viewport->draw_viewports(p_swap_buffers);
	MACRAME_PHASE("draw_viewports: tail (blit, 2D)");

	GodotProfileZoneGrouped(_profile_zone, "canvas_render->update");
	RSG::canvas_render->update();
	MACRAME_PHASE("canvas_render update");

	GodotProfileZoneGrouped(_profile_zone, "rasterizer->end_frame");
#ifdef MACRAME_ENABLED
	if (p_handoff_slot >= 0) {
		// Close the recorded frame into the hand-off value the next run's submit node consumes.
		// Everything that value names (that frame slot, that graph, the queue) is disjoint from
		// what this node touches from here on, and the ring slot belongs to this run's frame
		// number, so no other node can be looking at it.
		Handoff &h = handoff_ring[p_handoff_slot];
		h.staged = RSG::rasterizer->stage_submit();
		h.present = p_swap_buffers;
		h.valid = true;
	} else {
		RSG::rasterizer->end_frame(p_swap_buffers);
	}
#else
	RSG::rasterizer->end_frame(p_swap_buffers);
#endif

	MACRAME_PHASE("stage submit (advance frame)");
#ifndef XR_DISABLED
	if (xr_server != nullptr) {
		GodotProfileZone("xr_server->end_frame");
		// let our XR server know we're done so we can get our frame timing
		xr_server->end_frame();
	}
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "update_visibility_notifiers");
	RSG::canvas->update_visibility_notifiers();
	RSG::scene->update_visibility_notifiers();
	MACRAME_PHASE("visibility notifiers");

#ifdef MACRAME_ENABLED
	{
		// The frame's read-back outputs, staged here under the grant; draw() publishes them on
		// the main thread once this task is joined.
		GodotProfileZoneGrouped(_profile_zone, "stage render outputs");
		MacrameRenderOutputs outputs;
		outputs.frame = RSG::rasterizer->get_frame_number();
		RSG::viewport->macrame_collect_outputs(outputs);
		RSG::particles_storage->macrame_collect_inactive(outputs.inactive_particles);
		MacrameRenderSnapshot::stage(std::move(outputs));
		MACRAME_PHASE("stage outputs");
	}
#endif
	GodotProfileZoneGrouped(_profile_zone, "post_draw_steps");
#ifdef MACRAME_ENABLED
	const bool off_main_thread = true; // _draw runs on a worker; post-draw callbacks belong to the main thread.
#else
	const bool off_main_thread = create_thread;
#endif
	if (off_main_thread) {
		callable_mp(this, &RenderingServerDefault::_run_post_draw_steps).call_deferred();
	} else {
		_run_post_draw_steps();
	}

	if (RSG::utilities->get_captured_timestamps_count()) {
		GodotProfileZoneGrouped(_profile_zone, "frame_profile");
		Vector<RenderingServerTypes::FrameProfileArea> new_profile;
		if (RSG::utilities->capturing_timestamps) {
			new_profile.resize(RSG::utilities->get_captured_timestamps_count());
		}

		uint64_t base_cpu = RSG::utilities->get_captured_timestamp_cpu_time(0);
		uint64_t base_gpu = RSG::utilities->get_captured_timestamp_gpu_time(0);
		for (uint32_t i = 0; i < RSG::utilities->get_captured_timestamps_count(); i++) {
			uint64_t time_cpu = RSG::utilities->get_captured_timestamp_cpu_time(i);
			uint64_t time_gpu = RSG::utilities->get_captured_timestamp_gpu_time(i);

			String name = RSG::utilities->get_captured_timestamp_name(i);

			if (name.begins_with("vp_")) {
				RSG::viewport->handle_timestamp(name, time_cpu, time_gpu);
			}

			if (RSG::utilities->capturing_timestamps) {
				new_profile.write[i].gpu_msec = double((time_gpu - base_gpu) / 1000) / 1000.0;
				new_profile.write[i].cpu_msec = double(time_cpu - base_cpu) / 1000.0;
				new_profile.write[i].name = RSG::utilities->get_captured_timestamp_name(i);
			}
		}

		frame_profile = new_profile;
	}

	frame_profile_frame = RSG::utilities->get_captured_timestamps_frame();

	if (print_gpu_profile) {
		GodotProfileZoneGrouped(_profile_zone, "gpu_profile");
		if (print_frame_profile_ticks_from == 0) {
			print_frame_profile_ticks_from = OS::get_singleton()->get_ticks_usec();
		}
		double total_time = 0.0;

		for (int i = 0; i < frame_profile.size() - 1; i++) {
			String name = frame_profile[i].name;
			if (name[0] == '<' || name[0] == '>') {
				continue;
			}

			double time = frame_profile[i + 1].gpu_msec - frame_profile[i].gpu_msec;

			if (print_gpu_profile_task_time.has(name)) {
				print_gpu_profile_task_time[name] += time;
			} else {
				print_gpu_profile_task_time[name] = time;
			}
		}

		if (frame_profile.size()) {
			total_time = frame_profile[frame_profile.size() - 1].gpu_msec;
		}

		uint64_t ticks_elapsed = OS::get_singleton()->get_ticks_usec() - print_frame_profile_ticks_from;
		print_frame_profile_frame_count++;
		if (ticks_elapsed > 1000000) {
			print_line("GPU PROFILE (total " + rtos(total_time) + "ms): ");

			float print_threshold = 0.01;
			for (const KeyValue<String, float> &E : print_gpu_profile_task_time) {
				double time = E.value / double(print_frame_profile_frame_count);
				if (time > print_threshold) {
					print_line("\t-" + E.key + ": " + rtos(time) + "ms");
				}
			}
			print_gpu_profile_task_time.clear();
			print_frame_profile_ticks_from = OS::get_singleton()->get_ticks_usec();
			print_frame_profile_frame_count = 0;
		}
	}

	GodotProfileZoneGrouped(_profile_zone, "memory_info");
	RSG::utilities->update_memory_info();
}

void RenderingServerDefault::_run_post_draw_steps() {
	while (frame_drawn_callbacks.front()) {
		Callable c = frame_drawn_callbacks.front()->get();
		Variant result;
		Callable::CallError ce;
		c.callp(nullptr, 0, result, ce);
		if (ce.error != Callable::CallError::CALL_OK) {
			String err = Variant::get_callable_error_text(c, nullptr, 0, ce);
			ERR_PRINT("Error calling frame drawn function: " + err);
		}

		frame_drawn_callbacks.pop_front();
	}

	emit_signal(SNAME("frame_post_draw"));
}

double RenderingServerDefault::get_frame_setup_time_cpu() const {
	return frame_setup_time;
}

bool RenderingServerDefault::has_changed() const {
	return changes > 0;
}

void RenderingServerDefault::_init() {
	RSG::threaded = create_thread;

	RSG::canvas = memnew(RendererCanvasCull);
	RSG::viewport = memnew(RendererViewport);
	RendererSceneCull *sr = memnew(RendererSceneCull);
	RSG::camera_attributes = memnew(RendererCameraAttributes);
	RSG::scene = sr;
	RSG::rasterizer = RendererCompositor::create();
	RSG::utilities = RSG::rasterizer->get_utilities();
	RSG::rasterizer->initialize();
	RSG::light_storage = RSG::rasterizer->get_light_storage();
	RSG::material_storage = RSG::rasterizer->get_material_storage();
	RSG::mesh_storage = RSG::rasterizer->get_mesh_storage();
	RSG::particles_storage = RSG::rasterizer->get_particles_storage();
	RSG::texture_storage = RSG::rasterizer->get_texture_storage();
	RSG::gi = RSG::rasterizer->get_gi();
	RSG::fog = RSG::rasterizer->get_fog();
	RSG::canvas_render = RSG::rasterizer->get_canvas();
	sr->set_scene_render(RSG::rasterizer->get_scene());
#ifdef MACRAME_ENABLED
	// MACRAME_SPLIT_DRAW=0 makes `render` submit inline and adds no `submit` node, for A/B; a
	// compositor without a device object (the GL one) has no submit node either way.
	device_guarded = RSG::rasterizer->get_device_guarded();
	split_draw = device_guarded != nullptr && RSG::rasterizer->supports_split_submit() && OS::get_singleton()->get_environment("MACRAME_SPLIT_DRAW") != "0";
	print_verbose(split_draw ? "Macrame: split draw enabled (render node + submit node)" : "Macrame: split draw disabled");
#endif
}

void RenderingServerDefault::_finish() {
	if (test_cube.is_valid()) {
		free_rid(test_cube);
	}

	RSG::canvas->finalize();
	memdelete(RSG::canvas);
	RSG::rasterizer->finalize();
	memdelete(RSG::viewport);
	memdelete(RSG::rasterizer);
	memdelete(RSG::scene);
	memdelete(RSG::camera_attributes);
}

void RenderingServerDefault::init() {
#ifdef MACRAME_ENABLED
	server_thread = Thread::MAIN_ID;
	_init();
	return;
#endif
	if (create_thread) {
		print_verbose("RenderingServerWrapMT: Starting render thread");
		DisplayServer::get_singleton()->release_rendering_thread();
		WorkerThreadPool::TaskID tid = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &RenderingServerDefault::_thread_loop), true, "Rendering Server pump task", true);
		command_queue.set_pump_task_id(tid);
		command_queue.push(this, &RenderingServerDefault::_assign_mt_ids, tid);
		command_queue.push_and_sync(this, &RenderingServerDefault::_init);
		DEV_ASSERT(server_task_id == tid);
	} else {
		server_thread = Thread::MAIN_ID;
		_init();
	}
}

void RenderingServerDefault::finish() {
#ifdef MACRAME_ENABLED
	command_queue.wait();
	command_queue.sync();
	_finish();
	return;
#endif
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_finish);
		command_queue.push(this, &RenderingServerDefault::_thread_exit);
		if (server_task_id != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(server_task_id);
			server_task_id = WorkerThreadPool::INVALID_TASK_ID;
		}
		server_thread = Thread::MAIN_ID;
	} else {
		_finish();
	}
}

/* STATUS INFORMATION */

uint64_t RenderingServerDefault::get_rendering_info(RSE::RenderingInfo p_info) {
	if (p_info == RSE::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME) {
		return RSG::viewport->get_total_objects_drawn();
	} else if (p_info == RSE::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME) {
		return RSG::viewport->get_total_primitives_drawn();
	} else if (p_info == RSE::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME) {
		return RSG::viewport->get_total_draw_calls_used();
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_CANVAS);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_MESH) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_MESH) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_MESH);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE) {
		return RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SURFACE);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_DRAW) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_DRAW);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SPECIALIZATION) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SPECIALIZATION);
	}
	return RSG::utilities->get_rendering_info(p_info);
}

#ifdef RD_ENABLED
RenderingDeviceEnums::DeviceType RenderingServerDefault::get_video_adapter_type() const {
	return RSG::utilities->get_video_adapter_type();
}
#endif // RD_ENABLED

void RenderingServerDefault::set_frame_profiling_enabled(bool p_enable) {
	RSG::utilities->capturing_timestamps = p_enable;
}

uint64_t RenderingServerDefault::get_frame_profile_frame() {
	return frame_profile_frame;
}

Vector<RenderingServerTypes::FrameProfileArea> RenderingServerDefault::get_frame_profile() {
	return frame_profile;
}

/* TESTING */

Color RenderingServerDefault::get_default_clear_color() {
	return RSG::texture_storage->get_default_clear_color();
}

void RenderingServerDefault::set_default_clear_color(const Color &p_color) {
	RSG::texture_storage->set_default_clear_color(p_color);
}

#ifndef DISABLE_DEPRECATED
bool RenderingServerDefault::has_feature(RSE::Features p_feature) const {
	return false;
}
#endif

void RenderingServerDefault::sdfgi_set_debug_probe_select(const Vector3 &p_position, const Vector3 &p_dir) {
	RSG::scene->sdfgi_set_debug_probe_select(p_position, p_dir);
}

void RenderingServerDefault::set_print_gpu_profile(bool p_enable) {
	RSG::utilities->capturing_timestamps = p_enable;
	print_gpu_profile = p_enable;
}

RID RenderingServerDefault::get_test_cube() {
	if (!test_cube.is_valid()) {
		test_cube = _make_test_cube();
	}
	return test_cube;
}

bool RenderingServerDefault::has_os_feature(const String &p_feature) const {
	if (RSG::utilities) {
		return RSG::utilities->has_os_feature(p_feature);
	} else {
		return false;
	}
}

void RenderingServerDefault::set_debug_generate_wireframes(bool p_generate) {
	RSG::utilities->set_debug_generate_wireframes(p_generate);
}

bool RenderingServerDefault::is_low_end() const {
	return RendererCompositor::is_low_end();
}

Size2i RenderingServerDefault::get_maximum_viewport_size() const {
	if (RSG::utilities) {
		return RSG::utilities->get_maximum_viewport_size();
	} else {
		return Size2i();
	}
}

void RenderingServerDefault::_assign_mt_ids(WorkerThreadPool::TaskID p_pump_task_id) {
	server_thread = Thread::get_caller_id();
	server_task_id = p_pump_task_id;

#ifdef RD_ENABLED
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd) {
		// This is needed because the main RD is created on the main thread.
		rd->make_current();
	}
#endif // RD_ENABLED
}

void RenderingServerDefault::_thread_exit() {
	exit = true;
}

void RenderingServerDefault::_thread_loop() {
	DisplayServer::get_singleton()->gl_window_make_current(DisplayServerEnums::MAIN_WINDOW_ID); // Move GL to this thread.

	while (!exit) {
		WorkerThreadPool::get_singleton()->yield();
		command_queue.flush_all();
	}

	DisplayServer::get_singleton()->release_rendering_thread();
}

/* INTERPOLATION */

void RenderingServerDefault::set_physics_interpolation_enabled(bool p_enabled) {
	RSG::canvas->set_physics_interpolation_enabled(p_enabled);
	RSG::scene->set_physics_interpolation_enabled(p_enabled);
}

/* EVENT QUEUING */

void RenderingServerDefault::sync() {
#ifdef MACRAME_ENABLED
	// A no-op in frame-graph mode. The frame loop calls this once an iteration; applying the
	// staged batch here would move a whole frame of renderer commands onto the blue thread, which
	// is precisely the work the `render` node exists to hold. The batch is cut at the frame
	// boundary below and applied by the next run's render node under its grant; synchronous
	// getters and direct calls still flush through `flush_if_pending()`, and shutdown drains
	// explicitly (`macrame_drain_commands`).
	if (MacrameScene::frame_graph_running()) {
		return;
	}
	if (command_queue.is_in_flight()) {
		return;
	}
	command_queue.sync();
	return;
#endif
	if (create_thread) {
		command_queue.sync();
	} else {
		command_queue.flush_all(); // Flush all pending from other threads.
	}
}

void RenderingServerDefault::draw(bool p_present, double frame_step) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Manually triggering the draw function from the RenderingServer can only be done on the main thread. Call this function from the main thread or use call_deferred().");
	// Needs to be done before changes is reset to 0, to not force the editor to redraw.
	RS::get_singleton()->emit_signal(SNAME("frame_pre_draw"));
	changes = 0;
#ifdef MACRAME_ENABLED
	if (MacrameScene::frame_graph_running()) {
		// The frame boundary, on the blue thread, with nothing in flight: the graph run of this
		// iteration has been joined and the next one has not started. This is all `draw()` does in
		// frame-graph mode - no launch, no join. The frame it posts is drawn by the `render` node
		// of the next run and submitted by the `submit` node of the run after that.
		GodotProfileZone("Macrame: frame boundary");

		// 1. The outputs the render node staged during the run that just ended become the version
		//    the next run's shards read.
		MacrameRenderSnapshot::publish();

		// 2. What that render node staged for the device is what the next run's submit node
		//    drains. `last_render_slot` is written by the render node and read only here.
		submit_slot = last_render_slot;
		last_render_slot = -1;

		// 3. Cut the command journal: everything staged up to this point - by this run's shards
		//    and by the blue thread since the last cut - becomes the batch the next run's render
		//    node applies under the grant. New commands stage into the other journal.
		command_queue.cut_journal();

		// 4. Post the frame. `draw_seq` is the frame number the ring's ownership is keyed on.
		render_present = p_present;
		render_step = frame_step;
		render_slot = int(draw_seq % HANDOFF_SLOTS);
		draw_seq++;
		render_request = true;
		return;
	}
	// No graph is running this iteration (start-up, before the scene tree exists): draw
	// synchronously on this thread, under the grant, with the batch applied first.
	MacrameRenderSnapshot::publish();
	command_queue.sync();
	command_queue.get_guarded().access([&](RenderGrantToken &) {
							 MacrameRender::set_holds_grant(true);
							 _draw(p_present, frame_step, -1);
							 MacrameRender::set_holds_grant(false);
						 })
			.sync();
	return;
#endif
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_draw, p_present, frame_step, -1);
	} else {
		_draw(p_present, frame_step, -1);
	}
}

void RenderingServerDefault::tick() {
	RSG::canvas->tick();
	RSG::scene->tick();
}

void RenderingServerDefault::pre_draw(bool p_will_draw) {
	RSG::scene->pre_draw(p_will_draw);
}

void RenderingServerDefault::_call_on_render_thread(const Callable &p_callable) {
	p_callable.call();
}

RenderingServerDefault::RenderingServerDefault(bool p_create_thread) {
	RenderingServer::init();

	create_thread = p_create_thread;

#ifdef MACRAME_ENABLED
	// Hand the renderer's token and the direct-mode query to RenderingDevice's thread guards.
	command_queue.get_guarded().access([](RenderGrantToken &p_token) { MacrameRender::set_token(&p_token); }).sync();
	MacrameRender::set_access_query([]() {
		return static_cast<RenderingServerDefault *>(RenderingServer::get_singleton())->command_queue.may_call_direct();
	});
	// The renderer's two frame-graph nodes; `MacrameScene` calls this back when it compiles a
	// graph that has a frame phase.
	MacrameScene::set_frame_render_nodes(&RenderingServerDefault::_macrame_add_frame_nodes);
#endif
}

RenderingServerDefault::~RenderingServerDefault() {
#ifdef MACRAME_ENABLED
	// The RenderingDevice outlives the rendering server: `Main::cleanup` deletes the display
	// server (which frees its screens, and that records into the device) right after this. Both
	// hooks reach back into this object - the query dereferences the singleton, the token points
	// inside `command_queue` - so they have to go with it. With neither set,
	// `MacrameRender::check_access()` grants the direct path, which is what the single-threaded
	// tail of the shutdown is.
	MacrameRender::set_access_query(nullptr);
	MacrameRender::set_token(nullptr);
	MacrameScene::set_frame_render_nodes(nullptr);
#endif
}
