#!/bin/sh -ex
. $(dirname $0)/base.sh

iperf -s --parallel 2 -i 1 -t 3 &
sleep 0.5
iperf -c $ip --parallel 2 -i 1 -t 2
wait
