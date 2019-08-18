/*---------------------------------------------------------------
 * Copyright (c) 1999,2000,2001,2002,2003
 * The Board of Trustees of the University of Illinois
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software (Iperf) and associated
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
void* connection_notimpl( Connection_Info * nused, int nuse ) {
    return NULL;
}
void settings_notimpl( ReporterData * nused ) { }
void statistics_notimpl( Transfer_Info * nused ) { }
void serverstatistics_notimpl( Connection_Info *nused1, Transfer_Info *nused2 ) { }

// To add a reporting style include its header here.
#include "report_default.h"
#include "report_CSV.h"

extern Condition MultiBarrier;

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

char buffer[SNBUFFERSIZE]; // Buffer for printing
ReportHeader *ReportRoot = NULL;
extern Condition ReportCond;
int reporter_process_report ( ReportHeader *report );
void process_report ( ReportHeader *report );
int reporter_print( ReporterData *stats, int type, int end );
void PrintMSS( ReporterData *stats );

void UpdateMultiHdrRefCounter(MultiHeader *reporthdr, int val);

// Private routines
static int condprint_interval_reports (ReportHeader *reporthdr, ReportStruct *packet);
static void output_missed_reports(ReporterData *stats, ReportStruct *packet);
static void output_missed_multireports(ReporterData *stats, ReportStruct *packet);
static void output_transfer_report_client_tcp(ReporterData *stats, ReporterData *sumstats, int final);
static void output_transfer_report_client_udp(ReporterData *stats, ReporterData *sumstats, int final);
static void output_transfer_report_server_tcp(ReporterData *stats, ReporterData *sumstats, int final);
static void output_transfer_report_server_udp(ReporterData *stats, ReporterData *sumstats, int final);
static void output_transfer_sum_report_client_tcp(ReporterData *stats, int final);
static void output_transfer_sum_report_server_tcp(ReporterData *stats, int final);
static void output_transfer_bidir_report(ReporterData *stats);
static void output_transfer_bidir_final_report(ReporterData *stats);
static void reset_transfer_stats(ReporterData *stats);
static inline void reset_transfer_stats_client(ReporterData *stats);
static inline void reset_transfer_stats_server_udp(ReporterData *stats);
static inline void reset_transfer_stats_server_tcp(ReporterData *stats);
static void reporter_handle_packet_server_udp(ReportHeader *report, ReportStruct *packet);
static void reporter_handle_packet_server_tcp(ReportHeader *report, ReportStruct *packet);
static void reporter_handle_packet_client(ReportHeader *report, ReportStruct *packet);
static void InitDataReport(struct thread_Settings *mSettings);
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats(ReporterData *stats, ReporterData *sumstats, int final);
#endif
static PacketRing * init_packetring(int count);


MultiHeader* InitMulti(thread_Settings *agent, int inID, MultiHdrType type) {
    MultiHeader *multihdr = (MultiHeader *) calloc(1, sizeof(MultiHeader));
    if ( multihdr != NULL ) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Init multiheader %p id=%d (type=%d)", (void *)multihdr, inID, type);
#endif
        agent->multihdr = multihdr;
	multihdr->groupID = inID;
	multihdr->refcount = 1;
	if (agent->mThreadMode == kMode_Client) {
	    multihdr->threads = agent->mThreads;
	}
	if (isMultipleReport(agent)) {
	    ReporterData *data = &multihdr->report;
	    data->type = TRANSFER_REPORT;
	    if (agent->mInterval != 0.0) {
		struct timeval *interval = &data->intervalTime;
		interval->tv_sec = (long) agent->mInterval;
		interval->tv_usec = (long) ((agent->mInterval - interval->tv_sec)
					    * rMillion);
	    }
	    data->mHost = agent->mHost;
	    data->mLocalhost = agent->mLocalhost;
	    data->mBufLen = agent->mBufLen;
	    data->mMSS = agent->mMSS;
	    data->mTCPWin = agent->mTCPWin;
	    data->FQPacingRate = agent->mFQPacingRate;
	    data->flags = agent->flags;
	    data->mThreadMode = agent->mThreadMode;
	    data->mode = agent->mReportMode;
	    data->info.mFormat = agent->mFormat;
	    data->info.mTTL = agent->mTTL;
	    if (data->mThreadMode == kMode_Server)
		data->info.sock_callstats.read.binsize = data->mBufLen / 8;
	    if ( isEnhanced( agent ) ) {
		data->info.mEnhanced = 1;
	    } else {
		data->info.mEnhanced = 0;
	    }
	    if ( isUDP( agent ) ) {
		multihdr->report.info.mUDP = (char)agent->mThreadMode;
	    } else {
		multihdr->report.info.mTCP = (char)agent->mThreadMode;
	    }
	}
    } else {
            FAIL(1, "Out of Memory!!\n", agent);
    }
    return multihdr;
}

/*
 * BarrierClient allows for multiple stream clients to be syncronized
 */
void BarrierClient( MultiHeader *multihdr ) {
    Condition_Lock(MultiBarrier);
    multihdr->threads--;
    if ( multihdr->threads == 0 ) {
        // store the wake up or start time in the shared multihdr
        gettimeofday( &(multihdr->report.startTime), NULL );
        // last one wake's up everyone else
        Condition_Broadcast(&MultiBarrier);
    } else {
        Condition_Wait(&MultiBarrier);
    }
    multihdr->threads++;
    Condition_Unlock(MultiBarrier);
}

/*
 * InitReport is called by a transfer agent (client or
 * server) to setup the needed structures to communicate
 * traffic and connection information.  Also initialize
 * the report start time and next interval report time
 * Finally, in the case of parallel clients, have them all
 * synchronize on compeleting their connect()
 */
void InitReport(thread_Settings *mSettings) {
    // Note, this must be called in order as
    // the data report structures need to be
    // initialized first
    if (isDataReport(mSettings)) {
	InitDataReport(mSettings);
    }
    if (isConnectionReport(mSettings)) {
	InitConnectionReport(mSettings);
    }
}

static void free_packetring(PacketRing *pr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Free packet ring %p & condition variable await consumer %p", (void *)pr, (void *)&pr->await_consumer);
#endif
    if (pr->awaitcounter > 1000) fprintf(stderr, "WARN: Reporter thread may be too slow, await counter=%d, " \
					 "consider increasing NUM_REPORT_STRUCTS\n", pr->awaitcounter);
    Condition_Destroy(&pr->await_consumer);
    if (pr->data) free(pr->data);
}

void UpdateMultiHdrRefCounter(MultiHeader *multihdr, int val) {
  if (!multihdr)
    return;
  // decrease the reference counter for mutliheaders
  // and check to free the multiheader
  Mutex_Lock( &groupCond );
  if (multihdr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Sum multiheader %p ref=%d->%d", (void *)multihdr, \
		 multihdr->refcount, (multihdr->refcount + val));
#endif
    multihdr->refcount += val;
  }
  Mutex_Unlock( &groupCond );
}
void FreeReport(ReportHeader *reporthdr) {
    if (reporthdr) {
        if (reporthdr->delaycounter < 3) {
	    fprintf(stdout, "WARN: this test was likley CPU bound which may not detecting the underlying network devices\n");
	}
	free_packetring(reporthdr->packetring);
	if (reporthdr->report.info.latency_histogram) {
	    histogram_delete(reporthdr->report.info.latency_histogram);
	}
#ifdef HAVE_ISOCHRONOUS
	if (reporthdr->report.info.framelatency_histogram) {
	    histogram_delete(reporthdr->report.info.framelatency_histogram);
	}
#endif
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Free report hdr %p delay counter=%d", (void *)reporthdr, reporthdr->delaycounter);
#endif
	Mutex_Unlock( &groupCond );
	free(reporthdr);
    }
}

void InitDataReport(thread_Settings *mSettings) {
    /*
     * Create in one big chunk
     */
    ReportHeader *reporthdr = (ReportHeader *) calloc(1, sizeof(ReportHeader));
    ReporterData *data = NULL;

    if ( reporthdr != NULL ) {
	mSettings->reporthdr = reporthdr;
	reporthdr->multireport = mSettings->multihdr;
	reporthdr->bidirreport = mSettings->bidirhdr;
	data = &reporthdr->report;
	data->mThreadMode = mSettings->mThreadMode;
	reporthdr->packet_handler = NULL;
	if (!isConnectOnly(mSettings)) {
	    // Create a new packet ring which is used to communicate
	    // packet stats from the traffic thread to the reporter
	    // thread.  The reporter thread does all packet accounting
	    reporthdr->packetring = init_packetring(NUM_REPORT_STRUCTS);
	    // Set up the function vectors, there are three
	    // 1) packet_handler: does packet accounting per the test and protocol
	    // 2) output_handler: performs output, e.g. interval reports, per the test and protocol
	    // 3) output_sum_handler: performs summing output when multiple traffic threads
	    switch (data->mThreadMode) {
	    case kMode_Server :
		if (isUDP(mSettings)) {
		    reporthdr->packet_handler = reporter_handle_packet_server_udp;
		    reporthdr->output_handler = output_transfer_report_server_udp;
		} else {
		    reporthdr->packet_handler = reporter_handle_packet_server_tcp;
		    reporthdr->output_handler = output_transfer_report_server_tcp;
		    reporthdr->output_sum_handler = output_transfer_sum_report_server_tcp;
		}
		break;
	    case kMode_Client :
		reporthdr->packet_handler = reporter_handle_packet_client;
		if (isUDP(mSettings)) {
		    reporthdr->output_handler = output_transfer_report_client_udp;
		} else {
		    reporthdr->output_handler = output_transfer_report_client_tcp;
		    reporthdr->output_sum_handler = output_transfer_sum_report_client_tcp;
		}
		if (reporthdr->multireport) {
		  UpdateMultiHdrRefCounter(reporthdr->multireport, 1);
		}
		break;
	    case kMode_Unknown :
	    case kMode_Reporter :
	    case kMode_ReporterClient :
	    case kMode_ReporterClientFullDuplex :
	    case kMode_ReporterServer :
	    case kMode_ReporterServerFullDuplex :
	    case kMode_Listener:
	    default:
		reporthdr->packet_handler = NULL;
	    }
	}
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Init data report %p size %ld using packetring %p", (void *)reporthdr, sizeof(ReportHeader), (void *)(reporthdr->packetring));
#endif
	data->lastError = INITIAL_PACKETID;
	data->lastDatagrams = INITIAL_PACKETID;
	data->PacketID = INITIAL_PACKETID;
	data->info.transferID = mSettings->mSock;
	data->info.groupID = (mSettings->multihdr != NULL ? mSettings->multihdr->groupID : -1);
	data->type = TRANSFER_REPORT;
	if ( mSettings->mInterval != 0.0 ) {
	    struct timeval *interval = &data->intervalTime;
	    interval->tv_sec = (long) mSettings->mInterval;
	    interval->tv_usec = (long) ((mSettings->mInterval - interval->tv_sec) * rMillion);
	}
	data->mHost = mSettings->mHost;
	data->mLocalhost = mSettings->mLocalhost;
	data->mSSMMulticastStr = mSettings->mSSMMulticastStr;
	data->mIfrname = mSettings->mIfrname;
	data->mIfrnametx = mSettings->mIfrnametx;
	data->mBufLen = mSettings->mBufLen;
	data->mMSS = mSettings->mMSS;
	data->mTCPWin = mSettings->mTCPWin;
	data->FQPacingRate = mSettings->mFQPacingRate;
	data->flags = mSettings->flags;
	data->mThreadMode = mSettings->mThreadMode;
	data->mode = mSettings->mReportMode;
	data->info.mFormat = mSettings->mFormat;
	data->info.mTTL = mSettings->mTTL;
	if (data->mThreadMode == kMode_Server)
	    data->info.sock_callstats.read.binsize = data->mBufLen / 8;
	if ( isUDP( mSettings ) ) {
	    gettimeofday(&data->IPGstart, NULL);
	    reporthdr->report.info.mUDP = (char)mSettings->mThreadMode;
	} else {
	    reporthdr->report.info.mTCP = (char)mSettings->mThreadMode;
	}
	if ( isEnhanced( mSettings ) ) {
	    data->info.mEnhanced = 1;
	} else {
	    data->info.mEnhanced = 0;
	}
	if (data->mThreadMode == kMode_Server) {
	    if (isRxHistogram(mSettings)) {
		char name[] = "T8";
		data->info.latency_histogram =  histogram_init(mSettings->mRXbins,mSettings->mRXbinsize,0,\
							       (mSettings->mRXunits ? 1e6 : 1e3), \
							       mSettings->mRXci_lower, mSettings->mRXci_upper, data->info.transferID, name);
	    }
#ifdef HAVE_ISOCHRONOUS
	    if (isRxHistogram(mSettings) && isIsochronous(mSettings)) {
		char name[] = "F8";
		// make sure frame bin size min is 100 microsecond
		if (mSettings->mRXunits && (mSettings->mRXbinsize < 100))
		    mSettings->mRXbinsize = 100;
		mSettings->mRXunits = 1;
		data->info.framelatency_histogram =  histogram_init(mSettings->mRXbins,mSettings->mRXbinsize,0, \
								    (mSettings->mRXunits ? 1e6 : 1e3), mSettings->mRXci_lower, \
								    mSettings->mRXci_upper, data->info.transferID, name);
	    }
#endif
	}
#ifdef HAVE_ISOCHRONOUS
	if ( isIsochronous( mSettings ) ) {
	    data->info.mIsochronous = 1;
	} else {
	    data->info.mIsochronous = 0;
	}
#endif
    } else {
	FAIL(1, "Out of Memory!!\n", mSettings);
    }
}


/*
 * This init/update and print/finish (in the ReportDefault.c)
 * is poor.  It has to be done this way to preserve the
 * interface to older versions where the reporter settings
 * were delayed until a Transfer report came through.
 * This transfer report has all the reports bound to it.
 *
 * The better implmementation is to treat all reports
 * as independent objects that can be updated, processed,
 * and output independlty per the Reporter threads job queue
 * without shared state or copied state variables between these
 * reports.  The shared state, is really reporter state, that
 * should be maintained in and by the reporter object/thread.
 *
 * For now, just fix it good enough.  Later, write a c++
 * reporter object and use standard c++ design techniques
 * to achieve this.  Such code will be easier to maintain
 * and to extend.
 */
void InitConnectionReport (thread_Settings *mSettings) {
    ReportHeader *reporthdr = mSettings->reporthdr;
    ReporterData *data = NULL;
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init connection report %p", reporthdr);
#endif

    if (reporthdr == NULL) {
	/*
	 * We don't have a Data Report structure in which to hang
	 * the connection report so allocate a minimal one
	 */
	reporthdr = calloc( sizeof(ReportHeader), sizeof(char*) );
	if (reporthdr == NULL ) {
	    FAIL(1, "Out of Memory!!\n", mSettings);
	}
	mSettings->reporthdr = reporthdr;
	reporthdr->multireport = mSettings->multihdr;
    }
    // Fill out known fields for the connection report
    data = &reporthdr->report;
    data->info.transferID = mSettings->mSock;
    data->info.groupID = -1;
    data->type |= CONNECTION_REPORT;
    data->connection.peer = mSettings->peer;
    data->connection.size_peer = mSettings->size_peer;
    data->connection.local = mSettings->local;
    data->connection.size_local = mSettings->size_local;
    data->connection.peerversion = mSettings->peerversion;
    // Set the l2mode flags
    data->connection.l2mode = isL2LengthCheck(mSettings);
    if (data->connection.l2mode)
	data->connection.l2mode = ((isIPV6(mSettings) << 1) | data->connection.l2mode);
    if (isEnhanced(mSettings) && isTxStartTime(mSettings)) {
	data->connection.epochStartTime.tv_sec = mSettings->txstart_epoch.tv_sec;
	data->connection.epochStartTime.tv_usec = mSettings->txstart_epoch.tv_usec;
    }
    if (isFQPacing(data) && (data->mThreadMode == kMode_Client)) {
	char tmpbuf[40];
	byte_snprintf(tmpbuf, sizeof(tmpbuf), data->FQPacingRate, 'a');
	tmpbuf[39]='\0';
        printf(client_fq_pacing,tmpbuf);
    }
    //  Copy state from the settings object into the connection report
    //  See notes about how a proper C++ implmentation can fix this
    data->connection.flags = mSettings->flags;
    data->connection.flags_extend = mSettings->flags_extend;
    data->connection.mFormat = mSettings->mFormat;
    if (mSettings->mSock > 0)
      UpdateConnectionReport(mSettings, reporthdr);
}

// Read the actual socket window size data
void UpdateConnectionReport(thread_Settings *mSettings, ReportHeader *reporthdr) {
    if (reporthdr != NULL) {
        ReporterData *data = &reporthdr->report;
	if (mSettings && (mSettings->mSock > 0)) {
	    data->connection.winsize = getsock_tcp_windowsize(mSettings->mSock, \
                  (data->mThreadMode != kMode_Client ? 0 : 1) );
	}
	data->connection.winsize_requested = data->mTCPWin;
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Update connection report %p winreq=%d actual=%d", \
		     reporthdr, data->connection.winsize_requested, data->connection.winsize);
#endif
    }
}

void PostReport (ReportHeader *reporthdr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Post report %p", reporthdr);
#endif
    if (reporthdr) {
#ifdef HAVE_THREAD
	/*
	 * Update the ReportRoot to include this report.
	 */
	Condition_Lock( ReportCond );
	reporthdr->next = ReportRoot;
	ReportRoot = reporthdr;
	Condition_Signal( &ReportCond );
	Condition_Unlock( ReportCond );
#else
	/*
	 * Process the report in this thread
	 */
	reporthdr->next = NULL;
	process_report ( reporthdr );
#endif
    }
}

// Work in progress

static PacketRing * init_packetring (int count) {
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
    pr->awake_consumer = &ReportCond;
    Condition_Initialize(&pr->await_consumer);
    pr->consumerdone = 0;
    pr->awaitcounter = 0;
    return (pr);
}

static inline void enqueue_packetring(ReportHeader* agent, ReportStruct *metapacket) {
    PacketRing *pr = agent->packetring;
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
    memcpy((agent->packetring->data + writeindex), metapacket, sizeof(ReportStruct));
    pr->producer = writeindex;
}

static inline ReportStruct *dequeue_packetring(ReportHeader* agent) {
    PacketRing *pr = agent->packetring;
    ReportStruct *packet = NULL;
    if (pr->producer == pr->consumer)
	return NULL;

    int readindex;
    if ((pr->consumer + 1) == pr->maxcount)
	readindex = 0;
    else
	readindex = (pr->consumer + 1);
    packet = (agent->packetring->data + readindex);
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
static inline int getcount_packetring(ReportHeader *agent) {
    PacketRing *pr = agent->packetring;
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
/*
 * ReportPacket is called by a transfer agent to record
 * the arrival or departure of a "packet" (for TCP it
 * will actually represent many packets). This needs to
 * be as simple and fast as possible as it gets called for
 * every "packet".
 */
void ReportPacket( ReportHeader* agent, ReportStruct *packet ) {
    if ( agent != NULL ) {
#ifdef HAVE_THREAD_DEBUG
	if (packet->packetID < 0) {
	  thread_debug("Reporting last packet for %p  qdepth=%d", (void *) agent, getcount_packetring(agent));
	}
#endif
        enqueue_packetring(agent, packet);
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
 * the report and signal transfer is over.
 */
void CloseReport(ReportHeader *agent, ReportStruct *finalpacket) {
    if ( agent != NULL) {
	ReportStruct packet;
        /*
         * Using PacketID of -1 ends reporting
         * It pushes a "special packet" through
         * the packet ring which will be detected
         * by the reporter thread as and end of traffic
         * event
         */
        packet.packetID = -1;
        packet.packetLen = 0;
	packet.packetTime = finalpacket->packetTime;
        ReportPacket(agent, &packet);
    }
}

/*
 * EndReport signifies the agent no longer is interested
 * in the report. Calls to GetReport will no longer be
 * filled
 */
void EndReport( ReportHeader *agent ) {
    if ( agent != NULL ) {
#ifdef HAVE_THREAD_DEBUG
	thread_debug( "Traffic thread awaiting reporter to be done with %p", (void *)agent);
#endif
        Condition_Lock (agent->packetring->await_consumer);
	while (!agent->packetring->consumerdone) {
	    Condition_TimedWait(&agent->packetring->await_consumer, 1);
	    // printf("Consumer done may be stuck\n");
	}
        Condition_Unlock (agent->packetring->await_consumer);
#ifdef HAVE_THREAD_DEBUG
	thread_debug( "Traffic thread thinks reporter is done with %p", (void *)agent);
#endif
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
Transfer_Info *GetReport( ReportHeader *agent ) {
    Transfer_Info *final = NULL;
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
ReportHeader *ReportSettings( thread_Settings *agent ) {
  ReportHeader *reporthdr = agent->reporthdr;
  if ( isSettingsReport( agent ) ) {
        /*
         * Populate and optionally create a new settings report
         */
	 if (!reporthdr)
	    reporthdr = calloc(sizeof(ReportHeader), sizeof(char*));
	 if (reporthdr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init settings report %p", reporthdr);
#endif
            ReporterData *data = &reporthdr->report;
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
#ifdef HAVE_ISOCHRONOUS
	    if (isIsochronous(data)) {
		data->isochstats.mFPS = agent->mFPS;
		data->isochstats.mMean = agent->mMean/8;
		data->isochstats.mVariance = agent->mVariance/8;
		data->isochstats.mBurstIPG = (unsigned int) (agent->mBurstIPG*1000.0);
		data->isochstats.mBurstInterval = (unsigned int) (1 / agent->mFPS * 1000000);
	    }
#endif
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
void ReportServerUDP( thread_Settings *agent, server_hdr *server ) {
    unsigned int flags = ntohl(server->base.flags);
    // printf("Server flags = 0x%X\n", flags);
    if (isServerReport(agent) && ((flags & HEADER_VERSION1) != 0)) {
	/*
	 * Create in one big chunk
	 */
	ReportHeader *reporthdr = calloc( sizeof(ReportHeader), sizeof(char*));
	Transfer_Info *stats = &reporthdr->report.info;

	if ( !reporthdr ) {
	    FAIL(1, "Out of Memory!!\n", agent);
	}
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Init server relay report %p size %ld\n", (void *)reporthdr, sizeof(ReportHeader));
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
#define REPORTERDELAY_DURATION 4000 // units is microseconds
typedef struct ConsumptionDetectorType {
    int accounted_packets;
    int accounted_packet_threads;
    int delay_counter ;
} ConsumptionDetectorType;
ConsumptionDetectorType consumption_detector = \
  {.accounted_packets = 0, .accounted_packet_threads = 0, .delay_counter = 0};

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
	    consumption_detector.delay_counter++;
	    // printf("DEBUG: forced reporter suspend, accounted=%d,  queueue depth after = %d\n", accounted_packets, getcount_packetring(reporthdr));
	} else {
	    // printf("DEBUG: no suspend, accounted=%d,  queueue depth after = %d\n", accounted_packets, getcount_packetring(reporthdr));
	}
	reset_consumption_detector();
    }
}
/*
 * This function is called only when the reporter thread
 * This function is the loop that the reporter thread processes
 */
void reporter_spawn( thread_Settings *thread ) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Reporter thread started");
#endif
    //
    // Signal to other (client) threads that the
    // reporter is now running.  This is needed because
    // the client's traffic thread has a connect() within
    // it's constructor and that connect gets reported via
    // via this thread so let this thread go first
    Condition_Lock(reporter_state.await_reporter);
    reporter_state.reporter_running = 1;
    Condition_Unlock(reporter_state.await_reporter);
    Condition_Broadcast(&reporter_state.await_reporter);

    do {
        Condition_Lock ( ReportCond );
        if ( ReportRoot == NULL ) {
	    //  Use a timed wait because the traffic threads
	    //  that signal this condition may have already
	    //  completed
	    Condition_TimedWait ( &ReportCond, 1);
	    // The reporter is starting from an empty state
	    // so set the load detect to trigger an initial delay
	    reset_consumption_detector();
        }
        Condition_Unlock ( ReportCond );

      again:
        if ( ReportRoot != NULL ) {
            ReportHeader *tmp = ReportRoot;
            if ( reporter_process_report ( tmp ) ) {
                // This section allows for more reports to be added while
                // the reporter is processing reports without needing to
                // stop the reporter or immediately notify it
                Condition_Lock ( ReportCond );
                if ( tmp == ReportRoot ) {
                    // no new reports
                    ReportRoot = tmp->next;
                } else {
                    // new reports added
                    ReportHeader *itr = ReportRoot;
                    while ( itr->next != tmp ) {
                        itr = itr->next;
                    }
                    itr->next = tmp->next;
                }
		// See notes if reporter_process_report
#ifdef HAVE_THREAD_DEBUG
		thread_debug("Remove %p from reporter job queue in rs", (void *) tmp);
#endif
		if ((tmp->report.type & TRANSFER_REPORT) == 0) {
#ifdef HAVE_THREAD_DEBUG
		    thread_debug("Free %p in rs", (void *) tmp);
#endif
		    free(tmp);
		}
                Condition_Unlock ( ReportCond );
                if (ReportRoot)
                    goto again;
            }
        }
	/*
         * Keep the reporter thread alive under the following conditions
         *
         * o) There are more reports to ouput, ReportRoot has a report
         * o) The number of threads is greater than one which indicates
         *    either traffic threads are still running or a Listener thread
         *    is running. If equal to 1 then only the reporter thread is alive
         */
    } while ((thread_numuserthreads() > 1) || ReportRoot);
}

/*
 * Used for single threaded reporting
 */
void process_report ( ReportHeader *report ) {
    if ( report != NULL ) {
        if ( reporter_process_report( report ) ) {
	    if (report->report.info.latency_histogram) {
		histogram_delete(report->report.info.latency_histogram);
	    }
#ifdef HAVE_ISOCHRONOUS
	    if (report->report.info.framelatency_histogram) {
		histogram_delete(report->report.info.framelatency_histogram);
	    }
#endif
            free( report );
        }
    }
}

static int condprint_interval_reports (ReportHeader *reporthdr, ReportStruct *packet) {
    int nextring_event = 0;

    // Print a report if packet time exceeds the next report interval time,
    // Also signal to the caller to move to the next report (or packet ring)
    // if there was output. This will allow for more precise interval sum accounting.
    if (TimeDifference(reporthdr->report.nextTime, packet->packetTime) < 0) {
        reporthdr->report.packetTime=packet->packetTime;
        // In the (hopefully unlikely event) the reporter fell behind
        // ouput the missed reports to catch up
        // output_missed_reports(&reporthdr->report, packet);
	ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
	(*reporthdr->output_handler)(&reporthdr->report, sumstats, 0);
	TimeAdd(reporthdr->report.nextTime, reporthdr->report.intervalTime);
	if (reporthdr->multireport && (reporthdr->multireport->refcount > 1)) {
	    nextring_event = 1;
	    reporthdr->multireport->threads++;
	}
    }
    if (reporthdr->bidirreport && \
	(TimeDifference(reporthdr->bidirreport->report.nextTime, packet->packetTime) < 0)) {
	if ((--reporthdr->bidirreport->threads) <= 0) {
	    reporthdr->bidirreport->threads = 2;
	    output_missed_multireports(&reporthdr->bidirreport->report, packet);
	    output_transfer_bidir_report(&reporthdr->bidirreport->report);
	    TimeAdd(reporthdr->report.nextTime, reporthdr->report.intervalTime);
	}
    }
    if (reporthdr->multireport  && (reporthdr->multireport->refcount > 1) && \
	(reporthdr->multireport->threads == reporthdr->multireport->refcount)) {
	reporthdr->multireport->threads = 0;
	reporthdr->multireport->report.packetTime = packet->packetTime;
	// output_missed_multireports(&reporthdr->multireport->report, packet);
	(*reporthdr->output_sum_handler)(&reporthdr->multireport->report, 0);
	TimeAdd(reporthdr->multireport->report.nextTime, reporthdr->report.intervalTime);
    }
    return nextring_event;
}

/*
 * Process reports starting with "reporthdr"
 */
int reporter_process_report ( ReportHeader *reporthdr ) {
    int need_free = 0;

    // Recursively process reports
    if ( reporthdr->next != NULL ) {
        if (reporter_process_report(reporthdr->next)) {
            // Remove the report from the reporter job
	    // list.  Note: This structure is poorly
	    // implemented.  There are two types of jobs,
	    // persistent ones such as transfer reports
	    // and one shot ones such as connection and
	    // settings reports. Clean all of this up
	    // with a c++ based reporter implementation
	    // and live with it for now
            ReportHeader *tmp = reporthdr->next;
            reporthdr->next = reporthdr->next->next;
#ifdef HAVE_THREAD_DEBUG
	    thread_debug( "Remove %p from reporter job queue in rpr", (void *) tmp);
#endif
	    // Free reports that are one-shot. Note that
	    // Transfer Reports get freed by its calling thread,
	    // e.g. client or server. This is because those threads
	    // may need final reporter stats and freeing it in the
	    // reporter thread context makes for a race and locking
	    // to prevent it.  Better is to let the thread that
	    // allocated also free it which can be done in the thread's
	    // destructor
	    if ((tmp->report.type & TRANSFER_REPORT) == 0) {
#ifdef HAVE_THREAD_DEBUG
		thread_debug("Free %p in rpr", (void *) tmp);
#endif
		free(tmp);
	    }
        }
    }
    // This code works but is a mess - fix this and use a proper dispatcher
    // for updating reports and for outputing them
    if ( (reporthdr->report.type & SETTINGS_REPORT) != 0 ) {
        reporthdr->report.type &= ~SETTINGS_REPORT;
        return reporter_print( &reporthdr->report, SETTINGS_REPORT, 1 );
    } else if ( (reporthdr->report.type & CONNECTION_REPORT) != 0 ) {
        reporthdr->report.type &= ~CONNECTION_REPORT;
	need_free = (reporthdr->report.type == 0 ? 1 : 0);
        reporter_print( &reporthdr->report, CONNECTION_REPORT, need_free);
    } else if ( (reporthdr->report.type & SERVER_RELAY_REPORT) != 0 ) {
        reporthdr->report.type &= ~SERVER_RELAY_REPORT;
        return reporter_print( &reporthdr->report, SERVER_RELAY_REPORT, 1 );
    }
    if ( (reporthdr->report.type & TRANSFER_REPORT) != 0 ) {
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
	ReportStruct *packet = NULL;
	int nextring_event = 0;
        while (!nextring_event && (packet = dequeue_packetring(reporthdr))) {
	    // Increment the total packet count processed by this thread
	    // this will be used to make decisions on if the reporter
	    // thread should add some delay to eliminate cpu thread
	    // thrashing,
	    consumption_detector.accounted_packets--;
	    // update fields common to TCP and UDP, client and server which is bytes
	    reporthdr->report.TotalLen += packet->packetLen;
	    // Check for a final packet event on this packet ring
	    if (!(packet->packetID < 0)) {
		// Check for interval reports
	        nextring_event = condprint_interval_reports(reporthdr, packet);
		// Do the packet accounting per the handler type
		if (reporthdr->packet_handler) {
		  (*reporthdr->packet_handler)(reporthdr, packet);
		  if (reporthdr->bidirreport) {
		    reporthdr->bidirreport->report.TotalLen += packet->packetLen;
		  }
		  if (reporthdr->multireport) {
		      reporthdr->multireport->report.packetTime = packet->packetTime;
		  }
		}
	    } else {
	        // A last packet event was detected
		// printf("last packet event detected\n"); fflush(stdout);
	        need_free = 1;
		reporthdr->delaycounter = consumption_detector.delay_counter;
		// output final reports
		reporthdr->report.packetTime=packet->packetTime;
		ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
		(*reporthdr->output_handler)(&reporthdr->report, sumstats, 1);

		if (reporthdr->bidirreport)
		    output_transfer_bidir_final_report(&reporthdr->bidirreport->report);
		if (reporthdr->multireport && (reporthdr->multireport->refcount > 1)) {
		    if (++reporthdr->multireport->threads == reporthdr->multireport->refcount) {
			reporthdr->multireport->report.packetTime=packet->packetTime;
			(*reporthdr->output_sum_handler)(&reporthdr->multireport->report, 1);
		    }
		}
		// Thread is done with the packet ring, signal back to the traffic thread
		// which will proceed from the EndReport wait, this must be the last thing done
		reporthdr->packetring->consumerdone = 1;
	    }
	}
    }
    // need_free is a poor implementation.  It's done this way
    // because of the recursion.  It also signals two things,
    // one is remove from the reporter's job queue and the second
    // is to free the report's memory which was dynamically allocated
    // by another thread.  This is a good thing to fix with a c++
    // version of the reporter
    return need_free;
}

/*
 * Updates connection stats
 */
#define L2DROPFILTERCOUNTER 100

static inline void reporter_handle_packet_pps(ReporterData *data, Transfer_Info *stats) {
    data->cntDatagrams++;
    stats->IPGsum += TimeDifference(data->packetTime, data->IPGstart);
    stats->IPGcnt++;
    data->IPGstart = data->packetTime;
}

static inline void reporter_handle_packet_udp_transit(ReporterData *data, Transfer_Info *stats, ReportStruct *packet) {
    // Transit or latency updates done inline below
    double transit;
    transit = TimeDifference(packet->packetTime, packet->sentTime);
    double usec_transit = transit * 1e6;
    if (stats->latency_histogram) {
	histogram_insert(stats->latency_histogram, transit);
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
}

#ifdef HAVE_ISOCHRONOUS
static inline void reporter_handle_packet_isochronous(ReporterData *data, Transfer_Info *stats, ReportStruct *packet) {
  //printf("fid=%lu bs=%lu remain=%lu\n", packet->frameID, packet->burstsize, packet->remaining);
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
	histogram_insert(stats->framelatency_histogram, frametransit);
	matchframeid = 0;  // reset the matchid so any potential duplicate is ignored
      }
    }
    data->isochstats.frameID = packet->frameID;
  }
}
#endif

inline void reporter_handle_packet_server_tcp(ReportHeader *reporthdr, ReportStruct *packet) {
    Transfer_Info *stats = &reporthdr->report.info;
    if (packet->packetLen > 0) {
	int bin;
	// mean min max tests
	stats->sock_callstats.read.cntRead++;
	stats->sock_callstats.read.totcntRead++;
	bin = (int)floor((packet->packetLen -1)/stats->sock_callstats.read.binsize);
	if (bin < TCPREADBINCOUNT) {
	    stats->sock_callstats.read.bins[bin]++;
	    stats->sock_callstats.read.totbins[bin]++;
	}
    }
}

inline void reporter_handle_packet_server_udp(ReportHeader *reporthdr, ReportStruct *packet) {
    ReporterData *data = &reporthdr->report;
    Transfer_Info *stats = &reporthdr->report.info;

    data->packetTime = packet->packetTime;
    stats->socket = packet->socket;

    // These are valid packets that need standard iperf accounting
    if (packet->emptyreport) {
	if (stats->transit.cntTransit == 0) {
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
	}
	return;
    }
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
    reporter_handle_packet_pps(data, stats);
    reporter_handle_packet_udp_transit(data, stats, packet);
    reporter_handle_packet_isochronous(data, stats, packet);
    if (reporthdr->multireport) {
	ReporterData *sumdata = &reporthdr->multireport->report;
	Transfer_Info *sumstats = &reporthdr->multireport->report.info;
	sumdata->TotalLen += packet->packetLen;
	reporter_handle_packet_pps(sumdata, sumstats);
    }
}

void reporter_handle_packet_client(ReportHeader *reporthdr, ReportStruct *packet) {
    ReporterData *data = &reporthdr->report;
    Transfer_Info *stats = &reporthdr->report.info;

    data->packetTime = packet->packetTime;
    stats->socket = packet->socket;
    if (packet->errwrite) {
	if (packet->errwrite != WriteErrNoAccount) {
	    stats->sock_callstats.write.WriteErr++;
	    stats->sock_callstats.write.totWriteErr++;
	}
    } else {
	stats->sock_callstats.write.WriteCnt++;
	stats->sock_callstats.write.totWriteCnt++;
    }
    // These are valid packets that need standard iperf accounting
    if (!packet->emptyreport && isUDP(data)) {
	reporter_handle_packet_pps(data, stats);
    }
}


#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats (ReporterData *stats, ReporterData *sumstats, int final) {
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
static inline void output_missed_reports(ReporterData *stats, ReportStruct *packet) {
    while ((stats->intervalTime.tv_sec != 0 || \
	    stats->intervalTime.tv_usec != 0) && \
	   TimeDifference(stats->nextTime, stats->packetTime ) < 0 ) {
	stats->info.startTime = stats->info.endTime;
	stats->info.endTime = TimeDifference( stats->nextTime, stats->startTime );
	TimeAdd(stats->nextTime, stats->intervalTime);
	if (TimeDifference(stats->nextTime, stats->packetTime) < 0) {
	    ReporterData emptystats;
	    memset(&emptystats, 0, sizeof(ReporterData));
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
}
// If reports were missed, catch up now
static inline void output_missed_multireports(ReporterData *stats, ReportStruct *packet) {
    output_missed_reports(stats, packet);
}

static inline void set_endtime(ReporterData *stats) {
  stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
  // There is a corner case when the first packet is also the last where the start time (which comes
  // from app level syscall) is greater than the packetTime (which come for kernel level SO_TIMESTAMP)
  // For this case set the start and end time to both zero.
  if (stats->info.endTime < 0) {
    stats->info.endTime = 0;
  }
}

// Actions required after an interval report has been outputted
static inline void reset_transfer_stats(ReporterData *stats) {
    stats->info.startTime = stats->info.endTime;
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
	    stats->info.sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
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
#ifdef HAVE_ISOCHRONOUS
	    stats->info.isochstats.framecnt = 0;
	    stats->info.isochstats.framelostcnt = 0;
	    stats->info.isochstats.slipcnt = 0;
#endif
	}
    }
}

static inline void reset_transfer_stats_client(ReporterData *stats) {
    stats->info.startTime = stats->info.endTime;
    stats->lastTotal = stats->TotalLen;
    stats->info.sock_callstats.write.WriteCnt = 0;
    stats->info.sock_callstats.write.WriteErr = 0;
    stats->info.sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    stats->info.sock_callstats.write.up_to_date = 0;
#endif
}
static inline void reset_transfer_stats_server_tcp(ReporterData *stats) {
    int ix;
    stats->info.startTime = stats->info.endTime;
    stats->lastTotal = stats->TotalLen;
    stats->info.sock_callstats.read.cntRead = 0;
    for (ix = 0; ix < 8; ix++) {
	stats->info.sock_callstats.read.bins[ix] = 0;
    }
}
static inline void reset_transfer_stats_server_udp(ReporterData *stats) {
    // Reset the enhanced stats for the next report interval
    stats->info.startTime = stats->info.endTime;
    stats->lastTotal = stats->TotalLen;
    stats->info.transit.minTransit=stats->info.transit.lastTransit;
    stats->info.transit.maxTransit=stats->info.transit.lastTransit;
    stats->info.transit.sumTransit = stats->info.transit.lastTransit;
    stats->info.transit.cntTransit = 0;
    stats->info.transit.vdTransit = 0;
    stats->info.transit.meanTransit = 0;
    stats->info.transit.m2Transit = 0;
#ifdef HAVE_ISOCHRONOUS
    stats->info.isochstats.framecnt = 0;
    stats->info.isochstats.framelostcnt = 0;
    stats->info.isochstats.slipcnt = 0;
#endif
}

// These are the output handlers that get the reports ready and then prints them
static void output_transfer_report_server_udp(ReporterData *stats, ReporterData *sumstats, int final) {
    stats->info.cntOutofOrder = stats->cntOutofOrder - stats->lastOutofOrder;
    // assume most of the  time out-of-order packets are not
    // duplicate packets, so conditionally subtract them from the lost packets.
    stats->info.cntError = stats->cntError - stats->lastError;
    stats->info.cntError -= stats->info.cntOutofOrder;
    stats->info.cntDatagrams = ((stats->info.mUDP == kMode_Server) ? stats->PacketID - stats->lastDatagrams :
				stats->cntDatagrams - stats->lastDatagrams);
    stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
    reporter_print(stats, TRANSFER_REPORT, 0);
    reset_transfer_stats_server_udp(stats);
}

static void output_transfer_report_server_tcp(ReporterData *stats, ReporterData *sumstats, int final) {
    set_endtime(stats);
    int ix;
    if (final) {
        stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
        stats->info.sock_callstats.read.cntRead = stats->info.sock_callstats.read.totcntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->info.sock_callstats.read.bins[ix] = stats->info.sock_callstats.read.totbins[ix];
        }
    } else {
        stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
    }
    if (sumstats) {
        sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
        sumstats->info.sock_callstats.read.cntRead += stats->info.sock_callstats.read.cntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    sumstats->info.sock_callstats.read.bins[ix] += stats->info.sock_callstats.read.bins[ix];
        }
    }
    reporter_print(stats, TRANSFER_REPORT, 0);
    reset_transfer_stats_server_tcp(stats);
}

static void output_transfer_report_client_udp(ReporterData *stats, ReporterData *sumstats, int final) {
    stats->info.cntOutofOrder = stats->cntOutofOrder - stats->lastOutofOrder;
    // assume most of the  time out-of-order packets are not
    // duplicate packets, so conditionally subtract them from the lost packets.
    stats->info.cntError = stats->cntError - stats->lastError;
    stats->info.cntError -= stats->info.cntOutofOrder;
    stats->info.cntDatagrams = ((stats->info.mUDP == kMode_Server) ? stats->PacketID - stats->lastDatagrams :
				stats->cntDatagrams - stats->lastDatagrams);
    stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
    reporter_print(stats, TRANSFER_REPORT, 0);
    reset_transfer_stats(stats);
}

static void output_transfer_report_client_tcp(ReporterData *stats, ReporterData *sumstats, int final) {
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
    if (final) {
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.sock_callstats.write.TCPretry = stats->info.sock_callstats.write.totTCPretry;
	stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
	stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
	reporter_print(stats, TRANSFER_REPORT, 1);
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	stats->info.endTime = TimeDifference(stats->nextTime, stats->startTime);
	reporter_print(stats, TRANSFER_REPORT, 0);
	reset_transfer_stats_server_udp(stats);
    }
}

static void output_transfer_final_report_client_tcp(ReporterData *stats) {
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
        stats->info.endTime = TimeDifference( stats->packetTime, stats->startTime );

	// There is a corner case when the first packet is also the last where the start time (which comes
	// from app level syscall) is greater than the packetTime (which come for kernel level SO_TIMESTAMP)
	// For this case set the start and end time to both zero.
	if (stats->info.endTime < 0) {
	    stats->info.endTime = 0;
	}
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
#ifdef HAVE_ISOCHRONOUS
	if (stats->info.mIsochronous) {
	    stats->info.isochstats.framecnt = stats->isochstats.framecnt;
	    stats->info.isochstats.framelostcnt = stats->isochstats.framelostcnt;
	    stats->info.isochstats.slipcnt = stats->isochstats.slipcnt;
	}
#endif
        reporter_print( stats, TRANSFER_REPORT, 1 );
}

/*
 * Handles summing of threads
 */
static void output_transfer_sum_report_client_tcp(ReporterData *stats, int final) {
    set_endtime(stats);
    if (final) {
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.sock_callstats.write.TCPretry = stats->info.sock_callstats.write.totTCPretry;
	stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
	reporter_print( stats, MULTIPLE_REPORT, 1 );
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print( stats, MULTIPLE_REPORT, 0 );
	reset_transfer_stats_client(stats);
    }
}

static void output_transfer_sum_report_server_tcp(ReporterData *stats, int final) {
    set_endtime(stats);
    if (final) {
	int ix;
	stats->info.startTime = 0.0;
	stats->info.TotalLen = stats->TotalLen;
	stats->info.sock_callstats.read.cntRead = stats->info.sock_callstats.read.totcntRead;
	for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->info.sock_callstats.read.bins[ix] = stats->info.sock_callstats.read.totbins[ix];
	}
	reporter_print( stats, MULTIPLE_REPORT, 1 );
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print( stats, MULTIPLE_REPORT, 0 );
	reset_transfer_stats_server_tcp(stats);
    }
}

static void output_transfer_bidir_report(ReporterData *stats) {
}
static void output_transfer_bidir_final_report(ReporterData *stats) {
}

/*
 * This function handles multiple format printing by sending to the
 * appropriate dispatch function
 */
int reporter_print( ReporterData *stats, int type, int end ) {
    switch ( type ) {
        case TRANSFER_REPORT:
            statistics_reports[stats->mode]( &stats->info );
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

void PrintMSS( ReporterData *stats ) {
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
