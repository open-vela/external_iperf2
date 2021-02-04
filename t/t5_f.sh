#!/bin/sh -e
. $(dirname $0)/base.sh

iperf -s -P 1 -u -i 1 -t 3 &
sleep 0.5
iperf -c $ip -P 1 -u -b 1m -i 1 -F Makefile
wait

