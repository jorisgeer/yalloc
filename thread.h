/* thread.h - nonportable thread exit notification

 */

  #include <pthread.h>

// https://github.com/cpredef

#ifdef __musl__ // musl has no identifier, expected from cmdline
  typdedef struct __ptcb Thread_clean_info;
  #define Thread_clean_push(i,f,x) _pthread_cleanup_push(i,f,x)

#elif defined __GLIBC__
  typedef struct _pthread_cleanup_buffer Thread_clean_info;
  extern void __pthread_cleanup_push (struct _pthread_cleanup_buffer *buffer,void (*routine) (void *), void *arg);
  #define Thread_clean_push(i,f,x) // __pthread_cleanup_push(i,f,x)

#elif defined __APPLE__ && defined __MACH__
  // excerpt from pthread.h
  typedef struct __darwin_pthread_handler_rec Thread_clean_info;

  static void Thread_clean_push( struct __darwin_pthread_handler_rec *handler,void (*func)(void *),void *val)
  {
       pthread_t self = pthread_self();
	     handler->__routine = func;
	     handler->__arg = val;
	     handler->__next = self->__cleanup_stack;
	     self->__cleanup_stack = handler;
  }

#else
  typedef int Thread_clean_info;
  #define Thread_clean_push(i,f,x)

 #endif
