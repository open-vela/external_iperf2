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
import yaml

from flows import *
from ssh_nodes import *
from config_yaml import *
from ptp_test import *

parser = argparse.ArgumentParser(description='Run a bufferbloat test')
parser.add_argument('--config_file', type=str, required=True, help='Optional YAML config file to use')
parser.add_argument('--lsa_offered_load', type=str, default="1M", required=False, help='traffic load for lsa flows')
parser.add_argument('--lia_working_load', type=str, default=None, required=False, help='traffic load for background or congestion flow')
parser.add_argument('-i','--interval', type=int, required=False, default=1, help='iperf report interval')
parser.add_argument('-n','--runcount', type=int, required=False, default=2, help='number of runs')
parser.add_argument('-t','--time', type=float, default=30, required=False, help='time or duration to run traffic')
parser.add_argument('-o','--output_directory', type=str, required=False, default='./pyflow_log', help='output directory')
parser.add_argument('--test_name', type=str, default='lat1', required=False)
parser.add_argument('--loglevel', type=str, required=False, default='INFO', help='python logging level, e.g. INFO or DEBUG')
parser.add_argument('--ptp', dest='have_ptp', action='store_true', help='PTP is assumed', required=False)

parser.set_defaults(have_ptp=False)

# Parse command line arguments
args = parser.parse_args()

# Set up logging below
logfilename='main.log'
testselect_dir = args.test_name
t = '%s' % datetime.now()
testselect_dir = testselect_dir + "_" +  t[:-7]
testselect_dir = testselect_dir.replace(" ", "_")
print("Test directory: '" + testselect_dir + "'")
args.output_directory = os.path.join(args.output_directory, testselect_dir)
if not os.path.exists(args.output_directory):
    print('Making log directory {}'.format(args.output_directory))
    os.makedirs(args.output_directory)

fqlogfilename = os.path.join(args.output_directory, logfilename)
numeric_level = getattr(logging, args.loglevel.upper(), None)
if not isinstance(numeric_level, int):
    raise ValueError('Invalid log level: %s' % args.loglevel)
logging.basicConfig(filename=fqlogfilename, filemode='w', level=numeric_level, format='%(asctime)s %(name)s %(module)s %(levelname)-8s %(message)s')
logging.info("log file setup for {}".format(fqlogfilename))
print('Writing log to {}'.format(fqlogfilename))

#configure asyncio logging
logging.getLogger('asyncio').setLevel(logging.DEBUG)
logger = logging.getLogger(__name__)

yaml = config_yaml(os.path.abspath(args.config_file))

sta1_ip = sta2_ip = sta3_ip = ""

if str(yaml).find("TRAFFIC_DST1") != -1:
    sta1_ip = yaml['TRAFFIC_DST1']['control_ip']
if str(yaml).find("TRAFFIC_DST2") != -1:
    sta2_ip = yaml['TRAFFIC_DST2']['control_ip']
if str(yaml).find("TRAFFIC_DST3") != -1:
    sta3_ip = yaml['TRAFFIC_DST3']['control_ip']

if args.have_ptp:
    test_ptp_clock(False, [yaml['TRAFFIC_SRC']['control_ip'], sta1_ip, sta2_ip, sta3_ip])

loop = asyncio.new_event_loop()
#Make sure other modules which support concurrency use the same loop
ssh_node.loop = loop
iperf_flow.loop = loop
loop.set_debug(False)

stas_list = []

#instantiate DUT host and NIC devices
pc_traffic = ssh_node(name='PC-10G', ipaddr=yaml['TRAFFIC_SRC']['control_ip'], device=yaml['TRAFFIC_SRC']['data_eth_id'], devip=yaml['TRAFFIC_SRC']['data_ip'])
if sta1_ip:
    sta1 = ssh_node(name=yaml['TRAFFIC_DST1']['brcm_chip'], ipaddr=yaml['TRAFFIC_DST1']['control_ip'], device=yaml['TRAFFIC_DST1']['data_eth_id'], devip=yaml['TRAFFIC_DST1']['data_ip'])
    stas_list.append(sta1)
if sta2_ip:
    sta2 = ssh_node(name=yaml['TRAFFIC_DST2']['brcm_chip'], ipaddr=yaml['TRAFFIC_DST2']['control_ip'], device=yaml['TRAFFIC_DST2']['data_eth_id'], devip=yaml['TRAFFIC_DST2']['data_ip'])
    stas_list.append(sta2)
if sta3_ip:
    sta3 = ssh_node(name=yaml['TRAFFIC_DST3']['brcm_chip'], ipaddr=yaml['TRAFFIC_DST3']['control_ip'], device=yaml['TRAFFIC_DST3']['data_eth_id'], devip=yaml['TRAFFIC_DST3']['data_ip'])
    stas_list.append(sta3)

ssh_node.open_consoles(silent_mode=True)

# LSA - latency sensitive with tos VI
# LIA - latency insensitive with tos BE

if args.test_name == 'lat1' :
    #instantiate traffic objects
    # 1M load causes latency without buffer bloat
    # 3G load causes buffer bloat which causes bigger latency
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1dn', user='root', server=sta1, client=pc_traffic, dstip=sta1.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_traffic.devip, srcport='6001', dstport='6001', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2dn', user='root', server=sta2, client=pc_traffic, dstip=sta2.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_traffic.devip, srcport='6002', dstport='6002', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta3_ip:
        trfc3=iperf_flow(name='UDP_LIA1dn', user='root', server=sta3, client=pc_traffic, dstip=sta3.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_traffic.devip, srcport='7001', dstport='7001', offered_load=args.lia_working_load, txstart_delay_sec=1)
elif args.test_name == 'lat1up' :
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1up', user='root', client=sta1, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6011', dstport='6001', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2up', user='root', client=sta2, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta2.devip, srcport='6012', dstport='6002', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta3_ip:
        trfc3=iperf_flow(name='UDP_LIA1dn', user='root', server=sta3, client=pc_traffic, dstip=sta3.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_traffic.devip, srcport='7001', dstport='7001', offered_load=args.lia_working_load, txstart_delay_sec=1)
elif args.test_name == 'lat2' :
    #instantiate traffic objects
    # no LIA means no competition
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1dn', user='root', server=sta1, client=pc_traffic, dstip=sta1.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_traffic.devip, srcport='6111', dstport='6001', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2dn', user='root', server=sta2, client=pc_traffic, dstip=sta2.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_traffic.devip, srcport='6112', dstport='6002', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    del sta3
elif args.test_name == 'lat2up' :
    # no LIA means no competition
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1up', user='root', client=sta1, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6211', dstport='6001', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2up', user='root', client=sta2, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6222', dstport='6002', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    del sta3
elif args.test_name == 'lat3up' :
    # rate 1G for LIA
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1up', user='root', client=sta1, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6211', dstport='6001', offered_load=args.lsa_offered_load, tos="VI", txstart_delay_sec=1)
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LIA1up', user='root', client=sta2, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta2.devip, srcport='6222', dstport='6002', offered_load=args.lia_working_load, txstart_delay_sec=1)
    if sta3_ip:
        trfc3=iperf_flow(name='UDP_LIA2up', user='root', client=sta3, server=pc_traffic, dstip=pc_traffic.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta3.devip, srcport='7011', dstport='7001', offered_load=args.lia_working_load, txstart_delay_sec=1)
elif args.test_name == 'bb' :
    trfc1=iperf_flow(name='TCP_BBup', user='root', client=sta1, server=pc_traffic, dstip=pc_traffic.devip, proto='TCP', interval=1, debug=False, window='24M', srcip=sta1.devip, bounceback=True, bounceback_congest=True, txstart_delay_sec=1)
else :
    try :
        os.remove(fqlogfilename)
    except:
        pass
    try:
        os.rmdir(args.output_directory)
    except OSError as e:
        print("Error: %s : %s" % (args.output_directory, e.strerror))
    sys.exit('Unknown test named {}'.format(args.test_name))

traffic_flows = iperf_flow.get_instances()

if args.ampdudump:
    for sta in stas_list :
        cmd = "wl dump_clear ampdu"
        logging.info('ampdu clear for STA {} using ({})'.format(str(sta.ipaddr), cmd))
        ret = sta.rexec(cmd=cmd, run_now=True)
        cmd_results = ret.results.decode()
        cmd_results = cmd_results.splitlines()

        for line in cmd_results:
            logging.info('{}'.format(str(line)))

try:
    if traffic_flows:
        for runid in range(args.runcount) :
            run_log_file = os.path.join(args.output_directory, 'main_run{}.log'.format(str(runid + 1)))
            logging.basicConfig(filename=run_log_file, force=1, filemode='w', level=numeric_level, format='%(asctime)s %(name)s %(module)s %(levelname)-8s %(message)s')
            for traffic_flow in traffic_flows:
                tmp = "Running ({}/{}) {} traffic client={} server={} dest={} with load {} for {} seconds".format(str(runid+1), str(args.runcount), traffic_flow.proto, traffic_flow.client, traffic_flow.server, traffic_flow.dstip, traffic_flow.offered_load, args.time)
                logging.info(tmp)
                print(tmp)
            gc.disable()
            iperf_flow.run(time=args.time, flows='all')
            gc.enable()
            try :
                gc.collect()
            except:
                pass

        end_log_file = os.path.join(args.output_directory, 'main_end.log')
        logging.basicConfig(filename=end_log_file, force=1, filemode='w', level=numeric_level, format='%(asctime)s %(name)s %(module)s %(levelname)-8s %(message)s')

        for traffic_flow in traffic_flows :
            traffic_flow.compute_ks_table(args.runcount, directory=args.output_directory, title=args.test_name)
    else:
        print("No traffic Flows instantiated per test {}".format(args.test_name))

finally :

    ssh_node.close_consoles()
    if traffic_flows:
        iperf_flow.close_loop()
    logging.shutdown()
