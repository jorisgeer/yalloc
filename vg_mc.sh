#!/bin/sh

set -ex

valgrind --tool=memcheck --show-below-main=yes --suppressions=vg.sup --exit-on-first-error=yes --error-exitcode=2 --read-var-info=yes --track-fds=yes --soname-synonyms=somalloc=nouserintercept "$@"
