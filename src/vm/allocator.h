#ifndef __ALLOCATOR_H
#define __ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <bitmap.h>

struct allocator
{
  size_t         item_size;
  struct bitmap *used_map;
  void          *items;
};

bool allocator_init (struct allocator *a,
                     bool              userspace,
                     size_t            members,
                     size_t            item_size);
void allocator_destroy (struct allocator *a);

void *allocator_alloc (struct allocator *a, size_t amount);
void allocator_free (struct allocator *a, void *base, size_t amount);

#endif
