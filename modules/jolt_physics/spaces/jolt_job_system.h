/**************************************************************************/
/*  jolt_job_system.h                                                     */
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

#include "core/os/spin_lock.h"
#include "core/templates/hash_map.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Semaphore.h>

#ifdef MACRAME_ENABLED
#include "ts/parallel_for.h"
#endif

#include <atomic>

class JoltJobSystem final : public JPH::JobSystemWithBarrier {
	class Job : public JPH::JobSystem::Job {
		friend class JoltJobSystem;

		inline static std::atomic<Job *> completed_head = nullptr;

#ifdef DEBUG_ENABLED
		const char *name = nullptr;
#endif

		int64_t task_id = -1;

		std::atomic<Job *> completed_next = nullptr;

		static void _execute(void *p_user_data);

	public:
		Job(const char *p_name, JPH::ColorArg p_color, JPH::JobSystem *p_job_system, const JPH::JobSystem::JobFunction &p_job_function, JPH::uint32 p_dependency_count);
		Job(const Job &p_other) = delete;
		Job(Job &&p_other) = delete;
		~Job();

		static void push_completed(Job *p_job);
		static Job *pop_completed();

		void queue();

		Job &operator=(const Job &p_other) = delete;
		Job &operator=(Job &&p_other) = delete;
	};

#ifdef DEBUG_ENABLED
	// We use `const void*` here to avoid the cost of hashing the actual string, since the job names
	// are always literals and as such will point to the same address every time.
	inline static HashMap<const void *, uint64_t> timings_by_job;

	// TODO: Check whether the usage of SpinLock is justified or if this should be a mutex instead.
	inline static SpinLock timings_lock;
#endif

	JPH::FixedSizeFreeList<Job> jobs;

	int thread_count = 0;

	// Macrame: a census of what Jolt actually asks for per step. Jolt sizes its own splits by
	// `GetMaxConcurrency()`, so the job count is a function of the answer we give it - and the
	// answer was wrong for this build: the worker pool has no threads of its own, so every queued
	// job ran inline on the caller while Jolt was told it had 22 of them. MACRAME_JOLT_STATS=<n>
	// prints the census every n steps; MACRAME_JOLT_CONCURRENCY overrides the answer.
	struct Census {
		uint64_t steps = 0;
		uint64_t jobs_created = 0;
		uint64_t queued_at_create = 0;
		uint64_t batches = 0;
		uint64_t batched_jobs = 0;
		HashMap<const void *, uint64_t> by_name;
		HashMap<uint32_t, uint64_t> batch_hist;
		void report(int p_concurrency);
	};
	inline static Census census;
	inline static int stats_every = -1;

	virtual int GetMaxConcurrency() const override;

	virtual JPH::JobHandle CreateJob(const char *p_name, JPH::ColorArg p_color, const JPH::JobSystem::JobFunction &p_job_function, JPH::uint32 p_dependency_count = 0) override;
	virtual void QueueJob(JPH::JobSystem::Job *p_job) override;
	virtual void QueueJobs(JPH::JobSystem::Job **p_jobs, JPH::uint p_job_count) override;
	virtual void FreeJob(JPH::JobSystem::Job *p_job) override;

	void _reclaim_jobs();

#ifdef MACRAME_ENABLED
	// --- The execution backend ------------------------------------------------------------
	//
	// Jolt owns its own parallelism and its own safety; this supplies the one thing it was
	// missing here, workers to run on. Before this, `queue()` handed every job to
	// `WorkerThreadPool::add_native_task`, whose no-threads fallback runs the callable inline on
	// the caller - so Jolt built a full job graph, was told it had 22 executors, and then
	// executed all ~90 jobs of a step serially on one thread.
	//
	// The shape is Jolt's own `JobSystemThreadPool`, with Macrame worker lanes where that class
	// has OS threads: a ring of ready jobs, a semaphore, and lanes that drain it. The lanes live
	// exactly as long as one `PhysicsSystem::Update` and are opened by a single
	// `ts::parallel_for`, so the space's write grant flows into every job and the harness still
	// attributes every body write to the physics step node.
	//
	// The first lane to run claims the driver role and calls `Update` itself; the rest drain the
	// queue. Claiming rather than assigning by index is what makes this deadlock-free: a helper
	// lane can only exist once a driver is already running, so a lane can never park on the
	// semaphore waiting for a driver that the scheduler has not started yet. If no helper ever
	// gets a worker, the driver still completes on its own - Jolt's barrier executes ready jobs
	// on the waiting thread.
	static constexpr uint32_t QUEUE_LENGTH = 1024; // A step queues ~90; the ring never wraps.

	std::atomic<Job *> queue_slots[QUEUE_LENGTH] = {};
	std::atomic<uint32_t> queue_tail = 0;
	std::atomic<bool> lanes_quit = false;
	std::atomic<bool> driver_claimed = false;
	bool lanes_open = false;
	JPH::Semaphore lane_semaphore;

	void _push_ready(Job *p_job);
	void _drain_ready();
	void _run_lane();
#endif

	// How many workers a step may use. MACRAME_JOLT_LANES=0/1 restores the pre-conversion shape
	// (every job inline on the caller); the default is set by measurement in results 2.24.
	static int lane_count();

public:
	JoltJobSystem();

	void pre_step();
	void post_step();

#ifdef MACRAME_ENABLED
	// Runs `p_body` (the `PhysicsSystem::Update` call) with lanes open. Everything Jolt queues
	// while it runs is executed on Macrame workers; nothing outlives the call. The chunks are
	// `ts::parallel_for` chunks, so they inherit the step node's grants - which is what keeps our
	// own re-entrant check sites quiet: Jolt's contact and activation listeners call back into
	// Godot from inside these jobs, and a worker without the step's grant would fault on entirely
	// legitimate work.
	template <typename TBody>
	void run_with_lanes(TBody &&p_body) {
		const int lanes = lane_count();
		if (lanes <= 1 || lanes_open) {
			p_body(); // Disabled, or already inside a step.
			return;
		}
		queue_tail.store(0, std::memory_order_relaxed);
		lanes_quit.store(false, std::memory_order_relaxed);
		driver_claimed.store(false, std::memory_order_relaxed);
		// Drain permits left over from the previous step. A step releases one per queued job (~90)
		// but a lane's drain runs many jobs per wake, so most permits are never consumed; carried
		// into the next step they would turn the lane loop into a busy wait, which is precisely the
		// power trap that sank the parallel command replay (2.21). Safe here: no lane is running.
		const int leftover = lane_semaphore.GetValue();
		if (leftover > 0) {
			lane_semaphore.Acquire((JPH::uint)leftover);
		}
		lanes_open = true;
		ts::parallel_for(
				lanes,
				[this, &p_body](int) {
					if (!driver_claimed.exchange(true, std::memory_order_acq_rel)) {
						p_body();
						// Wake every lane that may be parked, then let them see the flag.
						lanes_quit.store(true, std::memory_order_release);
						lane_semaphore.Release((JPH::uint)lane_count());
					} else {
						_run_lane();
					}
				},
				ts::Parallel_options{ .max_workers = lanes, .balance = ts::Balance::unbalanced });
		lanes_open = false;
		_drain_ready(); // Anything a late release left behind; normally a no-op.
	}
#endif

#ifdef DEBUG_ENABLED
	void flush_timings();
#endif
};
