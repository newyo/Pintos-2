#ifndef __LIB_ROUND_H
#define __LIB_ROUND_H

/* Yields X rounded up to the nearest multiple of STEP.
   For X >= 0, STEP >= 1 only. */
#define ROUND_UP(X, STEP)                     \
({                                            \
  register const __typeof (X)    _x = (X);    \
  register const __typeof (STEP) _s = (STEP); \
  (_x / _s + (_x % _s ? 1 : 0)) * _s;         \
})

/* Yields X divided by STEP, rounded up.
   For X >= 0, STEP >= 1 only. */
#define DIV_ROUND_UP(X, STEP)                 \
({                                            \
  register const __typeof (X)    _x = (X);    \
  register const __typeof (STEP) _s = (STEP); \
  _x / _s + (_x % _s ? 1 : 0);                \
})

/* Yields X rounded down to the nearest multiple of STEP.
   For X >= 0, STEP >= 1 only. */
#define ROUND_DOWN(X, STEP)                   \
({                                            \
  register const __typeof (X)    _x = (X);    \
  register const __typeof (STEP) _s = (STEP); \
  (_x / _s) * _s;                             \
})

/* There is no DIV_ROUND_DOWN.   It would be simply X / STEP. */

#endif /* lib/round.h */
