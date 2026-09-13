/**************************************************************************/
/*  macrame_scene.h                                                       */
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

// Phase 2b of the Macrame conversion: scene shards.
//
// The scene tree is cut into a fixed number of shards. Every process group that a node
// opts into with `process_thread_group = SUB_THREAD` is assigned to one shard (round
// robin); everything else belongs to the main shard, which only the blue thread runs.
// A shard is one `Guarded` object. Processing a shard is one task with a write grant on
// that shard and read grants on the main shard and on the physics space, so shards run
// in parallel with no locks: by declaration they cannot touch each other's nodes.
//
// The harness sits at the node guard macros (`scene/main/node.h`): inside a shard task a
// node write checks the write grant on the node's shard, a node read checks a read grant,
// and a main-thread-only method checks a write grant on the main shard, which a shard
// never holds. Outside shard tasks (the blue thread, single-threaded by construction)
// the checks are skipped. Cross-shard writes go through the existing deferred paths
// (`call_deferred_thread_group`, `set_deferred_thread_group`); cross-shard reads fault.

class Node;

extern thread_local int macrame_tls_current_shard;
extern thread_local bool macrame_tls_in_shard_task;
class SceneTree;

struct SceneShardToken {
	int index = -1; // -1 is the main shard.
};

namespace MacrameScene {
constexpr int SHARD_COUNT = 64; // Fixed. MACRAME_SHARDS caps how many receive groups. Measured at 500 NPCs on a 22-thread laptop: 16 and 32 within noise, 64 worse (per-call cost grows with concurrency).

void init();
// Destroys the compiled frame and phase graphs (writing their traces first). Must run while
// every guarded object they name is still alive: `Main::cleanup` destroys the physics server
// and the renderer long before `MacrameRuntime::finish`, and a `Static_task_graph` outliving
// one of its guarded objects is fatal. Called at the end of the main loop; idempotent.
void finish_graphs();
void finish();
bool is_enabled();

// Round-robin shard assignment for a new sub-thread process group.
int assign_shard();

// Thread-local task context (out-of-line accessors, see macrame_render_grant.h).
inline int current_shard() {
	return macrame_tls_current_shard; // -1 when not inside a shard task.
}
inline bool in_shard_task() {
	return macrame_tls_in_shard_task;
}

// Harness entry points used by the node guard macros.
void check_write(const Node *p_node);
void check_read(const Node *p_node);
void check_main(const Node *p_node);

// Run every group in `p_groups` (a null-terminated array is not used; count given) as
// shard tasks and join them. Called from SceneTree::_process for a threaded batch.
void run_groups(SceneTree *p_tree, void **p_groups, int p_group_count, bool p_physics);

// Frame graphs: the tick's shard fan-out, the physics step, the navigation update and the
// frame's process fan-out as compiled `Static_task_graph`s. Godot's iteration carries 0, 1 or
// (catch-up) N physics ticks, so there is one compiled graph per frame shape, sharing the same
// node bodies and the same guarded objects - no node ever runs as a no-op:
//
//   tick_frame  64 tick shards + physics step + navigation + 64 frame shards (edge i -> i)
//                 + gather + body sync + render + submit
//   plain_frame 64 frame shards (an iteration with no tick) + render + submit
//   tick_only   64 tick shards + physics step + navigation + gather + body sync (each catch-up tick)
//
// An iteration with N ticks runs `tick_only` N-1 times inside the physics loop, then
// `tick_frame` after the process phase; an iteration with no tick runs `plain_frame`.
// Per-shard edges (tick shard i -> frame shard i) let a shard's process phase start as soon as
// its own physics phase is done, while the step and navigation run in the tick's tail. While
// capturing, `run_groups()` records its batch for the graph instead of running it; the
// blue-thread parts of the tree's phases run as before. MACRAME_FRAME_GRAPH=0 disables.
//
// Two nodes hold what used to be the next tick's serial head, where the blue thread ran it with the
// graph stopped (0.88 ms of every tick frame, results 2.25.4). Both consume the tick's results, so
// they run at the tail of the tick that produced them:
// - `gather` runs the gather groups: a sub-thread process group whose owner set the `macrame_gather`
//   meta before choosing its thread group reads every shard and writes the main shard (the town
//   publishing its NPCs' state for the next tick's shards). It runs a gather group's
//   `_physics_process` after every shard node of its run, so in `tick_frame` it sees the frame's
//   process phase too, and runs empty in a scene without gather groups. A gather group's
//   `_process` runs on the blue thread after the run, and outside the frame graph the blue thread
//   runs gather groups once the batch is joined, so `plain_frame` stays the graph it was: an empty
//   node there cost plain frames ~0.1 ms.
// - `body sync` delivers the step's state callbacks (`PhysicsServer3D::flush_queries`: every moved
//   rigid body told its new transform) right after the step, and after every frame shard, so
//   `_process` still sees the previous tick's transforms, as in stock Godot. The renderer sees them
//   a tick earlier. With physics interpolation on it leaves them to the head, which must snapshot
//   the transforms first.
//
// Both write the main shard, and every shard node reads it, so no shard node runs beside either.
// That makes the main shard's write grant the "every shard" grant a node could not otherwise
// declare (at most eight objects a node, and there are 64 shards): the harness accepts it for a
// read of any shard's node (`gather`) or also a write (`body sync`).
bool frame_graph_enabled();
void frame_set_capturing(bool p_capturing);
void frame_set_tick(double p_step); // The step this tick's `physics step` and `navigation` nodes use.
void frame_execute_tick(SceneTree *p_tree); // A catch-up tick: the `tick_only` graph, inside the physics loop.
void frame_execute(SceneTree *p_tree, bool p_with_tick); // `tick_frame` (a tick was captured) or `plain_frame`.

// The renderer's two nodes - `render` (writes the render server's guarded state) and `submit`
// (writes the device's) - are added to every graph that has a frame phase, by a callback the
// rendering server registers at construction. The parameter is a `ts::Static_task_graph *`,
// erased because this header is included by `scene/main/node.h` and must stay free of the task
// system's includes; the callback lives in `rendering_server_default.cpp`, which owns both
// guarded objects and both bodies.
using FrameNodeAdder = void (*)(void *p_graph);
void set_frame_render_nodes(FrameNodeAdder p_adder);

// Whether the frame graph is actually running this iteration's middle. `Main::iteration` decides
// (it also needs a scene tree) and sets it; the renderer reads it to choose between "post the
// frame for the graph's render node" and "draw synchronously on this thread", which is what the
// boot-time draws before the first iteration are. Not the same as `frame_graph_enabled()`.
bool frame_graph_running();
void frame_set_graph_running(bool p_running);
// Which graph the current run belongs to: 0 plain_frame, 1 tick_frame, 2 tick_only, 3 none. Set by
// the blue thread before `execute()`, read by node bodies (the render phase probe).
int frame_graph_kind();
// Whether the last run's `body sync` delivered the step's state callbacks, so the next tick's head
// must not (`Main::iteration`). Clears the flag.
bool frame_take_queries_flushed();
} // namespace MacrameScene
