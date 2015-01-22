#!/bin/bash
. add_ranges.sh
# Set the block device we are working on
echo 'setbd /dev/sda7' > /sys/kernel/dedup/stats
# Set the first block
echo 'block 34816' > /sys/kernel/dedup/stats
# Set the number of block between first and last blocks
echo 'dedup 954384' > /sys/kernel/dedup/stats
