#include <stdio.h>
#include <assert.h>

#define ASSERT assert

#include "fixed-point.h"

#define S "%c%d.(%.4x)"
#define P(fp) (fp).signedness ? '-' : '+', (fp).int_part, 4*(fp).frac_part

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
  fprintf(stdout, "\n\n-------------------------------------------\n\n");
  
  for (i = -4; i < 4; ++i)
    for (j = -4; j < 4; ++j)
      {
        if(j == 0)
          continue;
          
        fp_t num = fp_from_int (i);
        fp_t denom = fp_from_int(j);
        fp_t frac = fp_div (num, denom);
        
        fprintf(stdout, S "/" S " = " S "\n", P(num), P(denom), P(frac));
        fprintf(stdout, "trunc(" S "/" S ") = %d\n", P(num), P(denom), fp_truncate(frac));
        fprintf(stdout, "round(" S "/" S ") = %d\n", P(num), P(denom), fp_round(frac));
        fprintf(stdout, "\n");
      }
  fprintf(stdout, "\n-------------------------------------------\n\n");
  
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
  fprintf(stdout, "\n-------------------------------------------\n\n");
  
  for (i = -5; i <= 5; i += 2)
    {
      for (j = -5; j <= 5; j += 3)
        {
          fp_t left = fp_from_int (i);
          fp_t right = fp_div (fp_from_int (j), fp_from_int (60));
          fp_t result = fp_mult (left, right);
          fprintf (stdout, S " * " S " = " S "\n",
                   P(left), P(right), P(result));
        }
      fprintf(stdout, "\n");
    };
    
  return 0;
}
