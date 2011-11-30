#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* A counting semaphore. */
struct semaphore 
  {
    volatile unsigned value;    /* Current value. */
    struct list waiters;        /* List of waiting threads. */
  };

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);

/* Lock. */
struct lock 
  {
    struct thread *holder;      /* Thread holding lock */
    struct list_elem holder_elem; /* lock list of holder */
    struct semaphore semaphore; /* Binary semaphore controlling access. */
  };

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);
struct thread *lock_get_holder (struct lock *);

/* Condition variable. */
struct condition 
  {
    struct list waiters;        /* List of waiting threads. */
  };

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

struct rwlock
  {
    struct lock edit_lock;
    struct condition readers_list, writers_list;
    volatile unsigned readers_count, writers_count;
  };

void rwlock_init (struct rwlock *rwlock);
void rwlock_acquire_read (struct rwlock *rwlock);
void rwlock_acquire_write (struct rwlock *rwlock);
bool rwlock_try_acquire_read (struct rwlock *rwlock);
bool rwlock_try_acquire_write (struct rwlock *rwlock);
void rwlock_release_read (struct rwlock *rwlock);
void rwlock_release_write (struct rwlock *rwlock);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */
