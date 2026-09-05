/**************************************************************************/
/*  macrame_render_grant.h                                                */
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

namespace ts {
template <typename T>
class Guarded;
}

// Phase 1 of the Macrame conversion: the render server's state is one guarded object and the
// frame's draw runs as a write task on a worker. The wrapper macros in
// `rendering_server_default.h` need a cheap "am I running under that grant?" test to choose
// between the direct call and staging; this is it. The flag is set by the body that holds
// the grant and read through out-of-line accessors, per Macrame's thread-local guidance
// (a compiler may cache a thread-local's address across a suspension; these bodies never
// suspend, but the discipline costs nothing).

struct RenderGrantToken {
	// The renderer's guarded identity: RenderingDevice's thread guards check the calling task's
	// grants against this object (`MacrameRender::check_access`).
	int unused = 0;
};

extern thread_local bool macrame_tls_holds_render_grant;
extern thread_local bool macrame_tls_holds_render_device_grant;
extern thread_local bool macrame_tls_holds_record_grant;
extern thread_local bool macrame_tls_holds_physics_grant;

namespace MacrameRender {
inline bool holds_grant() {
	return macrame_tls_holds_render_grant;
}
void set_holds_grant(bool p_holds);

// The harness behind RenderingDevice's thread guards. True when the caller holds the render
// grant (the draw body) or may touch the renderer directly (a blue thread with no draw in
// flight, as the server's query reports); otherwise Macrame checks the running task's grants
// against the renderer's token and faults with its own diagnostics.
//
// This is the *recording* side only. The device task holds a different grant on a different
// object (`RenderingDeviceSubmit`) and must not pass these guards: the whole point of the split
// is that the harness can tell a recording task touching submission state from a submitting task
// touching recording state. See `MacrameRenderDevice::check_access`.
void set_access_query(bool (*p_query)());
void set_token(RenderGrantToken *p_token);
bool check_access();

// For a body that already holds the render grant by inheritance rather than by declaring it: a
// `ts::parallel_for` chunk launched from inside the draw task runs under the calling task's grants,
// so `ts::access_check` on the render token passes for the chunk and every RenderingDevice guard
// would let it through on the harness path. What the chunk does not have is the thread-local mirror
// `holds_grant()` reads, which is the fast path those guards take first - and one chunk crosses
// ~16k of them.
//
// This scope sets the mirror for the life of the chunk and restores it on the way out. It is not a
// correctness device and it grants nothing: it exists only because Macrame has no non-faulting
// "do I hold this grant?" query, so an inheriting body cannot compute the mirror for itself. That
// is an API gap; given such a query this type collapses into it.
struct Inherited_grant_scope {
	const bool previous;
	const bool previous_record;
	Inherited_grant_scope();
	~Inherited_grant_scope();
	Inherited_grant_scope(const Inherited_grant_scope &) = delete;
	Inherited_grant_scope &operator=(const Inherited_grant_scope &) = delete;
};
} // namespace MacrameRender

// The recording grant: the device's recording state - the frame graph being recorded into, its
// draw and compute lists, the uniform sets, pipelines and per-frame buffers a frame creates - as a
// guarded object of its own, split off the render grant. The three-node render pipeline needs it:
// the scene update writes the render server, the cull reads it, and only the record node may
// record into the device, which is what this token says. `RenderingDeviceGraph`'s owner check
// asks for it on the recording side; the render grant no longer opens the graph.
struct RecordGrantToken {
	int unused = 0;
};

namespace MacrameRecord {
inline bool holds_grant() {
	return macrame_tls_holds_record_grant;
}
void set_holds_grant(bool p_holds);
void set_token(RecordGrantToken *p_token);
// True when the caller holds the recording grant (the record node, or a body that holds both
// grants: the single render node, the synchronous draw) or when nothing is registered yet (device
// initialization, shutdown); otherwise the harness checks the running task's grants against the
// token and faults with its own diagnostics.
bool check_access();
} // namespace MacrameRecord

// The split draw's second guarded object is `RenderingDeviceSubmit` itself: the frame slot being
// submitted, the graph being replayed, the queue and the fence bookkeeping. It is the payload of
// the device `Guarded`, not a token, so its own methods carry `TS_CHECK_ACCESS()`. This namespace
// is the hook the parts that cannot include it (the command graph's owner check) call instead.
namespace MacrameRenderDevice {
inline bool holds_grant() {
	return macrame_tls_holds_render_device_grant;
}
void set_holds_grant(bool p_holds);

// Registered by `RenderingDevice::initialize` once a submit object exists; the checker faults
// (naming `RenderingDeviceSubmit` and the mode) unless the running task declared the device grant
// on *that* object. The instance is a parameter because a local device has its own submit object
// and its own guarded queue. Before registration, and in builds without a device, this is a no-op.
void set_access_checker(void (*p_check)(const void *));
void check_access(const void *p_object);
} // namespace MacrameRenderDevice

struct PhysicsGrantToken {
	int unused = 0;
};

namespace MacramePhysics {
inline bool holds_grant() {
	return macrame_tls_holds_physics_grant;
}
void set_holds_grant(bool p_holds);
// The physics wrapper registers its guarded space here so scene shards can declare a read on it.
void set_guarded(ts::Guarded<PhysicsGrantToken> *p_guarded);
ts::Guarded<PhysicsGrantToken> *get_guarded();
} // namespace MacramePhysics
