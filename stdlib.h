/* stdlib.h - local prototypes as implemented by yalloc.

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.
 */

// c90
extern void *malloc (size_t size);
extern void *calloc (size_t count, size_t size);
extern void *realloc (void *ptr, size_t size);
extern void free (void *ptr);

// c11
extern void *aligned_alloc(size_t align, size_t size);

// posix
extern int posix_memalign(void **memptr, size_t align, size_t size);
extern void *memalign(size_t a,size_t n);
extern void *valloc(size_t n);
extern void *pvalloc(size_t n);

// c23
extern void free_sized(void *ptr,size_t size);
extern void free_aligned_sized(void *ptr, size_t alignment, size_t size);
