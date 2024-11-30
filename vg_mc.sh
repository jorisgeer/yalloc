#!/bin/sh

set -ex

valgrind --show-below-main=yes --trace-children=yes --exit-on-first-error=yes --error-exitcode=2 --read-var-info=yes --soname-synonyms=somalloc=nouserintercept "$@"
