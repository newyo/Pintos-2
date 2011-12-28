#include "vm.h"
#include <hash.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "lru.h"
#include "swap.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"

#define MIN_ALLOC_ADDR ((void *) (1<<16))
#define SWAP_AT_ONCE 32

enum vm_physical_page_type
{
  VMPPT_UNUSED = 0,   // this physical page is not used, yet
  
  VMPPT_EMPTY,        // read from, but never written to -> all zeros
  VMPPT_USED,         // allocated, no swap file equivalent
  VMPPT_SWAPPED,      // retreived from swap and not dirty or disposed
                      // OR removed from RAM
  
  VMPPT_COUNT
};

struct vm_logical_page
{
  void                       *user_addr;   // virtual address
  struct thread              *thread;      // owner thread
  struct hash_elem            thread_elem; // for thread.vm_pages
  struct lru_elem             lru_elem;    // for pages_lru
  enum vm_physical_page_type  type;
};

static bool vm_is_initialized;
static struct lru pages_lru;
static struct lock vm_lock;

static void
assert_t_addr(struct thread *t, const void *addr)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  ASSERT (t == thread_current ()); // debug
  ASSERT (addr >= MIN_ALLOC_ADDR);
  ASSERT (pg_ofs (addr) == 0);
}

void
vm_init (void)
{
  ASSERT (!vm_is_initialized);
  
  lru_init (&pages_lru, 0, NULL, NULL);
  lock_init (&vm_lock);
  
  vm_is_initialized = true;
  printf ("Initialized user's virtual memory.\n");
}

static unsigned
vm_thread_page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  
  struct vm_logical_page *ee;
  ee = hash_entry (e, struct vm_logical_page, thread_elem);
  return (unsigned) ee->user_addr;
}

static bool
vm_thread_page_less (const struct hash_elem *a,
                     const struct hash_elem *b,
                     void *aux UNUSED)
{
  struct vm_logical_page *aa, *bb;
  aa = hash_entry (a, struct vm_logical_page, thread_elem);
  bb = hash_entry (b, struct vm_logical_page, thread_elem);
  ASSERT (aa->thread != NULL && bb->thread != NULL);
  ASSERT (aa->thread == bb->thread);
  return (unsigned) aa->user_addr < (unsigned) bb->user_addr;
}

void
vm_init_thread (struct thread *t)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  
  //printf ("   INITIALISIERE VM FÜR %8p.\n", t);
  hash_init (&t->vm_pages, &vm_thread_page_hash, &vm_thread_page_less, t);
}

static void
vm_clean_sub (struct hash_elem *e, void *aux UNUSED)
{
  struct vm_logical_page *ee;
  ee = hash_entry (e, struct vm_logical_page, thread_elem);
  
  // TODO
  (void) ee;
}

void
vm_clean (struct thread *t)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  //printf ("   CLEANE VM VON %8p.\n", t);
    
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  hash_destroy (&t->vm_pages, &vm_clean_sub);
  
  intr_set_level (old_level);
  lock_release (&vm_lock);
}

bool
vm_alloc_zero (struct thread *t, void *addr)
{
  assert_t_addr (t, addr);
  
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  //printf ("   ALLOC ZERO: %8p\n", addr);
  
  struct vm_logical_page *page = calloc (1, sizeof (*page));
  if (!page)
    {
      //intr_set_level (old_level);
      lock_release (&vm_lock);
      return false;
    }
    
  page->type      = VMPPT_EMPTY;
  page->thread    = t;
  page->user_addr = addr;
  
  hash_insert (&t->vm_pages, &page->thread_elem);
  
  intr_set_level (old_level);
  lock_release (&vm_lock);
  return true;
}

static struct vm_logical_page *
vm_get_logical_page (struct thread *t, void *base)
{
  assert_t_addr (t, base);
  
  struct vm_logical_page key;
  key.user_addr = base;
  key.thread = t;
  struct hash_elem *e = hash_find (&t->vm_pages, &key.thread_elem);
  if (e == NULL)
    return NULL;
  return hash_entry (e, struct vm_logical_page, thread_elem);
}

/* Called when swap needed room and disposed an unchanged page.
 * Not called when disposal was initiated through swap_dispose|swap_clean.
 * Called once per disposed page.
 * Might be called with interrupts on!
 */
void
vm_swap_disposed (struct thread *t, void *base)
{
  assert_t_addr (t, base);
  
  // Must not lock vm_lock, as vm_ensure could trigger swap disposal!
  
  // With being inside swaps lock, page cannot have been free'd.
  // No need to disable interrupts.
  struct vm_logical_page *ee = vm_get_logical_page (t, base);
  ASSERT (ee != NULL);
  
  if (ee->type != VMPPT_SWAPPED)
    return;
  ee->type = VMPPT_USED;
}

void
vm_tick (struct thread *t)
{
  ASSERT (t != NULL);
  ASSERT (t->pagedir != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  if (!vm_is_initialized)
    return;
  
  // TODO: lru for accessed pages
  // TODO: swap disposing for dirty pages
}

static bool
swap_free_page (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  bool result = false;
  
  // No other swap operation, except swap_tick, can disturb this flow,
  // b/c of vm_lock.
  //
  // Schema:
  //
  // NO INTERRUPTS:
  //   1) Search least recently accessed user page A, with kernel page K.
  //   2) If A was swapped in
  //     2.1) IF NOT DIRTY A:
  //       2.1.1) Remove dispose A and free K.
  //       2.1.2) Remove A from LRU list.
  //       2.2.3) Tell swap A was swapped out and cannot be disposed.
  //     2.2) Otherwise:
  //       2.2.1) Mark A as not dirty, tell LRU A was used.
  //       2.2.2) Tell swap A is dirty.
  //       2.2.3) Go back to (1).
  //   3) Dispose A, remove A from LRU list, mark it as swapped out.
  //      (Dispose A here, so that another thread had an page fault,
  //      if referencing A.)
  // INTERRUPTABLE:
  //   4) Try allocate swap space and write K to it.
  // NO INTERRUPTS:
  //   IF (4) WORKED:
  //     5.1.1) Free K.
  //   OTHERWISE:
  //     5.2.1) Set A -> K in pagedir again.
  
  intr_disable ();
  
  struct vm_logical_page *ee;
  void *kpage;
  for (;;)
    {
      // (1)
      struct lru_elem *e = lru_peek_least (&pages_lru);
      if (!e)
        goto end;
      ee = lru_entry (e, struct vm_logical_page, lru_elem);
      kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
      ASSERT (kpage != NULL);
      
      // (2)
      if (ee->type == VMPPT_SWAPPED)
        {
          if(!pagedir_is_dirty (ee->thread->pagedir, ee->user_addr))
            {
              // (2.1.1)
              pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
              palloc_free_page (kpage);
              // (2.1.2)
              lru_dispose (&pages_lru, &ee->lru_elem, false);
              // (2.1.3)
              // TODO: telling
              result = true;
              goto end;
            }
          else
            {
              // Page is dirty. Try some other page.
              // (2.2.1);
              pagedir_set_dirty (ee->thread->pagedir, ee->user_addr, false);
              lru_use (&pages_lru, &ee->lru_elem);
              ee->type = VMPPT_USED;
              // (2.2.2);
              bool swap_result UNUSED;
              swap_result = swap_dispose (ee->thread, ee->user_addr, 1);
              ASSERT (swap_result);
              // (2.2.3);
              continue;
            }
        }
      
      break;
    }
  
  // (3)
  pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
  lru_dispose (&pages_lru, &ee->lru_elem, false);
  ee->type = VMPPT_SWAPPED;
  
  // (4)
  intr_enable();
  result = swap_alloc_and_write (ee->thread, ee->user_addr, kpage, PGSIZE);
  intr_disable ();
  
  if (result) // (5.1.1)
    palloc_free_page (kpage);
  else // (5.2.1)
    pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage, true);
  
end:
  intr_enable();
  return result;
}

static bool
swap_free_memory (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  
  size_t freed = 0;
  while (freed < SWAP_AT_ONCE && !lru_is_empty (&pages_lru))
    {
      if (swap_free_page ())
        ++freed;
      else
        break;
    }
  return freed > 0;
}

static bool
vm_real_alloc (struct vm_logical_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->user_addr != NULL);
  ASSERT (ee->thread != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  void *kpage;
  for (;;)
    {
      kpage = palloc_get_page (PAL_USER|PAL_ZERO);
      if (kpage != NULL)
        break;
        
      if (!swap_free_memory ())
        return false;
    }
    
  bool result;
  result = pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage, true);
  ASSERT (result == true);
  
  return result;
}

static bool
vm_swap_in (struct vm_logical_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->user_addr != NULL);
  ASSERT (ee->thread != NULL);

  // TODO

  return false;
}

enum vm_ensure_result
vm_ensure (struct thread *t, void *base)
{
  assert_t_addr (t, base);
  ASSERT (intr_get_level () == INTR_ON);
  
  lock_acquire (&vm_lock);
  
  enum vm_ensure_result result;
  if (base == NULL)
    {
      result = VMER_SEGV;
      goto end;
    }
  
  if (pagedir_get_page (t->pagedir, base) != NULL)
    {
      result = VMER_OK;
      goto end;
    }
    
  struct vm_logical_page *ee = vm_get_logical_page (t, base);
  if (ee == NULL)
    {
      result = VMER_SEGV;
      goto end;
    }
  
  switch (ee->type)
    {
      case VMPPT_UNUSED:
        result = VMER_SEGV;
        break;
        
      case VMPPT_EMPTY:
        result = vm_real_alloc (ee) ? VMER_OK : VMER_OOM;
        break;
        
      case VMPPT_SWAPPED:
        result = vm_swap_in (ee) ? VMER_OK : VMER_OOM;
        break;
      
      case VMPPT_USED:
      default:
        // VMPPT_USED implies pagedir_get_page != NULL
        ASSERT (0);
    }
  lru_use (&pages_lru, &ee->lru_elem);
    
end:
  lock_release (&vm_lock);
  return result;
}

void
vm_dispose (struct thread *t, void *addr)
{
  assert_t_addr (t, addr);
  
  // TODO
  (void) t;
  (void) addr;
}

bool
vm_alloc_and_ensure (struct thread *t, void *addr)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_ON);
  
  if (!vm_alloc_zero (t, addr))
    return false;
    
  enum vm_ensure_result result = vm_ensure (t, addr);
  if (result == VMER_OK)
    return true;
    
  ASSERT (result == VMER_OOM);
  vm_dispose (t, addr);
  return false;
}
