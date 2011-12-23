#ifndef __SWAP_H
#define __SWAP_H

#include <stddef.h>
#include <stdbool.h>
#include "threads/thread.h"

extern const char SWAP_FILENAME[];

void swap_init (void);

bool swap_alloc_and_write (struct thread *owner,
                           void          *src,
                           size_t         length);
void swap_read_and_retain (struct thread *owner,
                           const void    *base,
                           size_t         length);
void swap_dispose (struct thread *owner, const void *base, size_t amount);
void swap_clean (struct thread *owner);

#endif
