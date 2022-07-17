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
import netlink

from flows import *
from ssh_nodes import *
from config_yaml import *
from ptp_test import *
from netlink import *

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
parser.add_argument('--apdump', dest='apdump', action='store_true', help='Enable AP dump', required=False)
parser.add_argument('--netlink', dest='netlink', action='store_true', help='Enable netlink telemetry', required=False)
parser.add_argument('--ptp_recover', dest='ptp_recover', action='store_true', help='Recover PTP after failure', required=False)

parser.set_defaults(netlink=False)
parser.set_defaults(ptp_recover=False)
parser.set_defaults(apdump=False)

# Parse command line arguments
args = parser.parse_args()

if args.ptp_recover:
    ptp_recover_flag = True
else:
    ptp_recover_flag = False

# Set up logging below
logfilename='test.log'
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

if str(yaml).find("STA1") != -1:
    sta1_ip = yaml['STA1']['lan_ip']
if str(yaml).find("STA2") != -1:
    sta2_ip = yaml['STA2']['lan_ip']
if str(yaml).find("STA3") != -1:
    sta3_ip = yaml['STA3']['lan_ip']

# test_ptp_clock(ptp_recover_flag, [yaml['WAN']['wan_ip'], sta1_ip, sta2_ip, sta3_ip])

loop = asyncio.new_event_loop()
loop.set_debug(False)

#instantiate DUT host and NIC devices
pc_wan = ssh_node(name='PC-10G', ipaddr=yaml['WAN']['wan_ip'], device=yaml['WAN']['eth_id'], devip=yaml['WAN']['device_ip'])
if sta1_ip:
    sta1 = ssh_node(name=yaml['STA1']['chip'], ipaddr=yaml['STA1']['lan_ip'], device=yaml['STA1']['eth_id'], devip=yaml['STA1']['device_ip'])
if sta2_ip:
    sta2 = ssh_node(name=yaml['STA2']['chip'], ipaddr=yaml['STA2']['lan_ip'], device=yaml['STA2']['eth_id'], devip=yaml['STA2']['device_ip'])
if sta3_ip:
    sta3 = ssh_node(name=yaml['STA3']['chip'], ipaddr=yaml['STA3']['lan_ip'], device=yaml['STA3']['eth_id'], devip=yaml['STA3']['device_ip'])

ssh_node.open_consoles(silent_mode=True)

netlinks = []
if args.netlink :
     if sta1_ip and yaml['STA1']['chip'].startswith('4389') :
         netlinks.extend([netlink_pktts(sshnode=sta1, debug=True, silent=False, chip='4389')])
     if sta2_ip and yaml['STA2']['chip'].startswith('4389') :
         netlinks.extend([netlink_pktts(sshnode=sta2, debug=True, silent=False, chip='4389')])
     if sta3_ip and yaml['STA3']['chip'].startswith('4389') :
         netlinks.extend([netlink_pktts(sshnode=sta3, debug=True, silent=False, chip='4389')])
     if netlinks :
         netlink_pktts.commence()

if args.apdump:
    ap = ssh_node(name='ap', ipaddr=yaml['AP']['lan_ip'], sshtype='ush', relay=yaml['AP']['relay'])

# LSA - latency sensitive with tos VI
# LIA - latency insensitive with tos BE

if args.test_name == 'lat1' :
    #instantiate traffic objects
    # 1M load causes latency without buffer bloat
    # 3G load causes buffer bloat which causes bigger latency
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1', user='root', server=sta1, client=pc_wan, dstip=sta1.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='6001', dstport='6001', offered_load=args.lsa_offered_load, tos="VI")
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2', user='root', server=sta2, client=pc_wan, dstip=sta2.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='6002', dstport='6002', offered_load=args.lsa_offered_load, tos="VI")
    if sta3_ip:
        trfc3=iperf_flow(name='UDP_LIA1', user='root', server=sta3, client=pc_wan, dstip=sta3.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='7001', dstport='7001', offered_load=args.lia_working_load)
elif args.test_name == 'lat1up' :
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1', user='root', client=sta1, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6011', dstport='6001', offered_load=args.lsa_offered_load, tos="VI")
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2', user='root', client=sta2, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta2.devip, srcport='6012', dstport='6002', offered_load=args.lsa_offered_load, tos="VI")
    if sta3_ip:
        trfc3=iperf_flow(name='UDP_LIA1', user='root', server=sta3, client=pc_wan, dstip=sta3.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='7001', dstport='7001', offered_load=args.lia_working_load)
elif args.test_name == 'lat2' :
    #instantiate traffic objects
    # no LIA means no competition
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1', user='root', server=sta1, client=pc_wan, dstip=sta1.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='6111', dstport='6001', offered_load=args.lsa_offered_load, tos="VI")
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2', user='root', server=sta2, client=pc_wan, dstip=sta2.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=pc_wan.devip, srcport='6112', dstport='6002', offered_load=args.lsa_offered_load, tos="VI")
    del sta3
elif args.test_name == 'lat2up' :
    # no LIA means no competition
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1', user='root', client=sta1, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6211', dstport='6001', offered_load=args.lsa_offered_load, tos="VI")
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LSA2', user='root', client=sta2, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6222', dstport='6002', offered_load=args.lsa_offered_load, tos="VI")
    del sta3
elif args.test_name == 'lat3up' :
    # rate 1G for LIA
    if sta1_ip:
        trfc1=iperf_flow(name='UDP_LSA1', user='root', client=sta1, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta1.devip, srcport='6211', dstport='6001', offered_load=args.lsa_offered_load, tos="VI")
    if sta2_ip:
        trfc2=iperf_flow(name='UDP_LIA1', user='root', client=sta2, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta2.devip, srcport='6222', dstport='6002', offered_load=args.lia_working_load)
    if sta3_ip:
        trfc3=iperf_flow(name='UDP_LIA2', user='root', client=sta3, server=pc_wan, dstip=pc_wan.devip, proto='UDP', interval=1, debug=False, window='24M', srcip=sta3.devip, srcport='7011', dstport='7001', offered_load=args.lia_working_load)
elif args.test_name == 'bb' :
        trfc1=iperf_flow(name='TCP_BB', user='root', client=sta1, server=pc_wan, dstip=pc_wan.devip, proto='TCP', interval=1, debug=False, window='24M', srcip=sta1.devip, bounceback=True, bounceback_congest=True)
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

ssh_node.open_consoles(silent_mode=True)

traffic_flows = iperf_flow.get_instances()

try:
    if traffic_flows:
        for runid in range(args.runcount) :
            for traffic_flow in traffic_flows:
                tmp = "Running ({}/{}) {} traffic client={} server={} dest={} with load {} for {} seconds".format(str(runid+1), str(args.runcount), traffic_flow.proto, traffic_flow.client, traffic_flow.server, traffic_flow.dstip, traffic_flow.offered_load, args.time)
                logging.info(tmp)
                print(tmp)
            gc.disable()
            iperf_flow.run(time=args.time, flows='all')
            for netlink in netlinks :
                netlink.await_io_finish()
            gc.enable()
            for netlink in netlinks :
                logging.info('Netlink: {} stdout={} stderr={}'.format(netlink.name, netlink.stdout_linecount, netlink.stderr_linecount))
                print('Netlink: {} stdout={} stderr={}'.format(netlink.name, netlink.stdout_linecount, netlink.stderr_linecount))
                if (netlink.stdout_linecount > 10) :
                    directory = os.path.join(args.output_directory, 'netlink_histograms', 'run{}'.format(runid))
                    logging.info('Netlink create {} histograms in directory {}'.format(netlink.name,  directory))
                    try :
                        logging.info('Flow id to name table = {}'.format(iperf_flow.flowid2name))
                        netlink_pktts.CreateHistograms(directory=directory, run_number=runid, starttime=trfc1.starttime, endtime=trfc1.endtime, testtitle='LSA', flowtable=iperf_flow.flowid2name, population_min = 10)
                    except:
                        raise
            netlink_pktts.ResetStats()
            try :
                gc.collect()
            except:
                pass

        for traffic_flow in traffic_flows :
            traffic_flow.compute_ks_table(directory=args.output_directory, title=args.test_name)
    else:
        print("No traffic Flows instantiated per test {}".format(args.test_name))

    if args.apdump:
        ret = ap.rexec(cmd='/bin/hw_accel_dump.sh', run_now=True)
        #print(ret)

finally :
    if args.netlink :
        netlink_pktts.cease()

    ssh_node.close_consoles()

    if traffic_flows:
        iperf_flow.close_loop()

    logging.shutdown()
