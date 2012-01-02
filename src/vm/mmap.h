#ifndef __MMAP_H
#define __MMAP_H

#include <stddef.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "vm/vm.h"

void mmap_init (void);
void mmap_init_thread (struct thread *owner);
void mmap_clean (struct thread *owner);

mapid_t mmap_open (struct thread *owner, void *base, struct file *file);
bool mmap_close (struct thread *owner, mapid_t map);

bool mmap_read (struct thread *owner, void *base, void *dest);
bool mmap_write (struct thread *owner, void *base, void *src);

#define MMAP_INVALID_SIZE ((size_t) -1u)
size_t mmap_get_size (struct thread *owner, void *base);
size_t mmap_get_size2 (struct thread *owner, mapid_t map);

#endif
