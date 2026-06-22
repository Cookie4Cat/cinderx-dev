#!/bin/bash

# Verify that the tests.bzl file is up to date and accounts for all of the CPython and cinder tests.
# If the tests diff you can run regen_test_defs.sh to update it.

set -e

cd "$(dirname "$(readlink -f "$0")")"

./gen_test_defs.sh /tmp/tests.txt

diff /tmp/tests.txt ../tests.bzl
