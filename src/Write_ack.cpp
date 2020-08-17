/*---------------------------------------------------------------
 * Copyright (c) 2019
 * Broadcom Corporation
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 *
 * Redistributions of source code must retain the above
 * copyright notice, this list of conditions and
 * the following disclaimers.
 *
 *
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimers in the documentation and/or other materials
 * provided with the distribution.
 *
 *
 * Neither the name of Broadcom Coporation,
 * nor the names of its contributors may be used to endorse
 * or promote products derived from this Software without
 * specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ________________________________________________________________
 *
 * write_acks
 * Application level write ack's back to sender
 *
 *
 * by Robert J. McMahon (rjmcmahon@rjmcmahon.com, bob.mcmahon@broadcom.com)
 * -------------------------------------------------------------------
 */
#include "headers.h"
#include "Write_ack.hpp"
#include "packet_ring.h"

WriteAck::WriteAck(thread_Settings *inSettings) {
  mSettings = inSettings;
#ifdef HAVE_THREAD_DEBUG
  thread_debug("Write ack in constructor (%p) (%x/%x)", (void *) inSettings, inSettings->flags, inSettings->flags_extend);
#endif
#ifdef TCP_NODELAY
  int optflag=1;
  int rc;
  // Disable Nagle to reduce latency of this intial message
  if ((rc = setsockopt( mSettings->mSock, IPPROTO_TCP, TCP_NODELAY, (char *)&optflag, sizeof(int))) < 0 ) {
    WARN_errno(rc < 0, "tcpnodelay write-ack" );
  } else {
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Write ack set TCP_NODELAY sock=%d", inSettings->mSock);
#endif
  }
#endif
}

WriteAck::~WriteAck() {
#ifdef HAVE_THREAD_DEBUG
  thread_debug("Write ack destructor (%p)", (void *) mSettings);
#endif
}

void WriteAck::Close(PacketRing *pr) {
#ifdef HAVE_THREAD_DEBUG
  thread_debug("Enqueue ackring final packet %p", (void *) pr);
#endif
  Condition_Lock((*(pr->awake_producer)));
  while (!pr->consumerdone) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Server await write ack thread done per cond %p/%d", \
		  (void *) pr->awake_producer, pr->consumerdone);
#endif
    Condition_TimedWait(pr->awake_producer, 1);
  }
  Condition_Unlock((*(pr->awake_producer)));
#ifdef HAVE_THREAD_DEBUG
  thread_debug( "Server thread thinks write ack thread is done with %p", (void *) pr);
#endif
  // Server must free the ack ring as it's the final user of it
  free_ackring(pr);
}

void WriteAck::RunServer(void) {
  struct ReportStruct *packet = NULL;
  while ((packet = dequeue_ackring(mSettings->ackring))) {
    if (packet->packetID < 0)
      break;
    int len = write(mSettings->mSock, packet, sizeof(ReportStruct));
#ifdef HAVE_THREAD_DEBUG
    // thread_debug("Write ack (bytes=%d ack len=%d) sent %p (%p)", packet->packetLen, len, (void *) packet, (void *) mSettings);
#endif
    if (len != sizeof(ReportStruct)) {
      fprintf(stderr, "Write ack length error: got %d but expected %d\n",len, (int) sizeof(ReportStruct));
    }
  }
#ifdef HAVE_THREAD_DEBUG
  thread_debug("Write ack got final packet %p so signal cond %p (%p)", (void *) packet, \
	       (void *) mSettings->ackring->awake_producer, (void *) mSettings);
#endif
  mSettings->ackring->consumerdone = 1;
  Condition_Signal(mSettings->ackring->awake_producer);
};

void WriteAck::RunClient(void) {
};
