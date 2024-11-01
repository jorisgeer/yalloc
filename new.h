// new.h - yalloc header for c++ new / delete

extern "C" void *malloc(unsigned long);
extern "C" void free(void *);

void * operator new(unsigned long size) {
  return malloc(size);
}

void * operator new[](unsigned long size) {
  return malloc(size);
}

void operator delete(void* ptr) { free(ptr); }
void operator delete[](void* ptr) { free(ptr); }
