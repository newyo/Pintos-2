#include "vm.h"
#include <hash.h>
#include <stddef.h>
#include <limits.h>
#include "lru.h"
#include "swap.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"

#define MIN_ALLOC_ADDR (1<<16)

enum vm_physical_page_type
{
  VMPPT_UNUSED = 0,   // this physical page is not used, yet
  
  VMPPT_EMPTY,        // read from, but never written to -> all zeros
  VMPPT_USED,         // allocated, no swap file equivalent
  VMPPT_SWAPPED_IN,   // retreived from swap and not dirty or disposed
  VMPPT_SWAPPED_OUT,  // removed from RAM
  
  VMPPT_COUNT
};

struct vm_logical_page
{
  void                       *user_addr;   // virtual address
  struct thread              *thread;      // owner thread
  struct hash_elem            thread_elem; // for thread.vm_pages
  struct lru_elem             lru_elem;    // for pages_lru
  swap_t                      swap_page;   // swapped from or to
  enum vm_physical_page_type  type;
};

static struct lru pages_lru;
static struct lock vm_lock;

void
vm_init (void)
{
  lru_init (&pages_lru, 0, NULL, NULL);
  lock_init (&vm_lock);
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
  hash_init (&t->swap_pages, &vm_thread_page_hash, &vm_thread_page_less, NULL);
}

void
vm_clean (struct thread *t)
{
  ASSERT (t != NULL);
  
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  // TODO
  
  intr_set_level (old_level);
  lock_release (&vm_lock);
}

bool
vm_alloc_zero (struct thread *t, void *addr)
{
  ASSERT ((uintptr_t) addr >= MIN_ALLOC_ADDR);
  ASSERT (pg_ofs (addr));
  
  lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  struct vm_logical_page *page = calloc (1, sizeof (struct vm_logical_page));
  if (!page)
    {
      intr_set_level (old_level);
      return false;
    }
    
  page->type      = VMPPT_EMPTY;
  page->thread    = t;
  page->user_addr = addr;
  page->swap_page = SWAP_FAIL;
  
  hash_insert (&t->swap_pages, &page->thread_elem);
  
  intr_set_level (old_level);
  lock_release (&vm_lock);
  return true;
}

static struct vm_logical_page *
vm_get_logical_page (struct thread *t, void *base)
{
  ASSERT (t != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  
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
  // Must not lock vm_lock, as vm_ensure could trigger swap disposal!
  
  // With being inside swaps lock, page cannot have been free'd.
  // No need to disable interrupts.
  struct vm_logical_page *ee = vm_get_logical_page (t, base);
  ASSERT (ee != NULL);
  
  if (ee->type != VMPPT_SWAPPED_IN)
    return;
  
  ee->type = VMPPT_USED;
  ee->swap_page = SWAP_FAIL;
}

void
vm_rescheduled (struct thread *from, struct thread *to UNUSED)
{
  ASSERT (from != NULL);
  lock_acquire (&vm_lock);
  
  // TODO: lru for dirty pages
  lock_release (&vm_lock);
}

static bool
vm_real_alloc (struct vm_logical_page *ee)
{
  // TODO: palloc and zero'd page, install it
  (void) ee;
  return false;
}

static bool
vm_swap_in (struct vm_logical_page *ee)
{
  // TODO
  (void) ee;
  return false;
}

enum vm_ensure_result
vm_ensure (struct thread *t, void *base)
{
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
        
      case VMPPT_SWAPPED_OUT:
        result = vm_swap_in (ee) ? VMER_OK : VMER_OOM;
        break;
      
      case VMPPT_USED:
      case VMPPT_SWAPPED_IN:
      default:
        // VMPPT_USED&VMPPT_SWAPPED_IN imply pagedir_get_page != NULL
        ASSERT (0);
    }
    
end:
  lock_release (&vm_lock);
  return result;
}
