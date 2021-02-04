#!/bin/sh -e
. $(dirname $0)/base.sh

iperf -s -i 1 -t 3 &
sleep 0.5
iperf -c $ip --full-duplex -i 1 -t 2
wait
