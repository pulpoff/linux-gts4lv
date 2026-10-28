#!/bin/sh
# Scripted UI benchmark: the same swipes on the app grid, display frames counted
# (msm-kms interrupts, ~3 per frame) for each GPU floor given as an argument.
G=/sys/class/devfreq/5000000.gpu
irq() { awk '/msm-kms/{print $2}' /proc/interrupts; }
systemctl stop touchboost
for f in "$@"; do
	echo $f > $G/min_freq
	gts4lv-swipe 1280 1500 1280 300 300 1	# app grid open
	sleep 1.5
	a=$(irq); t0=$(cut -d' ' -f1 /proc/uptime)
	gts4lv-swipe 2200 800 300 800 350 6	# page through the grid, both ways
	gts4lv-swipe 300 800 2200 800 350 6
	b=$(irq); t1=$(cut -d' ' -f1 /proc/uptime)
	gts4lv-swipe 1280 300 1280 1500 300 1	# close again
	sleep 1
	echo "gpu floor $f: $(echo "($b-$a)/($t1-$t0)" | bc -l | cut -c1-5) kms irq/s"
done
echo 180000000 > $G/min_freq
systemctl start touchboost
dmesg | grep -c -E "ctl start|frame done timeout"
