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

/*
  The following 4 functions are provided for Reporting
  styles that do not have all the reporting formats. For
  instance the provided CSV format does not have a settings
  report so it uses settings_notimpl.
  */
void* connection_notimpl( struct ConnectionInfo * nused, int nuse ) {
    return NULL;
}
void settings_notimpl( struct ReporterData * nused ) { }
void statistics_notimpl( struct TransferInfo * nused ) { }
void serverstatistics_notimpl( struct ConnectionInfo *nused1, struct TransferInfo *nused2 ) { }

// To add a reporting style include its header here.
#include "report_CSV.h"

struct ReportHeader *ReportRoot = NULL;
struct ReportHeader *ReportPendingHead = NULL;
struct ReportHeader *ReportPendingTail = NULL;
static int reporter_process_report (struct ReportHeader *report);
void process_report (struct ReportHeader *report);
int reporter_print(struct ReporterData *data, int type, int end);

#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats(struct ReporterData *data, int final);
#endif


// Reporter's reset of stats after a print occurs
static void reporter_reset_transfer_stats_client_tcp(struct TransferInfo *stats);
static void reporter_reset_transfer_stats_client_udp(struct TransferInfo *stats);
static void reporter_reset_transfer_stats_server_udp(struct TransferInfo *stats);
static void reporter_reset_transfer_stats_server_tcp(struct TransferInfo *stats);

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
	process_report ( reporthdr );
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
void ReportPacket (struct ReporterData* data, struct ReportStruct *packet) {
    assert(data != NULL);
#ifdef HAVE_THREAD_DEBUG
    if (packet->packetID < 0) {
	thread_debug("Reporting last packet for %p  qdepth=%d sock=%d", (void *) data, packetring_getcount(data->packetring), data->info.common->socket);
    }
#endif
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    // tcpi stats are only sampled on the report interval
    struct TransferInfo *stats = &data->info;
    if (isEnhanced(stats->common) && (stats->common->ThreadMode == kMode_Client) && \
	(TimeDifference(stats->ts.nextTime, packet->packetTime) < 0)) {
	gettcpistats(data, 0);
    }
#endif
    packetring_enqueue(data->packetring, packet);
#ifndef HAVE_THREAD
    /*
     * Process the report in this thread
     */
    process_report(data);
#endif
}

/*
 * EndJob is called by a traffic thread to inform the reporter
 * thread to print a final report and to remove the data report from its jobq.
 * It also handles the freeing reports and other closing actions
 */
void EndJob (struct ReportHeader *reporthdr, struct ReportStruct *finalpacket) {
    assert(reporthdr!=NULL);
    assert(finalpacket!=NULL);
    struct ReporterData *report = (struct ReporterData *) reporthdr->this_report;
    struct ReportStruct packet;
    struct TransferInfo *stats = &report->info;
    memset(&packet, 0, sizeof(struct ReportStruct));
    /*
     * Using PacketID of -1 ends reporting
     * It pushes a "special packet" through
     * the packet ring which will be detected
     * by the reporter thread as and end of traffic
     * event
     */
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    // tcpi stats are sampled on a final packet
    if (isEnhanced(stats->common) && (stats->common->ThreadMode == kMode_Client) && \
	(TimeDifference(stats->ts.nextTime, finalpacket->packetTime) < 0)) {
	gettcpistats(report, 0);
    }
#endif
    // clear the reporter done predicate
    report->packetring->consumerdone = 0;
    // the negative packetID is used to inform the report thread this traffic thread is done
    packet.packetID = -1;
    packet.packetLen = finalpacket->packetLen;
    packet.packetTime = finalpacket->packetTime;
    ReportPacket(report, &packet);
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
    if (isUDP(stats->common)) {
	if ((stats->common->ThreadMode == kMode_Server) && !isMulticast(stats->common) && !isNoUDPfin(stats->common)) {
	    // send a UDP acknowledgement back except when:
	    // 1) we're NOT receiving multicast
	    // 2) the user requested no final exchange
	    write_UDP_AckFIN(stats);
	}
	if ((stats->common->ThreadMode == kMode_Server) && (stats->common->socket != INVALID_SOCKET)) {
#if HAVE_THREAD_DEBUG
	    thread_debug("UDP server close sock=%d", stats->common->socket);
#endif
	    int rc = close(stats->common->socket);
	    WARN_errno(rc == SOCKET_ERROR, "end report close");
	}
    } else {
	if (report->FullDuplexReport && isBidir(report->FullDuplexReport->info.common)) {
	    if (bidir_stop_barrier(&report->FullDuplexReport->bidir_barrier)) {
		struct Condition *tmp = &report->FullDuplexReport->bidir_barrier.await;
		Condition_Destroy(tmp);
#if HAVE_THREAD_DEBUG
		thread_debug("Socket bidir close sock=%d", stats->common->socket);
#endif
		int rc = close(report->FullDuplexReport->info.common->socket);
		WARN_errno( rc == SOCKET_ERROR, "full duplex close" );
		FreeSumReport(report->FullDuplexReport);
	    }
	} else {
#if HAVE_THREAD_DEBUG
	    thread_debug("TCP close sock=%d", stats->common->socket);
#endif
	    int rc = close(stats->common->socket);
	    WARN_errno(rc == SOCKET_ERROR, "end report close");
	}
    }
    FreeReport(reporthdr);
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
static inline struct ReportHeader *reporter_jobq_set_root (void) {
    struct ReportHeader *root = NULL;
    Condition_Lock(ReportCond);
    // check the jobq for empty
    if (ReportRoot == NULL) {
	// The reporter is starting from an empty state
	// so set the load detect to trigger an initial delay
	reset_consumption_detector();
	if (!ReportPendingHead) {
	    Condition_TimedWait(&ReportCond, 1);
#ifdef HAVE_THREAD_DEBUG
	    thread_debug( "Jobq *WAIT* exit  %p/%p cond=%p", (void *) ReportRoot, (void *) ReportPendingHead, (void *) &ReportCond);
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
/*
 * This function is the loop that the reporter thread processes
 */
void reporter_spawn (struct thread_Settings *thread) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Reporter thread started");
#endif
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
     * o) There are more reports to ouput, ReportRoot has a report
     * o) The number of threads is greater than one which indicates
     *    either traffic threads are still running or a Listener thread
     *    is running. If equal to 1 then only the reporter thread is alive
     */
    while ((reporter_jobq_set_root() != NULL) || (thread_numuserthreads() > 1)){
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
		    // memory for *work_item is gone by now
		    *work_item = tmp;
		    if (!tmp)
			break;
		}
#ifdef HAVE_THREAD_DEBUG
//	        thread_debug( "Jobq *REMOVE* (%p)=%p (%p)=%p", (void *) work_item, (void *) (*work_item), (void *) &(*work_item)->next, (void *) *(&(*work_item)->next));
#endif
		work_item = &(*work_item)->next;
	    }
	}
    }
    if (thread->reporthdr) {
        reporter_connect_printf_tcp_final(thread->reporthdr);
	FreeReport(thread->reporthdr);
    }
#ifdef HAVE_THREAD_DEBUG
    if (sInterupted)
        reporter_jobq_dump();
    thread_debug("Reporter thread finished");
#endif
}

static void reporter_compute_connect_times (struct ReportHeader *hdr, double connect_time) {
#if 0
    // Compute end/end delay stats
    if (connect_time > 0.0) {
	hdr->connect_times.sum += connect_time;
	if ((hdr->connect_times.cnt++) == 1) {
	    hdr->connect_times.vd = connect_time;
	    hdr->connect_times.mean = connect_time;
	    hdr->connect_times.m2 = connect_time * connect_time;
	} else {
	    hdr->connect_times.vd = connect_time - hdr->connect_times.mean;
	    hdr->connect_times.mean = hdr->connect_times.mean + (hdr->connect_times.vd / hdr->connect_times.cnt);
	    hdr->connect_times.m2 = hdr->connect_times.m2 + (hdr->connect_times.vd * (connect_time - hdr->connect_times.mean));
	}
	// mean min max tests
	if (connect_time < hdr->connect_times.min)
	    hdr->connect_times.min = connect_time;
	if (connect_time > hdr->connect_times.max)
	    hdr->connect_times.max = connect_time;
    } else {
	hdr->connect_times.err++;
    }
#endif
}

/*
 * Used for single threaded reporting
 */
void process_report (struct ReportHeader *report) {
#if 0
    if (report != NULL) {
      if (reporter_process_report(report)) {
	    if (report->report.info.latency_histogram) {
		histogram_delete(report->report.info.latency_histogram);
	    }
	    if (report->report.info.framelatency_histogram) {
		histogram_delete(report->report.info.framelatency_histogram);
	    }
            free(report);
        }
    }
#endif
}


// The Transfer or Data report is by far the most complicated report

static int reporter_process_transfer_report (struct ReporterData *this_ireport) {
    assert(this_ireport != NULL);
    assert(this_ireport->packet_handler != NULL);
    struct TransferInfo *sumstats = (this_ireport->GroupSumReport ? &this_ireport->GroupSumReport->info : NULL);
    struct TransferInfo *bidirstats = (this_ireport->FullDuplexReport ? &this_ireport->FullDuplexReport->info : NULL);
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
	if (!(packet->packetID < 0)) {
	    // Check to output any interval reports, do this prior
	    // to packet handling to preserve interval accounting
	    if (this_ireport->transfer_interval_handler) {
		advance_jobq = (*this_ireport->transfer_interval_handler)(this_ireport, packet);
	    }
	    // Do the packet accounting per the handler type
	    (*this_ireport->packet_handler)(this_ireport, packet);
	    // Sum reports update the report header's last
	    // packet time after the handler. This means
	    // the report header's packet time will be
	    // the previous time before the interval
	    if (sumstats)
		sumstats->ts.packetTime = packet->packetTime;
	    if (bidirstats)
		bidirstats->ts.packetTime = packet->packetTime;
	} else {
	    need_free = 1;
	    advance_jobq = 1;
	    // A last packet event was detected
	    // printf("last packet event detected\n"); fflush(stdout);
	    this_ireport->reporter_thread_suspends = consumption_detector.reporter_thread_suspends;
	    (*this_ireport->packet_handler)(this_ireport, packet);
	    this_ireport->info.ts.packetTime = packet->packetTime;
	    assert(this_ireport->transfer_protocol_handler != NULL);
	    (*this_ireport->transfer_protocol_handler)(this_ireport, 1);
	    // This is a final report so set the sum report header's packet time
	    // Note, the thread with the max value will set this
	    if (bidirstats) {
		// The largest packet timestamp sets the sum report final time
		if (TimeDifference(bidirstats->ts.packetTime, packet->packetTime) > 0) {
		    bidirstats->ts.packetTime = packet->packetTime;
		}
		if (DecrSumReportRefCounter(this_ireport->FullDuplexReport) == 0) {
		    if (this_ireport->FullDuplexReport->transfer_protocol_sum_handler) {
			(*this_ireport->FullDuplexReport->transfer_protocol_sum_handler)(bidirstats, 1);
		    }
		    // FullDuplex report gets freed by a traffic thread (per its barrier)
		}
	    }
	    if (sumstats) {
		if (TimeDifference(sumstats->ts.packetTime, packet->packetTime) > 0) {
		    sumstats->ts.packetTime = packet->packetTime;
		}
		if (DecrSumReportRefCounter(this_ireport->GroupSumReport) == 0) {
		    if ((this_ireport->GroupSumReport->transfer_protocol_sum_handler) && \
			(this_ireport->GroupSumReport->reference.maxcount > (bidirstats ? 2 : 1))) {
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
static inline int reporter_process_report (struct ReportHeader *reporthdr) {
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
	reporter_print_connection_report((struct ConnectionInfo *)reporthdr->this_report);
	fflush(stdout);
	FreeReport(reporthdr);
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
	fprintf(stderr,"Invalid report type in process report %p\n", (void *) reporthdr->this_report);
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
    return;
}
void reporter_transfer_protocol_null (struct ReporterData *data, int final){
    return;
}

inline void reporter_handle_packet_pps (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    if (!packet->emptyreport) {
        stats->total.Datagrams.current++;
        stats->total.IPG.current++;
    }
    stats->ts.IPGstart = packet->packetTime;
    stats->IPGsum += TimeDifference(packet->sentTime, packet->prevSentTime);
#ifdef DEBUG_PPS
    printf("*** IPGsum = %f cnt=%ld ipg=%ld.%ld pt=%ld.%ld id=%ld empty=%d transit=%f %ld.%ld\n", stats->IPGsum, stats->cntIPG, stats->ts.IPGstart.tv_sec, stats->ts.IPGstart.tv_usec, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->packetID, packet->emptyreport, TimeDifference(packet->sentTime, packet->prevSentTime),packet->prevSentTime.tv_sec, packet->prevSentTime.tv_usec);
#endif
}

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

static inline void reporter_handle_burst_tcp_transit (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
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
	// printf("***Burst id = %ld, transit = %f\n", packet->frameID, stats->transit.lastTransit);
    }
}

inline void reporter_handle_packet_isochronous (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    // printf("fid=%lu bs=%lu remain=%lu\n", packet->frameID, packet->burstsize, packet->remaining);
    if (packet->frameID && packet->burstsize && packet->remaining) {
	int framedelta=0;
	// very first isochronous frame
	if (!stats->isochstats.frameID) {
	    stats->isochstats.framecnt=packet->frameID;
	    stats->isochstats.framecnt=1;
	    stats->isochstats.framecnt=1;
	}
	// perform client and server frame based accounting
	if ((framedelta = (packet->frameID - stats->isochstats.frameID))) {
	    stats->isochstats.framecnt++;
	    stats->isochstats.framecnt++;
	    if (framedelta > 1) {
		if (stats->common->ThreadMode == kMode_Server) {
		    int lost = framedelta - (packet->frameID - packet->prevframeID);
		    stats->isochstats.framelostcnt += lost;
		    stats->isochstats.framelostcnt += lost;
		} else {
		    stats->isochstats.framelostcnt += (framedelta-1);
		    stats->isochstats.framelostcnt += (framedelta-1);
		    stats->isochstats.slipcnt++;
		    stats->isochstats.slipcnt++;
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
	reporter_handle_burst_tcp_transit(data, packet);
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

void reporter_handle_packet_client (struct ReporterData *data, struct ReportStruct *packet) {
    struct TransferInfo *stats = &data->info;
    stats->total.Bytes.current += packet->packetLen;
    stats->ts.packetTime = packet->packetTime;
    if (!packet->emptyreport) {
        if (packet->errwrite && (packet->errwrite != WriteErrNoAccount)) {
	    stats->sock_callstats.write.WriteErr++;
	    stats->sock_callstats.write.totWriteErr++;
	}
	// These are valid packets that need standard iperf accounting
	stats->sock_callstats.write.WriteCnt++;
	stats->sock_callstats.write.totWriteCnt++;
	if (isIsochronous(stats->common)) {
	    reporter_handle_packet_isochronous(data, packet);
	}
    }
    if (isUDP(stats->common)) {
	stats->PacketID = packet->packetID;
	reporter_handle_packet_pps(data, packet);
    }
}


#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats (struct ReporterData *data, int final) {
    assert(data!=NULL);
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    static int cnt = 0;
    struct tcp_info tcp_internal;
    socklen_t tcp_info_length = sizeof(struct tcp_info);
    int retry = 0;
    // Read the TCP retry stats for a client.  Do this
    // on  a report interval period.
    int rc = (stats->common->socket==INVALID_SOCKET) ? 0 : 1;
    if (rc) {
        rc = (getsockopt(stats->common->socket, IPPROTO_TCP, TCP_INFO, &tcp_internal, &tcp_info_length) < 0) ? 0 : 1;
	if (!rc)
	    stats->common->socket = INVALID_SOCKET;
	else
	    // Mark stale now so next call at report interval will update
	    stats->sock_callstats.write.up_to_date = 1;
    }
    if (!rc) {
        stats->sock_callstats.write.TCPretry = 0;
	stats->sock_callstats.write.cwnd = -1;
	stats->sock_callstats.write.rtt = 0;
    } else {
        retry = tcp_internal.tcpi_total_retrans - stats->sock_callstats.write.lastTCPretry;
	stats->sock_callstats.write.TCPretry = retry;
	stats->sock_callstats.write.totTCPretry += retry;
	stats->sock_callstats.write.lastTCPretry = tcp_internal.tcpi_total_retrans;
	stats->sock_callstats.write.cwnd = tcp_internal.tcpi_snd_cwnd * tcp_internal.tcpi_snd_mss / 1024;
	stats->sock_callstats.write.rtt = tcp_internal.tcpi_rtt;
	// New average = old average * (n-1)/n + new value/n
	cnt++;
	stats->sock_callstats.write.meanrtt = (stats->sock_callstats.write.meanrtt * ((double) (cnt - 1) / (double) cnt)) + ((double) (tcp_internal.tcpi_rtt) / (double) cnt);
	stats->sock_callstats.write.rtt = tcp_internal.tcpi_rtt;
	if (sumstats) {
	    sumstats->sock_callstats.write.TCPretry += retry;
	    sumstats->sock_callstats.write.totTCPretry += retry;
	}
    }
    if (final) {
        stats->sock_callstats.write.rtt = stats->sock_callstats.write.meanrtt;
    }
}
#endif
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
    assert(stats->output_handler != NULL);
    while (TimeDifference(packet->packetTime, stats->ts.nextTime) > TimeDouble(stats->ts.intervalTime)) {
//	printf("**** cmp=%f/%f next %ld.%ld packet %ld.%ld id=%ld\n", TimeDifference(packet->packetTime, stats->ts.nextTime), TimeDouble(stats->ts.intervalTime), stats->ts.nextTime.tv_sec, stats->ts.nextTime.tv_usec, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->packetID);
	reporter_set_timestamps_time(&stats->ts, INTERVAL);
	struct TransferInfo emptystats;
	memset(&emptystats, 0, sizeof(struct TransferInfo));
	emptystats.ts.iStart = stats->ts.iStart;
	emptystats.ts.iEnd = stats->ts.iEnd;
	emptystats.common = stats->common;
	emptystats.header_printed = 1;
	(*stats->output_handler)(&emptystats);
    }
}
#if 0
// Actions required after an interval report has been outputted
static inline void reporter_reset_transfer_stats(struct ReporterData *data) {
    struct TransferInfo *stats = &data->info;
    stats->lastOutofOrder = stats->cntOutofOrder;
    if (stats->cntError < 0) {
	stats->cntError = 0;
    }
    data->lastError = data->cntError;
    data->lastDatagrams = ((stats->common.ThreadModem == kMode_Server) ? stats->PacketID : data->cntDatagrams);
    data->lastTotal = data->Bytes.current;
    /*
     * Reset transfer stats now that both the individual and SUM reports
     * have completed
     */
    if (stats->mUDP) {
	stats->IPGcnt = 0;
	stats->IPGsum = 0;
	if (stats->mUDP == kMode_Server) {
	    stats->l2counts.cnt = 0;
	    stats->l2counts.unknown = 0;
	    stats->l2counts.udpcsumerr = 0;
	    stats->l2counts.lengtherr = 0;
	}
    }
    if (stats->mEnhanced) {
	if ((stats->mTCP == (char)kMode_Client) || (stats->mUDP == (char)kMode_Client)) {
	    stats->sock_callstats.write.WriteCnt = 0;
	    stats->sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	    stats->sock_callstats.write.TCPretry = 0;
	    stats->sock_callstats.write.up_to_date = 0;
#endif
	} else if (stats->mTCP == (char)kMode_Server) {
	    int ix;
	    stats->sock_callstats.read.cntRead = 0;
	    for (ix = 0; ix < 8; ix++) {
		stats->sock_callstats.read.bins[ix] = 0;
	    }
	}
    // Reset the enhanced stats for the next report interval
	if (stats->mUDP) {
	    stats->transit.minTransit=stats->transit.lastTransit;
	    stats->transit.maxTransit=stats->transit.lastTransit;
	    stats->transit.sumTransit = stats->transit.lastTransit;
	    stats->transit.cntTransit = 0;
	    stats->transit.vdTransit = 0;
	    stats->transit.meanTransit = 0;
	    stats->transit.m2Transit = 0;
	    stats->isochstats.framecnt = 0;
	    stats->isochstats.framelostcnt = 0;
	    stats->isochstats.slipcnt = 0;
	}
    }
}
#endif

static inline void reporter_reset_transfer_stats_bidir (struct TransferInfo *stats) {
    stats->total.Bytes.prev = stats->total.Bytes.current;
}

static inline void reporter_reset_transfer_stats_client_tcp (struct TransferInfo *stats) {
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->sock_callstats.write.WriteCnt = 0;
    stats->sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    stats->sock_callstats.write.TCPretry = 0;
    stats->sock_callstats.write.up_to_date = 0;
#endif
}

static inline void reporter_reset_transfer_stats_client_udp (struct TransferInfo *stats) {
    if (stats->cntError < 0) {
	stats->cntError = 0;
    }
    stats->total.Lost.prev = stats->total.Lost.current;
    stats->total.Datagrams.prev = stats->PacketID;
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->total.IPG.prev = stats->total.IPG.current;
    stats->sock_callstats.write.WriteCnt = 0;
    stats->sock_callstats.write.WriteErr = 0;
    stats->isochstats.framecnt = 0;
    stats->isochstats.framelostcnt = 0;
    stats->isochstats.slipcnt = 0;
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
    stats->transit.minTransit=stats->transit.lastTransit;
    stats->transit.maxTransit=stats->transit.lastTransit;
    stats->transit.sumTransit = stats->transit.lastTransit;
    stats->transit.cntTransit = 0;
    stats->transit.vdTransit = 0;
    stats->transit.meanTransit = 0;
    stats->transit.m2Transit = 0;
}
static inline void reporter_reset_transfer_stats_server_udp (struct TransferInfo *stats) {
    // Reset the enhanced stats for the next report interval
    stats->total.Bytes.prev = stats->total.Bytes.current;
    stats->total.Datagrams.prev = stats->total.Datagrams.current;
    stats->total.OutofOrder.prev = stats->total.OutofOrder.current;
    stats->total.Lost.prev = stats->total.Lost.current;
    stats->total.IPG.prev = stats->total.IPG.current;
    stats->transit.minTransit=stats->transit.lastTransit;
    stats->transit.maxTransit=stats->transit.lastTransit;
    stats->transit.sumTransit = stats->transit.lastTransit;
    stats->transit.cntTransit = 0;
    stats->transit.vdTransit = 0;
    stats->transit.meanTransit = 0;
    stats->transit.m2Transit = 0;
    stats->isochstats.framecnt = 0;
    stats->isochstats.framelostcnt = 0;
    stats->isochstats.slipcnt = 0;
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
// o) updates the sum and bidir reports
//
void reporter_transfer_protocol_server_udp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *bidirstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
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
    if (sumstats) {
	sumstats->total.OutofOrder.current += stats->total.OutofOrder.current - stats->total.OutofOrder.prev;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	sumstats->total.Lost.current += stats->total.Lost.current - stats->total.Lost.prev;
	sumstats->total.Datagrams.current += stats->PacketID - stats->total.Datagrams.prev;
	sumstats->total.Bytes.current += stats->cntBytes;
	if (sumstats->IPGsum < stats->IPGsum)
	    sumstats->IPGsum = stats->IPGsum;
	sumstats->total.IPG.current += stats->total.IPG.current;
    }
    if (bidirstats) {
	bidirstats->total.Bytes.current += stats->cntBytes;
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
	    if (final)
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
	    (*stats->output_handler)(stats);
	}
	reporter_set_timestamps_time(&stats->ts, TOTAL);
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
	if (stats->latency_histogram) {
	    stats->latency_histogram->final = 1;
	}
    }
    if (stats->output_handler)
	(*stats->output_handler)(stats);
    if (!final)
	reporter_reset_transfer_stats_server_udp(stats);
}

void reporter_transfer_protocol_sum_server_udp (struct TransferInfo *stats, int final) {
    assert(stats->output_handler != NULL);
    if (stats->sumflag) {
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
	}
	(*stats->output_handler)(stats);
	if (!final)
	    reporter_reset_transfer_stats_server_udp(stats);
    }
}
void reporter_transfer_protocol_sum_client_udp (struct TransferInfo *stats, int final) {
    assert(stats->output_handler != NULL);
    if (final) {
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	stats->sock_callstats.write.TCPretry = stats->sock_callstats.write.totTCPretry;
	stats->cntDatagrams = stats->total.Datagrams.current;
	stats->cntBytes = stats->total.Bytes.current;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
    } else {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
    }
    (*stats->output_handler)(stats);
    if (!final)
	reporter_reset_transfer_stats_client_udp(stats);
}

void reporter_transfer_protocol_client_udp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *bidirstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    assert(stats->output_handler != NULL);
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
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
    }
    if (bidirstats) {
	bidirstats->total.Bytes.current += stats->cntBytes;
    }
    if (final) {
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	stats->cntBytes = stats->total.Bytes.current;
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	stats->cntIPG = stats->total.IPG.current;
	stats->cntDatagrams = stats->PacketID;
	stats->IPGsum = TimeDifference(stats->ts.packetTime, stats->ts.startTime);
    } else {
	if (stats->ts.iEnd > 0) {
	    stats->cntDatagrams = stats->total.Datagrams.current - stats->total.Datagrams.prev;
	    stats->cntIPG = (stats->total.IPG.current - stats->total.IPG.prev);
	} else {
	    stats->cntIPG = 0;
	}
    }
    (*stats->output_handler)(stats);
    if (final)
	printf(report_datagrams, stats->transferID, stats->total.Datagrams.current);
    else
	reporter_reset_transfer_stats_client_udp(stats);
}

void reporter_transfer_protocol_server_tcp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *bidirstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
    int ix;
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
    if (bidirstats) {
	bidirstats->total.Bytes.current += stats->cntBytes;
    }
    if (final) {
	if ((stats->cntBytes > 0) && stats->output_handler && !TimeZero(stats->ts.intervalTime)) {
	    // print a partial interval report if enable and this a final
	    reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
	    (*stats->output_handler)(stats);
	    reporter_reset_transfer_stats_server_tcp(stats);
        }
	reporter_set_timestamps_time(&stats->ts, TOTAL);
        stats->cntBytes = stats->total.Bytes.current;
        stats->sock_callstats.read.cntRead = stats->sock_callstats.read.totcntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->sock_callstats.read.bins[ix] = stats->sock_callstats.read.totbins[ix];
        }
	stats->transit.sumTransit = stats->transit.totsumTransit;
	stats->transit.cntTransit = stats->transit.totcntTransit;
	stats->transit.minTransit = stats->transit.totminTransit;
	stats->transit.maxTransit = stats->transit.totmaxTransit;
	stats->transit.m2Transit = stats->transit.totm2Transit;
	if (stats->framelatency_histogram) {
	    stats->framelatency_histogram->final = 1;
	}
    }
    if (stats->output_handler)
	(*stats->output_handler)(stats);
    if (!final)
	reporter_reset_transfer_stats_server_tcp(stats);
}

void reporter_transfer_protocol_client_tcp (struct ReporterData *data, int final) {
    struct TransferInfo *stats = &data->info;
    struct TransferInfo *sumstats = (data->GroupSumReport != NULL) ? &data->GroupSumReport->info : NULL;
    struct TransferInfo *bidirstats = (data->FullDuplexReport != NULL) ? &data->FullDuplexReport->info : NULL;
    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
    if (sumstats) {
	sumstats->total.Bytes.current += stats->cntBytes;
	sumstats->sock_callstats.write.WriteErr += stats->sock_callstats.write.WriteErr;
	sumstats->sock_callstats.write.WriteCnt += stats->sock_callstats.write.WriteCnt;
	sumstats->sock_callstats.write.TCPretry += stats->sock_callstats.write.TCPretry;
	sumstats->sock_callstats.write.totWriteErr += stats->sock_callstats.write.WriteErr;
	sumstats->sock_callstats.write.totWriteCnt += stats->sock_callstats.write.WriteCnt;
	sumstats->sock_callstats.write.totTCPretry += stats->sock_callstats.write.TCPretry;
	sumstats->threadcnt++;
    }
    if (bidirstats) {
	bidirstats->total.Bytes.current += stats->cntBytes;
    }
    if (final) {
	if ((stats->cntBytes > 0) && stats->output_handler && !TimeZero(stats->ts.intervalTime)) {
	    // print a partial interval report if enable and this a final
	    reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
	    (*stats->output_handler)(stats);
	    reporter_reset_transfer_stats_server_tcp(stats);
        }
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	stats->sock_callstats.write.TCPretry = stats->sock_callstats.write.totTCPretry;
	stats->cntBytes = stats->total.Bytes.current;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
    }
    if (stats->output_handler)
	(*stats->output_handler)(stats);
    if (!final)
	reporter_reset_transfer_stats_client_tcp(stats);
}

#if 0
void reporter_transfer_protocol_client_all_final(struct ReporterData *data) {
    struct TransferInfo *stats = &data->info;
    assert(stats->output_handler != NULL);

#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    if (isEnhanced(stats->common) && (stats->common->ThreadMode == kMode_Client) && (!stats->sock_callstats.write.up_to_date))
        gettcpistats(stats, 1);
#endif
        stats->cntOutofOrder = stats->cntOutofOrder;
        // assume most of the time out-of-order packets are not
        // duplicate packets, so conditionally subtract them from the lost packets.
        stats->cntError = stats->cntError;
        stats->cntError -= stats->cntOutofOrder;
        if ( stats->cntError < 0 ) {
            stats->cntError = 0;
        }
        stats->cntDatagrams = ((stats->common.ThreadMode == kMode_Server) ? stats->PacketID - INITIAL_PACKETID : stats->cntDatagrams);
        stats->cntBytes = stats->total.Bytes.current;
        stats->iStart = 0;

	if (stats->mUDP == kMode_Server) {
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
	}
	if ((stats->mTCP == kMode_Client) || (stats->mUDP == kMode_Client)) {
	    stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	    stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	    if (stats->mTCP == kMode_Client) {
		stats->sock_callstats.write.TCPretry = stats->sock_callstats.write.totTCPretry;
	    }
	}
	if (stats->mTCP == kMode_Server) {
	    int ix;
	    stats->sock_callstats.read.cntRead = stats->sock_callstats.read.totcntRead;
	    for (ix = 0; ix < 8; ix++) {
		stats->sock_callstats.read.bins[ix] = stats->sock_callstats.read.totbins[ix];
	    }
	    if (stats->clientStartTime.tv_sec > 0)
		stats->tripTime = TimeDifference( stats->packetTime, stats->clientStartTime );
	    else
		stats->tripTime = 0;
	}
	if (stats->iEnd > 0) {
	    stats->cntIPG = (int) (stats->cntDatagrams / stats->iEnd);
	} else {
	    stats->cntIPG = 0;
	}
	stats->IPGsum = 1;
        stats->free = 1;
	if (stats->mIsochronous) {
	    stats->isochstats.framecnt = stats->isochstats.framecnt;
	    stats->isochstats.framelostcnt = stats->isochstats.framelostcnt;
	    stats->isochstats.slipcnt = stats->isochstats.slipcnt;
	}
        reporter_print( stats, TRANSFER_REPORT, 1 );
}

#endif
/*
 * Handles summing of threads
 */
void reporter_transfer_protocol_sum_client_tcp (struct TransferInfo *stats, int final) {
    assert(stats->output_handler != NULL);
    if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	if (final)
	    reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
	(*stats->output_handler)(stats);
	if (!final)
	    stats->threadcnt = 0;
	reporter_reset_transfer_stats_client_tcp(stats);
    }
    if (final) {
	stats->sock_callstats.write.WriteErr = stats->sock_callstats.write.totWriteErr;
	stats->sock_callstats.write.WriteCnt = stats->sock_callstats.write.totWriteCnt;
	stats->sock_callstats.write.TCPretry = stats->sock_callstats.write.totTCPretry;
	stats->cntBytes = stats->total.Bytes.current;
        reporter_set_timestamps_time(&stats->ts, TOTAL);
	(*stats->output_handler)(stats);
    }
}

void reporter_transfer_protocol_sum_server_tcp (struct TransferInfo *stats, int final) {
    assert(stats->output_handler != NULL);
    if (stats->sumflag) {
	if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	    stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	    if (final)
		reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
	    (*stats->output_handler)(stats);
	    if (!final)
		stats->threadcnt = 0;
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
	    (*stats->output_handler)(stats);
	}
    }
}
void reporter_transfer_protocol_bidir_tcp (struct TransferInfo *stats, int final) {
    assert(stats->output_handler != NULL);
    if (!final || (final && (stats->cntBytes > 0) && !TimeZero(stats->ts.intervalTime))) {
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	if (final)
	    reporter_set_timestamps_time(&stats->ts, FINALPARTIAL);
	(*stats->output_handler)(stats);
	stats->total.Bytes.prev = stats->total.Bytes.current;
    }
    if (final) {
	stats->cntBytes = stats->total.Bytes.current;
	reporter_set_timestamps_time(&stats->ts, TOTAL);
	(*stats->output_handler)(stats);
    }
}

void reporter_transfer_protocol_bidir_udp (struct TransferInfo *stats, int final) {
    assert(stats->output_handler != NULL);
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
	struct TransferInfo *bidirstats = (data->FullDuplexReport ? &data->FullDuplexReport->info : NULL);
	stats->ts.packetTime = packet->packetTime;
#ifdef DEBUG_PPS
	printf("*** packetID TRIGGER = %ld pt=%ld.%ld empty=%d nt=%ld.%ld\n",packet->packetID, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->emptyreport, stats->ts.nextTime.tv_sec, stats->ts.nextTime.tv_usec);
#endif
	reporter_set_timestamps_time(&stats->ts, INTERVAL);
	(*data->transfer_protocol_handler)(data, 0);
	if (bidirstats && ((++data->FullDuplexReport->threads) == 2)) {
	    data->FullDuplexReport->threads = 0;
	    reporter_set_timestamps_time(&bidirstats->ts, INTERVAL);
	    assert(data->FullDuplexReport->transfer_protocol_sum_handler != NULL);
	    (*data->FullDuplexReport->transfer_protocol_sum_handler)(bidirstats, 0);
	}
	if (sumstats) {
	    if ((++data->GroupSumReport->threads) == data->GroupSumReport->reference.count)   {
		data->GroupSumReport->threads = 0;
		if (data->GroupSumReport->reference.count > (bidirstats ? 2 : 1))
		    sumstats->sumflag = 1;
		reporter_set_timestamps_time(&sumstats->ts, INTERVAL);
		assert(data->GroupSumReport->transfer_protocol_sum_handler != NULL);
		(*data->GroupSumReport->transfer_protocol_sum_handler)(sumstats, 0);
	    }
	}
        // In the (hopefully unlikely event) the reporter fell behind
        // ouput the missed reports to catch up
	reporter_transfer_protocol_missed_reports(stats, packet);
    }
    return advance_jobq;
}

// Conditional print based on bursts or frames
int reporter_condprint_frame_interval_report_udp (struct ReporterData *data, struct ReportStruct *packet) {
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
	(*stats->output_handler)(stats);
	reporter_reset_transfer_stats_server_udp(stats);
	advance_jobq = 1;
    }
    return advance_jobq;
}

int reporter_condprint_frame_interval_report_tcp (struct ReporterData *data, struct ReportStruct *packet) {
    assert(packet->burstsize != 0);
    struct TransferInfo *stats = &data->info;

    int advance_jobq = 0;
    if (!packet->frameID) {
	stats->matchframeID = 1;
	return 1;
    }
    // first packet of a burst and not a duplicate
    if (packet->transit_ready) {
	stats->matchframeID++;
//	printf("****sndpkt=%ld.%ld rxpkt=%ld.%ld\n", packet->sentTime.tv_sec, packet->sentTime.tv_usec, packet->packetTime.tv_sec,packet->packetTime.tv_usec);
	stats->ts.prevpacketTime = packet->prevSentTime;
	stats->ts.packetTime = packet->packetTime;
	reporter_set_timestamps_time(&stats->ts, FRAME);
	stats->cntBytes = stats->total.Bytes.current - stats->total.Bytes.prev;
	(*stats->output_handler)(stats);
	reporter_reset_transfer_stats_client_tcp(stats);
	advance_jobq = 1;
    }
    return advance_jobq;
}

#ifdef __cplusplus
} /* end extern "C" */
#endif
