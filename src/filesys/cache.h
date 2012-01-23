#ifndef CACHE_H__
#define CACHE_H__

#include "devices/block.h"
#include "vm/lru.h"
#include <bitmap.h>
#include <stdbool.h>
#include <stddef.h>

typedef char block_cache_page[BLOCK_SECTOR_SIZE];
struct block_cache;

struct block_cache *block_cache_init (struct block *device, size_t cache_size);
void block_cache_destroy (struct block_cache *bc);

block_cache_page *block_cache_retrieve (struct block_cache *bc,
                                        block_sector_t      nth);

void block_cache_return (struct block_cache *bc,
                         block_sector_t      nth,
                         block_cache_page   *page,
                         bool                wrote);

void block_cache_write (struct block_cache *bc,
                        block_sector_t      nth,
                        block_cache_page   *data);

void block_cache_flush (struct block_cache *bc);

struct block *block_cache_get_device (struct block_cache *bc);

#endif
