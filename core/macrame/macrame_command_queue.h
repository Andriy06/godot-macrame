/**************************************************************************/
/*  macrame_command_queue.h                                               */
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

#ifdef MACRAME_ENABLED

// A drop-in for `CommandQueueMT` backed by Macrame's staged-write primitives. The server's state
// is one guarded object; callers that do not hold its grant stage their commands into a
// `Deferred` journal instead of pushing into a mutex-guarded ring, and the batch is applied as
// one write by whoever next holds the grant.
//
// Two journals, double buffered, cut at a frame boundary. `cut_journal()` (the blue thread,
// between graph runs) makes the journal the frame just filled the "previous" one and switches
// staging to the other; `commit_previous_under_grant()` (the render node of the next run, which
// declared the write) applies exactly that cut. So a whole frame's commands travel as one value
// from the thread that produced them to the node that consumes them, and nothing is in flight
// across a run boundary.
//
// `launch()` remains for the servers whose write body is still a dynamic task (the physics step
// outside frame-graph mode). There is no pipelined launch: parallelism belongs to the frame
// graph's nodes, not to tasks this queue keeps handles on.
//
// The interface mirrors the subset of `CommandQueueMT` the `FUNC*` wrapper macros use, so
// `rendering_server_default.h` needs no macro changes beyond the async condition.

#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/priority.h"
#include "ts/recorder.h"
#include "ts/scheduler.h"
#include "ts/task.h"

#include "core/error/error_macros.h"
#include "core/string/ustring.h"
#include "core/macrame/macrame_scene.h"
#include "core/profiling/profiling.h"

#include <atomic>
#include <utility>
#include <vector>

template <typename Token>
class MacrameCommandQueue {
	struct Journal {
		ts::Deferred<Token> staged;
		ts::Recorder<Token> recorder; // The blue thread's recorder.
		std::vector<ts::Recorder<Token>> shard_recorders; // One per scene shard: FIFO per producer, deterministic apply order.
		std::atomic<uint64_t> count{ 0 };

		explicit Journal(ts::Guarded<Token> &p_guarded) :
				staged(p_guarded), recorder(staged.recorder()) {
			shard_recorders.reserve(MacrameScene::SHARD_COUNT);
			for (int i = 0; i < MacrameScene::SHARD_COUNT; i++) {
				shard_recorders.push_back(staged.recorder());
			}
		}
	};

	ts::Guarded<Token> guarded;
	// About 125 frames of the heaviest scene measured (16,000 commands a frame at 500 NPCs). No
	// legitimate backlog reaches it: the boundary cuts the journal once an iteration.
	static constexpr uint64_t MAX_STAGED_COMMANDS = 2000000;

	Journal journal_a;
	Journal journal_b;
	Journal *cur = &journal_a; // Where new commands are staged.
	ts::Task<void> in_flight; // A dynamic write body launched with `launch()`.
	bool has_in_flight = false;
	bool (*holds_grant)() = nullptr;
	bool stage_under_graph = false; // See `should_call_direct()`.

	// Join the launched body, if any.
	void _wait_in_flight() {
		if (has_in_flight) {
			GodotProfileZone("MacrameCommandQueue: wait for in-flight body");
			in_flight.sync();
			has_in_flight = false;
		}
	}

	Journal &_previous() { return cur == &journal_a ? journal_b : journal_a; }

	// This body holds the write grant: apply both journals inline, the one not being staged
	// into first, so a direct call observes every earlier command in the order it was recorded.
	void _commit_inline() {
		for (Journal *j : { &_previous(), cur }) {
			if (j->count.load(std::memory_order_relaxed) != 0) {
				(void)j->staged.commit();
				j->count.store(0, std::memory_order_relaxed);
			}
		}
	}

	bool _has_staged() const {
		return journal_a.count.load(std::memory_order_relaxed) != 0 || journal_b.count.load(std::memory_order_relaxed) != 0;
	}

public:
	// `p_holds_grant` reports whether the calling thread is running the body that holds the
	// object's grant (a graph node, a launched body); it is the one caller allowed to touch the
	// object directly while a task is in flight. `p_stage_under_graph` says that this object's
	// writer is a frame-graph node, so the wrappers stage while a graph is running rather than
	// take the direct path the blue thread is entitled to between runs (`should_call_direct()`).
	MacrameCommandQueue(const char *p_name, bool (*p_holds_grant)(), bool p_stage_under_graph = false) :
			guarded(ts::Named{ p_name }), journal_a(guarded), journal_b(guarded), holds_grant(p_holds_grant), stage_under_graph(p_stage_under_graph) {}

	~MacrameCommandQueue() {
		_wait_in_flight();
		journal_a.staged.discard();
		journal_b.staged.discard();
	}

	MacrameCommandQueue(const MacrameCommandQueue &) = delete;
	MacrameCommandQueue &operator=(const MacrameCommandQueue &) = delete;

	ts::Guarded<Token> &get_guarded() { return guarded; }

	// Record a member call for the next apply. Arguments are copied (decayed) like
	// `CommandQueueMT` does, so RIDs, Variants and containers are safe to hand over.
	template <typename T, typename M, typename... Args>
	void push(T *p_instance, M p_method, Args... p_args) {
		const int shard = MacrameScene::current_shard();
		Journal &j = *cur;
		ts::Recorder<Token> &rec = shard < 0 ? j.recorder : j.shard_recorders[shard];
		rec.stage([=](Token &) { (p_instance->*p_method)(p_args...); });
		const uint64_t staged = j.count.fetch_add(1, std::memory_order_relaxed) + 1;
		// A journal is emptied by the frame boundary, once an iteration. If one grows past a
		// backlog no frame can legitimately produce, its consumer has stopped running and the
		// journal is on its way to filling memory (about 1.5 MB of bone poses a frame at 500
		// NPCs, so unattended this reaches tens of gigabytes and the process dies with a
		// fail-fast and no message). Name it here instead, at the first command past the bound.
		CRASH_COND_MSG(staged > MAX_STAGED_COMMANDS, "Macrame: the render command journal passed " + itos(MAX_STAGED_COMMANDS) + " staged commands: its frame boundary has stopped running (nothing applies it).");
	}

	template <typename T, typename M, typename... Args>
	void push_and_sync(T *p_instance, M p_method, Args... p_args) {
		push(p_instance, p_method, p_args...);
		sync();
	}

	// A synchronous round trip: drain everything before it, then run the getter under the
	// grant on the calling (blue) thread. Same cost profile as the separate-thread mode's
	// `push_and_ret`: it serializes against the in-flight draw.
	template <typename T, typename M, typename R, typename... Args>
	void push_and_ret(T *p_instance, M p_method, R *r_ret, Args... p_args) {
		GodotProfileZone("MacrameCommandQueue: synchronous getter");
		if (MacrameScene::in_shard_task()) {
			// A synchronous server getter from a shard would need the server's write grant while
			// the shard holds a read grant on it (or none): a certain deadlock, and a design smell.
			// Read published state instead. Report and return the default.
			ERR_PRINT_ONCE("Macrame: synchronous server getter called from a scene shard; returning a default value. Read published state instead.");
			return;
		}
		_wait_in_flight();
		guarded.access([&](Token &) {
			_commit_inline();
			*r_ret = (p_instance->*p_method)(p_args...);
		}).sync();
	}

	// Whether the caller *may* touch the object directly: it holds the grant, or it is a blue
	// thread (not a worker) with no task in flight. This is the permission, and it is what the
	// harness query behind the servers' thread guards asks: in frame-graph mode nothing is in
	// flight between two runs, so the blue thread is alone with the object for the whole gap and
	// a direct touch is legal there. `flush_if_pending()` is what makes such a touch observe the
	// commands staged before it.
	bool may_call_direct() const {
		if (holds_grant()) {
			return true;
		}
		return ts::current_worker_index() < 0 && !has_in_flight && !MacrameScene::in_shard_task();
	}

	// Whether the wrappers *should* take it. Legal is not the same as wanted: while a frame graph
	// is running, this object's writer is one of its nodes, and a blue-thread command belongs in
	// the journal that node applies. Taking the direct path in the gap between two runs would
	// move a frame's worth of server work onto the blue thread, where it overlaps nothing - which
	// costs about 1 ms a frame at 500 NPCs, measured. So outside the grant, stage.
	//
	// Only the objects whose writer is a graph node ask for this (the renderer); an object whose
	// writer is still a dynamic task keeps "legal implies direct", where the in-flight test
	// already says the right thing.
	bool should_call_direct() const {
		if (holds_grant()) {
			return true;
		}
		if (stage_under_graph && MacrameScene::frame_graph_running()) {
			return false;
		}
		return may_call_direct();
	}

	// Before a direct call from a blue thread: join the in-flight task and apply what was
	// staged, so the direct call observes every earlier command in order. Under the grant
	// this is a no-op (the batch was applied before the grant was handed out).
	//
	// "In order" is why `_commit_inline()` applies the cut journal before the current one: on the
	// blue thread between two graph runs the previous journal is the frame the render node has
	// not drawn yet, and the current one is what has been staged since. A direct call that
	// skipped the older of the two would see the newer commands without the older ones.
	void flush_if_pending() {
		if (holds_grant() || MacrameScene::in_shard_task()) {
			return; // Shards read the state that was current when the phase began.
		}
		if (has_in_flight || _has_staged()) {
			sync();
		}
	}

	// Apply every staged command as one write on the calling thread.
	void flush_all() { sync(); }

	// The caller holds the object's write grant (a graph node, a launched body): apply the staged
	// journals inline.
	void commit_under_grant() { _commit_inline(); }

	// The frame boundary, on the blue thread between two graph runs: the journal this frame's
	// producers filled stops taking commands and the other becomes current. Exactly this cut is
	// what the next run's render node applies.
	void cut_journal() { cur = (cur == &journal_a) ? &journal_b : &journal_a; }

	// The caller holds the write grant (the frame graph's render node): apply the journal cut at
	// the last frame boundary, and only that one. The current journal is being staged into right
	// now by the shards of the run this node belongs to, and belongs to the next frame's render.
	void commit_previous_under_grant() {
		Journal &j = _previous();
		if (j.count.load(std::memory_order_relaxed) != 0) {
			(void)j.staged.commit();
			j.count.store(0, std::memory_order_relaxed);
		}
	}

	void sync() {
		GodotProfileZone("MacrameCommandQueue: sync + apply batch");
		_wait_in_flight();
		if (!_has_staged()) {
			return;
		}
		guarded.access([&](Token &) {
			_commit_inline();
		}).sync();
	}

	// Run `p_body(Token&)` as an asynchronous write task holding the grant. The caller must
	// have called `sync()` first (the physics tick does). Returns immediately.
	template <typename Fn>
	void launch(Fn &&p_body, const char *p_name) {
		_wait_in_flight();
		in_flight = guarded.async(std::forward<Fn>(p_body), { .name = p_name });
		has_in_flight = true;
	}

	// Interface parity with CommandQueueMT for the pump-task branch that Macrame mode never takes.
	template <typename TaskID>
	void set_pump_task_id(TaskID) {}

	bool is_in_flight() const { return has_in_flight; }
	void wait() { _wait_in_flight(); }
};

#endif // MACRAME_ENABLED
