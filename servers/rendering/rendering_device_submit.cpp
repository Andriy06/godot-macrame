/**************************************************************************/
/*  rendering_device_submit.cpp                                           */
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

#include "rendering_device_submit.h"

#include "core/macrame/macrame_phase_probe.h"

#include "core/profiling/profiling.h"
#include "servers/rendering/rendering_device.h"

#ifdef MACRAME_ENABLED
#include "core/macrame/macrame_render_grant.h"

namespace {
void macrame_device_access_check(const void *p_object) {
	// Fatal under TS_SAFETY_CHECKS unless the running task declared a write grant on this very
	// submit object; the message names `RenderingDeviceSubmit` and the mode it wanted.
	ts::access_check(const_cast<RenderingDeviceSubmit *>(static_cast<const RenderingDeviceSubmit *>(p_object)));
}
} // namespace

void macrame_register_device_submit_checker() {
	MacrameRenderDevice::set_access_checker(&macrame_device_access_check);
}
#endif // MACRAME_ENABLED

void RenderingDeviceSubmit::configure(RenderingDevice *p_device, RDD *p_driver, RDD::CommandQueueID p_main_queue, RDD::CommandQueueID p_present_queue) {
	TS_CHECK_ACCESS();
	device = p_device;
	driver = p_driver;
	main_queue = p_main_queue;
	present_queue = p_present_queue;
}

bool RenderingDeviceSubmit::is_configured() const {
	TS_CHECK_ACCESS();
	return driver != nullptr && !slots.is_empty();
}

bool RenderingDeviceSubmit::create_slots(uint32_t p_slot_count, const LocalVector<RDD::CommandPoolID> &p_command_pools, uint32_t p_transfer_worker_count) {
	TS_CHECK_ACCESS();
	slots.resize(p_slot_count);
	for (uint32_t i = 0; i < p_slot_count; i++) {
		Slot &slot = slots[i];
		slot.command_buffer_pool.pool = p_command_pools[i];
		slot.semaphore = driver->semaphore_create();
		if (!slot.semaphore) {
			return false;
		}
		slot.transfer_worker_semaphores.resize(p_transfer_worker_count);
		for (uint32_t j = 0; j < p_transfer_worker_count; j++) {
			slot.transfer_worker_semaphores[j] = driver->semaphore_create();
			if (!slot.transfer_worker_semaphores[j]) {
				return false;
			}
		}
	}
	return true;
}

void RenderingDeviceSubmit::destroy_slots() {
	TS_CHECK_ACCESS();
	for (uint32_t i = 0; i < slots.size(); i++) {
		Slot &slot = slots[i];
		if (slot.semaphore) {
			driver->semaphore_free(slot.semaphore);
			slot.semaphore = RDD::SemaphoreID();
		}
		for (uint32_t j = 0; j < slot.command_buffer_pool.buffers.size(); j++) {
			driver->semaphore_free(slot.command_buffer_pool.semaphores[j]);
		}
		slot.command_buffer_pool.buffers.clear();
		slot.command_buffer_pool.semaphores.clear();
		for (uint32_t j = 0; j < slot.transfer_worker_semaphores.size(); j++) {
			if (slot.transfer_worker_semaphores[j]) {
				driver->semaphore_free(slot.transfer_worker_semaphores[j]);
			}
		}
		slot.transfer_worker_semaphores.clear();
	}
	slots.clear();
}

void RenderingDeviceSubmit::submit(Staged &p_staged) {
	TS_CHECK_ACCESS();
	{
		GodotProfileZone("_end_frame");
		_end_frame(p_staged);
	}
	{
		GodotProfileZone("_execute_frame");
		_execute_frame(p_staged);
	}
	// Done with the graph. It is now closed rather than handed back: nothing may touch it until
	// the recording side reopens it with `begin`, which is the invariant the hand-off point exists
	// to keep.
	p_staged.graph->macrame_close_after_submit();
}

void RenderingDeviceSubmit::_end_frame(Staged &p_staged) {
	TS_CHECK_ACCESS();
	// The command buffer must be copied into a stack variable as the driver workarounds can change the command buffer in use.
	RDD::CommandBufferID command_buffer = p_staged.command_buffer;
	Slot &slot = slots[p_staged.slot];

	wait_semaphores.clear();
	GodotProfileZoneGroupedFirst(_profile_zone, "_submit_transfer_workers");
	device->_submit_transfer_workers(&slot.transfer_worker_semaphores, command_buffer, &wait_semaphores);
	GodotProfileZoneGrouped(_profile_zone, "_submit_transfer_barriers");
	device->_submit_transfer_barriers(command_buffer);

	GodotProfileZoneGrouped(_profile_zone, "draw_graph->end");
	p_staged.graph->end(p_staged.reorder_commands, p_staged.full_barriers, command_buffer, slot.command_buffer_pool);
	MACRAME_PHASE("submit: graph compile");
	GodotProfileZoneGrouped(_profile_zone, "driver->command_buffer_end");
	driver->command_buffer_end(command_buffer);
	GodotProfileZoneGrouped(_profile_zone, "driver->end_segment");
	driver->end_segment();
}

void RenderingDeviceSubmit::_execute_chained_cmds(Staged &p_staged, bool p_present_swap_chain, RDD::FenceID p_draw_fence,
		RDD::SemaphoreID p_dst_draw_semaphore_to_signal) {
	TS_CHECK_ACCESS();
	// Execute command buffers and use semaphores to wait on the execution of the previous one.
	// Normally there's only one command buffer, but driver workarounds can force situations where
	// there'll be more.
	uint32_t command_buffer_count = 1;
	RDG::CommandBufferPool &buffer_pool = slots[p_staged.slot].command_buffer_pool;
	if (buffer_pool.buffers_used > 0) {
		command_buffer_count += buffer_pool.buffers_used;
		buffer_pool.buffers_used = 0;
	}

	chained_swap_chains.clear();

	// Instead of having just one command; we have potentially many (which had to be split due to an
	// Adreno workaround on mobile, only if the workaround is active). Thus we must execute all of them
	// and chain them together via semaphores as dependent executions.
	chained_wait_semaphores = wait_semaphores;

	for (uint32_t i = 0; i < command_buffer_count; i++) {
		RDD::CommandBufferID command_buffer;
		RDD::SemaphoreID signal_semaphore;
		RDD::FenceID signal_fence;
		if (i > 0) {
			command_buffer = buffer_pool.buffers[i - 1];
		} else {
			command_buffer = p_staged.command_buffer;
		}

		if (i == (command_buffer_count - 1)) {
			// This is the last command buffer, it should signal the semaphore & fence.
			signal_semaphore = p_dst_draw_semaphore_to_signal;
			signal_fence = p_draw_fence;

			if (p_present_swap_chain) {
				// Just present the swap chains as part of the last command execution.
				chained_swap_chains = p_staged.swap_chains_to_present;
			}
		} else {
			signal_semaphore = buffer_pool.semaphores[i];
			// Semaphores always need to be signaled if it's not the last command buffer.
		}

		driver->command_queue_execute_and_present(main_queue, chained_wait_semaphores, command_buffer,
				signal_semaphore ? signal_semaphore : VectorView<RDD::SemaphoreID>(), signal_fence,
				chained_swap_chains);

		// Make the next command buffer wait on the semaphore signaled by this one.
		chained_wait_semaphores.resize(1);
		chained_wait_semaphores[0] = signal_semaphore;
	}

	wait_semaphores.clear();
}

void RenderingDeviceSubmit::_execute_frame(Staged &p_staged) {
	TS_CHECK_ACCESS();
	// Check whether this frame should present the swap chains and in which queue.
	const bool frame_can_present = p_staged.present && !p_staged.swap_chains_to_present.is_empty();
	const bool separate_present_queue = main_queue != present_queue;

	// The semaphore is required if the frame can be presented and a separate present queue is used;
	// since the separate queue will wait for that semaphore before presenting.
	const RDD::SemaphoreID semaphore = (frame_can_present && separate_present_queue)
			? slots[p_staged.slot].semaphore
			: RDD::SemaphoreID(nullptr);
	const bool present_swap_chain = frame_can_present && !separate_present_queue;

	_execute_chained_cmds(p_staged, present_swap_chain, p_staged.fence, semaphore);
	MACRAME_PHASE("submit: queue submit");

	// Indicate the fence has been signaled so the next time the frame's contents need to be used,
	// the CPU waits for the work to complete.
	if (p_staged.fence_signaled != nullptr) {
		p_staged.fence_signaled->store(true, std::memory_order_release);
	}

	if (frame_can_present) {
		if (separate_present_queue) {
			// Issue the presentation separately if the presentation queue is different from the main queue.
			driver->command_queue_execute_and_present(present_queue, slots[p_staged.slot].semaphore, {}, {}, {}, p_staged.swap_chains_to_present);
			MACRAME_PHASE("submit: present");
		}
	}

	// The frame's acquisitions have been consumed; the recording side refills the list for the
	// slot's next frame.
	p_staged.swap_chains_to_present.clear();
}
