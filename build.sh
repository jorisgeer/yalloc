#!/bin/bash

# build.sh - build script for yalloc

set -f
set -eu

tool=clang
dbg=0

case $tool in
  'clang')
  cc=clang
  cdiag='-Weverything -Wimplicit-int-conversion -Wunused -Wsign-conversion -Wpadded -Wchar-subscripts -Werror=format -Wno-c2x-compat -Wno-poison-system-directories'
  cfmt='-fno-caret-diagnostics -fno-color-diagnostics -fno-diagnostics-show-option -fno-diagnostics-fixit-info -fno-diagnostics-show-note-include-stack -std=c11 -funsigned-char'
  cxtra='-funsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -ftls-model=local-exec'
  cana="--analyze"
  if [ $dbg -eq 1 ]; then
    cdbg='-g -fsanitize=undefined,signed-integer-overflow,bounds -fno-sanitize-recover=all -ftrapv -fstack-protector -D_FORTIFY_SOURCE=3'
    UBSAN_OPTIONS=print_stacktrace=1
    libs=
  else
    cdbg='-g -fno-stack-protector -fno-wrapv -fcf-protection=none -fno-asynchronous-unwind-tables -D_FORTIFY_SOURCE=0' # -fno-stack-clash-protection
    libs=
  fi
  lflags="-O2 $cdbg"
  ;;

  'gcc')
  cc=gcc-13
  cdiag='-Wall -Wextra -Wshadow -Wundef -Wno-unused -Wno-padded -Wno-char-subscripts -Werror -Wstack-usage=35000'
  cfmt='-fmax-errors=60 -fno-diagnostics-show-caret -fno-diagnostics-show-option -fno-diagnostics-color -fcompare-debug-second'
  cxtra='-funsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -ftls-model=local-exec -fcf-protection=none'
  cana="-fanalyzer"
  if [ $dbg -eq 1 ]; then
    cdbg='-g -fsanitize=address,undefined,signed-integer-overflow,bounds -fno-sanitize-recover=all -ftrapv -fstack-protector'
    UBSAN_OPTIONS=print_stacktrace=1
  else
    cdbg='-g -fno-stack-protector -fno-wrapv -fcf-protection=none -fno-stack-clash-protection -fno-asynchronous-unwind-tables -D_FORTIFY_SOURCE=0'
  fi
  lflags="-O2 -fuse-ld=gold -g"
    libs=
  ;;
esac

copt='-O2 -march=native'

cflags="-I. -std=c11 $copt $cdiag $cfmt $cdbg $cxtra "

# --gc-sections --no-ld-generated-unwind-info -z stack-size=1234

asmcflags='-fverbose-asm -frandon-seed=0'

map=0
dogen=0
valgrind=0
vrb=0
bldtst=0
target=''

usage()
{
  echo 'usage: build [options] [target]'
  echo
  echo '-a  - analyze'
  echo '-g  - run generators'
  echo '-t   - build test'
  echo '-m  - create map file'
  echo '-v  - verbose'
  echo '-vg - valgrind'
  echo '-h  - help'
  echo
  echo 'target - only build given target'
}

error()
{
  echo $1
  exit 1
}

verbose()
{
  if [ $vrb -eq 0 ]; then
    echo $1
  else
    echo $2
  fi
}

cc()
{
  local src
  local tgt

  tgt="$1"
  src="$2"

  verbose "$cc -c $src" "$cc -c $cflags $src"
  $cc -c $cflags $src
  if [ "$tgt" = "$target" ]; then
    exit 0
  fi
}

ld()
{
  local tgt
  local obj

  tgt="$1"
  obj="$2"

  verbose "ld -o $tgt $obj" "$cc -o $tgt $lflags $obj"
  $cc -o "$tgt" "$lflags" $obj $libs
  if [ $map -eq 1 ]; then
    nm --line-numbers -S -r --size-sort $tgt > $tgt.map
  fi

  if [ "$tgt" = "$target" ]; then
    exit 0
  fi
}

while [ $# -ge 1 ]; do
  case "$1" in
  '-a') cflags="$cflags $cana" ;;
  '-h'|'-?') usage ;;
  '-m') map=1 ;;
  '-g') dogen=1 ;;
  '-t') bldtst=1 ;;
  '-v') vrb=1 ;;
  '-vg') valgrind=1; cflags="$cflags -DVALGRIND" ;;
  *) target="$1" ;;
  esac
  shift
done

# cc printf.o printf.c

if [ $dogen -eq 1 ]; then
  cc configure.o configure.c
#  cc  stdio.o stdio.c stdio.h printf.h
  ld  configure  "configure.o printf.o"

  ./configure "layout.h"
fi

cc yalloc.o yalloc.c
# $cc -shared -o yalloc.so yalloc.o os.o printf.o

# cc os.o os.c
# cc stdio.o stdio.c

# ld -dylib -o yalloc.so yalloc.o os.o printf.o -flat_namespace -undefined suppress

if [ $dogen -eq 1 ]; then
  cc test.o test.c stdlib.h
  ld test "test.o yalloc.o os.o printf.o"
fi
# ld test_libc "test.o printf.o"

# ~/bin/valgrind -s --redzone-size=4096 --exit-on-first-error=yes --error-exitcode=1 --track-fds=yes --leak-check=no --partial-loads-ok=no --track-origins=yes --malloc-fill=55 $*
