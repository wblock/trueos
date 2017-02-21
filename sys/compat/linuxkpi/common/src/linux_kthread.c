/*-
 * Copyright (c) 2017 Hans Petter Selasky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/compat.h>

#include <sys/bus.h>
#include <sys/interrupt.h>
#include <sys/priority.h>
#include <sys/kthread.h>
#include <sys/sched.h>

enum {
	KTHREAD_SHOULD_STOP_MASK = (1 << 0),
	KTHREAD_SHOULD_PARK_MASK = (1 << 1),
	KTHREAD_IS_PARKED_MASK = (1 << 2),
};

bool
kthread_should_stop_task(struct task_struct *task)
{

	return (atomic_read(&task->kthread_flags) & KTHREAD_SHOULD_STOP_MASK);
}

bool
kthread_should_stop(void)
{

	return (atomic_read(&current->kthread_flags) & KTHREAD_SHOULD_STOP_MASK);
}

bool
kthread_should_park(void)
{

	return (atomic_read(&current->kthread_flags) & KTHREAD_SHOULD_PARK_MASK);
}

int
kthread_park(struct task_struct *task)
{

	if (task == NULL)
		return (-ENOSYS);

	if (atomic_read(&task->kthread_flags) & KTHREAD_IS_PARKED_MASK)
		goto done;

	atomic_or(KTHREAD_SHOULD_PARK_MASK, &task->kthread_flags);

	if (task == current)
		goto done;

	wake_up_process(task);
	wait_for_completion(&task->parked);
done:
	return (0);
}

void
kthread_unpark(struct task_struct *task)
{

	if (task == NULL)
		return;

	atomic_andnot(KTHREAD_SHOULD_PARK_MASK, &task->kthread_flags);

	if (atomic_fetch_andnot(KTHREAD_IS_PARKED_MASK,
	    &task->kthread_flags) & KTHREAD_IS_PARKED_MASK) {
		wake_up_state(task, TASK_PARKED);
	}
}

void
kthread_parkme(void)
{
	struct task_struct *task = current;

	/* don't park threads without a task struct */
	if (task == NULL)
		return;

	atomic_set(&task->state, TASK_PARKED);

	while (atomic_read(&task->kthread_flags) & KTHREAD_SHOULD_PARK_MASK) {
		if (!(atomic_fetch_or(KTHREAD_IS_PARKED_MASK,
		      &task->kthread_flags) & KTHREAD_IS_PARKED_MASK)) {
			complete(&task->parked);
		}
		schedule();
		atomic_set(&task->state, TASK_PARKED);
	}

	atomic_andnot(KTHREAD_IS_PARKED_MASK, &task->kthread_flags);

	atomic_set(&task->state, TASK_RUNNING);
}

int
kthread_stop(struct task_struct *task)	
{
	int retval;

	/*
	 * Assume task is still alive else caller should not call
	 * kthread_stop():
	 */
	atomic_or(KTHREAD_SHOULD_STOP_MASK, &task->kthread_flags);
	kthread_unpark(task);
	wake_up_process(task);
	wait_for_completion(&task->exited);

	/*
	 * Get return code and free task structure:
	 */
	retval = task->task_ret;
	linux_free_current(task);

	return (retval);
}

struct task_struct *
linux_kthread_setup_and_run(struct thread *td, linux_task_fn_t *task_fn, void *arg)
{
	struct task_struct *task;

	linux_set_current(td);

	task = td->td_lkpi_task;
	task->task_fn = task_fn;
	task->task_data = arg;

	thread_lock(td);
	/* make sure the scheduler priority is raised */
	sched_prio(td, PI_SWI(SWI_NET));
	/* put thread into run-queue */
	sched_add(td, SRQ_BORING);
	thread_unlock(td);

	return (task);
}

void
linux_kthread_fn(void *arg __unused)
{
	struct task_struct *task = current;

	if (kthread_should_stop_task(task) == 0)
		task->task_ret = task->task_fn(task->task_data);

	if (kthread_should_stop_task(task) != 0) {
		struct thread *td = curthread;

		/* let kthread_stop() free data */
		td->td_lkpi_task = NULL;

		/* wakeup kthread_stop() */
		complete(&task->exited);
	}
	kthread_exit();
}

int
linux_try_to_wake_up(struct task_struct *task, unsigned int state)
{
	int retval;

	/*
	 * To avoid loosing any wakeups this function must be made
	 * atomic with regard to wakeup by locking the sleep_lock:
	 */
	mtx_lock(&task->sleep_lock);

	/* first check if the there are any sleepers */
	if (atomic_read(&task->state) & state) {
		if (atomic_xchg(&task->state, TASK_WAKING) != TASK_WAKING) {
			wakeup_one(task);
			retval = 1;
		} else {
			retval = 0;
		}
	} else {
		retval = 0;
	}
	mtx_unlock(&task->sleep_lock);

	return (retval);
}
