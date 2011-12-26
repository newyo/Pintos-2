#ifndef __SWAP_H
#define __SWAP_H

#include <stddef.h>
#include <stdbool.h>
#include "threads/thread.h"

void swap_init (void);

size_t swap_stats_pages (void);
size_t swap_stats_full_pages (void);

// All functions except swap_clean must be called with interrupts enabled.
// swap_clean may be called with interrupts disabled.
// swap_alloc_and_write may invoke process_dispose_unmodified_swap_page,
// the other functions won't.
// Length is in bytes, amount in pages.
// Most likely length is PGSIZE and amount is 1, resp.

bool swap_alloc_and_write (struct thread *owner,
                           void          *src,
                           size_t         length);
bool swap_read_and_retain (struct thread *owner,
                           const void    *base,
                           size_t         length);
bool swap_dispose (struct thread *owner,
                   const void    *base,
                   size_t         amount);

void swap_clean (struct thread *owner);

#endif
