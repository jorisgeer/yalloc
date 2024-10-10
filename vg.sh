#!/bin/sh

valgrind --tool=memcheck --show-below-main=yes --suppressions=vg.sup --exit-on-first-error=yes --track-origins=yes --soname-synonyms=somalloc=nouserintercept "$@"

