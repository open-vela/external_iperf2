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
#include "report_default.h"
#include "report_CSV.h"

// The following array of report structs contains the
// pointers required for reporting in different reporting
// styles. To add a reporting style add a report struct
// below.
report_connection connection_reports[kReport_MAXIMUM] = {
    reporter_reportpeer,
    CSV_peer
};

report_settings settings_reports[kReport_MAXIMUM] = {
    reporter_reportsettings,
    settings_notimpl
};

report_statistics statistics_reports[kReport_MAXIMUM] = {
    reporter_printstats,
    CSV_stats
};

report_serverstatistics serverstatistics_reports[kReport_MAXIMUM] = {
    reporter_serverstats,
    CSV_serverstats
};

report_statistics multiple_reports[kReport_MAXIMUM] = {
    reporter_multistats,
    CSV_stats
};

report_statistics bidir_reports[kReport_MAXIMUM] = {
    reporter_bidirstats,
    statistics_notimpl
};
report_statistics frame_udpreports[kReport_MAXIMUM] = {
    reporter_framestats_udp,
    statistics_notimpl
};
report_statistics frame_tcpreports[kReport_MAXIMUM] = {
    reporter_framestats_tcp,
    statistics_notimpl
};

char buffer[SNBUFFERSIZE]; // Buffer for printing
struct ReportHeader *ReportRoot = NULL;
struct ReportHeader *ReportPendingHead = NULL;
struct ReportHeader *ReportPendingTail = NULL;
static int reporter_process_report (struct ReportHeader *report, struct thread_Settings *mSettings);
void process_report (struct ReportHeader *report);
int reporter_print(struct ReporterData *stats, int type, int end);
void PrintMSS(struct ReporterData *stats);

// Reporter private routines below

static void reporter_handle_packet_null(struct ReportHeader *report, struct ReportStruct *packet) {return;}
static void reporter_transfer_protocol_null(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final){return;}

// Packet accounting routines
static void reporter_handle_packet_server_udp(struct ReportHeader *report, struct ReportStruct *packet);
static void reporter_handle_packet_server_tcp(struct ReportHeader *report, struct ReportStruct *packet);
static void reporter_handle_packet_client(struct ReportHeader *report, struct ReportStruct *packet);
static void reporter_handle_packet_pps(struct ReporterData *data, struct TransferInfo *stats, struct ReportStruct *packet);


// Reporter's conditional print, right now only time based sampling, possibly add packet based
static int reporter_condprint_time_interval_report(struct ReportHeader *reporthdr, struct ReportStruct *packet);
static int reporter_condprint_packet_interval_report(struct ReportHeader *reporthdr, struct ReportStruct *packet);
static int reporter_condprint_frame_interval_report_udp(struct ReportHeader *reporthdr, struct ReportStruct *packet);
static int reporter_condprint_frame_interval_report_tcp(struct ReportHeader *reporthdr, struct ReportStruct *packet);
static void reporter_set_timestamps_time(struct ReporterData *stats, enum TimestampType);

// Reporter's interval ouput specialize routines
static void reporter_transfer_protocol_reports(struct ReporterData *stats, struct ReportStruct *packet);
static void reporter_transfer_protocol_multireports(struct ReporterData *stats, struct ReportStruct *packet);
static void reporter_transfer_protocol_client_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void reporter_transfer_protocol_client_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void reporter_transfer_protocol_server_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void reporter_transfer_protocol_server_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);

// Reporter's sum ouput routines (per -P > 1)
static void reporter_transfer_protocol_sum_client_tcp(struct ReporterData *stats, int final);
static void reporter_transfer_protocol_sum_server_tcp(struct ReporterData *stats, int final);
static void reporter_transfer_protocol_sum_client_udp(struct ReporterData *stats, int final);
static void reporter_transfer_protocol_sum_server_udp(struct ReporterData *stats, int final);
static void reporter_transfer_protocol_bidir_tcp(struct ReporterData *stats, int final);
static void reporter_transfer_protocol_bidir_udp(struct ReporterData *stats, int final);
static void reporter_connect_printf_tcp_final(struct ReportHeader *multihdr);

// Reporter's reset of stats after a print occurs
static void reporter_reset_transfer_stats(struct ReporterData *stats);
static inline void reporter_reset_transfer_stats_client_tcp(struct ReporterData *stats);
static inline void reporter_reset_transfer_stats_client_udp(struct ReporterData *stats);
static inline void reporter_reset_transfer_stats_server_udp(struct ReporterData *stats);
static inline void reporter_reset_transfer_stats_server_tcp(struct ReporterData *stats);

static void InitDataReport(struct thread_Settings *mSettings);
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats(struct ReporterData *stats, struct ReporterData *sumstats, int final);
#endif


void PostReport (struct ReportHeader *reporthdr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Jobq *POST* report %p (0x%X)", reporthdr, reporthdr->report.type);
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
void ReportPacket( struct ReportHeader* agent, struct ReportStruct *packet ) {
    if ( agent != NULL ) {
#ifdef HAVE_THREAD_DEBUG
	if (packet->packetID < 0) {
	    thread_debug("Reporting last packet for %p  qdepth=%d sock=%d", (void *) agent, packetring_getcount(agent->packetring), agent->report.info.socket);
	}
#endif
        packetring_enqueue(agent->packetring, packet);
#ifndef HAVE_THREAD
        /*
         * Process the report in this thread
         */
        process_report ( agent );
#endif
    }
}

/*
 * CloseReport is called by a transfer agent to finalize
 * the report and signal transfer is over. Context is traffic thread
 */
void CloseReport(struct ReportHeader *agent, struct ReportStruct *finalpacket) {
    if (agent != NULL) {
	struct ReportStruct packet;
        /*
         * Using PacketID of -1 ends reporting
         * It pushes a "special packet" through
         * the packet ring which will be detected
         * by the reporter thread as and end of traffic
         * event
         */
        packet.packetID = -1;
        packet.packetLen = finalpacket->packetLen;
	packet.packetTime = finalpacket->packetTime;
        ReportPacket(agent, &packet);
    }
}

/*
 * EndReport signifies the agent no longer is interested
 * in the report. Calls to GetReport will no longer be
 * filled.  Context is traffic thread
 */
void EndReport(struct ReportHeader *agent) {
    if (agent) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug( "Traffic thread awaiting reporter to be done with %p and cond %p", (void *)agent, (void *) agent->packetring->awake_producer);
#endif
        Condition_Lock((*(agent->packetring->awake_producer)));
	while (!agent->packetring->consumerdone) {
	    // This wait time is the lag between the reporter thread
	    // and the traffic thread, a reporter thread with lots of
	    // reports (e.g. fastsampling) can lag per the i/o
	    Condition_TimedWait(agent->packetring->awake_producer, 1);
	    // printf("Consumer done may be stuck\n");
	}
	Condition_Unlock((*(agent->packetring->awake_producer)));
#ifdef HAVE_THREAD_DEBUG
	thread_debug( "Traffic thread thinks reporter is done with %p", (void *)agent);
#endif
	FreeReport(agent);
#ifndef HAVE_THREAD
        /*
         * Process the report in this thread
         */
        process_report ( agent );
#endif
    }
}

/*
 * GetReport is called by the agent after a CloseReport
 * to get the final stats generated by the reporterthread
 * so make sure the reporter thread is indeed done
 */
struct TransferInfo *GetReport( struct ReportHeader *agent ) {
    struct TransferInfo *final = NULL;
    if ( agent != NULL ) {
        EndReport(agent);
        final = &agent->report.info;
    }
    return final;
}

/*
 * ReportSettings will generate a summary report for
 * settings being used with Listeners or Clients
 */
struct ReportHeader *ReportSettings( struct thread_Settings *agent ) {
    struct ReportHeader *reporthdr = NULL;
    if ( isSettingsReport( agent ) ) {
	/*
	 * Populate and create a new settings report
	 */
	if ((reporthdr = ( struct ReportHeader *) calloc(sizeof(struct ReportHeader), sizeof(char*)))) {
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Init settings report %p", reporthdr);
#endif
	    struct ReporterData *data = &reporthdr->report;
	    data->info.transferID = agent->mSock;
	    data->info.groupID = -1;
	    data->mHost = agent->mHost;
	    data->mLocalhost = agent->mLocalhost;
	    data->mSSMMulticastStr = agent->mSSMMulticastStr;
	    data->mIfrname = agent->mIfrname;
	    data->mIfrnametx = agent->mIfrnametx;
	    data->mode = agent->mReportMode;
	    data->type = SETTINGS_REPORT;
	    data->mBufLen = agent->mBufLen;
	    data->mMSS = agent->mMSS;
	    data->mTCPWin = agent->mTCPWin;
	    data->FQPacingRate = agent->mFQPacingRate;
	    data->flags = agent->flags;
	    data->flags_extend = agent->flags_extend;
	    data->mThreadMode = agent->mThreadMode;
	    data->mPort = agent->mPort;
	    data->info.mFormat = agent->mFormat;
	    data->info.mTTL = agent->mTTL;
	    data->connection.peer = agent->peer;
	    data->connection.size_peer = agent->size_peer;
	    data->connection.local = agent->local;
	    data->connection.size_local = agent->size_local;
	    data->mUDPRate = agent->mUDPRate;
	    data->mUDPRateUnits = agent->mUDPRateUnits;
	    if (isIsochronous(data)) {
		data->isochstats.mFPS = agent->mFPS;
		data->isochstats.mMean = agent->mMean/8;
		data->isochstats.mVariance = agent->mVariance/8;
		data->isochstats.mBurstIPG = (unsigned int) (agent->mBurstIPG*1000.0);
		data->isochstats.mBurstInterval = (unsigned int) (1 / agent->mFPS * 1000000);
	    }
	} else {
	    FAIL(1, "Out of Memory!!\n", agent);
	}
    }
    return reporthdr;
}

/*
 * ReportServerUDP will generate a report of the UDP
 * statistics as reported by the server on the client
 * side.
 */
void ReportServerUDP( struct thread_Settings *agent, struct server_hdr *server ) {
    unsigned int flags = ntohl(server->base.flags);
    // printf("Server flags = 0x%X\n", flags);
    if (isServerReport(agent) && ((flags & HEADER_VERSION1) != 0)) {
	/*
	 * Create in one big chunk
	 */
	struct ReportHeader *reporthdr = calloc( sizeof(struct ReportHeader), sizeof(char*));
	struct TransferInfo *stats = &reporthdr->report.info;

	if ( !reporthdr ) {
	    FAIL(1, "Out of Memory!!\n", agent);
	}
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Init server relay report %p size %ld", (void *)reporthdr, sizeof(struct ReportHeader));
#endif

	stats->transferID = agent->mSock;
	stats->groupID = (agent->multihdr != NULL ? agent->multihdr->groupID \
			  : -1);

	reporthdr->report.type = SERVER_RELAY_REPORT;
	reporthdr->report.mode = agent->mReportMode;
	stats->mFormat = agent->mFormat;
	stats->jitter = ntohl( server->base.jitter1 );
	stats->jitter += ntohl( server->base.jitter2 ) / (double)rMillion;
#ifdef HAVE_INT64_T
	stats->TotalLen = (((intmax_t) ntohl( server->base.total_len1 )) << 32) + \
	    ntohl( server->base.total_len2 );
#else
	stats->TotalLen = (intmax_t) ntohl(server->base.total_len2);
#endif
	stats->startTime = 0;
	stats->endTime = ntohl( server->base.stop_sec );
	stats->endTime += ntohl( server->base.stop_usec ) / (double)rMillion;
	if ((flags & HEADER_SEQNO64B)) {
	  stats->cntError = (((intmax_t) ntohl( server->extend2.error_cnt2 )) << 32) + \
	    ntohl( server->base.error_cnt );
	  stats->cntOutofOrder = (((intmax_t) ntohl( server->extend2.outorder_cnt2 )) << 32) + \
	    ntohl( server->base.outorder_cnt );
	  stats->cntDatagrams = (((intmax_t) ntohl( server->extend2.datagrams2 )) << 32) + \
	    ntohl( server->base.datagrams );
	} else {
	  stats->cntError  = ntohl( server->base.error_cnt );
	  stats->cntOutofOrder = ntohl( server->base.outorder_cnt );
	  stats->cntDatagrams = ntohl( server->base.datagrams );
	}

	if ((flags & HEADER_EXTEND) != 0) {
	    stats->mEnhanced = 1;
	    stats->transit.minTransit = ntohl( server->extend.minTransit1 );
	    stats->transit.minTransit += ntohl( server->extend.minTransit2 ) / (double)rMillion;
	    stats->transit.maxTransit = ntohl( server->extend.maxTransit1 );
	    stats->transit.maxTransit += ntohl( server->extend.maxTransit2 ) / (double)rMillion;
	    stats->transit.sumTransit = ntohl( server->extend.sumTransit1 );
	    stats->transit.sumTransit += ntohl( server->extend.sumTransit2 ) / (double)rMillion;
	    stats->transit.meanTransit = ntohl( server->extend.meanTransit1 );
	    stats->transit.meanTransit += ntohl( server->extend.meanTransit2 ) / (double)rMillion;
	    stats->transit.m2Transit = ntohl( server->extend.m2Transit1 );
	    stats->transit.m2Transit += ntohl( server->extend.m2Transit2 ) / (double)rMillion;
	    stats->transit.vdTransit = ntohl( server->extend.vdTransit1 );
	    stats->transit.vdTransit += ntohl( server->extend.vdTransit2 ) / (double)rMillion;
	    stats->transit.cntTransit = ntohl( server->extend.cntTransit );
	    stats->IPGcnt = ntohl( server->extend.IPGcnt );
	    stats->IPGsum = ntohl( server->extend.IPGsum );
	}
	stats->mUDP = (char)kMode_Server;
	reporthdr->report.connection.peer = agent->local;
	reporthdr->report.connection.size_peer = agent->size_local;
	reporthdr->report.connection.local = agent->peer;
	reporthdr->report.connection.size_local = agent->size_peer;

#ifdef HAVE_THREAD
	PostReport(reporthdr);
#else
	/*
	 * Process the report in this thread
	 */
	reporthdr->next = NULL;
	process_report ( reporthdr );
#endif
    }
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

static inline void reset_consumption_detector(void) {
    consumption_detector.accounted_packet_threads = thread_numtrafficthreads();
    if ((consumption_detector.accounted_packets = thread_numtrafficthreads() * MINPERQUEUEDEPTH) <= MINPACKETDEPTH) {
	consumption_detector.accounted_packets = MINPACKETDEPTH;
    }
}
static inline void apply_consumption_detector(void) {
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

static void reporter_jobq_free_entry (struct ReportHeader *entry) {
    // Next, free it's memory either directly or indirectly
    // by signaling the traffic thread to do so
    if ((entry->report.type & TRANSFER_REPORT) == 0) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Free report %p (flags = %X)", (void *) entry, entry->report.type);
#endif
	free(entry);
    } else if ((entry->report.type & (TRANSFER_REPORT | CONNECTION_REPORT)) == TRANSFER_REPORT) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Signal producer to free report %p and cond %p", \
		   (void *) entry, (void *) &(entry->packetring->awake_producer));
#endif
        // Thread is done with the packet ring, signal back to the traffic thread
        // which will proceed from the EndReport wait, this must be the last thing done
        Condition_Lock((*(entry->packetring->awake_producer)));
        entry->packetring->consumerdone = 1;
        Condition_Unlock((*(entry->packetring->awake_producer)));
        Condition_Signal(entry->packetring->awake_producer);
    }
}

/* Concatenate pending reports and return the head */
static inline struct ReportHeader *reporter_jobq_set_root(void) {
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
	    thread_debug( "Jobq *WAIT* exit  %p/%p", (void *) ReportRoot, (void *) ReportPendingHead);
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
    // reporter is now running.  This is needed because
    // the client's traffic thread has a connect() within
    // it's constructor and that connect gets reported via
    // via this thread so let this thread go first
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
		// from the jobq
	        if (reporter_process_report(*work_item, thread)) {
		    struct ReportHeader *tmp = *work_item;
		    *work_item = (*work_item)->next;
#ifdef HAVE_THREAD_DEBUG
		    thread_debug( "Jobq *FREE* %p (%X) (%p) cr=%p", (void *) tmp, tmp->report.type,(void *) *work_item, thread->multihdr);
#endif
		    reporter_jobq_free_entry(tmp);
		    if (!(*work_item))
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
	free(thread->multihdr);
	FreeReport(thread->reporthdr);
    }
#ifdef HAVE_THREAD_DEBUG
    if (sInterupted)
        reporter_jobq_dump();
    thread_debug("Reporter thread finished");
#endif
}

/*
 * Used for single threaded reporting
 */
void process_report ( struct ReportHeader *report ) {
    if ( report != NULL ) {
      if ( reporter_process_report(report, NULL) ) {
	    if (report->report.info.latency_histogram) {
		histogram_delete(report->report.info.latency_histogram);
	    }
	    if (report->report.info.framelatency_histogram) {
		histogram_delete(report->report.info.framelatency_histogram);
	    }
            free( report );
        }
    }
}

static int reporter_condprint_time_interval_report (struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    int advance_jobq = 0;
    struct ReporterData *stats = &reporthdr->report;
    struct ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
    struct ReporterData *bidirstats = (reporthdr->bidirreport ? &reporthdr->bidirreport->report : NULL);

    // Print a report if packet time exceeds the next report interval time,
    // Also signal to the caller to move to the next report (or packet ring)
    // if there was output. This will allow for more precise interval sum accounting.
    if (TimeDifference(reporthdr->report.nextTime, packet->packetTime) < 0) {
	stats->packetTime = packet->packetTime;
#ifdef DEBUG_PPS
	printf("*** packetID TRIGGER = %ld pt=%ld.%ld empty=%d nt=%ld.%ld\n",packet->packetID, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->emptyreport, reporthdr->report.nextTime.tv_sec, reporthdr->report.nextTime.tv_usec);
#endif
        // In the (hopefully unlikely event) the reporter fell behind
        // ouput the missed reports to catch up
	reporter_set_timestamps_time(stats, INTERVAL);
	(*reporthdr->transfer_protocol_handler)(&reporthdr->report, sumstats, bidirstats, 0);
	if (reporthdr->transfer_interval_handler) {
	    reporter_transfer_protocol_reports(&reporthdr->report, packet);
	}
	if (reporthdr->multireport) {
	    advance_jobq = 1;
	    reporthdr->multireport->threads++;
	}
	if (reporthdr->bidirreport) {
	    advance_jobq = 1;
	    reporthdr->bidirreport->threads++;
	}
    }
    if (reporthdr->bidirreport && (reporthdr->bidirreport->refcount > 1) && \
	(reporthdr->bidirreport->threads == reporthdr->bidirreport->refcount)) {
	reporthdr->bidirreport->threads = 0;
	reporter_set_timestamps_time(bidirstats, INTERVAL);
	(*reporthdr->bidirreport->transfer_protocol_sum_handler)(&reporthdr->bidirreport->report, 0);
    }
    if (reporthdr->multireport && (reporthdr->multireport->reference.count > (reporthdr->bidirreport ? 2 : 1)) && \
	(reporthdr->multireport->threads == reporthdr->multireport->refcount))  {
	reporthdr->multireport->threads = 0;
	reporter_set_timestamps_time(sumstats, INTERVAL);
	(*reporthdr->multireport->transfer_protocol_sum_handler)(&reporthdr->multireport->report, 0);
    }
    return advance_jobq;
}

static int reporter_condprint_packet_interval_report (struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    int advance_jobq = 0;
    printf("packet reporting not done\n");
    // Print a report if packet time exceeds the next report interval time,
    // Also signal to the caller to move to the next report (or packet ring)
    // if there was output. This will allow for more precise interval sum accounting.
    if ((packet->packetID - reporthdr->report.lastDatagrams) >= 500) {
	struct ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
	struct ReporterData *bidirstats = (reporthdr->bidirreport ? &reporthdr->bidirreport->report : NULL);
	(*reporthdr->transfer_protocol_handler)(&reporthdr->report, sumstats, bidirstats, 0);
	TimeAdd(reporthdr->report.nextTime, reporthdr->report.intervalTime);
	if (reporthdr->multireport) {
	    advance_jobq = 1;
	    reporthdr->multireport->threads++;
	}
	if (reporthdr->bidirreport) {
	    advance_jobq = 1;
	    reporthdr->bidirreport->threads++;
	}
    }
    if (reporthdr->bidirreport && (reporthdr->bidirreport->refcount > 1) && \
	(reporthdr->bidirreport->threads == reporthdr->bidirreport->refcount)) {
	reporthdr->bidirreport->threads = 0;
	// transfer_protocol_multireports(&reporthdr->multireport->report, packet);
	(*reporthdr->bidirreport->transfer_protocol_sum_handler)(&reporthdr->bidirreport->report, 0);
	TimeAdd(reporthdr->bidirreport->report.nextTime, reporthdr->report.intervalTime);
    }
    if (reporthdr->multireport && (reporthdr->multireport->reference.count > (reporthdr->bidirreport ? 2 : 1)) && \
	(reporthdr->multireport->threads == reporthdr->multireport->reference.count))  {
	reporthdr->multireport->threads = 0;
	(*reporthdr->multireport->transfer_protocol_sum_handler)(&reporthdr->multireport->report, 0);
	TimeAdd(reporthdr->multireport->report.nextTime, reporthdr->report.intervalTime);
    }
    return advance_jobq;
}

static int reporter_condprint_frame_interval_report_udp (struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    int rc = 0;
    struct ReporterData *stats = &reporthdr->report;
    // first packet of a burst and not a duplicate
    assert(packet->burstsize != 0);
    if ((packet->burstsize == (packet->remaining + packet->packetLen)) && (stats->matchframeID != packet->frameID)) {
	reporthdr->report.matchframeID=packet->frameID;
	if (isTripTime(stats))
	    stats->nextTime = packet->sentTime;
	else
	    stats->nextTime = packet->packetTime;
    }
    if ((packet->packetLen == packet->remaining) && (packet->frameID == stats->matchframeID)) {
	if ((stats->info.startTime = TimeDifference(stats->nextTime, stats->startTime)) < 0)
	    stats->info.startTime = 0.0;
	stats->info.frameID = packet->frameID;
	stats->info.endTime = TimeDifference(packet->packetTime, stats->startTime);
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	stats->info.cntOutofOrder = stats->cntOutofOrder - stats->lastOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->info.cntError = stats->cntError - stats->lastError;
	stats->info.cntError -= stats->info.cntOutofOrder;
	stats->info.cntDatagrams = stats->PacketID - stats->lastDatagrams;
	reporter_print(stats, TRANSFER_FRAMEREPORTUDP, 0);
	reporter_reset_transfer_stats_server_udp(stats);
	rc = 1;
    }
    return rc;
}

static int reporter_condprint_frame_interval_report_tcp (struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    int rc = 0;
    assert(packet->burstsize != 0);
    struct ReporterData *stats = &reporthdr->report;
    // first packet of a burst and not a duplicate
    if ((packet->burstsize == (packet->remaining + packet->packetLen)) && (stats->matchframeID != packet->frameID)) {
	reporthdr->report.matchframeID=packet->frameID;
	if (isTripTime(stats))
	    stats->nextTime = packet->sentTime;
	else
	    stats->nextTime = packet->packetTime;
    }
    if ((packet->packetLen == packet->remaining) && (packet->frameID == stats->matchframeID)) {
	if ((stats->info.startTime = TimeDifference(stats->nextTime, stats->startTime)) < 0)
	    stats->info.startTime = 0.0;
	stats->info.frameID = packet->frameID;
	stats->info.endTime = TimeDifference(packet->packetTime, stats->startTime);
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	reporter_print(stats, TRANSFER_FRAMEREPORTTCP, 0);
	reporter_reset_transfer_stats_server_tcp(stats);
	rc = 1;
    }
    return rc;
}

static void reporter_compute_connect_times (struct ReportHeader *hdr, double connect_time) {
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
}

/*
 * Process reports starting with "reporthdr"
 */
static int reporter_process_report (struct ReportHeader *reporthdr, struct thread_Settings *mSettings) {
    int need_free = 1;
    // report.type is a bit field which indicates the reports requested,
    // note the special case for a Transfer interval and Connection report
    // which are "compound reports"
    if (reporthdr->report.type) {
	// This code works but is a mess - fix this and use a proper dispatcher
	// for updating reports and for outputing them
	if ((reporthdr->report.type & SETTINGS_REPORT) != 0 ) {
	    reporthdr->report.type &= ~SETTINGS_REPORT;
	    reporter_print( &reporthdr->report, SETTINGS_REPORT, need_free );
	} else if ((reporthdr->report.type & CONNECTION_REPORT) != 0) {
	    reporthdr->report.type &= ~CONNECTION_REPORT;
	    need_free = (reporthdr->report.type == 0 ? 1 : 0);
	    if (mSettings->reporthdr) {
	        reporter_compute_connect_times(mSettings->reporthdr, reporthdr->report.connection.connecttime);
	    }
	    reporter_print(&reporthdr->report, CONNECTION_REPORT, need_free);
	} else if ( (reporthdr->report.type & SERVER_RELAY_REPORT) != 0 ) {
	    reporthdr->report.type &= ~SERVER_RELAY_REPORT;
	    reporter_print( &reporthdr->report, SERVER_RELAY_REPORT, need_free);
	}
	if ((reporthdr->report.type & TRANSFER_REPORT) != 0) {
	    need_free = 0;
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
	    while (!advance_jobq && (packet = packetring_dequeue(reporthdr->packetring))) {
		// Check for a very first reported packet that needs to be summed
		// This has to be done in the reporter thread as these
		// reports are shared by multiple traffic threads
		// Note: the first reported packet may not have the earliest
		// timestamp but it should be good enough
		if (reporthdr->multireport && TimeZero(reporthdr->multireport->report.startTime)) {
		    reporthdr->multireport->report.startTime = reporthdr->report.startTime;
		    reporthdr->multireport->report.nextTime = reporthdr->report.nextTime;
		    reporthdr->multireport->report.packetTime = packet->packetTime;
		}
		if (reporthdr->bidirreport && TimeZero(reporthdr->bidirreport->report.startTime)) {
		    reporthdr->bidirreport->report.startTime = reporthdr->report.startTime;
		    reporthdr->bidirreport->report.nextTime = reporthdr->report.nextTime;
		    reporthdr->bidirreport->report.packetTime = packet->packetTime;
		}
		// Increment the total packet count processed by this thread
		// this will be used to make decisions on if the reporter
		// thread should add some delay to eliminate cpu thread
		// thrashing,
		consumption_detector.accounted_packets--;
		// Check against a final packet event on this packet ring
		if (!(packet->packetID < 0)) {
		    // Check to output any interval reports, do this prior
		    // to packet handling to preserve interval accounting
		    if (reporthdr->transfer_interval_handler) {
		        if (!packet->emptyreport)
			    // Stash this last timestamp away for calculations that need it, e.g. packet interval reporting
			    reporthdr->report.prevpacketTime = reporthdr->report.IPGstart;
			advance_jobq = (*reporthdr->transfer_interval_handler)(reporthdr, packet);
		    }
		    // Do the packet accounting per the handler type
		    if (reporthdr->packet_handler) {
			(*reporthdr->packet_handler)(reporthdr, packet);
			// Sum reports update the report header's last
			// packet time after the handler. This means
			// the report header's packet time will be
			// the previous time before the interval
			if (reporthdr->multireport)
			    reporthdr->multireport->report.packetTime = packet->packetTime;
			if (reporthdr->bidirreport)
			    reporthdr->bidirreport->report.packetTime = packet->packetTime;
		    }
		    // Transfer reports per interval reporting stay around until the final report
		} else {
		    need_free = 1;
		    advance_jobq = 1;
		    // A last packet event was detected
		    // printf("last packet event detected\n"); fflush(stdout);
		    reporthdr->reporter_thread_suspends = consumption_detector.reporter_thread_suspends;
		    if (reporthdr->packet_handler) {
			(*reporthdr->packet_handler)(reporthdr, packet);
			struct ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
			struct ReporterData *bidirstats = (reporthdr->bidirreport ? &reporthdr->bidirreport->report : NULL);
			reporthdr->report.packetTime = packet->packetTime;
			(*reporthdr->transfer_protocol_handler)(&reporthdr->report, sumstats, bidirstats, 1);
			// This is a final report so set the sum report header's packet time
			// Note, the thread with the max value will set this
			// Also note, the final sum report output occurs as part of freeing the
			// sum or bidir report per the last reference and not here
			if (reporthdr->bidirreport) {
			    if (TimeDifference(reporthdr->bidirreport->report.packetTime, packet->packetTime) > 0) {
				reporthdr->bidirreport->report.packetTime = packet->packetTime;
			    }
			    if (UpdateMultiHdrRefCounter(reporthdr->bidirreport, -1, reporthdr->bidirreport->sockfd)) {
				if (reporthdr->bidirreport->transfer_protocol_sum_handler) {
				    (*reporthdr->bidirreport->transfer_protocol_sum_handler)(&reporthdr->bidirreport->report, 1);
				}
				FreeMultiReport(reporthdr->bidirreport);
			    }
			}
			if (reporthdr->multireport) {
			    if (TimeDifference(reporthdr->multireport->report.packetTime, packet->packetTime) > 0) {
				reporthdr->multireport->report.packetTime = packet->packetTime;
			    }
			    UnbindSumReport(reporthdr->multireport);
			    if ((reporthdr->multireport->transfer_protocol_sum_handler) && \
				(reporthdr->multireport->reference.count == 0) && (reporthdr->multireport->reference.maxcount > 1)) {
				(*reporthdr->multireport->transfer_protocol_sum_handler)(&reporthdr->multireport->report, 1);
				FreeMultiReport(multihdr);
			    }
			}
		    }
		}
	    }
	}
    }
#ifdef HAVE_THREAD_DEBUG
    // thread_debug("Processed report %p (flags=%x) free=%d", (void *)reporthdr, reporthdr->report.type, need_free);
#endif

    // need_free is a poor implementation.  It's done this way
    // because of recursion in the original design.  It also signals a few things,
    // one is remove from the reporter's job queue, two s is to free the report's
    // memory which may have been dynamically allocated
    // by another thread and three is to flag a final report to the print routine.
    // This is a good thing to fix with a c++ version of the reporter
    return need_free;
}

/*
 * Updates connection stats
 */
#define L2DROPFILTERCOUNTER 100

static inline void reporter_handle_packet_pps(struct ReporterData *data, struct TransferInfo *stats, struct ReportStruct *packet) {
    if (!packet->emptyreport) {
        data->cntDatagrams++;
        stats->IPGcnt++;
        stats->IPGcnttot++;
    }
    stats->IPGsum += TimeDifference(packet->packetTime, data->IPGstart);
    data->IPGstart = packet->packetTime;
    if (!TimeZero(packet->prevSentTime)) {
        double delta = TimeDifference(packet->sentTime, packet->prevSentTime);
        stats->arrivalSum += delta;
        stats->totarrivalSum += delta;
    }
#ifdef DEBUG_PPS
    printf("*** IPGsum = %f cnt=%ld ipg=%ld.%ld pt=%ld.%ld id=%ld empty=%d\n", stats->IPGsum, stats->IPGcnt, data->IPGstart.tv_sec, data->IPGstart.tv_usec, packet->packetTime.tv_sec, packet->packetTime.tv_usec, packet->packetID, packet->emptyreport);
#endif
}

static inline double reporter_handle_packet_oneway_transit(struct ReporterData *data, struct TransferInfo *stats, struct ReportStruct *packet) {
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
	if ( deltaTransit < 0.0 ) {
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

static inline void reporter_handle_burst_tcp_transit(struct ReporterData *data, struct TransferInfo *stats, struct ReportStruct *packet) {
    if (packet->frameID && packet->transit_ready) {
        double transit = reporter_handle_packet_oneway_transit(data, stats, packet);

	if (!TimeZero(packet->prevSentTime)) {
	    double delta = TimeDifference(packet->sentTime, packet->prevSentTime);
	    stats->arrivalSum += delta;
	    stats->totarrivalSum += delta;
	}

	if (stats->framelatency_histogram) {
	  histogram_insert(stats->framelatency_histogram, transit, isTripTime(data) ? &packet->sentTime : NULL);
	}
       // printf("***Burst id = %ld, transit = %f\n", packet->frameID, stats->transit.lastTransit);
    }
}

static inline void reporter_handle_packet_isochronous(struct ReporterData *data, struct TransferInfo *stats, struct ReportStruct *packet) {
    // printf("fid=%lu bs=%lu remain=%lu\n", packet->frameID, packet->burstsize, packet->remaining);
    if (packet->frameID && packet->burstsize && packet->remaining) {
	int framedelta=0;
	// very first isochronous frame
	if (!data->isochstats.frameID) {
	    data->isochstats.framecnt=packet->frameID;
	    data->isochstats.framecnt=1;
	    stats->isochstats.framecnt=1;
	}
	// perform client and server frame based accounting
	if ((framedelta = (packet->frameID - data->isochstats.frameID))) {
	    data->isochstats.framecnt++;
	    stats->isochstats.framecnt++;
	    if (framedelta > 1) {
		if (stats->mUDP == kMode_Server) {
		    int lost = framedelta - (packet->frameID - packet->prevframeID);
		    stats->isochstats.framelostcnt += lost;
		    data->isochstats.framelostcnt += lost;
		} else {
		    stats->isochstats.framelostcnt += (framedelta-1);
		    data->isochstats.framelostcnt += (framedelta-1);
		    stats->isochstats.slipcnt++;
		    data->isochstats.slipcnt++;
		}
	    }
	}
	// peform frame latency checks
	if (stats->framelatency_histogram) {
	    static int matchframeid=0;
	    // first packet of a burst and not a duplicate
	    if ((packet->burstsize == packet->remaining) && (matchframeid!=packet->frameID)) {
		matchframeid=packet->frameID;
	    }
	    if ((packet->packetLen == packet->remaining) && (packet->frameID == matchframeid)) {
		// last packet of a burst (or first-last in case of a duplicate) and frame id match
		double frametransit = TimeDifference(packet->packetTime, packet->isochStartTime) \
		    - ((packet->burstperiod * (packet->frameID - 1)) / 1000000.0);
		histogram_insert(stats->framelatency_histogram, frametransit, NULL);
		matchframeid = 0;  // reset the matchid so any potential duplicate is ignored
	    }
	}
	data->isochstats.frameID = packet->frameID;
    }
}

inline void reporter_handle_packet_server_tcp(struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    struct TransferInfo *stats = &reporthdr->report.info;
    if (packet->packetLen > 0) {
	int bin;
	reporthdr->report.TotalLen += packet->packetLen;
	// mean min max tests
	stats->sock_callstats.read.cntRead++;
	stats->sock_callstats.read.totcntRead++;
	bin = (int)floor((packet->packetLen -1)/stats->sock_callstats.read.binsize);
	if (bin < TCPREADBINCOUNT) {
	    stats->sock_callstats.read.bins[bin]++;
	    stats->sock_callstats.read.totbins[bin]++;
	}
	reporter_handle_burst_tcp_transit(&reporthdr->report, stats, packet);
    }
}

inline void reporter_handle_packet_server_udp(struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    struct ReporterData *data = &reporthdr->report;
    struct TransferInfo *stats = &reporthdr->report.info;

    data->packetTime = packet->packetTime;
    stats->socket = packet->socket;

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
	reporthdr->report.TotalLen += packet->packetLen;
	// These are valid packets that need standard iperf accounting
	// Do L2 accounting first (if needed)
	if (packet->l2errors && (data->cntDatagrams > L2DROPFILTERCOUNTER)) {
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
	if ( packet->packetID != data->PacketID + 1 ) {
	    if (packet->packetID < data->PacketID + 1 ) {
		data->cntOutofOrder++;
	    } else {
		data->cntError += packet->packetID - data->PacketID - 1;
	    }
	}
	// never decrease datagramID (e.g. if we get an out-of-order packet)
	if ( packet->packetID > data->PacketID ) {
	    data->PacketID = packet->packetID;
	}
	reporter_handle_packet_pps(data, stats, packet);
	reporter_handle_packet_oneway_transit(data, stats, packet);
	reporter_handle_packet_isochronous(data, stats, packet);
    }
}

void reporter_handle_packet_client(struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    struct ReporterData *data = &reporthdr->report;
    struct TransferInfo *stats = &reporthdr->report.info;

    reporthdr->report.TotalLen += packet->packetLen;
    data->packetTime = packet->packetTime;
    stats->socket = packet->socket;
    if (!packet->emptyreport) {
        if (packet->errwrite && (packet->errwrite != WriteErrNoAccount)) {
	    stats->sock_callstats.write.WriteErr++;
	    stats->sock_callstats.write.totWriteErr++;
	}
	// These are valid packets that need standard iperf accounting
	stats->sock_callstats.write.WriteCnt++;
	stats->sock_callstats.write.totWriteCnt++;
	if (isUDP(data)) {
	    if (packet->packetID > 0)
		data->PacketID = packet->packetID;
	    reporter_handle_packet_pps(data, stats, packet);
	    reporter_handle_packet_isochronous(data, stats, packet);
	}
    }
}


#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats (struct ReporterData *stats, struct ReporterData *sumstats, int final) {
    static int cnt = 0;
    struct tcp_info tcp_internal;
    socklen_t tcp_info_length = sizeof(struct tcp_info);
    int retry = 0;
    // Read the TCP retry stats for a client.  Do this
    // on  a report interval period.
    int rc = (stats->info.socket==INVALID_SOCKET) ? 0 : 1;
    if (rc) {
        rc = (getsockopt(stats->info.socket, IPPROTO_TCP, TCP_INFO, &tcp_internal, &tcp_info_length) < 0) ? 0 : 1;
	if (!rc)
	    stats->info.socket = INVALID_SOCKET;
	else
	    // Mark stale now so next call at report interval will update
	    stats->info.sock_callstats.write.up_to_date = 1;
    }
    if (!rc) {
        stats->info.sock_callstats.write.TCPretry = 0;
	stats->info.sock_callstats.write.cwnd = -1;
	stats->info.sock_callstats.write.rtt = 0;
    } else {
        retry = tcp_internal.tcpi_total_retrans - stats->info.sock_callstats.write.lastTCPretry;
	stats->info.sock_callstats.write.TCPretry = retry;
	stats->info.sock_callstats.write.totTCPretry += retry;
	stats->info.sock_callstats.write.lastTCPretry = tcp_internal.tcpi_total_retrans;
	stats->info.sock_callstats.write.cwnd = tcp_internal.tcpi_snd_cwnd * tcp_internal.tcpi_snd_mss / 1024;
	stats->info.sock_callstats.write.rtt = tcp_internal.tcpi_rtt;
	// New average = old average * (n-1)/n + new value/n
	cnt++;
	stats->info.sock_callstats.write.meanrtt = (stats->info.sock_callstats.write.meanrtt * ((double) (cnt - 1) / (double) cnt)) + ((double) (tcp_internal.tcpi_rtt) / (double) cnt);
	stats->info.sock_callstats.write.rtt = tcp_internal.tcpi_rtt;
	if (sumstats) {
	    sumstats->info.sock_callstats.write.TCPretry += retry;
	    sumstats->info.sock_callstats.write.totTCPretry += retry;
	}
    }
    if (final) {
        stats->info.sock_callstats.write.rtt = stats->info.sock_callstats.write.meanrtt;
    }
}
#endif
/*
 * Report printing routines below
 */

// If reports were missed, catch up now
static inline void reporter_transfer_protocol_reports(struct ReporterData *stats, struct ReportStruct *packet) {
  while (TimeDifference(stats->nextTime, packet->packetTime) < 0) {
      reporter_set_timestamps_time(stats, INTERVAL);
      struct ReporterData emptystats;
      memset(&emptystats, 0, sizeof(struct ReporterData));
      emptystats.info.startTime = stats->info.startTime;
      emptystats.info.endTime = stats->info.endTime;
      emptystats.info.mFormat = stats->info.mFormat;
      emptystats.info.mTCP = stats->info.mTCP;
      emptystats.info.mUDP = stats->info.mUDP;
      emptystats.info.mIsochronous = stats->info.mIsochronous;
      emptystats.info.mEnhanced = stats->info.mEnhanced;
      emptystats.info.transferID = stats->info.transferID;
      emptystats.info.groupID = stats->info.groupID;
      reporter_print( &emptystats, TRANSFER_REPORT, 0);
    }
}
// If reports were missed, catch up now
static inline void reporter_transfer_protocol_multireports(struct ReporterData *stats, struct ReportStruct *packet) {
    reporter_transfer_protocol_reports(stats, packet);
}

static inline void reporter_set_timestamps_time(struct ReporterData *stats, enum TimestampType tstype) {
    // There is a corner case when the first packet is also the last where the start time (which comes
    // from app level syscall) is greater than the packetTime (which come for kernel level SO_TIMESTAMP)
    // For this case set the start and end time to both zero.
    if (TimeDifference(stats->packetTime, stats->startTime) < 0) {
	stats->info.endTime = 0;
	stats->info.startTime = 0;
    } else {
	switch (tstype) {
	case INTERVAL:
	    stats->info.startTime = stats->info.endTime;
	    stats->info.endTime = TimeDifference(stats->nextTime, stats->startTime);
	    TimeAdd(stats->nextTime, stats->intervalTime);
	    break;
	case TOTAL:
	    stats->info.startTime = 0;
	    stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
	    break;
	case FINALPARTIAL:
	    stats->info.startTime = stats->info.endTime;
	    stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
	    break;
	default:
	    break;
	}
    }
}

// Actions required after an interval report has been outputted
static inline void reporter_reset_transfer_stats(struct ReporterData *stats) {
    stats->lastOutofOrder = stats->cntOutofOrder;
    if (stats->info.cntError < 0) {
	stats->info.cntError = 0;
    }
    stats->lastError = stats->cntError;
    stats->lastDatagrams = ((stats->info.mUDP == kMode_Server) ? stats->PacketID : stats->cntDatagrams);
    stats->lastTotal = stats->TotalLen;
    /*
     * Reset transfer stats now that both the individual and SUM reports
     * have completed
     */
    if (stats->info.mUDP) {
	stats->info.IPGcnt = 0;
	stats->info.IPGsum = 0;
	if (stats->info.mUDP == kMode_Server) {
	    stats->info.l2counts.cnt = 0;
	    stats->info.l2counts.unknown = 0;
	    stats->info.l2counts.udpcsumerr = 0;
	    stats->info.l2counts.lengtherr = 0;
	}
    }
    if (stats->info.mEnhanced) {
	if ((stats->info.mTCP == (char)kMode_Client) || (stats->info.mUDP == (char)kMode_Client)) {
	    stats->info.sock_callstats.write.WriteCnt = 0;
	    stats->info.sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
	    stats->info.sock_callstats.write.TCPretry = 0;
	    stats->info.sock_callstats.write.up_to_date = 0;
#endif
	} else if (stats->info.mTCP == (char)kMode_Server) {
	    int ix;
	    stats->info.sock_callstats.read.cntRead = 0;
	    for (ix = 0; ix < 8; ix++) {
		stats->info.sock_callstats.read.bins[ix] = 0;
	    }
	}
    // Reset the enhanced stats for the next report interval
	if (stats->info.mUDP) {
	    stats->info.transit.minTransit=stats->info.transit.lastTransit;
	    stats->info.transit.maxTransit=stats->info.transit.lastTransit;
	    stats->info.transit.sumTransit = stats->info.transit.lastTransit;
	    stats->info.transit.cntTransit = 0;
	    stats->info.transit.vdTransit = 0;
	    stats->info.transit.meanTransit = 0;
	    stats->info.transit.m2Transit = 0;
	    stats->info.isochstats.framecnt = 0;
	    stats->info.isochstats.framelostcnt = 0;
	    stats->info.isochstats.slipcnt = 0;
	}
    }
}

static inline void reporter_reset_transfer_stats_bidir(struct ReporterData *stats) {
    stats->lastTotal = stats->TotalLen;
}
static inline void reporter_reset_transfer_stats_client_tcp(struct ReporterData *stats) {
    stats->lastTotal = stats->TotalLen;
    stats->info.sock_callstats.write.WriteCnt = 0;
    stats->info.sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    stats->info.sock_callstats.write.TCPretry = 0;
    stats->info.sock_callstats.write.up_to_date = 0;
#endif
}
static inline void reporter_reset_transfer_stats_client_udp(struct ReporterData *stats) {
    if (stats->info.cntError < 0) {
	stats->info.cntError = 0;
    }
    stats->lastError = stats->cntError;
    stats->lastDatagrams = stats->PacketID;
    stats->lastTotal = stats->TotalLen;
    stats->info.sock_callstats.write.WriteCnt = 0;
    stats->info.sock_callstats.write.WriteErr = 0;
    stats->info.isochstats.framecnt = 0;
    stats->info.isochstats.framelostcnt = 0;
    stats->info.isochstats.slipcnt = 0;
    if (stats->info.TotalLen) {
        stats->info.IPGcnt = 0;
        stats->info.IPGsum = 0;
    }
}
static inline void reporter_reset_transfer_stats_server_tcp(struct ReporterData *stats) {
    int ix;
    stats->lastTotal = stats->TotalLen;
    stats->info.sock_callstats.read.cntRead = 0;
    for (ix = 0; ix < 8; ix++) {
	stats->info.sock_callstats.read.bins[ix] = 0;
    }
    stats->info.transit.minTransit=stats->info.transit.lastTransit;
    stats->info.transit.maxTransit=stats->info.transit.lastTransit;
    stats->info.transit.sumTransit = stats->info.transit.lastTransit;
    stats->info.transit.cntTransit = 0;
    stats->info.transit.vdTransit = 0;
    stats->info.transit.meanTransit = 0;
    stats->info.transit.m2Transit = 0;
    stats->info.arrivalSum = 0;
}
static inline void reporter_reset_transfer_stats_server_udp(struct ReporterData *stats) {
    // Reset the enhanced stats for the next report interval
    stats->lastTotal = stats->TotalLen;
    stats->lastDatagrams = stats->cntDatagrams;
    stats->lastOutofOrder = stats->cntOutofOrder;
    stats->lastError = stats->cntError;
    stats->info.transit.minTransit=stats->info.transit.lastTransit;
    stats->info.transit.maxTransit=stats->info.transit.lastTransit;
    stats->info.transit.sumTransit = stats->info.transit.lastTransit;
    stats->info.transit.cntTransit = 0;
    stats->info.transit.vdTransit = 0;
    stats->info.transit.meanTransit = 0;
    stats->info.transit.m2Transit = 0;
    stats->info.isochstats.framecnt = 0;
    stats->info.isochstats.framelostcnt = 0;
    stats->info.isochstats.slipcnt = 0;
    if (stats->info.TotalLen) {
        stats->info.IPGcnt = 0;
	stats->info.IPGsum = 0;
    }
    stats->info.l2counts.cnt = 0;
    stats->info.l2counts.unknown = 0;
    stats->info.l2counts.udpcsumerr = 0;
    stats->info.l2counts.lengtherr = 0;
    stats->info.arrivalSum = 0;
}

// These are the output handlers that get the reports ready and then prints them
static void reporter_transfer_protocol_server_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
    if (sumstats) {
	sumstats->cntOutofOrder += stats->cntOutofOrder - stats->lastOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	sumstats->cntError += stats->cntError - stats->lastError;
	sumstats->cntDatagrams += stats->PacketID - stats->lastDatagrams;
	sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
	if (sumstats->info.IPGsum < stats->info.IPGsum)
	    sumstats->info.IPGsum = stats->info.IPGsum;
	sumstats->info.IPGcnt += stats->info.IPGcnt;
    }
    if (bidirstats) {
	bidirstats->TotalLen += stats->TotalLen - stats->lastTotal;
    }
    // print a interval report and possibly a partial interval report if this a final
    stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
    if (!final || (final && (stats->info.TotalLen > 0) && !TimeZero(stats->intervalTime))) {
	stats->info.cntOutofOrder = stats->cntOutofOrder - stats->lastOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->info.cntError = stats->cntError - stats->lastError;
	stats->info.cntError -= stats->info.cntOutofOrder;
	stats->info.cntDatagrams = stats->PacketID - stats->lastDatagrams;
	if (final)
	    reporter_set_timestamps_time(stats, FINALPARTIAL);
	reporter_print(stats, TRANSFER_REPORT, 0);
	reporter_reset_transfer_stats_server_udp(stats);
    }
    if (final) {
	reporter_set_timestamps_time(stats, TOTAL);
	stats->info.cntOutofOrder = stats->cntOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->info.cntError = stats->cntError;
	stats->info.cntError -= stats->info.cntOutofOrder;
	stats->info.cntDatagrams = stats->PacketID;
	stats->info.IPGcnt = stats->info.IPGcnttot;
	stats->info.IPGsum = TimeDifference(stats->packetTime, stats->startTime);
	stats->info.TotalLen = stats->TotalLen;
	stats->info.l2counts.cnt = stats->info.l2counts.tot_cnt;
	stats->info.l2counts.unknown = stats->info.l2counts.tot_unknown;
	stats->info.l2counts.udpcsumerr = stats->info.l2counts.tot_udpcsumerr;
	stats->info.l2counts.lengtherr = stats->info.l2counts.tot_lengtherr;
	stats->info.transit.minTransit = stats->info.transit.totminTransit;
        stats->info.transit.maxTransit = stats->info.transit.totmaxTransit;
	stats->info.transit.cntTransit = stats->info.transit.totcntTransit;
	stats->info.transit.sumTransit = stats->info.transit.totsumTransit;
	stats->info.transit.meanTransit = stats->info.transit.totmeanTransit;
	stats->info.transit.m2Transit = stats->info.transit.totm2Transit;
	stats->info.transit.vdTransit = stats->info.transit.totvdTransit;
	reporter_print(stats, TRANSFER_REPORT, 1);
    }
}
static void reporter_transfer_protocol_sum_server_udp(struct ReporterData *stats, int final) {
    if (final) {
	reporter_set_timestamps_time(stats, TOTAL);
	stats->info.cntOutofOrder = stats->cntOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->info.cntError = stats->cntError;
	stats->info.cntError -= stats->info.cntOutofOrder;
	stats->info.cntDatagrams = stats->cntDatagrams;
	stats->info.TotalLen = stats->TotalLen;
	reporter_print(stats, MULTIPLE_REPORT, 1);
    } else {
	stats->info.cntOutofOrder = stats->cntOutofOrder - stats->lastOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->info.cntError = stats->cntError - stats->lastError;
	stats->info.cntError -= stats->info.cntOutofOrder;
	stats->info.cntDatagrams = stats->cntDatagrams - stats->lastDatagrams;
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print(stats, MULTIPLE_REPORT, 0);
	reporter_reset_transfer_stats_server_udp(stats);
    }
}
static void reporter_connect_printf_tcp_final (struct ReportHeader *reporthdr) {
    if (reporthdr->connect_times.cnt > 1) {
        double variance = (reporthdr->connect_times.cnt < 2) ? 0 : sqrt(reporthdr->connect_times.m2 / (reporthdr->connect_times.cnt - 1));
        fprintf(stdout, "[ CT] final connect times (min/avg/max/stdev) = %0.3f/%0.3f/%0.3f/%0.3f ms (tot/err) = %d/%d\n", \
		reporthdr->connect_times.min,  \
	        (reporthdr->connect_times.sum / reporthdr->connect_times.cnt), \
		reporthdr->connect_times.max, variance,  \
		(reporthdr->connect_times.cnt + reporthdr->connect_times.err), \
		reporthdr->connect_times.err);
    }
}

static void reporter_transfer_protocol_sum_client_udp(struct ReporterData *stats, int final) {
    if (final) {
	reporter_set_timestamps_time(stats, TOTAL);
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.sock_callstats.write.TCPretry = stats->info.sock_callstats.write.totTCPretry;
	stats->info.cntDatagrams = stats->cntDatagrams;
	stats->info.TotalLen = stats->TotalLen;
	reporter_print( stats, MULTIPLE_REPORT, 1 );
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print( stats, MULTIPLE_REPORT, 0 );
	reporter_reset_transfer_stats_client_udp(stats);
    }
}

static void reporter_transfer_protocol_client_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
    if (sumstats) {
	sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
	sumstats->info.sock_callstats.write.WriteErr += stats->info.sock_callstats.write.WriteErr;
	sumstats->info.sock_callstats.write.WriteCnt += stats->info.sock_callstats.write.WriteCnt;
	sumstats->info.sock_callstats.write.totWriteErr += stats->info.sock_callstats.write.WriteErr;
	sumstats->info.sock_callstats.write.totWriteCnt += stats->info.sock_callstats.write.WriteCnt;
	sumstats->cntDatagrams += stats->cntDatagrams;
	if (sumstats->info.IPGsum < stats->info.IPGsum)
	    sumstats->info.IPGsum = stats->info.IPGsum;
	sumstats->info.IPGcnt += stats->info.IPGcnt;

    }
    if (bidirstats) {
	bidirstats->TotalLen += stats->TotalLen - stats->lastTotal;
    }
    if (final) {
	reporter_set_timestamps_time(stats, TOTAL);
	stats->info.TotalLen = stats->TotalLen;
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.TotalLen = stats->TotalLen;
	stats->info.IPGcnt = stats->info.IPGcnttot;
	stats->info.cntDatagrams = stats->PacketID;
	reporter_print(stats, TRANSFER_REPORT, 1);
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print(stats, TRANSFER_REPORT, 0);
	reporter_reset_transfer_stats_client_udp(stats);
    }
}

static void reporter_transfer_protocol_server_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
    int ix;
    if (sumstats) {
        sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
        sumstats->info.sock_callstats.read.cntRead += stats->info.sock_callstats.read.cntRead;
        sumstats->info.sock_callstats.read.totcntRead += stats->info.sock_callstats.read.cntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    sumstats->info.sock_callstats.read.bins[ix] += stats->info.sock_callstats.read.bins[ix];
	    sumstats->info.sock_callstats.read.totbins[ix] += stats->info.sock_callstats.read.bins[ix];
        }
    }
    if (bidirstats) {
	bidirstats->TotalLen += stats->TotalLen - stats->lastTotal;
    }
    stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
    if (!final) {
	if (!bidirstats)
	    reporter_print(stats, TRANSFER_REPORT, 0);
	else if (stats->info.mEnhanced)
	    reporter_print(stats, TRANSFER_REPORT, 0);
	reporter_reset_transfer_stats_server_tcp(stats);
    } else {
        // print a partial interval report if enable and this a final
        if ((stats->info.TotalLen > 0) && !TimeZero(stats->intervalTime)) {
	    reporter_set_timestamps_time(stats, FINALPARTIAL);
	    reporter_print(stats, TRANSFER_REPORT, 0);
	    reporter_reset_transfer_stats_server_tcp(stats);
        }
	reporter_set_timestamps_time(stats, TOTAL);
        stats->info.TotalLen = stats->TotalLen;
	stats->info.arrivalSum = stats->info.totarrivalSum;
        stats->info.sock_callstats.read.cntRead = stats->info.sock_callstats.read.totcntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->info.sock_callstats.read.bins[ix] = stats->info.sock_callstats.read.totbins[ix];
        }
	stats->info.transit.sumTransit = stats->info.transit.totsumTransit;
	stats->info.transit.cntTransit = stats->info.transit.totcntTransit;
	stats->info.transit.minTransit = stats->info.transit.totminTransit;
	stats->info.transit.maxTransit = stats->info.transit.totmaxTransit;
	stats->info.transit.m2Transit = stats->info.transit.totm2Transit;
	if (!bidirstats || stats->info.mEnhanced) {
	    reporter_print(stats, TRANSFER_REPORT, 1);
	}
    }
}

static void reporter_transfer_protocol_client_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    if (stats->info.mEnhanced && (stats->info.mTCP == kMode_Client))
	gettcpistats(stats, sumstats, 0);
#endif
    if (sumstats) {
	sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
	sumstats->info.sock_callstats.write.WriteErr += stats->info.sock_callstats.write.WriteErr;
	sumstats->info.sock_callstats.write.WriteCnt += stats->info.sock_callstats.write.WriteCnt;
	sumstats->info.sock_callstats.write.TCPretry += stats->info.sock_callstats.write.TCPretry;
	sumstats->info.sock_callstats.write.totWriteErr += stats->info.sock_callstats.write.WriteErr;
	sumstats->info.sock_callstats.write.totWriteCnt += stats->info.sock_callstats.write.WriteCnt;
	sumstats->info.sock_callstats.write.totTCPretry += stats->info.sock_callstats.write.TCPretry;
    }
    if (bidirstats) {
	bidirstats->TotalLen += stats->TotalLen - stats->lastTotal;
    }
    if (final) {
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.sock_callstats.write.TCPretry = stats->info.sock_callstats.write.totTCPretry;
	stats->info.TotalLen = stats->TotalLen;
	reporter_set_timestamps_time(stats, TOTAL);
	if (!bidirstats || stats->info.mEnhanced)
	    reporter_print(stats, TRANSFER_REPORT, 1);
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	if (!bidirstats || stats->info.mEnhanced)
	    reporter_print(stats, TRANSFER_REPORT, 0);
	reporter_reset_transfer_stats_client_tcp(stats);
    }
}

static void reporter_transfer_protocol_client_all_final(struct ReporterData *stats) {
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    if ((stats->info.mEnhanced && stats->info.mTCP == kMode_Client) && (!stats->info.sock_callstats.write.up_to_date))
        gettcpistats(stats, NULL, 1);
#endif
        stats->info.cntOutofOrder = stats->cntOutofOrder;
        // assume most of the time out-of-order packets are not
        // duplicate packets, so conditionally subtract them from the lost packets.
        stats->info.cntError = stats->cntError;
        stats->info.cntError -= stats->info.cntOutofOrder;
        if ( stats->info.cntError < 0 ) {
            stats->info.cntError = 0;
        }
        stats->info.cntDatagrams = ((stats->info.mUDP == kMode_Server) ? stats->PacketID - INITIAL_PACKETID : stats->cntDatagrams);
        stats->info.TotalLen = stats->TotalLen;
        stats->info.startTime = 0;

	if (stats->info.mUDP == kMode_Server) {
	    stats->info.l2counts.cnt = stats->info.l2counts.tot_cnt;
	    stats->info.l2counts.unknown = stats->info.l2counts.tot_unknown;
	    stats->info.l2counts.udpcsumerr = stats->info.l2counts.tot_udpcsumerr;
	    stats->info.l2counts.lengtherr = stats->info.l2counts.tot_lengtherr;
	    stats->info.transit.minTransit = stats->info.transit.totminTransit;
	    stats->info.transit.maxTransit = stats->info.transit.totmaxTransit;
	    stats->info.transit.cntTransit = stats->info.transit.totcntTransit;
	    stats->info.transit.sumTransit = stats->info.transit.totsumTransit;
	    stats->info.transit.meanTransit = stats->info.transit.totmeanTransit;
	    stats->info.transit.m2Transit = stats->info.transit.totm2Transit;
	    stats->info.transit.vdTransit = stats->info.transit.totvdTransit;
	}
	if ((stats->info.mTCP == kMode_Client) || (stats->info.mUDP == kMode_Client)) {
	    stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	    stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	    if (stats->info.mTCP == kMode_Client) {
		stats->info.sock_callstats.write.TCPretry = stats->info.sock_callstats.write.totTCPretry;
	    }
	}
	if (stats->info.mTCP == kMode_Server) {
	    int ix;
	    stats->info.sock_callstats.read.cntRead = stats->info.sock_callstats.read.totcntRead;
	    for (ix = 0; ix < 8; ix++) {
		stats->info.sock_callstats.read.bins[ix] = stats->info.sock_callstats.read.totbins[ix];
	    }
	    if (stats->clientStartTime.tv_sec > 0)
		stats->info.tripTime = TimeDifference( stats->packetTime, stats->clientStartTime );
	    else
		stats->info.tripTime = 0;
	}
	if (stats->info.endTime > 0) {
	    stats->info.IPGcnt = (int) (stats->cntDatagrams / stats->info.endTime);
	} else {
	    stats->info.IPGcnt = 0;
	}
	stats->info.IPGsum = 1;
        stats->info.free = 1;
	if (stats->info.mIsochronous) {
	    stats->info.isochstats.framecnt = stats->isochstats.framecnt;
	    stats->info.isochstats.framelostcnt = stats->isochstats.framelostcnt;
	    stats->info.isochstats.slipcnt = stats->isochstats.slipcnt;
	}
        reporter_print( stats, TRANSFER_REPORT, 1 );
}

/*
 * Handles summing of threads
 */
static void reporter_transfer_protocol_sum_client_tcp(struct ReporterData *stats, int final) {
    if (final) {
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.sock_callstats.write.TCPretry = stats->info.sock_callstats.write.totTCPretry;
	stats->info.TotalLen = stats->TotalLen;
        reporter_set_timestamps_time(stats, TOTAL);
	reporter_print( stats, MULTIPLE_REPORT, 1 );
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print( stats, MULTIPLE_REPORT, 0 );
	reporter_reset_transfer_stats_client_tcp(stats);
    }
}

static void reporter_transfer_protocol_sum_server_tcp(struct ReporterData *stats, int final) {
    if (final) {
	int ix;
	stats->info.TotalLen = stats->TotalLen;
	stats->info.sock_callstats.read.cntRead = stats->info.sock_callstats.read.totcntRead;
	for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->info.sock_callstats.read.bins[ix] = stats->info.sock_callstats.read.totbins[ix];
	}
        reporter_set_timestamps_time(stats, TOTAL);
	reporter_print( stats, MULTIPLE_REPORT, 1 );
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print( stats, MULTIPLE_REPORT, 0 );
	reporter_reset_transfer_stats_server_tcp(stats);
    }
}

static void reporter_transfer_protocol_bidir_tcp(struct ReporterData *stats, int final) {
    if (final) {
	stats->info.TotalLen = stats->TotalLen;
        reporter_set_timestamps_time(stats, TOTAL);
	reporter_print(stats, BIDIR_REPORT, 1);
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print(stats, BIDIR_REPORT, 0);
	reporter_reset_transfer_stats_bidir(stats);
    }
}

static void reporter_transfer_protocol_bidir_udp(struct ReporterData *stats, int final) {
}

/*
 * This function handles multiple format printing by sending to the
 * appropriate dispatch function
 */
int reporter_print( struct ReporterData *stats, int type, int end ) {
    switch ( type ) {
        case TRANSFER_REPORT:
	    stats->info.free = end;
	    statistics_reports[stats->mode]( &stats->info);
            if ( end != 0 && isPrintMSS( stats ) && !isUDP( stats ) ) {
                PrintMSS( stats );
            }
            break;
        case SERVER_RELAY_REPORT:
            serverstatistics_reports[stats->mode]( &stats->connection, &stats->info );
            break;
        case SETTINGS_REPORT:
            settings_reports[stats->mode]( stats );
            break;
        case CONNECTION_REPORT:
	    stats->info.reserved_delay = connection_reports[stats->mode] \
		(&stats->connection, stats->info.transferID);
            break;
        case MULTIPLE_REPORT:
            multiple_reports[stats->mode]( &stats->info );
            break;
        case BIDIR_REPORT:
            bidir_reports[stats->mode]( &stats->info );
            break;
        case TRANSFER_FRAMEREPORTUDP:
            frame_udpreports[stats->mode]( &stats->info );
            break;
        case TRANSFER_FRAMEREPORTTCP:
            frame_tcpreports[stats->mode]( &stats->info );
            break;
        default:
            fprintf( stderr, "Printing type not implemented! No Output\n" );
    }
    fflush( stdout );
    return end;
}

/* -------------------------------------------------------------------
 * Report the MSS and MTU, given the MSS (or a guess thereof)
 * ------------------------------------------------------------------- */

// compare the MSS against the (MTU - 40) to (MTU - 80) bytes.
// 40 byte IP header and somewhat arbitrarily, 40 more bytes of IP options.

#define checkMSS_MTU( inMSS, inMTU ) (inMTU-40) >= inMSS  &&  inMSS >= (inMTU-80)

void PrintMSS( struct ReporterData *stats ) {
    int inMSS = getsock_tcp_mss( stats->info.transferID );

    if ( inMSS <= 0 ) {
        printf( report_mss_unsupported, stats->info.transferID );
    } else {
        char* net;
        int mtu = 0;

        if ( checkMSS_MTU( inMSS, 1500 ) ) {
            net = "ethernet";
            mtu = 1500;
        } else if ( checkMSS_MTU( inMSS, 4352 ) ) {
            net = "FDDI";
            mtu = 4352;
        } else if ( checkMSS_MTU( inMSS, 9180 ) ) {
            net = "ATM";
            mtu = 9180;
        } else if ( checkMSS_MTU( inMSS, 65280 ) ) {
            net = "HIPPI";
            mtu = 65280;
        } else if ( checkMSS_MTU( inMSS, 576 ) ) {
            net = "minimum";
            mtu = 576;
            printf( "%s", warn_no_pathmtu );
        } else {
            mtu = inMSS + 40;
            net = "unknown interface";
        }

        printf( report_mss,
                stats->info.transferID, inMSS, mtu, net );
    }
}
// end ReportMSS


#ifdef __cplusplus
} /* end extern "C" */
#endif
