#define GLLOAD_IMPL
#include <stdio.h>
#include "glload.h"

#define GLLOAD_DEFINE(type, name) type p_##name = 0;
GLLOAD_FUNCS(GLLOAD_DEFINE)
#undef GLLOAD_DEFINE

int GLLoad(void *(*get_proc)(const char *)) {
  int ok = 1;
#define GLLOAD_GET(type, name) \
  p_##name = (type)get_proc(#name); \
  if (!p_##name) { fprintf(stderr, "GL: missing %s\n", #name); ok = 0; }
  GLLOAD_FUNCS(GLLOAD_GET)
#undef GLLOAD_GET
  return ok;
}
