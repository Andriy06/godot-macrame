/**************************************************************************/
/*  macrame_phase_probe.h                                                 */
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

// A flat, sequential phase timer for the render node, unsampled: every `mark(name)` charges the
// wall-clock time since the previous mark to `name`. Exactly one node body runs it at a time (the
// render node), so the state is a plain global. Enabled by MACRAME_RENDER_PHASES=1; the report
// (mean and max per phase, per graph kind) is printed every 1000 frames of a kind.

#ifdef MACRAME_ENABLED

class MacramePhaseProbe {
public:
	static bool enabled();
	// `p_kind`: 0 plain frame, 1 tick frame, 2 tick-only, 3 synchronous (no graph).
	static void frame_begin(int p_kind);
	static void mark(const char *p_name); // The end of phase `p_name`.
	static void frame_end();
};

#define MACRAME_PHASE(m_name) MacramePhaseProbe::mark(m_name)

#else

#define MACRAME_PHASE(m_name) ((void)0)

#endif // MACRAME_ENABLED
