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
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
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
 * rewritten by Robert McMahon
 * -------------------------------------------------------------------
 * Handle instantiation and deletion of reports, including sum reports,
 * in a thread safe way
 * ------------------------------------------------------------------- */

#include "headers.h"
#include <math.h>
#include "Settings.hpp"
#include "Reporter.h"
#include "Locale.h"
#include "active_hosts.h"
#include "payloads.h"

static inline void my_str_copy(char **dst, char *src) {
    if (src) {
	*dst = (char *) calloc(1, (strlen(src) + 1));
	if (*dst == NULL) {
	    fprintf(stderr, "Out of Memory!!\n");
	    exit(1);
	}
        strcpy((*dst), src);
    } else {
	*dst = NULL;
    }
}

// These are the thread settings that are shared among report types
// Make a copy vs referencing the thread setting object. This will
// better encpasulate report handling.
static void common_copy (struct ReportCommon **common, struct thread_Settings *inSettings) {
    // Do deep copies from settings
    *common = (struct ReportCommon *) calloc(1,sizeof(struct ReportCommon));
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Alloc common=%p", (void *)(*common));
#endif
    my_str_copy(&(*common)->Host, inSettings->mHost);
    my_str_copy(&(*common)->Localhost, inSettings->mLocalhost);
    my_str_copy(&(*common)->Ifrname, inSettings->mIfrname);
    my_str_copy(&(*common)->Ifrnametx, inSettings->mIfrnametx);
    my_str_copy(&(*common)->SSMMulticastStr, inSettings->mSSMMulticastStr);
    my_str_copy(&(*common)->Congestion, inSettings->mCongestion);
    // copy some relevant settings
    (*common)->flags = inSettings->flags;
    (*common)->flags_extend = inSettings->flags_extend;
    (*common)->ThreadMode = inSettings->mThreadMode;
    (*common)->Format = inSettings->mFormat;
    (*common)->TTL = inSettings->mTTL;
    // copy some traffic related settings
    (*common)->BufLen = inSettings->mBufLen;
    (*common)->MSS = inSettings->mMSS;
    (*common)->TCPWin = inSettings->mTCPWin;
    (*common)->FQPacingRate = inSettings->mFQPacingRate;
    (*common)->Port = inSettings->mPort;
    (*common)->BindPort = inSettings->mBindPort;
    (*common)->ListenPort = inSettings->mListenPort;
    (*common)->UDPRate = inSettings->mUDPRate;
    (*common)->UDPRateUnits = inSettings->mUDPRateUnits;
    (*common)->socket = inSettings->mSock;
    (*common)->threads = inSettings->mThreads;
    (*common)->winsize_requested = inSettings->mTCPWin;
#if defined(HAVE_LINUX_FILTER_H) && defined(HAVE_AF_PACKET)
    (*common)->socketdrop = inSettings->mSockDrop;
#endif
    (*common)->peer = inSettings->peer;
    (*common)->size_peer = inSettings->size_peer;
    (*common)->local = inSettings->local;
    (*common)->size_local = inSettings->size_local;
    (*common)->RXbins =inSettings->mRXbins;
    (*common)->RXbinsize =inSettings->mRXbinsize;
    (*common)->RXunits =inSettings->mRXunits;
    (*common)->pktIPG =inSettings->mBurstIPG;
}

static void free_common_copy (struct ReportCommon *common) {
    assert(common != NULL);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Free common=%p", (void *)common);
#endif
    // Free deep copies
    if (common->Host)
	free(common->Host);
    if (common->Localhost)
	free(common->Localhost);
    if (common->Ifrname)
	free(common->Ifrname);
    if (common->Ifrnametx)
	free(common->Ifrnametx);
    if (common->SSMMulticastStr)
	free(common->SSMMulticastStr);
    if (common->Congestion)
	free(common->Congestion);
    free(common);
}

static void SetSumHandlers (struct thread_Settings *inSettings, struct SumReport* sumreport, int fullduplex) {
    if (fullduplex) {
	if (isUDP(inSettings)) {
	    sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_fullduplex_udp;
	    sumreport->info.output_handler = ((isSumOnly(inSettings) || (inSettings->mReportMode == kReport_CSV)) ? NULL : \
					      (isEnhanced(inSettings) ? udp_output_fullduplex_sum : udp_output_fullduplex_sum));
	} else {
	    sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_fullduplex_tcp;
	    sumreport->info.output_handler = ((isSumOnly(inSettings) || (inSettings->mReportMode == kReport_CSV)) ? NULL : \
					      (isEnhanced(inSettings) ? tcp_output_fullduplex_sum_enhanced : tcp_output_fullduplex_sum));
	}
    } else {
	switch (inSettings->mThreadMode) {
	case kMode_Server :
	    if (isUDP(inSettings)) {
		sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_udp;
		sumreport->info.output_handler =  (isSumOnly(inSettings) ? udp_output_sumcnt_read : udp_output_sum_read);
	    } else {
		if (isEnhanced(inSettings)) {
		    sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_tcp;
		    sumreport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_read_enhanced : tcp_output_sum_read_enhanced);
		} else {
		    sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_tcp;
		    sumreport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_read : tcp_output_sum_read);
		}
	    }
	    break;
	case kMode_Client :
	    if (isUDP(inSettings)) {
		sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_udp;
		sumreport->info.output_handler =  (isSumOnly(inSettings) ? udp_output_sumcnt_write : \
						   (isEnhanced(inSettings) ? udp_output_sum_write_enhanced : udp_output_sum_write));
	    } else {
		sumreport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_tcp;
		sumreport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_write : tcp_output_sum_write);
	    }
	    break;
	default:
	    FAIL(1, "SetSumReport", inSettings);
	}
    }
    // overide output handlers when csv reporting set
    if (inSettings->mReportMode == kReport_CSV)
	sumreport->info.output_handler = NULL;

}

struct SumReport* InitSumReport(struct thread_Settings *inSettings, int inID, int fullduplex) {
    struct SumReport *sumreport = (struct SumReport *) calloc(1, sizeof(struct SumReport));
    if (sumreport == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    sumreport->reference.count = 0;
    sumreport->reference.maxcount = 0;
    Mutex_Initialize(&sumreport->reference.lock);
    sumreport->threads = 0;
    common_copy(&sumreport->info.common, inSettings);
    sumreport->info.groupID = inID;
    sumreport->info.transferID = inSettings->mSock;
    sumreport->info.threadcnt = 0;
    // Only initialize the interval time here
    // The startTime and nextTime for summing reports will be set by
    // the reporter thread in realtime
    if ((inSettings->mInterval) && (inSettings->mIntervalMode == kInterval_Time)) {
	sumreport->info.ts.intervalTime.tv_sec = (long) (inSettings->mInterval / rMillion);
	sumreport->info.ts.intervalTime.tv_usec = (long) (inSettings->mInterval % rMillion);
    }
    SetSumHandlers(inSettings, sumreport, fullduplex);
    if (fullduplex) {
	if (!isServerReverse(inSettings)) {
	    sumreport->fullduplex_barrier.count = 0;
	    Condition_Initialize(&sumreport->fullduplex_barrier.await);
	    sumreport->fullduplex_barrier.timeout = (isModeTime(inSettings) ? ((int)(inSettings->mAmount / 100) + 1) : 2);
	    if (sumreport->fullduplex_barrier.timeout < MINBARRIERTIMEOUT)
		sumreport->fullduplex_barrier.timeout = MINBARRIERTIMEOUT;
	} else {
	    if (isTripTime(inSettings)) {
		sumreport->info.ts.startTime = inSettings->triptime_start;
	    } else {
		sumreport->info.ts.startTime = inSettings->accept_time;
	    }
	    sumreport->info.ts.nextTime = sumreport->info.ts.startTime;
	    TimeAdd(sumreport->info.ts.nextTime, sumreport->info.ts.intervalTime);
	}
    }
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init sum report %p id=%d", (void *)sumreport, inID);
#endif
    return sumreport;
}

struct ConnectionInfo * InitConnectOnlyReport (struct thread_Settings *thread) {
    assert(thread != NULL);
    // this connection report used only by report for accumulate stats
    struct ConnectionInfo *creport = (struct ConnectionInfo *) calloc(1, sizeof(struct ConnectionInfo));
    if (!creport) {
	FAIL(1, "Out of Memory!!\n", thread);
    }
    common_copy(&creport->common, thread);
    creport->connect_times.min = FLT_MAX;
    creport->connect_times.max = FLT_MIN;
    creport->connect_times.vd = 0;
    creport->connect_times.m2 = 0;
    creport->connect_times.mean = 0;
    return creport;
}

void FreeSumReport (struct SumReport *sumreport) {
    assert(sumreport);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Free sum report hdr=%p", (void *)sumreport);
#endif
    Condition_Destroy_Reference(&sumreport->reference);
    free_common_copy(sumreport->info.common);
    free(sumreport);
}


static void Free_iReport (struct ReporterData *ireport) {
    assert(ireport != NULL);

#ifdef HAVE_THREAD_DEBUG
    thread_debug("Free report hdr=%p reporter thread suspend count=%d packetring=%p histo=%p frame histo=%p", \
		 (void *)ireport, ireport->reporter_thread_suspends, (void *) ireport->packetring, \
		 (void *)ireport->info.latency_histogram, (void *) ireport->info.framelatency_histogram);
#endif
    if (ireport->packetring && ireport->info.total.Bytes.current && !(isSingleUDP(ireport->info.common)) && \
	!TimeZero(ireport->info.ts.intervalTime) && (ireport->reporter_thread_suspends < 3)) {
	fprintf(stdout, "WARN: this test may have been CPU bound (%d) (or may not be detecting the underlying network devices)\n", \
		ireport->reporter_thread_suspends);
    }
    if (ireport->packetring) {
	packetring_free(ireport->packetring);
    }
    if (ireport->info.latency_histogram) {
	histogram_delete(ireport->info.latency_histogram);
    }
    if (ireport->info.framelatency_histogram) {
	histogram_delete(ireport->info.framelatency_histogram);
    }
    free_common_copy(ireport->info.common);
    free(ireport);
}

void FreeConnectionReport (struct ConnectionInfo *report) {
    free_common_copy(report->common);
    free(report);
}

static void Free_sReport (struct ReportSettings *report) {
    free_common_copy(report->common);
    free(report);
}

static void Free_srReport (struct TransferInfo *report) {
    free_common_copy(report->common);
    free(report);
}

void FreeReport (struct ReportHeader *reporthdr) {
    assert(reporthdr != NULL);
#ifdef HAVE_THREAD_DEBUG
    char rs[REPORTTXTMAX];
    reporttype_text(reporthdr, &rs[0]);
    thread_debug("Jobq *FREE* report %p (%s)", reporthdr, &rs[0]);
#endif
    switch (reporthdr->type) {
    case DATA_REPORT:
	Free_iReport((struct ReporterData *)reporthdr->this_report);
	break;
    case CONNECTION_REPORT:
	FreeConnectionReport((struct ConnectionInfo *)reporthdr->this_report);
	break;
    case SETTINGS_REPORT:
	Free_sReport((struct ReportSettings *)reporthdr->this_report);
	break;
    case SERVER_RELAY_REPORT:
	Free_srReport((struct TransferInfo *)reporthdr->this_report);
	break;
    default:
	fprintf(stderr, "Invalid report type in free\n");
	assert(0);
	break;
    }
    free(reporthdr);
}

/*
 * InitReport is called by a transfer agent (client or
 * server) to setup the needed structures to communicate
 * traffic and connection information.  Also initialize
 * the report start time and next interval report time
 * Finally, in the case of parallel clients, have them all
 * synchronize on compeleting their connect()
 */

void IncrSumReportRefCounter (struct SumReport *sumreport) {
    assert(sumreport);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Sum multiheader %p ref=%d->%d", (void *)sumreport, sumreport->reference.count, (sumreport->reference.count + 1));
#endif
    Mutex_Lock(&sumreport->reference.lock);
    sumreport->reference.count++;
    if (sumreport->reference.count > sumreport->reference.maxcount)
	sumreport->reference.maxcount = sumreport->reference.count;
    Mutex_Unlock(&sumreport->reference.lock);
}

int DecrSumReportRefCounter (struct SumReport *sumreport) {
    assert(sumreport);
//    thread_debug("before lock hdr=%p", (void *)sumreport);
    Mutex_Lock(&sumreport->reference.lock);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Sum multiheader %p ref=%d->%d", (void *)sumreport, sumreport->reference.count, (sumreport->reference.count - 1));
#endif
//    thread_debug("in lock hdr=%p", (void *)sumreport);
    int refcnt = --sumreport->reference.count;
    Mutex_Unlock(&sumreport->reference.lock);
//    thread_debug("unlock hdr=%p", (void *)sumreport);
    return refcnt;
}


// Note, this report structure needs to remain self contained and not coupled
// to any settings structure pointers. This allows the thread settings to
// be freed without impacting the reporter.  It's not recommended that
// this be done, i.e. free the settings before the report, but be defensive
// here to allow it
struct ReportHeader* InitIndividualReport (struct thread_Settings *inSettings) {
    /*
     * Create the report header and an ireport (if needed)
     */
    assert(inSettings!=NULL);
    struct ReportHeader *reporthdr = (struct ReportHeader *) calloc(1, sizeof(struct ReportHeader));
    if (reporthdr == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->this_report = (void *) calloc(1, sizeof(struct ReporterData));
    if (reporthdr->this_report == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->type = DATA_REPORT;
    reporthdr->ReportMode = inSettings->mReportMode;

    struct ReporterData *ireport = (struct ReporterData *)(reporthdr->this_report);
    if (inSettings->mSumReport) {
	IncrSumReportRefCounter(inSettings->mSumReport);
	ireport->GroupSumReport = inSettings->mSumReport;
    }
    if (isFullDuplex(inSettings)) {
	assert(inSettings->mFullDuplexReport != NULL);
	IncrSumReportRefCounter(inSettings->mFullDuplexReport);
	ireport->FullDuplexReport = inSettings->mFullDuplexReport;
    }
    // Copy common settings into the transfer report section
    common_copy(&ireport->info.common, inSettings);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Job report %p uses multireport %p and fullduplex report is %p (socket=%d)", (void *)reporthdr->this_report, (void *)inSettings->mSumReport, (void *)inSettings->mFullDuplexReport, inSettings->mSock);
#endif
    // Create a new packet ring which is used to communicate
    // packet stats from the traffic thread to the reporter
    // thread.  The reporter thread does all packet accounting
    ireport->packetring = packetring_init((inSettings->numreportstructs ? inSettings->numreportstructs : (isSingleUDP(inSettings) ? 40 : NUM_REPORT_STRUCTS)), \
					  &ReportCond, (isSingleUDP(inSettings) ? NULL : &inSettings->awake_me));
    if (inSettings->numreportstructs)
	fprintf (stdout, "[%3d] NUM_REPORT_STRUCTS override from %d to %d\n", inSettings->mSock, NUM_REPORT_STRUCTS, inSettings->numreportstructs);
    ireport->info.csv_peer[0] = '\0';

    // Set up the function vectors, there are three
    // 1) packet_handler: does packet accounting per the test and protocol
    // 2) transfer_protocol_handler: performs output, e.g. interval reports, per the test and protocol

    switch (inSettings->mThreadMode) {
    case kMode_Server :
	if (isUDP(inSettings)) {
	    ireport->packet_handler = reporter_handle_packet_server_udp;
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_server_udp;
            if (inSettings->mReportMode == kReport_CSV) {
		ireport->info.output_handler = udp_output_basic_csv;
	    } else if (isSumOnly(inSettings)) {
		ireport->info.output_handler = NULL;
	    } else if (isTripTime(inSettings)) {
		if (isIsochronous(inSettings))
		    ireport->info.output_handler = udp_output_read_enhanced_triptime_isoch;
		else
		    ireport->info.output_handler = udp_output_read_enhanced_triptime;
	    } else if (isEnhanced(inSettings)) {
		ireport->info.output_handler = udp_output_read_enhanced_triptime;
	    } else if (!isFullDuplex(inSettings)) {
		ireport->info.output_handler = udp_output_read;
	    } else {
		ireport->info.output_handler =  NULL;
	    }
	} else {
	    ireport->packet_handler = reporter_handle_packet_server_tcp;
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_server_tcp;
            if (inSettings->mReportMode == kReport_CSV) {
		ireport->info.output_handler = tcp_output_basic_csv;
	    } else if (isSumOnly(inSettings)) {
		ireport->info.output_handler = NULL;
	    } else if (isTripTime(inSettings)) {
		ireport->info.output_handler = tcp_output_read_enhanced_triptime;
	    } else if (isEnhanced(inSettings)) {
		ireport->info.output_handler = tcp_output_read_enhanced;
	    } else if (!isFullDuplex(inSettings)) {
		ireport->info.output_handler = tcp_output_read;
	    } else {
		ireport->info.output_handler = NULL ;
	    }
	}
	break;
    case kMode_Client :
	ireport->packet_handler = reporter_handle_packet_client;
	if (isUDP(inSettings)) {
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_client_udp;
            if (inSettings->mReportMode == kReport_CSV) {
		ireport->info.output_handler = udp_output_basic_csv;
	    } else if (isSumOnly(inSettings)) {
		ireport->info.output_handler = NULL;
	    } else if (isIsochronous(inSettings)) {
		ireport->info.output_handler = udp_output_write_enhanced_isoch;
	    } else if (isEnhanced(inSettings)) {
		ireport->info.output_handler = udp_output_write_enhanced;
	    } else if (isFullDuplex(inSettings)) {
		ireport->info.output_handler = NULL;
	    } else {
		ireport->info.output_handler = udp_output_write;
	    }
	} else {
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_client_tcp;
	    if (inSettings->mReportMode == kReport_CSV) {
		ireport->info.output_handler = tcp_output_basic_csv;
	    } else if (isSumOnly(inSettings)) {
		ireport->info.output_handler = NULL;
	    } else if (isEnhanced(inSettings) || isIsochronous(inSettings)) {
		ireport->info.output_handler = tcp_output_write_enhanced;
	    } else if (isFullDuplex(inSettings)) {
		ireport->info.output_handler = NULL;
	    } else {
		ireport->info.output_handler = tcp_output_write;
	    }
	}
	break;
    case kMode_WriteAckClient :
	ireport->packet_handler = reporter_handle_packet_null;
	ireport->transfer_protocol_handler = reporter_transfer_protocol_null;
	break;
    case kMode_Unknown :
    case kMode_Reporter :
    case kMode_ReporterClient :
    case kMode_Listener:
    default:
	FAIL(1, "InitIndividualReport\n", inSettings);
    }

#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init data report %p using packetring=%p cond=%p", \
		 (void *)ireport, (void *)(ireport->packetring), (void *)(ireport->packetring->awake_producer));
#endif
    ireport->info.transferID = inSettings->mSock;

    switch (inSettings->mIntervalMode) {
    case kInterval_Time :
	ireport->info.ts.intervalTime.tv_sec = (long) (inSettings->mInterval / rMillion);
	ireport->info.ts.intervalTime.tv_usec = (long) (inSettings->mInterval % rMillion);
	ireport->transfer_interval_handler = reporter_condprint_time_interval_report;
	break;
    case kInterval_Frames :
	if (isUDP(inSettings)) {
	    ireport->transfer_interval_handler = reporter_condprint_frame_interval_report_udp;
	} else {
	    if (isTripTime(inSettings) || isIsochronous(inSettings))
		ireport->transfer_interval_handler = reporter_condprint_frame_interval_report_tcp;
	    else
		ireport->transfer_interval_handler = NULL;
	}
	break;
    default :
	ireport->transfer_interval_handler = NULL;
	break;
    }
    if (inSettings->mThreadMode == kMode_Server) {
	ireport->info.sock_callstats.read.binsize = inSettings->mBufLen / 8;
	if (isRxHistogram(inSettings) && isUDP(inSettings) && isTripTime(inSettings)) {
	    char name[] = "T8";
	    ireport->info.latency_histogram =  histogram_init(inSettings->mRXbins,inSettings->mRXbinsize,0,\
							      pow(10,inSettings->mRXunits), \
							      inSettings->mRXci_lower, inSettings->mRXci_upper, ireport->info.transferID, name);
	}
	if (isRxHistogram(inSettings) && isIsochronous(inSettings) && isTripTime(inSettings)) {
	    char name[] = "F8";
	    // make sure frame bin size min is 100 microsecond
	    ireport->info.framelatency_histogram =  histogram_init(inSettings->mRXbins,inSettings->mRXbinsize,0, \
								   pow(10,inSettings->mRXunits), inSettings->mRXci_lower, \
								   inSettings->mRXci_upper, ireport->info.transferID, name);
	}
    }
    return reporthdr;
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
struct ReportHeader* InitConnectionReport (struct thread_Settings *inSettings, double ct) {
    assert(inSettings != NULL);
    struct ReportHeader *reporthdr = calloc(sizeof(struct ReportHeader), sizeof(char*));
    if (reporthdr == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->this_report = (void *) calloc(1, sizeof(struct ConnectionInfo));
    if (reporthdr->this_report == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->type = CONNECTION_REPORT;
    reporthdr->ReportMode = inSettings->mReportMode;

    struct ConnectionInfo * creport = (struct ConnectionInfo *)(reporthdr->this_report);
    common_copy(&creport->common, inSettings);
    if (!isUDP(inSettings) && (inSettings->mSock > 0))
	creport->MSS = getsock_tcp_mss(inSettings->mSock);
    else
	creport->MSS = -1;
    // Fill out known fields for the connection report
    reporter_peerversion(creport, inSettings->peer_version_u, inSettings->peer_version_l);
    creport->connecttime = ct;
    if (isEnhanced(inSettings) && isTxStartTime(inSettings)) {
	creport->epochStartTime.tv_sec = inSettings->txstart_epoch.tv_sec;
	creport->epochStartTime.tv_usec = inSettings->txstart_epoch.tv_usec;
    } else if (isTripTime(inSettings)) {
	creport->epochStartTime.tv_sec = inSettings->accept_time.tv_sec;
	creport->epochStartTime.tv_usec = inSettings->accept_time.tv_usec;
    }
    //  Copy state from the settings object into the connection report
    creport->connect_times.min = FLT_MAX;
    creport->connect_times.max = FLT_MIN;
    creport->connect_times.vd = 0;
    creport->connect_times.m2 = 0;
    creport->connect_times.mean = 0;
    if (inSettings->mSock > 0) {
	creport->winsize = getsock_tcp_windowsize(inSettings->mSock,	\
                  (inSettings->mThreadMode != kMode_Client ? 0 : 1) );
    }
    creport->common->winsize_requested = inSettings->mTCPWin;
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init connection report %p", reporthdr);
#endif
    return reporthdr;
}

/*
 * ReportSettings will generate a summary report for
 * settings being used with Listeners or Clients
 */
struct ReportHeader *InitSettingsReport (struct thread_Settings *inSettings) {
    assert(inSettings != NULL);
    struct ReportHeader *reporthdr = calloc(sizeof(struct ReportHeader), sizeof(char*));
    if (reporthdr == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->this_report = (void *) calloc(1, sizeof(struct ReportSettings));
    if (reporthdr->this_report == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->type = SETTINGS_REPORT;
    reporthdr->ReportMode = inSettings->mReportMode;

    struct ReportSettings *sreport = (struct ReportSettings *)reporthdr->this_report;
    common_copy(&sreport->common, inSettings);
    sreport->peer = inSettings->peer;
    sreport->size_peer = inSettings->size_peer;
    sreport->local = inSettings->local;
    sreport->size_local = inSettings->size_local;
    sreport->isochstats.mFPS = inSettings->mFPS;
    sreport->isochstats.mMean = inSettings->mMean/8;
    sreport->isochstats.mVariance = inSettings->mVariance/8;
    sreport->isochstats.mBurstIPG = (unsigned int) (inSettings->mBurstIPG*1000.0);
    sreport->isochstats.mBurstInterval = (unsigned int) (1 / inSettings->mFPS * 1000000);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init settings report %p", reporthdr);
#endif
    return reporthdr;
}

/*
 * This will generate a report of the UDP
 * statistics as reported by the server on the client
 * side.
 */
struct ReportHeader* InitServerRelayUDPReport(struct thread_Settings *inSettings, struct server_hdr *server) {
    /*
     * Create the report header and an ireport (if needed)
     */
    struct ReportHeader *reporthdr = (struct ReportHeader *) calloc(1, sizeof(struct ReportHeader));
    if (reporthdr == NULL) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
    reporthdr->this_report = (void *) calloc(1, sizeof(struct ServerRelay));
    if (!reporthdr->this_report) {
	FAIL(1, "Out of Memory!!\n", inSettings);
    }
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init server relay report %p size %ld", (void *)reporthdr, sizeof(struct ReportHeader) + sizeof(struct ServerRelay));
#endif
    reporthdr->type = SERVER_RELAY_REPORT;
    reporthdr->ReportMode = inSettings->mReportMode;
    struct ServerRelay *sr_report = (struct ServerRelay *)reporthdr->this_report;
    common_copy(&sr_report->info.common, inSettings);
    struct TransferInfo *stats = (struct TransferInfo *)&sr_report->info;
    stats->transferID = inSettings->mSock;

    stats->jitter = ntohl(server->base.jitter1);
    stats->jitter += ntohl(server->base.jitter2) / (double)rMillion;
#ifdef HAVE_INT64_T
    stats->cntBytes = (((intmax_t) ntohl(server->base.total_len1)) << 32) + \
	ntohl(server->base.total_len2);
#else
    stats->cntBytes = (intmax_t) ntohl(server->base.total_len2);
#endif
    stats->ts.iStart = 0;
    stats->ts.iEnd = ntohl(server->base.stop_sec);
    stats->ts.iEnd += ntohl(server->base.stop_usec) / (double)rMillion;
    uint32_t flags = ntohl(server->base.flags);
    if ((flags & HEADER_SEQNO64B)) {
	stats->cntError = (((intmax_t) ntohl(server->extend2.error_cnt2)) << 32) + \
	    ntohl(server->base.error_cnt);
	stats->cntOutofOrder = (((intmax_t) ntohl(server->extend2.outorder_cnt2)) << 32) + \
	    ntohl(server->base.outorder_cnt);
	stats->cntDatagrams = (((intmax_t) ntohl(server->extend2.datagrams2)) << 32) + \
	    ntohl(server->base.datagrams);
    } else {
	stats->cntError  = ntohl(server->base.error_cnt);
	stats->cntOutofOrder = ntohl(server->base.outorder_cnt);
	stats->cntDatagrams = ntohl(server->base.datagrams);
    }
    if (isEnhanced(stats->common)) {
	if ((flags & SERVER_HEADER_EXTEND) != 0) {
	    stats->transit.minTransit = ntohl(server->extend.minTransit1);
	    stats->transit.minTransit += ntohl(server->extend.minTransit2) / (double)rMillion;
	    stats->transit.maxTransit = ntohl(server->extend.maxTransit1);
	    stats->transit.maxTransit += ntohl(server->extend.maxTransit2) / (double)rMillion;
	    stats->transit.sumTransit = ntohl(server->extend.sumTransit1);
	    stats->transit.sumTransit += ntohl(server->extend.sumTransit2) / (double)rMillion;
	    stats->transit.meanTransit = ntohl(server->extend.meanTransit1);
	    stats->transit.meanTransit += ntohl(server->extend.meanTransit2) / (double)rMillion;
	    stats->transit.m2Transit = ntohl(server->extend.m2Transit1);
	    stats->transit.m2Transit += ntohl(server->extend.m2Transit2) / (double)rMillion;
	    stats->transit.vdTransit = ntohl(server->extend.vdTransit1);
	    stats->transit.vdTransit += ntohl(server->extend.vdTransit2) / (double)rMillion;
	    stats->transit.cntTransit = ntohl(server->extend.cntTransit);
	    stats->cntIPG = ntohl(server->extend.cntIPG);
	    stats->IPGsum = ntohl(server->extend.IPGsum);
	} else {
	    unsetEnhanced(stats->common);
	}
    }
    sr_report->peer = inSettings->local;
    sr_report->size_peer = inSettings->size_local;
    sr_report->local = inSettings->peer;
    sr_report->size_local = inSettings->size_peer;
    return reporthdr;
}

/* -------------------------------------------------------------------
 * Send an AckFIN (a datagram acknowledging a FIN) on the socket,
 * then select on the socket for some time to check for silence.
 * If additional datagrams come in (not silent), probably our AckFIN
 * was lost so the client has re-transmitted
 * termination datagrams, so re-transmit our AckFIN.
 * Sent by server to client
 * ------------------------------------------------------------------- */
void write_UDP_AckFIN (struct TransferInfo *stats) {
    assert(stats!= NULL);
    int ackpacket_length = (int) (sizeof(struct UDP_datagram) + sizeof(struct server_hdr));
    char *ackPacket = (char *) calloc(1, ackpacket_length);
    int success = 0;
    assert(ackPacket);
    if (ackPacket) {
	struct UDP_datagram *UDP_Hdr = (struct UDP_datagram *)ackPacket;
	struct server_hdr *hdr = (struct server_hdr *)(UDP_Hdr+1);

	UDP_Hdr = (struct UDP_datagram*) ackPacket;
	int flags = HEADER_VERSION1;
#ifdef HAVE_INT64_T
	flags |=  HEADER_SEQNO64B;
#endif
	hdr->base.flags        = htonl((long) flags);
#ifdef HAVE_INT64_T
	hdr->base.total_len1   = htonl((long) (stats->cntBytes >> 32));
#else
	hdr->base.total_len1   = htonl(0x0);
#endif
	hdr->base.total_len2   = htonl((long) (stats->cntBytes & 0xFFFFFFFF));
        hdr->base.stop_sec     = htonl( (long) stats->ts.iEnd);
        hdr->base.stop_usec    = htonl( (long)((stats->ts.iEnd - (long)stats->ts.iEnd) * rMillion));
	hdr->base.error_cnt    = htonl((long) (stats->cntError & 0xFFFFFFFF));
	hdr->base.outorder_cnt = htonl((long) (stats->cntOutofOrder  & 0xFFFFFFFF));
	hdr->base.datagrams    = htonl((long) (stats->cntDatagrams & 0xFFFFFFFF));
	if (flags & HEADER_SEQNO64B) {
	    hdr->extend2.error_cnt2    = htonl((long) (stats->cntError >> 32));
	    hdr->extend2.outorder_cnt2 = htonl((long) (stats->cntOutofOrder >> 32) );
	    hdr->extend2.datagrams2    = htonl((long) (stats->cntDatagrams >> 32));
	}
	hdr->base.jitter1      = htonl((long) stats->jitter);
	hdr->base.jitter2      = htonl((long) ((stats->jitter - (long)stats->jitter) * rMillion));
	flags |=  SERVER_HEADER_EXTEND;
	hdr->extend.minTransit1  = htonl((long) stats->transit.totminTransit);
	hdr->extend.minTransit2  = htonl((long) ((stats->transit.totminTransit - (long)stats->transit.totminTransit) * rMillion));
	hdr->extend.maxTransit1  = htonl((long) stats->transit.totmaxTransit);
	hdr->extend.maxTransit2  = htonl((long) ((stats->transit.totmaxTransit - (long)stats->transit.totmaxTransit) * rMillion));
	hdr->extend.sumTransit1  = htonl((long) stats->transit.totsumTransit);
	hdr->extend.sumTransit2  = htonl((long) ((stats->transit.totsumTransit - (long)stats->transit.totsumTransit) * rMillion));
	hdr->extend.meanTransit1  = htonl((long) stats->transit.totmeanTransit);
	hdr->extend.meanTransit2  = htonl((long) ((stats->transit.totmeanTransit - (long)stats->transit.totmeanTransit) * rMillion));
	hdr->extend.m2Transit1  = htonl((long) stats->transit.totm2Transit);
	hdr->extend.m2Transit2  = htonl((long) ((stats->transit.totm2Transit - (long)stats->transit.totm2Transit) * rMillion));
	hdr->extend.vdTransit1  = htonl((long) stats->transit.totvdTransit);
	hdr->extend.vdTransit2  = htonl((long) ((stats->transit.totvdTransit - (long)stats->transit.totvdTransit) * rMillion));
	hdr->extend.cntTransit   = htonl(stats->transit.totcntTransit);
	hdr->extend.cntIPG = htonl((long) (stats->cntDatagrams / (stats->ts.iEnd - stats->ts.iStart)));
	hdr->extend.IPGsum = htonl(1);

#define TRYCOUNT 10
	int count = TRYCOUNT;
	while (--count) {
	    int rc;
	    struct timeval timeout;
	    // write data
#if defined(HAVE_LINUX_FILTER_H) && defined(HAVE_AF_PACKET)
	    // If in l2mode, use the AF_INET socket to write this packet
	    //
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("UDP server send done-ack w/server-stats to client (sock=%d)", stats->common->socket);
#endif
	    write(((stats->common->socketdrop > 0) ? stats->common->socketdrop : stats->common->socket), ackPacket, ackpacket_length);
#else
	    write(stats->common->socket, ackPacket, ackpacket_length);
#endif
	    // wait here is for silence, no more packets from the client
	    fd_set readSet;
	    FD_ZERO(&readSet);
	    FD_SET(stats->common->socket, &readSet);
	    timeout.tv_sec  = 1;
	    timeout.tv_usec = 0;
	    rc = select(stats->common->socket+1, &readSet, NULL, NULL, &timeout);
	    if (rc == 0) {
#ifdef HAVE_THREAD_DEBUG
		thread_debug("UDP server detected silence - server stats assumed received by client");
#endif
		success = 1;
		break;
	    }
	    rc = read(stats->common->socket, ackPacket, ackpacket_length);
	    WARN_errno(rc < 0, "read");
	    if (rc > 0) {
#ifdef HAVE_THREAD_DEBUG
		thread_debug("UDP server thinks server stats packet lost, will retransmit and try again", rc);
#endif
	    }
	}
	free(ackPacket);
    }
    if (!success)
	fprintf(stderr, warn_ack_failed, stats->common->socket);
}
// end write_UDP_AckFIN
