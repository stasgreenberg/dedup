#!/bin/bash
#debian1 ranges
echo 'range 34816 34816' > /sys/kernel/dedup/stats
echo 'range 34832 34864' > /sys/kernel/dedup/stats
echo 'range 34880 98303' > /sys/kernel/dedup/stats
echo 'range 100352 163839' > /sys/kernel/dedup/stats
echo 'range 165888 229375' > /sys/kernel/dedup/stats
echo 'range 231424 294911' > /sys/kernel/dedup/stats
echo 'range 296960 458751' > /sys/kernel/dedup/stats
echo 'range 475136 476623' > /sys/kernel/dedup/stats
echo 'range 931280 931839' > /sys/kernel/dedup/stats
echo 'range 987136 987679' > /sys/kernel/dedup/stats
#debian2 ranges
echo 'range 477184 477184' > /sys/kernel/dedup/stats
echo 'range 477200 477232' > /sys/kernel/dedup/stats
echo 'range 477248 524287' > /sys/kernel/dedup/stats
echo 'range 557056 819199' > /sys/kernel/dedup/stats
echo 'range 821248 884735' > /sys/kernel/dedup/stats
echo 'range 886784 929791' > /sys/kernel/dedup/stats
echo 'range 931840 933887' > /sys/kernel/dedup/stats
echo 'range 989184 989199' > /sys/kernel/dedup/stats
