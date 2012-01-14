#include "mmap.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/interrupt.h"

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
static struct hash mmap_upages;

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
  wrote = file_write_at (r->file, page->kernel_page->user_addr, len, start);
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
  void *user_addr = page->kernel_page->user_addr;
  if (r->length >= start+PGSIZE)
    {
      off_t read UNUSED;
      read = file_read_at (r->file, user_addr, PGSIZE, start);
      ASSERT (read == PGSIZE);
    }
  else
    {
      off_t len = r->length >= start+PGSIZE ? PGSIZE : r->length - start;
      off_t read UNUSED;
      read = file_read_at (r->file, user_addr, len, start);
      ASSERT (read == len);
      memset (user_addr+len, 0, PGSIZE-len);
    }
    
  lock_release (&mmap_filesys_lock);
}

static void
mmap_writer_func (void *aux UNUSED)
{ 
  ASSERT (intr_get_level () == INTR_ON);
  
  for(;;)
    {
      struct list task_list = LIST_INITIALIZER (task_list);
      sema_down (&mmap_writer_sema);
      
      enum intr_level old_level;
      lock_acquire2 (&mmap_writer_lock, &old_level);
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
mmap_upages_hash (const struct hash_elem *e, void *t UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (mapid_t))];
  ASSERT (e != NULL);
  
  struct mmap_upage *ee = hash_entry (e, struct mmap_upage, upages_elem);
  return (unsigned) ee->vm_page;
}

static bool
mmap_upages_less (const struct hash_elem *a,
                  const struct hash_elem *b,
                  void *aux UNUSED)
{
  return mmap_upages_hash (a, NULL) < mmap_upages_hash (b, NULL);
}

void
mmap_init (void)
{
  lock_init (&mmap_filesys_lock);
  hash_init (&mmap_regions, &mmap_region_hash, &mmap_region_less, NULL);
  hash_init (&mmap_upages, &mmap_upages_hash, &mmap_upages_less, NULL);
  
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
  return mmap_aliases_hash (a, t) < mmap_aliases_hash (b, t);
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
  ASSERT (intr_get_level () == INTR_OFF);
  
  // TODO
  
  free (ee);
}

static void
mmap_clean_sub (struct hash_elem *e, void *t UNUSED)
{
  ASSERT (e != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  struct mmap_alias *ee = hash_entry (e, struct mmap_alias, aliases_elem);
  mmap_alias_dispose_real (ee);
}

void
mmap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  hash_destroy (&owner->mmap_aliases, &mmap_clean_sub);
}

static unsigned
mmap_alias_upage_hash (const struct hash_elem *e, void *alias UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  ASSERT (e != NULL);
  
  struct mmap_upage *ee = hash_entry (e, struct mmap_upage, alias_elem);
  ASSERT (ee->alias == alias);
  
  return ee->alias->id ^ (ee->page_num << 16) ^ (ee->page_num >> 16);
}

static bool
mmap_alias_upage_less (const struct hash_elem *a,
                       const struct hash_elem *b,
                       void                   *alias UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);
  
  struct mmap_upage *aa = hash_entry (a, struct mmap_upage, alias_elem);
  struct mmap_upage *bb = hash_entry (b, struct mmap_upage, alias_elem);
  ASSERT (aa->alias == alias);
  ASSERT (bb->alias == alias);
  
  if (aa->page_num < bb->page_num)
    return true;
  else if (aa->page_num > bb->page_num)
    return false;
  else
    return aa->alias->id < bb->alias->id;
}

static unsigned
mmap_region_kpage_hash (const struct hash_elem *e, void *region)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (size_t))];
  ASSERT (e != NULL);
  
  struct mmap_kpage *ee = hash_entry (e, struct mmap_kpage, region_elem);
  ASSERT (ee->region == region);
  return (unsigned) ee->page_num;
}

static bool
mmap_region_kpage_less (const struct hash_elem *a,
                        const struct hash_elem *b,
                        void                   *region)
{
  return mmap_region_kpage_hash (a, region)<mmap_region_kpage_hash (b, region);
}

struct mmap_alias *
mmap_retreive_alias  (struct thread *owner, mapid_t id)
{
  ASSERT (owner != NULL);
  ASSERT (id != MAP_FAILED);
  ASSERT (intr_get_level () == INTR_OFF);
  
  struct mmap_alias key;
  memset (&key, 0, sizeof (key));
  key.id = id;
  
  struct hash_elem *e = hash_find (&owner->mmap_aliases, &key.aliases_elem);
  if (!e)
    return NULL;
  return hash_entry (e, struct mmap_alias, aliases_elem);
}

size_t
mmap_alias_pages_count (struct mmap_alias *alias)
{
  ASSERT (alias != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  return (alias->region->length + PGSIZE-1) / PGSIZE;
}

mapid_t
mmap_alias_acquire (struct thread *owner, struct file *file)
{
  ASSERT (owner != NULL);
  ASSERT (file != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  struct mmap_region key;
  memset (&key, 0, sizeof (key));
  key.file = file;
  
  struct hash_elem *e_region = hash_find (&mmap_regions, &key.regions_elem);
  struct mmap_region *region;
  if (e_region)
    region = hash_entry (e_region, struct mmap_region, regions_elem);
  else
    {
      region = calloc (1, sizeof (*region));
      if (region == NULL)
        {
          ASSERT (0); // TODO: remove  line
          return MAP_FAILED;
        }
      region->file = file_reopen (file);
      if (region->file == NULL)
        {
          ASSERT (0); // TODO: remove  line
          free (region);
          return MAP_FAILED;
        }
      region->length = file_length (region->file);
      list_init (&region->aliases);
    }
    
  struct mmap_alias *alias = calloc (1, sizeof (*alias));
  if (alias == NULL)
    {
      ASSERT (0); // TODO: remove  line
      if (e_region != NULL)
        free (region);
      return MAP_FAILED;
    }
  else if (e_region == NULL)
    hash_init (&region->kpages, &mmap_region_kpage_hash, mmap_region_kpage_less,
               region);
  static mapid_t id = 0;
  alias->id = ++id;
  alias->region = region;
  hash_init (&alias->upages, &mmap_alias_upage_hash, &mmap_alias_upage_less,
             alias);
  
  struct hash_elem *e UNUSED;
  e = hash_insert (&owner->mmap_aliases, &alias->aliases_elem);
  ASSERT (e == NULL);
  
  list_push_front (&region->aliases, &alias->region_elem);
  
  return alias->id;
}

bool
mmap_alias_dispose (struct thread *owner, mapid_t id)
{
  ASSERT (owner != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  if (id == MAP_FAILED)
    return false;
    
  struct mmap_alias key;
  memset (&key, 0, sizeof (key));
  key.id = id;
  struct hash_elem *e = hash_delete (&owner->mmap_aliases, &key.aliases_elem);
  if (!e)
    return false;
  
  struct mmap_alias *ee = hash_entry (e, struct mmap_alias, aliases_elem);
  mmap_alias_dispose_real (ee);
  return true;
}

struct mmap_upage *
mmap_alias_map_upage (struct mmap_alias *alias,
                      struct vm_page    *vm_page,
                      size_t             nth_page)
{
  ASSERT (alias != NULL);
  ASSERT (vm_page != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  struct mmap_upage *page = calloc (1, sizeof (*page));
  if (!page)
    return NULL;
  page->vm_page = vm_page;
  page->page_num = nth_page;
  page->alias = alias;
  
  struct hash_elem *e UNUSED;
  e = hash_insert (&alias->upages, &page->alias_elem);
  ASSERT (e == NULL);
  
  e = hash_insert (&mmap_upages, &page->upages_elem);
  ASSERT (e == NULL);
  
  return page;
}

struct mmap_kpage *
mmap_load_kpage (struct mmap_upage *upage, struct vm_page *kernel_page)
{
  ASSERT (upage != NULL);
  ASSERT (kernel_page != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  // TODO
  return false;
}

struct mmap_upage *
mmap_retreive_upage (struct vm_page *vm_page)
{
  ASSERT (vm_page != NULL);
  ASSERT (vm_page->type == VMPPT_MMAP_ALIAS);
  
  struct mmap_upage key;
  memset (&key, 0, sizeof (key));
  key.vm_page = vm_page;
  struct hash_elem *e = hash_find (&mmap_upages, &key.upages_elem);
  
  ASSERT (e != NULL);
  return hash_entry (e, struct mmap_upage, upages_elem);
}
