#ifndef __DEBUG_MALLOC
#define __DEBUG_MALLOC

#define malloc(S)                                                             \
({                                                                            \
  __extension__ __typeof (S) _s = (S);                                        \
  __extension__ void *_p = malloc (_s);                                       \
  printf ("    malloc'd %d bytes: %p (%s:%d).\n", _s, _p, __FILE__, __LINE__);\
  _p;                                                                         \
})

#define calloc(Z, S)                                                          \
({                                                                            \
  __extension__ __typeof (Z) _z = (Z);                                        \
  __extension__ __typeof (S) _s = (S);                                        \
  __extension__ void *_p = calloc (_z, _s);                                   \
  printf ("    calloc'd %d*%d bytes: %p (%s:%d).\n", _z, _s, _p, __FILE__,    \
                                                                 __LINE__);   \
  _p;                                                                         \
})

#define free(P)                                                               \
({                                                                            \
  __extension__ __typeof (P) _p = (P);                                        \
  printf ("    Freeing %p (%s:%d).\n", _p, __FILE__, __LINE__);               \
  free (_p);                                                                  \
  _p;                                                                         \
})

#endif
