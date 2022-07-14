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
# Date Dec 2021
#

import yaml
import ssh_nodes
import logging

from ssh_nodes import *

logger = logging.getLogger(__name__)

def config_yaml(config_file_name):
    print("Loading test config file: " + config_file_name)

    test_yaml_file = open(config_file_name)
    test_data = yaml.safe_load(test_yaml_file)

    # get Test
    test_yaml_name = test_data.get('TEST').get('NAME')
    print("Running test: " + test_yaml_name)

    # get Testbed
    testbed_yaml_name = test_data.get('TESTBED').get('yaml_file')
    print("Loading testbed config file: " + testbed_yaml_name)

    testbed_yaml_file = open(testbed_yaml_name)
    testbed_data = yaml.safe_load(testbed_yaml_file)

    yaml_test = {}

    # process each TEST item
    for test_item in test_data.get(test_yaml_name):
        test_item_val = test_data.get(test_yaml_name).get(test_item)

        tmp_print_str = "Processing YAML Test Item: " + test_item + ": " + test_item_val

        test_item_val = test_item_val.split(' ')

        #print("test_item_val=" + str(test_item_val))

        yaml_test[test_item] = {}
        data_dhcp_ip_flag = False

        # process each test item
        for testbed_item, item_value in testbed_data[test_item_val[0]].items():

            #print(str(test_item_val[0]) + " testbed_item=" + str(testbed_item) + " item_value=" + str(item_value))

            # LAN/Control IP
            if testbed_item == "control_ip":
                yaml_test[test_item]['control_ip'] = item_value
                if tmp_print_str:
                    tmp_print_str += "\t(" + item_value + ")"
                    print(tmp_print_str)
                    logging.info(tmp_print_str)                    
                    tmp_print_str = ""

            # WLAN Broadcom chip id
            elif testbed_item == "brcm_chip":
                if item_value == None:
                    item_value = 'None'
                yaml_test[test_item]['brcm_chip'] = item_value

            # Is WLAN IP dhcp?
            elif testbed_item == "data_dhcp_ip":
                data_dhcp_ip_flag = item_value

            # WLAN data IP
            elif testbed_item == "data_ip":
                yaml_test[test_item]['data_ip'] = item_value

            # WLAN data ethernet id
            elif testbed_item == "data_eth_id":
                yaml_test[test_item]['data_eth_id'] = item_value

            # multi port LAN card
            elif len(test_item_val) > 1 and test_item_val[1] == testbed_item:
                for dev_item, dev_value in item_value.items():
                    if dev_item == "data_dhcp_ip":
                        data_dhcp_ip_flag = dev_value
                    elif dev_item == "data_ip":
                        yaml_test[test_item]['data_ip'] = dev_value                        
                    elif dev_item == "data_eth_id":
                        yaml_test[test_item]['data_eth_id'] = dev_value

            # AP or STA Port Forwarding ip
            elif testbed_item == "port_forward":
                if item_value is not None:
                    port_forward_value = testbed_data.get(item_value).get('control_ip')
                    yaml_test[test_item]['port_forward'] = port_forward_value

                # process dhcp at the end of STA options
                if data_dhcp_ip_flag:
                    data_dhcp_ip_flag = False
                    sta_ip = yaml_test[test_item]['control_ip']
                    dhcp_ip = get_dhcp_ip(sta_ip, yaml_test[test_item]['data_eth_id'])
                    yaml_test[test_item]['data_ip'] = dhcp_ip
                    tmp_str = test_item_val[0] + " " + sta_ip + " DHCP device IP is " + dhcp_ip
                    print(tmp_str)
                    logging.info(tmp_str)                    

    return yaml_test

def get_dhcp_ip(sta_control_ip, sta_data_eth_id):

    sta = ssh_node(name=sta_control_ip, ipaddr=sta_control_ip)
    ssh_node.open_consoles(silent_mode=True)

    print('{} Command: ifconfig {}'.format(sta_control_ip, sta_data_eth_id))
    ret = sta.rexec(cmd='ifconfig {}'.format(sta_data_eth_id), run_now=True)

    ssh_node.close_consoles()

    ret2 = str(bytes(ret.results))
    x = re.split("inet ", ret2, 1)
    x2 = re.split(" ", x[1], 1)
    dev_ip_val = x2[0]
    
    return dev_ip_val
  