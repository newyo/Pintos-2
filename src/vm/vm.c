#include "vm.h"
#include <hash.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "lru.h"
#include "swap.h"
#include "mmap.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"

#define MIN_ALLOC_ADDR ((void *) (1<<16))
#define SWAP_PERCENT_AT_ONCE 3

#define VMLP_MAGIC (('V'<<16) + ('L'<<8) + 'P')
typedef char _CASSERT_VMLP_MAGIC24[0 - !(VMLP_MAGIC < (1<<24))];

enum vm_page_type
{
  VMPPT_UNUSED = 0,   // this physical page is not used, yet
  
  VMPPT_EMPTY,        // read from, but never written to -> all zeros
  VMPPT_USED,         // allocated, no swap file equivalent
  VMPPT_SWAPPED,      // retreived from swap and not dirty or disposed
                      // OR removed from RAM
  
  VMPPT_COUNT
};
typedef char _CASSERT_VMPPT_SIZE[0 - !(VMPPT_COUNT < (1<<7))];

struct vm_page
{
  void                *user_addr;   // virtual address
  struct thread       *thread;      // owner thread
  struct hash_elem     thread_elem; // for thread.vm_pages
  struct lru_elem      lru_elem;    // for pages_lru
  
  struct
  {
    uint32_t           vmlp_magic :24;
    bool               readonly   :1;
    enum vm_page_type  type       :7;
  };
};

static bool vm_is_initialized;
static struct lru pages_lru;
static struct lock vm_lock;

static void
assert_t_addr (struct thread *t, const void *addr)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  ASSERT (addr >= MIN_ALLOC_ADDR);
  ASSERT (is_user_vaddr (addr));
  ASSERT (pg_ofs (addr) == 0);
}

void
vm_init (void)
{
  ASSERT (!vm_is_initialized);
  
  lru_init (&pages_lru, 0, NULL, NULL);
  lock_init (&vm_lock);
  
  size_t user_pool_size;
  palloc_fill_ratio (NULL, NULL, NULL, &user_pool_size);
  
  vm_is_initialized = true;
  
  printf ("Initialized user's virtual memory.\n");
}

static inline struct vm_page *
vmlp_entry (const struct hash_elem *e, void *t)
{
  if (e == NULL)
    return NULL;
  struct vm_page *ee = hash_entry (e, struct vm_page, thread_elem);
  ASSERT (t == NULL || ee->thread == t);
  ASSERT (ee->vmlp_magic == VMLP_MAGIC);
  return ee;
}

static unsigned
vm_thread_page_hash (const struct hash_elem *e, void *t)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  
  ASSERT (t != NULL);
  return (unsigned) vmlp_entry (e,t)->user_addr;
}

static bool
vm_thread_page_less (const struct hash_elem *a,
                     const struct hash_elem *b,
                     void *t)
{
  return vm_thread_page_hash (a,t) < vm_thread_page_hash (b,t);
}

void
vm_init_thread (struct thread *t)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  //printf ("   INITIALISIERE VM FÜR %8p.\n", t);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  swap_init_thread (t);
  hash_init (&t->vm_pages, &vm_thread_page_hash, &vm_thread_page_less, t);
  mmap_init_thread (t);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
}

static void
vm_dispose_real (struct vm_page *ee)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (ee != NULL);
  ASSERT (ee->vmlp_magic == VMLP_MAGIC);
  
  lru_dispose (&pages_lru, &ee->lru_elem, false);
  hash_delete (&ee->thread->vm_pages, &ee->thread_elem);
  
  switch (ee->type)
    {
    case VMPPT_USED:
    case VMPPT_EMPTY:
      break;
      
    case VMPPT_SWAPPED:
      (void) swap_dispose (ee->thread, ee->user_addr);
      break;
      
    default:
      PANIC ("ee->type == %d", ee->type);
    }
    
  void *kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
  if (kpage != NULL)
    {
      pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
      palloc_free_page (kpage);
    }
  
  free (ee);
}

static void
vm_clean_sub (struct hash_elem *e, void *t)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (e != NULL);
  
  struct vm_page *ee = vmlp_entry (e,t);
  
  vm_dispose_real (ee);
}

void
vm_clean (struct thread *t)
{
  ASSERT (vm_is_initialized);
  ASSERT (t != NULL);
  //printf ("   CLEANE VM VON %8p.\n", t);
    
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  mmap_clean (t);
  hash_destroy (&t->vm_pages, &vm_clean_sub);
  swap_clean (t);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
}

bool
vm_alloc_zero (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  
  bool result;
  
  // TODO: my have race conditions
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  //printf ("   ALLOC ZERO: %8p\n", addr);
  
  struct vm_page *page = calloc (1, sizeof (*page));
  if (!page)
    {
      result = false;
      goto end;
    }
    
  page->type       = VMPPT_EMPTY;
  page->thread     = t;
  page->user_addr  = addr;
  page->vmlp_magic = VMLP_MAGIC;
  page->readonly   = !!readonly;
  hash_insert (&t->vm_pages, &page->thread_elem);
  
  result = true;
  
end:
  if (!outer_lock)
    lock_release (&vm_lock);
  intr_set_level (old_level);
  return result;
}

static struct vm_page *
vm_get_logical_page (struct thread *t, void *user_addr)
{
  assert_t_addr (t, user_addr);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  struct vm_page key;
  key.user_addr = user_addr;
  key.thread = t;
  key.vmlp_magic = VMLP_MAGIC;
  
  struct hash_elem *e = hash_find (&t->vm_pages, &key.thread_elem);
  return vmlp_entry (e,t);
}

/* Called when swap needed room and disposed an unchanged page.
 * Not called when disposal was initiated through swap_dispose|swap_clean.
 * Called once per disposed page.
 * Might be called with interrupts on!
 */
void
vm_swap_disposed (struct thread *t, void *user_addr)
{
  assert_t_addr (t, user_addr);
  lock_held_by_current_thread (&vm_lock);
  
  // With being inside swaps lock, page cannot have been free'd.
  // No need to disable interrupts.
  struct vm_page *ee = vm_get_logical_page (t, user_addr);
  ASSERT (ee != NULL);
  
  ASSERT (ee->type == VMPPT_SWAPPED);
  ee->type = VMPPT_USED;
}

enum vm_page_usage
{
  VMPU_CLEAR,
  VMPU_ACCESSED,
  VMPU_DIRTY,
};

static enum vm_page_usage
vm_handle_page_usage (struct vm_page *ee)
{
  ASSERT (intr_get_level () == INTR_OFF);
    
  if (!lru_is_interior (&ee->lru_elem))
    return VMPU_CLEAR;
  
  enum vm_page_usage result;
  if (!ee->readonly && pagedir_is_dirty (ee->thread->pagedir, ee->user_addr))
    {
      switch (ee->type)
        {
        case VMPPT_UNUSED:
          PANIC ("ee->type == VMPPT_UNUSED");
          
        case VMPPT_EMPTY:
          ee->type = VMPPT_USED;
          break;
          
        case VMPPT_USED:
          break;
          
        case VMPPT_SWAPPED:
          swap_dispose (ee->thread, ee->user_addr);
          break;
              
        case VMPPT_COUNT:
        default:
          PANIC ("ee->type == %d", ee->type);
        }
      pagedir_set_accessed (ee->thread->pagedir, ee->user_addr, false);
      pagedir_set_dirty (ee->thread->pagedir, ee->user_addr, false);
      result = VMPU_DIRTY;
    }
  else if (pagedir_is_accessed (ee->thread->pagedir, ee->user_addr))
    {
      pagedir_set_accessed (ee->thread->pagedir, ee->user_addr, false);
      result = VMPU_ACCESSED;
    }
  else
    return VMPU_CLEAR;
  
  lru_use (&pages_lru, &ee->lru_elem);
  
  return result;
}

static void
vm_tick_sub (struct hash_elem *e, void *t)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (e != NULL);
  ASSERT (t != NULL);
  
  struct vm_page *ee = hash_entry (e, struct vm_page, thread_elem);
  ASSERT (ee->thread == t);
  vm_handle_page_usage (ee);
}

void
vm_tick (struct thread *t)
{
  ASSERT (t != NULL);
  ASSERT (t->pagedir != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  if (!vm_is_initialized || lock_held_by_current_thread (&vm_lock))
    return;
  
  if (lock_try_acquire (&vm_lock))
    {
      hash_apply (&t->vm_pages, &vm_tick_sub);
      lock_release (&vm_lock);
    }
}

static void *
vm_free_a_page (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  intr_disable ();
  
  bool result;
  void *kpage = NULL;
  for (;;)
    {
      struct lru_elem *e = lru_peek_least (&pages_lru);
      ASSERT (e != NULL); // TODO: remove line
      if (!e)
        {
          result = false;
          break;
        }
      struct vm_page *ee = lru_entry (e, struct vm_page, lru_elem);
      if (vm_handle_page_usage (ee) != VMPU_CLEAR)
        continue;
        
      kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
      ASSERT (kpage != NULL);
      
      switch (ee->type)
        {
        case VMPPT_EMPTY:
          {
            lru_dispose (&pages_lru, &ee->lru_elem, false);
            pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
            result = true;
            break;
          }
          
        case VMPPT_USED:
          {
            pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
            ee->type = VMPPT_SWAPPED;
            
            intr_enable ();
            result = swap_alloc_and_write (ee->thread, ee->user_addr, kpage);
            intr_disable ();
            
            if (result)
              lru_dispose (&pages_lru, &ee->lru_elem, false);
            else
              {
                pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage,
                                  true);
                ee->type = VMPPT_USED;
              }
            break;
          }
          
        case VMPPT_SWAPPED:
          {
            result = swap_must_retain (ee->thread, ee->user_addr);
            if (result)
              {
                lru_dispose (&pages_lru, &ee->lru_elem, false);
                pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
              }
            break;
          }
          
        case VMPPT_UNUSED:
        case VMPPT_COUNT:
        default:
          result = false;
          PANIC ("ee->type == %d", ee->type);
        }
      break;
    }
    
  intr_enable();
  return result ? kpage : NULL;
}

static void *
vm_alloc_kpage (struct vm_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->user_addr != NULL);
  ASSERT (ee->thread != NULL);
  ASSERT (ee->vmlp_magic == VMLP_MAGIC);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (pagedir_get_page (ee->thread->pagedir, ee->user_addr) == NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  int i, j;
  for (i = 0; i < 3; ++i)
    {
      void *kpage = vm_palloc ();
      if (kpage != NULL)
        {
          if (pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage,
                                !ee->readonly))
            {
              lru_use (&pages_lru, &ee->lru_elem);
              return kpage;
            }
          palloc_free_page (kpage);
        }
      for (j = 0; j < 5; ++j)
        {
          kpage = vm_free_a_page ();
          if (kpage == NULL)
            {
              ASSERT (0); // TODO: remove line
              return NULL;
            }
          palloc_free_page (kpage);
        }
    }
  ASSERT (0); // TODO: remove line
  return NULL;
}

void *
vm_palloc (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  
  void *kpage = palloc_get_page (PAL_USER);
  if (kpage == NULL)
    kpage = vm_free_a_page ();
    
  if (!outer_lock)
    lock_release (&vm_lock);
  return kpage;
}

enum vm_ensure_result
vm_ensure (struct thread *t, void *user_addr, void **kpage_)
{
  ASSERT (kpage_ != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  if (user_addr < MIN_ALLOC_ADDR || !is_user_vaddr (user_addr))
    {
      *kpage_ = NULL;
      return VMER_SEGV;
    }
    
  assert_t_addr (t, user_addr);
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  
  enum vm_ensure_result result;
    
  struct vm_page *ee = vm_get_logical_page (t, user_addr);
  if (ee == NULL)
    {
      *kpage_ = NULL;
      result = VMER_SEGV;
      goto end;
    }
  
  *kpage_ = pagedir_get_page (t->pagedir, user_addr);
  if (*kpage_ != NULL)
    {
      result = VMER_OK;
      if (lru_is_interior (&ee->lru_elem))
        lru_use (&pages_lru, &ee->lru_elem);
      goto end;
    }
  
  *kpage_ = vm_alloc_kpage (ee);
  if (!*kpage_)
    {
      ASSERT (0); // TODO: remove line
      result = VMER_OOM;
      goto end;
    }
  
  
  switch (ee->type)
    {
      case VMPPT_EMPTY:
        memset (*kpage_, 0, PGSIZE);
        result = VMER_OK;
        break;
      
      case VMPPT_USED:
        // VMPPT_USED implies pagedir_get_page != NULL
        PANIC ("ee->type == VMPPT_USED, but pagedir_get_page (...) == NULL");
        
      case VMPPT_SWAPPED:
        {
          if (swap_read_and_retain (ee->thread, ee->user_addr, *kpage_))
            result = VMER_OK;
          else
            result = VMER_SEGV;
          break;
        }
        
      case VMPPT_UNUSED:
      case VMPPT_COUNT:
      default:
        PANIC ("ee->type == %d", ee->type);
    }
  lru_use (&pages_lru, &ee->lru_elem);
    
end:
  if (result == VMER_OK)
    {
      ASSERT (*kpage_ != NULL);
    }
  else
    {
      palloc_free_page (*kpage_);
      *kpage_ = NULL;
    }
    
  if (!outer_lock)
    lock_release (&vm_lock);
  return result;
}

void
vm_dispose (struct thread *t, void *addr)
{
  assert_t_addr (t, addr);
  
  // TODO: may have race conditions
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  enum intr_level old_level = intr_disable ();
  
  struct vm_page *ee = vm_get_logical_page (t, addr);
  vm_dispose_real (ee);
  
  if (!outer_lock)
    lock_release (&vm_lock);
  intr_set_level (old_level);
}

void *
vm_alloc_and_ensure (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_ON);
  
  void *kpage = NULL;
  
  bool outer_lock = lock_held_by_current_thread (&vm_lock);
  if (!outer_lock)
    lock_acquire (&vm_lock);
  
  if (!vm_alloc_zero (t, addr, readonly))
    goto end;
  
  enum vm_ensure_result result = vm_ensure (t, addr, &kpage);
  if (result == VMER_OK)
    goto end;
    
  ASSERT (result == VMER_OOM);
  vm_dispose (t, addr);
  
end:
  if (!outer_lock)
    lock_release (&vm_lock);
  return kpage;
}

enum vm_is_readonly_result
vm_is_readonly (struct thread *t, void *user_addr)
{
  ASSERT (t != NULL);
  if (user_addr < MIN_ALLOC_ADDR || !is_user_vaddr (user_addr))
    return VMER_SEGV;
  
  struct vm_page *ee = vm_get_logical_page (t, user_addr);
    
  lock_release (&vm_lock);
  return ee ? ee->readonly ? VMIR_READONLY : VMIR_READWRITE : VMIR_INVALID;
}

struct vm_ensure_group_entry
{
  struct hash_elem  elem;
  struct vm_page   *page;
};

static struct vm_ensure_group_entry *
vm_ensure_group_get (struct vm_ensure_group  *g,
                     void                    *user_addr,
                     struct vm_page         **page_)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (g != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (page_ != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  
  *page_ = vm_get_logical_page (g->thread, user_addr);
  if (!*page_)
    return NULL;
  
  struct vm_ensure_group_entry key;
  memset (&key, 0, sizeof (key));
  key.page = *page_;
  
  struct hash_elem *e = hash_find (&g->entries, &key.elem);
  return hash_entry (e, struct vm_ensure_group_entry, elem);
}

static unsigned
vm_ensure_group_hash (const struct hash_elem *e, void *t)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  
  ASSERT (e != NULL);
  struct vm_ensure_group_entry *ee;
  ee = hash_entry (e, struct vm_ensure_group_entry, elem);
  ASSERT (ee->page != NULL);
  ASSERT (ee->page->thread == t);
  
  return (unsigned) ee->page;
}

static bool
vm_ensure_group_less (const struct hash_elem *a,
                      const struct hash_elem *b,
                      void *t)
{
  return vm_ensure_group_hash (a,t) < vm_ensure_group_hash (b,t);
}

void
vm_ensure_group_init (struct vm_ensure_group *g, struct thread *t, void *esp)
{
  ASSERT (g != NULL);
  ASSERT (t != NULL);
  
  memset (g, 0, sizeof (*g));
  g->thread = t;
  g->esp = esp;
  hash_init (&g->entries, &vm_ensure_group_hash, &vm_ensure_group_less, t);
}

static void
vm_ensure_group_dispose_real (struct hash_elem *e, void *t)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  
  ASSERT (e != NULL);
  struct vm_ensure_group_entry *ee;
  ee = hash_entry (e, struct vm_ensure_group_entry, elem);
  ASSERT (ee->page != NULL);
  ASSERT (ee->page->thread == t);
  ASSERT (ee->page->vmlp_magic == VMLP_MAGIC);
  
  lru_use (&pages_lru, &ee->page->lru_elem);
    
  free (ee);
}

void
vm_ensure_group_destroy (struct vm_ensure_group *g)
{
  ASSERT (g != NULL);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  hash_destroy (&g->entries, &vm_ensure_group_dispose_real);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
}

#define MAX_STACK (8*1024*1024)

static inline bool __attribute__ ((const))
vm_is_stack_addr (void *user_addr)
{
  return (user_addr < PHYS_BASE) && (user_addr >= PHYS_BASE - MAX_STACK);
}

static inline bool __attribute__ ((const))
vm_is_valid_stack_addr (void *esp, void *user_addr)
{
  return vm_is_stack_addr (esp) &&
         (((esp    <= user_addr) && vm_is_stack_addr (user_addr))    ||
          ((esp-4  == user_addr) && vm_is_stack_addr (user_addr-4))  ||
          ((esp-32 == user_addr) && vm_is_stack_addr (user_addr-32)));
}

enum vm_ensure_result
vm_ensure_group_add (struct vm_ensure_group *g, void *user_addr, void **kpage_)
{
  ASSERT (g != NULL);
  if (user_addr < MIN_ALLOC_ADDR || !is_user_vaddr (user_addr))
    return VMER_SEGV;
  
  lock_acquire (&vm_lock);
    
  enum vm_ensure_result result;
  result = vm_ensure (g->thread, pg_round_down (user_addr), kpage_);
  ASSERT (result != VMER_OOM); // TODO: remove line
  if (result == VMER_SEGV && vm_is_valid_stack_addr (g->esp, user_addr))
    {
      *kpage_ = vm_alloc_and_ensure (g->thread, pg_round_down (user_addr),
                                     false);
      result = *kpage_ ? VMER_OK : VMER_OOM;
    }
  if (result != VMER_OK)
    goto end2;
  
  enum intr_level old_level = intr_disable ();
  
  struct vm_page *page;
  struct vm_ensure_group_entry *entry;
  entry = vm_ensure_group_get (g, pg_round_down (user_addr), &page);
  if (entry != NULL)
    goto end;
  if (page == NULL)
    {
      result = VMER_SEGV;
      goto end;
    }
  if (lru_is_interior (&page->lru_elem))
    {
      entry = calloc (1, sizeof (*entry));
      entry->page = page;
      hash_insert (&g->entries, &entry->elem);
      lru_dispose (&pages_lru, &page->lru_elem, false);
    }
    
end:
  ASSERT (result == VMER_OK);
  intr_set_level (old_level);
end2:
  ASSERT (result == VMER_OK ? *kpage_ != NULL : true);
  ASSERT (result != VMER_OK ? *kpage_ == NULL : true);
  lock_release (&vm_lock);
  //printf ("(%d) kpage_ = %p -> %p\n", result, kpage_, *kpage_);
  ASSERT (result != VMER_OOM);
  return result;
}

bool
vm_ensure_group_remove (struct vm_ensure_group *g, void *user_addr)
{
  ASSERT (g != NULL);
  ASSERT (user_addr != NULL);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  struct vm_page *page;
  struct vm_ensure_group_entry *entry;
  entry = vm_ensure_group_get (g, user_addr, &page);
  if (entry != NULL)
    {
      hash_delete_found (&g->entries, &entry->elem);
      vm_ensure_group_dispose_real (&entry->elem, g->thread);
      lock_release (&vm_lock);
      intr_set_level (old_level);
      return true;
    }
    
  lock_release (&vm_lock);
  intr_set_level (old_level);
  return page != NULL;
}

enum vm_is_readonly_result
vm_ensure_group_is_readonly (struct vm_ensure_group *g, void *user_addr)
{
  ASSERT (g != NULL);
  if (user_addr < MIN_ALLOC_ADDR || !is_user_vaddr (user_addr))
    return VMER_SEGV;
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  struct vm_page *ee = vm_get_logical_page (g->thread, user_addr);
  
  enum vm_is_readonly_result result;
  if (ee)
    result = ee->readonly ? VMIR_READONLY : VMIR_READWRITE;
  else if (vm_is_valid_stack_addr (g->esp, user_addr))
    result = VMIR_READWRITE;
  else
    result = VMIR_INVALID;
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
  return result;
}
