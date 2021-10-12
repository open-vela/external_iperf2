#!/bin/env python3
#
# ---------------------------------------------------------------
# * Copyright (c) 2021
# * Broadcom Corporation
# * All Rights Reserved.
# *---------------------------------------------------------------
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#
# Redistributions of source code must retain the above copyright notice, this list of conditions
# and the following disclaimer.  Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the documentation and/or other
# materials provided with the distribution.  Neither the name of the Broadcom nor the names of
# contributors may be used to endorse or promote products derived from this software without
# specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
# IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
# OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Author Robert J. McMahon, Broadcom LTD
# Date October 2021
#
# Script flow
#   Parse the command line arguements
#   Set up python logging and logger
#   Instantiate devices or DUTs that are controlled via ssh
#   Instantiate traffic objects
#   Open DUT consoles (will set up ssh control masters and pipe dmesg -w to the logger
#   Disable python garbage collector
#   Run traffic
#   Enable and invoke python garbage collector
#   Shut everything down cleanly
#
import shutil
import logging
import flows
import argparse
import time, datetime
import os,sys
import ssh_nodes
import gc

from flows import *
from ssh_nodes import *

parser = argparse.ArgumentParser(description='Run a bufferbloat test')
parser.add_argument('--host_wan', type=str, default="10.19.85.xx", required=False, help='PC WAN host to run iperf')
parser.add_argument('--host_sta1', type=str, default="10.19.85.xx", required=False, help='STA host to run iperf')
parser.add_argument('--host_sta2', type=str, default="10.19.85.xx", required=False, help='STA host to run iperf')
parser.add_argument('--host_sta3', type=str, default="10.19.85.xx", required=False, help='STA host to run iperf')
parser.add_argument('-i','--interval', type=int, required=False, default=1, help='iperf report interval')
parser.add_argument('-n','--runcount', type=int, required=False, default=2, help='number of runs')
parser.add_argument('-t','--time', type=float, default=10, required=False, help='time or duration to run traffic')
parser.add_argument('-o','--output_directory', type=str, required=False, default='./pyflow_log', help='output directory')
parser.add_argument('--test_name', type=str, default='lat1', required=False)
parser.add_argument('--loglevel', type=str, required=False, default='INFO', help='python logging level, e.g. INFO or DEBUG')

# Parse command line arguments
args = parser.parse_args()

# Set up logging below
logfilename='test.log'
testselect_dir = args.test_name
args.output_directory = os.path.join(args.output_directory, testselect_dir)
if not os.path.exists(args.output_directory):
    print('Making log directory {}'.format(args.output_directory))
    os.makedirs(args.output_directory)

fqlogfilename = os.path.join(args.output_directory, logfilename)
numeric_level = getattr(logging, args.loglevel.upper(), None)
if not isinstance(numeric_level, int):
    raise ValueError('Invalid log level: %s' % args.loglevel)
logging.basicConfig(filename=fqlogfilename, level=numeric_level, format='%(asctime)s %(name)s %(module)s %(levelname)-8s %(message)s')
logging.info("log file setup for {}".format(fqlogfilename), exc_info=True)
print('Writing log to {}'.format(fqlogfilename))

#configure asyncio logging
logging.getLogger('asyncio').setLevel(logging.INFO)
logger = logging.getLogger(__name__)
loop = asyncio.get_event_loop()
loop.set_debug(False)

#instantiate DUT host and NIC devices
pc_wan = ssh_node(name='PC-10G', ipaddr=args.host_wan, device='enp1s0', devip='192.168.1.15')
sta1 = ssh_node(name='4389a', ipaddr=args.host_sta1, device='eth1', devip='192.168.1.233')
sta2 = ssh_node(name='4388b', ipaddr=args.host_sta2, device='eth1', devip='192.168.1.231')
sta3 = ssh_node(name='4388c', ipaddr=args.host_sta3, device='eth1', devip='192.168.1.232')

if args.test_name == 'lat1' :
    #instantiate traffic objects
    trfc1=iperf_flow(name='UDP-LA1', user='root', server=sta1, client=pc_wan, dstip=sta1.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='6001', dstport='6001', offered_load='1M')
    trfc2=iperf_flow(name='UDP-LA2', user='root', server=sta2, client=pc_wan, dstip=sta2.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='6002', dstport='6002', offered_load='1M')
    trfc3=iperf_flow(name='UDP-LIA', user='root', server=sta3, client=pc_wan, dstip=sta3.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='7001', dstport='7001', offered_load='3G')


ssh_node.open_consoles(silent_mode=True)

traffic_flows = iperf_flow.get_instances()
try:
    if traffic_flows:
        for runid in range(args.runcount) :
            for traffic_flow in traffic_flows:
                print("Running ({}/{}) {} traffic client={} server={} dest={} with load {} for {} seconds".format(str(runid+1), str(args.runcount), traffic_flow.proto, traffic_flow.client, traffic_flow.server, traffic_flow.dstip, traffic_flow.offered_load, args.time))
            gc.disable()
            iperf_flow.run(time=args.time, flows='all')
            gc.enable()
            try :
                gc.collect()
            except:
                pass

        for traffc_flow in traffic_flows :
            traffic_flow.compute_ks_table(directory=args.output_directory, title=args.test_name)

    else:
        print("No traffic Flows instantiated per test {}".format(args.test_name))

finally :
    ssh_node.close_consoles()
    if traffic_flows:
        iperf_flow.close_loop()
    logging.shutdown()
