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

struct PacketRing * init_packetring (int count, struct Condition *awake_consumer, struct Condition *awake_producer) {
    struct PacketRing *pr = NULL;
    if ((pr = (struct PacketRing *) calloc(1, sizeof(struct PacketRing)))) {
	pr->data = (struct ReportStruct *) calloc(count, sizeof(struct ReportStruct));
    }
    if (!pr || !pr->data) {
	fprintf(stderr, "ERROR: no memory for packet ring\n");
	exit(1);
    }
    pr->producer = 0;
    pr->consumer = 0;
    pr->maxcount = count;
    pr->awake_consumer = awake_consumer;
    pr->awake_producer = awake_producer;
    pr->consumerdone = 0;
    pr->awaitcounter = 0;
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init %d element packet ring=%p producer=%p consumer=%p", count, \
		 (void *)pr, (void *) pr->awake_producer, (void *) pr->awake_consumer);
#endif
    return (pr);
}


inline void enqueue_packetring(struct PacketRing *pr, struct ReportStruct *metapacket) {
    while (((pr->producer == pr->maxcount) && (pr->consumer == 0)) || \
	   ((pr->producer + 1) == pr->consumer)) {
	// Signal the consumer thread to process a full queue
        Condition_Signal(pr->awake_consumer);
	// Wait for the consumer to create some queue space
	Condition_Lock((*(pr->awake_producer)));
	pr->awaitcounter++;
#ifdef HAVE_THREAD_DEBUG_PERF
	{
	    struct timeval now;
	    static struct timeval prev={.tv_sec=0, .tv_usec=0};
	    gettimeofday( &now, NULL );
	    if (!prev.tv_sec || (TimeDifference(now, prev) > 1.0)) {
		prev = now;
		thread_debug( "Not good, traffic's packet ring %p stalled per %p", (void *)pr, (void *)&pr->awake_producer);
	    }
	}
#endif
	int timeout;
	Condition_TimedWait(pr->awake_producer, 1, timeout);
	if (!timeout)
	    Condition_Unlock((*(pr->awake_producer)));
    }
    int writeindex;
    if ((pr->producer + 1) == pr->maxcount)
	writeindex = 0;
    else
	writeindex = (pr->producer  + 1);

    /* Next two lines must be maintained as is */
    memcpy((pr->data + writeindex), metapacket, sizeof(struct ReportStruct));
    pr->producer = writeindex;
}

inline struct ReportStruct *dequeue_packetring(struct PacketRing *pr) {
    struct ReportStruct *packet = NULL;
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
      // thread_debug( "Consumer signal packet ring %p empty per %p", (void *)pr, (void *)&pr->awake_producer);
#endif
	Condition_Signal(pr->awake_producer);
    }
    return packet;
}

inline void enqueue_ackring(struct PacketRing *pr, struct ReportStruct *metapacket) {
    enqueue_packetring(pr, metapacket);
    // Keep the latency low by signaling the consumer thread
    // per each enqueue
#ifdef HAVE_THREAD_DEBUG
    // thread_debug( "Producer signal consumer ackring=%p per %p", (void *)pr, (void *)&pr->awake_consumer);
#endif
    Condition_Signal(pr->awake_consumer);
}

inline struct ReportStruct *dequeue_ackring(struct PacketRing *pr) {
    struct ReportStruct *packet = NULL;


  Condition_Lock((*(pr->awake_consumer)));
  int timeout = 0;
  while (((packet = dequeue_packetring(pr)) == NULL) && !timeout) {
      timeout = 0;
      Condition_TimedWait(pr->awake_consumer, 1, timeout);
  }
  if (!timeout)
      Condition_Unlock((*(pr->awake_consumer)));
  if (packet) {
    // Signal the producer thread for low latency
    // indication of space available
    Condition_Signal(pr->awake_producer);
  }
  return packet;
}

void free_packetring(struct PacketRing *pr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Free packet ring=%p producer=%p (consumer=%p)", \
		 (void *)pr, (void *) pr->awake_producer, (void *) pr->awake_consumer);
#endif
    if (pr->awaitcounter > 1000) fprintf(stderr, "WARN: Reporter thread may be too slow, await counter=%d, " \
					 "consider increasing NUM_REPORT_STRUCTS\n", pr->awaitcounter);
    Condition_Destroy(pr->awake_producer);
    if (pr->data) free(pr->data);
}

void free_ackring(struct PacketRing *pr) {
    free_packetring(pr);
    Condition_Destroy(pr->awake_consumer);
}

/*
 * This is an estimate and can be incorrect as these counters
 * done like this is not thread safe.  Use with care as there
 * is no guarantee the return value is accurate
 */
#ifdef HAVE_THREAD_DEBUG
inline int getcount_packetring(struct PacketRing *pr) {
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
