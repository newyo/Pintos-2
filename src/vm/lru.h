#ifndef __LRU_H
#define __LRU_H

#include <stddef.h>
#include <stdbool.h>
#include <debug.h>
#include <list.h>

struct lru;
struct lru_elem
{
  void             *datum;
  struct list_elem  elem;
  struct lru       *lru_list;
  char              end[0];
};

#define lru_entry(LIST_ELEM, STRUCT, MEMBER)                           \
({                                                                     \
  typedef __typeof (STRUCT) _t;                                        \
  __typeof (LIST_ELEM) _e = (LIST_ELEM);                               \
  ASSERT (_e != NULL);                                                 \
  (_t*) ((uint8_t*)&_e->end - offsetof (_t, MEMBER.end));              \
})

static inline bool
lru_is_interior (const struct lru_elem *elem)
{
  ASSERT (elem != NULL);
  return elem->lru_list != NULL;
}

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

struct lru_elem *lru_peek_least (struct lru *l);

static inline struct lru_elem *
lru_pop_least (struct lru *l)
{
  ASSERT (l != NULL);
  struct lru_elem *e = lru_peek_least (l);
  if (e)
    lru_dispose (l, e, false);
  return e;
}

static inline size_t
lru_usage (struct lru *l)
{
  ASSERT (l != NULL);
  return l->item_count;
}

static inline size_t
lru_is_empty (struct lru *l)
{
  return lru_usage (l) == 0;
}

static inline size_t
lru_capacity (struct lru *l)
{
  ASSERT (l != NULL);
  return l->item_count;
}

#endif
