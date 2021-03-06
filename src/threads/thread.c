#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "vm/vm.h"
#include "vm/swap.h"
#include "vm/mmap.h"

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
static struct list sleep_list;
static struct list zombie_list;
static struct hash tids_hash;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

static bool tick_print_free;

bool
thread_activate_pool_statistics (bool yes)
{
  bool old = tick_print_free;
  tick_print_free = yes;
  return old;
}

fp_t thread_load_avg;

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux) NO_RETURN;

static void NO_RETURN idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void thread_recalculate_priorities (struct thread *t, void *aux);
static void thread_recalculate_load_avg (void);
static size_t thread_get_ready_threads (void);

static void sleep_wakeup (void);
static int thread_get_priority_of (struct thread *t);
static void thread_recalculate_recent_cpu (struct thread *t, void *aux);

#define ASSERT_STACK_NOT_EXCEEDED(T)                 \
({                                                   \
  const struct thread *_t = (T);                     \
  const uintptr_t _s UNUSED = (uintptr_t) _t->stack; \
  const uintptr_t _n UNUSED = (uintptr_t) _t;        \
  ASSERT (_s <= _n + PGSIZE);                        \
  ASSERT (_s >= _n + sizeof (struct thread));        \
  (void) 0;                                          \
})

static inline struct thread *
thread_list_entry (const struct list_elem *e)
{
  struct thread *result = list_entry (e, struct thread, elem);
  ASSERT (is_thread (result));
  return result;
}

static unsigned
thread_tids_hash (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  return hash_entry (e, struct thread, tids_elem)->tid;
}

/* Compares the value of two hash elements A and B, given
   auxiliary data AUX.  Returns true if A is less than B, or
   false if A is greater than or equal to B. */
static bool
thread_tids_less (const struct hash_elem *a,
                  const struct hash_elem *b,
                  void *aux UNUSED)
{
  return thread_tids_hash (a, NULL) < thread_tids_hash (b, NULL);
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);

  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);
#if USERPROG
  list_init (&zombie_list);
#endif

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
  hash_init (&tids_hash, thread_tids_hash, thread_tids_less, NULL);
  
  printf ("Initialized threading.\n");
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}
 
# define UNSAFE_PRINTF(...) \
  ({ \
    enum intr_level _old_level = intr_disable (); \
    struct thread *_current_thread = running_thread (); \
    enum thread_status _old_status =  _current_thread->status; \
    _current_thread->status = THREAD_RUNNING; \
    printf (__VA_ARGS__); \
    _current_thread->status = _old_status; \
    intr_set_level (_old_level); \
    (void)0; \
  })

static void
thread_print_tick_status (struct thread *t)
{
  static size_t tickcount = 0;
  size_t ksize, kfree, usize, ufree, ssize, sfree;
  uint64_t processor_ticks;
  
  palloc_fill_ratio (&kfree, &ksize, &ufree, &usize);
  
  ssize = swap_stats_pages ();
  sfree = ssize - swap_stats_full_pages ();
  
  asm ("rdtsc" : "=A"(processor_ticks));
  
  UNSAFE_PRINTF ("    %8u (%20llu). %s    kernel: %4u/% 4d (%3u%%)"
                                       "    user: %4u/% 4d (%3u%%)"
                                       "    swap: %4u/% 4d (%3u%%)\n",
                 tickcount++, processor_ticks,
                 t == idle_thread ? "IDLE  " :
                       t->pagedir ? "USER  " :
                                    "KERNEL",
                 kfree, ksize, fp_round (fp_percent_from_uint (kfree, ksize)),
                 ufree, usize, fp_round (fp_percent_from_uint (ufree, usize)),
                 sfree, ssize, fp_round (fp_percent_from_uint (sfree, ssize)));
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();
  ASSERT_STACK_NOT_EXCEEDED (t);
  
  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
  else
    {
      fp_incr_inplace (&t->recent_cpu);
      if (t->pagedir != NULL)
        user_ticks++;
      else
        kernel_ticks++;
    }

  if (thread_mlfqs && ((timer_ticks () % TIMER_FREQ) == 0))
    {
      /* Because of assumptions made by some of the tests, 
       * load_avg must be updated exactly when the system 
       * tick counter reaches a multiple of a second, that 
       * is, when timer_ticks () % TIMER_FREQ == 0, and 
       * not at any other time.
       */
      thread_recalculate_load_avg ();
      thread_foreach (thread_recalculate_recent_cpu, NULL);
      thread_foreach (thread_recalculate_priorities, NULL);
    }
    
  if (t->pagedir != NULL)
    vm_tick (t);

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    {
      if(thread_mlfqs && t != idle_thread)
        {
          thread_recalculate_priorities (thread_current (), NULL);
        }
      intr_yield_on_return ();
    }
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  hash_insert (&tids_hash, &t->tids_elem);

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;
  
  hash_init (&t->fds_hash, fd_hash, fd_less, t);
  heap_init (&t->fds_heap, fd_heap_less, t);
  t->exit_code = -1;
  
  struct thread *current_thread = thread_current ();
  t->parent = current_thread;
  list_push_back (&current_thread->children, &t->parent_elem);

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  /* Test if the newly created thread has higher priority than the current one.
   * Yield if appropriate. 
   */
  if (thread_get_priority () <= priority)
    {
       thread_yield ();
    }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());
  struct thread *t = thread_current ();
  
  //printf (">>> Exit: %u (%s)\n", t->tid, t->name);
  
  intr_disable ();
  list_remove (&t->allelem);
  
  ASSERT (list_empty (&t->lock_list));
  
  if (t->cwd)
    {
      pifs_close (t->cwd);
      t->cwd = NULL;
    }

  // when the parent dies, all children must be disposed
  while (!list_empty (&t->children))
    {
      struct thread *child = list_entry (list_pop_front (&t->children),
                                         struct thread, parent_elem);
      ASSERT (is_thread (child));
      ASSERT (child->parent == t);
      
      if (child->status == THREAD_ZOMBIE)
        thread_dispel_zombie (child);
      else
        child->parent = NULL;
    }
    
  if (t->pagedir)
    process_exit ();
  ASSERT (list_empty (&t->lock_list));
  
  if (t->parent == NULL)
    t->status = THREAD_DYING;
  else
    {
      ASSERT (list_is_interior (&t->parent_elem));
      
      list_push_back (&zombie_list, &t->elem);
      sema_up (&t->wait_sema);
      t->status = THREAD_ZOMBIE;
    }

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  schedule ();
  NOT_REACHED ();
}

void
thread_dispel_zombie (struct thread *t)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (t));
  
  if (t->parent)
    {
      ASSERT (list_is_interior (&t->parent_elem));
      list_remove (&t->parent_elem);
    }
  else
    ASSERT (!list_is_interior (&t->parent_elem));
  
  hash_delete (&tids_hash, &t->tids_elem);
  list_remove (&t->elem);
  palloc_free_page (t);
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
    
  if (tick_print_free)
    thread_print_tick_status (cur);

  if (cur != idle_thread)
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
static void
thread_for_list (thread_action_func *func, void *aux, struct list *list)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (func != NULL);

  for (e = list_begin (list); e != list_end (list); e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      ASSERT (is_thread (t));
      func (t, aux);
    }
}

void
thread_foreach (thread_action_func *func, void *aux)
{
  thread_for_list (func, aux, &all_list);
}

bool
thread_cmp_wakeup (const struct list_elem *a,
                   const struct list_elem *b,
                   void *aux)
{
  (void)aux;
  
  struct thread *aa = thread_list_entry (a);
  struct thread *bb = thread_list_entry (b);

  return aa->wakeup < bb->wakeup;
}

bool
thread_cmp_priority (const struct list_elem *a,
                     const struct list_elem *b,
                     void *aux)
{
  (void)aux;
  
  struct thread *aa = thread_list_entry (a);
  struct thread *bb = thread_list_entry (b);

  return thread_get_priority_of (aa) < thread_get_priority_of (bb);
}

/* removes thread from ready_list and inserts it to sleep_list */
void
sleep_add (int64_t wakeup)
{
  if(wakeup < 0)
    wakeup = 0;
  
  enum intr_level old_level = intr_disable ();
  struct thread *current_thread = thread_current ();
  
  current_thread->wakeup = wakeup + timer_ticks ();
  
  list_insert_ordered (&sleep_list, &current_thread->elem, thread_cmp_wakeup,
                       NULL);
  
//  printf ("\tsleep_add(%lld) for %d, %d elements in list.\n",
//          wakeup, current_thread->tid, list_size (&sleep_list));
  thread_block ();
  intr_set_level (old_level);
}

static void
sleep_wakeup (void)
{
  enum intr_level old_level = intr_disable ();
  while (!list_empty (&sleep_list))
    {
      struct list_elem *head = list_begin (&sleep_list);
      struct thread *t = thread_list_entry (head);
      if (t->wakeup > timer_ticks ())
        break;
        
      list_pop_front (&sleep_list);
      t->wakeup = 0;
      thread_unblock (t);
    }
  intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority)
{
  ASSERT (PRI_MIN <= new_priority && new_priority <= PRI_MAX);

  if (thread_mlfqs)
    return;

  thread_current ()->priority = new_priority;
  thread_yield ();
}

static int
thread_get_priority_of_real (struct thread *t)
{
  /* Idea:
   * 
   * result = t->priority
   * for each lock_iter in t->lock_list:
   *   for each thread_iter in lock->semaphore.waiters:
   *     result = max(result, thread_get_priority_of_real(thread_iter))
   * return result
   */
   
  // TODO: detect deadlock.
  // Deadlocked threads would render a stackover currently.
  
  ASSERT (is_thread (t));
  ASSERT (intr_get_level () == INTR_OFF);
  
  int result = t->priority;
  
  struct list_elem *lock_iter;
  struct list_elem *lock_iter_end = list_end (&t->lock_list);
  
  for (lock_iter = list_begin (&t->lock_list);
       lock_iter != lock_iter_end;
       lock_iter = list_next (lock_iter))
    {
      struct lock *lock = list_entry (lock_iter, struct lock, holder_elem);
      struct list_elem *thread_iter;
      struct list_elem *thread_iter_end = list_end (&lock->semaphore.waiters);
      
      for (thread_iter = list_begin (&lock->semaphore.waiters);
           thread_iter != thread_iter_end;
           thread_iter = list_next (thread_iter))
        {
          struct thread *locked_thread = thread_list_entry (thread_iter);
          int locked_prio = thread_get_priority_of_real (locked_thread);
          if (locked_prio > result)
            result = locked_prio;
        }
    }
  return result;
}

static int
thread_get_priority_of (struct thread *t)
{
  ASSERT (is_thread(t));
  enum intr_level old_level = intr_disable ();

  int result;
  if(thread_mlfqs)
    result = t->priority;
  else
    result = thread_get_priority_of_real (t);

  intr_set_level (old_level);
  return result;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_get_priority_of (thread_current ());
}

/* Sets the current thread's nice value to new_nice and 
 * recalculates the thread's priority based on the new 
 * value (see section B.2 Calculating Priority). If the 
 * running thread no longer has the highest priority, 
 * yields.
 */
void
thread_set_nice (int nice) 
{
  if(nice < -20)
    nice = -20;
  else if(nice > 20)
    nice = 20;
  struct thread *t = thread_current ();
    
  enum intr_level old_level = intr_disable ();
  t->nice = nice;
  thread_recalculate_priorities (t, NULL);
  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

static void
thread_recalculate_recent_cpu (struct thread *t, void *aux UNUSED)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (t));

  /* (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice.*/
  fp_t result = thread_load_avg;
  result = fp_mult (result, fp_from_int (2));
  result = fp_div  (result, fp_add (fp_mult (thread_load_avg, fp_from_int (2)),
                                    fp_from_int (1)));
  result = fp_mult (result, t->recent_cpu);
  result = fp_add  (result, fp_from_int (t->nice));
  t->recent_cpu = result;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  int result = fp_round (fp_mult (thread_load_avg, fp_from_int (100)));
  ASSERT (0 <= result);
  return result;
}

/* Returns 100 times the current thread's recent_cpu value, 
 * rounded to the nearest integer.
 */
static int
thread_get_recent_cpu_of (struct thread *t)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (t));
  return fp_round (fp_mult (t->recent_cpu, fp_from_int (100)));
}

/* Returns 100 times the current thread's recent_cpu value, 
 * rounded to the nearest integer.
 */
int
thread_get_recent_cpu (void)
{
  enum intr_level old_level = intr_disable ();
  int result = thread_get_recent_cpu_of (thread_current ());
  intr_set_level (old_level);
  return result;
}

static void
thread_recalculate_priorities (struct thread *t, void *aux UNUSED)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (t));

  /*     PRI_MAX -  (recent_cpu / 4) - (nice * 2) */
  /* <=> PRI_MAX - ((recent_cpu / 4) + (nice * 2)) */
  int result = PRI_MAX - fp_round (fp_add (fp_div (t->recent_cpu,
                                                   fp_from_int (4)),
                                           fp_from_int (t->nice*2)));
  if (result > PRI_MAX)
    result = PRI_MAX;
  else if (result < PRI_MIN)
    result = PRI_MIN;

  t->priority = result;
  if (t == running_thread ())
    return;
  
  if (t->status == THREAD_RUNNING)
    {
      list_remove (&t->elem);
      list_push_back (&ready_list, &t->elem);
    }
  
}

static void
thread_recalculate_load_avg (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  size_t ready_threads = thread_get_ready_threads ();
  
  /* (59/60)*load_avg + (1/60)*ready_threads */
  fp_t summand1 = fp_mult (fp_div (fp_from_uint (59), fp_from_uint (60)),
                           thread_load_avg);
  fp_t summand2 = fp_mult (fp_div (fp_from_uint (1),  fp_from_uint (60)),
                           fp_from_uint (ready_threads));

  thread_load_avg = fp_add (summand1, summand2);
  ASSERT (0 <= fp_round (thread_load_avg));
}

static size_t
thread_get_ready_threads (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  
  size_t result = list_size (&ready_list);
  if (thread_current () != idle_thread)
    ++result;
  return result;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void NO_RETURN
idle (void *idle_started_)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void NO_RETURN
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool UNUSED
is_thread (struct thread *t)
{
  ASSERT (t != NULL);
  ASSERT ((uintptr_t) t % 4096 == 0);
  ASSERT (t->status > 0 && t->status < THREAD_MAX);
  ASSERT (t->magic == THREAD_MAGIC);
  return true;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  
  t->magic = THREAD_MAGIC;
  list_init (&t->lock_list);
  list_push_back (&all_list, &t->allelem);
  
  list_elem_init (&t->parent_elem);
  list_init (&t->children);
  sema_init (&t->wait_sema, 0);
  
  struct thread *current_thread = running_thread ();
  if (!thread_mlfqs)
    {
      t->priority = priority;
    }
  else if (t != idle_thread)
    {
      t->priority = current_thread->priority;
      t->nice = current_thread->nice;
    }
    
#ifdef FILESYS
  struct pifs_inode *cwd = current_thread->cwd;
  if (cwd != NULL)
    {
      ++cwd->open_count;
      t->cwd = cwd;
    }
#endif
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
schedule_tail (struct thread *prev) 
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

  /* Activate the new address space. */
  process_activate ();

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      hash_delete (&tids_hash, &prev->tids_elem);
      palloc_free_page (prev);
    }
}

static bool
thread_prio_less (const struct list_elem *a, const struct list_elem *b,
				void *aux UNUSED)
{
 struct thread *aa = thread_list_entry (a);
 struct thread *bb = thread_list_entry (b);
 return thread_get_priority_of (aa) < thread_get_priority_of (bb);
}

static struct thread *
next_thread_to_run (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  
  if (list_empty (&ready_list))
    return idle_thread;
  
  struct list_elem *e = list_max (&ready_list, &thread_prio_less, NULL);
  list_remove (e);
  return thread_list_entry (e);
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  sleep_wakeup ();
  ASSERT (intr_get_level () == INTR_OFF);
  
  struct thread *cur = running_thread ();
  ASSERT_STACK_NOT_EXCEEDED (cur);
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    {
      ASSERT_STACK_NOT_EXCEEDED (next);
      prev = switch_threads (cur, next);
    }
  schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 0;
  return __sync_add_and_fetch (&next_tid, 1);
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
const uint32_t thread_stack_ofs = offsetof (struct thread, stack);

struct thread *
thread_find_tid (tid_t tid)
{
  enum intr_level old_level = intr_disable ();
  struct thread key;
  key.magic = THREAD_MAGIC;
  key.status = THREAD_ZOMBIE;
  key.tid = tid;
  struct hash_elem *e = hash_find (&tids_hash, &key.tids_elem);
  intr_set_level (old_level);
  return e ? hash_entry (e, struct thread, tids_elem) : NULL;
}

bool
thread_is_file_currently_executed (struct file *f)
{
  ASSERT (f != NULL);
  int old_level = intr_disable ();
  
  struct pifs_inode *inode = file_get_inode (f);
  bool result = false;
  
  struct list_elem *e;
  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      ASSERT (is_thread (t));
      
      if (t->executable && inode == file_get_inode (t->executable))
        {
          result = true;
          break;
        }
    }
  intr_set_level (old_level);
  return result;
}

#ifdef FILESYS
bool
thread_is_file_currently_cwd (struct pifs_inode *inode)
{
  if (inode == NULL)
    return false;
  int old_level = intr_disable ();
  
  bool result = false;
  
  struct list_elem *e;
  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      ASSERT (is_thread (t));
      if (inode == t->cwd)
        {
          result = true;
          break;
        }
    }
  intr_set_level (old_level);
  return result;
}
#endif
