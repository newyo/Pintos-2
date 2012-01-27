#include "canonical_path.h"

#include <stdint.h>
#include <string.h>
#include <debug.h>

#include "threads/malloc.h"

// This file has to be optimized ... it copies memory a lot

static char *
strmemcpy (const char *s, size_t len)
{
  char *result = malloc (len+1);
  if (result)
    {
      memcpy (result, s, len);
      result[len] = 0;
    }
  return result;
}

static char *
strdup (const char *s)
{
  return strmemcpy (s, strlen (s));
}

#define _END(C) _IN (C, 0, '/')

// cwd starts and ends with a slash
static char *
real (const char *cwd, const char *rel_path)
{
  ASSERT (cwd != NULL);
  ASSERT (cwd[0] == '/');
  ASSERT (cwd[strlen (cwd)-1] == '/');
  ASSERT (rel_path != NULL);
  
  if (rel_path[0] == 0)
    {
      // relpath is empty
      size_t len = strlen (cwd);
      char *result = malloc (len+1);
      if (result)
        {
          memcpy (result, cwd, len);
          result[len] = 0;
        }
      return result;
    }
  else if (rel_path[0] == '/' && rel_path[1] == 0)
    {
      // path ends with a slash, must be retained
      return strdup (cwd);
    }
  else if (rel_path[0] == '/')
    {
      // superfluous slash
      return real (cwd, rel_path+1);
    }
  else if (rel_path[0] == '.' && _END (rel_path[1] == 0))
    {
      // superfluous current folder dot
      return strdup (cwd);
    }
  else if (rel_path[0] == '.' && rel_path[1] == '.' && _END (rel_path[2]))
    {
      // folder up dot dot
      size_t len = strlen (cwd);
      rel_path += 2;
      if (len == 1)
        {
          // we are already at root
          return real ("/", rel_path);
        }
        
      // strip topmost entry of cwd
      --len;
      while (cwd[len] != '/')
        --len;
      char *up = strmemcpy (cwd, len);
      if (!up)
        return NULL;
        
      char *result = real (up, rel_path);
      free (up);
      return result;
    }
    
  // need to concat cwd and path item
  
  const char *end = strchrnul (rel_path, '/');
  size_t cwd_len = strlen (cwd);
  size_t item_len = (uintptr_t) (end - rel_path) + 1; // including end
  char *concatted = malloc (cwd_len + item_len + 1);
  if (!concatted)
    return NULL;
  memcpy (concatted, cwd, cwd_len);
  memcpy (concatted+cwd_len, rel_path, item_len);
  concatted[cwd_len+item_len] = 0;
  
  if(!*end)
    {
      // we are finished, does not end on slash
      return concatted;
    }
  else
    {
      rel_path += item_len;
      char *result = real (concatted, rel_path);
      free (concatted);
      return result;
    }
}

char *
canonical_path_get (const char *cwd, const char *rel_path)
{
  ASSERT (cwd != NULL);
  ASSERT (cwd[0] == '/');
  ASSERT (rel_path != NULL);
  
  if (rel_path[0] == 0)
    return strdup (cwd);
  else if (rel_path[0] == '/')
    return canonical_path_get ("/", rel_path+1);
  else
    return real (cwd, rel_path);
}
