/**************************************************************************/
/*  macrame_canvas_stamp.h                                                */
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

#include "core/macrame/macrame_run_parity.h"

// The 2D canvas (canvases, items and their command lists, lights, occluders) is not yet split
// between the scene update and the record node the way the 3D scene is: the journal mutates it in
// the scene update while the record node draws it, in the same run, in the lagged shape. Until the
// canvas journal is the record node's (results 2.18), the overlap is made deterministic instead
// of silent: a mutation from the scene update and a 2D draw by the record node in one run fault
// at the blue thread's boundary check, naming the run. Static 2D (built before the graph runs, or
// changed only between runs) is fine. The stamps cover the item command lists, the common item
// setters, lights, occluders and frees; see the call sites.
struct MacrameCanvasStamp {
	static inline uint64_t written_run = UINT64_MAX;
	static inline uint64_t drawn_run = UINT64_MAX;
	static void note_write() {
		if (MacrameRunParity::on_update_node()) {
			written_run = MacrameRunParity::current();
		}
	}
	static void note_draw() {
		if (MacrameRunParity::on_record_node()) {
			drawn_run = MacrameRunParity::current();
		}
	}
	static void check_boundary() {
		CRASH_COND_MSG(written_run == MacrameRunParity::run && drawn_run == MacrameRunParity::run, "Macrame: 2D content was changed by the scene update in run " + itos(MacrameRunParity::run) + " while the record node drew it (the canvas is not yet owned by one node: results 2.18). Change 2D only between runs, or run with MACRAME_RENDER_SPLIT=0.");
	}
};

#define MACRAME_CANVAS_WRITE() MacrameCanvasStamp::note_write()

#else

#define MACRAME_CANVAS_WRITE() ((void)0)

#endif // MACRAME_ENABLED
