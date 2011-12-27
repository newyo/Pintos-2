#include "lru.h"
#include <string.h>

#define ASSERT_FILLING(X) \
({ \
  __typeof (X) _x = (X); \
  ASSERT (_x != NULL); \
  ASSERT (_x->item_count == 0 || list_empty (&_x->lru_list)); \
  ASSERT (_x->item_count > 0 || !list_empty (&_x->lru_list)); \
  (void) 0; \
})

void
lru_init (struct lru         *l,
          size_t              size,
          lru_dispose_action  dispose_action,
          void               *aux)
{
  ASSERT (l != NULL);
  
  memset (l, sizeof (*l), 0);
  list_init (&l->lru_list);
  l->lru_size = size;
  l->dispose_action = dispose_action;
  l->aux = aux;
}

void
lru_free (struct lru *l)
{
  ASSERT_FILLING (l);
  while (!list_empty (&l->lru_list))
    {
      struct list_elem *e = list_front (&l->lru_list);
      ASSERT (e != NULL);
      lru_dispose (l, list_entry (e, struct lru_elem, elem), true);
    }
}

void
lru_use (struct lru *l, struct lru_elem *e)
{
  ASSERT_FILLING (l);
  ASSERT (e != NULL);
  if (e->lru_magic == 0)
    {
      e->lru_magic = LRU_MAGIC;
      ++l->item_count;
      if (l->lru_size > 0 && l->item_count > l->lru_size)
        lru_dispose (l, lru_peek_least (l), true);
    }
  else
    ASSERT (e->lru_magic == LRU_MAGIC);
  list_push_front (&l->lru_list, &e->elem);
}

void
lru_dispose (struct lru *l, struct lru_elem *e, bool run_dispose_action)
{
  ASSERT_FILLING (l);
  
  ASSERT (e != NULL);
  if (e->lru_magic == 0)
    return;
  
  ASSERT (e->lru_magic == LRU_MAGIC);
  ASSERT (list_is_interior (&e->elem));
  
  list_remove (&e->elem);
  e->lru_magic = 0;
  
  --l->item_count;
  if (run_dispose_action && l->dispose_action != NULL)
    l->dispose_action (e, l->aux);
  
  ASSERT_FILLING (l);
}

struct lru_elem *
lru_peek_least (struct lru *l)
{
  ASSERT_FILLING (l);
  if (l->item_count == 0)
    return NULL;
  return list_entry (list_back (&l->lru_list), struct lru_elem, elem);
}
