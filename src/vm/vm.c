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

#define VMLP_MAGIC (('V'<<16) + ('L'<<8) + 'P')
typedef char _CASSERT_VMLP_MAGIC24[0 - !(VMLP_MAGIC < (1<<24))];
typedef char _CASSERT_VMPPT_SIZE[0 - !(VMPPT_COUNT < (1<<7))];

static bool vm_is_initialized;
static struct lru pages_lru;
static struct lock vm_lock;

static inline void
assert_t_addr (struct thread *t UNUSED, const void *addr UNUSED)
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
vmlp_entry (const struct hash_elem *e, void *t UNUSED)
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
  if (ee->thread)
    hash_delete (&ee->thread->vm_pages, &ee->thread_elem);
  
  switch (ee->type)
    {
    case VMPPT_USED:
    case VMPPT_EMPTY:
    case VMPPT_MMAP_ALIAS:
    case VMPPT_MMAP_KPAGE:
      break;
      
    case VMPPT_SWAPPED:
      (void) swap_dispose (ee->thread, ee->user_addr);
      break;
    
    default:
      PANIC ("ee->type == %d", ee->type);
    }
    
  if (ee->thread)
    {
      void *kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
      if (kpage != NULL)
        {
          pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
          if (ee->type != VMPPT_MMAP_ALIAS)
            palloc_free_page (kpage);
        }
    }
  else
    {
      ASSERT (ee->type == VMPPT_MMAP_KPAGE);
      palloc_free_page (ee->user_addr);
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

static bool
vm_alloc_zero_real (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  //printf ("   ALLOC ZERO: %8p\n", addr);
  
  struct vm_page *page = calloc (1, sizeof (*page));
  if (!page)
    return false;
    
  page->type       = VMPPT_EMPTY;
  page->thread     = t;
  page->user_addr  = addr;
  page->vmlp_magic = VMLP_MAGIC;
  page->readonly   = !!readonly;
  hash_insert (&t->vm_pages, &page->thread_elem);
  
  return true;
}

bool
vm_alloc_zero (struct thread *t, void *addr, bool readonly)
{
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  bool result = vm_alloc_zero_real (t, addr, readonly);
  
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

void
vm_mmap_disposed (struct vm_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->type == VMPPT_MMAP_KPAGE || ee->type == VMPPT_MMAP_ALIAS);
  lock_held_by_current_thread (&vm_lock);
  ASSERT (intr_get_level () == INTR_OFF);
  
  vm_dispose_real (ee);
}

void
vm_kernel_wrote (struct thread *t, void *user_addr, size_t amount)
{
  ASSERT (t != NULL);
  ASSERT (user_addr != NULL);
  if (amount == 0)
    return;
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  uint8_t *start = pg_round_down (user_addr);
  uint8_t *end = pg_round_down (start + amount - 1);
  uint8_t *i;
  
  for (i = start; i <= end; i += PGSIZE)
    {
      struct vm_page *ee = vm_get_logical_page (t, i);
      ASSERT (ee != NULL);
      ASSERT (!lru_is_interior (&ee->lru_elem));
      
      switch (ee->type)
        {
        case VMPPT_EMPTY:
        case VMPPT_USED:
        case VMPPT_MMAP_KPAGE:
        case VMPPT_MMAP_ALIAS:
          break;
          
        case VMPPT_SWAPPED:
          swap_dispose (ee->thread, ee->user_addr);
          break;
          
        case VMPPT_UNUSED:
        case VMPPT_COUNT:
        default:
          PANIC ("ee->type == %d", ee->type);
        }
        
      ee->type = VMPPT_USED;
      pagedir_set_dirty (ee->thread->pagedir, ee->user_addr, true);
    }
    
  lock_release (&vm_lock);
  intr_set_level (old_level);
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
  if (!ee->thread)
    {
      ASSERT (ee->type == VMPPT_MMAP_KPAGE);
      // TODO: check refs
      return VMPU_CLEAR;
    }
    
  if (!ee->readonly && pagedir_is_dirty (ee->thread->pagedir, ee->user_addr))
    {
      switch (ee->type)
        {
        case VMPPT_EMPTY:
          ee->type = VMPPT_USED;
          break;
          
        case VMPPT_USED:
          break;
          
        case VMPPT_SWAPPED:
          swap_dispose (ee->thread, ee->user_addr);
          break;
          
        case VMPPT_MMAP_ALIAS:
          {
            struct mmap_upage *mmap_upage = mmap_retreive_upage (ee);
            ASSERT (mmap_upage != NULL);
            ASSERT (mmap_upage->kpage != NULL);
            mmap_upage->kpage->dirty = true;
            break;
          }
        
        case VMPPT_MMAP_KPAGE:
        case VMPPT_UNUSED:
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
  
  if (ee->type != VMPPT_MMAP_ALIAS)
    lru_use (&pages_lru, &ee->lru_elem);
  else
    {
      struct mmap_upage *mmap_upage = mmap_retreive_upage (ee);
      ASSERT (mmap_upage != NULL);
      ASSERT (mmap_upage->kpage != NULL);
      if (lru_is_interior (&mmap_upage->kpage->kernel_page->lru_elem))
        lru_use (&pages_lru, &mmap_upage->kpage->kernel_page->lru_elem);
    }
  
  return result;
}

static void
vm_tick_sub (struct hash_elem *e, void *t UNUSED)
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
  if (!lock_try_acquire (&vm_lock))
    return;
    
  hash_apply (&t->vm_pages, &vm_tick_sub);
  lock_release (&vm_lock);
}

static struct vm_page *
vm_mmap_evict_real (struct mmap_kpage *kpage)
{
  ASSERT (kpage != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  bool dirty = kpage->dirty;
  while (!list_empty (&kpage->upages))
    {
      struct list_elem *e = list_pop_front (&kpage->upages);
      struct mmap_upage *ee = list_entry (e, struct mmap_upage, kpage_elem);
      ee->kpage = NULL;
      if (!dirty)
        dirty = pagedir_is_dirty (ee->vm_page->thread->pagedir,
                                  ee->vm_page->user_addr);
    }
  
  if (kpage->dirty)
    {
      intr_enable ();
      mmap_write_kpage (kpage);
      intr_disable ();
    }
    
  hash_delete (&kpage->region->kpages, &kpage->region_elem);
  
  struct vm_page *result = kpage->kernel_page;
  lru_dispose (&pages_lru, &result->lru_elem, false);
  ASSERT (result->thread == NULL);
  free (kpage);
  return result;
}

void
vm_mmap_evicting (struct mmap_kpage *kpage)
{
  ASSERT (kpage != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  struct vm_page *vm_page = vm_mmap_evict_real (kpage);
  vm_dispose_real (vm_page);
  
  lock_release (&vm_lock);
  intr_enable ();
}

static void *
vm_free_a_page (void)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  intr_disable ();
  
  bool result = false;
  void *kpage = NULL;
  int retry;
  for (retry = 0; retry < 32; ++retry)
    {
      struct lru_elem *e = lru_peek_least (&pages_lru);
      if (!e)
        break;
      struct vm_page *ee = lru_entry (e, struct vm_page, lru_elem);
      if (vm_handle_page_usage (ee) != VMPU_CLEAR)
        continue;
        
      if (ee->thread)
        kpage = pagedir_get_page (ee->thread->pagedir, ee->user_addr);
      else
        {
          ASSERT (ee->type == VMPPT_MMAP_KPAGE);
          kpage = ee->user_addr;
        }
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
          
        case VMPPT_SWAPPED:
          {
            result = swap_must_retain (ee->thread, ee->user_addr);
            if (result)
              {
                lru_dispose (&pages_lru, &ee->lru_elem, false);
                pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
                break;
              }
            ee->type = VMPPT_UNUSED;
            // fallthrough
          }
          
        case VMPPT_USED:
          {
            pagedir_clear_page (ee->thread->pagedir, ee->user_addr);
            ee->type = VMPPT_SWAPPED;
            
            intr_enable ();
            result = swap_alloc_and_write (ee->thread, ee->user_addr, kpage);
            ASSERT (result);
            intr_disable ();
            
            if (result)
              lru_dispose (&pages_lru, &ee->lru_elem, false);
            else
              {
                lru_use (&pages_lru, &ee->lru_elem);
                pagedir_set_page (ee->thread->pagedir, ee->user_addr, kpage,
                                  true);
                ee->type = VMPPT_USED;
                continue;
              }
            break;
          }
          
        case VMPPT_MMAP_KPAGE:
          {
            ASSERT (0);
            // TODO
            break;
          }
          
        case VMPPT_MMAP_ALIAS:
        case VMPPT_UNUSED:
        case VMPPT_COUNT:
        default:
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
              return NULL;
            }
          palloc_free_page (kpage);
        }
    }
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

static enum vm_ensure_result
vm_ensure_mmap_alias (struct vm_page *vm_page, void **kpage_)
{
  ASSERT (vm_page != NULL);
  ASSERT (vm_page->type == VMPPT_MMAP_ALIAS);
  ASSERT (kpage_ != NULL);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_ON);
  
  struct mmap_upage *mmap_upage = mmap_retreive_upage (vm_page);
  ASSERT (mmap_upage != NULL);
  
  if (mmap_upage->kpage != NULL || mmap_assign_kpage (mmap_upage))
    {
      ASSERT (mmap_upage->kpage->kernel_page != NULL);
      *kpage_ = mmap_upage->kpage->kernel_page;
      return VMER_OK;
    }
  
  *kpage_ = vm_alloc_kpage (vm_page);
  if (!*kpage_)
    {
      *kpage_ = NULL;
      return VMER_OOM;
    }
    
  struct vm_page *kernel_page = calloc (1, sizeof (*kernel_page));
  if (!kernel_page)
    {
      palloc_free_page (*kpage_);
      *kpage_ = NULL;
      return VMER_OOM;
    }
  
  kernel_page->user_addr  = *kpage_;
  kernel_page->vmlp_magic = VMLP_MAGIC;
  kernel_page->type       = VMPPT_MMAP_KPAGE;
    
  if (!mmap_load_kpage (mmap_upage, kernel_page))
    {
      free (kernel_page);
      palloc_free_page (*kpage_);
      *kpage_ = NULL;
      return VMER_SEGV;
    }
  ASSERT (mmap_upage->kpage != NULL);
    
  lru_use (&pages_lru, &kernel_page->lru_elem);
  return true;
}

enum vm_ensure_result
vm_ensure (struct thread *t, void *user_addr, void **kpage_)
{
  ASSERT (t != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
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
    
  if (ee->type == VMPPT_MMAP_ALIAS)
    {
      result = vm_ensure_mmap_alias (ee, kpage_);
      if (!outer_lock)
        lock_release (&vm_lock);
      return result;
    }
  
  *kpage_ = vm_alloc_kpage (ee);
  if (!*kpage_)
    {
      result = VMER_OOM;
      goto end;
    }
  
  switch (ee->type)
    {
      case VMPPT_EMPTY:
        memset (*kpage_, 0, PGSIZE);
        result = VMER_OK;
        lru_use (&pages_lru, &ee->lru_elem);
        break;
        
      case VMPPT_SWAPPED:
        if (swap_read_and_retain (ee->thread, ee->user_addr, *kpage_))
          {
            result = VMER_OK;
            lru_use (&pages_lru, &ee->lru_elem);
          }
        else
          {
            result = VMER_SEGV;
          }
        break;
        
      case VMPPT_USED: // implies pagedir_get_page != NULL
      case VMPPT_UNUSED:
      case VMPPT_MMAP_KPAGE:
      case VMPPT_MMAP_ALIAS:
      case VMPPT_COUNT:
      default:
        PANIC ("ee->type == %d", ee->type);
    }
    
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

static void
vm_dispose_real2 (struct thread *t, void *addr)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  struct vm_page *ee = vm_get_logical_page (t, addr);
  vm_dispose_real (ee);
}

void
vm_dispose (struct thread *t, void *addr)
{
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  vm_dispose_real2 (t, addr);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
}

static void *
vm_alloc_and_ensure_real (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  
  void *kpage = NULL;
  
  if (!vm_alloc_zero_real (t, addr, readonly))
    goto end;
  
  intr_enable ();
  enum vm_ensure_result result = vm_ensure (t, addr, &kpage);
  if (result == VMER_OK)
    goto end;
    
  ASSERT (result == VMER_OOM);
  intr_disable ();
  vm_dispose_real2 (t, addr);
  
end:
  return kpage;
}

void *
vm_alloc_and_ensure (struct thread *t, void *addr, bool readonly)
{
  assert_t_addr (t, addr);
  ASSERT (intr_get_level () == INTR_ON);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  void *result = vm_alloc_and_ensure_real (t, addr, readonly);
  
  lock_release (&vm_lock);
  intr_enable ();
  return result;
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
  struct vm_page   *kernel_page;
  struct vm_page   *user_page;
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
  key.user_page = *page_;
  
  struct hash_elem *e = hash_find (&g->entries, &key.elem);
  return hash_entry (e, struct vm_ensure_group_entry, elem);
}

static unsigned
vm_ensure_group_hash (const struct hash_elem *e, void *t UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  
  ASSERT (e != NULL);
  struct vm_ensure_group_entry *ee;
  ee = hash_entry (e, struct vm_ensure_group_entry, elem);
  ASSERT (ee->user_page != NULL);
  ASSERT (ee->user_page->thread == t);
  
  return (unsigned) ee->user_page;
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
vm_ensure_group_dispose_real (struct hash_elem *e, void *t UNUSED)
{
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (intr_get_level () == INTR_OFF);
  
  ASSERT (e != NULL);
  struct vm_ensure_group_entry *ee;
  ee = hash_entry (e, struct vm_ensure_group_entry, elem);
  ASSERT (ee->user_page != NULL);
  ASSERT (ee->user_page->thread == t);
  ASSERT (ee->user_page->vmlp_magic == VMLP_MAGIC);
  ASSERT (ee->kernel_page != NULL);
  ASSERT (ee->kernel_page->vmlp_magic == VMLP_MAGIC);
  
  lru_use (&pages_lru, &ee->kernel_page->lru_elem);
    
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
  if (result == VMER_SEGV && vm_is_valid_stack_addr (g->esp, user_addr))
    {
      intr_disable ();
      *kpage_ = vm_alloc_and_ensure_real (g->thread, pg_round_down (user_addr),
                                          false);
      intr_enable ();
      
      result = *kpage_ ? VMER_OK : VMER_OOM;
    }
  if (result != VMER_OK)
    goto end2;
  
  enum intr_level old_level = intr_disable ();
  
  struct vm_page *user_page, *kernel_page;
  struct vm_ensure_group_entry *entry;
  entry = vm_ensure_group_get (g, pg_round_down (user_addr), &user_page);
  if (entry != NULL)
    goto end;
  else if (user_page == NULL)
    {
      result = VMER_SEGV;
      goto end;
    }
  
  if (user_page->type == VMPPT_MMAP_ALIAS)
    {
      struct mmap_upage *upage = mmap_retreive_upage (user_page);
      ASSERT (upage != NULL);
      ASSERT (upage->kpage != NULL);
      ASSERT (upage->kpage->kernel_page != NULL);
      kernel_page = upage->kpage->kernel_page;
    }
  else
    kernel_page = user_page;
  
  if (lru_is_interior (&kernel_page->lru_elem))
    {
      ASSERT (kernel_page->type == VMPPT_MMAP_KPAGE ?
          user_page->type == VMPPT_MMAP_ALIAS :
          user_page->type != VMPPT_MMAP_ALIAS);
          
      entry = calloc (1, sizeof (*entry));
      if (entry == NULL)
        {
          result = VMER_OOM;
          goto end;
        }
      entry->user_page = user_page;
      entry->kernel_page = kernel_page;
      struct hash_elem *e UNUSED = hash_insert (&g->entries, &entry->elem);
      ASSERT (e == NULL);
      lru_dispose (&pages_lru, &kernel_page->lru_elem, false);
    }
    
end:
  intr_set_level (old_level);
end2:
  ASSERT (result == VMER_OK ? *kpage_ != NULL : true);
  ASSERT (result != VMER_OK ? *kpage_ == NULL : true);
  lock_release (&vm_lock);
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
  
  struct vm_page *ee = vm_get_logical_page (g->thread,
                                            pg_round_down (user_addr));
  
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

mapid_t
vm_mmap_acquire (struct thread *owner, struct pifs_inode *inode)
{
  ASSERT (owner != NULL);
  ASSERT (inode != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  mapid_t result = mmap_alias_acquire (owner, inode);
  
  lock_release (&vm_lock);
  intr_set_level (old_level);
  
  return result;
}

void
vm_mmap_dispose2 (struct mmap_alias *alias)
{
  ASSERT (alias != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  lock_acquire (&vm_lock);
  mmap_alias_dispose (NULL, alias);
  lock_release (&vm_lock);
}

bool
vm_mmap_dispose (struct thread *owner, mapid_t id)
{
  ASSERT (owner != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  struct mmap_alias *alias = mmap_retreive_alias (owner, id);
  if (alias != NULL)
    {
      intr_enable ();
      mmap_alias_dispose (owner, alias);
      lock_release (&vm_lock);
      return true;
    }
  else
    {
      lock_release (&vm_lock);
      intr_enable ();
      return false;
    }
}

bool
vm_mmap_page (struct thread *owner, mapid_t id, void *base, size_t nth_page)
{
  ASSERT (owner != NULL);
  ASSERT (id != MAP_FAILED);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  ASSERT (intr_get_level () == INTR_ON);
  
  bool result = false;
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  struct vm_page *ee = vm_get_logical_page (owner, base);
  if (ee != NULL)
    goto end;
  
  struct mmap_alias *alias = mmap_retreive_alias (owner, id);
  if (!alias || nth_page >= mmap_alias_pages_count (alias))
    goto end;
  
  struct vm_page *page = calloc (1, sizeof (*page));
  if (!page)
    goto end;
  page->type       = VMPPT_MMAP_ALIAS;
  page->thread     = owner;
  page->user_addr  = base;
  page->vmlp_magic = VMLP_MAGIC;
  page->readonly   = false;
  
  if (!mmap_alias_map_upage (alias, page, nth_page))
    {
      free (page);
      goto end;
    }
  
  struct hash_elem *e UNUSED;
  e = hash_insert (&owner->vm_pages, &page->thread_elem);
  ASSERT (e == NULL);
  
  result = true;
  
end:
  lock_release (&vm_lock);
  intr_set_level (old_level);
  return result;
}

bool
vm_mmap_pages (struct thread *owner, mapid_t id, void *base)
{
  ASSERT (owner != NULL);
  ASSERT (id != MAP_FAILED);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  ASSERT (intr_get_level () == INTR_ON);
  
  bool result = false;
  
  enum intr_level old_level;
  lock_acquire2 (&vm_lock, &old_level);
  
  struct mmap_alias *alias = mmap_retreive_alias (owner, id);
  if (!alias)
    goto end;
  size_t pages_count = mmap_alias_pages_count (alias);
  
  size_t i;
  for (i = 0; i < pages_count; ++i)
    {
      struct vm_page *ee = vm_get_logical_page (owner, base + i*PGSIZE);
      if (ee != NULL)
        goto end;
        
      struct vm_page *page = calloc (1, sizeof (*page));
      if (!page)
        goto end;
      page->type       = VMPPT_MMAP_ALIAS;
      page->thread     = owner;
      page->user_addr  = base + i*PGSIZE;
      page->vmlp_magic = VMLP_MAGIC;
      page->readonly   = false;
      
      if (!mmap_alias_map_upage (alias, page, i))
        {
          free (page);
          goto end;
        }
      
      struct hash_elem *e UNUSED;
      e = hash_insert (&owner->vm_pages, &page->thread_elem);
      ASSERT (e == NULL);
    }
  result = true;
  
end:
  lock_release (&vm_lock);
  intr_set_level (old_level);
  return result;
}

void
vm_mmap_dispose_real (struct vm_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (ee->type == VMPPT_MMAP_KPAGE ||
          ee->type == VMPPT_MMAP_ALIAS);
  
  enum intr_level old_level = intr_disable ();
  if (ee->type == VMPPT_MMAP_ALIAS && pagedir_is_dirty (ee->thread->pagedir,
                                                        ee->user_addr))
    {
      struct mmap_upage *upage = mmap_retreive_upage (ee);
      if (upage->kpage)
        upage->kpage->dirty = true;
    }
  vm_dispose_real (ee);
  intr_set_level (old_level);
}
