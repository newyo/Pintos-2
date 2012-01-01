#ifndef __MMAP_H
#define __MMAP_H

#include "threads/thread.h"
#include "filesys/file.h"

void mmap_init (void);
void mmap_init_thread (struct thread *owner);
void mmap_clean (struct thread *owner);

bool mmap_open (struct thread *owner, void *base, struct file *file);
bool mmap_close (struct thread *owner, void *base);

bool mmap_read (struct thread *owner, void *base, void *dest);
bool mmap_write (struct thread *owner, void *base, void *src);

#endif
