// signal.h - dummy interface for yalloc analysis

#define SIGSEGV 1
#define SIGBUS 2

#define SA_SIGINFO 1

struct siginfo {
  void *si_addr;
};
typedef struct siginfo siginfo_t;

struct sigaction {
  void (*sa_handler)(int sig);
  void (*sa_sigaction)(int sig, siginfo_t *si,void *);
  unsigned int sa_flags;
};

extern int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
