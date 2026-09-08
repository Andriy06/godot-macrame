/**************************************************************************/
/*  jolt_job_system.cpp                                                   */
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

#include "jolt_job_system.h"

#include "core/debugger/engine_debugger.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "servers/physics_3d/physics_server_3d_manager.h"

#include <Jolt/Physics/PhysicsSettings.h>

#ifdef MACRAME_ENABLED
#include "ts/parallel_for.h"
#endif

#include <cstdlib>

void JoltJobSystem::Job::_execute(void *p_user_data) {
	Job *job = static_cast<Job *>(p_user_data);

#ifdef DEBUG_ENABLED
	const uint64_t time_start = Time::get_singleton()->get_ticks_usec();
#endif

	job->Execute();

#ifdef DEBUG_ENABLED
	const uint64_t time_end = Time::get_singleton()->get_ticks_usec();
	const uint64_t time_elapsed = time_end - time_start;

	timings_lock.lock();
	timings_by_job[job->name] += time_elapsed;
	timings_lock.unlock();
#endif

	job->Release();
}

JoltJobSystem::Job::Job(const char *p_name, JPH::ColorArg p_color, JPH::JobSystem *p_job_system, const JPH::JobSystem::JobFunction &p_job_function, JPH::uint32 p_dependency_count) :
		JPH::JobSystem::Job(p_name, p_color, p_job_system, p_job_function, p_dependency_count)
#ifdef DEBUG_ENABLED
		,
		name(p_name)
#endif
{
}

JoltJobSystem::Job::~Job() {
	if (task_id != -1) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
	}
}

void JoltJobSystem::Job::push_completed(Job *p_job) {
	Job *prev_head = nullptr;

	do {
		prev_head = completed_head.load(std::memory_order_relaxed);
		p_job->completed_next = prev_head;
	} while (!completed_head.compare_exchange_weak(prev_head, p_job, std::memory_order_release, std::memory_order_relaxed));
}

JoltJobSystem::Job *JoltJobSystem::Job::pop_completed() {
	Job *prev_head = nullptr;

	do {
		prev_head = completed_head.load(std::memory_order_relaxed);
		if (prev_head == nullptr) {
			return nullptr;
		}
	} while (!completed_head.compare_exchange_weak(prev_head, prev_head->completed_next, std::memory_order_acquire, std::memory_order_relaxed));

	return prev_head;
}

void JoltJobSystem::Job::queue() {
	AddRef();

	// Ideally we would use Jolt's actual job name here, but I'd rather not incur the overhead of a memory allocation or
	// thread-safe lookup every time we create/queue a task. So instead we use the same cached description for all of them.
	static const String task_name(PhysicsServer3DManager::JOLT_PHYSICS_NAME);

	task_id = WorkerThreadPool::get_singleton()->add_native_task(&_execute, this, true, task_name);
}

#ifdef MACRAME_ENABLED

void JoltJobSystem::_push_ready(Job *p_job) {
	p_job->AddRef(); // Released by whichever lane executes it.
	const uint32_t slot = queue_tail.fetch_add(1, std::memory_order_relaxed);
	CRASH_COND_MSG(slot >= QUEUE_LENGTH, "Macrame: the Jolt ready ring overflowed in one step.");
	queue_slots[slot & (QUEUE_LENGTH - 1)].store(p_job, std::memory_order_release);
	lane_semaphore.Release();
}

// Take and run whatever is ready right now. Each slot is claimed by exchange, so a job runs once
// however many lanes walk the ring.
void JoltJobSystem::_drain_ready() {
	const uint32_t tail = queue_tail.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < tail && i < QUEUE_LENGTH; i++) {
		std::atomic<Job *> &slot = queue_slots[i];
		if (slot.load(std::memory_order_acquire) == nullptr) {
			continue;
		}
		Job *job = slot.exchange(nullptr, std::memory_order_acq_rel);
		if (job != nullptr) {
			job->Execute();
			job->Release();
		}
	}
}

void JoltJobSystem::_run_lane() {
	while (!lanes_quit.load(std::memory_order_acquire)) {
		lane_semaphore.Acquire();
		_drain_ready();
	}
	_drain_ready();
}

#endif // MACRAME_ENABLED

int JoltJobSystem::GetMaxConcurrency() const {
	// Macrame: Jolt sizes every split it makes by this number. Answering 22 while the worker pool
	// runs every queued job inline on the caller means Jolt builds 22-way splits and then executes
	// all 22 of them serially, paying the job overhead for width it never gets. The override lets
	// the honest answer be measured against the flattering one.
	static const int override_count = []() {
		const char *env = std::getenv("MACRAME_JOLT_CONCURRENCY");
		return env != nullptr ? atoi(env) : 0;
	}();
	if (override_count > 0) {
		return override_count;
	}
	const int lanes = lane_count();
	return lanes > 1 ? lanes : thread_count;
}

JPH::JobHandle JoltJobSystem::CreateJob(const char *p_name, JPH::ColorArg p_color, const JPH::JobSystem::JobFunction &p_job_function, JPH::uint32 p_dependency_count) {
	Job *job = nullptr;

	while (true) {
		JPH::uint32 job_index = jobs.ConstructObject(p_name, p_color, this, p_job_function, p_dependency_count);

		if (job_index != JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex) {
			job = &jobs.Get(job_index);
			break;
		}

		WARN_PRINT_ONCE("Jolt Physics job system exceeded the maximum number of jobs. This should not happen. Please report this. Waiting for jobs to become available...");

		OS::get_singleton()->delay_usec(100);

		_reclaim_jobs();
	}

	// This will increment the job's reference count, so must happen before we queue the job
	JPH::JobHandle job_handle(job);

	if (stats_every > 0) {
		census.jobs_created++;
		census.by_name[static_cast<const void *>(p_name)]++;
	}

	if (p_dependency_count == 0) {
		if (stats_every > 0) {
			census.queued_at_create++;
		}
		QueueJob(job);
	}

	return job_handle;
}

void JoltJobSystem::QueueJob(JPH::JobSystem::Job *p_job) {
#ifdef MACRAME_ENABLED
	if (lanes_open) {
		_push_ready(static_cast<Job *>(p_job));
		return;
	}
#endif
	static_cast<Job *>(p_job)->queue();
}

void JoltJobSystem::QueueJobs(JPH::JobSystem::Job **p_jobs, JPH::uint p_job_count) {
	// Jolt hands a whole wave of newly-ready jobs here at once (`JobHandle::sRemoveDependencies`
	// batches them): a step is five such waves, three of them 22 jobs wide. They go on the ready
	// ring for the lanes to take; Jolt decides what may run when, we only say where it runs.
	if (stats_every > 0) {
		census.batches++;
		census.batched_jobs += p_job_count;
		census.batch_hist[MIN(p_job_count, uint32_t(32))]++;
	}
	for (JPH::uint i = 0; i < p_job_count; ++i) {
		QueueJob(p_jobs[i]);
	}
}

int JoltJobSystem::lane_count() {
	// 0 or 1 keeps the old shape: every job inline on the calling thread. The default is chosen in
	// 2.24 by measurement - the step shares the tick frame with the frame shards, so taking every
	// worker is not automatically best (2.21's lesson: check the node you did not change).
	static const int lanes = []() {
		const char *env = std::getenv("MACRAME_JOLT_LANES");
		return env != nullptr ? atoi(env) : 0;
	}();
	return lanes;
}

void JoltJobSystem::FreeJob(JPH::JobSystem::Job *p_job) {
	Job::push_completed(static_cast<Job *>(p_job));
}

void JoltJobSystem::_reclaim_jobs() {
	while (Job *job = Job::pop_completed()) {
		jobs.DestructObject(job);
	}
}

JoltJobSystem::JoltJobSystem() :
		JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers),
		thread_count(MAX(1, WorkerThreadPool::get_singleton()->get_thread_count())) {
	jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
}

void JoltJobSystem::Census::report(int p_concurrency) {
	String line = vformat("MACRAME_JOLT_STATS steps=%d concurrency=%d jobs/step=%.1f queued_at_create/step=%.1f |",
			steps, p_concurrency, double(jobs_created) / double(steps), double(queued_at_create) / double(steps));
	for (const KeyValue<const void *, uint64_t> &E : by_name) {
		line += vformat(" %s=%.1f", static_cast<const char *>(E.key), double(E.value) / double(steps));
	}
	String hist;
	for (const KeyValue<uint32_t, uint64_t> &E : batch_hist) {
		hist += vformat(" %d:%.1f", E.key, double(E.value) / double(steps));
	}
	print_line(line);
	print_line(vformat("MACRAME_JOLT_STATS batches/step=%.1f batched_jobs/step=%.1f | batch sizes/step:%s",
			double(batches) / double(steps), double(batched_jobs) / double(steps), hist));
	steps = 0;
	jobs_created = 0;
	queued_at_create = 0;
	batches = 0;
	batched_jobs = 0;
	by_name.clear();
	batch_hist.clear();
}

void JoltJobSystem::pre_step() {
	if (stats_every < 0) {
		const char *env = std::getenv("MACRAME_JOLT_STATS");
		stats_every = env != nullptr ? atoi(env) : 0;
	}
}

void JoltJobSystem::post_step() {
	_reclaim_jobs();
	if (stats_every > 0) {
		census.steps++;
		if (census.steps >= uint64_t(stats_every)) {
			census.report(GetMaxConcurrency());
		}
	}
}

#ifdef DEBUG_ENABLED

void JoltJobSystem::flush_timings() {
	const StringName profiler_name = SNAME("servers");

	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();

	if (engine_debugger->is_profiling(profiler_name)) {
		Array timings;

		for (const KeyValue<const void *, uint64_t> &E : timings_by_job) {
			timings.push_back(static_cast<const char *>(E.key));
			timings.push_back(USEC_TO_SEC(E.value));
		}

		timings.push_front("physics_3d");

		engine_debugger->profiler_add_frame_data(profiler_name, timings);
	}

	for (KeyValue<const void *, uint64_t> &E : timings_by_job) {
		E.value = 0;
	}
}

#endif
