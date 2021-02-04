#!/bin/sh -e
. $(dirname $0)/base.sh

iperf -V -s -P 1 -u -i 1 -t 3 &
sleep 0.5
iperf -V -c $ip6 -P 1 -u -b 10m -i 1 -t 2
wait

