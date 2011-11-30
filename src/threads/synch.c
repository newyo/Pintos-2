/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      struct thread *current_thread = thread_current ();
      list_push_back (&sema->waiters, &current_thread->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;
  struct list_elem *max;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  sema->value++;
  if (list_empty (&sema->waiters))
    max = NULL;
  else
    {
      max = list_max (&sema->waiters, thread_cmp_priority, NULL);
      list_remove (max);
      thread_unblock (list_entry (max, struct thread, elem));
    }
  
  if (max && thread_cmp_priority (&thread_current ()->elem, max, NULL))
    {
      intr_set_level (old_level);
      thread_yield ();
    }
  else
    intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  list_elem_init (&lock->holder_elem);
  sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  sema_down (&lock->semaphore);
  
  enum intr_level old_level = intr_disable ();
  struct thread *current_thread = thread_current ();
  list_push_back (&current_thread->lock_list, &lock->holder_elem);
  lock->holder = current_thread;
  intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));
  ASSERT (!intr_context ());

  bool success = sema_try_down (&lock->semaphore);
  if (success)
    {
      enum intr_level old_level = intr_disable ();
      struct thread *current_thread = thread_current ();
      list_push_back (&current_thread->lock_list, &lock->holder_elem);
      lock->holder = current_thread;
      intr_set_level (old_level);
    }
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));
  ASSERT (!intr_context ());
  
  enum intr_level old_level = intr_disable ();
  list_remove_properly (&lock->holder_elem);
  lock->holder = NULL;
  intr_set_level (old_level);
  
  sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (
    (lock->holder == NULL && !list_is_interior (&lock->holder_elem)) ||
    (lock->holder != NULL &&  list_is_interior (&lock->holder_elem))
  )
  
  return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  struct semaphore_elem waiter;
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);
  
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

static bool
cond_cmp_waiters (const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED)
{
  struct semaphore_elem *aa, *bb;
  struct list_elem      *ae, *be;
  
  aa = list_entry (a, struct semaphore_elem, elem);
  ae = list_max (&aa->semaphore.waiters, thread_cmp_priority, NULL);
  
  bb = list_entry (b, struct semaphore_elem, elem);
  be = list_max (&bb->semaphore.waiters, thread_cmp_priority, NULL);
  
  return thread_cmp_priority (ae, be, NULL);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  enum intr_level old_level = intr_disable ();
  if (!list_empty (&cond->waiters))
    {
      struct list_elem *e = list_max (&cond->waiters, cond_cmp_waiters, NULL);
      list_remove (e);
      struct semaphore_elem *s = list_entry (e, struct semaphore_elem, elem);
      sema_up (&s->semaphore);
    }
  intr_set_level (old_level);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  enum intr_level old_level = intr_disable ();
  while (!list_empty (&cond->waiters))
    {
      struct list_elem *e = list_pop_front (&cond->waiters);
      struct semaphore_elem *s = list_entry (e, struct semaphore_elem, elem);
      sema_up (&s->semaphore);
    }
  intr_set_level (old_level);
}

void
rwlock_init (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  
  lock_init (&rwlock->edit_lock);
  cond_init (&rwlock->readers_list);
  cond_init (&rwlock->writers_list);
  rwlock->readers_count = rwlock->writers_count = 0;
}

void
rwlock_acquire_read (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  
  lock_acquire (&rwlock->edit_lock);
  while (rwlock->writers_count > 0)
    cond_wait (&rwlock->readers_list, &rwlock->edit_lock);
  ++rwlock->readers_count;
  lock_release (&rwlock->edit_lock);
}

void
rwlock_acquire_write (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  
  lock_acquire (&rwlock->edit_lock);
  while (rwlock->readers_count > 0 || rwlock->writers_count > 0)
    cond_wait (&rwlock->writers_list, &rwlock->edit_lock);
  ++rwlock->writers_count;
  lock_release (&rwlock->edit_lock);
}

bool
rwlock_try_acquire_read (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  
  if (!lock_try_acquire (&rwlock->edit_lock))
    return false;
  if (rwlock->writers_count > 0)
    return false;
  ++rwlock->readers_count;
  lock_release (&rwlock->edit_lock);
  
  return true;
}

bool
rwlock_try_acquire_write (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  
  if (!lock_try_acquire (&rwlock->edit_lock))
    return false;
  if (rwlock->readers_count > 0 || rwlock->writers_count > 0)
    return false;
  ++rwlock->writers_count;
  lock_release (&rwlock->edit_lock);
  
  return true;
}

void
rwlock_release_read (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  ASSERT (rwlock->readers_count > 0 && rwlock->writers_count == 0);
  
  lock_acquire (&rwlock->edit_lock);
  --rwlock->readers_count;
  if (rwlock->readers_count == 0 && !list_empty (&rwlock->writers_list.waiters))
    cond_signal (&rwlock->writers_list, &rwlock->edit_lock);
  else if (!list_empty (&rwlock->readers_list.waiters))
    cond_broadcast (&rwlock->readers_list, &rwlock->edit_lock);
  lock_release (&rwlock->edit_lock);
}

void
rwlock_release_write (struct rwlock *rwlock)
{
  ASSERT (rwlock != NULL);
  ASSERT (rwlock->readers_count == 0 && rwlock->writers_count == 1);
  
  lock_acquire (&rwlock->edit_lock);
  --rwlock->writers_count;
  if (!list_empty (&rwlock->readers_list.waiters))
    cond_broadcast (&rwlock->readers_list, &rwlock->edit_lock);
  else if (!list_empty (&rwlock->writers_list.waiters))
    cond_signal (&rwlock->writers_list, &rwlock->edit_lock);
  lock_release (&rwlock->edit_lock);
}
