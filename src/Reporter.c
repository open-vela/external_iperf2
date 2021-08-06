/*---------------------------------------------------------------
 * Copyright (c) 1999,2000,2001,2002,2003
 * The Board of Trustees of the University of Illinois
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software (Iperf) and associated
 * documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
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
 * Neither the names of the University of Illinois, NCSA,
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
 * National Laboratory for Applied Network Research
 * National Center for Supercomputing Applications
 * University of Illinois at Urbana-Champaign
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________
 *
 * Reporter.c
 * by Kevin Gibbs <kgibbs@nlanr.net>
 *
 * Major rewrite by Robert McMahon (Sept 2020, ver 2.0.14)
 * ________________________________________________________________ */

#include <math.h>
#include "headers.h"
#include "Settings.hpp"
#include "util.h"
#include "Reporter.h"
#include "Thread.h"
#include "Locale.h"
#include "PerfSocket.hpp"
#include "SocketAddr.h"
#include "histogram.h"
#include "delay.h"
#include "packet_ring.h"
#include "payloads.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef INITIAL_PACKETID
# define INITIAL_PACKETID 0
#endif

struct ReportHeader *ReportRoot = NULL;
struct ReportHeader *ReportPendingHead = NULL;
struct ReportHeader *ReportPendingTail = NULL;

// Reporter's reset of stats after a print occurs
static void reporter_reset_transfer_stats_client_tcp(struct TransferInfo *stats);
static void reporter_reset_transfer_stats_client_udp(struct TransferInfo *stats);
static void reporter_reset_transfer_stats_server_udp(struct TransferInfo *stats);
static void reporter_reset_transfer_stats_server_tcp(struct TransferInfo *stats);

#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static inline bool sample_tcpistats(struct ReporterData *data, struct ReportStruct *sample, struct tcp_info *tcp_stats);
static inline void reporter_handle_packet_tcpistats(struct ReporterData *data, struct ReportStruct *packet);
#endif

static struct ConnectionInfo *myConnectionReport;

void PostReport (struct ReportHeader *reporthdr) {
#ifdef HAVE_THREAD_DEBUG
    char rs[REPORTTXTMAX];
    reporttype_text(reporthdr, &rs[0]);
    thread_debug("Jobq *POST* report %p (%s)", reporthdr, &rs[0]);
#endif
    if (reporthdr) {
#ifdef HAVE_THREAD
	/*
	 * Update the ReportRoot to include this report.
	 */
	Condition_Lock(ReportCond);
	reporthdr->next = NULL;
	if (!ReportPendingHead) {
	  ReportPendingHead = reporthdr;
	  ReportPendingTail = reporthdr;
	} else {
	  ReportPendingTail->next = reporthdr;
	  ReportPendingTail = reporthdr;
	}
	Condition_Unlock(ReportCond);
	// wake up the reporter thread
	Condition_Signal(&ReportCond);
#else
	/*
	 * Process the report in this thread
	 */
	reporthdr->next = NULL;
	reporter_process_report(reporthdr);
#endif
    }
}
/*
 * ReportPacket is called by a transfer agent to record
 * the arrival or departure of a "packet" (for TCP it
 * will actually represent many packets). This needs to
 * be as simple and fast as possible as it gets called for
 * every "packet".
 */
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
bool ReportPacket (struct ReporterData* data, struct ReportStruct *packet, struct tcp_info *tcp_stats) {
    assert(data != NULL);
    struct TransferInfo *stats = &data->info;
    bool rc = false;
  #ifdef HAVE_THREAD_DEBUG
    if (packet->packetID < 0) {
	thread_debug("Reporting last packet for %p  qdepth=%d sock=%d", (void *) data, packetring_getcount(data->packetring), data->info.common->socket);
    }
  #endif
    if (stats->common->enable_sampleTCPstats) {
	packet->tcpistat_valid = false;
	if (stats->common->intervalonly_sampleTCPstats) {
	    if (TimeDifference(stats->ts.nextTCPStampleTime, packet->packetTime) < 0) {
		rc = sample_tcpistats(data, packet, tcp_stats);
		TimeAdd(stats->ts.nextTCPStampleTime, stats->ts.intervalTime);
	    }
	} else {
	    rc = sample_tcpistats(data, packet, tcp_stats);
	}
    }
    // Note for threaded operation all that needs
    // to be done is to enqueue the packet data
    // into the ring.
    packetring_enqueue(data->packetring, packet);
    // The traffic thread calls the reporting process
    // directly forr non-threaded operation
    // These defeats the puropse of separating
    // traffic i/o from user i/o and really
    // should be avoided.
  #ifdef HAVE_THREAD
    // bypass the reporter thread here for single UDP
    if (isSingleUDP(stats->common))
        reporter_process_transfer_report(data);
  #else
    /*
     * Process the report in this thread
     */
    reporter_process_transfer_report(data);
  #endif
    return rc;
}
#else
void ReportPacket (struct ReporterData* data, struct ReportStruct *packet) {
    assert(data != NULL);
    struct TransferInfo *stats = &data->info;
  #ifdef HAVE_THREAD_DEBUG
    if (packet->packetID < 0) {
	thread_debug("Reporting last packet for %p  qdepth=%d sock=%d", (void *) data, packetring_getcount(data->packetring), data->info.common->socket);
    }
  #endif
    // Note for threaded operation all that needs
    // to be done is to enqueue the packet data
    // into the ring.
    packetring_enqueue(data->packetring, packet);
    // The traffic thread calls the reporting process
    // directly forr non-threaded operation
    // These defeats the puropse of separating
    // traffic i/o from user i/o and really
    // should be avoided.
  #ifdef HAVE_THREAD
    // bypass the reporter thread here for single UDP
    if (isSingleUDP(stats->common))
        reporter_process_transfer_report(data);
  #else
    /*
     * Process the report in this thread
     */
    reporter_process_transfer_report(data);
  #endif
}
#endif

/*
 * EndJob is called by a traffic thread to inform the reporter
 * thread to print a final report and to remove the data report from its jobq.
 * It also handles the freeing reports and other closing actions
 */
int EndJob (struct ReportHeader *reporthdr, struct ReportStruct *finalpacket) {
    assert(reporthdr!=NULL);
    assert(finalpacket!=NULL);
    struct ReporterData *report = (struct ReporterData *) reporthdr->this_report;
    struct ReportStruct packet;
#if defined(HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS) | defined(HAVE_THREAD_DEBUG)
    struct TransferInfo *stats = &report->info;
#endif
    memset(&packet, 0, sizeof(struct ReportStruct));
    int do_close = 1;
    /*
     * Using PacketID of -1 ends reporting
     * It pushes a "special packet" through
     * the packet ring which will be detected
     * by the reporter thread as and end of traffic
     * event
     */
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    // tcpi stats are sampled on a final packet
    if (stats->common->enable_sampleTCPstats) {
	sample_tcpistats(report, &packet, NULL);
    }
#endif
    // clear the reporter done predicate
    report->packetring->consumerdone = 0;
    // the negative packetID is used to inform the report thread this traffic thread is done
    packet.packetID = -1;
    packet.packetLen = finalpacket->packetLen;
    packet.packetTime = finalpacket->packetTime;
    if (isSingleUDP(report->info.common)) {
	packetring_enqueue(report->packetring, &packet);
	reporter_process_transfer_report(report);
    } else {
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	ReportPacket(report, &packet, NULL);
#else
	ReportPacket(report, &packet);
#endif
#ifdef HAVE_THREAD_DEBUG
	thread_debug( "Traffic thread awaiting reporter to be done with %p and cond %p", (void *)report, (void *) report->packetring->awake_producer);
#endif
	Condition_Lock((*(report->packetring->awake_producer)));
	while (!report->packetring->consumerdone) {
	    // This wait time is the lag between the reporter thread
	    // and the traffic thread, a reporter thread with lots of
	    // reports (e.g. fastsampling) can lag per the i/o
	    Condition_TimedWait(report->packetring->awake_producer, 1);
	    // printf("Consumer done may be stuck\n");
	}
	Condition_Unlock((*(report->packetring->awake_producer)));
    }
    if (report->FullDuplexReport && isFullDuplex(report->FullDuplexReport->info.common)) {
	if (fullduplex_stop_barrier(&report->FullDuplexReport->fullduplex_barrier)) {
	    struct Condition *tmp = &report->FullDuplexReport->fullduplex_barrier.await;
	    Condition_Destroy(tmp);
#if HAVE_THREAD_DEBUG
	    thread_debug("Socket fullduplex close sock=%d", stats->common->socket);
#endif
	    FreeSumReport(report->FullDuplexReport);
	} else {
	    do_close = 0;
	}
    }
    return do_close;
}

//  This is used to determine the packet/cpu load into the reporter thread
//  If the overall reporter load is too low, add some yield
//  or delay so the traffic threads can fill the packet rings
#define MINPACKETDEPTH 10
#define MINPERQUEUEDEPTH 20
#define REPORTERDELAY_DURATION 16000 // units is microseconds
struct ConsumptionDetectorType {
    int accounted_packets;
    int accounted_packet_threads;
    int reporter_thread_suspends ;
};
struct ConsumptionDetectorType consumption_detector = \
  {.accounted_packets = 0, .accounted_packet_threads = 0, .reporter_thread_suspends = 0};

static inline void reset_consumption_detector (void) {
    consumption_detector.accounted_packet_threads = thread_numtrafficthreads();
    if ((consumption_detector.accounted_packets = thread_numtrafficthreads() * MINPERQUEUEDEPTH) <= MINPACKETDEPTH) {
	consumption_detector.accounted_packets = MINPACKETDEPTH;
    }
}
static inline void apply_consumption_detector (void) {
    if (--consumption_detector.accounted_packet_threads <= 0) {
	// All active threads have been processed for the loop,
	// reset the thread counter and check the consumption rate
	// If the rate is too low add some delay to the reporter
	consumption_detector.accounted_packet_threads = thread_numtrafficthreads();
	// Check to see if we need to suspend the reporter
	if (consumption_detector.accounted_packets > 0) {
	    /*
	     * Suspend the reporter thread for some (e.g. 4) milliseconds
	     *
	     * This allows the thread to receive client or server threads'
	     * packet events in "aggregates."  This can reduce context
	     * switching allowing for better CPU utilization,
	     * which is very noticble on CPU constrained systems.
	     */
	    delay_loop(REPORTERDELAY_DURATION);
	    consumption_detector.reporter_thread_suspends++;
	    // printf("DEBUG: forced reporter suspend, accounted=%d,  queueue depth after = %d\n", accounted_packets, getcount_packetring(reporthdr));
	} else {
	    // printf("DEBUG: no suspend, accounted=%d,  queueue depth after = %d\n", accounted_packets, getcount_packetring(reporthdr));
	}
	reset_consumption_detector();
    }
}

#ifdef HAVE_THREAD_DEBUG
static void reporter_jobq_dump(void) {
  thread_debug("reporter thread job queue request lock");
  Condition_Lock(ReportCond);
  struct ReportHeader *itr = ReportRoot;
  while (itr) {
    thread_debug("Job in queue %p",(void *) itr);
    itr = itr->next;
  }
  Condition_Unlock(ReportCond);
  thread_debug("reporter thread job queue unlock");
}
#endif


/* Concatenate pending reports and return the head */
static inline struct ReportHeader *reporter_jobq_set_root (struct thread_Settings *inSettings) {
    struct ReportHeader *root = NULL;
    Condition_Lock(ReportCond);
    // check the jobq for empty
    if (ReportRoot == NULL) {
	// The reporter is starting from an empty state
	// so set the load detect to trigger an initial delay
        if (!isSingleUDP(inSettings)) {
	    reset_consumption_detector();
	    reporter_default_heading_flags((inSettings->mReportMode == kReport_CSV));
        }
	// Only hang the timed wait if more than this thread is active
	if (!ReportPendingHead && (thread_numuserthreads() > 1)) {
	    Condition_TimedWait(&ReportCond, 1);
#ifdef HAVE_THREAD_DEBUG
	    thread_debug( "Jobq *WAIT* exit  %p/%p cond=%p threads u/t=%d/%d", \
			  (void *) ReportRoot, (void *) ReportPendingHead, \
			  (void *) &ReportCond, thread_numuserthreads(), thread_numtrafficthreads());
#endif
	}
    }
    // update the jobq per pending reports
    if (ReportPendingHead) {
	ReportPendingTail->next = ReportRoot;
	ReportRoot = ReportPendingHead;
#ifdef HAVE_THREAD_DEBUG
	thread_debug( "Jobq *ROOT* %p (last=%p)", \
		      (void *) ReportRoot, (void * ) ReportPendingTail->next);
#endif
	ReportPendingHead = NULL;
	ReportPendingTail = NULL;
    }
    root = ReportRoot;
    Condition_Unlock(ReportCond);
    return root;
}

static void reporter_update_connect_time (double connect_time) {
    assert(myConnectionReport != NULL);
    if (connect_time > 0.0) {
	myConnectionReport->connect_times.sum += connect_time;
	if ((myConnectionReport->connect_times.cnt++) == 1) {
	    myConnectionReport->connect_times.vd = connect_time;
	    myConnectionReport->connect_times.mean = connect_time;
	    myConnectionReport->connect_times.m2 = connect_time * connect_time;
	} else {
	    myConnectionReport->connect_times.vd = connect_time - myConnectionReport->connect_times.mean;
	    myConnectionReport->connect_times.mean = myConnectionReport->connect_times.mean + (myConnectionReport->connect_times.vd / myConnectionReport->connect_times.cnt);
	    myConnectionReport->connect_times.m2 = myConnectionReport->connect_times.m2 + (myConnectionReport->connect_times.vd * (connect_time - myConnectionReport->connect_times.mean));
	}
	// mean min max tests
	if (connect_time < myConnectionReport->connect_times.min)
	    myConnectionReport->connect_times.min = connect_time;
	if (connect_time > myConnectionReport->connect_times.max)
	    myConnectionReport->connect_times.max = connect_time;
    } else {
	myConnectionReport->connect_times.err++;
    }
}

/*
 * This function is the loop that the reporter thread processes
 */
void reporter_spawn (struct thread_Settings *thread) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Reporter thread started");
#endif
    myConnectionReport = InitConnectOnlyReport(thread);
    /*
     * reporter main loop needs to wait on all threads being started
     */
    Condition_Lock(threads_start.await);
    while (!threads_start.ready) {
	Condition_TimedWait(&threads_start.await, 1);
    }
    Condition_Unlock(threads_start.await);
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Reporter await done");
#endif

    //
    // Signal to other (client) threads that the
    // reporter is now running.
    //
    Condition_Lock(reporter_state.await);
    reporter_state.ready = 1;
    Condition_Unlock(reporter_state.await);
    Condition_Broadcast(&reporter_state.await);
#if HAVE_SCHED_SETSCHEDULER
    // set reporter thread to realtime if requested
    thread_setscheduler(thread);
#endif
    /*
     * Keep the reporter thread alive under the following conditions
     *
     * o) There are more reports to output, ReportRoot has a report
     * o) The number of threads is greater than one which indicates
     *    either traffic threads are still running or a Listener thread
     *    is running. If equal to 1 then only the reporter thread is alive
     */
    while ((reporter_jobq_set_root(thread) != NULL) || (thread_numuserthreads() > 1)){
#ifdef HAVE_THREAD_DEBUG
	// thread_debug( "Jobq *HEAD* %p (%d)", (void *) ReportRoot, thread_numuserthreads());
#endif
	if (ReportRoot) {
	    // https://blog.kloetzl.info/beautiful-code/
	    // Linked list removal/processing is derived from:
	    //
	    // remove_list_entry(entry) {
	    //     indirect = &head;
	    //     while ((*indirect) != entry) {
	    //	       indirect = &(*indirect)->next;
	    //     }
	    //     *indirect = entry->next
	    // }
	    struct ReportHeader **work_item = &ReportRoot;
	    while (*work_item) {
#ifdef HAVE_THREAD_DEBUG
		// thread_debug( "Jobq *NEXT* %p", (void *) *work_item);
#endif
		// Report process report returns true
		// when a report needs to be removed
		// from the jobq.  Also, work item might
		// be removed as part of processing
		// Store a cached pointer for the linked list maitenance
		struct ReportHeader *tmp = (*work_item)->next;
	        if (reporter_process_report(*work_item)) {
#ifdef HAVE_THREAD_DEBUG
		  thread_debug("Jobq *REMOVE* %p", (void *) (*work_item));
#endif
		    // memory for *work_item is gone by now
		    *work_item = tmp;
		    if (!tmp)
			break;
		}
		work_item = &(*work_item)->next;
	    }
	}
    }
    if (myConnectionReport) {
	if (myConnectionReport->connect_times.cnt > 1) {
	    reporter_connect_printf_tcp_final(myConnectionReport);
	}
	FreeConnectionReport(myConnectionReport);
    }
#ifdef HAVE_THREAD_DEBUG
    if (sInterupted)
        reporter_jobq_dump();
    thread_debug("Reporter thread finished user/traffic %d/%d", thread_numuserthreads(), thread_numtrafficthreads());
#endif
}

// The Transfer or Data report is by far the most complicated report
int reporter_process_transfer_report (struct ReporterData *this_ireport) {
    assert(this_ireport != NULL);
    struct TransferInfo *sumstats = (this_ireport->GroupSumReport ? &this_ireport->GroupSumReport->info : NULL);
    struct TransferInfo *fullduplexstats = (this_ireport->FullDuplexReport ? &this_ireport->FullDuplexReport->info : NULL);
    int need_free = 0;
    // The consumption detector applies delay to the reporter
    // thread when its consumption rate is too low.   This allows
    // the traffic threads to send aggregates vs thrash
    // the packet rings.  The dissimilarity between the thread
    // speeds is due to the performance differences between i/o
    // bound threads vs cpu bound ones, and it's expected
    // that reporter thread being CPU limited should be much
    // faster than the traffic threads, even in aggregate.
    // Note: If this detection is not going off it means
    // the system is likely CPU bound and iperf is now likely
    // becoming a CPU bound test vs a network i/o bound test
    if (!isSingleUDP(this_ireport->info.common))
	apply_consumption_detector();
    // If there are more packets to process then handle them
    struct ReportStruct *packet = NULL;
    int advance_jobq = 0;
    while (!advance_jobq && (packet = packetring_dequeue(this_ireport->packetring))) {
	// Increment the total packet count processed by this thread
	// this will be used to make decisions on if the reporter
	// thread should add some delay to eliminate cpu thread
	// thrashing,
	consumption_detector.accounted_packets--;
	// Check against a final packet event on this packet ring
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	if (this_ireport->info.common->enable_sampleTCPstats && packet->tcpistat_valid) {
	    reporter_handle_packet_tcpistats(this_ireport, packet);
	}
#endif
	if (!(packet->packetID < 0)) {
	    // Check to output any interval reports,
            // bursts need to report the packet first
	    if (this_ireport->packet_handler_pre_report) {
		(*this_ireport->packet_handler_pre_report)(this_ireport, packet);
	    }
	    if (this_ireport->transfer_interval_handler) {
		advance_jobq = (*this_ireport->transfer_interval_handler)(this_ireport, packet);
	    }
	    if (this_ireport->packet_handler_post_report) {
		(*this_ireport->packet_handler_post_report)(this_ireport, packet);
	    }
	    // Sum reports update the report header's last
	    // packet time after the handler. This means
	    // the report header's packet time will be
	    // the previous time before the interval
	    if (sumstats)
		sumstats->ts.packetTime = packet->packetTime;
	    if (fullduplexstats)
		fullduplexstats->ts.packetTime = packet->packetTime;
	} else {
	    need_free = 1;
	    advance_jobq = 1;
	    // A last packet event was detected
	    // printf("last packet event detected\n"); fflush(stdout);
	    this_ireport->reporter_thread_suspends = consumption_detector.reporter_thread_suspends;
	    if (this_ireport->packet_handler_pre_report) {
		(*this_ireport->packet_handler_pre_report)(this_ireport, packet);
	    }
	    if (this_ireport->packet_handler_post_report) {
		(*this_ireport->packet_handler_post_report)(this_ireport, packet);
	    }
	    this_ireport->info.ts.packetTime = packet->packetTime;
	    assert(this_ireport->transfer_protocol_handler != NULL);
	    (*this_ireport->transfer_protocol_handler)(this_ireport, 1);
	    // This is a final report so set the sum report header's packet time
	    // Note, the thread with the max value will set this
	    if (fullduplexstats && isEnhanced(this_ireport->info.common)) {
		// The largest packet timestamp sets the sum report final time
		if (TimeDifference(fullduplexstats->ts.packetTime, packet->packetTime) > 0) {
		    fullduplexstats->ts.packetTime = packet->packetTime;
		}
		if (DecrSumReportRefCounter(this_ireport->FullDuplexReport) == 0) {
		    if (this_ireport->FullDuplexReport->transfer_protocol_sum_handler) {
			(*this_ireport->FullDuplexReport->transfer_protocol_sum_handler)(fullduplexstats, 1);
		    }
		    // FullDuplex report gets freed by a traffic thread (per its barrier)
		}
	    }
	    if (sumstats) {
		if (TimeDifference(sumstats->ts.packetTime, packet->packetTime) > 0) {
		    sumstats->ts.packetTime = packet->packetTime;
		}
		if (DecrSumReportRefCounter(this_ireport->GroupSumReport) == 0) {
		    if (this_ireport->GroupSumReport->transfer_protocol_sum_handler && \
			((this_ireport->GroupSumReport->reference.maxcount > 1) || isSumOnly(this_ireport->info.common))) {
			(*this_ireport->GroupSumReport->transfer_protocol_sum_handler)(&this_ireport->GroupSumReport->info, 1);
		    }
		    FreeSumReport(this_ireport->GroupSumReport);
		}
	    }
	}
    }
    return need_free;
}
/*
 * Process reports
 *
 * Make notice here, the reporter thread is freeing most reports, traffic threads
 * can't use them anymore (except for the DATA REPORT);
 *
 */
inline int reporter_process_report (struct ReportHeader *reporthdr) {
    assert(reporthdr != NULL);
    int done = 1;
    switch (reporthdr->type) {
    case DATA_REPORT:
	done = reporter_process_transfer_report((struct ReporterData *)reporthdr->this_report);
	fflush(stdout);
	if (done) {
	    struct ReporterData *tmp = (struct ReporterData *)reporthdr->this_report;
	    struct PacketRing *pr = tmp->packetring;
	    pr->consumerdone = 1;
	    // Data Reports are special because the traffic thread needs to free them, just signal
	    Condition_Signal(pr->awake_producer);
	}
	break;
    case CONNECTION_REPORT:
    {
	struct ConnectionInfo *creport = (struct ConnectionInfo *)reporthdr->this_report;
	assert(creport!=NULL);
	if (!isCompat(creport->common) && (creport->common->ThreadMode == kMode_Client)) {
	    // Clients' connect times will be inputs to the overall connect stats
	    reporter_update_connect_time(creport->connecttime);
	}
	reporter_print_connection_report(creport);
	fflush(stdout);
	FreeReport(reporthdr);
    }
	break;
    case SETTINGS_REPORT:
	reporter_print_settings_report((struct ReportSettings *)reporthdr->this_report);
	fflush(stdout);
	FreeReport(reporthdr);
	break;
    case SERVER_RELAY_REPORT:
	reporter_print_server_relay_report((struct ServerRelay *)reporthdr->this_report);
	fflush(stdout);
	FreeReport(reporthdr);
	break;
    default:
	fprintf(stderr,"Invalid report type in process report %p\n", reporthdr->this_report);
	assert(0);
	break;
    }
#ifdef HAVE_THREAD_DEBUG
    // thread_debug("Processed report %p type=%d", (void *)reporthdr, reporthdr->report.type);
#endif
    return done;
}

/*
 * Updates connection stats
 */
#define L2DROPFILTERCOUNTER 100

// Reporter private routines
void reporter_handle_packet_null (struct ReporterData *data, struct ReportStruct *packet) {
}
void reporter_transfer_protocol_null (struct ReporterData *data, int final){
}

inline void reporter_handle_packet_pps (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    if (!packet->emptyreport) {
        stats->total.Datagrams.current++;
        stats->total.IPG.current++;
    }
    stats->ts.IPGstart = packet->packetTime;
    stats->IPGsum += TimeDifference(packet->packetTime, packet->prevPacketTime);
#ifdef DEBUG_PPS
    printf("*** IPGsum = %f cnt=%ld ipg=%ld.%ld pkt=%ld.%ld id=%ld empty=%d transit=%f prev=%ld.%ld\n", stats->IPGsum, stats->cntIPG, stats->ts.IPGstart.tv_sec, stats->ts.IPGstart.tv_usec, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->packetID, packet->emptyreport, TimeDifference(packet->packetTime, packet->prevPacketTime), packet->prevPacketTime.tv_sec, packet->prevPacketTime.tv_usec);
#endif
}

// Variance uses the Welford inline algorithm, mean is also inline
static inline double reporter_handle_packet_oneway_transit (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    // Transit or latency updates done inline below
    double transit = TimeDifference(packet->packetTime, packet->sentTime);
    double usec_transit = transit * 1e6;

    if (stats->latency_histogram) {
        histogram_insert(stats->latency_histogram, transit, NULL);
    }

    if (stats->transit.totcntTransit == 0) {
	// Very first packet
	stats->transit.minTransit = transit;
	stats->transit.maxTransit = transit;
	stats->transit.sumTransit = transit;
	stats->transit.cntTransit = 1;
	stats->transit.totminTransit = transit;
	stats->transit.totmaxTransit = transit;
	stats->transit.totsumTransit = transit;
	stats->transit.totcntTransit = 1;
	// For variance, working units is microseconds
	stats->transit.vdTransit = usec_transit;
	stats->transit.meanTransit = usec_transit;
	stats->transit.m2Transit = usec_transit * usec_transit;
	stats->transit.totvdTransit = usec_transit;
	stats->transit.totmeanTransit = usec_transit;
	stats->transit.totm2Transit = usec_transit * usec_transit;
    } else {
	double deltaTransit;
	// from RFC 1889, Real Time Protocol (RTP)
	// J = J + ( | D(i-1,i) | - J ) /
	// Compute jitter
	deltaTransit = transit - stats->transit.lastTransit;
	if (deltaTransit < 0.0) {
	    deltaTransit = -deltaTransit;
	}
	stats->jitter += (deltaTransit - stats->jitter) / (16.0);
	// Compute end/end delay stats
	stats->transit.sumTransit += transit;
	stats->transit.cntTransit++;
	stats->transit.totsumTransit += transit;
	stats->transit.totcntTransit++;
	// mean min max tests
	if (transit < stats->transit.minTransit) {
	    stats->transit.minTransit=transit;
	}
	if (transit < stats->transit.totminTransit) {
	    stats->transit.totminTransit=transit;
	}
	if (transit > stats->transit.maxTransit) {
	    stats->transit.maxTransit=transit;
	}
	if (transit > stats->transit.totmaxTransit) {
	    stats->transit.totmaxTransit=transit;
	}
	// For variance, working units is microseconds
	// variance interval
	stats->transit.vdTransit = usec_transit - stats->transit.meanTransit;
	stats->transit.meanTransit = stats->transit.meanTransit + (stats->transit.vdTransit / stats->transit.cntTransit);
	stats->transit.m2Transit = stats->transit.m2Transit + (stats->transit.vdTransit * (usec_transit - stats->transit.meanTransit));
	// variance total
	stats->transit.totvdTransit = usec_transit - stats->transit.totmeanTransit;
	stats->transit.totmeanTransit = stats->transit.totmeanTransit + (stats->transit.totvdTransit / stats->transit.totcntTransit);
	stats->transit.totm2Transit = stats->transit.totm2Transit + (stats->transit.totvdTransit * (usec_transit - stats->transit.totmeanTransit));
    }
    stats->transit.lastTransit = transit;
    return (transit);
}

static inline void reporter_handle_burst_tcp_server_transit (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    // very first burst
    if (!stats->isochstats.frameID) {
	stats->isochstats.frameID = packet->frameID;
    }
    if (packet->frameID && packet->transit_ready) {
        double transit = reporter_handle_packet_oneway_transit(data, packet);
	if (!TimeZero(stats->ts.prevpacketTime)) {
	    double delta = TimeDifference(packet->sentTime, stats->ts.prevpacketTime);
	    stats->IPGsum += delta;
	}
	stats->ts.prevpacketTime = packet->sentTime;
	if (stats->framelatency_histogram) {
	    histogram_insert(stats->framelatency_histogram, transit, isTripTime(stats->common) ? &packet->sentTime : NULL);
	}
	stats->isochstats.frameID++;  // RJM fix this overload
	stats->burstid_transition = true;
	// printf("***Burst id = %ld, transit = %f\n", packet->frameID, stats->transit.lastTransit);
    } else if (stats->burstid_transition && packet->frameID && (packet->frameID != stats->isochstats.frameID)) {
	stats->burstid_transition = false;
	fprintf(stderr,"%sError: expected burst id %u but got %" PRIdMAX "\n", \
		stats->common->transferIDStr, stats->isochstats.frameID + 1, packet->frameID);
	stats->isochstats.frameID = packet->frameID;
    }
}

static inline void reporter_handle_burst_tcp_client_transit (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    // very first burst
    if (!stats->isochstats.frameID) {
	stats->isochstats.frameID = packet->frameID;
    }
    if (stats->burstid_transition && packet->frameID && packet->transit_ready) {
	stats->burstid_transition = false;
	// printf("***Burst id = %ld, transit = %f\n", packet->frameID, stats->transit.lastTransit);
    } else if (!stats->burstid_transition) {
	stats->burstid_transition = true;
	if (packet->frameID && (packet->frameID != (stats->isochstats.frameID + 1))) {
	    fprintf(stderr,"%sError: expected burst id %u but got %" PRIdMAX "\n", \
		    stats->common->transferIDStr, stats->isochstats.frameID + 1, packet->frameID);
	}
	stats->isochstats.frameID = packet->frameID;
    }
}

inline void reporter_handle_packet_isochronous (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    // printf("fid=%lu bs=%lu remain=%lu\n", packet->frameID, packet->burstsize, packet->remaining);
    if (packet->frameID && packet->transit_ready) {
	int framedelta=0;
	// very first isochronous frame
	if (!stats->isochstats.frameID) {
	    stats->isochstats.framecnt.current=packet->frameID;
	}
	// perform client and server frame based accounting
	if ((framedelta = (packet->frameID - stats->isochstats.frameID))) {
	    stats->isochstats.framecnt.current++;
	    if (framedelta > 1) {
		if (stats->common->ThreadMode == kMode_Server) {
		    int lost = framedelta - (packet->frameID - packet->prevframeID);
		    stats->isochstats.framelostcnt.current += lost;
		} else {
		    stats->isochstats.framelostcnt.current += (framedelta-1);
		    stats->isochstats.slipcnt.current++;
		}
	    }
	}
	// peform frame latency checks
	if (stats->framelatency_histogram) {
	    // first packet of a burst and not a duplicate
	    if ((packet->burstsize == packet->remaining) && (stats->matchframeID!=packet->frameID)) {
		stats->matchframeID=packet->frameID;
	    }
	    if ((packet->packetLen == packet->remaining) && (packet->frameID == stats->matchframeID)) {
		// last packet of a burst (or first-last in case of a duplicate) and frame id match
		double frametransit = TimeDifference(packet->packetTime, packet->isochStartTime) \
		    - ((packet->burstperiod * (packet->frameID - 1)) / 1000000.0);
		histogram_insert(stats->framelatency_histogram, frametransit, NULL);
		stats->matchframeID = 0;  // reset the matchid so any potential duplicate is ignored
	    }
	}
	stats->isochstats.frameID = packet->frameID;
    }
}

inline void reporter_handle_packet_server_tcp (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    if (packet->packetLen > 0) {
	int bin;
	stats->total.Bytes.current += packet->packetLen;
	// mean min max tests
	stats->sock_callstats.read.cntRead++;
	stats->sock_callstats.read.totcntRead++;
	bin = (int)floor((packet->packetLen -1)/stats->sock_callstats.read.binsize);
	if (bin < TCPREADBINCOUNT) {
	    stats->sock_callstats.read.bins[bin]++;
	    stats->sock_callstats.read.totbins[bin]++;
	}
	if (isPeriodicBurst(stats->common) || isTripTime(stats->common))
	    reporter_handle_burst_tcp_server_transit(data, packet);
    }
}

inline void reporter_handle_packet_server_udp (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    stats->ts.packetTime = packet->packetTime;
    if (packet->emptyreport && (stats->transit.cntTransit == 0)) {
	// This is the case when empty reports
	// cross the report interval boundary
	// Hence, set the per interval min to infinity
	// and the per interval max and sum to zero
	stats->transit.minTransit = FLT_MAX;
	stats->transit.maxTransit = FLT_MIN;
	stats->transit.sumTransit = 0;
	stats->transit.vdTransit = 0;
	stats->transit.meanTransit = 0;
	stats->transit.m2Transit = 0;
    } else if (packet->packetID > 0) {
	stats->total.Bytes.current += packet->packetLen;
	// These are valid packets that need standard iperf accounting
	// Do L2 accounting first (if needed)
	if (packet->l2errors && (stats->total.Datagrams.current > L2DROPFILTERCOUNTER)) {
	    stats->l2counts.cnt++;
	    stats->l2counts.tot_cnt++;
	    if (packet->l2errors & L2UNKNOWN) {
		stats->l2counts.unknown++;
		stats->l2counts.tot_unknown++;
	    }
	    if (packet->l2errors & L2LENERR) {
		stats->l2counts.lengtherr++;
		stats->l2counts.tot_lengtherr++;
	    }
	    if (packet->l2errors & L2CSUMERR) {
		stats->l2counts.udpcsumerr++;
		stats->l2counts.tot_udpcsumerr++;
	    }
	}
	// packet loss occured if the datagram numbers aren't sequential
	if (packet->packetID != stats->PacketID + 1) {
	    if (packet->packetID < stats->PacketID + 1) {
		stats->total.OutofOrder.current++;
	    } else {
		stats->total.Lost.current += packet->packetID - stats->PacketID - 1;
	    }
	}
	// never decrease datagramID (e.g. if we get an out-of-order packet)
	if (packet->packetID > stats->PacketID) {
	    stats->PacketID = packet->packetID;
	}
	reporter_handle_packet_pps(data, packet);
	reporter_handle_packet_oneway_transit(data, packet);
	reporter_handle_packet_isochronous(data, packet);
    }
}

#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
// This is done in traffic thread context
static inline bool sample_tcpistats (struct ReporterData *data, struct ReportStruct *sample, struct tcp_info *tcp_stats) {
    assert(sample);
    struct tcp_info tcp_info_buf;
    struct tcp_info *this_tcp_stats = (tcp_stats != NULL) ? tcp_stats : &tcp_info_buf;
    socklen_t tcp_info_length = sizeof(struct tcp_info);
    if ((data->info.common->socket > 0) &&				\
	!(getsockopt(data->info.common->socket, IPPROTO_TCP, TCP_INFO, this_tcp_stats, &tcp_info_length) < 0)) {
        sample->cwnd = this_tcp_stats->tcpi_snd_cwnd * this_tcp_stats->tcpi_snd_mss / 1024;
	sample->rtt = this_tcp_stats->tcpi_rtt;
	sample->retry_tot = this_tcp_stats->tcpi_total_retrans;
	sample->tcpistat_valid  = true;
    } else {
        sample->cwnd = -1;
	sample->rtt = 0;
	sample->retry_tot = 0;
	sample->tcpistat_valid  = false;
    }
    return sample->tcpistat_valid;
}

// This is done in reporter thread context
static inline void reporter_handle_packet_tcpistats (struct ReporterData *data, struct ReportStruct *packet) {
    assert(data!=NULL);
    struct TransferInfo *stats = &data->info;

    stats->sock_callstats.write.TCPretry += (packet->retry_tot - stats->sock_callstats.write.totTCPretry);
    stats->sock_callstats.write.totTCPretry = packet->retry_tot;
    stats->sock_callstats.write.cwnd = packet->cwnd;
    stats->sock_callstats.write.rtt = packet->rtt;
}
#endif

void reporter_handle_packet_client (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    stats->ts.packetTime = packet->packetTime;
    if (!packet->emptyreport) {
	stats->total.Bytes.current += packet->packetLen;
        if (packet->errwrite && (packet->errwrite != WriteErrNoAccount)) {
	    stats->sock_callstats.write.WriteErr++;
	    stats->sock_callstats.write.totWriteErr++;
	}
	// These are valid packets that need standard iperf accounting
	stats->sock_callstats.write.WriteCnt++;
	stats->sock_callstats.write.totWriteCnt++;
	if (isIsochronous(stats->common)) {
	    reporter_handle_packet_isochronous(data, packet);
	} else if (isPeriodicBurst(stats->common)) {
	    reporter_handle_burst_tcp_client_transit(data, packet);
	}
#if HAVE_DECL_TCP_NOTSENT_LOWAT
	if (stats->latency_histogram && (packet->select_delay > 0.0)) {
	   histogram_insert(stats->latency_histogram, packet->select_delay, &packet->packetTime);
       }
#endif
    }
    if (isUDP(stats->common)) {
	stats->PacketID = packet->packetID;
	reporter_handle_packet_pps(data, packet);
    }
}

/*
 * Report printing routines below
 */
static inline void reporter_set_timestamps_time (struct ReportTimeStamps *times, enum TimeStampType tstype) {
    // There is a corner case when the first packet is also the last where the start time (which comes
    // from app level syscall) is greater than the packetTime (which come for kernel level SO_TIMESTAMP)
    // For this case set the start and end time to both zero.
    if (TimeDifference(times->packetTime, times->startTime) < 0) {
	times->iEnd = 0;
	times->iStart = 0;
    } else {
	switch (tstype) {
	case INTERVAL:
	    times->iStart = times->iEnd;
	    times->iEnd = TimeDifference(times->nextTime, times->startTime);
	    TimeAdd(times->nextTime, times->intervalTime);
	    break;
	case TOTAL:
	    times->iStart = 0;
	    times->iEnd = TimeDifference(times->packetTime, times->startTime);
	    break;
	case FINALPARTIAL:
	    times->iStart = times->iEnd;
	    times->iEnd = TimeDifference(times->packetTime, times->startTime);
	    break;
	case FRAME:
	    if ((times->iStart = TimeDifference(times->prevpacketTime, times->startTime)) < 0)
		times->iStart = 0.0;
	    times->iEnd = TimeDifference(times->packetTime, times->startTime);
	    break;
	default:
	    times->iEnd = -1;
	    times->iStart = -1;
	    break;
	}
    }
}

// If reports were missed, catch up now
static inline void reporter_transfer_protocol_missed_reports (struct TransferInfo *stats, struct ReportStruct *packet) {
    while (TimeDifference(packet->packetTime, stats->ts.nextTime) > TimeDouble(stats->ts.intervalTime)) {
//	printf("**** cmp=%f/%f next %ld.%ld packet %ld.%ld id=%ld\n", TimeDifference(packet->packetTime, stats->ts.nextTime), TimeDouble(stats->ts.intervalTime), stats->ts.nextTime.tv_sec, stats->ts.nextTime.tv_usec, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->packetID);
	reporter_set_timestamps_time(&stats->ts, INTERVAL);
	struct TransferInfo emptystats;
	memset(&emptystats, 0, sizeof(struct TransferInfo));
	emptystats.ts.iStart = stats->ts.iStart;
	emptystats.ts.iEnd = stats->ts.iEnd;
	emptystats.common = stats->common;
	if ((stats->output_handler) && !(stats->filter_this_sample_output))
	    (*stats->output_handler)(&emptystats);
    }
}

static inline void reporter_reset_transfer_stats_client_tcp (struct TransferInfo *stats) {
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->sock_callstats.write.WriteCnt = 0;
    stats->sock_callstats.write.WriteErr = 0;
    stats->isochstats.framecnt.prev = stats->isochstats.framecnt.current;
    stats->isochstats.framelostcnt.prev = stats->isochstats.framelostcnt.current;
    stats->isochstats.slipcnt.prev = stats->isochstats.slipcnt.current;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    stats->sock_callstats.write.TCPretry = 0;
#endif
}

static inline void reporter_reset_transfer_stats_client_udp (struct TransferInfo *stats) {
    if (stats->cntError < 0) {
	stats->cntError = 0;
    }
    stats->total.Lost.prev = stats->total.Lost.current;
    stats->total.Datagrams.prev = stats->total.Datagrams.current;
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->total.IPG.prev = stats->total.IPG.current;
    stats->sock_callstats.write.WriteCnt = 0;
    stats->sock_callstats.write.WriteErr = 0;
    stats->isochstats.framecnt.prev = stats->isochstats.framecnt.current;
    stats->isochstats.framelostcnt.prev = stats->isochstats.framelostcnt.current;
    stats->isochstats.slipcnt.prev = stats->isochstats.slipcnt.current;
    if (stats->cntDatagrams)
	stats->IPGsum = 0;
}

static inline void reporter_reset_transfer_stats_server_tcp (struct TransferInfo *stats) {
    int ix;
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->sock_callstats.read.cntRead = 0;
    for (ix = 0; ix < 8; ix++) {
	stats->sock_callstats.read.bins[ix] = 0;
    }
    stats->transit.minTransit = FLT_MAX;
    stats->transit.maxTransit = FLT_MIN;
    stats->transit.sumTransit = 0;
    stats->transit.cntTransit = 0;
    stats->transit.vdTransit = 0;
    stats->transit.meanTransit = 0;
    stats->transit.m2Transit = 0;
    stats->IPGsum = 0;
}

static inline void reporter_reset_transfer_stats_server_udp (struct TransferInfo *stats) {
    // Reset the enhanced stats for the next report interval
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->total.Datagrams.prev = stats->PacketID;
    stats->total.OutofOrder.prev = stats->total.OutofOrder.current;
    stats->total.Lost.prev = stats->total.Lost.current;
    stats->total.IPG.prev = stats->total.IPG.current;
    stats->transit.minTransit = FLT_MAX;
    stats->transit.maxTransit = FLT_MIN;
    stats->transit.sumTransit = 0;
    stats->transit.cntTransit = 0;
    stats->transit.vdTransit = 0;
    stats->transit.meanTransit = 0;
    stats->transit.m2Transit = 0;
    stats->isochstats.framecnt.prev = stats->isochstats.framecnt.current;
    stats->isochstats.framelostcnt.prev = stats->isochstats.framelostcnt.current;
    stats->isochstats.slipcnt.prev = stats->isochstats.slipcnt.current;
    stats->l2counts.cnt = 0;
    stats->l2counts.unknown = 0;
    stats->l2counts.udpcsumerr = 0;
    stats->l2counts.lengtherr = 0;
    if (stats->cntDatagrams)
	stats->IPGsum = 0;
}

// These do the following
//
// o) set the TransferInfo struct and then calls the individual report output handler
// o) updates the sum and fullduplex reports
//
void reporter_transfer_protocol_server_udp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *fullduplexstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    // print a interval report and possibly a partial interval report if this a final
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
    stats->cntOutofOrder = stats->total.OutofOrder.current - stats->total.OutofOrder.prev;
    // assume most of the  time out-of-order packets are
    // duplicate packets, so conditionally subtract them from the lost packets.
    stats->cntError = stats->total.Lost.current - stats->total.Lost.prev - stats->cntOutofOrder;
    if (stats->cntError < 0)
	stats->cntError = 0;
    stats->cntDatagrams = stats->PacketID - stats->total.Datagrams.prev;
    stats->cntIPG = stats->total.IPG.current - stats->total.IPG.prev;
    if (stats->latency_histogram) {
        stats->latency_histogram->final = 1;
    }

    if (isIsochronous(stats->common)) {
	stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
	stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
	stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
	if (stats->framelatency_histogram) {
	    stats->framelatency_histogram->final = 1;
	}

    }
    if (stats->total.Datagrams.current == 1)
	stats->jitter = 0;
    if (sumstats) {
	sumstats->total.OutofOrder.current += stats->total.OutofOrder.current - stats->total.OutofOrder.prev;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	sumstats->total.Lost.current += stats->total.Lost.current - stats->total.Lost.prev;
	sumstats->total.Datagrams.current += stats->PacketID - stats->total.Datagrams.prev;
	sumstats->total.Bytes.current += stats->cntBytes;
	sumstats->total.IPG.current += stats->cntIPG;
	if (sumstats->IPGsum < stats->IPGsum)
	    sumstats->IPGsum = stats->IPGsum;
	sumstats->threadcnt++;
    }
    if (fullduplexstats) {
	fullduplexstats->total.Bytes.current += stats->cntBytes;
	fullduplexstats->total.IPG.current += stats->cntIPG;
	fullduplexstats->total.Datagrams.current += (stats->total.Datagrams.current - stats->total.Datagrams.prev);
	if (fullduplexstats->IPGsum < stats->IPGsum)
	    fullduplexstats->IPGsum = stats->IPGsum;
    }
    if (final) {
	if ((stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime)) {
	    stats->cntOutofOrder = stats->total.OutofOrder.current - stats->total.OutofOrder.prev;
	    // assume most of the  time out-of-order packets are not
	    // duplicate packets, so conditionally subtract them from the lost packets.
	    stats->cntError = stats->total.Lost.current - stats->total.Lost.prev;
	    stats->cntError -= stats->cntOutofOrder;
	    if (stats->cntError < 0)
		stats->cntError = 0;
	    stats->cntDatagrams = stats->PacketID - stats->total.Datagrams.prev;
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
	    }
	}
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	stats->final = true;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
	stats->cntOutofOrder = stats->total.OutofOrder.current;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->cntError = stats->total.Lost.current;
	stats->cntError -= stats->cntOutofOrder;
	if (stats->cntError < 0)
	    stats->cntError = 0;
	stats->cntDatagrams = stats->PacketID;
	stats->cntIPG = stats->total.IPG.current;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
	stats->cntBytes = stats->total.Bytes.current;
	stats->l2counts.cnt = stats->l2counts.tot_cnt;
	stats->l2counts.unknown = stats->l2counts.tot_unknown;
	stats->l2counts.udpcsumerr = stats->l2counts.tot_udpcsumerr;
	stats->l2counts.lengtherr = stats->l2counts.tot_lengtherr;
	stats->transit.minTransit = stats->transit.totminTransit;
        stats->transit.maxTransit = stats->transit.totmaxTransit;
	stats->transit.cntTransit = stats->transit.totcntTransit;
	stats->transit.sumTransit = stats->transit.totsumTransit;
	stats->transit.meanTransit = stats->transit.totmeanTransit;
	stats->transit.m2Transit = stats->transit.totm2Transit;
	stats->transit.vdTransit = stats->transit.totvdTransit;
	if (isIsochronous(stats->common)) {
	    stats->isochstats.cntFrames = stats->isochstats.framecnt.current;
	    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current;
	    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current;
	}
	if (stats->latency_histogram) {
	    stats->latency_histogram->final = 1;
	}
	if (stats->framelatency_histogram) {
	    stats->framelatency_histogram->final = 1;
	}
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output))
	(*stats->output_handler)(stats);
    if (!final)
	reporter_reset_transfer_stats_server_udp(stats);
}

void reporter_transfer_protocol_sum_server_udp (struct TransferInfo *stats, int final) {
    if (final) {
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	stats->cntOutofOrder = stats->total.OutofOrder.current;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->cntError = stats->total.Lost.current;
	stats->cntError -= stats->cntOutofOrder;
	if (stats->cntError < 0)
	    stats->cntError = 0;
	stats->cntDatagrams = stats->total.Datagrams.current;
	stats->cntBytes = stats->total.Bytes.current;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
	stats->cntIPG = stats->total.IPG.current;
    } else {
	stats->cntOutofOrder = stats->total.OutofOrder.current - stats->total.OutofOrder.prev;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->cntError = stats->total.Lost.current - stats->total.Lost.prev;
	stats->cntError -= stats->cntOutofOrder;
	if (stats->cntError < 0)
	    stats->cntError = 0;
	stats->cntDatagrams = stats->total.Datagrams.current - stats->total.Datagrams.prev;
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	stats->cntIPG = stats->total.IPG.current - stats->total.IPG.prev;
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output))
	(*stats->output_handler)(stats);
    if (!final) {
	stats->threadcnt = 0;
	// there is no packet ID for sum server reports, set it to total cnt for calculation
	stats->PacketID = stats->total.Datagrams.current;
	reporter_reset_transfer_stats_server_udp(stats);
    }
}
void reporter_transfer_protocol_sum_client_udp (struct TransferInfo *stats, int final) {
    if (final) {
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	stats->cntDatagrams = stats->total.Datagrams.current;
	stats->cntBytes = stats->total.Bytes.current;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
	stats->cntIPG = stats->total.IPG.current;
    } else {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	stats->cntIPG = stats->total.IPG.current - stats->total.IPG.prev;
	stats->cntDatagrams = stats->total.Datagrams.current - stats->total.Datagrams.prev;
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output))
	(*stats->output_handler)(stats);

    if (!final) {
	stats->threadcnt = 0;
	reporter_reset_transfer_stats_client_udp(stats);
    } else if ((stats->common->ReportMode != kReport_CSV) && !(stats->filter_this_sample_output)) {
	printf(report_sumcnt_datagrams, stats->threadcnt, stats->total.Datagrams.current);
	fflush(stdout);
    }
}

void reporter_transfer_protocol_client_udp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *fullduplexstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
    stats->cntDatagrams = stats->total.Datagrams.current - stats->total.Datagrams.prev;
    stats->cntIPG = stats->total.IPG.current - stats->total.IPG.prev;
    if (isIsochronous(stats->common)) {
	stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
	stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
	stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
    }
    if (sumstats) {
	sumstats->total.Bytes.current += stats->cntBytes;
	sumstats->sock_callstats.write.WriteErr += stats->sock_callstats.write.WriteErr;
	sumstats->sock_callstats.write.WriteCnt += stats->sock_callstats.write.WriteCnt;
	sumstats->sock_callstats.write.totWriteErr += stats->sock_callstats.write.WriteErr;
	sumstats->sock_callstats.write.totWriteCnt += stats->sock_callstats.write.WriteCnt;
	sumstats->total.Datagrams.current += stats->cntDatagrams;
	if (sumstats->IPGsum < stats->IPGsum)
	    sumstats->IPGsum = stats->IPGsum;
	sumstats->total.IPG.current += stats->cntIPG;
	sumstats->threadcnt++;
    }
    if (fullduplexstats) {
	fullduplexstats->total.Bytes.current += stats->cntBytes;
	fullduplexstats->total.IPG.current += stats->cntIPG;
	fullduplexstats->total.Datagrams.current += stats->cntDatagrams;
	if (fullduplexstats->IPGsum < stats->IPGsum)
	    fullduplexstats->IPGsum = stats->IPGsum;
    }
    if (final) {
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	stats->cntBytes = stats->total.Bytes.current;
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	stats->cntIPG = stats->total.IPG.current;
	stats->cntDatagrams = stats->PacketID;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
	if (isIsochronous(stats->common)) {
	    stats->isochstats.cntFrames = stats->isochstats.framecnt.current;
	    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current;
	    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current;
	}
    } else {
	if (stats->ts.iEnd > 0) {
	    stats->cntIPG = (stats->total.IPG.current - stats->total.IPG.prev);
	} else {
	    stats->cntIPG = 0;
	}
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
	(*stats->output_handler)(stats);
	if (final && (stats->common->ReportMode != kReport_CSV)) {
	    printf(report_datagrams, stats->common->transferID, stats->total.Datagrams.current);
	    fflush(stdout);
	}
    }
    reporter_reset_transfer_stats_client_udp(stats);
}

void reporter_transfer_protocol_server_tcp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *fullduplexstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
    int ix;
    if (stats->framelatency_histogram) {
        stats->framelatency_histogram->final = 0;
    }
    if (sumstats) {
	sumstats->threadcnt++;
	sumstats->total.Bytes.current += stats->cntBytes;
        sumstats->sock_callstats.read.cntRead += stats->sock_callstats.read.cntRead;
        sumstats->sock_callstats.read.totcntRead += stats->sock_callstats.read.cntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    sumstats->sock_callstats.read.bins[ix] += stats->sock_callstats.read.bins[ix];
	    sumstats->sock_callstats.read.totbins[ix] += stats->sock_callstats.read.bins[ix];
        }
    }
    if (fullduplexstats) {
	fullduplexstats->total.Bytes.current += stats->cntBytes;
    }
    if (final) {
        if (stats->framelatency_histogram) {
	    stats->framelatency_histogram->final = 1;
	}
	if ((stats->cntBytes > 0) && stats->output_handler && !TimeZero(stats->ts.intervalTime)) {
	    // print a partial interval report if enable and this a final
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		if (isIsochronous(stats->common)) {
		    stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
		    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
		    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
		}
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
		reporter_reset_transfer_stats_server_tcp(stats);
	    }
        }
	stats->final = true;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
        stats->cntBytes = stats->total.Bytes.current;
	stats->IPGsum = stats->ts.iEnd;
        stats->sock_callstats.read.cntRead = stats->sock_callstats.read.totcntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->sock_callstats.read.bins[ix] = stats->sock_callstats.read.totbins[ix];
        }
	if (isIsochronous(stats->common)) {
	    stats->isochstats.cntFrames = stats->isochstats.framecnt.current;
	    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current;
	    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current;
	}
	stats->transit.sumTransit = stats->transit.totsumTransit;
	stats->transit.cntTransit = stats->transit.totcntTransit;
	stats->transit.minTransit = stats->transit.totminTransit;
	stats->transit.maxTransit = stats->transit.totmaxTransit;
	stats->transit.m2Transit = stats->transit.totm2Transit;
	if (stats->framelatency_histogram) {
	    stats->framelatency_histogram->final = 1;
	}
    } else if (isIsochronous(stats->common)) {
	stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
	stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
	stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
    }
    if ((stats->output_handler) && !stats->filter_this_sample_output) {
	(*stats->output_handler)(stats);
	if (isFrameInterval(stats->common) && stats->framelatency_histogram) {
	    histogram_print(stats->framelatency_histogram, stats->ts.iStart, stats->ts.iEnd);
	}
    }
    if (!final)
	reporter_reset_transfer_stats_server_tcp(stats);
}

void reporter_transfer_protocol_client_tcp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *fullduplexstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
#if HAVE_DECL_TCP_NOTSENT_LOWAT
    if (stats->latency_histogram) {
        stats->latency_histogram->final = 0;
    }
#endif
    if (isIsochronous(stats->common)) {
	if (final) {
	    stats->isochstats.cntFrames = stats->isochstats.framecnt.current;
	    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current;
	    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current;
	} else {
	    stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
	    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
	    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
	}
    }
    if (sumstats) {
	sumstats->total.Bytes.current += stats->cntBytes;
	sumstats->sock_callstats.write.WriteErr += stats->sock_callstats.write.WriteErr;
	sumstats->sock_callstats.write.WriteCnt += stats->sock_callstats.write.WriteCnt;
	sumstats->sock_callstats.write.totWriteErr += stats->sock_callstats.write.WriteErr;
	sumstats->sock_callstats.write.totWriteCnt += stats->sock_callstats.write.WriteCnt;
	sumstats->threadcnt++;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	sumstats->sock_callstats.write.TCPretry += stats->sock_callstats.write.TCPretry;
	sumstats->sock_callstats.write.totTCPretry += stats->sock_callstats.write.TCPretry;
#endif
    }
    if (fullduplexstats) {
	fullduplexstats->total.Bytes.current += stats->cntBytes;
    }
    if (final) {
#if HAVE_DECL_TCP_NOTSENT_LOWAT
	if (stats->latency_histogram) {
	    stats->latency_histogram->final = 1;
	}
#endif
	if ((stats->cntBytes > 0) && stats->output_handler && !TimeZero(stats->ts.intervalTime)) {
	    // print a partial interval report if enable and this a final
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		if (isIsochronous(stats->common)) {
		    stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
		    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
		    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
		}
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
		reporter_reset_transfer_stats_client_tcp(stats);
	    }
        }
	if (isIsochronous(stats->common)) {
	    stats->isochstats.cntFrames = stats->isochstats.framecnt.current;
	    stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current;
	    stats->isochstats.cntSlips = stats->isochstats.slipcnt.current;
	}
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	stats->sock_callstats.write.TCPretry = stats->sock_callstats.write.totTCPretry;
#endif
	if (stats->framelatency_histogram) {
	    stats->framelatency_histogram->final = 1;
	}
	stats->cntBytes = stats->total.Bytes.current;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
    } else if (isIsochronous(stats->common)) {
	stats->isochstats.cntFrames = stats->isochstats.framecnt.current - stats->isochstats.framecnt.prev;
	stats->isochstats.cntFramesMissed = stats->isochstats.framelostcnt.current - stats->isochstats.framelostcnt.prev;
	stats->isochstats.cntSlips = stats->isochstats.slipcnt.current - stats->isochstats.slipcnt.prev;
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
	(*stats->output_handler)(stats);
    }
    if (!final)
	reporter_reset_transfer_stats_client_tcp(stats);
}

/*
 * Handles summing of threads
 */
void reporter_transfer_protocol_sum_client_tcp (struct TransferInfo *stats, int final) {
    if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	if (final) {
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
		reporter_reset_transfer_stats_client_tcp(stats);
	    }
	} else if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
	    (*stats->output_handler)(stats);
	    stats->threadcnt = 0;
	}
	reporter_reset_transfer_stats_client_tcp(stats);
    }
    if (final) {
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	stats->sock_callstats.write.TCPretry = stats->sock_callstats.write.totTCPretry;
#endif
	stats->cntBytes = stats->total.Bytes.current;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	if ((stats->output_handler) && !(stats->filter_this_sample_output))
	    (*stats->output_handler)(stats);
    }
}

void reporter_transfer_protocol_sum_server_tcp (struct TransferInfo *stats, int final) {
    if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	if (final) {
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
	    }
	} else if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
	    (*stats->output_handler)(stats);
	    stats->threadcnt = 0;
	}
	reporter_reset_transfer_stats_server_tcp(stats);
    }
    if (final) {
	int ix;
	stats->cntBytes = stats->total.Bytes.current;
	stats->sock_callstats.read.cntRead = stats->sock_callstats.read.totcntRead;
	for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->sock_callstats.read.bins[ix] = stats->sock_callstats.read.totbins[ix];
	}
	stats->cntBytes = stats->total.Bytes.current;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	if ((stats->output_handler) && !(stats->filter_this_sample_output))
	    (*stats->output_handler)(stats);
    }
}
void reporter_transfer_protocol_fullduplex_tcp (struct TransferInfo *stats, int final) {
    if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	if (final) {
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
	    }
	}
	stats->total.Bytes.prev = stats->total.Bytes.current;
    }
    if (final) {
	stats->cntBytes = stats->total.Bytes.current;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
    } else {
	reporter_set_timestamps_time(&stats->ts, INTERVAL);
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output))
	(*stats->output_handler)(stats);
}

void reporter_transfer_protocol_fullduplex_udp (struct TransferInfo *stats, int final) {
    if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	stats->cntDatagrams = stats->total.Datagrams.current - stats->total.Datagrams.prev;
	stats->cntIPG = stats->total.IPG.current - stats->total.IPG.prev;
	if (final) {
	    if ((stats->output_handler) && !(stats->filter_this_sample_output)) {
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
		if ((stats->ts.iEnd - stats->ts.iStart) > stats->ts.significant_partial)
		    (*stats->output_handler)(stats);
	    }
	}
	stats->total.Bytes.prev = stats->total.Bytes.current;
	stats->total.IPG.prev = stats->total.IPG.current;
	stats->total.Datagrams.prev = stats->total.Datagrams.current;
	if (stats->cntDatagrams)
	    stats->IPGsum = 0.0;
    }
    if (final) {
	stats->cntBytes = stats->total.Bytes.current;
	stats->cntBytes = stats->total.Bytes.current;
	stats->cntDatagrams = stats->total.Datagrams.current ;
	stats->cntIPG = stats->total.IPG.current;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
	reporter_set_timestamps_time(&stats->ts, TOTAL);
    } else {
	reporter_set_timestamps_time(&stats->ts, INTERVAL);
    }
    if ((stats->output_handler) && !(stats->filter_this_sample_output))
	(*stats->output_handler)(stats);
}

// Conditional print based on time
int reporter_condprint_time_interval_report (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    assert(stats!=NULL);
    //   printf("***sum handler = %p\n", (void *) data->GroupSumReport->transfer_protocol_sum_handler);
    int advance_jobq = 0;
    // Print a report if packet time exceeds the next report interval time,
    // Also signal to the caller to move to the next report (or packet ring)
    // if there was output. This will allow for more precise interval sum accounting.
    if (TimeDifference(stats->ts.nextTime, packet->packetTime) < 0) {
	assert(data->transfer_protocol_handler!=NULL);
	advance_jobq = 1;
	struct TransferInfo *sumstats = (data->GroupSumReport ? &data->GroupSumReport->info : NULL);
	struct TransferInfo *fullduplexstats = (data->FullDuplexReport ? &data->FullDuplexReport->info : NULL);
	stats->ts.packetTime = packet->packetTime;
#ifdef DEBUG_PPS
	printf("*** packetID TRIGGER = %ld pt=%ld.%ld empty=%d nt=%ld.%ld\n",packet->packetID, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->emptyreport, stats->ts.nextTime.tv_sec, stats->ts.nextTime.tv_usec);
#endif
	reporter_set_timestamps_time(&stats->ts, INTERVAL);
	(*data->transfer_protocol_handler)(data, 0);
	if (fullduplexstats && ((++data->FullDuplexReport->threads) == 2) && isEnhanced(stats->common)) {
	    data->FullDuplexReport->threads = 0;
	    assert(data->FullDuplexReport->transfer_protocol_sum_handler != NULL);
	    (*data->FullDuplexReport->transfer_protocol_sum_handler)(fullduplexstats, 0);
	}
	if (sumstats) {
	    if ((++data->GroupSumReport->threads) == data->GroupSumReport->reference.count)   {
		data->GroupSumReport->threads = 0;
		if ((data->GroupSumReport->reference.count > 1) || \
		    isSumOnly(data->info.common)) {
		    sumstats->filter_this_sample_output = 0;
		} else {
		    sumstats->filter_this_sample_output = 1;
		}
		reporter_set_timestamps_time(&sumstats->ts, INTERVAL);
		assert(data->GroupSumReport->transfer_protocol_sum_handler != NULL);
		(*data->GroupSumReport->transfer_protocol_sum_handler)(sumstats, 0);
	    }
	}
        // In the (hopefully unlikely event) the reporter fell behind
        // output the missed reports to catch up
	if ((stats->output_handler) && !(stats->filter_this_sample_output))
	    reporter_transfer_protocol_missed_reports(stats, packet);
    }
    return advance_jobq;
}

// Conditional print based on bursts or frames
int reporter_condprint_frame_interval_report_server_udp (struct ReporterData *data, struct ReportStruct *packet) {
    int advance_jobq = 0;
    struct TransferInfo *stats = &data->info;
    // first packet of a burst and not a duplicate
    assert(packet->burstsize != 0);
    if ((packet->burstsize == (packet->remaining + packet->packetLen)) && (stats->matchframeID != packet->frameID)) {
	stats->matchframeID=packet->frameID;
    }
    if ((packet->packetLen == packet->remaining) && (packet->frameID == stats->matchframeID)) {
	if ((stats->ts.iStart = TimeDifference(stats->ts.nextTime, stats->ts.startTime)) < 0)
	    stats->ts.iStart = 0.0;
	stats->frameID = packet->frameID;
	stats->ts.iEnd = TimeDifference(packet->packetTime, stats->ts.startTime);
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	stats->cntOutofOrder = stats->total.OutofOrder.current - stats->total.OutofOrder.prev;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->cntError = stats->total.Lost.current - stats->total.Lost.prev;
	stats->cntError -= stats->cntOutofOrder;
	if (stats->cntError < 0)
	    stats->cntError = 0;
	stats->cntDatagrams = stats->PacketID - stats->total.Datagrams.prev;
	if ((stats->output_handler) && !(stats->filter_this_sample_output))
	    (*stats->output_handler)(stats);
	reporter_reset_transfer_stats_server_udp(stats);
	advance_jobq = 1;
    }
    return advance_jobq;
}

int reporter_condprint_frame_interval_report_server_tcp (struct ReporterData *data, struct ReportStruct *packet) {
    fprintf(stderr, "FIX ME\n");
    return 1;
}

int reporter_condprint_burst_interval_report_server_tcp (struct ReporterData *data, struct ReportStruct *packet) {
    assert(packet->burstsize != 0);
    struct TransferInfo *stats = &data->info;

    int advance_jobq = 0;
    // first packet of a burst and not a duplicate
    if (packet->transit_ready) {
        stats->tripTime = reporter_handle_packet_oneway_transit(data, packet);
	if (stats->framelatency_histogram) {
	    histogram_insert(stats->framelatency_histogram, stats->tripTime, &packet->sentTime);
	}
	stats->tripTime *= 1e3; // convert from secs millisecs
//	printf("****sndpkt=%ld.%ld rxpkt=%ld.%ld\n", packet->sentTime.tv_sec, packet->sentTime.tv_usec, packet->packetTime.tv_sec,packet->packetTime.tv_usec);
	stats->ts.prevpacketTime = packet->prevSentTime;
	stats->ts.packetTime = packet->packetTime;
	reporter_set_timestamps_time(&stats->ts, FRAME);
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	if ((stats->output_handler) && !(stats->filter_this_sample_output))
	    (*stats->output_handler)(stats);
	reporter_reset_transfer_stats_server_tcp(stats);
	advance_jobq = 1;
    }
    return advance_jobq;
}

#ifdef __cplusplus
} /* end extern "C" */
#endif
