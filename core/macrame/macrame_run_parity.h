/**************************************************************************/
/*  macrame_run_parity.h                                                  */
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

#include "core/error/error_macros.h"
#include "core/string/ustring.h"
#include "core/macrame/macrame_render_grant.h"
#include "core/templates/local_vector.h"
#include "core/templates/self_list.h"

#include "ts/scheduler.h"

// The run parity of the render pipeline's storage hand-offs.
//
// The scene update node (the journal: materials, meshes, multimeshes, skeletons, decals) fills the
// storages' dirty lists; the record node drains them into the device. In the same-run shapes an
// edge orders the two; in the lagged shape (results 2.16 / 2.17) they run side by side. So every
// such list is two lists, and every queued object has one link per list: the update node writes
// the slot of the current run, the record node drains the other slot, and the blue thread moves
// the run number between runs, when nobody reads it. Nothing is read across the slots, not even
// "is it queued": a link is the slot's.
//
// What the update node frees is destroyed by the update node two runs later (`MacrameRunFrees`):
// the record node of the run of the free may still draw with it, the record node of the next run
// may still drain it from the slot it was queued in, and both are joined by then. The device's
// own dispose lists are the record node's: a device free from the update node waits one run for
// the record node (`RenderingDevice::macrame_free_deferred`).
//
// A slot the update node wrote and the record node drained in the same run is the overlap this
// exists to forbid. Every list stamps both (the run of the last update write, the run of the last
// record drain, per slot) and the blue thread checks them at the run boundary, when the run is
// joined: the fault is deterministic, whatever the interleaving inside the run was. The self-test
// `MACRAME_PARITY_SELFTEST=1` reintroduces that overlap on purpose (the record node drains the
// update node's slot) and must crash with that message at the first boundary after both touched
// a slot.
struct MacrameRunParity {
	static inline uint64_t run = 0;
	static inline bool selftest_overlap = false;
	// The same-run three-node shape: the compiler's edge orders the scene update before the record
	// node, so the record node may drain the slot the update wrote this run as well.
	static inline bool same_run_edge = false;

	// The blue thread, between runs.
	static void post(uint64_t p_run) { run = p_run; }
	static uint64_t current() { return run; }
	static int write_slot() { return int(run & 1); }
	static int drain_slot() { return selftest_overlap ? int(run & 1) : (int(run & 1) ^ 1); }

	// The record node of the three-node shapes: it owns the drain slot, and what it queues itself
	// goes there.
	static bool on_record_node() { return MacrameRecord::holds_grant() && !MacrameRender::holds_grant(); }
	// A body that holds the render grant beside the recording (the single node, the blue thread's
	// sync draw): no overlap is possible and both slots are its to drain.
	static bool drains_both() { return ts::current_worker_index() < 0 || (MacrameRender::holds_grant() && MacrameRecord::holds_grant()) || (same_run_edge && MacrameRecord::holds_grant()); }
	// The update node of the three-node shapes: what it frees waits.
	static bool defers_frees() { return ts::current_worker_index() >= 0 && !MacrameRecord::holds_grant(); }
	// The slot a queue from this node goes to.
	static int add_slot() { return on_record_node() ? drain_slot() : write_slot(); }
	// A record node's drain (a body without the render grant): the stamp the boundary compares.
	static bool stamps_drain() { return ts::current_worker_index() >= 0 && !MacrameRender::holds_grant(); }
	static void check_boundary(const uint64_t *p_update_written, const uint64_t *p_record_drained, const char *p_what) {
		for (int slot = 0; slot < 2; slot++) {
			CRASH_COND_MSG(p_update_written[slot] == run && p_record_drained[slot] == run, String("Macrame: ") + p_what + ": the scene update wrote and the record node drained the same run-parity slot in run " + itos(run) + " (run parity overlap).");
		}
	}
};

// The two links of a queued object, one per slot.
template <class T>
struct MacrameParityLinks {
	SelfList<T> slot[2];
	explicit MacrameParityLinks(T *p_self) :
			slot{ SelfList<T>(p_self), SelfList<T>(p_self) } {}
	bool in_any_list() const { return slot[0].in_list() || slot[1].in_list(); }
};

// An intrusive dirty list by run parity (see `MacrameRunParity`).
template <class T>
struct MacrameParityList {
	typename SelfList<T>::List slots[2];
	uint64_t update_written[2] = { UINT64_MAX, UINT64_MAX };
	uint64_t record_drained[2] = { UINT64_MAX, UINT64_MAX };

	void check_boundary(const char *p_what) const { MacrameRunParity::check_boundary(update_written, record_drained, p_what); }
	// Queued in the slot this node adds to? (Never a look at the other slot's link.)
	static bool queued(const MacrameParityLinks<T> &p_links) { return p_links.slot[MacrameRunParity::add_slot()].in_list(); }
	void add(MacrameParityLinks<T> &p_links) {
		const int w = MacrameRunParity::add_slot();
		if (p_links.slot[w].in_list()) {
			return;
		}
		if (!MacrameRunParity::on_record_node()) {
			update_written[w] = MacrameRunParity::current();
		}
		slots[w].add(&p_links.slot[w]);
	}
	// Both links, from a body that owns both slots (a free applied two runs later, the teardown).
	static void remove(MacrameParityLinks<T> &p_links) {
		for (int i = 0; i < 2; i++) {
			if (p_links.slot[i].in_list()) {
				p_links.slot[i].remove_from_list();
			}
		}
	}
	bool has_pending() const { return slots[0].first() != nullptr || slots[1].first() != nullptr; }

	// Everything the caller may drain now: the drain slot, and the write slot when no overlap is
	// possible. `p_each(object, live)`: `live` says the object comes from the write slot (its
	// data was not copied at a run boundary).
	template <class F>
	void drain(F &&p_each) {
		const int d = MacrameRunParity::drain_slot();
		if (MacrameRunParity::stamps_drain()) {
			record_drained[d] = MacrameRunParity::current();
		}
		while (SelfList<T> *e = slots[d].first()) {
			slots[d].remove(e);
			p_each(e->self(), false);
		}
		if (MacrameRunParity::drains_both()) {
			const int w = MacrameRunParity::write_slot();
			while (SelfList<T> *e = slots[w].first()) {
				slots[w].remove(e);
				p_each(e->self(), true);
			}
		}
	}
	// The run boundary (blue thread): the objects the update node queued this run.
	template <class F>
	void for_each_written(F &&p_each) {
		for (SelfList<T> *e = slots[MacrameRunParity::write_slot()].first(); e; e = e->next()) {
			p_each(e->self());
		}
	}
};

// Frees by run: what the update node frees in run k, the update node destroys in run k + 2 (the
// storages' tables are the update node's; the record nodes of runs k and k + 1 may still name the
// object). `apply()` from the update node's head; from a body that owns everything (the blue
// thread, the teardown), everything goes.
template <class T>
struct MacrameRunFrees {
	struct Entry {
		uint64_t run = 0;
		T item;
	};
	LocalVector<Entry> pending;

	// True when the free was deferred (the caller does nothing more); false when the caller must
	// free now (no overlap possible).
	bool defer(const T &p_item) {
		if (!MacrameRunParity::defers_frees()) {
			return false;
		}
		Entry e;
		e.run = MacrameRunParity::current();
		e.item = p_item;
		pending.push_back(e);
		return true;
	}
	template <class F>
	void apply(F &&p_free, bool p_all = false) {
		const uint64_t now = MacrameRunParity::current();
		uint32_t kept = 0;
		for (uint32_t i = 0; i < pending.size(); i++) {
			if (p_all || pending[i].run + 2 <= now) {
				p_free(pending[i].item);
			} else {
				pending[kept++] = pending[i];
			}
		}
		pending.resize(kept);
	}
	bool has_pending() const { return pending.size() > 0; }
};

// Deferred by one run to the record node: the device's dispose lists and the decal atlas are the
// recording's, and the record node of the next run applies what the update node asked for.
template <class T>
struct MacrameParityFrees {
	LocalVector<T> slots[2];
	uint64_t update_written[2] = { UINT64_MAX, UINT64_MAX };
	uint64_t record_drained[2] = { UINT64_MAX, UINT64_MAX };

	void check_boundary(const char *p_what) const { MacrameRunParity::check_boundary(update_written, record_drained, p_what); }
	bool defer(const T &p_item) {
		if (!MacrameRunParity::defers_frees()) {
			return false;
		}
		const int w = MacrameRunParity::write_slot();
		update_written[w] = MacrameRunParity::current();
		slots[w].push_back(p_item);
		return true;
	}
	template <class F>
	void apply(F &&p_free) {
		const int d = MacrameRunParity::drain_slot();
		if (MacrameRunParity::stamps_drain()) {
			record_drained[d] = MacrameRunParity::current();
		}
		for (const T &item : slots[d]) {
			p_free(item);
		}
		slots[d].clear();
		if (MacrameRunParity::drains_both()) {
			const int w = MacrameRunParity::write_slot();
			for (const T &item : slots[w]) {
				p_free(item);
			}
			slots[w].clear();
		}
	}
	bool has_pending() const { return slots[0].size() || slots[1].size(); }
};

#endif // MACRAME_ENABLED
