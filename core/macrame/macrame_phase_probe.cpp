/**************************************************************************/
/*  macrame_phase_probe.cpp                                               */
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

#include "macrame_phase_probe.h"

#ifdef MACRAME_ENABLED

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"

#include "ts/scheduler.h"

#include <cstring>

namespace {

constexpr int MAX_PHASES = 64;
constexpr int KINDS = 4;
constexpr const char *KIND_NAMES[KINDS] = { "plain_frame", "tick_frame", "tick_only", "sync" };

struct Phase {
	const char *name = nullptr;
	double sum_us = 0.0;
	double max_us = 0.0;
	uint64_t n = 0;
};

struct KindStats {
	Phase phases[MAX_PHASES];
	int phase_count = 0;
	uint64_t frames = 0;
	double total_sum_us = 0.0;
	double total_max_us = 0.0;
};

KindStats stats[KINDS];

// One lane per worker (slot 0 is a blue thread). A node body runs on one worker for its whole
// span, so the worker index identifies the lane without any thread-local of our own.
constexpr int MAX_LANES = 128;
struct Lane {
	bool active = false;
	int kind = 0;
	uint64_t start_us = 0;
	uint64_t last_mark_us = 0;
	const char *total_name = nullptr;
};
Lane lanes[MAX_LANES];

Lane *_lane() {
	const int w = ts::current_worker_index() + 1;
	return (w >= 0 && w < MAX_LANES) ? &lanes[w] : nullptr;
}

bool _enabled() {
	static const bool e = OS::get_singleton()->get_environment("MACRAME_RENDER_PHASES") == "1";
	return e;
}

void _report(int p_kind) {
	KindStats &k = stats[p_kind];
	if (k.frames == 0) {
		return;
	}
	String out = vformat("MACRAME_PHASES kind=%s frames=%d total_mean_us=%.0f total_max_us=%.0f\n", KIND_NAMES[p_kind], (int64_t)k.frames, k.total_sum_us / k.frames, k.total_max_us);
	for (int i = 0; i < k.phase_count; i++) {
		const Phase &p = k.phases[i];
		out += vformat("  %-36s mean_us=%8.1f  max_us=%8.0f  per_frame_calls=%.2f\n", p.name, p.sum_us / k.frames, p.max_us, double(p.n) / k.frames);
	}
	print_line(out);
}

} // namespace

bool MacramePhaseProbe::enabled() {
	return _enabled();
}

void MacramePhaseProbe::lane_begin(int p_kind, const char *p_total_name) {
	if (!_enabled()) {
		return;
	}
	Lane *l = _lane();
	if (!l) {
		return;
	}
	l->active = true;
	l->kind = (p_kind >= 0 && p_kind < KINDS) ? p_kind : KINDS - 1;
	l->start_us = OS::get_singleton()->get_ticks_usec();
	l->last_mark_us = l->start_us;
	l->total_name = p_total_name;
}

static void _charge(KindStats &k, const char *p_name, double dt);

void MacramePhaseProbe::mark(const char *p_name) {
	if (!_enabled()) {
		return;
	}
	Lane *l = _lane();
	if (!l || !l->active) {
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const double dt = double(now - l->last_mark_us);
	l->last_mark_us = now;
	_charge(stats[l->kind], p_name, dt);
}

static void _charge(KindStats &k, const char *p_name, double dt) {
	Phase *p = nullptr;
	for (int i = 0; i < k.phase_count; i++) {
		// Names are string literals: the pointer identifies the phase; strcmp is the fallback for
		// two translation units spelling the same literal.
		if (k.phases[i].name == p_name || strcmp(k.phases[i].name, p_name) == 0) {
			p = &k.phases[i];
			break;
		}
	}
	if (!p) {
		if (k.phase_count == MAX_PHASES) {
			return;
		}
		p = &k.phases[k.phase_count++];
		p->name = p_name;
	}
	p->sum_us += dt;
	p->n++;
	if (dt > p->max_us) {
		p->max_us = dt;
	}
}

void MacramePhaseProbe::lane_end() {
	if (!_enabled()) {
		return;
	}
	Lane *l = _lane();
	if (!l || !l->active) {
		return;
	}
	mark("(tail)");
	l->active = false;
	KindStats &k = stats[l->kind];
	const double total = double(OS::get_singleton()->get_ticks_usec() - l->start_us);
	_charge(k, l->total_name, total);
	if (l->total_name && strcmp(l->total_name, "render node") == 0) {
		// The single-node shape: the lane is the frame.
		k.frames++;
		k.total_sum_us += total;
		if (total > k.total_max_us) {
			k.total_max_us = total;
		}
	} else if (l->total_name && strcmp(l->total_name, "record node") == 0) {
		k.frames++; // One record per frame; the per-phase means divide by this.
	}
	if (k.frames > 0 && k.frames % 1000 == 0 && l->total_name && (strcmp(l->total_name, "render node") == 0 || strcmp(l->total_name, "record node") == 0)) {
		_report(l->kind);
	}
}

#endif // MACRAME_ENABLED
