/* Prints the command-line arguments.
   This program is used for all of the args-* tests.  Grading is
   done differently for each of the args-* tests based on the
   output. */

#include "tests/lib.h"

#define CELLS (7*1024*1024 / sizeof (unsigned))

int
main (int argc, char *argv[]) 
{
  unsigned space[CELLS];
  
  unsigned i;
  for (i = 0; i < sizeof (space)/sizeof (space[0]); ++i)
    space[i] = i;
  
  return (int) space;
}
