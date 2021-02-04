#!/bin/sh -e
. $(dirname $0)/base.sh

iperf -s -P 2 -i 1 -t 3 &
sleep 0.5
iperf -c $ip -P 2 -i 1 -t 2
wait
