# *---------------------------------------------------------------
# * Copyright (c) 2018
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
# Date April 2016

import re
import subprocess
import logging
import asyncio, sys
import time, datetime
import locale
import signal
import weakref
import os
import ssh_nodes
import collections
import math
import scipy
import scipy.spatial
import numpy as np
import tkinter
import matplotlib.pyplot as plt
import concurrent.futures
import functools
import csv

from datetime import datetime as datetime, timezone
from scipy import stats
from scipy.cluster import hierarchy
from scipy.cluster.hierarchy import linkage
from ssh_nodes import *
from math import floor
from collections import namedtuple, defaultdict, Counter

# See pages 10 and 13 of https://docs.google.com/document/d/1a2Vo0AUBMo1utUWYLkErSSqQM9sMf0D9FPfyni_e4Qk/edit#heading=h.9lme8ct208v3
#
# Probe points: Tx Path*
#   T8Tx - Frame/Pkt generated timestamp at Application
#   DhdT0 - DHD Driver xmits pkt to dongle
#   DhdT5 - DHD receives Tx-Completion from FW
#   FWT1  - Firmware sees the TxPost work item from host
#   FWT2  - Firmware submits the TxPost work item to Mac tx DMA (after header conversion)
#   FWT3  - Firmware processes TxStatus from uCode
#   FWT4  - Firmware posts Tx-Completion message to host
# Rx Path*
#   FWR1 - RxStatus TSF as reported by Ucode (time at which pkt was rxed by the MAC)
#   FWR2 - Time at which Rx-Completion is posted to host.
#   DRRx - DHD Driver process Rx-Completion and forwards Rxed pkt to Network Stack
#   T8Rx - Frame/Packet Rxed by Application
class FlowPoint(object):

    def __init__(self):
        self.flowid = None
        self.seqno = None
        self._dhdt0gps = None
        self._dhdt5gps = None
        self._dhdr3gps = None
        self._fwt1tsf = None
        self._fwt2tsf = None
        self._fwt3tsf = None
        self._fwt4tsf = None
        self._fwr1tsf = None
        self._fwr2tsf = None
        self.tsf_txdrift = 0
        self.tsf_rxdrift = 0
        self.tsfgps_txt0 = None
        self.tsfgps_rxt0 = None
        self.tsf_rxt0 = None
        self.tsf_txt0 = None
        # Type 3 below
        self._pktfetch = None
        self.media_ac_delay = None
        self.rxdur = None
        self.mac_suspend = None
        self._txstatus = None
        self.txencnt = None
        self.datatxscnt = None
        self.oactxscnt = None
        self.rtstxcnt = None
        self.ctsrxcnt = None

    @property
    def dhdr3gps(self) :
        if self._dhdr3gps :
            return self._dhdr3gps
        else :
            return None
    @dhdr3gps.setter
    def dhdr3gps(self, value) :
        self._dhdr3gps = value * 1000

    @property
    def dhdt0gps(self) :
        if self._dhdt0gps :
            return self._dhdt0gps
        else :
            return None
    @dhdt0gps.setter
    def dhdt0gps(self, value) :
        self._dhdt0gps = value * 1000

    @property
    def dhdt5gps(self) :
        if self._dhdt5gps :
            return self._dhdt5gps
        else :
            return None
    @dhdt5gps.setter
    def dhdt5gps(self, value) :
        self._dhdt5gps = value * 1000

    @property
    def fwt1tsf(self) :
        return self._fwt1tsf
    @fwt1tsf.setter
    def fwt1tsf(self, value) :
        if value < self.tsf_txt0 :
            value +=  (1<<32)
        self._fwt1tsf = value
    @property
    def fwt2tsf(self) :
        return self._fwt2tsf
    @fwt2tsf.setter
    def fwt2tsf(self, value) :
        if value < self.tsf_txt0 :
            value +=  (1<<32)
        self._fwt2tsf = value
    @property
    def fwt3tsf(self) :
        return self._fwt3tsf
    @fwt3tsf.setter
    def fwt3tsf(self, value) :
        if value < self.tsf_txt0 :
            value +=  (1<<32)
        self._fwt3tsf = value
    @property
    def fwt4tsf(self) :
        return self._fwt4tsf
    @fwt4tsf.setter
    def fwt4tsf(self, value) :
        if value < self.tsf_txt0 :
            value +=  (1<<32)
        self._fwt4tsf = value
    @property
    def fwr1tsf(self) :
        return self._fwr1tsf
    @fwr1tsf.setter
    def fwr1tsf(self, value) :
        if value < self.tsf_rxt0 :
            value +=  (1<<32)
        self._fwr1tsf = value
    @property
    def fwr2tsf(self) :
        return self._fwr2tsf
    @fwr2tsf.setter
    def fwr2tsf(self, value) :
        if value < self.tsf_rxt0 :
            value +=  (1<<32)
        self._fwr2tsf = value

    @property
    def fwt1gps(self) :
        if self.fwt1tsf and self.tsfgps_txt0:
            return ((self.tsfgps_txt0 + (self.fwt1tsf / 1000000.0) + (self.tsf_txdrift / 1000000.0)) * 1000)
        else :
            return None

    @property
    def fwt2gps(self) :
        if self.fwt2tsf and self.tsfgps_txt0:
            return ((self.tsfgps_txt0 + (self.fwt2tsf / 1000000.0) + (self.tsf_txdrift / 1000000.0)) * 1000)
        else :
            return None

    @property
    def fwt3gps(self) :
        if self.fwt3tsf and self.tsfgps_txt0:
            return ((self.tsfgps_txt0 + (self.fwt3tsf / 1000000.0) + (self.tsf_txdrift / 1000000.0)) * 1000)
        else :
            return None

    @property
    def fwt4gps(self) :
        if self.fwt4tsf and self.tsfgps_txt0:
            return ((self.tsfgps_txt0 + (self.fwt4tsf / 1000000.0) + (self.tsf_txdrift / 1000000.0)) * 1000)
        else :
            return None

    @property
    def fwr1gps(self) :
        if self.fwr1tsf and self.tsfgps_txt0:
            return ((self.tsfgps_rxt0 + (self.fwr1tsf / 1000000.0) + (self.tsf_rxdrift / 1000000.0)) * 1000)
        else :
            return None

    @property
    def fwr2gps(self):
        if self.fwr2tsf and self.tsfgps_txt0:
            return ((self.tsfgps_rxt0 + (self.fwr2tsf / 1000000.0) + (self.tsf_rxdrift / 1000000.0)) * 1000)
        else :
            return None
    # type 3 below

    @property
    def pktfetchtsf(self) :
        return self._pktfetch
    @pktfetchtsf.setter
    def pktfetchtsf(self, value) :
        if self.tsf_txt0 is not None and (value < self.tsf_txt0) :
            value +=  (1<<32)
        self._pktfetch = value

    @property
    def pktfetchgps(self) :
        if self.pktfetchtsf and self.tsfgps_txt0:
            return ((self.tsfgps_txt0 + (self.pktfetchtsf / 1000000.0) + (self.tsf_txdrift / 1000000.0)) * 1000)
        else :
            return None

    @property
    def txstatustsf(self) :
        return self._txstatus
    @txstatustsf.setter
    def txstatustsf(self, value) :
        if self.tsf_txt0 is not None and (value < self.tsf_txt0) :
            value +=  (1<<32)
        self._txstatus = value
    @property
    def txstatusgps(self) :
        if self.txstatustsf and self.tsfgps_txt0:
            return ((self.tsfgps_txt0 + (self.txstatustsf / 1000000.0) + (self.tsf_txdrift / 1000000.0)) * 1000)
        else :
            return None

    # Delta timings below

    @property
    def txfw_total_tsf(self):
        if self.fwt2tsf and self.fwt1tsf :
            return (self.fwt2tsf - self.fwt1tsf)
        else :
            return None

    @property
    def host_total(self):
        if self.dhdr3gps and self.dhdt0gps :
            return (self.dhdr3gps - self.dhdt0gps)
        else :
            return None
    @property
    def tx_airwithtxstatus_tsf(self):
        if self.fwt3tsf and self.fwt2tsf:
            return (self.fwt3tsf - self.fwt2tsf)
        else :
            return None

    @property
    def tx_total(self):
        if self.fwt2gps and self.dhdt0gps :
            return (self.fwt2gps - self.dhdt0gps)
        else :
            return None

    @property
    def tx_airwithtxstatus(self):
        if self.fwt3gps and self.fwt2gps:
            return (self.fwt3gps - self.fwt2gps)
        else :
            return None

    @property
    def rxfw_total(self):
        if self.fwr2gps and self.fwr1gps :
            return (self.fwr2gps - self.fwr1gps)
        else :
            return None
    @property
    def rxfw_total_tsf(self):
        if self.fwr2tsf and self.fwr1tsf :
            return (self.fwr2tsf - self.fwr1tsf)
        else :
            return None
    @property
    def rxfwdhd(self):
        if self.dhdr3gps and self.fwr2gps:
            return(self.dhdr3gps - self.fwr2gps)
        else :
            return None
    @property
    def txdhdfw1(self):
        if self.fwt1gps and self.dhdt0gps:
            return(self.fwt1gps - self.dhdt0gps)
        else :
            return None
    @property
    def tx_air(self):
        if self.fwr1gps and self.fwt2gps :
            return (self.fwr1gps - self.fwt2gps)
        else :
            return None

    @property
    def txfw_total(self):
        if self.fwt2gps and self.fwt1gps:
            return (self.fwt2gps - self.fwt1gps)
        else :
            return None


    # This is the order of tsf timestamps:
    # fwt1 < fwt2 < uc_pktfetch < uc_txstatus < fwt3 < fwt4
    #
    # Type 3 deltas:
    # BoffTime = uCMacAccDly - Rx_Duration
    # ucdmadly = T2 - ucdma;
    # Ucpacketdly = uctxstatus - ucdma;
    # drvstsdly = T3 - uctxstatus;
    # drvdma2txsdly = T3 - T2

    @property
    def BoffTime(self) :
        if self.media_ac_delay is not None and self.rxdurtsf is not None:
            return (self.media_ac_delay - self.rxdurtsf)
        else :
            return None
    @property
    def ucdmadly(self) :
        if self.fwt2tsf and self.pktfetchtsf:
            return (int(self.pktfetchtsf - self.fwt2tsf))
        else :
            return None
    @property
    def ucpacketdly(self) :
        if self.txstatustsf and self.pktfetchtsf:
            return (int(self.txstatustsf - self.pktfetchtsf))
        else :
            return None
    @property
    def drvstsdly(self) :
        if self.txstatustsf and self.fwt3tsf:
            return (int(self.fwt3tsf - self.txstatustsf))
        else :
            return None
    @property
    def drvdma2txsdly(self) :
        if self.fwt2tsf and self.fwt3tsf:
            return (int(self.fwt3tsf - self.fwt2tsf))
        else :
            return None

#pktlat tx only histograms
    @property
    def txdhdfw3(self):
        if self.fwt3gps and self.dhdt0gps:
            return(self.fwt3gps - self.dhdt0gps)
        else :
            return None

    @property
    def txdhdfw4(self):
        if self.fwt4gps and self.dhdt0gps:
            return(self.fwt4gps - self.dhdt0gps)
        else :
            return None

    @property
    def fw4fw3(self):
        if self.fwt4tsf and self.fwt3tsf:
            return((self.fwt4tsf - self.fwt3tsf)/1000.0)
        else :
            return None

    @property
    def txcomplete(self):
        return (self.dhdt0gps \
                and self.dhdt5gps \
                and self.fwt1tsf \
                and self.fwt2tsf \
                and self.fwt3tsf \
                and self.fwt4tsf \
                and self.tsfgps_txt0)

    @property
    def complete(self):
        return (self.dhdt0gps \
                and self.dhdt5gps \
                and self.fwt1tsf \
                and self.fwt2tsf \
                and self.fwt3tsf \
                and self.fwt4tsf \
                and self.dhdr3gps \
                and self.fwr1tsf \
                and self.fwr2tsf \
                and self.tsfgps_rxt0 \
                and self.tsfgps_txt0)

    @property
    def t3complete(self):
        return (self.pktfetchtsf \
                and self.txstatustsf \
                and (self.media_ac_delay is not None) \
                and (self.rxdurtsf is not None) \
                and (self.mac_suspend is not None) \
                and (self.txencnt is not None) \
                and (self.datatxscnt is not None) \
                and (self.oactxscnt is not None) \
                and (self.rtstxcnt is not None) \
                and (self.ctsrxcnt is not None))

    @classmethod
    def plot(cls, flowpoints=None, directory='.', type=None, title=None, filename=None, keyposition='left', average=None) :
        if not flowpoints :
            return

        # write out the datafiles for the plotting tool,  e.g. gnuplot
        if filename is None:
            filename = flowpoints[0].name + '_' + str(flowpoints[0].flowid)

        if title is None:
            title=" ".join(filename.split('_'))

        if not os.path.exists(directory):
            logging.debug('Making results directory {}'.format(directory))
            os.makedirs(directory)
        logging.info('Writing {} results to directory {}'.format(directory, filename))

        basefilename = os.path.join(directory, filename)
        datafilename = os.path.join(basefilename + '.data')
        with open(datafilename, 'w') as fid :
            if average :
                fid.write('{} {} {} {} {} {} {} {}\n'.format\
                          ('flowid', 'avg', average['host_total'], average['txdhdfw1'], average['txfw_total'],\
                           average['tx_air'], average['rxfw_total'], average['rxfwdhd']))
            for flowpoint, _, bin in flowpoints :
                fid.write('{} {} {} {} {} {} {} {}\n'.format\
                  (flowpoint.flowid, flowpoint.seqno, flowpoint.host_total, flowpoint.txdhdfw1, flowpoint.txfw_total,\
                   flowpoint.tx_air, flowpoint.rxfw_total, flowpoint.rxfwdhd))

        # write gpc file
        gpcfilename = basefilename + '.gpc'
        #write out the gnuplot control file
        with open(gpcfilename, 'w') as fid :
            fid.write('set output \"{}.{}\"\n'.format(basefilename, 'png'))
            fid.write('set terminal png size 1920,1080\n')
            fid.write('set key {}\n'.format(keyposition))
            fid.write('set title \"{}\" noenhanced\n'.format(title))
            fid.write('set grid x\n')
            fid.write('set style data histograms\n')
            fid.write('set style histogram rowstacked\n')
            fid.write('set boxwidth 0.4\n')
            fid.write('set style fill solid\n')
            fid.write('set xtics rotate\n')
            fid.write('set yrange [0:]\n')
            fid.write('plot \"{0}\" using 4:xtic(2) title \"DHDFW1\", "" using 5 title \"TXFW\", "" using 6 title \"Mac2Mac\", "" using 7 title \"RXFW\", "" using 8 title \"RXDHD"\n'.format(datafilename))
        try:
            gnuplotcmd = ['/usr/bin/gnuplot', gpcfilename]
            logging.info('Gnuplot {}'.format(gnuplotcmd))
            subprocess.run(gnuplotcmd)
        except:
            pass

    def __str__(self) :
        if self.complete and self.t3complete :
            return('FLOWID={} SEQNO={} DHDT0={} DHDT5={} DHDR3={} FWT1={}/0x{:08x} FWT2={}/0x{:08x} FWT3={}/0x{:08x} FWT4={}/0x{:08x} FWR1={}/0x{:08x} FWR2={}/0x{:08x} TXT0={}/0x{:08x} RXT0={}/0x{:08x} TXdrift={} RXdrift={} pktfetch={}'\
                   .format(self.flowid, self.seqno, self.dhdt0gps, self.dhdt5gps, self.dhdr3gps, self.fwt1gps, self.fwt1tsf, \
                           self.fwt2gps, self.fwt2tsf, self.fwt3gps, self.fwt3tsf, self.fwt4gps, self.fwt4tsf, self.fwr1gps, self.fwr1tsf, \
                           self.fwr2gps, self.fwr2tsf, self.tsfgps_txt0, self.tsf_txt0, self.tsfgps_rxt0, self.tsf_rxt0, self.tsf_txdrift, \
                           self.tsf_rxdrift, self.pktfetchgps))

        elif self.complete :
            return('FLOWID={} SEQNO={} DHDT0={} DHDT5={} DHDR3={} FWT1={}/0x{:08x} FWT2={}/0x{:08x} FWT3={}/0x{:08x} FWT4={}/0x{:08x} FWR1={}/0x{:08x} FWR2={}/0x{:08x} TXT0={}/0x{:08x} RXT0={}/0x{:08x} TXdrift={} RXdrift={}'\
                   .format(self.flowid, self.seqno, self.dhdt0gps, self.dhdt5gps, self.dhdr3gps, self.fwt1gps, self.fwt1tsf, \
                           self.fwt2gps, self.fwt2tsf, self.fwt3gps, self.fwt3tsf, self.fwt4gps, self.fwt4tsf, self.fwr1gps, self.fwr1tsf, \
                           self.fwr2gps, self.fwr2tsf, self.tsfgps_txt0, self.tsf_txt0, self.tsfgps_rxt0, self.tsf_rxt0, self.tsf_txdrift, self.tsf_rxdrift))
        elif self.txcomplete :
            return('FLOWID={} SEQNO={} DHDT0={} DHDT5={} FWT1={}/0x{:08x} FWT2={}/0x{:08x} FWT3={}/0x{:08x} FWT4={}/0x{:08x} TXT0={}/0x{:08x} TXdrift={}'\
                   .format(self.flowid, self.seqno, self.dhdt0gps, self.dhdt5gps, self.fwt1gps, self.fwt1tsf, \
                           self.fwt2gps, self.fwt2tsf, self.fwt3gps, self.fwt3tsf, self.fwt4gps, self.fwt4tsf, \
                           self.tsf_txt0, self.tsf_txdrift))

        else:
            return ('Not complete')

    def log_basics(self, bin=None) :
        if self.complete :
            logging.info('{}'.format(self))
            logging.info('FLOWID={0} SEQNO={1} HostTot={2:.3f}: FWT2-DHDT0={3:.3f} FWT2-FWT1={4:.3f}/{5} FWT3-FWT2={6:.3f}/{7} FWR2-FWR1={8:.3f}/{9} bin={10}'.format\
                        (self.flowid, self.seqno, self.host_total, self.tx_total, self.txfw_total, self.txfw_total_tsf, \
                         self.tx_airwithtxstatus, self.tx_airwithtxstatus_tsf, self.rxfw_total, self.rxfw_total_tsf, bin))
        else :
            logging.info('FLOWID={} SEQNO={} Not complete'.format(self.flowid, self.seqno))

    #
    # For your stacked plot, the whole tx timeline could be represented by these timestamps in order:
    #
    # T8Tx
    # DHDT0
    # FWT1
    # FWT2 (FWT3-FWT2 has interupt latency vs pure AIR)
    # ------------- (Air)
    # FWR1
    # FWR2
    # DHDR3
    # T8Rx
    #
    # Note, FWT3 and FWT4 do not figure into the Tx timeline. They are on the status reporting feedback of the tx path, and could overlap with Rx.
    #
    def packet_timeline(self) :
        if self.complete :
            logging.info('FLOWID={0} SEQNO={1} HostTot={2:.3f}: DHD2FWT1={3:.3f} FWTX={4:.3f}/{5} AIR={6:.3f} FWRX={7:.3f}/{8} FWR2DHD={9:.3f}'.format\
                  (self.flowid, self.seqno, self.host_total, self.txdhdfw1, self.txfw_total, self.txfw_total_tsf,\
                   self.tx_air, self.rxfw_total, self.rxfw_total_tsf, self.rxfwdhd))
            if self.host_total < 0 :
                logging.error('Err'.format(self))
        else :
            logging.info('FLOWID={} SEQNO={} Not complete'.format(self.flowid, self.seqno))


class FlowPointHistogram(object):
    instances = weakref.WeakSet()
    def __init__(self, name=None, flowid=None, flowname = None, binwidth=1e-5, title=None) :
        FlowPointHistogram.instances.add(self)
        self.name = name
        self.flowname = flowname
        self.flowid = flowid
        self.binwidth = binwidth
        self.bins = defaultdict(list)
        self.population = 0
        self.population_min = None
        self.sum = 0.0
        self.ks_index = None
        self.createtime = datetime.now(timezone.utc).astimezone()
        self.title=title
        self.basefilename = None
        self.starttime = None
        self.endtime = None
        self.ci3std_low = None
        self.ci3std_high = None
        self.ci_highest = None
        self.offeredload = None
        self.flowname = None
        self.mytitle = None
        self.testtitle = None
        self.run_number = None
        self.units = None

    def __str__(self) :
        return('{} {}'.format(self.population, self.sum))

    @property
    def average(self):
        return (self.sum / self.population)

    # units for insert are milliseconds, bins are 10 us wide, insert is the actual flowpoint object
    def insert(self, flowpoint=None, value=None, name=None, flowname = None, statname = None, units = 'ms', population_min = 0):
        assert flowpoint is not None, 'insert of None for flowpoint not allowed'
        assert value is not None, 'insert of None for flowpoint value not allowed'
        assert statname is not None, 'insert of None for flowpoint stat not allowed'
        self.statname = statname
        if self.flowid is None :
            self.flowid = flowpoint.flowid
        if self.name is None :
            self.name = name
        if self.population_min is None :
            self.population_min = population_min
        elif name and (self.name != name) :
            logging.error('Invalid insert per name mismatch {} and {}'.format(self.name, name))
        if self.flowname is None :
            self.flowname = flowname
        elif flowname and (self.flowname != flowname) :
            logging.error('Invalid insert per flowname mismatch {} and {}'.format(self.flowname, flowname))
        if self.units is None :
            self.units = units
        elif self.units != units :
            logging.error('Invalid insert per unit mismatch {} and {}'.format(self.units, units))
        if self.flowid != flowpoint.flowid :
            logging.error('Invalid insert per flowid mismatch {} and {}'.format(self.flowid, flowpoint.flowid))
        else :
            self.sum += value
            self.population += 1
            if self.units == 'ms' :
                bin_no = floor(value * 1e2)
            else :
                bin_no = floor(value / 10)
            self.bins[bin_no].append((flowpoint,value, bin_no))

    def ci2bin(self, ci=0.997, log=False):
        assert (self.population > 0), "ci2bin histogram {}({}) plot called but no samples}".format(self.name, self.flowid)
        assert (self.population >= self.population_min), "ci2bkn histogram {}({}) plot called with too few samples{}/{}".format(self.name, self.flowid, self.population_min, self.population)
        runningsum = 0;
        result = 0
        for binkey in sorted(self.bins.keys()) :
            flowpoints=self.bins[binkey]
            runningsum += len(flowpoints)
            if (runningsum / float(self.population)) < ci :
                result = binkey
                pass
            else :
                break

        if result and log :
            logging.info('(***Packets below***) STAT={} CI={}'.format(self.name,ci))
            for flowpoint, _  in self.bins[result] :
                flowpoint.log_basics()
            logging.info('(***Packets done***)')
        return result

    def topn(self, count=50, log=False, which='worst'):
        assert (self.population > 0), "topn histogram {}({}) plot called but no samples}".format(self.name, self.flowid)
        assert (self.population >= self.population_min), "topn histogram {}({}) plot called with too few samples{}/{}".format(self.name, self.flowid, self.population_min, self.population)
        if which == 'worst' :
            reverseflag = True
            txttag = 'WORST'
        else :
            reverseflag = False
            txttag = 'BEST'

        binkeys = sorted(self.bins.keys(), reverse=reverseflag)
        ix = 0
        topnpackets = []
        while (len(topnpackets) < count) :
            try :
                thisbin = sorted(self.bins[binkeys[ix]], key=lambda x : float(x[1]), reverse=reverseflag)
                topnpackets.extend(thisbin)
                ix += 1
            except :
                break
        if len(topnpackets) > count :
            topnpackets = topnpackets[:count]

        if False :
            logging.info('(*** STAT={} -- {}({}) -- Packets below ***)'.format(self.name, txttag, count))
            for flowpoint, _, bin in topnpackets :
                flowpoint.log_basics(bin=bin)

        return topnpackets

    def plot(self, directory='.', filename=None, title=None) :
        assert (self.population > 0), "Histogram {}({}) plot called but no samples}".format(self.name, self.flowid)
        assert (self.population >= self.population_min), "Histogram {}({}) plot called with too few samples{}/{}".format(self.name, self.flowid, self.population_min, self.population)
        # write out the datafiles for the plotting tool,  e.g. gnuplot
        if filename is None:
            filename = self.name + '_' + str(self.flowid)

        if not os.path.exists(directory):
            logging.debug('Making results directory {}'.format(directory))
            os.makedirs(directory)
        logging.info('Writing {} results to directory {}'.format(directory, filename))

        basefilename = os.path.join(directory, filename)
        datafilename = os.path.join(basefilename + '.data')
        xmax = 0
        with open(datafilename, 'w') as fid :
            runningsum = 0;
            result = 0
            for bin in sorted(self.bins.keys()) :
                flowpoints=self.bins[bin]
                runningsum += len(flowpoints)
                perc = (runningsum / float(self.population))
                #logging.debug('bin={} x={} y={}'.format(bin, len(flowpoints), perc))
                if self.units == 'ms' :
                    value = (bin*10)/1000.0
                else :
                    value = (bin*10)
                fid.write('{} {} {}\n'.format(value, len(flowpoints), perc))
                if bin > xmax :
                    xmax = bin

        runtime = round((self.endtime - self.starttime).total_seconds())

        if title is None :
            mytitle = '{} {}\\n({} samples)(3std, 99.99={}/{}, {} us) runtime={}s'.format(self.name, self.testtitle, self.population, self.ci3std_low, self.ci3std_high, self.ci_highest, runtime)
            self.mytitle = mytitle
        else :
            self.mytitle = title

        print('mytitle={} run={} dir={}'.format(mytitle, self.run_number, directory))
        # write gpc file
        gpcfilename = basefilename + '.gpc'
        #write out the gnuplot control file
        with open(gpcfilename, 'w') as fid :
            fid.write('set output \"{}.{}\"\n'.format(basefilename, 'png'))
            fid.write('set terminal png size 1024,768\n')
            fid.write('set key bottom\n')
            fid.write('set title \"{}\" noenhanced\n'.format(mytitle))
            if self.units == 'ms' :
                if xmax < 50 :
                    fid.write('set format x \"%.2f"\n')
                else :
                    fid.write('set format x \"%.1f"\n')
            else :
                fid.write('set format x \"%.0f"\n')
            fid.write('set format y \"%.1f"\n')
            fid.write('set xrange [0:*]\n')
            fid.write('set yrange [0:1.01]\n')
            fid.write('set y2range [0:*]\n')
            fid.write('set ytics add 0.1\n')
            fid.write('set y2tics nomirror\n')
            fid.write('set grid\n')
            fid.write('set xlabel \"time ({})\\n{} - {}\"\n'.format(self.units, self.starttime, self.endtime))
            fid.write('plot \"{0}\" using 1:2 index 0 axes x1y2 with impulses linetype 3 notitle, \"{0}\" using 1:3 index 0 axes x1y1 with lines linetype 1 linewidth 2 notitle\n'.format(datafilename))
        try:
            gnuplotcmd = ['/usr/bin/gnuplot', gpcfilename]
            logging.info('Gnuplot {}'.format(gnuplotcmd))
            subprocess.run(gnuplotcmd)
        except:
            pass


class netlink_pktts(object):
    instances = weakref.WeakSet()
    flowpoints = defaultdict(lambda: defaultdict(FlowPoint))
    flowpoint_histograms = defaultdict(lambda: defaultdict(FlowPointHistogram))
    flowpoint_filter = set()

    @classmethod
    def get_instances(cls):
        return list(netlink_pktts.instances)

    #    dhd -i eth1 pktts_flow --help
    #    required args: <srcip> <destip> <sport> <dport> <proto> <ip_prec> <pkt_offset>
    #
    #    pktts_flow
    #        set/get pktts flow configuration
    #
    #    Examples:
    #
    #  $ dhd -i eth1 pktts_flow 192.168.1.1 192.168.1.5 1 5 17 3 10
    #
    #  $ dhd -i eth1 pktts_flow
    #  [0]. ip:101a8c0:501a8c0, port:01:05, proto:11, prec:3, offset:000a, chksum:5000513
    @classmethod
    def flowpoint_filter_add(cls, flowhash):
        netlink_pktts.flowpoint_filter.add(flowhash)
        print('Add flowhash {}'.format(flowhash))
        # logging.info('Flowhash {} added to netlink filter as permit'.format(flowhash))

    @classmethod
    def flowpoint_filter_del(cls, flowhash):
        netlink_pktts.flowpoint_filter.discard(flowhash)

    @classmethod
    def commence(cls, time=None, netlinks='all', start_dhd_pipe=True, dhd_pktts_enab=True, start_servo=True) :
        loop = asyncio.get_running_loop()
        if netlinks == 'all' :
            mynetlinks = netlink_pktts.get_instances()
        if not netlinks:
            logging.warn('netlink_pktts commence method called with none instantiated')
            return
        logging.info('netlink commence invoked on {} devices'.format(len(mynetlinks)))
        for netlink in mynetlinks :
            netlink.mynode.rexec(cmd='pkill dhd')
            netlink.mynode.rexec(cmd='pkill tsfservo')
            netlink.mynode.wl(cmd='mpc 0')
            netlink.mynode.rexec(cmd='cat /proc/net/netlink')
        ssh_node.run_all_commands()
        if dhd_pktts_enab :
            for netlink in mynetlinks :
                netlink.mynode.dhd(cmd='pktts_enab 1')
        ssh_node.run_all_commands()

        if start_servo :
            tasks = [asyncio.ensure_future(netlink.servo_start(), loop=loop) for netlink in mynetlinks]
            if tasks :
                try :
                    logging.info('netlink servo starting')
                    loop.run_until_complete(asyncio.wait(tasks, timeout=10))
                except :
                    for task in tasks :
                        if task.exception() :
                            logging.error('netlink servo start exception')
                    raise

        if start_dhd_pipe :
            tasks = [asyncio.ensure_future(netlink.start(), loop=loop) for netlink in mynetlinks]
            if tasks :
                try :
                    logging.info('netlink dhd servo starting')
                    loop.run_until_complete(asyncio.wait(tasks, timeout=10))
                except :
                    for task in tasks :
                        if task.exception() :
                            logging.error('netlink dhd servo start timeout')
                            task.cancel()
                    raise

            for netlink in mynetlinks :
                netlink.mynode.rexec(cmd='cat /proc/net/netlink')
            ssh_node.run_all_commands()

    @classmethod
    def cease(cls, time=None, netlinks='all') :
        loop = asyncio.get_running_loop()
        if netlinks == 'all' :
            mynetlinks = netlink_pktts.get_instances()
        if not netlinks:
            logging.warn('netlink_pktts stop method called with none instantiated')
            return

        logging.info('netlink stop invoked')
        for netlink in mynetlinks :
            netlink.mynode.rexec(cmd='pkill tsfservo')
            netlink.mynode.rexec(cmd='pkill dhd')
            netlink.mynode.wl(cmd='mpc 0')
        ssh_node.run_all_commands()
        loop.run_until_complete(asyncio.sleep(1))

    @classmethod
    def disable(cls, netlinks='all') :
        if netlinks == 'all' :
            mynetlinks = netlink_pktts.get_instances()
        else :
            mynetlinks = netlinks

        for netlink in mynetlinks :
            netlink.sampling.clear()

    @classmethod
    def enable(cls, netlinks='all') :
        if netlinks == 'all' :
            mynetlinks = netlink_pktts.get_instances()
        else :
            mynetlinks = netlinks

        for netlink in mynetlinks :
            netlink.sampling.set()

    @classmethod
    async def zipfile(cls, filename=None, zipcmd='gzip', loop=None) :
        if filename and loop:
            logging.info('compress file {} using {}'.format(filename, zipcmd))
            childprocess = await asyncio.create_subprocess_exec(zipcmd, filename, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=loop)
            stdout, stderr = await childprocess.communicate()
            if stderr:
                logging.error('zip failed {}'.format(stderr))

    @classmethod
    def DumpFlowpoints(cls, directory='./', flowtable=None):

        logging.info('Netlink dump flowpoints: flow count={} with flow IDs={}'.format(len(netlink_pktts.flowpoints), netlink_pktts.flowpoints.keys()))
        if not os.path.exists(directory):
            logging.debug('Making results directory {}'.format(directory))
            os.makedirs(directory)
        print('Writing {} candidate flowpoints to {}'.format(len(netlink_pktts.flowpoints), directory))
        ix = 0;
        for flowid, seqnos in list(netlink_pktts.flowpoints.items()) :
            if flowtable :
                try :
                    flowname = flowtable[flowid]
                except KeyError :
                    flowname = 'unk_{}'.format(flowid)
            else :
                flowname = 'flowid_{}'.format(flowid)

            failed_writes = []
            csvfilename = os.path.join(directory, '{}.csv'.format(flowname))
            completecnt = 0;
            t3completecnt = 0;
            with open(csvfilename, 'w', newline='') as f:
                logging.info('Dumping flowpoints to file {} using csv'.format(csvfilename))
                writer = csv.writer(f)
                writer.writerow(['seq_no', 'flowhash', 'host_total', 'tx_total', 'txfw_total', 'rxfw_total', 'tx_airwithtxstatus', 'tx_air', 'txdhdfw1', 'rxfwdhd', 'dhdt0gps', 'dhdt5gps', 'dhdr3gps', 'fwt1gps', 'fwt2gps', 'fwt3gps', 'fwt4gps', 'fwr1gps', 'fwr2gps', 'fwt1tsf', 'fwt2tsf', 'fwt3tsf', 'fwt4tsf', 'fwr1tsf', 'fwr2tsf', 'tsf_txdrift', 'tsf_rxdrift', 'tsfgps_txt0', 'tsf_txt0', 'tsfgps_rxt0', 'tsf_rxt0', 'pktfetchtsf', 'pktfetchgps', 'media_ac_delay', 'rxdurtsf', 'mac_suspend', 'txstatustsf', 'txstatusgps', 'txencnt', 'datatxscnt', 'oactxscnt', 'rtstxcnt', 'ctsrxcnt', 'BoffTime','ucdmadly','ucpacketdly', 'drvstsdly', 'drvdma2txsdly'])
                pkts = seqnos.items()
                for seqno, flowpoint in pkts :
                    if flowpoint.complete :
                        completecnt += 1
                        if flowpoint.t3complete :
                            t3completecnt += 1
                    try :
                        writer.writerow([flowpoint.seqno, flowid, flowpoint.host_total, flowpoint.tx_total, flowpoint.txfw_total, flowpoint.rxfw_total, flowpoint.tx_airwithtxstatus, flowpoint.tx_air, flowpoint.txdhdfw1, flowpoint.rxfwdhd, flowpoint.dhdt0gps, flowpoint.dhdt5gps, flowpoint.dhdr3gps, flowpoint.fwt1gps, flowpoint.fwt2gps, flowpoint.fwt3gps, flowpoint.fwt4gps, flowpoint.fwr1gps, flowpoint.fwr2gps, flowpoint.fwt1tsf, flowpoint.fwt2tsf, flowpoint.fwt3tsf, flowpoint.fwt4tsf, flowpoint.fwr1tsf, flowpoint.fwr2tsf, flowpoint.tsf_txdrift, flowpoint.tsf_rxdrift, flowpoint.tsfgps_txt0, flowpoint.tsf_txt0, flowpoint.tsfgps_rxt0, flowpoint.tsf_rxt0, flowpoint.pktfetchtsf, flowpoint.pktfetchgps, flowpoint.media_ac_delay, flowpoint.rxdurtsf, flowpoint.mac_suspend, flowpoint.txstatustsf, flowpoint.txstatusgps, flowpoint.txencnt, flowpoint.datatxscnt, flowpoint.oactxscnt, flowpoint.rtstxcnt, flowpoint.ctsrxcnt, flowpoint.BoffTime,flowpoint.ucdmadly,flowpoint.ucpacketdly, flowpoint.drvstsdly, flowpoint.drvdma2txsdly])
                    except :
                        failed_writes.append(flowpoint.seqno)
                if failed_writes :
                    logging.warn('Write row failed for flow/flowpoints={}/{}'.format(flowid,failed_writes))
                logging.info('Flowpoints: Total={} Completed={} T3completed={}'.format(len(pkts), completecnt, t3completecnt))

            if os.path.isfile(csvfilename) :
                loop = asyncio.get_running_loop()
                tasks = [asyncio.ensure_future(netlink_pktts.zipfile(filename=csvfilename, loop=loop))]
                try :
                    loop.run_until_complete(asyncio.wait(tasks, timeout=60))
                except asyncio.TimeoutError:
                    logging.error('compress timeout')
                    raise
            print('Wrote {}/{} completed flowpoints to {}'.format(completecnt, t3completecnt,csvfilename))

    @classmethod
    def CreateHistograms(cls, directory='./', run_number=None, starttime=None, endtime=None, testtitle=None, flowtable=None, population_min=0):
        for flowid, seqnos in list(netlink_pktts.flowpoints.items()) :
            if flowtable :
                try :
                    flowname = flowtable[flowid]
                except KeyError :
                    flowname = 'unk_{}'.format(flowid)
            else :
                flowname = 'flowid_{}'.format(flowid)
            pkts = seqnos.items()
            pktscnt = len(pkts)
            logging.info('Netlink CreateHistogram for flowname/flowid={}/{} samples={} min={}'.format(flowname, flowid, len(pkts), population_min))
            #Make sure there are enough overall samples
            if pktscnt > population_min :
                complete_count = 0
                t3_complete_count = 0
                txcomplete_count = 0
                for seqno, flowpoint in pkts :
#                    logging.info('DHDT0={} DHDT5={} FWT1={} FWT2={} FWT3={} FWT4={} GPS={}'.flowpoint.dhdt0gps, flowpoint.dhdt5gps, flowpoint.fwt1tsf, flowpoint.fwt2tsf, flowpoint.fwt3tsf, flowpoint.fwt4tsf, flowpoint.tsfgps_txt0)
                    if flowpoint.txcomplete :
                        txcomplete_count += 1
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['tx_host_total_wair']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.txdhdfw3, name='(DHDT0-FWT3)_' + flowname, statname='DHDT0-FWT3', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['tx_host_total_wdoorbell']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.txdhdfw4, name='(DHDT0-FWT4)_' + flowname, statname='DHDT0-FWT4', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['tx_doorbell']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.fw4fw3, name='(FWT4-FWT3)_' + flowname, statname='FWT4-FWT3', population_min = population_min)

                    if flowpoint.complete :
                        complete_count += 1
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['host_total']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.host_total, name='host_total_(DHDR3-DHDT0)_' + flowname, statname='DHDR3-DHDT0', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['tx_total']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.tx_total, name='tx_total_(FWT2-DHDT0)_' + flowname, statname='FWT2-DHDT0', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['txfw_total']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.txfw_total, name='txfw_total_(FWT2-FWT1)_' + flowname, statname='FWT2-FWT1', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['rxfw_total']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.rxfw_total, name='rxfw_total_(FWR2-FWR1)_'+ flowname, statname='FWR2-FWR1', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['tx_airwithtxstatus']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.tx_airwithtxstatus, name='tx_airwithtxstatus_(FWT3-FWT2)_' + flowname, statname='FWT3-FWT2', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['tx_air']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.tx_air, name='tx_air_(FWT2-FWR1)_' + flowname, statname='FWT2-FWR1', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['txdhdfw1']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.txdhdfw1, name='txdhdfw1_(FWT1-DHDT0)_' + flowname, statname='FWT1-DHDT0', population_min = population_min)
                        fphisto=netlink_pktts.flowpoint_histograms[flowid]['rxfwdhd']
                        fphisto.insert(flowpoint=flowpoint, value=flowpoint.rxfwdhd, name='rxfwdhd_(DHDR3-FWR2)_' + flowname, statname='DHDR3-FWR2', population_min = population_min)
                        if flowpoint.t3complete :
                            t3_complete_count += 1
                            fphisto=netlink_pktts.flowpoint_histograms[flowid]['BoffTime']
                            fphisto.insert(flowpoint=flowpoint, value=flowpoint.BoffTime, name='BoffTime_' + flowname, statname='BoffTime', units='us', population_min = population_min)
                            fphisto=netlink_pktts.flowpoint_histograms[flowid]['ucdmadly']
                            fphisto.insert(flowpoint=flowpoint, value=flowpoint.ucdmadly, name='ucdmadly_' + flowname, statname='ucdmadly', units='us', population_min = population_min)
                            fphisto=netlink_pktts.flowpoint_histograms[flowid]['ucpacketdly']
                            fphisto.insert(flowpoint=flowpoint, value=flowpoint.ucpacketdly, name='ucpacketdly_' + flowname, statname='ucpacketdly', units='us', population_min = population_min)
                            fphisto=netlink_pktts.flowpoint_histograms[flowid]['drvstsdly']
                            fphisto.insert(flowpoint=flowpoint, value=flowpoint.drvstsdly, name='drvstsdly_' + flowname, statname='drvstsdly', units='us', population_min = population_min)
                            fphisto=netlink_pktts.flowpoint_histograms[flowid]['drvdma2txsdly']
                            fphisto.insert(flowpoint=flowpoint, value=flowpoint.drvdma2txsdly, name='drvdma2txsdly_' + flowname, statname='drvdma2txsdly', units='us', population_min = population_min)
            logging.info('Netlink histograms done for flowname/flowid={}/{} complete/t3complete/txcomplete/samples={}/{}/{}/{}'.format(flowname, flowid, complete_count, t3_complete_count, txcomplete_count, pktscnt))

        # filter out uninteresting stats, i.e. stats without sufficient samples
        for flowid, stats in list(netlink_pktts.flowpoint_histograms.items()) :
            for stat, fp_histo in list(stats.items()):
                if netlink_pktts.flowpoint_histograms[flowid][stat].population < population_min :
                    print('Filtered flowid {} stat of {}  population min/actual={}/{}'.format(flowid, stat, population_min, netlink_pktts.flowpoint_histograms[flowid][stat].population))
                    logging.info('Filtered flowid {} stat of {} population min/actual={}/{}'.format(flowid, stat, population_min, netlink_pktts.flowpoint_histograms[flowid][stat].population))
                    del netlink_pktts.flowpoint_histograms[flowid][stat]

        logging.info("Plot of {} flows".format(len(netlink_pktts.flowpoint_histograms.items())))
        for flowid, stats in list(netlink_pktts.flowpoint_histograms.items()) :
            if flowtable :
                try :
                    flowname = flowtable[flowid]
                except KeyError :
                    flowname = 'unk_{}'.format(flowid)
            else :
                flowname = 'flowid_{}'.format(flowid)

            # Produce the plots
            avg = {}
            for stat, fp_histo in list(stats.items()):
                avg[stat] = netlink_pktts.flowpoint_histograms[flowid][stat].average
                logging.info('Flowid {} histo avg for stat {} = {}'.format(flowid, stat, avg[stat]))
                statdirectory = os.path.join(directory, stat)
                histo = netlink_pktts.flowpoint_histograms[flowid][stat]
                histo.starttime = starttime
                histo.endtime = endtime
                logging.info('Flowid plot stat {} population={} bins={}'.format(flowid, stat, histo.population, len(histo.bins.keys())))
                # RJM, fix below - don't use constant of 10
                histo.ci3std_low=histo.ci2bin(ci=0.003, log=False) * 10
                histo.ci3std_high=histo.ci2bin(ci=0.997, log=False) * 10
                histo.ci_highest=histo.ci2bin(ci=0.9999, log=False) * 10
                histo.title = stat
                histo.testtitle = testtitle
                histo.run_number = run_number
                worst=histo.topn(which='worst')
                best=histo.topn(which='best')
                # FIX ME: Use an object, out to json
                print('Pyflows test results: flowid={} flowname={} statname={} value={} run={} files={}'.format(flowid, flowname, histo.statname, histo.ci3std_high, histo.run_number, directory))
                # FIX ME: have an option not to plot
                try :
                    histo.plot(directory=statdirectory)
                except :
                    pass
                try :
                    topdirectory = os.path.join(statdirectory, 'bottom50')
                    FlowPoint.plot(flowpoints=worst, type='timeline', title='{} Bottom50'.format(histo.mytitle), directory=topdirectory, filename='{}_{}_{}_bottom'.format(stat, flowname, flowid), keyposition='right', average=avg)
                except :
                    pass
                try :
                    topdirectory = os.path.join(statdirectory, 'top50')
                    FlowPoint.plot(flowpoints=best, type='timeline', title='{} Top50'.format(histo.mytitle), directory=topdirectory, filename='{}_{}_{}_top'.format(stat, flowname, flowid), keyposition='right', average=avg)
                except :
                    pass


    @classmethod
    def CreateHistogramsThreaded(cls, directory='./', starttime=None, endtime=None, testtitle=None, flowtable=None) :
        # don't allow inserts while the thread is running
        netlink_pktts.disable()
        try :
            event_loop = asyncio.get_running_loop()
            logging.debug('CreateHistogramsThreaded start')
            keywordfunc = functools.partial(netlink_pktts.CreateHistograms, directory=directory, starttime=starttime, endtime=endtime, testtitle=testtitle, flowtable=flowtable)
            coro = event_loop.run_in_executor(None, keywordfunc)
            event_loop.run_until_complete(coro)
        finally :
            netlink_pktts.enable()
            logging.debug('CreateHistogramsThreaded done')

    @classmethod
    def LogAllFlowpoints(cls):
        for flowid, seqnos in list(netlink_pktts.flowpoints.items()) :
            for seqno, flowpoint in list(seqnos.items()):
                flowpoint.log_basics()
                flowpoint.packet_timeline()


    @classmethod
    def ResetStats(cls):
        logging.info('netlink_pktts reset stats')
        netlink_pktts.flowpoints.clear()
        netlink_pktts.flowpoint_histograms.clear()



    # Asycnio protocol for subprocess transport to decode netlink pktts messages
    #
    # See pages 10 and 13 of https://docs.google.com/document/d/1a2Vo0AUBMo1utUWYLkErSSqQM9sMf0D9FPfyni_e4Qk/edit#heading=h.9lme8ct208v3
    #
    # Probe points: Tx Path*
    #   T8Tx - Frame/Pkt generated timestamp at Application
    #   DhdT0 - DHD Driver xmits pkt to dongle
    #   DhdT5 - DHD receives Tx-Completion from FW
    #   FWT1  - Firmware sees the TxPost work item from host
    #   FWT2  - Firmware submits the TxPost work item to Mac tx DMA (after header conversion)
    #   FWT3  - Firmware processes TxStatus from uCode
    #   FWT4  - Firmware posts Tx-Completion message to host
    # Rx Path*
    #   FWR1 - RxStatus TSF as reported by Ucode (time at which pkt was rxed by the MAC)
    #   FWR2 - Time at which Rx-Completion is posted to host.
    #   DRRx - DHD Driver process Rx-Completion and forwards Rxed pkt to Network Stack
    #   T8Rx - Frame/Packet Rxed by Application

    class NetlinkPkttsProtocol(asyncio.SubprocessProtocol):
        def __init__(self, session):
            self.loop = asyncio.get_running_loop()
            self.exited = False
            self.closed_stdout = False
            self.closed_stderr = False
            self.stdoutbuffer = ""
            self.stderrbuffer = ""
            self._session = session
            self.debug = session.debug
            self.silent = session.silent
            self.txmatch_output = True
            self.io_watch_timer = None
            self.io_watch_default = 2 # units seconds

            #type:1, flowid:0x4cee02be, prec:0, xbytes:0x000000000008d56b5b999b8000000000, :::1536793472578955:1536793472579721:::0x000ae906:0x000ae93b:0x000ae965:0x000ae98c
            #type:2, flowid:0x4cee02be, prec:0, xbytes:0x000000000008d56b5b999b8000000000, :::1536793472579709:::0x00a0a8a4:0x00a0a8d5
            self.netlink_type_flowid_parse = re.compile(r'type:(?P<type>[\d]),\s+flowid:(?P<flowid>0x[0-9a-f]{8}),\sprec');
            self.netlink_type1_parse = re.compile(r'type:1,\s+flowid:(?P<flowid>0x[0-9a-f]{8}),\sprec:[0-7],\sxbytes:(?P<payload>0x[0-9a-f]{32}),\s:::(?P<dhdt0>[0-9]{16}):(?P<dhdt5>[0-9]{16}):::(?P<fwt1>0x[0-9a-f]{8}):(?P<fwt2>0x[0-9a-f]{8}):(?P<fwt3>0x[0-9a-f]{8}):(?P<fwt4>0x[0-9a-f]{8})');
            self.netlink_type2_parse = re.compile(r'type:2,\s+flowid:(?P<flowid>0x[0-9a-f]{8}),\sprec:[0-7],\sxbytes:(?P<payload>0x[0-9a-f]{32}),\s:::(?P<dhdr3>[0-9]{16}):::(?P<fwr1>0x[0-9a-f]{8}):(?P<fwr2>0x[0-9a-f]{8})');
            # type:3, flowid:0x73177206, prec:6, xbytes:0x0000000000022f795eb078120000000f, 0671ba16:00000000:00000000:00000000:0671bab6:::0001:0000:0001:0000:0000:beea:beeb:beec
            self.netlink_type3_parse = re.compile(r'type:3,\s+flowid:(?P<flowid>0x[0-9a-f]{8}),\sprec:[0-7],\sxbytes:(?P<payload>0x[0-9a-f]{32}),\s(?P<pktfetch>[0-9a-f]{8}):(?P<medacdly>[0-9a-f]{8}):(?P<rxdur>[0-9a-f]{8}):(?P<macsusdur>[0-9a-f]{8}):(?P<txstatusts>[0-9a-f]{8}):::(?P<txencnt>[0-9]{4}):(?P<oactxscnt>[0-9]{4}):(?P<datatxscnt>[0-9]{4}):(?P<rtstxcnt>[0-9]{4}):(?P<ctsrxcnt>[0-9]{4})');

        @property
        def finished(self):
            return self.exited and self.closed_stdout and self.closed_stderr

        def signal_exit(self):
            if not self.finished:
                return
            self._session.closed.set()
            self._session.opened.clear()
            logging.info('Netlink proto connection done (session={})'.format(self._session.name))

        def connection_made(self, trans):
            self._session.closed.clear()
            self._session.opened.set()
            self.mypid = trans.get_pid()
            self.io_watch_timer = ssh_node.loop.call_later(30, self.io_watch_event)
            self._session.io_inactive.clear()
            logging.info('Netlink proto connection made (session={})'.format(self._session.name))

        def io_watch_event(self, type=None):
            self._session.io_inactive.set()
            logging.debug('Netlink watch evetnt io_inactive(session={})'.format(self._session.name))

        def pipe_data_received(self, fd, data):
            data = data.decode("utf-8")
            self._session.io_inactive.clear()
            if self.io_watch_timer :
                self.io_watch_timer.cancel()
                logging.debug('cancel io_watch_timer');
            self.io_watch_timer = ssh_node.loop.call_later(self.io_watch_default, self.io_watch_event)
            logging.debug('set io_watch_timer for {} seconds'.format(self.io_watch_default));
            if fd == 1:
                self.stdoutbuffer += data
                if not self._session.sampling.is_set() :
                    return
                while "\n" in self.stdoutbuffer:
                    line, self.stdoutbuffer = self.stdoutbuffer.split("\n", 1)
                    self._session.stdout_linecount += 1
                    if self.debug:
                        logging.debug('{} pktts PREPARSE: {} (stdout)'.format(self._session.name, line))
                    m = self.netlink_type_flowid_parse.match(line)
                    if m :
                        type = m.group('type')
                        flowid = m.group('flowid')
                        if netlink_pktts.flowpoint_filter and (flowid not in netlink_pktts.flowpoint_filter) :
                            if self.debug :
                                logging.debug('MISS pktts flowid={} filter={}'.format(flowid, netlink_pktts.flowpoint_filter))
                            return
                        if type == '1' :
                            m = self.netlink_type1_parse.match(line)
                            if m :
                                if self.debug:
                                    logging.debug('MATCHTx: {} {} {} {} {} {} {} {}'.format(m.group('flowid'),m.group('payload'),m.group('dhdt0'),m.group('dhdt5'),m.group('fwt1'), m.group('fwt2'), m.group('fwt3'), m.group('fwt4')))
                                if self._session.servo_match and (m.group('flowid') != '0x00000000') :
                                    # The Tx should have DHDT0, DHDT5, FWTx0, FWTx1, FWTx2, FWTx3
                                    flowid = m.group('flowid')
                                    if self.txmatch_output :
                                        logging.info('ServoType1MATCH: {} {} {} {} {} {} {} {}'.format(m.group('flowid'),m.group('payload'),m.group('dhdt0'),m.group('dhdt5'),m.group('fwt1'), m.group('fwt2'), m.group('fwt3'), m.group('fwt4')))
                                        self.txmatch_output = False
                                    seqno = int(m.group('payload')[-8:],16)
                                    dhdt0gps = float(m.group('dhdt0')[:-6] + '.' + m.group('dhdt0')[-6:])
                                    dhdt5gps = float(m.group('dhdt5')[:-6] + '.' + m.group('dhdt5')[-6:])
                                    #insert the values into the flowpoints table for after test processing
                                    fp=netlink_pktts.flowpoints[flowid][seqno] # flowid, seqno
                                    fp.seqno = seqno
                                    fp.dhdt0gps = dhdt0gps
                                    fp.dhdt5gps = dhdt5gps
                                    fp.flowid = flowid
                                    if fp.tsf_txt0 is None :
                                        assert self._session.tsf_t0 is not None, "tsfservo on {} not initialized".format(self._session.name)
                                        fp.tsf_txt0 = self._session.tsf_t0
                                        fp.tsf_txdrift = self._session.tsf_drift
                                        fp.tsfgps_txt0 = self._session.tsf_t0gpsadjust

                                    fp.fwt1tsf = int(m.group('fwt1'),16)
                                    fp.fwt2tsf = int(m.group('fwt2'),16)
                                    fp.fwt3tsf = int(m.group('fwt3'),16)
                                    fp.fwt4tsf = int(m.group('fwt4'),16)
                        elif type == '2' :
                            m = self.netlink_type2_parse.match(line)
                            if m :
                                if self.debug:
                                    logging.debug('MATCHRx: {} {} {} {} {}'.format(m.group('flowid'), m.group('payload'), m.group('dhdr3'), m.group('fwr1'), m.group('fwr2')))

                                if self._session.servo_match and (m.group('flowid') != '0x00000000') :
                                    # The Tx should have DHDT0, DHDT5, FWTx0, FWTx1, FWTx2, FWTx3
                                    flowid = m.group('flowid')
                                    seqno = int(m.group('payload')[-8:],16)
                                    seqno = int(m.group('payload')[-8:],16)
                                    dhdr3gps = float(m.group('dhdr3')[:-6] + '.' + m.group('dhdr3')[-6:])
                                    #insert the values into the flowpoints table for after test processing
                                    fp=netlink_pktts.flowpoints[flowid][seqno] # flowid, seqno
                                    fp.dhdr3gps = dhdr3gps
                                    fp.seqno = seqno
                                    fp.flowid = flowid
                                    if fp.tsf_rxt0 is None :
                                        assert self._session.tsf_t0gpsadjust is not None, "tsfservo on {} not initialized".format(self._session.name)
                                        fp.tsf_rxdrift = self._session.tsf_drift
                                        fp.tsfgps_rxt0 = self._session.tsf_t0gpsadjust
                                        fp.tsf_rxt0 = self._session.tsf_t0

                                    fp.fwr1tsf = int(m.group('fwr1'),16)
                                    fp.fwr2tsf = int(m.group('fwr2'),16)

                        elif type == '3' :
                            m = self.netlink_type3_parse.match(line)
                            if m :
                                if self.debug :
                                    logging.debug('MATCHT3: {} {} {} {} {} {} {} {} {} {} {} {}'.format(m.group('flowid'), m.group('payload'), m.group('pktfetch'), m.group('medacdly'), m.group('rxdur'), m.group('macsusdur'), m.group('txstatusts'), m.group('txencnt'), m.group('oactxscnt'), m.group('datatxscnt'), m.group('rtstxcnt'), m.group('ctsrxcnt')));
                                if self._session.servo_match and (m.group('flowid') != '0x00000000') :
                                    flowid = m.group('flowid')
                                    seqno = int(m.group('payload')[-8:],16)
                                    fp=netlink_pktts.flowpoints[flowid][seqno] # flowid, seqno
                                    fp.seqno = seqno
                                    fp.flowid = flowid

                                    fp.media_ac_delay = int(m.group('medacdly'),16)
                                    fp.rxdurtsf = int(m.group('rxdur'),16)
                                    fp.mac_suspend = int(m.group('macsusdur'),16)
                                    fp.txencnt = int(m.group('txencnt'))
                                    fp.oactxscnt = int(m.group('oactxscnt'))
                                    fp.datatxscnt = int(m.group('datatxscnt'))
                                    fp.rtstxcnt = int(m.group('rtstxcnt'))
                                    fp.ctsrxcnt = int(m.group('ctsrxcnt'))

                                    if not fp.tsf_txt0 is None:
                                        assert self._session.tsf_t0 is not None, "tsfservo on {} not initialized".format(self._session.name)
                                        fp.tsf_txt0 = self._session.tsf_t0
                                        fp.tsf_txdrift = self._session.tsf_drift
                                        fp.tsfgps_txt0 = self._session.tsf_t0gpsadjust

                                    fp.pktfetchtsf = int(m.group('pktfetch'),16)
                                    fp.txstatustsf = int(m.group('txstatusts'),16)
                        else :
                            logging.debug('Unknown type pktts {}: {} (stdout)'.format(type, line))
                    else :
                        if self.debug:
                            logging.debug('MISS: pktts {} (stdout)'.format(line))

            elif fd == 2:
                self.stderrbuffer += data
                while "\n" in self.stderrbuffer:
                    line, self.stderrbuffer = self.stderrbuffer.split("\n", 1)
                    self._session.stderr_linecount += 1
                    logging.warning('{} {} (pktts - stderr)'.format(line, self._session.ipaddr))


        def pipe_connection_lost(self, fd, exc):
            if self.io_watch_timer :
                self.io_watch_timer.cancel()
                logging.debug('cancel io_watch_timer lost connection');
            self.signal_exit()

        def process_exited(self,):
            self.signal_exit()

    # Asycnio protocol for subprocess transport to decode netlink pktts messages
    class NetlinkServoProtocol(asyncio.SubprocessProtocol):
        def __init__(self, session):
            self.loop = asyncio.get_running_loop()
            self.exited = False
            self.closed_stdout = False
            self.closed_stderr = False
            self.stdoutbuffer = ""
            self.stderrbuffer = ""
            self._session = session
            self.debug = session.debug
            self.silent = session.silent

            # TSFGPS servo for dev ap0 GPS=1537306425.681630927 TSF=2833.449983 RAW=0xa8e303ff TSFT0=1537303592.231647927 (with delay 1.000000 second(s) pid=16591)
            # TSFGPS servo for dev eth0 GPS=1537146706.237886334 TSF=25.444833 RAW=0x18441e1 drift=-25542 ns rate=-25.538413 usec/sec
            self.tsfservo_init_parse = re.compile(r'TSFGPS servo for dev (?P<device>[a-z0-9\.]+)\sGPS=(?P<gpsts>[0-9\.]+)\s(?P<tsfts>TSF=[0-9\.]+)\sRAW=(?P<tsfraw>0x[0-9a-f]+)\sDHDIoctl=[0-9]+\sTSFT0=(?P<tsft0>[0-9\.]+)')
            self.tsfservo_drift_parse = re.compile(r'TSFGPS servo for dev (?P<device>[a-z0-9\.]+)\sGPS=(?P<gpsts>[0-9\.]+)\s(?P<tsfts>TSF=[0-9\.]+)\sRAW=(?P<tsfraw>0x[0-9a-f]+)\sDHDIoctl=[0-9]+\sdrift=(?P<tsfdrift>[\-0-9]+)\sns')

        @property
        def finished(self):
            return self.exited and self.closed_stdout and self.closed_stderr

        def signal_exit(self):
            if not self.finished:
                return
            self._session.servo_closed.set()
            self._session.servo_opened.clear()

        def connection_made(self, trans):
            self._session.servo_closed.clear()
            self._session.servo_opened.set()
            self.mypid = trans.get_pid()

        def pipe_data_received(self, fd, data):
            data = data.decode("utf-8")
            if fd == 1:
                self.stdoutbuffer += data
                while "\n" in self.stdoutbuffer:
                    line, self.stdoutbuffer = self.stdoutbuffer.split("\n", 1)
                    logging.debug('{} {} (stout)'.format(self._session.name, line))
                    if not self._session.servo_match :
                        m = self.tsfservo_init_parse.match(line)
                        if m :
                            self._session.servo_match = True
                            self._session.tsf_t0gpsadjust = float(m.group('tsft0'))
                            self._session.tsf_t0 = int(m.group('tsfraw'),16)
                            self._session.tsf_drift = 0
                            logging.info('{} TSF init match {} (stout)'.format(self._session.name, line))
                    else :
                        m = self.tsfservo_drift_parse.match(line)
                        if m:
                            # convert from ns to us
                            self._session.tsf_drift = round((int(m.group('tsfdrift')))/1000)
                            if not self._session.servo_ready.is_set() :
                                self._session.servo_ready.set()

            elif fd == 2:
                self.stderrbuffer += data
                while "\n" in self.stderrbuffer:
                    line, self.stderrbuffer = self.stderrbuffer.split("\n", 1)
                    self._session.stderr_linecount += 1
                    logging.warning('{} {} (servo - stderr)'.format(line, self._session.ipaddr))

        def pipe_connection_lost(self, fd, exc):
            self.signal_exit()

        def process_exited(self,):
            self.signal_exit()

    def __init__(self, user='root', name=None, loop=None, sshnode=None, debug=False, output='ssh', silent=True, chip='4387'):
        netlink_pktts.instances.add(self)
        self.loop = asyncio.get_running_loop()
        self.ssh = '/usr/bin/ssh'
        self.user = user
        self.mynode = sshnode
        self.chip = chip
        try:
            self.ipaddr = sshnode.ipaddr
        except AttributeError:
            self.ipaddr = sshnode
        self.device=sshnode.device
        print('Netlink name={} device={} ip={}'.format(sshnode.name, sshnode.device, sshnode.ipaddr))
        self.pktts_cmd = '/usr/bin/dhd --pktts'
        self.servo_cmd = '/usr/local/bin/tsfservo -i {} -f 2 -t 7200 -d {}'.format(self.device, self.chip)
        if output != 'ssh' :
            self.pktts_cmd = '{} > /dev/null 2>&1'.format(self.pktts_cmd)
        self.stdout_linecount = 0
        self.stderr_linecount = 0
        self.opened = asyncio.Event()
        self.closed = asyncio.Event()
        self.sampling = asyncio.Event()
        self.servo_opened = asyncio.Event()
        self.servo_closed = asyncio.Event()
        self.servo_ready = asyncio.Event()
        self.name = sshnode.name
        self.debug  = debug
        self.silent = silent
        self.servo_match = False
        self.io_inactive = asyncio.Event()

    def reset(self) :
        self.stdout_linecount = 0
        self.stderr_linecount = 0

    async def servo_start(self):
        sshcmd=[self.ssh, self.user + '@' + self.ipaddr, self.servo_cmd]
        logging.info('servo_start {}'.format(str(sshcmd)))
        self.servo_closed.set()
        self.servo_opened.clear()
        self.servo_ready.clear()
        try :
            self._transport, self._protocol = await self.loop.subprocess_exec(lambda: self.NetlinkServoProtocol(self), *sshcmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=None)
        except:
            print('netlink dhd pktts start error: {}'.format(str(sshcmd)))
            logging.error('netlink dhd pktts start error: {}'.format(str(sshcmd)))
        else :
            await self.servo_ready.wait()

    async def start(self):
        sshcmd=[self.ssh, self.user + '@' + self.ipaddr, self.pktts_cmd]
        logging.info('{}'.format(str(sshcmd)))
        self.stdout_linecount = 0
        self.stderr_linecount = 0
        self.closed.set()
        self.opened.clear()
        self.sampling.set()
        try :
            self._transport, self._protocol = await self.loop.subprocess_exec(lambda: self.NetlinkPkttsProtocol(self), *sshcmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=None)
            await self.opened.wait()
        except:
            logging.error('netlink dhd pktts start error: {}'.format(str(sshcmd)))
            pass

    async def await_io_finish(self) :
        if not self._session.io_inactive.is_set() :
            await self._session.io_inactive.wait()
