#!/bin/sh
# Host tests for the dim-before-screen-off rules. Run from any directory as
# `sh tests/run.sh`; it finds the project root itself.
set -e
cd "$(dirname "$0")/.."

cc -O2 -Wall -Wextra -I. -o /tmp/dim_test tests/test_dim.c
/tmp/dim_test
