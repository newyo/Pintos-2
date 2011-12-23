#ifndef __LRU_H
#define __LRU_H

#include <stddef.h>
#include <stdbool.h>
#include <debug.h>
#include <list.h>

#define LRU_MAGIC ('L'<<24 | 'R'<<16 | 'U'<<8)

struct lru_elem
{
  void             *datum;
  struct list_elem  elem;
  uint32_t          lru_magic;
  char              end[0];
};

#define lru_entry(LIST_ELEM, STRUCT, MEMBER) \
({ \
  typedef __typeof (STRUCT) _t; \
  __typeof (LIST_ELEM) _e = (LIST_ELEM); \
  ASSERT (_e != NULL); \
  ASSERT (_e->lru_magic == LRU_MAGIC || !list_is_interior (&_e->elem)); \
  ASSERT (_e->lru_magic == 0         ||  list_is_interior (&_e->elem)); \
  (_t*) ((uint8_t*)&_e->end - offsetof (_t, MEMBER.end)); \
})

typedef void lru_dispose_action (struct lru_elem *e, void *aux);

struct lru
{
  struct list         lru_list;
  size_t              lru_size, item_count;
  lru_dispose_action *dispose_action;
  void               *aux;
};

void lru_init (struct lru         *l,
               size_t              size,
               lru_dispose_action  dispose_action,
               void               *aux);
void lru_free (struct lru *l);

void lru_use (struct lru *l, struct lru_elem *e);
void lru_dispose (struct lru *l, struct lru_elem *e, bool run_dispose_action);

struct lru_elem *lru_pop_least (struct lru *l);

#define lru_is_empty(L) ((L)->item_count == 0)

#endif
