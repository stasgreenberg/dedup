#!/bin/bash
# must run as SU
free
sync
echo 3 > /proc/sys/vm/drop_caches
echo 2 > /proc/sys/vm/drop_caches
echo 1 > /proc/sys/vm/drop_caches
free
