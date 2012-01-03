#ifndef __MMAP_H
#define __MMAP_H

#include <stddef.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "vm/vm.h"

void mmap_init (void);
void mmap_init_thread (struct thread *owner);
void mmap_clean (struct thread *owner);

#endif
