/**************************************************************************/
/*  macrame_render_grant.cpp                                              */
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

#include "macrame_render_grant.h"

#ifdef MACRAME_ENABLED
#include "ts/access.h"
#include "ts/scheduler.h"
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define MACRAME_NO_INLINE __declspec(noinline)
#else
#define MACRAME_NO_INLINE [[gnu::noinline]]
#endif

thread_local bool macrame_tls_holds_render_grant = false;
thread_local bool macrame_tls_holds_render_device_grant = false;
thread_local bool macrame_tls_holds_record_grant = false;
thread_local bool macrame_tls_holds_physics_grant = false;

MACRAME_NO_INLINE void MacramePhysics::set_holds_grant(bool p_holds) {
	macrame_tls_holds_physics_grant = p_holds;
}

namespace {
ts::Guarded<PhysicsGrantToken> *physics_guarded = nullptr;
} // namespace

void MacramePhysics::set_guarded(ts::Guarded<PhysicsGrantToken> *p_guarded) {
	physics_guarded = p_guarded;
}

ts::Guarded<PhysicsGrantToken> *MacramePhysics::get_guarded() {
	return physics_guarded;
}

MACRAME_NO_INLINE void MacrameRender::set_holds_grant(bool p_holds) {
	macrame_tls_holds_render_grant = p_holds;
}

MACRAME_NO_INLINE void MacrameRenderDevice::set_holds_grant(bool p_holds) {
	macrame_tls_holds_render_device_grant = p_holds;
}

MACRAME_NO_INLINE void MacrameRecord::set_holds_grant(bool p_holds) {
	macrame_tls_holds_record_grant = p_holds;
}

MacrameRender::Inherited_grant_scope::Inherited_grant_scope() :
		previous(holds_grant()), previous_record(MacrameRecord::holds_grant()) {
	// A chunk inherits every grant of the body that launched it, so both mirrors follow.
	set_holds_grant(true);
	MacrameRecord::set_holds_grant(true);
}

MacrameRender::Inherited_grant_scope::~Inherited_grant_scope() {
	set_holds_grant(previous);
	MacrameRecord::set_holds_grant(previous_record);
}

namespace {
bool (*render_access_query)() = nullptr;
RenderGrantToken *render_token = nullptr;
RecordGrantToken *record_token = nullptr;
void (*device_access_check)(const void *) = nullptr;
} // namespace

void MacrameRenderDevice::set_access_checker(void (*p_check)(const void *)) {
	device_access_check = p_check;
}

void MacrameRenderDevice::check_access(const void *p_object) {
	if (device_access_check && p_object) {
		device_access_check(p_object);
	}
}

void MacrameRender::set_access_query(bool (*p_query)()) {
	render_access_query = p_query;
}

void MacrameRender::set_token(RenderGrantToken *p_token) {
	render_token = p_token;
}

void MacrameRecord::set_token(RecordGrantToken *p_token) {
	record_token = p_token;
}

bool MacrameRecord::check_access() {
	if (macrame_tls_holds_record_grant) {
		return true;
	}
	if (!record_token) {
		return true; // Before the render server registers, and after it has gone: direct.
	}
#ifdef MACRAME_ENABLED
	// Direct mode is a blue thread with nothing in flight - never a worker: the render server's
	// query answers "yes" for the body holding the render grant too, and that body is exactly the
	// one this check exists to refuse.
	if (ts::current_worker_index() < 0 && render_access_query && render_access_query()) {
		return true;
	}
	ts::access_check(record_token); // Fatal under TS_SAFETY_CHECKS unless the running task declared the grant.
#else
	if (render_access_query && render_access_query()) {
		return true;
	}
#endif
	return true;
}

bool MacrameRender::check_access() {
	// The render grant or the recording grant. The device task holds a grant on
	// `RenderingDeviceSubmit` instead and is refused here, which is what makes the split visible
	// to the harness rather than a matter of review. The recording grant passes because the
	// record node reaches the device through the same entry points (`buffer_update`, draw lists,
	// resource creation) as the render server always did; what it may *not* do is write the
	// scene, and that has no entry point to guard.
	if (macrame_tls_holds_render_grant || macrame_tls_holds_record_grant) {
		return true;
	}
	if (!render_access_query) {
		// Before the render server registers (RenderingDevice::initialize runs first, on the main
		// thread) there is no draw to conflict with: a blue thread may call directly.
		return true;
	}
	if (render_access_query()) {
		return true; // Direct mode: a blue thread, nothing in flight.
	}
#ifdef MACRAME_ENABLED
	if (render_token) {
		ts::access_check(render_token); // Fatal under TS_SAFETY_CHECKS unless the running task declared the grant.
		return true;
	}
#endif
	return false;
}
