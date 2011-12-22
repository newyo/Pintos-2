#ifndef __SWAP_H
#define __SWAP_H

#include <bitmap.h>

extern const char SWAP_FILENAME[];

void swap_init (void);

typedef size_t swap_t;
#define SWAP_FAIL ((swap_t) BITMAP_ERROR)

swap_t swap_get_disposable_bytes (size_t count);
swap_t swap_get_disposable_pages (size_t count);

// TODO

#endif
