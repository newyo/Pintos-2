#ifndef __MMAP_H
#define __MMAP_H

#include <stddef.h>
#include <hash.h>
#include <list.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "vm/vm.h"

struct mmap_region
{
  struct file      *file;
  size_t            length;
  struct list       aliases;
  struct hash       kpages;
  
  struct hash_elem  regions_elem;
  
  uint32_t          magic;
};

struct mmap_alias
{
  mapid_t             id;
  struct mmap_region *region;
  struct hash         upages;
  
  struct hash_elem    aliases_elem;
  struct list_elem    region_elem;
  
  uint32_t            magic;
};

struct mmap_kpage
{
  struct vm_page     *kernel_page;
  struct mmap_region *region;
  size_t              page_num;
  bool                dirty;
  struct list         upages;
  
  struct hash_elem    region_elem;
};

struct mmap_upage
{
  struct vm_page    *vm_page;
  struct mmap_alias *alias;
  size_t             page_num;
  struct mmap_kpage *kpage;
  
  struct hash_elem   alias_elem;
  struct hash_elem   upages_elem;
  struct list_elem   kpage_elem;
};

void mmap_init (void);
void mmap_init_thread (struct thread *owner);
void mmap_clean (struct thread *owner);

mapid_t mmap_alias_acquire (struct thread *owner, struct file *file);
void mmap_alias_dispose (struct thread *owner, struct mmap_alias *alias);

struct mmap_upage *mmap_retreive_upage (struct vm_page *vm_page);
struct mmap_kpage *mmap_assign_kpage (struct mmap_upage *upage);
struct mmap_kpage *mmap_load_kpage (struct mmap_upage *upage,
                                    struct vm_page    *kernel_page);

struct mmap_alias *mmap_retreive_alias (struct thread *owner, mapid_t id);
size_t mmap_alias_pages_count (struct mmap_alias *alias);
struct mmap_upage *mmap_alias_map_upage (struct mmap_alias *alias,
                                         struct vm_page    *vm_page,
                                         size_t             nth_page);

void mmap_write_kpage (struct mmap_kpage *kpage);

#endif
