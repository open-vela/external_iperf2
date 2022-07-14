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
# Author Kevin Mirabadi, Broadcom LTD
# Date Nov 2021
#

import time, datetime
import re

import ssh_nodes
import logging


from ssh_nodes import *
from datetime import datetime as datetime, timezone

logger = logging.getLogger(__name__)
    
def check_ptp(sta):

    logging.info("============================================ check_ptp ============================================\n")
    
    for retry_cnt in range(1, 4, 1):

        logging.info("")
        logging.info("=========================== Command pmc try " + str(retry_cnt) + " ===========================")
            
        cnt1 = 0
        cnt2 = 0
        cnt3 = 0
        ptp_failed_flag = False

        # Use command "pmc -u -b 2 'GET TIME_STATUS_NP' -d 100"
        # Check for 'master_offset' (unit in nanoseconds), 'gmPresent', and 'gmIdentity'
        # if 12 out of 17 items are good then it passes

        cmd = "pmc -u -b 2 'GET TIME_STATUS_NP' -d 100"
        logging.info('Check PTP for PC {} using ({})'.format(sta.ipaddr, cmd))
        ret = sta.rexec(cmd=cmd, run_now=True)
        cmd_results = ret.results.decode()
        cmd_results = cmd_results.splitlines()

        for line in cmd_results:
            if re.search('master_offset', line, re.IGNORECASE):
                m = re.match( r'.*master_offset.*?(?P<value>-?\d+)', line, re.M|re.I)
                if m:
                    x = int(m.group('value'))
                    # offset up to 2 us is ok
                    if x < 2000 and x > -2000:
                        cnt1 += 1
            if re.search('gmPresent', line, re.IGNORECASE):
                m = re.match( r'.*gmPresent.*?(?P<value>\w+)', line, re.M|re.I)
                if m:
                    if m.group('value') == "true":
                        cnt2 += 1
            if re.search('gmIdentity', line, re.IGNORECASE):
                m = re.match( r'.*gmIdentity.*?(?P<value>\w+.\w+.\w+)', line, re.M|re.I)
                if m:
                    if m.group('value') == "6cb311.fffe.071a40":
                        cnt3 += 1

        if cnt1 == 0 :
            msg = '***Failed. PC {} ptp4l is not running.'.format(sta.ipaddr)
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
            logging.info("")
            sta.rexec(cmd="journalctl -u ptp4l -n 10", run_now=True)            
            break
        if cnt1 < 13 :
            # Don't print "Fail" with the first try and do a retry first
            if retry_cnt > 1 :
                msg = "***Failed ptp4l 'master_offset'. PC {0} only found {1} items between 2000 and -2000".format(sta.ipaddr, cnt1)          
                logging.info(msg)
                print(msg)
            ptp_failed_flag = True
            # wait some more to see if the service become more stable
            time.sleep(20)
        else :
            logging.info("Passed. {} good 'ptp4l master_offset' numbers.".format(str(cnt1)))
        if cnt2 < 13 :
            msg = "***Failed ptp4l 'gmPresent'. PC {0} only found {1} items with 'true'".format(sta.ipaddr, cnt2)          
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
        else :
            logging.info("Passed. {} good 'ptp4l gmPresent' numbers.".format(str(cnt2)))
        if cnt3 < 13 :
            msg = "***Failed ptp4l 'gmIdentity'. PC {0} only found {1} items with '6cb311.fffe.071a40'".format(sta.ipaddr, cnt3)          
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
        else :
            logging.info("Passed. {} good 'ptp4l gmIdentity' numbers.".format(str(cnt3)))

        if ptp_failed_flag == False :
            break

        logging.info("Retry pmc command.")

    if ptp_failed_flag == True :
        return False

    for retry_cnt in range(1, 4, 1):

        logging.info("")
        logging.info("=========================== Command 'journalctl ptp4l' try " + str(retry_cnt) + " ===========================")
            
        cnt1 = 0
        ptp_failed_flag = False

        # Use command "journalctl ptp4l" for the last 20 secs
        # Check for 'master_offset'
        # if 15 out of 20 items are good then it passes
        
        cmd = "journalctl -u ptp4l --since '20 second ago'"
        logging.info("")
        logging.info('Check PTP for PC {} using ({})'.format(sta.ipaddr, cmd))
        ret = sta.rexec(cmd=cmd, run_now=True)
        cmd_results = ret.results.decode()
        cmd_results = cmd_results.splitlines()

        journalctl_line_cnt = 0
        for line in cmd_results:
            m = re.match( r'.*master offset.*?(?P<value>-?\d+).*', line, re.M|re.I)
            if m:
                journalctl_line_cnt += 1
                x = int(m.group('value'))
                # offset up to 2 us is ok
                if x < 2000 and x > -2000:
                    cnt1 += 1

        if journalctl_line_cnt == 0 :
            msg = '***Failed. PC {} ptp4l is not running.'.format(sta.ipaddr)
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
            logging.info("")
            sta.rexec(cmd="journalctl -u ptp4l -n 10", run_now=True)            
            break
        elif journalctl_line_cnt > 30 :
            msg = '***Failed. PC {} ptp4l shows too many log lines.'.format(sta.ipaddr)
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
            break
        elif cnt1 > 15 :
            logging.info("Passed. {} good 'ptp4l master_offset' numbers.".format(str(cnt1)))
            break
            
        logging.info("Retry. Only {} good 'ptp4l master_offset' numbers.".format(str(cnt1)))
        
        # wait some more to see if the service become more stable
        time.sleep(20)

    if ptp_failed_flag == True :
        return False

    for retry_cnt in range(1, 4, 1):

        logging.info("")
        logging.info("=========================== Command 'journalctl phc2sys' try " + str(retry_cnt) + " ===========================")
            
        cnt1 = 0
        ptp_failed_flag = False

        # Use command "journalctl phc2sys" for the last 20 secs
        # Check for 'offset'
        # if 10 out of 20 items are good then it passes
        # Every minutes show about 15 big numbers (why?)

        cmd = "journalctl -u phc2sys --since '20 second ago'"
        logging.info("")
        logging.info('Check PTP for PC {} using ({})'.format(sta.ipaddr, cmd))
        ret = sta.rexec(cmd=cmd, run_now=True)
        cmd_results = ret.results.decode()
        cmd_results = cmd_results.splitlines()

        journalctl_line_cnt = 0
        for line in cmd_results:
            m = re.match( r'.*phc offset.*?(?P<value>-?\d+).*', line, re.M|re.I)
            if m:
                journalctl_line_cnt += 1
                x = int(m.group('value'))
                # offset up to 2 us is ok
                if x < 2000 and x > -2000:
                    cnt1 += 1

        if journalctl_line_cnt == 0:
            msg = '***Failed. PC {} ptp phc2sys is not running.'.format(sta.ipaddr)
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
            logging.info("")
            sta.rexec(cmd="journalctl -u phc2sys -n 10", run_now=True)            
            break
        elif journalctl_line_cnt > 30:
            msg = '***Failed. PC {} ptp phc2sys shows too many log lines.'.format(sta.ipaddr)
            logging.info(msg)
            print(msg)
            ptp_failed_flag = True
            break
        elif cnt1 > 10 :
            logging.info("Passed. {} good 'phc2sys offset' numbers.".format(str(cnt1)))
            break
            
        logging.info("Retry. Only {} good 'phc2sys offset' numbers.".format(str(cnt1)))
        
        # wait 10 secs to skip some of the big offset numbers that happens every minute
        time.sleep(10)

    if ptp_failed_flag == True :
        msg = '***Failed. PC {} ptp4l/phc2sys is not running correctly.'.format(sta.ipaddr)
        return False

    # Passed
    return True        


def restart_ptp(sta):

    logging.info("")
    logging.info("=========================== restart_ptp for PC " + str(sta.ipaddr) + " ===========================\n")

    # journalctl --vacuum-time=2d
    
    cmd = "systemctl stop phc2sys"
    logging.info('PC {} Command: {}'.format(sta.ipaddr, cmd))
    ret = sta.rexec(cmd=cmd, run_now=True)

    cmd = "systemctl stop ptp4l"
    logging.info('PC {} Command: {}'.format(sta.ipaddr, cmd))
    ret = sta.rexec(cmd=cmd, run_now=True)
    
    time.sleep(1)

    cmd = "systemctl start ptp4l"
    logging.info('PC {} Command: {}'.format(sta.ipaddr, cmd))
    ret = sta.rexec(cmd=cmd, run_now=True)

    cmd = "systemctl start phc2sys"
    logging.info('PC {} Command: {}'.format(sta.ipaddr, cmd))
    ret = sta.rexec(cmd=cmd, run_now=True)

    msg = "Wait 2 minutes"
    logging.info(msg)
    print(msg)
    time.sleep(120)
    
    cmd = "systemctl status ptp4l"
    logging.info('PC {} Command: {}'.format(sta.ipaddr, cmd))
    ret = sta.rexec(cmd=cmd, run_now=True)

    cmd = "systemctl status phc2sys"
    logging.info('PC {} Command: {}'.format(sta.ipaddr, cmd))
    ret = sta.rexec(cmd=cmd, run_now=True)

    
def test_ptp_clock(ptp_recover_flag, stas_lan_ip_list):

    # Remove duplicate and blank sta_lan_ip from the list
    new_stas_lan_ip_list = []
    [new_stas_lan_ip_list.append(x) for x in stas_lan_ip_list if x not in new_stas_lan_ip_list and x != '']

    rc_final = True
    
    for sta_lan_ip in new_stas_lan_ip_list:

        rc = False

        sta = ssh_node(name='ptp', ipaddr=sta_lan_ip)
        ssh_node.open_consoles(silent_mode=True)

        for retry_cnt in range(1, 4, 1):

            msg = ""

            if not check_ptp(sta):
                if retry_cnt >= 3 or not ptp_recover_flag:
                    logging.info("")
                    break
                else:
                    print("Restart PTP4l services for PC " + sta.ipaddr)
                    restart_ptp(sta)
            else:
                msg = 'PC {} ptp4l/phc2sys is running correctly.'.format(sta.ipaddr)
                rc = True
                break
        
        # node close taken out since it causes failure
        #ssh_node.close_consoles()

        if msg != "" :
            logging.info("")
            logging.info(msg)
            print(msg)
            logging.info("")
        
        if rc == False and rc_final == True:
            rc_final = False

    if rc_final == False:
        msg = '***Failed. ptp4l issue. Stop.'.format()
        logging.info("")
        logging.info(msg)
        print(msg)
        logging.info("")
        exit(-1)

    return rc_final        
    