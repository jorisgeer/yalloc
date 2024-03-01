
CC=gcc
CFLAGS=-std=c11

all: yalloc.o test.o printf.o os.o
	$(CC) -o test $(CFLAGS) test.o yalloc.c os.o printf.o

printf.o: printf.c printf.h base.h
	$(CC) $(CFLAGS) -c printf.c

yalloc.o: yalloc.c
	$(CC) $(CFLAGS) -c yalloc.c

os.o: os.c os.h base.h
	$(CC) $(CFLAGS) -c os.c

test.o: test.c stdlib.h
	$(CC) $(CFLAGS) -c test.c

