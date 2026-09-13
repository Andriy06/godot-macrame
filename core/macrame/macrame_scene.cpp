/**************************************************************************/
/*  macrame_scene.cpp                                                     */
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

#include "macrame_scene.h"

#ifdef MACRAME_ENABLED

#include "core/macrame/macrame_render_grant.h"
#include "core/macrame/macrame_render_outputs.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "servers/navigation_3d/navigation_server_3d.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"

#include "graph_trace.h" // Macrame tools: the aggregated runtime trace of a static graph.

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define MACRAME_NO_INLINE __declspec(noinline)
#else
#define MACRAME_NO_INLINE [[gnu::noinline]]
#endif

namespace {

// Experiment: a scene phase (the physics tick's shards, or the frame's process shards) as a
// compiled `Static_task_graph` - one node per shard, edges derived from the declared access -
// with Macrame's aggregated runtime trace attached. The per-run inputs (which groups each
// shard runs, for which tree, which phase) are plain state the node bodies read: a graph is
// build-once and its bodies take no run arguments. MACRAME_STATIC_GRAPH=0 selects the
// dynamic `ts::async` fan-out instead; MACRAME_TRACE_DIR=<dir> writes the DOT dump at
// compile and the average-run SVGs at shutdown.
struct PhaseGraph {
	ts::Static_task_graph graph;
	ts::tools::Graph_trace trace;
	std::vector<std::vector<void *>> buckets;
	SceneTree *tree = nullptr;
	bool physics = false;
	bool built = false;
	uint64_t runs = 0;
	bool written = false;
	std::string names[MacrameScene::SHARD_COUNT]; // `ts::Named` keeps the pointer: stable storage.
};

struct NavGrantToken {
	int unused = 0; // The navigation server's guarded identity (its update node writes, shards read).
};

// Everything the frame graphs' node bodies read: the captured batches, the tree and the tick's
// step. Shared by all three graphs, so the bodies are the same lambdas whichever graph runs.
struct FrameState {
	std::vector<std::vector<void *>> tick_buckets;
	std::vector<std::vector<void *>> frame_buckets;
	std::vector<void *> gather_tick_groups; // Gather groups captured in the tick's physics phase,
	std::vector<void *> gather_frame_groups; // and in the frame's process phase.
	SceneTree *tree = nullptr;
	bool capturing = false;
	double tick_step = 0.0;
};

// One compiled graph per frame shape. Godot's iteration carries 0, 1 or (catch-up) N physics
// ticks; a graph cannot express that, so instead of a flag that makes nodes no-ops there are
// three graphs over the same node bodies and the same guarded objects.
struct FrameGraph {
	ts::Static_task_graph graph;
	ts::tools::Graph_trace trace;
	bool with_tick = false; // 32 tick shards + the physics step + navigation.
	bool with_frame = false; // 32 frame shards (after tick shard i, when both are present).
	bool built = false;
	uint64_t runs = 0;
	bool written = false;
	const char *dot_name = nullptr;
	const char *svg_name = nullptr;
	int kind = 3; // 0 plain_frame, 1 tick_frame, 2 tick_only (MacrameScene::frame_graph_kind).
	std::string tick_names[MacrameScene::SHARD_COUNT]; // `ts::Named` keeps the pointer: stable storage.
	std::string frame_names[MacrameScene::SHARD_COUNT];
};

// The compiled graphs, held apart from the rest of the state because they die first. A
// `Static_task_graph` names the guarded objects its nodes take grants on, and Macrame makes
// destroying one of those objects while a compiled graph still references it fatal. Two of
// them - the physics space (`PhysicsServer3DWrapMT`) and the render outputs - belong to
// objects that `Main::cleanup` destroys long before `MacrameRuntime::finish`, so the graphs
// are torn down at the end of the main loop instead (`MacrameScene::finish_graphs`).
struct Graphs {
	FrameGraph tick_frame;
	FrameGraph plain_frame;
	FrameGraph tick_only;
	PhaseGraph physics_graph;
	PhaseGraph process_graph;
};

struct State {
	std::vector<std::unique_ptr<ts::Guarded<SceneShardToken>>> shards;
	std::unique_ptr<ts::Guarded<NavGrantToken>> nav_guard;
	FrameState frame;
	std::unique_ptr<Graphs> graphs = std::make_unique<Graphs>();
	std::vector<SceneShardToken *> shard_ptrs; // Stable addresses of the guarded tokens, for the harness.
	std::unique_ptr<ts::Guarded<SceneShardToken>> main_shard;
	SceneShardToken *main_ptr = nullptr;
	std::atomic<int> next_shard{ 0 };
};
State *state = nullptr;
MacrameScene::FrameNodeAdder render_node_adder = nullptr;
bool graph_running = false;
int graph_kind = 3;
bool queries_flushed = false; // Set by `body sync` on a worker, read after the run's join.
void _write_phase_trace(PhaseGraph &pg);
void _write_frame_trace(FrameGraph &fg);
namespace {
// Graph_trace freezes its utilization bucket width from the makespan mean of the first bucketed
// run. The benchmark's first runs execute an empty scene (~0.2 ms), so a trace attached at
// compile time would bucket a 7 ms run into a 0.2 ms wash. The trace is attached only once the
// scene is populated, at run MACRAME_TRACE_WARMUP (default 300); the SVG is written 3000 runs later.
uint64_t trace_warmup_runs() {
	static const uint64_t n = [] {
		const String v = OS::get_singleton()->get_environment("MACRAME_TRACE_WARMUP");
		return v.is_empty() ? uint64_t(300) : uint64_t(v.to_int());
	}();
	return n;
}
} // namespace


} // namespace

thread_local int macrame_tls_current_shard = -1;
thread_local bool macrame_tls_in_shard_task = false;

namespace {
// What a task that is not a shard may do with the shards' nodes by holding the main shard's write
// grant, which excludes every shard node (see macrame_scene.h): read any of them (`gather`), or
// write them too (`body sync`).
enum FamilyAccess {
	FAMILY_NONE,
	FAMILY_READ,
	FAMILY_WRITE,
};
thread_local FamilyAccess tls_family_access = FAMILY_NONE;

MACRAME_NO_INLINE void set_context(int p_shard, bool p_in_task, FamilyAccess p_family = FAMILY_NONE) {
	macrame_tls_current_shard = p_shard;
	macrame_tls_in_shard_task = p_in_task;
	tls_family_access = p_family;
}
} // namespace

void MacrameScene::init() {
	if (state) {
		return;
	}
	state = new State();
	static const char *shard_name = "scene_shard";
	for (int i = 0; i < SHARD_COUNT; i++) {
		state->shards.emplace_back(new ts::Guarded<SceneShardToken>(ts::Named{ shard_name }));
		SceneShardToken *ptr = nullptr;
		state->shards.back()->access([&](SceneShardToken &t) { t.index = i; ptr = &t; }).sync();
		state->shard_ptrs.push_back(ptr);
	}
	state->main_shard.reset(new ts::Guarded<SceneShardToken>(ts::Named{ "scene_main_shard" }));
	state->main_shard->access([&](SceneShardToken &t) { state->main_ptr = &t; }).sync();
	state->nav_guard.reset(new ts::Guarded<NavGrantToken>(ts::Named{ "navigation" }));
	state->frame.tick_buckets.resize(SHARD_COUNT);
	state->frame.frame_buckets.resize(SHARD_COUNT);
	// One compiled graph per frame shape (see macrame_scene.h); built lazily, because the
	// physics space's Guarded only exists once the physics server has started.
	Graphs &g = *state->graphs;
	g.tick_frame.with_tick = true;
	g.tick_frame.with_frame = true;
	g.tick_frame.kind = 1;
	g.plain_frame.kind = 0;
	g.tick_only.kind = 2;
	g.tick_frame.dot_name = "macrame_tick_frame.dot";
	g.tick_frame.svg_name = "macrame_tick_frame_avg.svg";
	g.plain_frame.with_frame = true;
	g.plain_frame.dot_name = "macrame_plain_frame.dot";
	g.plain_frame.svg_name = "macrame_plain_frame_avg.svg";
	g.tick_only.with_tick = true;
	g.tick_only.dot_name = "macrame_tick_only.dot";
	g.tick_only.svg_name = "macrame_tick_only_avg.svg";
	print_verbose(vformat("Macrame: %d scene shards.", SHARD_COUNT));
}

void MacrameScene::finish_graphs() {
	if (!state || !state->graphs) {
		return;
	}
	Graphs &g = *state->graphs;
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	if (!dir.is_empty()) {
		for (FrameGraph *fg : { &g.tick_frame, &g.plain_frame, &g.tick_only }) {
			if (fg->built && !fg->written) {
				_write_frame_trace(*fg);
			}
		}
		for (PhaseGraph *pg : { &g.physics_graph, &g.process_graph }) {
			if (pg->built && !pg->written) {
				_write_phase_trace(*pg);
			}
		}
	}
	// Before the physics server and the renderer go: a compiled graph outliving a guarded
	// object it names is fatal in Macrame, and both of those objects die inside `Main::cleanup`.
	state->graphs.reset();
}

void MacrameScene::finish() {
	finish_graphs(); // A no-op when the main loop already ran it; a safety net when it did not.
	delete state;
	state = nullptr;
}

bool MacrameScene::is_enabled() {
	return state != nullptr;
}

namespace {
int active_shard_count() {
	// MACRAME_SHARDS=<n> caps how many of the shards receive groups (scaling experiments).
	static int n = [] {
		String v = OS::get_singleton()->get_environment("MACRAME_SHARDS");
		int c = v.is_empty() ? MacrameScene::SHARD_COUNT : v.to_int();
		return CLAMP(c, 1, MacrameScene::SHARD_COUNT);
	}();
	return n;
}
} // namespace

int MacrameScene::assign_shard() {
	return state ? state->next_shard.fetch_add(1) % active_shard_count() : -1;
}

void MacrameScene::check_write(const Node *p_node) {
	if (!in_shard_task() || !p_node->is_inside_tree()) {
		return;
	}
	const int s = SceneTree::macrame_shard_of(p_node);
	if (s >= 0 && tls_family_access == FAMILY_WRITE) {
		ts::access_check(state->main_ptr); // Every shard node reads the main shard: its write grant covers them all.
		return;
	}
	ts::access_check(s < 0 ? state->main_ptr : state->shard_ptrs[s]);
}

void MacrameScene::check_read(const Node *p_node) {
	if (!in_shard_task() || !p_node->is_inside_tree()) {
		return;
	}
	const int s = SceneTree::macrame_shard_of(p_node);
	if (s >= 0 && tls_family_access != FAMILY_NONE) {
		ts::access_check(state->main_ptr); // `gather` and `body sync` read any shard's nodes.
		return;
	}
	const SceneShardToken *ptr = s < 0 ? state->main_ptr : state->shard_ptrs[s];
	ts::access_check(ptr);
}

void MacrameScene::check_main(const Node *p_node) {
	if (!in_shard_task() || !p_node->is_inside_tree()) {
		return;
	}
	ts::access_check(state->main_ptr);
}

namespace {
void _write_phase_trace(PhaseGraph &pg) {
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	if (dir.is_empty() || !pg.built) {
		return;
	}
	const String path = dir + (pg.physics ? "/macrame_physics_phase_avg.svg" : "/macrame_process_phase_avg.svg");
	const bool ok = pg.trace.write_SVG(path.utf8().get_data());
	pg.written = true;
	print_line(vformat("Macrame: trace of %d runs %s -> %s", (int)pg.runs, ok ? "written" : "FAILED", path));
	fflush(stdout);
}

void _build_phase_graph(PhaseGraph &pg, bool p_physics, ts::Guarded<PhysicsGrantToken> *p_space) {
	pg.physics = p_physics;
	pg.buckets.resize(MacrameScene::SHARD_COUNT);
	for (int s = 0; s < MacrameScene::SHARD_COUNT; s++) {
		pg.names[s] = std::string(p_physics ? "physics shard " : "process shard ") + std::to_string(s);
		auto body = [&pg, s](const MacrameRenderOutputs &p_render_outputs) {
			ScriptServer::thread_enter();
			set_context(s, true);
			MacrameRenderSnapshot::set_current(&p_render_outputs);
			for (void *g : pg.buckets[s]) {
				pg.tree->macrame_process_group(g, pg.physics);
			}
			MacrameRenderSnapshot::set_current(nullptr);
			set_context(-1, false);
		};
		if (p_physics) {
			pg.graph.add_node(ts::Named{ pg.names[s].c_str() },
					[body](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &, const MacrameRenderOutputs &p_render_outputs) { body(p_render_outputs); },
					*state->shards[s], *state->main_shard, *p_space, MacrameRenderSnapshot::front());
		} else {
			pg.graph.add_node(ts::Named{ pg.names[s].c_str() },
					[body](SceneShardToken &, const SceneShardToken &, const MacrameRenderOutputs &p_render_outputs) { body(p_render_outputs); },
					*state->shards[s], *state->main_shard, MacrameRenderSnapshot::front());
		}
	}
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	const String dot = dir.is_empty() ? String() : dir + (p_physics ? "/macrame_physics_phase.dot" : "/macrame_process_phase.dot");
	pg.graph.compile(dot.is_empty() ? nullptr : dot.utf8().get_data());
	// The trace is attached at run trace_warmup_runs() (see above), not here.
	pg.built = true;
	print_verbose(vformat("Macrame: %s phase compiled as a static graph (%d nodes).", p_physics ? "physics" : "process", pg.graph.node_count()));
}
} // namespace

namespace {
// Gather groups outside the frame graph: the blue thread runs them once the batch is joined, which
// is where stock Godot runs a later-ordered group, and where nothing else touches the shards.
void _run_gather_on_blue(SceneTree *p_tree, const std::vector<void *> &p_groups, bool p_physics) {
	for (void *g : p_groups) {
		p_tree->macrame_process_group(g, p_physics);
	}
}
} // namespace

void MacrameScene::run_groups(SceneTree *p_tree, void **p_groups, int p_group_count, bool p_physics) {
	ERR_FAIL_NULL(state);
	ts::Guarded<PhysicsGrantToken> *space = MacramePhysics::get_guarded();
	ERR_FAIL_NULL_MSG(space, "Macrame: the physics server did not register its guarded space.");

	// Bucket the groups by shard, gather groups apart.
	std::vector<std::vector<void *>> buckets(SHARD_COUNT);
	std::vector<void *> gather;
	bool any_shard_group = false;
	for (int i = 0; i < p_group_count; i++) {
		if (p_tree->macrame_is_gather_group(p_groups[i])) {
			gather.push_back(p_groups[i]);
			continue;
		}
		const int s = p_tree->macrame_shard_of_group(p_groups[i]);
		if (s < 0) {
			// A sub-thread group with no shard yet: run it on the blue thread.
			p_tree->macrame_process_group(p_groups[i], p_physics);
			continue;
		}
		buckets[s].push_back(p_groups[i]);
		any_shard_group = true;
	}

	if (state->frame.capturing) {
		// Frame-graph mode: a graph runs this batch; groups without a shard already ran above. A phase
		// can hand over more than one threaded batch - a gather group sits in a later
		// `process_thread_group_order`, so that stock Godot runs it after the shards - so batches are
		// appended, never swapped: a swap dropped the earlier batch's shard groups. Two batches of
		// shard groups lose their mutual order inside one graph phase, which is worth saying.
		std::vector<std::vector<void *>> &dst = p_physics ? state->frame.tick_buckets : state->frame.frame_buckets;
		if (any_shard_group) {
			for (const std::vector<void *> &b : dst) {
				if (!b.empty()) {
					ERR_PRINT_ONCE("Macrame: two threaded batches of shard groups in one phase; the frame graph runs them as one, unordered.");
					break;
				}
			}
			for (int s = 0; s < SHARD_COUNT; s++) {
				dst[s].insert(dst[s].end(), buckets[s].begin(), buckets[s].end());
			}
		}
		std::vector<void *> &gather_dst = p_physics ? state->frame.gather_tick_groups : state->frame.gather_frame_groups;
		gather_dst.insert(gather_dst.end(), gather.begin(), gather.end());
		state->frame.tree = p_tree;
		return;
	}

	if (!any_shard_group) {
		_run_gather_on_blue(p_tree, gather, p_physics); // A batch of gather groups alone.
		return;
	}

	static const bool use_graph = OS::get_singleton()->get_environment("MACRAME_STATIC_GRAPH") != "0";
	if (use_graph && state->graphs) {
		PhaseGraph &pg = p_physics ? state->graphs->physics_graph : state->graphs->process_graph;
		if (!pg.built) {
			_build_phase_graph(pg, p_physics, space);
		}
		pg.tree = p_tree;
		pg.physics = p_physics;
		pg.buckets.swap(buckets);
		pg.graph.execute().sync(); // One run at a time; the phases are sequential on the main thread.
		_run_gather_on_blue(p_tree, gather, p_physics);
		++pg.runs;
		if (pg.runs == trace_warmup_runs()) {
			pg.graph.set_trace(&pg.trace); // From here on: populated runs only.
		}
		if (pg.runs == trace_warmup_runs() + 3000 && !pg.written) {
			// The average run so far, written mid-run (the benchmark harness kills the process; the
			// shutdown path is not reached).
			_write_phase_trace(pg);
		}
		return;
	}

	std::vector<ts::Task<void>> tasks;
	tasks.reserve(SHARD_COUNT);
	for (int s = 0; s < SHARD_COUNT; s++) {
		if (buckets[s].empty()) {
			continue;
		}
		std::vector<void *> groups = buckets[s];
		tasks.push_back(ts::async(
				[p_tree, groups, p_physics, s](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &, const MacrameRenderOutputs &p_render_outputs) {
					ScriptServer::thread_enter();
					set_context(s, true);
					MacrameRenderSnapshot::set_current(&p_render_outputs);
					for (void *g : groups) {
						p_tree->macrame_process_group(g, p_physics);
					}
					MacrameRenderSnapshot::set_current(nullptr);
					set_context(-1, false);
				},
				*state->shards[s], *state->main_shard, *space, MacrameRenderSnapshot::front()));
	}
	for (ts::Task<void> &t : tasks) {
		t.sync();
	}
	_run_gather_on_blue(p_tree, gather, p_physics);
}

namespace {

void _write_frame_trace(FrameGraph &fg) {
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	if (dir.is_empty() || !fg.built) {
		return;
	}
	const String path = dir + "/" + String(fg.svg_name);
	const bool ok = fg.trace.write_SVG(path.utf8().get_data());
	fg.written = true;
	print_line(vformat("Macrame: frame trace of %d runs %s -> %s", (int)fg.runs, ok ? "written" : "FAILED", path));
	fflush(stdout);
}

void _run_bucket(const std::vector<void *> &p_groups, int p_shard, bool p_physics, const MacrameRenderOutputs &p_outputs) {
	if (p_groups.empty()) {
		return;
	}
	ScriptServer::thread_enter();
	set_context(p_shard, true);
	MacrameRenderSnapshot::set_current(&p_outputs);
	for (void *g : p_groups) {
		state->frame.tree->macrame_process_group(g, p_physics);
	}
	MacrameRenderSnapshot::set_current(nullptr);
	set_context(-1, false);
}

// `gather` (see macrame_scene.h): the tick's gather groups, on a worker, holding the main shard's
// write grant: it covers their own nodes (main-shard nodes) and a read of any shard's node.
void _run_gather(const std::vector<void *> &p_groups, bool p_physics) {
	if (p_groups.empty()) {
		return;
	}
	ScriptServer::thread_enter();
	set_context(-1, true, FAMILY_READ);
	for (void *g : p_groups) {
		state->frame.tree->macrame_process_group(g, p_physics);
	}
	set_context(-1, false);
}

// `body sync` (see macrame_scene.h): the step's state callbacks at the tail of the tick that
// stepped. The rigid bodies may live in any shard or in the main one, so it writes both.
void _body_sync() {
	if (state->frame.tree && state->frame.tree->is_physics_interpolation_enabled()) {
		return; // The head delivers them, after `iteration_prepare` has snapshotted the transforms.
	}
	// MACRAME_BODY_SYNC=0 leaves them to the head as well: an isolating knob for A/B work.
	static const bool enabled = OS::get_singleton()->get_environment("MACRAME_BODY_SYNC") != "0";
	if (!enabled) {
		return;
	}
	ScriptServer::thread_enter();
	set_context(-1, true, FAMILY_WRITE);
	static_cast<PhysicsServer3DWrapMT *>(PhysicsServer3D::get_singleton())->macrame_flush_queries_under_grant();
	set_context(-1, false);
	queries_flushed = true;
}

// The three frame graphs are built from this one function; `with_tick` / `with_frame` select the
// shape. The bodies are the same lambdas over the same `FrameState` and the same guarded objects,
// so a node means exactly one thing whichever graph it belongs to. Only `gather` can run empty (a
// tick in a scene with no gather group).
void _build_frame_graph(FrameGraph &fg, ts::Guarded<PhysicsGrantToken> *p_space) {
	FrameState &fs = state->frame;
	std::vector<ts::Graph_node> tick_nodes;
	std::vector<ts::Graph_node> frame_nodes;
	ts::Graph_node step;
	if (fg.with_tick) {
		tick_nodes.reserve(MacrameScene::SHARD_COUNT);
		for (int s = 0; s < MacrameScene::SHARD_COUNT; s++) {
			fg.tick_names[s] = "tick shard " + std::to_string(s);
			tick_nodes.push_back(fg.graph.add_node(ts::Named{ fg.tick_names[s].c_str() },
					[&fs, s](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &, const NavGrantToken &, const MacrameRenderOutputs &p_outputs) {
						_run_bucket(fs.tick_buckets[s], s, true, p_outputs);
					},
					*state->shards[s], *state->main_shard, *p_space, *state->nav_guard, MacrameRenderSnapshot::front()));
		}
		// The step writes the space every tick shard read: compile() derives the edges. It also
		// applies the tick's staged body writes first (upstream FIFO semantics).
		step = fg.graph.add_node("physics step", [&fs](PhysicsGrantToken &) {
			static_cast<PhysicsServer3DWrapMT *>(PhysicsServer3D::get_singleton())->macrame_step_under_grant(fs.tick_step);
		}, *p_space);
		step.set_priority(ts::Priority::high);
		fg.graph.add_node("navigation", [&fs](NavGrantToken &) {
			NavigationServer3D::get_singleton()->physics_process(fs.tick_step);
		}, *state->nav_guard).set_priority(ts::Priority::high);
	}
	if (fg.with_frame) {
		for (int s = 0; s < MacrameScene::SHARD_COUNT; s++) {
			fg.frame_names[s] = "frame shard " + std::to_string(s);
			// A shard's process phase follows its own physics phase (the derived write-write edge; the
			// explicit `after` fixes the direction) and nothing else: it overlaps the tick's tail, the
			// step and navigation. Physics queries from _process are refused by the wrapper's guard.
			ts::Graph_node n = fg.graph.add_node(ts::Named{ fg.frame_names[s].c_str() },
					[&fs, s](SceneShardToken &, const SceneShardToken &, const MacrameRenderOutputs &p_outputs) {
						_run_bucket(fs.frame_buckets[s], s, false, p_outputs);
					},
					*state->shards[s], *state->main_shard, MacrameRenderSnapshot::front());
			if (fg.with_tick) {
				n.after(tick_nodes[s]);
			}
			frame_nodes.push_back(n);
		}
	}
	if (fg.with_tick) {
		// `gather`: after every shard node of the run. Its main-shard write orders it there anyway,
		// since every shard node reads the main shard; the explicit edges say it is meant.
		ts::Graph_node gather = fg.graph.add_node("gather", [&fs](SceneShardToken &) {
			_run_gather(fs.gather_tick_groups, true);
		}, *state->main_shard);
		for (const ts::Graph_node &n : tick_nodes) {
			gather.after(n);
		}
		for (const ts::Graph_node &n : frame_nodes) {
			gather.after(n);
		}
		// `body sync`: after the step whose results it delivers, and after every frame shard, so the
		// frame's `_process` sees the previous tick's transforms. Declared after `gather`, so the two
		// writers of the main shard take it in that order: gather is ready first.
		ts::Graph_node body_sync = fg.graph.add_node("body sync", [](PhysicsGrantToken &, SceneShardToken &) {
			_body_sync();
		}, *p_space, *state->main_shard);
		body_sync.after(step);
		for (const ts::Graph_node &n : frame_nodes) {
			body_sync.after(n);
		}
		body_sync.set_priority(ts::Priority::high);
	}
	if (fg.with_frame && render_node_adder) {
		// `render` and `submit`. They declare the render server's and the device's guarded
		// objects, which no other node names, so `compile()` derives no edge to anything here and
		// none is wanted: the three chains of a run consume different frames' values. The render
		// node applies the command journal cut at the last frame boundary and draws the frame the
		// blue thread posted then; the submit node submits what the *previous* run's render node
		// staged. See `RenderingServerDefault::_macrame_add_frame_nodes`.
		render_node_adder(&fg.graph);
	}
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	const String dot = dir.is_empty() ? String() : dir + "/" + String(fg.dot_name);
	fg.graph.compile(dot.is_empty() ? nullptr : dot.utf8().get_data());
	// The trace is attached at run trace_warmup_runs() (see above), not here.
	fg.built = true;
	print_verbose(vformat("Macrame: %s compiled as a static graph (%d nodes).", fg.svg_name, fg.graph.node_count()));
}

// Run one of the graphs and hand the tree back the flushes its phases owe the shards.
void _frame_run(FrameGraph &fg, SceneTree *p_tree) {
	FrameState &fs = state->frame;
	fs.capturing = false;
	if (!fg.built) {
		_build_frame_graph(fg, MacramePhysics::get_guarded());
	}
	fs.tree = p_tree;
	graph_kind = fg.kind;
	fg.graph.execute().sync();
	graph_kind = 3;
	// A gather group's `_process`, on the blue thread after the run (see macrame_scene.h).
	_run_gather_on_blue(p_tree, fs.gather_frame_groups, false);
	// Both sides, unconditionally: a phase that captured a batch and then quit the iteration
	// (`physics_process` returning true) must not leave it for the next run's graph.
	for (auto &b : fs.tick_buckets) {
		b.clear();
	}
	for (auto &b : fs.frame_buckets) {
		b.clear();
	}
	fs.gather_tick_groups.clear();
	fs.gather_frame_groups.clear();
	++fg.runs;
	if (fg.runs == trace_warmup_runs()) {
		fg.graph.set_trace(&fg.trace); // From here on: populated runs only.
	}
	if (fg.runs == trace_warmup_runs() + 3000 && !fg.written) {
		// The average run so far, written mid-run (the benchmark harness kills the process; the
		// shutdown path is not reached).
		_write_frame_trace(fg);
	}
	p_tree->macrame_post_shards();
}

} // namespace

bool MacrameScene::frame_graph_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("MACRAME_FRAME_GRAPH") != "0";
	return enabled && state != nullptr && state->graphs != nullptr && MacramePhysics::get_guarded() != nullptr;
}

void MacrameScene::frame_set_capturing(bool p_capturing) {
	ERR_FAIL_NULL(state);
	state->frame.capturing = p_capturing;
}

void MacrameScene::set_frame_render_nodes(FrameNodeAdder p_adder) {
	render_node_adder = p_adder;
}

int MacrameScene::frame_graph_kind() {
	return graph_kind;
}

bool MacrameScene::frame_graph_running() {
	return graph_running;
}

void MacrameScene::frame_set_graph_running(bool p_running) {
	graph_running = p_running;
}

void MacrameScene::frame_set_tick(double p_step) {
	ERR_FAIL_NULL(state);
	state->frame.tick_step = p_step;
}

bool MacrameScene::frame_take_queries_flushed() {
	const bool flushed = queries_flushed;
	queries_flushed = false;
	return flushed;
}

void MacrameScene::frame_execute_tick(SceneTree *p_tree) {
	ERR_FAIL_NULL(state);
	ERR_FAIL_NULL(state->graphs);
	GodotProfileZone("Macrame: tick_only graph");
	_frame_run(state->graphs->tick_only, p_tree);
}

void MacrameScene::frame_execute(SceneTree *p_tree, bool p_with_tick) {
	ERR_FAIL_NULL(state);
	ERR_FAIL_NULL(state->graphs);
	if (p_with_tick) {
		GodotProfileZone("Macrame: tick_frame graph");
		_frame_run(state->graphs->tick_frame, p_tree);
	} else {
		// No tick was captured this iteration: only the process shards.
		GodotProfileZone("Macrame: plain_frame graph");
		_frame_run(state->graphs->plain_frame, p_tree);
	}
}

#else

void MacrameScene::init() {}
void MacrameScene::finish_graphs() {}
void MacrameScene::finish() {}
bool MacrameScene::is_enabled() { return false; }
int MacrameScene::assign_shard() { return -1; }
thread_local int macrame_tls_current_shard = -1;
thread_local bool macrame_tls_in_shard_task = false;
void MacrameScene::check_write(const Node *) {}
void MacrameScene::check_read(const Node *) {}
void MacrameScene::check_main(const Node *) {}
void MacrameScene::run_groups(SceneTree *, void **, int, bool) {}
bool MacrameScene::frame_graph_enabled() { return false; }
void MacrameScene::set_frame_render_nodes(FrameNodeAdder) {}
bool MacrameScene::frame_graph_running() { return false; }
int MacrameScene::frame_graph_kind() { return 3; }
void MacrameScene::frame_set_graph_running(bool) {}
void MacrameScene::frame_set_capturing(bool) {}
void MacrameScene::frame_set_tick(double) {}
bool MacrameScene::frame_take_queries_flushed() { return false; }
void MacrameScene::frame_execute_tick(SceneTree *) {}
void MacrameScene::frame_execute(SceneTree *, bool) {}

#endif // MACRAME_ENABLED
