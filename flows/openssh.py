#!/usr/bin/env python3.5
#
# ---------------------------------------------------------------
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
#
# Python object to support remote command and remote pipes over openssh with concurrency
#
# Basic use looks something like:
#    sessions = [ssh_session() for i in range(count)]
#    loop = asyncio.get_event_loop()
#    tasks = [asyncio.ensure_future(session.rexec(host=remotehost,cmd=remotecommand)) for session in sessions]
#    loop.run_until_complete(asyncio.wait(tasks))
#    loop.close
#
# Date July 2016
import re
import subprocess
import logging
import asyncio, sys
import time
import locale
import signal
import os, fcntl


logger = logging.getLogger(__name__)

class ssh_session:
    def __init__(self, loop=None, user='root', host='localhost', cmd='pwd'):
        self.IO_TIMEOUT = 2.0
        self.CMD_TIMEOUT = 30
        self.ssh = "/usr/bin/ssh"
        self.host = host
        self.user = user
        self.cmd = cmd
        if loop is not None:
            self.loop = loop
        else:
            self.loop = asyncio.get_event_loop()
        self.done = asyncio.Event(loop=loop)

    async def rexec(self, cmd=None):
        self.done.clear()
        try:
            sshcmd = [self.ssh, self.user + "@" + self.host, self.cmd]
            logging.info('{}({}) {}'.format(self.user, self.host, str(sshcmd)))
            self.childprocess = subprocess.Popen(sshcmd, bufsize=1, universal_newlines = True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False)
            self.loop.add_reader(self.childprocess.stdout, self.stdout_event)
            self.loop.add_reader(self.childprocess.stderr, self.stderr_event)
            self.iowatchdog = self.loop.call_later(self.IO_TIMEOUT, self.io_timer)
            logging.debug('await rexec pid={}'.format(self.childprocess.pid))
            await self.done.wait()
            self.iowatchdog.cancel()
            logging.debug('await rexec done: pid={}'.format(self.childprocess.pid))

        except:
            logging.error("{}({}) {}".format(str(self.host), str(sshcmd)))
            pass

    def stdout_event(self):
        self.iowatchdog.cancel()
        trailer = '{}({})'.format(self.childprocess.pid, self.childprocess.stdout.name)
        while True :
            data = self.childprocess.stdout.readline()
            if data :
                logging.info('{}: {} {}'.format(self.host, str(data).strip('\r\n'), trailer))
                trailer = ''
            else :
                break

        self.childprocess.poll()
        if self.childprocess.returncode is not None :
            logging.debug("{}: stdout closed: pid={}".format(self.host, self.childprocess.pid))
            self.childprocess.stdout.close()
            self.loop.remove_reader(self.childprocess.stdout)
            self.close_actions(origin="stderr")
        else :
            self.iowatchdog = self.loop.call_later(self.IO_TIMEOUT, self.io_timer)

    def stderr_event(self):
        self.iowatchdog.cancel()
        trailer = '{}({})'.format(self.childprocess.pid, self.childprocess.stderr.name)
        while True :
            data = self.childprocess.stderr.readline()
            if data :
                logging.info('{}: {} {}'.format(self.host, str(data).strip('\r\n'), trailer))
                trailer = ''
            else :
                break

        self.childprocess.poll()
        if self.childprocess.returncode is not None :
            logging.debug("{}: stderr closed: pid={}".format(self.host, self.childprocess.pid))
            self.childprocess.stderr.close()
            self.loop.remove_reader(self.childprocess.stderr)
            self.close_actions(origin="stderr")
        else :
            self.iowatchdog = self.loop.call_later(self.IO_TIMEOUT, self.io_timer)

    def close_actions(self,origin):
        self.returncode = self.childprocess.returncode
        if self.childprocess.stdout.closed and self.childprocess.stderr.closed :
            logging.debug("{}: DONE pid={}".format(self.host, self.childprocess.pid))
            self.done.set()

    def io_timer(self):
        logging.debug("{}: IO timeout: pid={}".format(self.host, self.childprocess.pid))
        self.childprocess.send_signal(signal.SIGKILL)
        self.done.set()


