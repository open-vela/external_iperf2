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
 * Suppport for packet rings between threads
 *
 * by Robert J. McMahon (rjmcmahon@rjmcmahon.com, bob.mcmahon@broadcom.com)
 * -------------------------------------------------------------------
 */
#include "headers.h"
#include "packet_ring.h"
#include "Condition.h"
#include "Thread.h"

PacketRing * init_packetring (int count, Condition *awake_consumer) {
    PacketRing *pr = NULL;
    if ((pr = (PacketRing *) calloc(1, sizeof(PacketRing)))) {
	pr->data = (ReportStruct *) calloc(count, sizeof(ReportStruct));
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Init %d element packet ring %p", count, (void *)pr);
#endif
    }
    if (!pr || !pr->data) {
	fprintf(stderr, "ERROR: no memory for packet ring\n");
	exit(1);
    }
    pr->producer = 0;
    pr->consumer = 0;
    pr->maxcount = count;
    pr->awake_consumer = awake_consumer;
    Condition_Initialize(&pr->await_consumer);
    pr->consumerdone = 0;
    pr->awaitcounter = 0;
    return (pr);
}

inline void enqueue_packetring(PacketRing *pr, ReportStruct *metapacket) {
    while (((pr->producer == pr->maxcount) && (pr->consumer == 0)) || \
	   ((pr->producer + 1) == pr->consumer)) {
	// Signal the consumer thread to process a full queue
	Condition_Signal(pr->awake_consumer);
	// Wait for the consumer to create some queue space
	Condition_Lock(pr->await_consumer);
	pr->awaitcounter++;
#ifdef HAVE_THREAD_DEBUG
	{
	    struct timeval now;
	    static struct timeval prev={.tv_sec=0, .tv_usec=0};
	    gettimeofday( &now, NULL );
	    if (!prev.tv_sec || (TimeDifference(now, prev) > 1.0)) {
		prev = now;
		thread_debug( "Not good, traffic's packet ring %p stalled per %p", (void *)pr, (void *)&pr->await_consumer);
	    }
	}
#endif
	Condition_TimedWait(&pr->await_consumer, 1);
	Condition_Unlock(pr->await_consumer);
    }
    int writeindex;
    if ((pr->producer + 1) == pr->maxcount)
	writeindex = 0;
    else
	writeindex = (pr->producer  + 1);

    /* Next two lines must be maintained as is */
    memcpy((pr->data + writeindex), metapacket, sizeof(ReportStruct));
    pr->producer = writeindex;
}

inline ReportStruct *dequeue_packetring(PacketRing *pr) {
    ReportStruct *packet = NULL;
    if (pr->producer == pr->consumer)
	return NULL;

    int readindex;
    if ((pr->consumer + 1) == pr->maxcount)
	readindex = 0;
    else
	readindex = (pr->consumer + 1);
    packet = (pr->data + readindex);
    // advance the consumer pointer last
    pr->consumer = readindex;
    // Signal the traffic thread assigned to this ring
    // when the ring goes from having something to empty
    if (pr->producer == pr->consumer) {
#ifdef HAVE_THREAD_DEBUG
      // thread_debug( "Consumer signal packet ring %p empty per %p", (void *)pr, (void *)&pr->await_consumer);
#endif
	Condition_Signal(&pr->await_consumer);
    }
    return packet;
}

/*
 * This is an estimate and can be incorrect as these counters
 * done like this is not thread safe.  Use with care as there
 * is no guarantee the return value is accurate
 */
#ifdef HAVE_THREAD_DEBUG
inline int getcount_packetring(PacketRing *pr) {
    int depth = 0;
    if (pr->producer != pr->consumer) {
        depth = (pr->producer > pr->consumer) ? \
	    (pr->producer - pr->consumer) :  \
	    ((pr->maxcount - pr->consumer) + pr->producer);
        // printf("DEBUG: Depth=%d for packet ring %p\n", depth, (void *)pr);
    }
    return depth;
}
#endif
