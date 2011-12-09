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
#include "../devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "filesys/file.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list[PRI_MAX + 1];

static struct list sleep_list;
#ifdef USERPROG
static struct list zombie_list;
#endif

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

fp_t thread_load_avg;

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void thread_recalculate_priorities (struct thread *t, void *aux);
static void thread_recalculate_load_avg (void);
static int thread_get_ready_threads (void);

static void sleep_wakeup (void);
static int thread_get_priority_of (struct thread *t);
static void thread_recalculate_recent_cpu (struct thread *t, void *aux);

static inline struct thread *
thread_list_entry (const struct list_elem *e)
{
  struct thread *result = list_entry (e, struct thread, elem);
  ASSERT (is_thread (result));
  return result;
}

static void*
ready_lists_arent_messed_up_sub (struct list_elem *e, void *aux)
{
  ASSERT (thread_list_entry (e) != NULL);
  return aux;
}

static bool
ready_lists_arent_messed_up (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  int prio;
  for (prio = PRI_MIN; prio <= PRI_MAX; ++prio)
    list_foldl (&ready_list[prio], ready_lists_arent_messed_up_sub, NULL);
#ifdef USERPROG
  list_foldl (&zombie_list, ready_lists_arent_messed_up_sub, NULL);
#endif
  return true;
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

  int i;
  for (i = PRI_MIN; i <= PRI_MAX; ++i)
    {
      list_init (&ready_list[i]);
    }

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

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
  else
    {
      fp_incr_inplace (&t->recent_cpu);
#ifdef USERPROG
      if (t->pagedir != NULL)
        user_ticks++;
      else
#endif
        kernel_ticks++;
    }

  if(thread_mlfqs && ((timer_ticks () % TIMER_FREQ) == 0))
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
  
#ifdef USERPROG
  hash_init (&t->fds, fd_hash, fd_less, t);
  t->exit_code = -1;
  
  struct thread *current_thread = thread_current ();
  t->parent = current_thread;
  list_push_back (&current_thread->children, &t->parent_elem);
#endif

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
  int priority = thread_get_priority_of (t);
  list_push_back (&ready_list[priority], &t->elem);
  t->status = THREAD_READY;
  
  ASSERT (ready_lists_arent_messed_up ());
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
  
  intr_disable ();
  list_remove (&t->allelem);

#ifdef USERPROG
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
    {
      process_exit ();
      t->pagedir = NULL;
    }
  
  if (t->parent == NULL)
    {
      ASSERT (!list_is_interior (&t->parent_elem));
      t->status = THREAD_DYING;
    }
  else
    {
      ASSERT (list_is_interior (&t->parent_elem));
      
      list_push_back (&zombie_list, &t->elem);
      sema_up (&t->wait_sema);
      t->status = THREAD_ZOMBIE;
    }
#else
  t->status = THREAD_DYING;
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  schedule ();
  NOT_REACHED ();
}

#ifdef USERPROG
void
thread_dispel_zombie (struct thread *t)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_ZOMBIE);
  
  if (t->parent)
    {
      ASSERT (list_is_interior (&t->parent_elem));
      list_remove (&t->parent_elem);
    }
  else
    ASSERT (!list_is_interior (&t->parent_elem));
  
  list_remove (&t->elem);
  palloc_free_page (t);
}
#endif

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    {
      int priority = thread_get_priority_of (cur);
      list_push_back (&ready_list[priority], &cur->elem);
    }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (func != NULL);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      ASSERT (is_thread (t));
      func (t, aux);
    }
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
sleep_add(int64_t wakeup)
{
  if(wakeup < 0)
    {
      wakeup = 0;
    }
  
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
  int result = PRI_MAX - fp_round (fp_add (fp_div (t->recent_cpu, fp_from_int(4)),
                                           fp_from_int(t->nice * 2)));
  if (result > PRI_MAX)
    result = PRI_MAX;
  else if (result < PRI_MIN)
    result = PRI_MIN;

  t->priority = result;
  if (t == running_thread ())
    return;
  
  ASSERT (ready_lists_arent_messed_up ());
  if (t->status == THREAD_RUNNING)
    {
      list_remove (&t->elem);
      list_push_back (&ready_list[result], &t->elem);
    }
  ASSERT (ready_lists_arent_messed_up ());
  
}

static void
thread_recalculate_load_avg (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  int ready_threads = thread_get_ready_threads ();
  
  /* (59/60)*load_avg + (1/60)*ready_threads */
  fp_t summand1 = fp_mult (fp_div (fp_from_int (59), fp_from_int (60)), thread_load_avg);
  fp_t summand2 = fp_mult (fp_div (fp_from_int (1),  fp_from_int (60)), fp_from_int (ready_threads));

  thread_load_avg = fp_add (summand1, summand2);
  ASSERT (0 <= fp_round (thread_load_avg));
}

static int
thread_get_ready_threads (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  
  int result = 0;
  int prio;
  for (prio = PRI_MIN; prio <= PRI_MAX; ++prio)
    result += list_size (&ready_list[prio]);
  if (thread_current () != idle_thread)
    ++result;
  ASSERT (ready_lists_arent_messed_up ());
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
static void
idle (void *idle_started_ UNUSED) 
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
static void
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
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
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
  
#ifdef USERPROG
  list_elem_init (&t->parent_elem);
  list_init (&t->children);
  sema_init (&t->wait_sema, 0);
#endif
  
  if (!thread_mlfqs)
    {
      t->priority = priority;
    }
  else if (t != idle_thread)
    {
      struct thread *current_thread = running_thread ();
      t->priority = current_thread->priority;
      t->nice = current_thread->nice;
    }
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

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);
  
  int i;
  for (i = PRI_MAX; PRI_MIN <= i; --i)
    if (!list_empty (&ready_list[i]))
      return thread_list_entry (list_pop_front (&ready_list[i]));
  return idle_thread;
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
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

// TODO: we don't need 64 lists ...
static void
reschedule_ready_lists (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  
  int prio;
  struct list reschedule[PRI_MAX-PRI_MIN+1];
  for(prio = PRI_MIN; prio <= PRI_MAX; ++prio)
    list_init (&reschedule[prio]);
    
  struct list_elem *e, *end, *next;
  for (prio = PRI_MAX; prio >= PRI_MIN; --prio)
    {
      end = list_end (&ready_list[prio]);
      for (e = list_begin (&ready_list[prio]); e != end; e = next)
        {
          next = list_next (e);
          int actual_prio = thread_get_priority_of (thread_list_entry (e));
          if (actual_prio == prio)
            continue;
          list_remove (e);
          list_push_back (&reschedule[actual_prio], e);
        }
    }
      
  for(prio = PRI_MIN; prio <= PRI_MAX; ++prio)
    {
      end = list_end (&reschedule[prio]);
      for (e = list_begin (&reschedule[prio]); e != end; e = next)
        {
          next = list_remove (e);
          list_push_back (&ready_list[prio], e);
        }
    }
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
  
  reschedule_ready_lists();
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
const uint32_t thread_stack_ofs = offsetof (struct thread, stack);

#ifdef USERPROG
struct thread *
thread_find_tid (tid_t tid)
{
  struct thread *result = NULL;
  enum intr_level old_level = intr_disable ();
  
  struct list_elem *e;
  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      ASSERT (is_thread (t));
      if (t->tid == tid)
        {
          result = t;
          goto end;
        }
    }
  
  for (e = list_begin (&zombie_list); e != list_end (&zombie_list);
       e = list_next (e))
    {
      struct thread *t = thread_list_entry (e);
      if (t->tid == tid)
        {
          result = t;
          goto end;
        }
    }

end:
  intr_set_level (old_level);
  return result;
}

bool
thread_is_file_currently_executed (struct file *f)
{
  ASSERT (f != NULL);
  int old_level = intr_disable ();
  
  struct inode *inode = file_get_inode (f);
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
#endif
