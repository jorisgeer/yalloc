/* yaldum.c - dummy replacement yalloc

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <stddef.h>

#include "stdlib.h"
#include "malloc.h"

size_t yal_mstats(struct yal_stats *sp,unsigned int opts,unsigned int tag,const char *desc) { return opts + tag; }

unsigned int yal_options(enum Yal_options opt,size_t arg1,size_t arg2) { return opt; }

void * yal_alloc(size_t size,unsigned int tag)
{
  return malloc(size);
}

void yal_free(void *p,unsigned int tag)
{
  free(p);
}

void * yal_realloc(void *p,size_t newsize,unsigned int tag)
{
  return  realloc(p,newsize);
}

#if defined __APPLE__ && defined __MACH__
#include <malloc/malloc.h>
size_t malloc_usable_size(void * ptr)
{
  if (ptr == (void *)0) return 0;
  return malloc_size(ptr);
}
#endif
