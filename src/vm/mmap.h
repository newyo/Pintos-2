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

struct vm_page *mmap_load (const struct vm_page *vm_page, void *dest);

struct mmap_alias *mmap_retreive_alias (struct thread *owner, mapid_t id);
size_t mmap_alias_pages_count (struct mmap_alias *alias);
struct mmap_upage *mmap_alias_map_upage (struct mmap_alias *alias,
                                         struct vm_page    *vm_page,
                                         size_t             nth_page);

#endif
