/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "arch/i386.h"
#include "arch/i386-div64.h"
#include "kernel.h"
#include "smp/smp.h"
#include "smp/apic.h"
#include "util/printf.h"
#include "sched/sched.h"

/* Scheduler-integrated blocking sleep routines */

//#define DEBUG_SCHED_SLEEP

#ifdef DEBUG_SCHED_SLEEP
#define DLOG(fmt,...) DLOG_PREFIX("sched-sleep",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

static quest_tss * sleepqueue = NULL;

extern uint64 tsc_freq;         /* timestamp counter frequency */

static inline uint64
compute_finish (uint32 usec)
{
  uint64 ticks;
  uint64 start;

  RDTSC (start);

  ticks = div64_64 (tsc_freq * (u64) usec, 1000000LL);

  return start + ticks;
}

static inline uint64
compute_finish_nanosec (u64 nanosec)
{
  uint64 ticks;
  uint64 start;

  RDTSC (start);

  ticks = div64_64 (tsc_freq * nanosec, 1000000000LL);

  return start + ticks;
}

/* Must hold lock */
extern void
sched_usleep (uint32 usec)
{
  if (mp_enabled) {
    quest_tss *tssp;
    uint64 finish = compute_finish (usec);
#ifdef DEBUG_SCHED_SLEEP
    u64 now; RDTSC (now);
#endif
    tssp = str ();
    DLOG ("task 0x%x sleeping for %d usec (0x%llX -> 0x%llX)",
          tssp->tid, usec, now, finish);
    tssp->time = finish;
    queue_append (&sleepqueue, tssp);

    schedule ();
  } else
    /* interrupts not enabled */
    tsc_delay_usec (usec);
}

extern void
sched_nanosleep (struct timespec * t)
{
  if (mp_enabled) {
    quest_tss *tssp;
    uint64 nanosec = t->tv_sec * 1000000000LL + t->tv_nsec;
    uint64 finish = compute_finish_nanosec (nanosec);
#ifdef DEBUG_SCHED_SLEEP
    u64 now; RDTSC (now);
#endif
    tssp = str();
    DLOG ("task 0x%x sleeping for %llX nanosec (0x%llX -> 0x%llX)",
          tssp->tid, nanosec, now, finish);
    tssp->time = finish;
    /* a magic number to indicate nanosleep */
    tssp->hr_sleep = 73;
    queue_append (&sleepqueue, tssp);

    /* --TC--
     * The longest time to the next timer interrupt is 1 millisecond.
     * If I need to sleep longer than that, don't bother with the timer.
     * Otherwise before I go to sleep, program the timer to wake me up.
     * Note that it's possible the timer is already set to fire
     * before my wakeup time. In this case I shouldn't program the timer
     * to delay its fire (But I don't really need to worry about this because 
     * LAPIC_start_timer() will do the checking for me). When the timer
     * fires, sleep queue will be traversed in the timer interrupt handler.
     * Then, I need to update my time left to wake up and make the decision
     * again to program the timer or not.
     */
    if (nanosec < 1000000LL)
      LAPIC_start_timer_tick_only(finish);

    schedule ();
  } 
}

/* Spin for a given amount of microsec. */
extern void
tsc_delay_usec (uint32 usec)
{
  uint64 value, finish;

  finish = compute_finish (usec);
  for (;;) {
    RDTSC (value);
    if (value >= finish)
      break;
    asm volatile ("pause");
  }
}

/* Must hold lock */
extern void
process_sleepqueue (void)
{
  uint64 now;
  quest_tss **q, *next;
  quest_tss *tssp;

  RDTSC (now);

  if (sleepqueue == NULL)
    return;

  q = &sleepqueue;
  tssp = sleepqueue;
  DLOG ("process_sleepqueue 0x%llX", now);
  for (;;) {
    /* examine finish time of task */
    next = tssp->next;

    if (tssp->time <= now) {
      DLOG ("waking task 0x%x (0x%llX <= 0x%llX)", *q, tssp->time, now);
      /* time to wake-up */
      wakeup (*q);
      /* remove from sleepqueue */
      *q = next;
      tssp->time = 0;
      tssp->hr_sleep = 0;
    } else {
      if (tssp->hr_sleep == 73) {
        /* nanosleep */
        uint64 tick_left = tssp->time - now;
        uint64 millisec = div64_64((tick_left * 1000), tsc_freq);
        if (millisec < 1)
          LAPIC_start_timer_tick_only(tick_left);
      }
      q = &tssp->next;
    }

    /* move to next sleeper */
    if (next == NULL)
      break;
    tssp = *q;
  }
}

/* Detach a task from sleep queue. This is used in migration. */
/* Must hold lock */
extern bool
sleepqueue_detach (quest_tss *task)
{
  quest_tss **q = NULL;
  quest_tss *tssp = NULL, *next = NULL;
  task_id tid = task->tid;

  if (sleepqueue == NULL)
    return FALSE;

  q = &sleepqueue;
  tssp = sleepqueue;

  for (;;) {
    next = tssp->next;
    if ((*q)->tid == tid) {
      *q = next;
      return TRUE;
    }
    
    if (next == NULL)
      return FALSE;
    q = &tssp->next;
    tssp = *q;
  }

  return FALSE;
}

extern void
sleepqueue_append (quest_tss *tid)
{
  queue_append (&sleepqueue, tid);
}
/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
