#include "mmap.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <hash.h>
#include <list.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/interrupt.h"

struct mmap_region
{
  struct file      *file;
  size_t            length;
  struct list       refs;
  
  struct hash_elem  regions_elem;
};

struct mmap_alias
{
  mapid_t             id;
  struct mmap_region *ref;
  struct hash         upages;
  
  struct hash_elem    aliases_elem;
};

struct mmap_kpage
{
  void               *kernel_addr;
  struct mmap_region *region;
  size_t              page_num;
  bool                dirty;
  struct list         refs;
  
  struct hash_elem    kpages_elem;
};

struct mmap_upage
{
  struct vm_page    *upage;
  struct mmap_alias *ref;
  struct mmap_kpage *kpage;
};

enum mmap_writer_task_type
{
  MMWTT_READ = 1,
  MMWTT_WRITE,
};

struct mmap_writer_task
{
  struct mmap_kpage          *page;
  enum mmap_writer_task_type  type;
  struct list_elem            tasks_elem;
  struct semaphore           *sema;
  struct lock                *lock;
};

static struct lock mmap_filesys_lock;
static struct hash mmap_regions;
static struct hash mmap_kpages;

static struct semaphore mmap_writer_sema;
static struct lock      mmap_writer_lock;
static struct list      mmap_writer_tasks;
static tid_t            mmap_writer_thread;

static void
mmap_writer_write (struct mmap_kpage *page)
{
  ASSERT (page != NULL);
  ASSERT (intr_get_level () == INTR_ON);

  lock_acquire (&mmap_filesys_lock);
  
  struct mmap_region *r = page->region;
  ASSERT (r != NULL);
  
  size_t start = PGSIZE * page->page_num;
  off_t len = r->length >= start+PGSIZE ? PGSIZE : r->length - start;
  off_t wrote UNUSED;
  wrote = file_write_at (r->file, page->kernel_addr, len, start);
  ASSERT (wrote == len);
  
  lock_release (&mmap_filesys_lock);
}

static void
mmap_writer_read (struct mmap_kpage *page)
{
  ASSERT (page != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (lock_held_by_current_thread (&mmap_filesys_lock));

  lock_acquire (&mmap_filesys_lock);
  
  struct mmap_region *r = page->region;
  ASSERT (r != NULL);
  
  size_t start = PGSIZE * page->page_num;
  if (r->length >= start+PGSIZE)
    {
      off_t read UNUSED;
      read = file_read_at (r->file, page->kernel_addr, PGSIZE, start);
      ASSERT (read == PGSIZE);
    }
  else
    {
      off_t len = r->length >= start+PGSIZE ? PGSIZE : r->length - start;
      off_t read UNUSED;
      read = file_read_at (r->file, page->kernel_addr, len, start);
      ASSERT (read == len);
      memset (page->kernel_addr+len, 0, PGSIZE-len);
    }
    
  lock_release (&mmap_filesys_lock);
}

static void
mmap_writer_func (void *aux UNUSED)
{ 
  for(;;)
    {
      struct list task_list = LIST_INITIALIZER (task_list);
      sema_down (&mmap_writer_sema);
      
      lock_acquire (&mmap_writer_lock);
      enum intr_level old_level = intr_disable ();
      for (;;)
        {
          struct list_elem *e = list_pop_front (&mmap_writer_tasks);
          ASSERT (e != NULL);
          list_push_back (&task_list, e);
          
          if (!sema_try_down (&mmap_writer_sema))
            {
              ASSERT (list_empty (&mmap_writer_tasks))
              break;
            }
        }
      intr_set_level (old_level);
      lock_release (&mmap_writer_lock);
      
      while (!list_empty (&task_list))
        {
          struct list_elem *e = list_pop_front (&task_list);
          ASSERT (e != NULL);
          
          struct mmap_writer_task *task;
          task = list_entry (e, struct mmap_writer_task, tasks_elem);
          
          if (!task->page)
            {
              if (task->sema)
                sema_up (task->sema);
              free (task);
              return;
            }
          
          switch (task->type)
            {
            case MMWTT_READ:
              mmap_writer_read (task->page);
              break;
            case MMWTT_WRITE:
              mmap_writer_write (task->page);
              break;
            default:
              ASSERT (0);
            }
          
          if (task->sema)
            sema_up (task->sema);
          free (task);
        }
    }
}

static unsigned
mmap_region_hash (const struct hash_elem *e, void *aux UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (struct inode*))];
  ASSERT (e != NULL);
  
  struct mmap_region *ee = hash_entry (e, struct mmap_region, regions_elem);
  ASSERT (ee->file != NULL);
  
  struct inode *result = file_get_inode (ee->file);
  ASSERT (result != NULL);
  return (unsigned) result;
}

static bool
mmap_region_less (const struct hash_elem *a,
                  const struct hash_elem *b,
                  void *aux UNUSED)
{
  return mmap_region_hash (a, NULL) < mmap_region_hash (b, NULL);
}

static unsigned
mmap_kpage_hash (const struct hash_elem *e, void *aux UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void*))];
  ASSERT (e != NULL);
  
  struct mmap_kpage *ee = hash_entry (e, struct mmap_kpage, kpages_elem);
  ASSERT (ee->kernel_addr != NULL);
  return (unsigned) ee->kernel_addr;
}

static bool
mmap_kpage_less (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
  return mmap_kpage_hash (a, NULL) < mmap_kpage_hash (b, NULL);
}

void
mmap_init (void)
{
  lock_init (&mmap_filesys_lock);
  hash_init (&mmap_regions, &mmap_region_hash, &mmap_region_less, NULL);
  hash_init (&mmap_kpages, &mmap_kpage_hash, &mmap_kpage_less, NULL);
  
  sema_init (&mmap_writer_sema, 0);
  lock_init (&mmap_writer_lock);
  list_init (&mmap_writer_tasks);
  mmap_writer_thread = thread_create ("MMAP", PRI_MAX, mmap_writer_func, NULL);
  
  printf ("Initialized mmapping.\n");
}

static unsigned
mmap_aliases_hash (const struct hash_elem *e, void *t UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (mapid_t))];
  ASSERT (e != NULL);
  
  struct mmap_alias *ee = hash_entry (e, struct mmap_alias, aliases_elem);
  return (unsigned) ee->id;
}

static bool
mmap_aliases_less (const struct hash_elem *a,
                   const struct hash_elem *b,
                   void *t)
{
  return mmap_kpage_hash (a, t) < mmap_kpage_hash (b, t);
}

void
mmap_init_thread (struct thread *owner)
{
  ASSERT (owner != NULL);
  
  hash_init (&owner->mmap_aliases, &mmap_aliases_hash, &mmap_aliases_less,
             owner);
}

static void
mmap_alias_dispose_real (struct mmap_alias *ee)
{
  ASSERT (ee != NULL);
  
  // TODO
  
  free (ee);
}

static void
mmap_clean_sub (struct hash_elem *e, void *t UNUSED)
{
  ASSERT (e != NULL);
  struct mmap_alias *ee = hash_entry (e, struct mmap_alias, aliases_elem);
  mmap_alias_dispose_real (ee);
}

void
mmap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  hash_destroy (&owner->mmap_aliases, &mmap_clean_sub);
}

bool
mmap_alias_dispose (struct thread *owner, mapid_t id)
{
  ASSERT (owner != NULL);
  if (id == MAP_FAILED)
    return false;
    
  struct mmap_alias key;
  memset (&key, 0, sizeof (key));
  key.id = id;
  struct hash_elem *e = hash_find (&owner->mmap_aliases, &key.aliases_elem);
  if (!e)
    return false;
  
  struct mmap_alias *ee = hash_entry (e, struct mmap_alias, aliases_elem);
  mmap_alias_dispose_real (ee);
  return true;
}
