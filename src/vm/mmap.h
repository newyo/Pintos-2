#ifndef __MMAP_H
#define __MMAP_H

#include <stddef.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "vm/vm.h"

struct mmap_region;
struct mmap_alias;
struct mmap_kpage;
struct mmap_upage;

void mmap_init (void);
void mmap_init_thread (struct thread *owner);
void mmap_clean (struct thread *owner);

mapid_t mmap_alias_acquire (struct thread *owner, struct file *file);
bool mmap_alias_dispose (struct thread *owner, mapid_t id);

bool mmap_map_upage (struct thread *owner,
                     mapid_t        id,
                     void          *base,
                     size_t         nth_page);
bool mmap_map_upages (struct thread *owner,
                      mapid_t        id,
                      void          *base);

bool mmap_load (const struct vm_page *vm_page, void *dest);

#endif
