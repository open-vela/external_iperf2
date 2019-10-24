/*---------------------------------------------------------------
 * Copyright (c) 2017
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
 * isochronous.c
 * Suppport for isochonronous traffic testing
 *
 * by Robert J. McMahon (rjmcmahon@rjmcmahon.com, bob.mcmahon@broadcom.com)
 * -------------------------------------------------------------------
 */
#include "headers.h"
#include "Timestamp.hpp"
#include "isochronous.hpp"
#include "delay.h"

using namespace Isochronous;

FrameCounter::FrameCounter(double value, Timestamp start)  : frequency(value) {
    period = (unsigned int) (1000000 / frequency);
    startTime = start;
    lastcounter = 0;
}
FrameCounter::FrameCounter(double value)  : frequency(value) {
    period = (unsigned int) (1000000 / frequency);
    lastcounter = 0;
}

#if defined(HAVE_CLOCK_NANOSLEEP)
unsigned int FrameCounter::wait_tick(void) {
    Timestamp txslot = next_slot();
    unsigned int mycounter = get(txslot);
    timespec txtime_ts;
    txtime_ts.tv_sec = txslot.getSecs();
    txtime_ts.tv_nsec = txslot.getUsecs() * 1000;
    int rc = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &txtime_ts, NULL);
    // printf("last=%d current=%d\n", lastcounter, mycounter);
    if (rc) {
	fprintf(stderr, "txstart failed clock_nanosleep()=%d\n", rc);
    } else if (lastcounter && ((mycounter - lastcounter) > 1))
	slip++;
#ifdef HAVE_THREAD_DEBUG
    // thread_debug("Client tick occurred per %ld.%ld", txtime_ts.tv_sec, txtime_ts.tv_nsec / 1000);
#endif
    lastcounter = mycounter;
    return(mycounter);
}
#else
unsigned int FrameCounter::wait_tick(void) {
    long remaining;
    unsigned int framecounter;

    if (!lastcounter) {
	reset();
	framecounter = 1;
    } else {
	framecounter = get(&remaining);
	if ((framecounter - lastcounter) > 1)
	    slip++;
    	delay_loop(remaining);
	framecounter ++;
    }
    lastcounter = framecounter;
    return(framecounter);
}
#endif
inline unsigned int FrameCounter::get(void) {
    Timestamp sampleTime;  // Constructor will initialize timestamp to now
    long usecs = sampleTime.subUsec(startTime);
    // This will round towards zero per the integer divide
    unsigned int counter = (unsigned int) (usecs / period);
    return(counter + 1); // Frame counter for packets starts at 1
}

inline unsigned int FrameCounter::get(Timestamp slot) {
    long usecs = -startTime.subUsec(slot);
    // This will round towards zero per the integer divide
    unsigned int counter = (unsigned int) (usecs / period);
    return(counter + 1); // Frame counter for packets starts at 1
}

inline unsigned int FrameCounter::get(long *ticks_remaining) {
    assert(ticks_remaing);
    Timestamp sampleTime;  // Constructor will initialize timestamp to now
    long usecs = -startTime.subUsec(sampleTime);
    unsigned int counter = (unsigned int) (usecs / period);
    // figure out how many usecs before the next frame counter tick
    // the caller can use this to delay until the next tick
    *ticks_remaining = (counter * period) - usecs;
    return(counter + 1); // Frame counter for packets starts at 1
}

inline Timestamp FrameCounter::next_slot(void) {
    Timestamp next = startTime;
    slot_counter = get();
    // period unit is in microseconds, convert to seconds
    next.add(slot_counter * (period / 1e6));
    return next;
}

unsigned int FrameCounter::period_us(void) {
    return(period);
}

void FrameCounter::reset(void) {
    period = (1000000 / frequency);
    startTime.setnow();
}

unsigned int FrameCounter::wait_sync(long sec, long usec) {
    long remaining;
    unsigned int framecounter;
    startTime.set(sec, usec);
    framecounter = get(&remaining);
    delay_loop(remaining);
    reset();
    framecounter = 1;
    lastcounter = 1;
    return(framecounter);
}

long FrameCounter::getSecs( void ) {
    return startTime.getSecs();
}

long FrameCounter::getUsecs( void ) {
    return startTime.getUsecs();
}
