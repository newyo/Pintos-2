#include <stdio.h>
#include <assert.h>

#define ASSERT assert

#include "fixed-point.h"

#define S "%c%d.(%x)"
#define P(fp) (fp).signedness ? '-' : '+', (fp).int_part, (fp).frac_part

int main ()
{
  int i, j;
  
  for (i = -5; i <= 5; ++i)
    {
      fp_t fp = fp_from_int (i);
      fprintf (stdout, S "\n", P (fp));
    }
  fprintf(stdout, "\n");
  
  for (i = -5; i <= 5; ++i)
    {
      fp_t fp = fp_from_int (i);
      fp_t n = fp_negate (fp);
      fprintf (stdout, S " = -(" S ")\n", P(fp), P(n));
    }
  fprintf(stdout, "\n");
  
  fprintf(stdout, S "\n", P(fp_div(fp_from_int(1), fp_from_int(2))));
  
  for (i = -5; i <= 5; i += 2)
    {
      for (j = -5; j <= 5; j += 3)
        {
          fp_t left = fp_from_int (i);
          fp_t right = fp_from_int (j);
          fp_t result = fp_add (left, right);
          fprintf (stdout, S " + " S " = " S "\n",
                   P(left), P(right), P(result));
        }
      fprintf(stdout, "\n");
    }
  return 0;
}
