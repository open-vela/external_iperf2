#!/bin/sh -e
. $(dirname $0)/base.sh

iperf -s -i 1 -t 3 &
sleep 0.5
iperf -c $ip -d -L $lport -i 1 -t 2
wait
