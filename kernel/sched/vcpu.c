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

/* ************************************************** */

#include "sched/vcpu.h"
#include "sched/sched.h"
#include "arch/i386-percpu.h"
#include "arch/i386-div64.h"
#include "util/perfmon.h"
#include "util/cpuid.h"
#include "smp/smp.h"
#include "smp/apic.h"
#include "smp/spinlock.h"
#include "util/debug.h"
#include "util/printf.h"
#include "mem/malloc.h"
#include "vm/migration.h"
#include "vm/shm.h"

#define UNITS_PER_SEC 1000

//#define DEBUG_VCPU
//#define DEBUG_VCPU_VERBOSE
//#define DUMP_STATS_VERBOSE
//#define DUMP_STATS_VERBOSE_2
//#define CHECK_INVARIANTS

/* Use sporadic servers for I/O VCPUs */
//#define SPORADIC_IO

#ifdef DEBUG_VCPU
#define DLOG(fmt,...) DLOG_PREFIX("vcpu",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif


#ifdef DEBUG_VCPU_VERBOSE
#define DLOGV(fmt,...) DLOG_PREFIX("vcpu",fmt,##__VA_ARGS__)
#else
#define DLOGV(fmt,...) ;
#endif

u32 tsc_freq_msec, tsc_unit_freq;
u64 vcpu_init_time;
static bool vcpu_init_called = FALSE;


static struct sched_param init_params[] = {
  { .type = MAIN_VCPU, .C = 10, .T = 100 }, /* Best Effort VCPU */
#ifdef SPORADIC_IO
  { .type = IO_VCPU, .C = 10, .T = 300, .io_class = IOVCPU_CLASS_USB },
  { .type = IO_VCPU, .C = 10, .T = 400, .io_class = IOVCPU_CLASS_ATA },
  { .type = IO_VCPU, .C = 10, .T = 500, .io_class = IOVCPU_CLASS_NET },
#else
  { .type = IO_VCPU, .C = 1, .T = 10, .io_class = IOVCPU_CLASS_USB },
  { .type = IO_VCPU, .C = 1, .T = 10, .io_class = IOVCPU_CLASS_ATA },
  { .type = IO_VCPU, .C = 1, .T = 10, .io_class = IOVCPU_CLASS_NET },
  { .type = IO_VCPU, .C = 1, .T = 10, .io_class = IOVCPU_CLASS_GPIO },
#endif
};

#define NUM_INIT_VCPUS (sizeof (init_params) / sizeof (struct sched_param))

#define MAX_NUM_VCPUS 100

static vcpu *vcpu_list[MAX_NUM_VCPUS];
static vcpu_id_t max_vcpu_id = 0;
static int num_vcpus = 0;

extern vcpu_id_t
lowest_priority_vcpu (void)
{
  vcpu_id_t i;
  vcpu_id_t n=0;
  u64 T=0;
  for (i=0; i<max_vcpu_id; i++) {
    if (vcpu_list[i] && vcpu_list[i]->type == MAIN_VCPU && vcpu_list[i]->T >= T) {
      T = vcpu_list[i]->T;
      n = i;
    }
  }
  return n;
}

vcpu *
vcpu_lookup (vcpu_id_t i)
{
  if (0 <= i && i < MAX_NUM_VCPUS)
    return vcpu_list[i];
  return NULL;
}

vcpu_id_t
vcpu_index (vcpu *v)
{
  return v->index;
}

void
vcpu_lock (vcpu *vcpu)
{
  spinlock_lock (&vcpu->lock);
}

void
vcpu_unlock (vcpu *vcpu)
{
  spinlock_unlock (&vcpu->lock);
}

/* locked functions */

bool
vcpu_in_runqueue (vcpu *vcpu, quest_tss *task)
{
  quest_tss *i = vcpu->runqueue;

  while (i != NULL) {
    if (task == i) return TRUE;
    i = i->next;
  }
  return task == NULL;
}

void vcpu_queue_remove(vcpu** queue, vcpu* vcpu)
{
  while (*queue) {
    if (*queue == vcpu) {
      *queue = vcpu->next;
      return;
    }
    queue = &((*queue)->next);
  }
}

void vcpu_destroy(vcpu_id_t vcpu_index)
{
  vcpu* v = vcpu_lookup(vcpu_index);
  if(v) {
    vcpu_queue_remove(percpu_pointer (v->cpu, vcpu_queue), v);
    vcpu_list[vcpu_index] = NULL;
  }
}

void
vcpu_remove_from_runqueue (vcpu *vcpu, quest_tss *task)
{
  quest_tss **q = &vcpu->runqueue;
  while (*q != NULL) {
    if ((*q) == task) {
      *q = (*q)->next;
      return;
    }
    q = &(*q)->next;
  }
}

void
vcpu_internal_schedule (vcpu *vcpu)
{
  u64 now = vcpu->virtual_tsc;

  if (vcpu->next_schedule == 0 || vcpu->next_schedule <= now)
    goto sched;

  if (vcpu_in_runqueue (vcpu, vcpu->tr) == TRUE) {
    /* keep vcpu->tr running, remove from runqueue */
    vcpu_remove_from_runqueue (vcpu, vcpu->tr);
  } else
    goto sched;

  return;

 sched:
  /* round-robin */
  vcpu->tr = queue_remove_head (&vcpu->runqueue);
  vcpu->next_schedule = now + vcpu->quantum;
}

void
vcpu_runqueue_append (vcpu *vcpu, quest_tss *task)
{
  queue_append (&vcpu->runqueue, task);
}

#define preserve_segment(next)                                  \
  {                                                             \
    tss *tssp = (tss *)lookup_TSS (next);                       \
    u16 sel;                                                    \
    asm volatile ("movw %%"PER_CPU_SEG_STR", %0":"=r" (sel));   \
    tssp->usFS = sel;                                           \
  }

#define switch_to(next) software_context_switch (next)

void
vcpu_switch_to (vcpu *vcpu)
{
  switch_to (vcpu->tr);
}

bool
vcpu_is_idle (vcpu *vcpu)
{
  return vcpu->tr == 0;
}

void
vcpu_queue_append (vcpu **queue, vcpu *vcpu)
{
  while (*queue) {
    if (*queue == vcpu)
      /* already on queue */
      return;
    queue = &((*queue)->next);
  }
  vcpu->next = NULL;
  *queue = vcpu;
}


static vcpu_id_t next_vcpu_index(void)
{
  vcpu_id_t i;
  for(i = 0; i < MAX_NUM_VCPUS; ++i) {
    if(!vcpu_list[i]) return i;
  }
  return -1;
}


vcpu *
vcpu_queue_remove_head (vcpu **queue)
{
  vcpu *vcpu = NULL;
  if (*queue) {
    vcpu = *queue;
    *queue = (*queue)->next;
  }
  return vcpu;
}

/* ************************************************** */

DEF_PER_CPU (vcpu *, vcpu_queue);
INIT_PER_CPU (vcpu_queue) {
  percpu_write (vcpu_queue, NULL);
}

DEF_PER_CPU (vcpu *, vcpu_current);
INIT_PER_CPU (vcpu_current) {
  percpu_write (vcpu_current, NULL);
}

DEF_PER_CPU (quest_tss *, vcpu_idle_task);
INIT_PER_CPU (vcpu_idle_task) {
  percpu_write (vcpu_idle_task, NULL);
}

/* task accounting */
u64
vcpu_current_vtsc (void)
{
  vcpu *v = percpu_read (vcpu_current);
  return v->virtual_tsc;
}

static void
vcpu_acnt_end_timeslice (vcpu *vcpu)
{
  u64 now;

  RDTSC (now);

  if (vcpu->prev_tsc) {
    vcpu->timestamps_counted += now - vcpu->prev_tsc;
    vcpu->virtual_tsc += now - vcpu->prev_tsc;
  }

#if 0
  int i;
  for (i=0; i<2; i++) {
    u64 value = perfmon_pmc_read (i);
    if (vcpu->prev_pmc[i])
      vcpu->pmc_total[i] += value - vcpu->prev_pmc[i];
  }
#endif
}

static void
vcpu_acnt_begin_timeslice (vcpu *vcpu)
{
  u64 now;

  RDTSC (now);
  vcpu->prev_tsc = now;

#if 0
  int i;
  for (i=0; i<2; i++) {
    u64 value = perfmon_pmc_read (i);
    vcpu->prev_pmc[i] = value;
  }
#endif
}

extern void
vcpu_rr_schedule (void)
{
  quest_tss *next = NULL;
  vcpu
    *queue = percpu_read (vcpu_queue),
    *cur   = percpu_read (vcpu_current),
    *vcpu  = NULL;

  if (cur)
    /* handle end-of-timeslice accounting */
    vcpu_acnt_end_timeslice (cur);
  if (queue) {
    /* get next vcpu from queue */
    vcpu = vcpu_queue_remove_head (&queue);
    /* perform 2nd-level scheduling */
    vcpu_internal_schedule (vcpu);
    next = vcpu->tr;
    /* if vcpu still has a runqueue, put it back on 1st-level queue */
    if (vcpu->runqueue)
      vcpu_queue_append (&queue, vcpu);
    percpu_write (vcpu_queue, queue);
    percpu_write (vcpu_current, vcpu);
    DLOG ("vcpu_schedule: pcpu=%d vcpu=%p vcpu->tr=0x%x ->runqueue=0x%x next=0x%x",
          get_pcpu_id (), vcpu, vcpu->tr, vcpu->runqueue, next);
  }
  if (vcpu)
    /* handle beginning-of-timeslice accounting */
    vcpu_acnt_begin_timeslice (vcpu);
  if (next == NULL) {
    /* no task selected, go idle */
    next = percpu_read (vcpu_idle_task);
    percpu_write (vcpu_current, NULL);
  }
  if (next == NULL) {
    /* workaround: vcpu_idle_task was not initialized yet */
    next = idleTSS_selector[get_pcpu_id ()];
    percpu_write (vcpu_idle_task, next);
  }

  /* switch to new task or continue running same task */
  if (str () == next)
    return;
  else
    switch_to (next);
}

#if 0
extern void
vcpu_rr_wakeup (task_id task)
{
  DLOG ("vcpu_wakeup (0x%x), cpu=%d", task, get_pcpu_id ());
  quest_tss *tssp = lookup_TSS (task);
  static int next_vcpu_binding = 1;

  if (tssp->cpu == 0xFF) {
    do {
      tssp->cpu = next_vcpu_binding;
      next_vcpu_binding++;
      if (next_vcpu_binding >= NUM_VCPUS)
        next_vcpu_binding = 0;
    } while  (vcpu_lookup (tssp->cpu)->type != MAIN_VCPU);
    logger_printf ("vcpu: task 0x%x now bound to vcpu=%d\n", task, tssp->cpu);
  }

  vcpu *vcpu = vcpu_lookup (tssp->cpu);

  /* put task on vcpu runqueue (2nd level) */
  vcpu_runqueue_append (vcpu, task);

  /* put vcpu on pcpu queue (1st level) */
  vcpu_queue_append (percpu_pointer (vcpu->cpu, vcpu_queue), vcpu);

  if (!vcpu->runnable && !vcpu->running && vcpu->hooks->unblock)
    vcpu->hooks->unblock (vcpu);
  vcpu->runnable = TRUE;
}
#endif

/* ************************************************** */

DEF_PER_CPU (u64, pcpu_tprev);
INIT_PER_CPU (pcpu_tprev) {
  percpu_write64 (pcpu_tprev, 0LL);
}
DEF_PER_CPU (s64, pcpu_overhead);
INIT_PER_CPU (pcpu_overhead) {
  percpu_write64 (pcpu_overhead, 0LL);
}
DEF_PER_CPU (u64, pcpu_idle_time);
INIT_PER_CPU (pcpu_idle_time) {
  percpu_write64 (pcpu_idle_time, 0LL);
}
DEF_PER_CPU (u64, pcpu_idle_prev_tsc);
INIT_PER_CPU (pcpu_idle_prev_tsc) {
  percpu_write64 (pcpu_idle_prev_tsc, 0LL);
}
DEF_PER_CPU (u32, pcpu_sched_time);
INIT_PER_CPU (pcpu_sched_time) {
  percpu_write (pcpu_sched_time, 0);
}

static void
idle_time_acnt_begin ()
{
  u64 now;

  RDTSC (now);
  percpu_write64 (pcpu_idle_prev_tsc, now);
}

static void
idle_time_acnt_end ()
{
  u64 idle_time = percpu_read64 (pcpu_idle_time);
  u64 idle_prev_tsc = percpu_read64 (pcpu_idle_prev_tsc);
  u64 now;

  RDTSC (now);

  if (idle_prev_tsc)
    idle_time += now - idle_prev_tsc;
  percpu_write64 (pcpu_idle_time, idle_time);
}

static inline u32
compute_percentage (u64 overall, u64 usage)
{
  u64 res = div64_64 (usage * 10000, overall);
  u16 whole, frac;

  whole = ((u16) res) / 100;
  frac = ((u16) res) - whole * 100;
  return (((u32) whole) << 16) | (u32) frac;
}

extern void
vcpu_dump_stats (void)
{
  static u32 dump_count = 0;
  int i;
#ifdef DUMP_STATS_VERBOSE_2
  vcpu *cur = percpu_read (vcpu_current);
#endif
  s64 overhead = percpu_read64 (pcpu_overhead);
  u64 idle_time = percpu_read64 (pcpu_idle_time);
  u64 sum = idle_time;
  u32 stime = percpu_read (pcpu_sched_time);
  extern u32 uhci_sample_bps (void);
  extern u32 atapi_sample_bps (void);
  u32 uhci_bps = 0;//uhci_sample_bps ();
  u32 atapi_bps = atapi_sample_bps ();
  u64 now; RDTSC (now);

  logger_printf ("cpu %d vcpu_dump_stats n=%d t=0x%llX ms=0x%X\n",
                 get_pcpu_id (), dump_count++, now, tsc_freq_msec);

  now -= vcpu_init_time;
  RDTSC (vcpu_init_time);

  u32 sched = compute_percentage (now, stime);
  logger_printf ("  overhead=0x%llX sched=%02d.%02d uhci_bps=%d atapi_bps=%d\n",
                 overhead,
                 sched >> 16, sched & 0xFF,
                 uhci_bps, atapi_bps);
#define DUMP_CACHE_STATS
#ifdef DUMP_CACHE_STATS
  vcpu *queue = percpu_read (vcpu_queue);
  vcpu **ptr = NULL;

  if (queue) {
    for (ptr = &queue; *ptr != NULL; ptr = &(*ptr)->next) {
      logger_printf ("vcpu=%X pcpu=%d cache occupancy=%llX mpki=%llX\n type=%d",
                     (uint32) *ptr, (*ptr)->cpu, (*ptr)->cache_occupancy,
                     (*ptr)->mpki, (*ptr)->type);
    }
  }
#endif
#ifdef DUMP_STATS_VERBOSE
  extern u64 irq_response, irq_turnaround, irq_resp_max, irq_resp_min;
  extern u64 atapi_sector_read_time, atapi_sector_cpu_time;
  extern u64 atapi_req_diff;
  extern u32 atapi_req_count, ata_irq_count;
  extern u32 e1000_packet_count;
  extern u64 e1000_packet_bytes;
  if (ata_irq_count && atapi_req_count)
    logger_printf ("  response=0x%llX responsemax=0x%llX responsemin=0x%llX\n"
                   "  readtime=0x%llX readvcpu=0x%llX"
                   " avgreqdiff=0x%llX\n",
                   div64_64 (irq_response, (u64) ata_irq_count),
                   irq_resp_max,
                   irq_resp_min,
                   div64_64 (atapi_sector_read_time, (u64) atapi_req_count),
                   div64_64 (atapi_sector_cpu_time, (u64) atapi_req_count),
                   div64_64 (atapi_req_diff, (u64) atapi_req_count));

  extern u64 atapi_count, atapi_cycles, atapi_max, atapi_min;
  if (atapi_count)
    logger_printf ("  atapiavg=0x%llX atapimax=0x%llX atapimin=0x%llX\n",
                   div64_64 (atapi_cycles, atapi_count),
                   atapi_max, atapi_min);
  atapi_count = atapi_max = atapi_cycles = 0;
  atapi_min = ~0LL;

  logger_printf ("  e1000pps=0x%llX e1000bps=0x%llX\n",
                 div64_64 ((u64) e1000_packet_count * tsc_freq, now),
                 div64_64 (e1000_packet_bytes * tsc_freq, now));
  e1000_packet_bytes = 0;
  e1000_packet_count = 0;

  /* 5-sec window */
  ata_irq_count = 0;
  irq_resp_max = irq_turnaround = irq_response = 0;
  irq_resp_min = ~0LL;
  atapi_req_count = 0;
  atapi_req_diff = 0;
  atapi_sector_read_time = atapi_sector_cpu_time = 0;

#ifdef DUMP_STATS_VERBOSE_2
  logger_printf ("idle tsc=0x%llX%s\n", idle_time, (cur==NULL ? " (*)" : ""));
#endif
#endif

  percpu_write64 (pcpu_idle_time, 0LL);
  percpu_write (pcpu_sched_time, 0);

  for (i=0; i<max_vcpu_id; i++) {
    vcpu *vcpu = vcpu_list[i];
    if(vcpu) {
#if defined(DUMP_STATS_VERBOSE) && defined (DUMP_STATS_VERBOSE_2)
      if (vcpu->type == IO_VCPU) {
        logger_printf ("vcpu=%d pcpu=%d tsc=0x%llX pmc[0]=0x%llX pmc[1]=0x%llX%s\n",
                       i, vcpu->cpu,
                       vcpu->timestamps_counted,
                       vcpu->pmc_total[0],
                       vcpu->pmc_total[1],
                       (vcpu == cur ? " (*)" : ""));
        logger_printf ("  b=0x%llX overhead=0x%llX delta=0x%llX usage=0x%X\n",
                       vcpu->b, vcpu->sched_overhead, vcpu->prev_delta,
                       vcpu->prev_usage);
      }
#endif
    }
    sum += vcpu->timestamps_counted;
  }

  u32 res = compute_percentage (now, idle_time);
  logger_printf (" idle=%02d.%02d\n", res >> 16, res & 0xFF);
  for (i=0; i<max_vcpu_id; i++) {
    vcpu *vcpu = vcpu_list[i];
    if(vcpu) {
      res = compute_percentage (now, vcpu->timestamps_counted);
      vcpu->timestamps_counted = 0;
#ifndef SPORADIC_IO
      logger_printf (" V%02d=%02d.%02d %d", i, res >> 16, res & 0xFF,
                     vcpu->type != MAIN_VCPU ? 0 : vcpu->main.Q.size);
#else
      logger_printf (" V%02d=%02d.%02d %d", i, res >> 16, res & 0xFF,
                     vcpu->main.Q.size);
#endif
      if ((i % 4) == 3) {
        logger_printf ("\n");
      }
    }
  }
  logger_printf ("\nend vcpu_dump_stats\n");
}

/* ************************************************** */

void
repl_queue_pop (repl_queue *Q)
{
  if (Q->head) {
    replenishment *r = Q->head->next;
    Q->head->t = Q->head->b = 0;
    Q->head->next = NULL;
    Q->head = r;
    Q->size--;
  }
}

void
repl_queue_add (repl_queue *Q, u64 b, u64 t)
{
  if (Q->size < MAX_REPL) {
    replenishment *r = NULL, **rq;
    int i;
    for (i=0; i<MAX_REPL; i++) {
      if (Q->array[i].t == 0) {
        r = &Q->array[i];
        break;
      }
    }
    if (!r) panic ("Q->size < MAX_REPL but no free entry");
    rq = &Q->head;
    /* find insertion point */
    while (*rq && (*rq)->t < t)
      rq = &(*rq)->next;
    /* insert */
    r->next = *rq;
    *rq = r;
    r->t = t;
    r->b = b;
    Q->size++;
  }
}

/* ************************************************** */

#ifdef CHECK_INVARIANTS
static void
check_run_invariants (void)
{
  vcpu
    *queue = percpu_read (vcpu_queue),
    *cur   = percpu_read (vcpu_current),
    *v, *q;
  int i;
  if (cur && !cur->running) panic ("current is not running");
  for (i=0; i<NUM_VCPUS; i++) {
    v = &vcpus[i];
    if (v->running && v != cur)
      panic ("vcpu running is not current");
    if (v->runnable) {
      for (q = queue; q; q = q->next) {
        if (q == v) goto ok;
      }
      panic ("vcpu runnable is not on queue");
    ok:;
    } else {
      for (q = queue; q; q = q->next) {
        if (q == v) panic ("vcpu not runnable is on queue");
      }
    }
    if (v->type == MAIN_VCPU) {
      if (v->main.Q.size >= MAX_REPL-1)
        logger_printf ("vcpu %d has %d repls\n", i, v->main.Q.size);
      u64 sum = 0;
      replenishment *r;
      for (r = v->main.Q.head; r != NULL; r = r->next)
        sum += r->b;
      if (sum != v->C) {
        com1_printf ("v->C=0x%llX sum=0x%llX\n", v->C, sum);
        panic ("vcpu replenishments out of whack");
      }
    }
  }
}
#endif

extern bool migration_thread_ready;

extern void
vcpu_schedule (void)
{
  quest_tss *next = NULL;

#ifdef USE_VMX
  vcpu * mvcpu = NULL;
#endif

  vcpu
    *queue = percpu_read (vcpu_queue),
    *cur   = percpu_read (vcpu_current),
    *vcpu  = NULL,
    **ptr,
    **vnext = NULL;
  u64 tprev = percpu_read64 (pcpu_tprev);
  u64 tcur, tdelta, Tnext = 0;
  //u64 Tprev = 0;
  bool timer_set = FALSE;

#ifdef CHECK_INVARIANTS
  check_run_invariants ();
#endif

  RDTSC (tcur);

  tdelta = tcur - tprev;

  DLOGV ("tcur=0x%llX tprev=0x%llX tdelta=0x%llX", tcur, tprev, tdelta);

  if (cur) {
    //Tprev = cur->T;

    /* handle end-of-timeslice accounting */
    vcpu_acnt_end_timeslice (cur);

    /* invoke VCPU-specific end of timeslice budgeting */
    if (cur->hooks->end_timeslice)
      cur->hooks->end_timeslice (cur, tdelta);
  } else idle_time_acnt_end ();

  if (queue) {
    /* pick highest priority vcpu with available budget */
    for (ptr = &queue; *ptr != NULL; ptr = &(*ptr)->next) {
      if (Tnext == 0 || (*ptr)->T < Tnext) {
        /* update replenishments to refresh budget */
        if ((*ptr)->hooks->update_replenishments)
          (*ptr)->hooks->update_replenishments (*ptr, tcur);
        if ((*ptr)->b > 0) {
          Tnext = (*ptr)->T;
          vnext = ptr;
        }
      }
    }

    if (vnext) {
      vcpu = *vnext;
      /* internally schedule */
      vcpu_internal_schedule (vcpu);
      /* keep vcpu on queue if it has other runnable tasks */
      if (vcpu->runqueue == 0) {
        /* otherwise, remove it */
        *vnext = (*vnext)->next;
        vcpu->runnable = FALSE;
      }
      next = vcpu->tr;

#if 0
      if (cur != vcpu) {
        perfmon_vcpu_acnt_end (cur);
      }
#endif

      percpu_write (vcpu_current, vcpu);
      percpu_write (vcpu_queue, queue);
      DLOGV ("scheduling vcpu=%p with budget=0x%llX", vcpu, vcpu->b);
    } else {
#if 0
      if (cur) {
        perfmon_vcpu_acnt_end (cur);
      }
#endif

      percpu_write (vcpu_current, NULL);
    }

    /* find time of next important event */
    if (vcpu)
      tdelta = vcpu->b;
    else
      tdelta = 0;
    for (ptr = &queue; *ptr != NULL; ptr = &(*ptr)->next) {
      if (Tnext == 0 || (*ptr)->T <= Tnext) {
        u64 event = 0;
        if ((*ptr)->hooks->next_event)
          event = (*ptr)->hooks->next_event (*ptr);
        if (event != 0 && (tdelta == 0 || event - tcur < tdelta))
          tdelta = event - tcur;
      }
    }

    /* set timer */
    if (tdelta > 0) {
      u32 count = (u32) div64_64 (tdelta * ((u64) cpu_bus_freq), tsc_freq);
      if (count == 0) {
        count = 1;
#ifdef NANOSLEEP
        LAPIC_start_timer_count_tick (count, tsc2cpu_bus_ratio);
#else
        LAPIC_start_timer(count);
#endif
      } else if (count > cpu_bus_freq / QUANTUM_HZ) {
        count = cpu_bus_freq / QUANTUM_HZ;
#ifdef NANOSLEEP
        LAPIC_start_timer_count_tick (count, tsc2QUANTUM_HZ_ratio);
#else
        LAPIC_start_timer(count);
#endif
      } else {
#ifdef NANOSLEEP
        LAPIC_start_timer_count_only (count);
#else
        LAPIC_start_timer(count);
#endif
      }
      if (vcpu) {
        vcpu->prev_delta = tdelta;
        vcpu->prev_count = count;
      }
      timer_set = TRUE;
    }
  }

#ifdef USE_VMX
  /* Check migration request */
  if (str () != next) {
    quest_tss * tss = str ();
    uint32 * ph_tss = NULL;
    quest_tss *waiter;
    uint current_cpu = get_pcpu_id ();

    if (tss) {
      if (tss->sandbox_affinity != current_cpu) {
        DLOG ("Migration Request: taskid=0x%X, src=%d, dest=%d, vcpu=%d",
            str ()->tid, current_cpu, tss->sandbox_affinity, tss->cpu);
        DLOG ("Current task: 0x%X, Next task: 0x%X", str ()->tid, next->tid);
        mvcpu = vcpu_lookup (tss->cpu);
        if (!mvcpu) {
          logger_printf ("No VCPU (%d) associated with Task 0x%X\n", tss->cpu, tss->tid);
        } else {
#ifdef USE_MIGRATION_THREAD
          if (!migration_thread_ready) {
            /* migration thread is not ready */
            goto vmx_migration_end;
          }
#endif
          if (shm->migration_queue[tss->sandbox_affinity]) {
            /* Located the migrating quest_tss and its VCPU */
            /* TODO:
             * Assume the queue can have only one element for now. In case of queued
             * requests, quest_tss should be chained as usual.
             */
            logger_printf ("Migration queue in sandbox %d is not empty\n",
                tss->sandbox_affinity);
            goto vmx_migration_end;
          } else {
            /* Can migration condition be met? */
            //if (!validate_migration_condition (tss)) {
            //  goto resume_schedule;
            //}
            /* Detach the migrating task from local scheduler */
            ph_tss = detach_task (tss, TRUE);
            DLOG ("Process quest_tss to be migrated: 0x%X", ph_tss);
            /* Add the migrating process to migration queue of destination sandbox */
            shm->migration_queue[tss->sandbox_affinity] = ph_tss;
          }

          /* All tasks waiting for us now belong on the runqueue. */
          /* TODO:
           * For now, we wake up all the waiting processes. This
           * is not really a solution for the shared resource problem.
           * Some more specific mechanisms must be devised for this
           * issue in the future.
           */
          while ((waiter = queue_remove_head (&tss->waitqueue))) {
            wakeup (waiter);
          }

          if (ph_tss) {
            /* Request migration */
            if (!request_migration (tss->sandbox_affinity))
              logger_printf ("Failed to send migration request to sandbox %d\n",
                  tss->sandbox_affinity);
          } else {
            logger_printf ("Failed to detach task 0x%X from local scheduler\n", tss->tid);
          }
        }
      }
    }
  }
vmx_migration_end:
#endif

#ifdef USB_MIGRATION
  /* Check migration request */
  if (str () != next) {
    quest_tss * tss = str ();
    uint32 * ph_tss = NULL;
    quest_tss *waiter;
    if (tss) {
      if (tss->machine_affinity) {
        DLOG ("Migration Request to another machine: taskid=0x%X, vcpu=%d",
            str ()->tid, tss->cpu);
        DLOG ("Current task: 0x%X, Next task: 0x%X", str ()->tid, next->tid);
        mvcpu = vcpu_lookup (tss->cpu);
        if (!mvcpu) {
          logger_printf ("No VCPU (%d) associated with Task 0x%X\n", tss->cpu, tss->tid);
        }
        else {

          if (usb_migration_queue_full()) {
            logger_printf ("Migration queue for usb is not empty empty\n");
          }
          else {
            /* Detach the migrating task from local scheduler */
            ph_tss = detach_task (tss, FALSE);
            if(!ph_tss) {
              DLOG("Failed to detach task for usb migration");
              panic("Failed to detach task for usb migration");
            }
            DLOG ("Process quest_tss to be migrated: 0x%X", ph_tss);
            /* Add the migrating process to the usb migration queue  */
            usb_add_task_to_migration_queue(ph_tss);

            /* All tasks waiting for us now belong on the runqueue. */
            /* TODO:
             * For now, we wake up all the waiting processes. This
             * is not really a solution for the shared resource problem.
             * Some more specific mechanisms must be devised for this
             * issue in the future.
             */
            while ((waiter = queue_remove_head (&tss->waitqueue))) {
              wakeup (waiter);
            }
          }
        }
      }
    }
  }
#endif

  if (!timer_set)
#ifdef NANOSLEEP
    LAPIC_start_timer_count_tick (cpu_bus_freq / QUANTUM_HZ, tsc2QUANTUM_HZ_ratio);
#else
    LAPIC_start_timer (cpu_bus_freq / QUANTUM_HZ);
#endif
  }

  if (vcpu) {
    /* handle beginning-of-timeslice accounting */
    vcpu_acnt_begin_timeslice (vcpu);
#if 0
    if (cur != vcpu)
      perfmon_vcpu_acnt_start (vcpu);
#endif
  } else
    idle_time_acnt_begin ();
  if (next == NULL) {
    /* no task selected, go idle */
    next = percpu_read (vcpu_idle_task);
    percpu_write (vcpu_current, NULL);
  }
  if (next == NULL) {
    /* workaround: vcpu_idle_task was not initialized yet */
    next = idleTSS_selector[get_pcpu_id ()];
    percpu_write (vcpu_idle_task, next);
  }

  /* current time becomes previous time */
  percpu_write64 (pcpu_tprev, tcur);

  /* measure schedule running time */
  u64 now; RDTSC (now);
  u32 sched_time = percpu_read (pcpu_sched_time);
  percpu_write (pcpu_sched_time, sched_time + (u32) (now - tcur));

  if (cur) cur->running = FALSE;
  if (vcpu) vcpu->running = TRUE;

  /* switch to new task or continue running same task */
  if (str () == next)
    return;
  else
    switch_to (next);
}

extern void
vcpu_wakeup (quest_tss *tssp)
{
  extern bool sleepqueue_detach (quest_tss *t);

  /* assign to vcpu BEST_EFFORT_VCPU if not already set */
  if (tssp->cpu == 0xFF) {
    tssp->cpu = BEST_EFFORT_VCPU;
  }

  sleepqueue_detach (tssp);

  vcpu *v = vcpu_lookup (tssp->cpu);

  /* put task on vcpu runqueue (2nd level) */
  vcpu_runqueue_append (v, tssp);

  /* put vcpu on pcpu queue (1st level) */
  vcpu_queue_append (percpu_pointer (v->cpu, vcpu_queue), v);

  if (!v->runnable && !v->running && v->hooks->unblock)
    v->hooks->unblock (v);

  v->runnable = TRUE;

  /* check if preemption necessary */
  u64 now;
  vcpu *cur = percpu_read (vcpu_current);

  RDTSC (now);

  if (v->hooks->update_replenishments)
    v->hooks->update_replenishments (v, now);

  if (v->b > 0 && (cur == NULL || cur->T > v->T))
    /* preempt */
#ifdef NANOSLEEP
    LAPIC_start_timer_count_tick (1, tsc2cpu_bus_ratio);
#else
    LAPIC_start_timer (1);
#endif
}

/* ************************************************** */

/* MAIN_VCPU */

static inline s64
capacity (vcpu *v)
{
  u64 now; RDTSC (now);
  if (v->main.Q.head == NULL || v->main.Q.head->t > now)
    return 0;
  return (s64) v->main.Q.head->b - (s64) v->usage;
}

static void
main_vcpu_update_replenishments (vcpu *v, u64 tcur)
{
  s64 cap = capacity (v);
  v->b = (cap > 0 ? cap : 0);
}

static u64
main_vcpu_next_event (vcpu *v)
{
  replenishment *r;
  u64 now; RDTSC (now);
  for (r = v->main.Q.head; r != NULL; r = r->next) {
    if (now < r->t)
      return r->t;
  }
  return 0;
}

#if 0
static void
repl_merge (vcpu *v)
{
  /* possibly merge */
  while (v->main.Q.size > 1) {
    u64 t = v->main.Q.head->t;
    u64 b = v->main.Q.head->b;
    /* observation 3 */
    if (t + b >= v->main.Q.head->next->t) {
      repl_queue_pop (&v->main.Q);
      v->main.Q.head->b += b;
      v->main.Q.head->t = t;
    } else
      break;
  }
}
#endif

static void
budget_check (vcpu *v)
{
  if (capacity (v) <= 0) {
    while (v->main.Q.head->b <= v->usage) {
      /* exhaust and reschedule the replenishment */
      v->usage -= v->main.Q.head->b;
      u64 b = v->main.Q.head->b, t = v->main.Q.head->t;
      repl_queue_pop (&v->main.Q);
      t += v->T;
      repl_queue_add (&v->main.Q, b, t);
    }
    if (v->usage > 0) {
      /* v->usage is the overrun amount */
      v->main.Q.head->t += v->usage;
      /* possibly merge */
      if (v->main.Q.size > 1) {
        u64 t = v->main.Q.head->t;
        u64 b = v->main.Q.head->b;
        if (t + b >= v->main.Q.head->next->t) {
          repl_queue_pop (&v->main.Q);
          v->main.Q.head->b += b;
          v->main.Q.head->t = t;
        }
      }
    }
#if 0
    if (capacity (v) == 0) {
      /* S.Q.head.time > Now */
      /* if not blocked then S.replenishment.enqueue (S.Q.head.time) */
    }
#endif
  }
}

static void
split_check (vcpu *v)
{
  u64 now; RDTSC (now);
  if (v->usage > 0 && v->main.Q.head->t <= now) {
    u64 remnant = v->main.Q.head->b - v->usage;
    if (v->main.Q.size == MAX_REPL) {
      /* merge with next replenishment */
      repl_queue_pop (&v->main.Q);
      v->main.Q.head->b += remnant;
    } else {
      /* leave remnant as reduced replenishment */
      v->main.Q.head->b = remnant;
    }
    repl_queue_add (&v->main.Q, v->usage, v->main.Q.head->t + v->T);
    /* invariant: sum of replenishments remains the same */
    v->usage = 0;
  }
}

static void
main_vcpu_end_timeslice (vcpu *cur, u64 tdelta)
{
  /* timeslice ends for one of 3 reasons: budget depletion,
   * preemption, or blocking */

  if (cur->b < tdelta) {
    cur->sched_overhead = tdelta - cur->b;
    percpu_write64 (pcpu_overhead, cur->sched_overhead);
  }

  cur->usage += tdelta;
  budget_check (cur);
  if (!cur->runnable)
    /* blocked */
    split_check (cur);

  s64 cap = capacity (cur);
  if (cap > 0) {
    /* was preempted or blocked */
    cur->b = cap;
  } else {
    /* budget was depleted */
    cur->b = 0;
    cur->prev_usage = cur->usage;
  }
}

static void
unblock_check (vcpu *v)
{
  if (capacity (v) > 0) {
    u64 now;
    RDTSC (now);
    v->main.Q.head->t = now;
    /* merge replenishments using observation 3 */
    while (v->main.Q.size > 1) {
      u64 b = v->main.Q.head->b;
      if (v->main.Q.head->next->t <= now + b - v->usage) {
        repl_queue_pop (&v->main.Q);
        v->main.Q.head->b += b;
        v->main.Q.head->t = now;
      } else
        break;
    }
  } else {
    /* S.replenishment.enqueue (S.Q.head.time) */
  }
}

static void
main_vcpu_unblock (vcpu *v)
{
  unblock_check (v);
}

static vcpu_hooks main_vcpu_hooks = {
  .update_replenishments = main_vcpu_update_replenishments,
  .next_event = main_vcpu_next_event,
  .end_timeslice = main_vcpu_end_timeslice,
  .unblock = main_vcpu_unblock
};

/* IO_VCPU */

static void
io_vcpu_update_replenishments (vcpu *v, u64 tcur)
{
  if (v->io.r.t != 0 && v->io.r.t <= tcur) {
    v->b = v->io.r.b;
    v->io.r.t = 0;
  }
}

static u64
io_vcpu_next_event (vcpu *v)
{
  return v->io.r.t;
}

static void
io_vcpu_end_timeslice (vcpu *cur, u64 tdelta)
{
  u64 u = tdelta;
  s64 overhead = percpu_read64 (pcpu_overhead);

  /* subtract from budget of current */
  if (cur->b < tdelta) {
    u = cur->b;
    cur->sched_overhead = tdelta - cur->b;
    overhead = cur->sched_overhead;
    percpu_write64 (pcpu_overhead, overhead);
  }

  cur->b -= u;

  cur->usage += tdelta;
  if (!cur->runnable || cur->b <= 0) {
    /* if blocked or exhausted, advance eligibility time */
    cur->io.e += div64_64 (cur->usage * cur->io.Uden, cur->io.Unum);
    /* schedule next replenishment */
    if (cur->io.r.t == 0) {
      cur->io.r.t = cur->io.e;
      cur->io.r.b = div64_64 (cur->T * cur->io.Unum, cur->io.Uden);
    } else {
      /* or update existing replenishment with new eligibility time */
      cur->io.r.t = cur->io.e;
    }
    cur->prev_usage = cur->usage;
    cur->usage = 0;

    cur->b = 0;
    if (!cur->runnable)
      /* we were blocked, reset budgeted state */
      cur->io.budgeted = FALSE;
  }
}

extern void
io_vcpu_unblock (vcpu *v)
{
  u64 now;
  RDTSC (now);

  if (!v->io.budgeted && v->io.e < now)
    /* eligibility time is in the past, push it up to now */
    v->io.e = now;

  u64 Cmax = div64_64 (v->T * v->io.Unum, v->io.Uden);

  if (v->io.r.t == 0) {
    if (!v->io.budgeted) {
      v->io.r.t = v->io.e;
      v->io.r.b = Cmax;
    }
  } else {
    v->io.r.b = Cmax;
  }
  v->io.budgeted = TRUE;
}

extern void
iovcpu_job_wakeup (quest_tss *tssp, u64 T)
{
  vcpu *v = vcpu_lookup (tssp->cpu);
  if (v->type == IO_VCPU) {
    if (T < v->T || !(v->running || v->runnable))
      v->T = T;
  }
  wakeup (tssp);
}

extern void
iovcpu_job_wakeup_for_me (quest_tss *job)
{
  vcpu *cur = percpu_read (vcpu_current);
  if (cur)
    iovcpu_job_wakeup (job, cur->T);
  else
    wakeup (job);
}

extern void
iovcpu_job_completion (void)
{
  schedule ();
}

extern uint
count_set_bits (u32 v)
{
  /* Wegner's method */
  u32 c;
  for (c = 0; v; c++) {
    v &= v - 1;              /* clear the least significant bit set */
  }
  return c;
}

extern uint
select_iovcpu (iovcpu_class class)
{
  uint i, idx = lowest_priority_vcpu (), matches = 0;
  for (i=0; i<max_vcpu_id; i++) {
    vcpu* v = vcpu_list[i];
    if (v->type == IO_VCPU) {
      u32 m = count_set_bits (v->io.class & class);
      if (m >= matches) {
        idx = i;
        matches = m;
      }
    }
  }
  return idx;
}

extern void
set_iovcpu (quest_tss *task, iovcpu_class class)
{
  uint i = select_iovcpu (class);
#if 0
  logger_printf ("iovcpu: task 0x%x requested class 0x%x and got IO-VCPU %d\n",
                 task, class, i);
#endif
  task->cpu = i;
}

static vcpu_hooks io_vcpu_hooks = {
  .update_replenishments = io_vcpu_update_replenishments,
  .next_event = io_vcpu_next_event,
  .end_timeslice = io_vcpu_end_timeslice,
  .unblock = io_vcpu_unblock
};

/* ************************************************** */

static vcpu_hooks *vcpu_hooks_table[] = {
  [MAIN_VCPU] = &main_vcpu_hooks,
  [IO_VCPU] = &io_vcpu_hooks
};

vcpu_id_t create_main_vcpu(int C, int T, vcpu** vcpu_p)
{
  struct sched_param sp = { .type = MAIN_VCPU, .C = C, .T = T };
  return create_vcpu(&sp, vcpu_p);
}

vcpu_id_t create_vcpu(struct sched_param* params, vcpu** vcpu_p)
{
  vcpu* vcpu;
  static int cpu_i=0;
  u32 C = params->C;
  u32 T = params->T;
  vcpu_type type = params->type;
  iovcpu_class io_class = params->io_class;
  vcpu_id_t vcpu_i = next_vcpu_index();

  if(!vcpu_init_called) {
    DLOG("Called VCPU create before init called");
    panic("Called VCPU create before init called");
  }
  if(vcpu_i < 0) return vcpu_i;

  vcpu = vcpu_list[vcpu_i] = kzalloc(sizeof(struct _vcpu));

  if(!vcpu) return -1;

  /* Can't fail at this point */

  vcpu->index = vcpu_i;

  num_vcpus++;
  if(max_vcpu_id < vcpu_i+1) max_vcpu_id = vcpu_i + 1;

#ifdef USE_VMX
  /* All VCPUs bind to current sandbox.
   * Initialization function will be called after forking
   * VM by each sandbox again.
   */
  cpu_i = get_pcpu_id ();
  vcpu->cpu = cpu_i;
#else
  vcpu->cpu = cpu_i++;
  if (cpu_i >= mp_num_cpus)
    cpu_i = 0;
#endif
  vcpu->quantum = div_u64_u32_u32 (tsc_freq, QUANTUM_HZ);
  vcpu->C = (u64)C * (u64)tsc_unit_freq;
  vcpu->T = (u64)T * (u64)tsc_unit_freq;
  vcpu->_C = C;
  vcpu->_T = T;
  vcpu->type = type;
#ifndef SPORADIC_IO
  if (vcpu->type == MAIN_VCPU) {
    repl_queue_add (&vcpu->main.Q, vcpu->C, vcpu_init_time);
  } else if (vcpu->type == IO_VCPU) {
    vcpu->io.Unum = C;
    vcpu->io.Uden = T;
    vcpu->io.class = io_class;
    vcpu->b = vcpu->C;
  }
  vcpu->hooks = vcpu_hooks_table[type];
#else
  repl_queue_add (&vcpu->main.Q, vcpu->C, vcpu_init_time);
  vcpu->hooks = vcpu_hooks_table[MAIN_VCPU];
#endif

  if(vcpu_p) {
    *vcpu_p = vcpu;
  }

  DLOG("vcpu: %svcpu=%d pcpu=%d C=0x%llX T=0x%llX U=%d%%\n",
       type == IO_VCPU ? "IO " : "",
       vcpu_i, vcpu->cpu, vcpu->C, vcpu->T, (C * 100) / T);

  return vcpu_i;
  
}


extern bool
vcpu_init (void)
{
  uint eax, ecx;
  int i;

  vcpu_init_called = TRUE;

  cpuid (1, 0, NULL, NULL, &ecx, NULL);
  cpuid (6, 0, &eax, NULL, NULL, NULL);

  DLOG("vcpu: init num_vcpus=%d num_cpus=%d TSC_deadline=%s ARAT=%s",
       NUM_INIT_VCPUS, mp_num_cpus,
       (ecx & (1 << 24)) ? "yes" : "no",
       (eax & (1 << 2))  ? "yes" : "no");

  memset (vcpu_list, 0, sizeof(vcpu_list));
  
  RDTSC (vcpu_init_time);
  tsc_freq_msec = div_u64_u32_u32 (tsc_freq, 1000);
  tsc_unit_freq = div_u64_u32_u32 (tsc_freq, UNITS_PER_SEC);
  logger_printf ("vcpu: tsc_freq_msec=0x%X unit_freq=0x%X\n",
                 tsc_freq_msec, tsc_unit_freq);

  /* distribute VCPUs across PCPUs */
  for (i=0; i<NUM_INIT_VCPUS; i++) {
    if(create_vcpu(&init_params[i], NULL) < 0) {
      com1_printf("Failed to create initial VCPUs\n");
      panic("Failed to create initial VCPUs");
    }
  }

  if(vcpu_lookup(0)->type != MAIN_VCPU) {
    /* VCPU zero must be a main VCPU.  Tasks that are not explicitly
       assigned a VCPU are treated as best effort tasks and assigned
       to VCPU zero */
    panic("Error VCPU 0 must be a main vcpu");
  }
  
  return TRUE;
}

#ifdef USE_VMX
/*
 * Verify the migration condition is met. Refer to Quest-V publication
 * for details. E_s needs to be determined and compare with migration
 * cost prediction if the migrating thread requires predictable migration.
 *
 * E_s >= floor (Delta_s / C_m) * T_m + Delta_s mod C_m
 *
 * TODO: Only monitoring now. Need to implement a flag and corresponding
 * user interface for user to specify migration requirement for each VCPU.
 */
bool
validate_migration_condition (quest_tss * tss)
{
  repl_queue * rq = NULL;
  replenishment * rp = NULL;
  vcpu * v = NULL;
  uint64 E_s = 0, time = 0;

  if (!tss) {
    logger_printf ("validate_migration_condition: Invalid tss!\n");
    return FALSE;
  }

  v = vcpu_lookup (tss->cpu);
  rq = &v->main.Q;
  rp = rq->head;

  if (rp) {
    /* TODO: Need to also look at sleep time and compare it with E_s */
    /* TODO: If the following equation holds:
     *
     *   E_s >= floor (Delta_s / C_m) * T_m + Delta_s mod C_m
     *
     * We return TRUE. Otherwise, we need to check wheter tss (its VCPU)
     * requires strict predictability.
     */
    RDTSC (time);
    if (rp->t <= time) {
      DLOG ("Migration rejected, replenishment pending!");
      return FALSE;
    }
    E_s = rp->t - time;
    logger_printf ("E_s=0x%X\n", E_s);
    //com1_printf ("Next Rep: 0x%llX\n", rp->t);
    //com1_printf ("Current Time: 0x%llX\n", time);
    //com1_printf ("Next event time is: 0x%llX\n", E_s);
  } else {
    /* Replenishment queue is empty */
    /* TODO: Check sleep queue */
    return FALSE;
  }

  if (!v) {
    DLOG ("validate_migration_condition: No VCPU for tss!");
    return FALSE;
  }

  return TRUE;
}

#endif


/* Fix replenishment queue */
bool
vcpu_fix_replenishment (quest_tss * tss, vcpu * v, replenishment r[], bool remote_tsc_diff,
                        uint64 remote_tsc)
{
  int i = 0;
  repl_queue * rq = NULL;
  replenishment * rp = NULL;

#ifdef DEBUG_VCPU
  /* What is the current replenishment queue of v? */
  rq = &v->main.Q;
  rp = rq->head;
  com1_printf ("Target VCPU Replenishment Queue:\n");
  while (rp) {
    com1_printf ("  b=0x%llX, t=0x%llX\n", rp->b, rp->t);
    rp = rp->next;
  }
#endif

  /* Assume this is a new VCPU with no task binded. */
  /* This is always true is we create a new VCPU for migrating task */
  /* Now, restore VCPU parameters */
  if ((tss->C_bak != v->C) || (tss->T_bak != v->T)) {
    /* VCPU in destination sandbox is not compatible with original one */
    logger_printf ("VCPU in destination is not compatible!\n");
    logger_printf ("  C=0x%llX, T=0x%llX, Cn=0x%llX, Tn=0x%llX\n",
                 tss->C_bak, tss->T_bak, v->C, v->T);
    return FALSE;
  } else {
    /* Clear the current replenishment queue */
    rq = &v->main.Q;
    rp = rq->head;
    while (rp) {
      repl_queue_pop (rq);
      rp = rq->head;
    }
    /* Add fixed new replenishments */
    for (i = 0; i < MAX_REPL; i++) {
      if (tss->vcpu_backup[i].t == 0) break;
      /* Fix timestamp values */
      if(remote_tsc_diff) {
        /* Local TSC is faster */
        repl_queue_add (rq, tss->vcpu_backup[i].b,
                        tss->vcpu_backup[i].t + remote_tsc);
      } else {
        /* Local TSC is slower */
        repl_queue_add (rq, tss->vcpu_backup[i].b,
                        tss->vcpu_backup[i].t - remote_tsc);
      }
    }
    v->b = tss->b_bak;
    v->usage = tss->usage_bak;

#ifdef DEBUG_VCPU
    /* Check the updated replenishment queue of v */
    rq = &v->main.Q;
    rp = rq->head;
    com1_printf ("Updated VCPU Replenishment Queue (b=0x%llX, usage=0x%llX):\n",
                 v->b, v->usage);
    while (rp) {
      com1_printf ("  b=0x%llX, t=0x%llX\n", rp->b, rp->t);
      rp = rp->next;
    }
#endif

    return TRUE;
  }

  return FALSE;
}
#ifdef USE_VMX

/* For sandbox to reset scheduler after vm fork. */
void
vcpu_reset (void)
{
  vcpu_init ();
 
  percpu_write (vcpu_queue, NULL);
  percpu_write (vcpu_current, NULL);
  percpu_write (vcpu_idle_task, NULL);
}
#endif

/* ************************************************** */

#include "module/header.h"

static const struct module_ops mod_ops = {
  .init = vcpu_init
};

DEF_MODULE (sched___vcpu, "VCPU scheduler", &mod_ops, {});

/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
