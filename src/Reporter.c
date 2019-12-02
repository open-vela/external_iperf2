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

char buffer[SNBUFFERSIZE]; // Buffer for printing
struct ReportHeader *ReportRoot = NULL;
struct ReportHeader *ReportPendingHead = NULL;
struct ReportHeader *ReportPendingTail = NULL;
int reporter_process_report ( struct ReportHeader *report );
void process_report ( struct ReportHeader *report );
int reporter_print( struct ReporterData *stats, int type, int end );
void PrintMSS( struct ReporterData *stats );

static void reporter_handle_packet_null(struct ReportHeader *report, struct ReportStruct *packet) {return;}
static void output_transfer_report_null(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final){return;}

// Private routines
// Packet accounting:
static void reporter_handle_packet_server_udp(struct ReportHeader *report, struct ReportStruct *packet);
static void reporter_handle_packet_server_tcp(struct ReportHeader *report, struct ReportStruct *packet);
static void reporter_handle_packet_client(struct ReportHeader *report, struct ReportStruct *packet);

// Reporter ouput
static int condprint_interval_reports (struct ReportHeader *reporthdr, struct ReportStruct *packet);
static void output_missed_reports(struct ReporterData *stats, struct ReportStruct *packet);
static void output_missed_multireports(struct ReporterData *stats, struct ReportStruct *packet);
static void output_transfer_report_client_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void output_transfer_report_client_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void output_transfer_report_server_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void output_transfer_report_server_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
static void output_transfer_sum_report_client_tcp(struct ReporterData *stats, int final);
static void output_transfer_sum_report_server_tcp(struct ReporterData *stats, int final);
static void output_transfer_sum_report_client_udp(struct ReporterData *stats, int final);
static void output_transfer_sum_report_server_udp(struct ReporterData *stats, int final);
static void output_transfer_bidir_report_tcp(struct ReporterData *stats, int final);
static void output_transfer_bidir_report_udp(struct ReporterData *stats, int final);
static void output_connect_final_report_tcp(struct MultiHeader *multihdr);

static void reset_transfer_stats(struct ReporterData *stats);
static inline void reset_transfer_stats_client_tcp(struct ReporterData *stats);
static inline void reset_transfer_stats_client_udp(struct ReporterData *stats);
static inline void reset_transfer_stats_server_udp(struct ReporterData *stats);
static inline void reset_transfer_stats_server_tcp(struct ReporterData *stats);

static void InitDataReport(struct thread_Settings *mSettings);
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
static void gettcpistats(struct ReporterData *stats, struct ReporterData *sumstats, int final);
#endif

struct MultiHeader* InitSumReport(struct thread_Settings *agent, int inID) {
    struct MultiHeader *multihdr = (struct MultiHeader *) calloc(1, sizeof(struct MultiHeader));
    if ( multihdr != NULL ) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Init multiheader sum report %p id=%d", (void *)multihdr, inID);
#endif
        agent->multihdr = multihdr;
	multihdr->groupID = inID;
	multihdr->refcount = 0;
	Mutex_Initialize(&multihdr->refcountlock);
	multihdr->threads = 0;
	multihdr->connect_times.min = FLT_MAX;
	multihdr->connect_times.max = FLT_MIN;
	multihdr->connect_times.vd = 0;
	multihdr->connect_times.m2 = 0;
	multihdr->connect_times.mean = 0;
	if (isMultipleReport(agent)) {
	    struct ReporterData *data = &multihdr->report;
	    data->type = TRANSFER_REPORT;
	    // Only initialize the interval time here
	    // The startTime and nextTime for summing reports will be set by
	    // the reporter thread in realtime
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
	    if (data->mThreadMode == kMode_Server) {
		data->info.sock_callstats.read.binsize = data->mBufLen / 8;
	    }
	    if ( isEnhanced( agent ) ) {
		data->info.mEnhanced = 1;
		multihdr->output_connect_handler = output_connect_final_report_tcp;
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

struct MultiHeader* InitBiDirReport(struct thread_Settings *agent, int inID) {
    struct MultiHeader *multihdr = (struct MultiHeader *) calloc(1, sizeof(struct MultiHeader));
    if ( multihdr != NULL ) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Init multiheader bidir report %p id=%d", (void *)multihdr, inID);
#endif
        agent->bidirhdr = multihdr;
	multihdr->groupID = inID;
	multihdr->refcount = 0;
	Mutex_Initialize(&multihdr->refcountlock);
	if (isMultipleReport(agent)) {
	    struct ReporterData *data = &multihdr->report;
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
void BarrierClient(struct MultiHeader *multihdr, int starttime) {
#ifdef HAVE_THREAD
    assert(multihdr == NULL);
    Condition_Lock(multihdr->multibarrier_cond);
    multihdr->multibarrier_cnt--;
    if ( multihdr->multibarrier_cnt == 0 ) {
        // store the wake up or start time in the shared multihdr
        if (starttime) {
#ifdef HAVE_CLOCK_GETTIME
            struct timespec t1;
            clock_gettime(CLOCK_REALTIME, &t1);
            multihdr->report.startTime.tv_sec  = t1.tv_sec;
            multihdr->report.startTime.tv_usec = t1.tv_nsec / 1000;
#else
            gettimeofday( &multihdr->report.startTime, NULL );
#endif
	    multihdr->report.nextTime= multihdr->report.startTime;
	    TimeAdd(multihdr->report.nextTime, multihdr->report.intervalTime);
	}
        // last one wake's up everyone else
        Condition_Broadcast(&multihdr->multibarrier_cond);
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Barrier BROADCAST on condition %p", (void *)&multihdr->multibarrier_cond);
#endif
    } else {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Barrier WAIT on condition %p count=%d", (void *)&multihdr->multibarrier_cond, multihdr->multibarrier_cnt);
#endif
        Condition_Wait(&multihdr->multibarrier_cond);
    }
    multihdr->multibarrier_cnt++;
    Condition_Unlock(multihdr->multibarrier_cond);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Barrier EXIT on condition %p", (void *)&multihdr->multibarrier_cond);
#endif

#else
    if (starttime) {
        gettimeofday( &(multihdr->report.startTime), NULL );
	TimeAdd(multihdr->report.nextTime, multihdr->report.intervalTime);
    }
#endif
}

/*
 * InitReport is called by a transfer agent (client or
 * server) to setup the needed structures to communicate
 * traffic and connection information.  Also initialize
 * the report start time and next interval report time
 * Finally, in the case of parallel clients, have them all
 * synchronize on compeleting their connect()
 */

void InitReport(struct thread_Settings *mSettings) {
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

void UpdateMultiHdrRefCounter(struct MultiHeader *multihdr, int val, int sockfd) {
    if (!multihdr)
	return;
    int need_free = 0;
    // decrease the reference counter for mutliheaders
    // and check to free the multiheader
    Mutex_Lock(&multihdr->refcountlock);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Sum multiheader %p ref=%d->%d", (void *)multihdr, \
		 multihdr->refcount, (multihdr->refcount + val));
#endif
    if ((multihdr->refcount == 0) && (val > 0)) {
	multihdr->sockfd = sockfd;
    }
    multihdr->refcount += val;
    if (multihdr->refcount > multihdr->maxrefcount)
	multihdr->maxrefcount = multihdr->refcount;
    if ((multihdr->maxrefcount > 1) && (multihdr->refcount == 0) && (val < 0)) {
	// Output a final report before freeing it
	if (*multihdr->output_sum_handler)
	    (*multihdr->output_sum_handler)(&multihdr->report, 1);
	if (*multihdr->output_connect_handler)
	    (*multihdr->output_connect_handler)(multihdr);

	if (sockfd && (multihdr->sockfd == sockfd)) {
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Close socket %d per last reference", sockfd);
#endif
	    int rc = close(multihdr->sockfd);
	    WARN_errno( rc == SOCKET_ERROR, "client bidir close" );
	}
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Free sum multiheader %p per last reference", (void *)multihdr);
#endif
	need_free = 1;
    }
    Mutex_Unlock(&multihdr->refcountlock);
    if (need_free) {
	Condition_Destroy(&multihdr->multibarrier_cond);
	Mutex_Destroy(&multihdr->refcountlock);
	free(multihdr);
    }
}

void FreeReport(struct ReportHeader *reporthdr) {
    if (reporthdr) {
	if (reporthdr->packetring && (reporthdr->reporter_thread_suspends < 3)) {
	  fprintf(stdout, "WARN: this test was likley CPU bound (%d) (or may not be detecting the underlying network devices)\n", reporthdr->reporter_thread_suspends);
	}
	if (reporthdr->packetring) {
	    free_packetring(reporthdr->packetring);
	}
	if (reporthdr->report.info.latency_histogram) {
	    histogram_delete(reporthdr->report.info.latency_histogram);
	}
	if (reporthdr->report.info.framelatency_histogram) {
	    histogram_delete(reporthdr->report.info.framelatency_histogram);
	}
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Free report hdr=%p reporter thread suspend count=%d packetring=%p histo=%p frame histo=%p", \
		     (void *)reporthdr, reporthdr->reporter_thread_suspends, (void *) reporthdr->packetring, \
		     (void *)reporthdr->report.info.latency_histogram, (void *) reporthdr->report.info.framelatency_histogram);
#endif
	free(reporthdr);
    }
}

void InitDataReport(struct thread_Settings *mSettings) {
     /*
     * Create in one big chunk
     */
    struct ReportHeader *reporthdr = (struct ReportHeader *) calloc(1, sizeof(struct ReportHeader));
    struct ReporterData *data = NULL;

    if ( reporthdr != NULL ) {
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Job report %p uses multireport %p and bidirreport is %p", (void *)mSettings->reporthdr, (void *)mSettings->multihdr, (void *)mSettings->bidirhdr);
#endif
	mSettings->reporthdr = reporthdr;
	reporthdr->multireport = mSettings->multihdr;
	reporthdr->bidirreport = mSettings->bidirhdr;
	if (reporthdr->bidirreport) {
	    reporthdr->bidirreport->report.info.transferID = mSettings->mSock;
	}
	data = &reporthdr->report;
	data->mThreadMode = mSettings->mThreadMode;
	reporthdr->packet_handler = NULL;
	if (reporthdr->multireport)
	    reporthdr->multireport->output_connect_handler = NULL;
	if (!isConnectOnly(mSettings)) {
	    // Create a new packet ring which is used to communicate
	    // packet stats from the traffic thread to the reporter
	    // thread.  The reporter thread does all packet accounting
	    reporthdr->packetring = init_packetring((mSettings->numreportstructs ? mSettings->numreportstructs : NUM_REPORT_STRUCTS), \
						    &ReportCond, &mSettings->awake_me);
	    if (mSettings->numreportstructs)
	        fprintf (stdout, "[%3d] NUM_REPORT_STRUCTS override from %d to %d\n", mSettings->mSock, NUM_REPORT_STRUCTS, mSettings->numreportstructs);

	    // Set up the function vectors, there are three
	    // 1) packet_handler: does packet accounting per the test and protocol
	    // 2) output_handler: performs output, e.g. interval reports, per the test and protocol
	    // 3) output_sum_handler: performs summing output when multiple traffic threads

	    switch (data->mThreadMode) {
	    case kMode_Server :
		if (isUDP(mSettings)) {
		    reporthdr->packet_handler = reporter_handle_packet_server_udp;
		    reporthdr->output_handler = output_transfer_report_server_udp;
		    if (reporthdr->multireport)
		      reporthdr->multireport->output_sum_handler = output_transfer_sum_report_server_udp;
		    if (reporthdr->bidirreport)
		      reporthdr->bidirreport->output_sum_handler = output_transfer_bidir_report_udp;
		} else {
		    reporthdr->packet_handler = reporter_handle_packet_server_tcp;
		    reporthdr->output_handler = output_transfer_report_server_tcp;
		    if (reporthdr->multireport)
		        reporthdr->multireport->output_sum_handler = output_transfer_sum_report_server_tcp;
		    if (reporthdr->bidirreport)
		        reporthdr->bidirreport->output_sum_handler = output_transfer_bidir_report_tcp;
		}
		break;
	    case kMode_Client :
		reporthdr->packet_handler = reporter_handle_packet_client;
		if (isUDP(mSettings)) {
		    reporthdr->output_handler = output_transfer_report_client_udp;
		    if (reporthdr->multireport)
		      reporthdr->multireport->output_sum_handler = output_transfer_sum_report_client_udp;
		    if (reporthdr->bidirreport)
		      reporthdr->bidirreport->output_sum_handler = output_transfer_bidir_report_udp;
		} else {
		    reporthdr->output_handler = output_transfer_report_client_tcp;
		    if (reporthdr->multireport) {
		        reporthdr->multireport->output_sum_handler = output_transfer_sum_report_client_tcp;
			if (isEnhanced(mSettings))
			    reporthdr->multireport->output_connect_handler = output_connect_final_report_tcp;
		    }
		    if (reporthdr->bidirreport)
		        reporthdr->bidirreport->output_sum_handler = output_transfer_bidir_report_tcp;
		}
		break;
	    case kMode_WriteAckClient :
	        reporthdr->packet_handler = reporter_handle_packet_null;
		reporthdr->output_handler = output_transfer_report_null;
		break;
	    case kMode_Unknown :
	    case kMode_Reporter :
	    case kMode_ReporterClient :
	    case kMode_Listener:
	    default:
		reporthdr->packet_handler = NULL;
	    }
	    // increment the reference counters for bidir and sum reports
	    if (mSettings->bidirhdr != NULL) {
	        UpdateMultiHdrRefCounter(mSettings->bidirhdr, 1, mSettings->mSock);
	    }
	    if (mSettings->multihdr != NULL) {
	        UpdateMultiHdrRefCounter(mSettings->multihdr, 1, 0);
	    }
	}

#ifdef HAVE_THREAD_DEBUG
	thread_debug("Init data report %p size %ld using packetring=%p cond=%p", \
		     (void *)reporthdr, sizeof(struct ReportHeader),
		     (void *)(reporthdr->packetring), (void *)(reporthdr->packetring->awake_producer));
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
	    reporthdr->output_interval_report_handler = condprint_interval_reports;
	} else {
	    reporthdr->output_interval_report_handler = NULL;
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
	data->flags_extend = mSettings->flags_extend;
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
	data->info.flags_extend = mSettings->flags_extend;
	if (data->mThreadMode == kMode_Server) {
	    if (isRxHistogram(mSettings) && isUDP(mSettings)) {
		char name[] = "T8";
		data->info.latency_histogram =  histogram_init(mSettings->mRXbins,mSettings->mRXbinsize,0,\
							       pow(10,mSettings->mRXunits), \
							       mSettings->mRXci_lower, mSettings->mRXci_upper, data->info.transferID, name);
	    }
	    if (isRxHistogram(mSettings) && (isIsochronous(mSettings) || isTripTime(mSettings))) {
		char name[] = "F8";
		// make sure frame bin size min is 100 microsecond
		data->info.framelatency_histogram =  histogram_init(mSettings->mRXbins,mSettings->mRXbinsize,0, \
								    pow(10,mSettings->mRXunits), mSettings->mRXci_lower, \
								    mSettings->mRXci_upper, data->info.transferID, name);
	    }
	}
	if ( isIsochronous( mSettings ) ) {
	    data->info.mIsochronous = 1;
	} else {
	    data->info.mIsochronous = 0;
	}
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
void InitConnectionReport (struct thread_Settings *mSettings) {
    struct ReportHeader *reporthdr = mSettings->reporthdr;
    struct ReporterData *data = NULL;
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init connection report %p", reporthdr);
#endif

    if (reporthdr == NULL) {
	/*
	 * We don't have a Data Report structure in which to hang
	 * the connection report so allocate a minimal one
	 */
	reporthdr = calloc( sizeof(struct ReportHeader), sizeof(char*) );
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
    data->connection.mThreadMode = mSettings->mThreadMode;
    // Set the l2mode flags
    data->connection.l2mode = isL2LengthCheck(mSettings);
    if (data->connection.l2mode)
	data->connection.l2mode = ((isIPV6(mSettings) << 1) | data->connection.l2mode);
    if (isEnhanced(mSettings)) {
      if (isTxStartTime(mSettings)) {
	data->connection.epochStartTime.tv_sec = mSettings->txstart_epoch.tv_sec;
	data->connection.epochStartTime.tv_usec = mSettings->txstart_epoch.tv_usec;
      } else if (isTripTime(mSettings)) {
	data->connection.epochStartTime.tv_sec = mSettings->accept_time.tv_sec;
	data->connection.epochStartTime.tv_usec = mSettings->accept_time.tv_usec;
      }
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
    data->connection.WriteAckLen = (mSettings->mWriteAckLen > 0) ? mSettings->mWriteAckLen : mSettings->mBufLen;
    if (mSettings->mSock > 0)
	UpdateConnectionReport(mSettings, reporthdr);
    if (isConnectOnly(mSettings) && mSettings->multihdr) {
	UpdateMultiHdrRefCounter(mSettings->multihdr, 1, 0);
    }
}

// Read the actual socket window size data
void UpdateConnectionReport(struct thread_Settings *mSettings, struct ReportHeader *reporthdr) {
    if (reporthdr != NULL) {
        struct ReporterData *data = &reporthdr->report;
	data->info.transferID = mSettings->mSock;
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

void PostReport (struct ReportHeader *reporthdr) {
#ifdef HAVE_THREAD_DEBUG
    thread_debug( "Jobq report %p (0x%X) post", reporthdr, reporthdr->report.type);
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
	  thread_debug("Reporting last packet for %p  qdepth=%d", (void *) agent, getcount_packetring(agent->packetring));
	}
#endif
        enqueue_packetring(agent->packetring, packet);
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
void CloseReport(struct ReportHeader *agent, struct ReportStruct *finalpacket) {
    if ( agent != NULL) {
	struct ReportStruct packet;
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
void EndReport( struct ReportHeader *agent ) {
    if ( agent != NULL ) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug( "Traffic thread awaiting reporter to be done with %p and cond %p", (void *)agent, (void *) agent->packetring->awake_producer);
#endif
        Condition_Lock((*(agent->packetring->awake_producer)));
	while (!agent->packetring->consumerdone) {
	    Condition_TimedWait(agent->packetring->awake_producer, 1);
	    // printf("Consumer done may be stuck\n");
	}
	Condition_Unlock((*(agent->packetring->awake_producer)));
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
#define REPORTERDELAY_DURATION 4000 // units is microseconds
typedef struct ConsumptionDetectorType {
    int accounted_packets;
    int accounted_packet_threads;
    int reporter_thread_suspends ;
} ConsumptionDetectorType;
ConsumptionDetectorType consumption_detector = \
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
  int wait;
  wait = 3;
  thread_debug("reporter thread job queue request lock (with %ld second wait)", wait);
  Condition_TimedLock(ReportCond, wait);
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
	// Signal the producer (traffic thread) that the consumer (reporter thread)
	// it is done with this report.
	Condition_Signal(entry->packetring->awake_producer);
    }
}

/* Concatenate pending reports and return the head */
static inline struct ReportHeader *reporter_jobq_set_root(void) {
    Condition_Lock(ReportCond);
    // check the jobq for empty
    if (ReportRoot == NULL) {
	// The reporter is starting from an empty state
	// so set the load detect to trigger an initial delay
	reset_consumption_detector();
	if (!ReportPendingHead) {
	    Condition_TimedWait(&ReportCond, 1);
#ifdef HAVE_THREAD_DEBUG
	    thread_debug( "Jobq *WAIT* exit  %p ", (void *) ReportPendingHead);
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
    Condition_Unlock(ReportCond);
    return ReportRoot;
}
/*
 * This function is the loop that the reporter thread processes
 */
void reporter_spawn( struct thread_Settings *thread ) {
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
	// thread_debug( "Jobq *HEAD* %p (%d)", (void *) ReportRoot, thread_numtrafficthreads());
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
		if (reporter_process_report(*work_item)) {
		    struct ReportHeader *tmp = *work_item;
		    *work_item = (*work_item)->next;
#ifdef HAVE_THREAD_DEBUG
		    thread_debug( "Jobq *FREE* %p (%X) (%p)", (void *) tmp, tmp->report.type,(void *) *work_item);
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
        if ( reporter_process_report( report ) ) {
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

static int condprint_interval_reports (struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    int advance_jobq = 0;
    // Print a report if packet time exceeds the next report interval time,
    // Also signal to the caller to move to the next report (or packet ring)
    // if there was output. This will allow for more precise interval sum accounting.
    if (TimeDifference(reporthdr->report.nextTime, packet->packetTime) < 0) {
        // In the (hopefully unlikely event) the reporter fell behind
        // ouput the missed reports to catch up
        // output_missed_reports(&reporthdr->report, packet);
	struct ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
	struct ReporterData *bidirstats = (reporthdr->bidirreport ? &reporthdr->bidirreport->report : NULL);
	WARN(!*reporthdr->output_handler, "Transfer output handler is not set:");
	(*reporthdr->output_handler)(&reporthdr->report, sumstats, bidirstats, 0);
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
	// output_missed_multireports(&reporthdr->multireport->report, packet);
	(*reporthdr->bidirreport->output_sum_handler)(&reporthdr->bidirreport->report, 0);
	TimeAdd(reporthdr->bidirreport->report.nextTime, reporthdr->report.intervalTime);
    }
    if (reporthdr->multireport && (reporthdr->multireport->refcount > (reporthdr->bidirreport ? 2 : 1)) && \
	(reporthdr->multireport->threads == reporthdr->multireport->refcount))  {
	reporthdr->multireport->threads = 0;
	// output_missed_multireports(&reporthdr->multireport->report, packet);
	(*reporthdr->multireport->output_sum_handler)(&reporthdr->multireport->report, 0);
	TimeAdd(reporthdr->multireport->report.nextTime, reporthdr->report.intervalTime);
    }
    return advance_jobq;
}

static void reporter_compute_connect_times (struct MultiHeader *hdr, double connect_time) {
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
int reporter_process_report (struct ReportHeader *reporthdr) {
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
	} else if ( (reporthdr->report.type & CONNECTION_REPORT) != 0 ) {
	    reporthdr->report.type &= ~CONNECTION_REPORT;
	    need_free = (reporthdr->report.type == 0 ? 1 : 0);
	    if (reporthdr->multireport) {
	        reporter_compute_connect_times(reporthdr->multireport, reporthdr->report.connection.connecttime);
	    }
	    reporter_print( &reporthdr->report, CONNECTION_REPORT, need_free);
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
	    while (!advance_jobq) {
		if ((packet = dequeue_packetring(reporthdr->packetring))) {
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
			if (reporthdr->output_interval_report_handler) {
			    advance_jobq = (*reporthdr->output_interval_report_handler)(reporthdr, packet);
			}
			// update fields common to TCP and UDP, client and server which is bytes and packet time
			reporthdr->report.TotalLen += packet->packetLen;
			reporthdr->report.packetTime = packet->packetTime;
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
			// output final reports
			reporthdr->report.TotalLen += packet->packetLen;
			reporthdr->report.packetTime=packet->packetTime;
			struct ReporterData *sumstats = (reporthdr->multireport ? &reporthdr->multireport->report : NULL);
			struct ReporterData *bidirstats = (reporthdr->bidirreport ? &reporthdr->bidirreport->report : NULL);
			(*reporthdr->output_handler)(&reporthdr->report, sumstats, bidirstats, 1);
			// Thread is done with the packet ring, signal back to the traffic thread
			// which will proceed from the EndReport wait, this must be the last thing done
			reporthdr->packetring->consumerdone = 1;
			// This is a final report so set the sum report header's packet time
			// Note, the thread with the max value will set this
			// Also note, the final sum report output occurs as part of freeing the
			// sum or bidir report per the last reference and not here
			if (reporthdr->multireport && \
			    (TimeDifference(reporthdr->multireport->report.packetTime, packet->packetTime) > 0))
			    reporthdr->multireport->report.packetTime = packet->packetTime;
			if (reporthdr->bidirreport && \
			    (TimeDifference(reporthdr->bidirreport->report.packetTime, packet->packetTime) > 0))
			    reporthdr->bidirreport->report.packetTime = packet->packetTime;
		    }
		} else {
		    advance_jobq = 1;
		    apply_consumption_detector();
		    if (reporthdr->output_interval_report_handler) {
		      // Interval reports need packet events
		      // so generate them
		      struct ReportStruct nullpacket;
#ifdef HAVE_CLOCK_GETTIME
		      struct timespec t1;
		      clock_gettime(CLOCK_REALTIME, &t1);
		      nullpacket.packetTime.tv_sec  = t1.tv_sec;
		      nullpacket.packetTime.tv_usec = t1.tv_nsec / 1000;
#else
		      gettimeofday(&nullpacket.packetTime, NULL);
#endif
		      nullpacket.packetLen = 0;
		      nullpacket.packetID = reporthdr->report.lastDatagrams;
		      reporthdr->report.packetTime = nullpacket.packetTime;
		      advance_jobq = (*reporthdr->output_interval_report_handler)(reporthdr, &nullpacket);
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

static inline void reporter_handle_packet_pps(struct ReporterData *data, struct TransferInfo *stats) {
    data->cntDatagrams++;
    stats->IPGsum += TimeDifference(data->packetTime, data->IPGstart);
    stats->IPGcnt++;
    stats->IPGcnttot++;
    data->IPGstart = data->packetTime;
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
	// mean min max tests
	stats->sock_callstats.read.cntRead++;
	stats->sock_callstats.read.totcntRead++;
	bin = (int)floor((packet->packetLen -1)/stats->sock_callstats.read.binsize);
	if (bin < TCPREADBINCOUNT) {
	    stats->sock_callstats.read.bins[bin]++;
	    stats->sock_callstats.read.totbins[bin]++;
	}
    }
    reporter_handle_burst_tcp_transit(&reporthdr->report, stats, packet);
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
    } else if (!packet->emptyreport) {
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
      reporter_handle_packet_pps(data, stats);
      reporter_handle_packet_oneway_transit(data, stats, packet);
      reporter_handle_packet_isochronous(data, stats, packet);
    }
}

void reporter_handle_packet_client(struct ReportHeader *reporthdr, struct ReportStruct *packet) {
    struct ReporterData *data = &reporthdr->report;
    struct TransferInfo *stats = &reporthdr->report.info;

    data->packetTime = packet->packetTime;
    stats->socket = packet->socket;
    if (packet->errwrite && (packet->errwrite != WriteErrNoAccount)) {
	stats->sock_callstats.write.WriteErr++;
	stats->sock_callstats.write.totWriteErr++;
    }
    if (!packet->emptyreport) {
	// These are valid packets that need standard iperf accounting
	stats->sock_callstats.write.WriteCnt++;
	stats->sock_callstats.write.totWriteCnt++;
	if (isUDP(data)) {
	  reporter_handle_packet_pps(data, stats);
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
static inline void output_missed_reports(struct ReporterData *stats, struct ReportStruct *packet) {
    while ((stats->intervalTime.tv_sec != 0 || \
	    stats->intervalTime.tv_usec != 0) && \
	   TimeDifference(stats->nextTime, stats->packetTime ) < 0 ) {
	stats->info.startTime = stats->info.endTime;
	stats->info.endTime = TimeDifference( stats->nextTime, stats->startTime );
	TimeAdd(stats->nextTime, stats->intervalTime);
	if (TimeDifference(stats->nextTime, stats->packetTime) < 0) {
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
}
// If reports were missed, catch up now
static inline void output_missed_multireports(struct ReporterData *stats, struct ReportStruct *packet) {
    output_missed_reports(stats, packet);
}

static inline void set_endtime(struct ReporterData *stats, int final) {
  // There is a corner case when the first packet is also the last where the start time (which comes
  // from app level syscall) is greater than the packetTime (which come for kernel level SO_TIMESTAMP)
  // For this case set the start and end time to both zero.
  if (TimeDifference(stats->packetTime, stats->startTime) < 0) {
    stats->info.endTime = 0;
  } else if (!final) {
    stats->info.endTime = TimeDifference(stats->nextTime, stats->startTime);
  } else {
    stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
  }
}

// Actions required after an interval report has been outputted
static inline void reset_transfer_stats(struct ReporterData *stats) {
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

static inline void reset_transfer_stats_bidir(struct ReporterData *stats) {
    stats->info.startTime = stats->info.endTime;
    stats->lastTotal = stats->TotalLen;
}
static inline void reset_transfer_stats_client_tcp(struct ReporterData *stats) {
    stats->info.startTime = stats->info.endTime;
    stats->lastTotal = stats->TotalLen;
    stats->info.sock_callstats.write.WriteCnt = 0;
    stats->info.sock_callstats.write.WriteErr = 0;
#ifdef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    stats->info.sock_callstats.write.TCPretry = 0;
    stats->info.sock_callstats.write.up_to_date = 0;
#endif
}
static inline void reset_transfer_stats_client_udp(struct ReporterData *stats) {
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
    stats->info.IPGcnt = 0;
    stats->info.IPGsum = 0;
    stats->info.startTime = stats->info.endTime;
}
static inline void reset_transfer_stats_server_tcp(struct ReporterData *stats) {
    int ix;
    stats->info.startTime = stats->info.endTime;
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
}
static inline void reset_transfer_stats_server_udp(struct ReporterData *stats) {
    // Reset the enhanced stats for the next report interval
    stats->info.startTime = stats->info.endTime;
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
    stats->info.IPGcnt = 0;
    stats->info.IPGsum = 0;
    stats->info.l2counts.cnt = 0;
    stats->info.l2counts.unknown = 0;
    stats->info.l2counts.udpcsumerr = 0;
    stats->info.l2counts.lengtherr = 0;
}

// These are the output handlers that get the reports ready and then prints them
static void output_transfer_report_server_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
    set_endtime(stats, final);
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
	reporter_print(stats, TRANSFER_REPORT, 0);
	reset_transfer_stats_server_udp(stats);
    }
    if (final) {
	stats->info.cntOutofOrder = stats->cntOutofOrder;
	// assume most of the  time out-of-order packets are not
	// duplicate packets, so conditionally subtract them from the lost packets.
	stats->info.cntError = stats->cntError;
	stats->info.cntError -= stats->info.cntOutofOrder;
	stats->info.cntDatagrams = stats->PacketID;
	stats->info.IPGcnt = stats->info.IPGcnttot;
	stats->info.IPGsum = TimeDifference(stats->packetTime, stats->startTime);
	stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
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
static void output_transfer_sum_report_server_udp(struct ReporterData *stats, int final) {
    set_endtime(stats,final);
    if (final) {
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
	reset_transfer_stats_server_udp(stats);
    }
}
static void output_connect_final_report_tcp (struct MultiHeader *multihdr) {
    if (multihdr->connect_times.cnt > 1) {
        double variance = (multihdr->connect_times.cnt < 2) ? 0 : sqrt(multihdr->connect_times.m2 / (multihdr->connect_times.cnt - 1));
        fprintf(stdout, "[ CT] final connect times (min/avg/max/stdev) = %0.3f/%0.3f/%0.3f/%0.3f ms (tot/err) = %d/%d\n", \
		multihdr->connect_times.min,  \
	        (multihdr->connect_times.sum / multihdr->connect_times.cnt), \
		multihdr->connect_times.max, variance,  \
		(multihdr->connect_times.cnt + multihdr->connect_times.err), \
		multihdr->connect_times.err);
    }
}

static void output_transfer_sum_report_client_udp(struct ReporterData *stats, int final) {
    set_endtime(stats,final);
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
	reset_transfer_stats_client_udp(stats);
    }
}

static void output_transfer_report_client_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
    set_endtime(stats,final);
    if (sumstats) {
	sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
	sumstats->info.sock_callstats.write.WriteErr += stats->info.sock_callstats.write.WriteErr;
	sumstats->info.sock_callstats.write.WriteCnt += stats->info.sock_callstats.write.WriteCnt;
	sumstats->info.sock_callstats.write.totWriteErr += stats->info.sock_callstats.write.WriteErr;
	sumstats->info.sock_callstats.write.totWriteCnt += stats->info.sock_callstats.write.WriteCnt;
	sumstats->cntDatagrams += stats->cntDatagrams;
	sumstats->info.IPGsum += stats->info.IPGsum;
	sumstats->info.IPGcnt += stats->info.IPGcnt;
    }
    if (bidirstats) {
	bidirstats->TotalLen += stats->TotalLen - stats->lastTotal;
    }
    if (final) {
	stats->info.TotalLen = stats->TotalLen;
	stats->info.sock_callstats.write.WriteErr = stats->info.sock_callstats.write.totWriteErr;
	stats->info.sock_callstats.write.WriteCnt = stats->info.sock_callstats.write.totWriteCnt;
	stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
	stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
	reporter_print(stats, TRANSFER_REPORT, 1);
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print(stats, TRANSFER_REPORT, 0);
	reset_transfer_stats_client_udp(stats);
    }
}

static void output_transfer_report_server_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
    set_endtime(stats,final);
    int ix;
    if (sumstats) {
        sumstats->TotalLen += stats->TotalLen - stats->lastTotal;
        sumstats->info.sock_callstats.read.cntRead += stats->info.sock_callstats.read.cntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    sumstats->info.sock_callstats.read.bins[ix] += stats->info.sock_callstats.read.bins[ix];
        }
    }
    if (bidirstats) {
	bidirstats->TotalLen += stats->TotalLen - stats->lastTotal;
    }
    // print a interval report and possibly a partial interval report if this a final
    stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
    if (!final || (final && (stats->info.TotalLen > 0) && !TimeZero(stats->intervalTime))) {
	if (!bidirstats)
	  reporter_print(stats, TRANSFER_REPORT, 0);
	else if (stats->info.mEnhanced)
	  reporter_print(stats, TRANSFER_REPORT, 0);
	reset_transfer_stats_server_tcp(stats);
    }
    if (final) {
        stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
        stats->info.sock_callstats.read.cntRead = stats->info.sock_callstats.read.totcntRead;
        for (ix = 0; ix < TCPREADBINCOUNT; ix++) {
	    stats->info.sock_callstats.read.bins[ix] = stats->info.sock_callstats.read.totbins[ix];
        }
	stats->info.transit.sumTransit = stats->info.transit.totsumTransit;
	stats->info.transit.cntTransit = stats->info.transit.totcntTransit;
	if (!bidirstats) {
	  reporter_print(stats, TRANSFER_REPORT, 1);
	} else if (stats->info.mEnhanced)
	  reporter_print(stats, TRANSFER_REPORT, 1);
    }
}

static void output_transfer_report_client_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final) {
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
	stats->info.startTime = 0.0;
	stats->info.endTime = TimeDifference(stats->packetTime, stats->startTime);
	if (!bidirstats)
	    reporter_print(stats, TRANSFER_REPORT, 1);
	else if (stats->info.mEnhanced)
	  reporter_print(stats, TRANSFER_REPORT, 1);
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	stats->info.endTime = TimeDifference(stats->nextTime, stats->startTime);
	if (!bidirstats)
	    reporter_print(stats, TRANSFER_REPORT, 0);
	else if (stats->info.mEnhanced)
	  reporter_print(stats, TRANSFER_REPORT, 0);
	reset_transfer_stats_client_tcp(stats);
    }
}

static void output_transfer_final_report_client_tcp(struct ReporterData *stats) {
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
static void output_transfer_sum_report_client_tcp(struct ReporterData *stats, int final) {
    set_endtime(stats,final);
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
	reset_transfer_stats_client_tcp(stats);
    }
}

static void output_transfer_sum_report_server_tcp(struct ReporterData *stats, int final) {
    set_endtime(stats,final);
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

static void output_transfer_bidir_report_tcp(struct ReporterData *stats, int final) {
    set_endtime(stats,final);
    if (final) {
	stats->info.TotalLen = stats->TotalLen;
	stats->info.startTime = 0.0;
	reporter_print( stats, BIDIR_REPORT, 1 );
    } else {
	stats->info.TotalLen = stats->TotalLen - stats->lastTotal;
	reporter_print( stats, BIDIR_REPORT, 0 );
	reset_transfer_stats_bidir(stats);
    }
}

static void output_transfer_bidir_report_udp(struct ReporterData *stats, int final) {
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
