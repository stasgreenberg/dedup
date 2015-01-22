#!/bin/bash
for i in {1..10}
do
	. drop_pages.sh > /dev/null
	sleep 2
	qemu-system-i386 -hda /media/dedup/debian1.img -m 256 &
	sleep 40
	qemu-system-i386 -hda /media/dedup/debian2.img -m 256 &
	sleep 40
	pkill qemu
	. cat_dedup.sh
	sleep 3
done
