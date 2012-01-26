#include "lru.h"
#include <string.h>

static inline void
assert_filling (struct lru *l UNUSED)
{
  ASSERT (l != NULL);
  ASSERT ((l->item_count == 0) == list_empty (&l->lru_list));
}

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
  assert_filling (l);
  while (!list_empty (&l->lru_list))
    {
      struct list_elem *e = list_front (&l->lru_list);
      ASSERT (e != NULL);
      lru_dispose (l, list_entry (e, struct lru_elem, elem), true);
    }
  assert_filling (l);
}

void
lru_use (struct lru *l, struct lru_elem *e)
{
  assert_filling (l);
  ASSERT (e != NULL);
  
  if (e->lru_list == NULL)
    {
      e->lru_list = l;
      ++l->item_count;
      if (l->lru_size > 0 && l->item_count > l->lru_size)
        lru_dispose (l, lru_peek_least (l), true);
    }
  else
    {
      ASSERT (e->lru_list == l);
      list_remove (&e->elem);
    }
    
  list_push_front (&l->lru_list, &e->elem);
  assert_filling (l);
}

void
lru_dispose (struct lru *l, struct lru_elem *e, bool run_dispose_action)
{
  assert_filling (l);
  
  ASSERT (e != NULL);
  if (e->lru_list == NULL)
    return;
  
  ASSERT (e->lru_list == l);
  ASSERT (list_is_interior (&e->elem));
  
  list_remove_properly (&e->elem);
  e->lru_list = NULL;
  
  --l->item_count;
  if (run_dispose_action && l->dispose_action != NULL)
    l->dispose_action (e, l->aux);
  
  assert_filling (l);
}

struct lru_elem *
lru_peek_least (struct lru *l)
{
  assert_filling (l);
  if (l->item_count == 0)
    return NULL;
  return list_entry (list_back (&l->lru_list), struct lru_elem, elem);
}
