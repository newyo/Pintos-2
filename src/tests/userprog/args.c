/* Prints the command-line arguments.
   This program is used for all of the args-* tests.  Grading is
   done differently for each of the args-* tests based on the
   output. */

#include "tests/lib.h"

#define CELLS (5*1024*1024 / sizeof (void *))

int
main (int argc, char *argv[]) 
{
  void *space[CELLS];
  
  unsigned i;
  for (i = 0; i < sizeof (space)/sizeof (space[0]); ++i)
    space[i] = 0x78563412;
  for (i = 0; i < sizeof (space)/sizeof (space[0]); ++i)
    printf ("    %p == %p\n", space[i], i);
  
  return (int) space;
}
