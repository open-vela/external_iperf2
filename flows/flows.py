#!/usr/bin/env python3.5
#
# Author Robert J. McMahon
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
import getpass
import math
import scipy
import scipy.spatial
import numpy as np

from datetime import datetime as datetime, timezone
from scipy import stats
from scipy.cluster.hierarchy import linkage

logger = logging.getLogger(__name__)

class iperf_flow(object):
    port = 61000
    iperf = '/usr/bin/iperf'
    instances = weakref.WeakSet()
    loop = None
    flow_scope = ("flowstats")
    tasks = []

    @classmethod
    def sleep(cls, time=0, text=None, stoptext=None) :
        loop = asyncio.get_event_loop()
        if text :
            logging.info('Sleep {} ({})'.format(time, text))
        loop.run_until_complete(asyncio.sleep(time))
        if stoptext :
            logging.info('Sleep done ({})'.format(stoptext))

    @classmethod
    def get_instances(cls):
        return list(iperf_flow.instances)

    @classmethod
    def set_loop(cls, loop=None):
        if loop :
            iperf_flow.loop = loop
        elif os.name == 'nt':
            # On Windows, the ProactorEventLoop is necessary to listen on pipes
            iperf_flow.loop = asyncio.ProactorEventLoop()
        else:
            loop = asyncio.get_event_loop()
            iperf_flow.loop = asyncio.get_event_loop()

    @classmethod
    def close_loop(cls, loop=None):
        iperf_flow.loop.close()

    @classmethod
    def run(cls, time=None, flows='all', sample_delay=None, io_timer=None, preclean=False) :
        if flows == 'all' :
            flows = iperf_flow.get_instances()
        if not flows:
            logging.warn('flow run method called with no flows instantiated')
            return

        if preclean:
            hosts = [flow.server for flow in flows]
            hosts.extend([flow.client for flow in flows])
            hosts=list(set(hosts))
            tasks = [asyncio.ensure_future(iperf_flow.cleanup(user='root', host=host)) for host in hosts]
            try :
                iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=10, loop=iperf_flow.loop))
            except asyncio.TimeoutError:
                logging.error('preclean timeout')
                raise

        logging.info('flow run invoked')
        tasks = [asyncio.ensure_future(flow.rx.start(time=time), loop=iperf_flow.loop) for flow in flows]
        try :
            iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=10, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('flow server start timeout')
            raise
        tasks = [asyncio.ensure_future(flow.tx.start(time=time), loop=iperf_flow.loop) for flow in flows]
        try :
            iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=10, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('flow client start timeout')
            raise
        if sample_delay :
            iperf_flow.sleep(time=0.3, text="ramp up", stoptext="ramp up done")
        if io_timer :
            tasks = [asyncio.ensure_future(flow.is_traffic(), loop=iperf_flow.loop) for flow in flows]
            try :
                iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=10, loop=iperf_flow.loop))
            except asyncio.TimeoutError:
                logging.error('flow traffic check timeout')
                raise

        iperf_flow.sleep(time=time, text="Running traffic start", stoptext="Stopping flows")

        # Signal the remote iperf client sessions to stop them
        tasks = [asyncio.ensure_future(flow.tx.signal_stop(), loop=iperf_flow.loop) for flow in flows]
        try :
            iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=3, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('flow tx stop timeout')
            raise

        # Now signal the remote iperf server sessions to stop them
        tasks = [asyncio.ensure_future(flow.rx.signal_stop(), loop=iperf_flow.loop) for flow in flows]
        try :
            iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=3, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('flow tx stop timeout')
            raise

        # iperf_flow.loop.close()
        logging.info('flow run finished')

    @classmethod
    def plot(cls, flows='all', title='None', directory='None') :
        if flows == 'all' :
            flows = iperf_flow.get_instances()

        tasks = []
        for flow in flows :
            for this_name in flow.histogram_names :
                i = 0
                # group by name
                histograms = [h for h in flow.histograms if h.name == this_name]
                for histogram in histograms :
                    if histogram.ks_index is not None :
                        histogram.output_dir = directory + '/' + this_name + '_' + str(i)
                    else :
                        histogram.output_dir = directory + '/' + this_name + '_' + str(histogram.ks_index)

                    logging.info('scheduling task {}'.format(histogram.output_dir))
                    tasks.append(asyncio.ensure_future(histogram.async_plot(directory=histogram.output_dir, title=title), loop=iperf_flow.loop))
                    i += 1
        try :
            logging.info('runnings tasks')
            iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=600, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('plot timed out')
            raise


    @classmethod
    def stop(cls, flows='all') :
        loop = asyncio.get_event_loop()
        if flows == 'all' :
            flows = iperf_flow.get_instances()
        iperf_flow.set_loop(loop=loop)
        tasks = [asyncio.ensure_future(flow.tx.stop(), loop=loop) for flow in flows]
        try :
            loop.run_until_complete(asyncio.wait(tasks, timeout=10, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('flow server start timeout')
            raise

        tasks = [asyncio.ensure_future(flow.rx.stop(), loop=loop) for flow in flows]
        try :
            loop.run_until_complete(asyncio.wait(tasks, timeout=10, loop=iperf_flow.loop))
        except asyncio.TimeoutError:
            logging.error('flow server start timeout')
            raise

    @classmethod
    async def cleanup(cls, host=None, sshcmd='/usr/bin/ssh', user='root') :
        if host:
            logging.info('ssh {}@{} pkill iperf'.format(user, host))
            childprocess = await asyncio.create_subprocess_exec(sshcmd, '{}@{}'.format(user, host), 'pkill', 'iperf', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=iperf_flow.loop)
            stdout, _ = await childprocess.communicate()
            if stdout:
                logging.info('cleanup: host({}) stdout={} '.format(host, stdout))

    @classmethod
    def tos_to_txt(cls, tos) :
        switcher = {
            int(0x0)  : "BE",
            int(0x02) : "BK",
            int(0xC0) : "VO",
            int(0x80) : "VI",
        }
        return switcher.get(int(tos), None)

    @classmethod
    def txt_to_tos(cls, txt) :
        switcher = {
            "BE" : "0x0",
            "BESTEFFORT" : "0x0",
            "0x0" : "0x0",
            "BK" : "0x20",
            "BACKGROUND" : "0x20",
            "0x20" : "0x20",
            "VO" : "0xC0",
            "VOICE" : "0xC0",
            "0xC0" : "0xC0",
            "VI" : "0x80",
            "VIDEO" : "0x80",
            "0x80" : "0x80",
        }
        return switcher.get(txt.upper(), None)

    def __init__(self, name='iperf', server='localhost', client = 'localhost', user = None, proto = 'TCP', dst = '127.0.0.1', interval = 0.5, flowtime=10, offered_load = None, tos='BE', window='150K', debug = False):
        iperf_flow.instances.add(self)
        if not iperf_flow.loop :
            iperf_flow.set_loop()
        self.loop = iperf_flow.loop
        self.name = name
        self.flowname = name
        iperf_flow.port += 1
        self.port = iperf_flow.port
        self.server = server
        self.client = client
        if not user :
            self.user = getpass.getuser()
        else :
            self.user = user
        self.proto = proto
        self.dst = dst
        self.tos = tos
        self.interval = round(interval,3)
        self.offered_load = offered_load
        self.debug = debug
        self.TRAFFIC_EVENT_TIMEOUT = round(self.interval * 4, 3)
        self.flowstats = {'current_rxbytes' : None , 'current_txbytes' : None , 'flowrate' : None, 'starttime' : None}
        self.flowtime = flowtime
        # use python composition for the server and client
        # i.e. a flow has a server and a client
        self.rx = iperf_server(name='{}->RX({})'.format(name, str(self.server)), loop=self.loop, host=self.server, flow=self, debug=self.debug)
        self.tx = iperf_client(name='{}->TX({})'.format(name, str(self.client)), loop=self.loop, host=self.client, flow=self, debug=self.debug)
        self.rx.window=window
        self.tx.window=window
        # Initialize the flow stats dictionary
        self.flowstats['txdatetime']=[]
        self.flowstats['txbytes']=[]
        self.flowstats['txthroughput']=[]
        self.flowstats['writes']=[]
        self.flowstats['errwrites']=[]
        self.flowstats['retry']=[]
        self.flowstats['cwnd']=[]
        self.flowstats['rtt']=[]
        self.flowstats['rxdatetime']=[]
        self.flowstats['rxbytes']=[]
        self.flowstats['rxthroughput']=[]
        self.flowstats['reads']=[]
        self.flowstats['histograms']=[]
        self.flowstats['histogram_names'] = set()
        self.ks_critical_p = 0.01

    def destroy(self) :
        iperf_flow.instances.remove(self)

    def __getattr__(self, attr) :
        if attr in self.flowstats :
            return self.flowstats[attr]

    async def start(self):
        self.flowstats = {'current_rxbytes' : None , 'current_txbytes' : None , 'flowrate' : None}
        await self.rx.start()
        await self.tx.start()

    async def is_traffic(self) :
        if self.interval < 0.005 :
            logging.warn('{} {}'.format(self.name, 'traffic check invoked without interval sampling'))
        else :
            self.rx.traffic_event.clear()
            self.tx.traffic_event.clear()
            logging.info('{} {}'.format(self.name, 'traffic check invoked'))
            await self.rx.traffic_event.wait()
            await self.tx.traffic_event.wait()

    async def stop(self):
        self.tx.stop()
        self.rx.stop()

    def stats(self):
        logging.info('stats')

    def compute_ks_table(self, plot=True, directory='.') :
        for this_name in self.histogram_names :
            # group by name
            histograms = [h for h in self.histograms if h.name == this_name]
            for index, h in enumerate(histograms) :
                h.ks_index = index
            print('KS Table has {} entries',len(histograms))
            self.condensed_distance_matrix = ([])

            tasks = []
            for rowindex, h1 in enumerate(histograms) :
                resultstr = rowindex * 'x'
                maxp = None
                minp = None
                for h2 in histograms[rowindex:] :
                    d,p = stats.ks_2samp(h1.samples, h2.samples)
                    self.condensed_distance_matrix = np.append(self.condensed_distance_matrix,d)
                    logging.debug('D,p={},{} cp={}'.format(str(d),str(p), str(self.ks_critical_p)))
                    if not minp or p < minp :
                        minp = p
                    if not maxp or (p != 1 and p > maxp) :
                        maxp = p
                    if p > self.ks_critical_p :
                        resultstr += '1'
                    else :
                        resultstr += '0'
                    if plot :
                        tasks.append(asyncio.ensure_future(flow_histogram.plot_two_sample_ks(h1, h2, directory=directory), loop=iperf_flow.loop))
                print('KS: {0}({1:3d}):{2} minp={3} ptest={4}'.format(this_name, rowindex, resultstr, str(minp), str(self.ks_critical_p)))
                logging.info('KS: {0}({1:3d}):{2} minp={3} ptest={4}'.format(this_name, rowindex, resultstr, str(minp), str(self.ks_critical_p)))
                if tasks :
                    try :
                        logging.debug('runnings KS table plotting coroutines for {} row {}'.format(this_name,str(rowindex)))
                        iperf_flow.loop.run_until_complete(asyncio.wait(tasks, timeout=300, loop=iperf_flow.loop))
                    except asyncio.TimeoutError:
                        logging.error('plot timed out')
                        raise
            self.linkage_matrix=linkage(self.condensed_distance_matrix, 'ward')
            logging.info('{}(distance matrix)\n{}'.format(this_name,scipy.spatial.distance.squareform(self.condensed_distance_matrix)))
            print('{}(cluster linkage)\n{}'.format(this_name,self.linkage_matrix))
            logging.info('{}(cluster linkage)\n{}'.format(this_name,self.linkage_matrix))
            flattened=scipy.cluster.hierarchy.fcluster(self.linkage_matrix, 0.5*self.condensed_distance_matrix.max())
            print('Clusters:{}'.format(flattened))
            logging.info('Clusters:{}'.format(flattened))

class iperf_server(object):

    class IperfServerProtocol(asyncio.SubprocessProtocol):
        def __init__(self, server, flow):
            self.__dict__['flow'] = flow
            self._exited = False
            self._closed_stdout = False
            self._closed_stderr = False
            self._mypid = None
            self._server = server
            self._stdoutbuffer = ""
            self._stderrbuffer = ""

        def __setattr__(self, attr, value):
            if attr in iperf_flow.flow_scope:
                self.flow.__setattr__(self.flow, attr, value)
            else:
                self.__dict__[attr] = value

        # methods and attributes not here are handled by the flow object,
        # aka, the flow object delegates to this object per composition
        def __getattr__(self, attr):
            if attr in iperf_flow.flow_scope:
                return getattr(self.flow, attr)

        @property
        def finished(self):
            return self._exited and self._closed_stdout and self._closed_stderr

        def signal_exit(self):
            if not self.finished:
                return
            self._server.closed.set()
            self._server.opened.clear()

        def connection_made(self, trans):
            self._server.closed.clear()
            self._mypid = trans.get_pid()
            logging.debug('server connection made pid=({})'.format(self._mypid))

        def pipe_data_received(self, fd, data):
            if self.debug :
                logging.debug('{} {}'.format(fd, data))
            data = data.decode("utf-8")
            if fd == 1:
                self._stdoutbuffer += data
                while "\n" in self._stdoutbuffer:
                    line, self._stdoutbuffer = self._stdoutbuffer.split("\n", 1)
                    logging.info('{} {} (stdout,{})'.format(self._server.name, line, self._server.remotepid))
                    if not self._server.opened.is_set() :
                        m = self._server.regex_open_pid.match(line)
                        if m :
                            self._server.remotepid = m.group('pid')
                            self._server.opened.set()
                            logging.debug('{} pipe reading (stdout,{})'.format(self._server.name, self._server.remotepid))
                    else :
                        if self.proto == 'TCP' :
                            m = self._server.regex_traffic.match(line)
                            if m :
                                timestamp = datetime.now()
                                if not self._server.traffic_event.is_set() :
                                    self._server.traffic_event.set()

                                bytes = float(m.group('bytes'))
                                if self.flowstats['current_txbytes'] :
                                    flowrate = round((bytes / self.flowstats['current_txbytes']), 2)
                                    # *consume* the current *txbytes* where the client pipe will repopulate on its next sample
                                    # do this by setting the value to None
                                    self.flowstats['current_txbytes'] = None
                                    # logging.debug('{} flow  ratio={:.2f}'.format(self._server.name, flowrate))
                                    self.flowstats['flowrate'] = flowrate
                                else :
                                    # *produce* the current *rxbytes* so the client pipe can know this event occurred
                                    # indicate this by setting the value to value
                                    self.flowstats['current_rxbytes'] = bytes
                                    self.flowstats['rxdatetime'].append(timestamp)
                                    self.flowstats['rxbytes'].append(m.group('bytes'))
                                    self.flowstats['rxthroughput'].append(m.group('throughput'))
                                    self.flowstats['reads'].append(m.group('reads'))
                        else :
                            m = self._server.regex_final_isoch_traffic.match(line)
                            if m :
                                timestamp = datetime.now(timezone.utc).astimezone()
                                self.flowstats['histogram_names'].add(m.group('pdfname'))
                                self.flowstats['histograms'].append(flow_histogram(name=m.group('pdfname'),values=m.group('pdf'), population=m.group('population'), binwidth=m.group('binwidth'), starttime=self.flowstats['starttime'], endtime=timestamp))
                                # logging.debug('pdf {} {}={}'.format(m.group('pdfname'), m.group('pdf'), m.group('binwidth')))
                                logging.info('pdf {} found with bin width={} us'.format(m.group('pdfname'),  m.group('binwidth')))

            elif fd == 2:
                self._stderrbuffer += data
                while "\n" in self._stderrbuffer:
                    line, self._stderrbuffer = self._stderrbuffer.split("\n", 1)
                    logging.info('{} {} (stderr)'.format(self._server.name, line))


        def pipe_connection_lost(self, fd, exc):
            if fd == 1:
                self._closed_stdout = True
                logging.debug('stdout pipe to {} closed (exception={})'.format(self._server.name, exc))
            elif fd == 2:
                self._closed_stderr = True
                logging.debug('stderr pipe to {} closed (exception={})'.format(self._server.name, exc))
            if self._closed_stdout and self._closed_stderr :
                self.remotepid = None;
            self.signal_exit()

        def process_exited(self):
            logging.debug('subprocess with pid={} closed'.format(self._mypid))
            self._exited = True
            self._mypid = None
            self.signal_exit()

    def __init__(self, name='Server', loop=None, host='localhost', flow=None, debug=False):
        self.__dict__['flow'] = flow
        self.loop = iperf_flow.loop
        self.name = name
        self.iperf = '/usr/local/bin/iperf'
        self.ssh = '/usr/bin/ssh'
        self.host = host
        self.flow = flow
        self.debug = debug
        self.opened = asyncio.Event(loop=self.loop)
        self.closed = asyncio.Event(loop=self.loop)
        self.closed.set()
        self.traffic_event = asyncio.Event(loop=self.loop)
        self._transport = None
        self._protocol = None
        self.time = time

        # ex. Server listening on TCP port 61003 with pid 2565
        self.regex_open_pid = re.compile(r'^Server listening on {} port {} with pid (?P<pid>\d+)'.format(self.proto, str(self.port)))
        # ex. [  4] 0.00-0.50 sec  657090 Bytes  10513440 bits/sec  449    449:0:0:0:0:0:0:0
        self.regex_traffic = re.compile(r'\[\s+\d+] (?P<timestamp>.*) sec\s+(?P<bytes>[0-9]+) Bytes\s+(?P<throughput>[0-9]+) bits/sec\s+(?P<reads>[0-9]+)')
        #ex. [  3] 0.00-21.79 sec T8(f)-PDF: bin(w=10us):cnt(261674)=223:1,240:1,241:1 (5/95%=117/144,obl/obu=0/0)
        self.regex_final_isoch_traffic = re.compile(r'\[\s+\d+\] (?P<timestamp>.*) sec\s+(?P<pdfname>[A-Z][0-9])\(f\)-PDF: bin\(w=(?P<binwidth>[0-9]+)us\):cnt\((?P<population>[0-9]+)\)=(?P<pdf>.+)\s+\([0-9]+/[0-9]+%=[0-9]+/[0-9]+,obl/obu=[0-9]+/[0-9]+\)')

    def __getattr__(self, attr):
        return getattr(self.flow, attr)

    async def start(self, time=time):
        if not self.closed.is_set() :
            return

        self.opened.clear()
        self.remotepid = None
        iperftime = time + 30
        self.sshcmd=[self.ssh, self.user + '@' + self.host, self.iperf, '-s', '-p ' + str(self.port), '-e',  '-t ' + str(iperftime), '-z', '-fb', '-w' , self.window]
        if self.interval >= 0.05 :
            self.sshcmd.extend(['-i ', str(self.interval)])
        if self.proto == 'UDP' :
            self.sshcmd.extend(['-u', '--udp-histogram 10u,50000'])
        logging.info('{}'.format(str(self.sshcmd)))
        self._transport, self._protocol = await self.loop.subprocess_exec(lambda: self.IperfServerProtocol(self, self.flow), *self.sshcmd)
        await self.opened.wait()

    async def signal_stop(self):
        if self.remotepid :
            childprocess = await asyncio.create_subprocess_exec(self.ssh, '{}@{}'.format(self.user, self.host), 'kill', '-HUP', '{}'.format(self.remotepid), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=self.loop)
            logging.debug('({}) sending signal HUP to {} (pid={})'.format(self.user, self.host, self.remotepid))
            stdout, _ = await childprocess.communicate()
            if stdout:
                logging.info('{}({}) {}'.format(self.user, self.host, stdout))
            if not self.closed.is_set() :
                await self.closed.wait()

class iperf_client(object):

    # Asycnio protocol for subprocess transport
    class IperfClientProtocol(asyncio.SubprocessProtocol):
        def __init__(self, client, flow):
            self.__dict__['flow'] = flow
            self._exited = False
            self._closed_stdout = False
            self._closed_stderr = False
            self._mypid = None
            self._client = client
            self._stdoutbuffer = ""
            self._stderrbuffer = ""

        def __setattr__(self, attr, value):
            if attr in iperf_flow.flow_scope:
                self.flow.__setattr__(self.flow, attr, value)
            else:
                self.__dict__[attr] = value

        def __getattr__(self, attr):
            if attr in iperf_flow.flow_scope:
                return getattr(self.flow, attr)

        @property
        def finished(self):
            return self._exited and self._closed_stdout and self._closed_stderr

        def signal_exit(self):
            if not self.finished:
                return
            self._client.closed.set()
            self._client.opened.clear()

        def connection_made(self, trans):
            self._client.closed.clear()
            self._mypid = trans.get_pid()
            logging.debug('client connection made pid=({})'.format(self._mypid))

        def pipe_data_received(self, fd, data):
            if self.debug :
                logging.debug('{} {}'.format(fd, data))
            data = data.decode("utf-8")
            if fd == 1:
                self._stdoutbuffer += data
                while "\n" in self._stdoutbuffer:
                    line, self._stdoutbuffer = self._stdoutbuffer.split("\n", 1)
                    logging.info('{} {} (stdout,{})'.format(self._client.name, line, self._client.remotepid))
                    if not self._client.opened.is_set() :
                        m = self._client.regex_open_pid.match(line)
                        if m :
                            # logging.debug('remote pid match {}'.format(m.group('pid')))
                            self._client.opened.set()
                            self._client.remotepid = m.group('pid')
                            self.flowstats['starttime'] = datetime.now(timezone.utc).astimezone()
                            logging.debug('{} pipe reading at {} (stdout,{})'.format(self._client.name, self.flowstats['starttime'].isoformat(), self._client.remotepid))
                    else :
                        if self.proto == 'TCP':
                            m = self._client.regex_traffic.match(line)
                            if m :
                                timestamp = datetime.now()
                                if not self._client.traffic_event.is_set() :
                                    self._client.traffic_event.set()

                                bytes = float(m.group('bytes'))
                                if self.flowstats['current_rxbytes'] :
                                    flowrate = round((self.flowstats['current_rxbytes'] / bytes), 2)
                                    # *consume* the current *rxbytes* where the server pipe will repopulate on its next sample
                                    # do this by setting the value to None
                                    self.flowstats['current_rxbytes'] = None
                                    # logging.debug('{} flow ratio={:.2f}'.format(self._client.name, flowrate))
                                    self.flowstats['flowrate'] = flowrate
                                else :
                                    # *produce* the current txbytes so the server pipe can know this event occurred
                                    # indicate this by setting the value to value
                                    self.flowstats['current_txbytes'] = bytes

                                self.flowstats['txdatetime'].append(timestamp)
                                self.flowstats['txbytes'].append(m.group('bytes'))
                                self.flowstats['txthroughput'].append(m.group('throughput'))
                                self.flowstats['writes'].append(m.group('writes'))
                                self.flowstats['errwrites'].append(m.group('errwrites'))
                                self.flowstats['retry'].append(m.group('retry'))
                                self.flowstats['cwnd'].append(m.group('cwnd'))
                                self.flowstats['rtt'].append(m.group('rtt'))
                        else :
                            pass

            elif fd == 2:
                self._stderrbuffer += data
                while "\n" in self._stderrbuffer:
                    line, self._stderrbuffer = self._stderrbuffer.split("\n", 1)
                    logging.info('{} {} (stderr)'.format(self._client.name, line))

        def pipe_connection_lost(self, fd, exc):
            if fd == 1:
                logging.debug('stdout pipe to {} closed (exception={})'.format(self._client.name, exc))
                self._closed_stdout = True
            elif fd == 2:
                logging.debug('stderr pipe to {} closed (exception={})'.format(self._client.name, exc))
                self._closed_stderr = True
            self.signal_exit()

        def process_exited(self,):
            logging.debug('subprocess with pid={} closed'.format(self._mypid))
            self._exited = True
            self._mypid = None
            self.signal_exit()

    def __init__(self, name='Client', loop=None, host='localhost', flow = None, debug=False):
        self.__dict__['flow'] = flow
        self.loop = loop
        self.opened = asyncio.Event(loop=self.loop)
        self.closed = asyncio.Event(loop=self.loop)
        self.closed.set()
        self.traffic_event = asyncio.Event(loop=self.loop)
        self.name = name
        self.iperf = '/usr/local/bin/iperf'
        self.ssh = '/usr/bin/ssh'
        self.host = host
        self.debug = debug
        self._transport = None
        self._protocol = None
        # Client connecting to 192.168.100.33, TCP port 61009 with pid 1903
        self.regex_open_pid = re.compile(r'Client connecting to .*, {} port {} with pid (?P<pid>\d+)'.format(self.proto, str(self.port)))
        # traffic ex: [  3] 0.00-0.50 sec  655620 Bytes  10489920 bits/sec  14/211        446      446K/0 us
        self.regex_traffic = re.compile(r'\[\s+\d+] (?P<timestamp>.*) sec\s+(?P<bytes>\d+) Bytes\s+(?P<throughput>\d+) bits/sec\s+(?P<writes>\d+)/(?P<errwrites>\d+)\s+(?P<retry>\d+)\s+(?P<cwnd>\d+)K/(?P<rtt>\d+) us')

    def __getattr__(self, attr):
        return getattr(self.flow, attr)

    async def start(self, time=time):
        if not self.closed.is_set() :
            return

        self.opened.clear()
        self.remotepid = None
        if time:
            iperftime = time + 30
        else :
            ipertime = self.time + 30
        self.sshcmd=[self.ssh, self.user + '@' + self.host, self.iperf, '-c', self.dst, '-p ' + str(self.port), '-e', '-t ' + str(iperftime), '-z', '-fb', '-S ', iperf_flow.txt_to_tos(self.tos), '-w' , self.window]
        if self.interval >= 0.05 :
            self.sshcmd.extend(['-i ', str(self.interval)])

        if self.proto == 'UDP' and self.offered_load :
            self.sshcmd.extend(['-u', '--isochronous', self.offered_load])
        elif self.proto == 'TCP' and self.offered_load :
            self.sshcmd.extend(['-b', self.offered_load])

        logging.info('{}'.format(str(self.sshcmd)))
        try :
            self._transport, self._protocol = await self.loop.subprocess_exec(lambda: self.IperfClientProtocol(self, self.flow), *self.sshcmd)
            await self.opened.wait()
        except:
            logging.error('flow client start error')
            raise

    async def signal_stop(self):
        if self.remotepid :
            childprocess = await asyncio.create_subprocess_exec(self.ssh, '{}@{}'.format(self.user, self.host), 'kill', '-INT', '{}'.format(self.remotepid), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=self.loop)
            logging.debug('({}) sending signal HUP to {} (pid={})'.format(self.user, self.host, self.remotepid))
            stdout, _ = await childprocess.communicate()
            if stdout:
                logging.info('{}({}) {}'.format(self.user, self.host, stdout))
            if not self.closed.is_set():
                await self.closed.wait()

    async def signal_pause(self):
        if self.remotepid :
            childprocess = await asyncio.create_subprocess_exec(self.ssh, '{}@{}'.format(self.user, self.host), 'kill', '-STOP', '{}'.format(self.remotepid), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=self.loop)
            logging.debug('({}) sending signal STOP to {} (pid={})'.format(self.user, self.host, self.remotepid))
            stdout, _ = await childprocess.communicate()
            if stdout:
                logging.info('{}({}) {}'.format(self.user, self.host, stdout))
            if not self.closed.is_set():
                await self.closed.wait()

    async def signal_resume(self):
        if self.remotepid :
            childprocess = await asyncio.create_subprocess_exec(self.ssh, '{}@{}'.format(self.user, self.host), 'kill', '-CONT', '{}'.format(self.remotepid), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=self.loop)
            logging.debug('({}) sending signal CONT to {} (pid={})'.format(self.user, self.host, self.remotepid))
            stdout, _ = await childprocess.communicate()
            if stdout:
                logging.info('{}({}) {}'.format(self.user, self.host, stdout))
            if not self.closed.is_set():
                await self.closed.wait()

class flow_histogram(object):

    @classmethod
    async def plot_two_sample_ks(cls, h1=None, h2=None, outputtype='png', directory='.') :

        title = 'Two Sample KS({},{})'.format(h1.ks_index, h2.ks_index)
        if h1.basefilename is None :
            h1.output_dir = directory + '/' + h1.name + '_' + str(h1.ks_index)
            await h1.write(directory=h1.output_dir)

        if h2.basefilename is None :
            h2.output_dir = directory + '/' + h2.name + '_' + str(h2.ks_index)
            await h2.write(directory=h2.output_dir)

        if (h1.basefilename is not None) and (h2.basefilename is not None) :
            basefilename = '{}_{}_{}'.format(h1.basefilename, h1.ks_index, h2.ks_index)
            gpcfilename = basefilename + '.gpc'
            #write out the gnuplot control file
            with open(gpcfilename, 'w') as fid :
                if outputtype == 'canvas' :
                    fid.write('set output \"{}.{}\"\n'.format(basefilename, 'html'))
                    fid.write('set terminal canvas standalone mousing size 1024,768\n')
                if outputtype == 'svg' :
                    fid.write('set output \"{}_svg.{}\"\n'.format(basefilename, 'html'))
                    fid.write('set terminal svg size 1024,768 dynamic mouse\n')
                else :
                    fid.write('set output \"{}.{}\"\n'.format(basefilename, 'png'))
                    fid.write('set terminal png size 1024,768\n')

                fid.write('set key bottom\n')
                fid.write('set title \"{}\"\n'.format(title))
                fid.write('set format x \"%.0f"\n')
                fid.write('set format y \"%.1f"\n')
                fid.write('set yrange [0:1.01]\n')
                fid.write('set y2range [0:*]\n')
                fid.write('set ytics add 0.1\n')
                fid.write('set y2tics nomirror\n')
                fid.write('set grid\n')
                fid.write('set xlabel \"time (ms)\\n{} - {}\"\n'.format(h1.starttime, h2.endtime))
                if h1.max < 5.0 and h2.max < 5.0 :
                    fid.write('set xrange [0:5]\n')
                    fid.write('set xtics auto\n')
                elif h1.max < 10.0 and h2.max < 10.0:
                    fid.write('set xrange [0:10]\n')
                    fid.write('set xtics add 1\n')
                elif h1.max < 20.0 and h2.max < 20.0 :
                    fid.write('set xrange [0:20]\n')
                    fid.write('set xtics add 1\n')
                elif h1.max < 40.0 and h2.max < 40.0:
                    fid.write('set xrange [0:40]\n')
                    fid.write('set xtics add 5\n')
                elif h1.max < 50.0 and h2.max < 50.0:
                    fid.write('set xrange [0:50]\n')
                    fid.write('set xtics add 5\n')
                elif h1.max < 75.0 and h2.max < 75.0:
                    fid.write('set xrange [0:75]\n')
                    fid.write('set xtics add 5\n')
                else :
                    fid.write('set xrange [0:100]\n')
                    fid.write('set xtics add 10\n')
                fid.write('plot \"{0}\" using 1:2 index 0 axes x1y2 with impulses linetype 3 notitle,  \"{1}\" using 1:2 index 0 axes x1y2 with impulses linetype 2 notitle, \"{1}\" using 1:3 index 0 axes x1y1 with lines linetype 1 linewidth 2 notitle, \"{0}\" using 1:3 index 0 axes x1y1 with lines linetype -1 linewidth 2 notitle\n'.format(h1.datafilename, h2.datafilename))

            childprocess = await asyncio.create_subprocess_exec(flow_histogram.gnuplot,gpcfilename, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=iperf_flow.loop)
            stdout, stderr = await childprocess.communicate()
            if stderr :
                logging.error('Exec {} {}'.format(flow_histogram.gnuplot, gpcfilename))
            else :
                logging.debug('Exec {} {}'.format(flow_histogram.gnuplot, gpcfilename))

    gnuplot = '/usr/bin/gnuplot'
    def __init__(self, binwidth=None, name=None, values=None, population=None, starttime=None, endtime=None, title=None) :
        self.raw = values
        self._entropy = None
        self.bins = self.raw.split(',')
        self.name = name
        self.ks_index = None
        self.population = int(population)
        self.samples = np.zeros(int(self.population))
        self.binwidth = int(binwidth)
        self.createtime = datetime.now(timezone.utc).astimezone()
        self.starttime=starttime
        self.endtime=endtime
        self.title=title
        self.basefilename = None
        ix = 0
        for bin in self.bins :
            x,y = bin.split(':')
            for i in range(int(y)) :
                self.samples[ix] = x
                ix += 1

    @property
    def entropy(self) :
        if not self._entropy :
            self._entropy = 0
            for bin in self.bins :
                x,y = bin.split(':')
                y1 = float(y) / float(self.population)
                self._entropy -= y1 * math.log2(y1)
        return self._entropy

    async def __exec_gnuplot(self) :
        logging.info('Plotting {} {}'.format(self.name, self.gpcfilename))
        childprocess = await asyncio.create_subprocess_exec(flow_histogram.gnuplot, self.gpcfilename, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, loop=iperf_flow.loop)
        stdout, stderr = await childprocess.communicate()
        if stderr :
            logging.error('Exec {} {}'.format(flow_histogram.gnuplot, self.gpcfilename))
        else  :
            logging.debug('Exec {} {}'.format(flow_histogram.gnuplot, self.gpcfilename))

    async def write(self, directory='.', filename=None) :
        # write out the datafiles for the plotting tool,  e.g. gnuplot
        if filename is None:
            filename = self.name

        if not os.path.exists(directory):
            logging.debug('Making results directory {}'.format(directory))
            os.makedirs(directory)

        logging.debug('Writing {} results to directory {}'.format(directory, filename))
        basefilename = os.path.join(directory, filename)
        datafilename = os.path.join(directory, filename + '.data')
        self.max  = None
        with open(datafilename, 'w') as fid :
            cummulative = 0
            for bin in self.bins :
                x,y = bin.split(':')
                #logging.debug('bin={} x={} y={}'.format(bin, x, y))
                cummulative += float(y)
                perc = cummulative / float(self.population)
                if not self.max and (perc > 0.98) :
                    self.max = float(x) * float(self.binwidth) / 1000.0
                    logging.debug('98% max = {}'.format(self.max))
                fid.write('{} {} {}\n'.format((float(x) * float(self.binwidth) / 1000.0), int(y), perc))

        if self.max :
            self.basefilename = basefilename
            self.datafilename = datafilename
        else :
            self.basefilename = None

    async def async_plot(self, title=None, directory='.', outputtype='png', filename=None) :
        if self.basefilename is None :
            await self.write(directory=directory, filename=filename)

        if self.basefilename is not None :
            self.gpcfilename = self.basefilename + '.gpc'
            #write out the gnuplot control file
            with open(self.gpcfilename, 'w') as fid :
                if outputtype == 'canvas' :
                    fid.write('set output \"{}.{}\"\n'.format(basefilename, 'html'))
                    fid.write('set terminal canvas standalone mousing size 1024,768\n')
                if outputtype == 'svg' :
                    fid.write('set output \"{}_svg.{}\"\n'.format(basefilename, 'html'))
                    fid.write('set terminal svg size 1024,768 dynamic mouse\n')
                else :
                    fid.write('set output \"{}.{}\"\n'.format(basefilename, 'png'))
                    fid.write('set terminal png size 1024,768\n')

                if not title and self.title :
                    title = self.title

                fid.write('set key bottom\n')
                if self.ks_index is not None :
                    fid.write('set title \"{}({}) {}({}) E={}\"\n'.format(self.name, str(self.ks_index), title, int(self.population), self.entropy))
                else :
                    fid.write('set title \"{}{}({}) E={}\"\n'.format(self.name, title, int(self.population), self.entropy))
                fid.write('set format x \"%.0f"\n')
                fid.write('set format y \"%.1f"\n')
                fid.write('set yrange [0:1.01]\n')
                fid.write('set y2range [0:*]\n')
                fid.write('set ytics add 0.1\n')
                fid.write('set y2tics nomirror\n')
                fid.write('set grid\n')
                fid.write('set xlabel \"time (ms)\\n{} - {}\"\n'.format(self.starttime, self.endtime))
                if self.max < 5.0 :
                    fid.write('set xrange [0:5]\n')
                    fid.write('set xtics auto\n')
                elif self.max < 10.0 :
                    fid.write('set xrange [0:10]\n')
                    fid.write('set xtics add 1\n')
                elif self.max < 20.0 :
                    fid.write('set xrange [0:20]\n')
                    fid.write('set xtics add 1\n')
                elif self.max < 40.0 :
                    fid.write('set xrange [0:40]\n')
                    fid.write('set xtics add 5\n')
                elif self.max < 50.0 :
                    fid.write('set xrange [0:50]\n')
                    fid.write('set xtics add 5\n')
                elif self.max < 75.0 :
                    fid.write('set xrange [0:75]\n')
                    fid.write('set xtics add 5\n')
                else :
                    fid.write('set xrange [0:100]\n')
                    fid.write('set xtics add 10\n')
                fid.write('plot \"{0}\" using 1:2 index 0 axes x1y2 with impulses linetype 3 notitle, \"{0}\" using 1:3 index 0 axes x1y1 with lines linetype -1 linewidth 2 notitle\n'.format(datafilename))

                if outputtype == 'png' :
                    # Create a thumbnail too
                    fid.write('unset output; unset xtics; unset ytics; unset key; unset xlabel; unset ylabel; unset border; unset grid; unset yzeroaxis; unset xzeroaxis; unset title; set lmargin 0; set rmargin 0; set tmargin 0; set bmargin 0\n')
                    fid.write('set output \"{}_thumb.{}\"\n'.format(basefilename, 'png'))
                    fid.write('set terminal png transparent size 64,32 crop\n')
                    fid.write('plot \"{0}\" using 1:2 index 0 axes x1y2 with impulses linetype 3 notitle, \"{0}\" using 1:3 index 0 axes x1y1 with lines linetype -1 linewidth 2 notitle\n'.format(datafilename))

            await self.__exec_gnuplot()
