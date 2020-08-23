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
#include "List.h"

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
    my_str_copy(&(*common)->Host, inSettings->mHost);
    my_str_copy(&(*common)->Localhost, inSettings->mLocalhost);
    my_str_copy(&(*common)->Ifrname, inSettings->mIfrname);
    my_str_copy(&(*common)->Ifrnametx, inSettings->mIfrnametx);
    my_str_copy(&(*common)->SSMMulticastStr, inSettings->mSSMMulticastStr);
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
}

static void free_common_copy (struct ReportCommon *common) {
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
    free(common);
}

struct SumReport* InitSumReport(struct thread_Settings *inSettings, int inID) {
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
    sumreport->info.transferID = -1;
    sumreport->info.threadcnt = 0;
    // Only initialize the interval time here
    // The startTime and nextTime for summing reports will be set by
    // the reporter thread in realtime
    if ((inSettings->mInterval) && (inSettings->mIntervalMode == kInterval_Time)) {
	sumreport->info.ts.intervalTime.tv_sec = (long) (inSettings->mInterval / rMillion);
	sumreport->info.ts.intervalTime.tv_usec = (long) (inSettings->mInterval % rMillion);
    }
    if (inSettings->mThreadMode == kMode_Server) {
	sumreport->info.sock_callstats.read.binsize = inSettings->mBufLen / 8;
    }
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init sum report %p id=%d", (void *)sumreport, inID);
#endif
    return sumreport;
}

void FreeSumReport (struct SumReport *sumreport) {
    assert(sumreport);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Free multi report hdr=%p", (void *)sumreport);
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
    if (ireport->packetring && ireport->info.total.Bytes.current && \
	!TimeZero(ireport->info.ts.intervalTime) && (ireport->reporter_thread_suspends < 3)) {
	fprintf(stdout, "WARN: this test was likley CPU bound (%d) (or may not be detecting the underlying network devices)\n", \
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


static void Free_cReport (struct ConnectionInfo *report) {
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
	Free_cReport((struct ConnectionInfo *)reporthdr->this_report);
	break;
    case SETTINGS_REPORT:
	Free_sReport((struct ReportSettings *)reporthdr->this_report);
	break;
    case SERVER_RELAY_REPORT:
	Free_srReport((struct TransferInfo *)reporthdr->this_report);
	break;
    default:
	fprintf(stderr, "Invalid report type in free\n");
	exit(1);
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
    Mutex_Lock(&sumreport->reference.lock);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Sum multiheader %p ref=%d->%d", (void *)sumreport, sumreport->reference.count, (sumreport->reference.count + 1));
#endif
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
    ireport->GroupSumReport = inSettings->mSumReport;
    ireport->FullDuplexReport = inSettings->mBidirReport;

    // Copy common settings into the transfer report section
    common_copy(&ireport->info.common, inSettings);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Job report %p uses multireport %p and bidirreport is %p (socket=%d)", (void *)reporthdr->this_report, (void *)inSettings->mSumReport, (void *)inSettings->mBidirReport, inSettings->mSock);
#endif
    // Create a new packet ring which is used to communicate
    // packet stats from the traffic thread to the reporter
    // thread.  The reporter thread does all packet accounting
    ireport->packetring = packetring_init((inSettings->numreportstructs ? inSettings->numreportstructs : NUM_REPORT_STRUCTS), \
					  &ReportCond, &inSettings->awake_me);
    if (inSettings->numreportstructs)
	fprintf (stdout, "[%3d] NUM_REPORT_STRUCTS override from %d to %d\n", inSettings->mSock, NUM_REPORT_STRUCTS, inSettings->numreportstructs);

    // Set up the function vectors, there are three
    // 1) packet_handler: does packet accounting per the test and protocol
    // 2) transfer_protocol_handler: performs output, e.g. interval reports, per the test and protocol
    // 3) transfer_protocol_sum_handler: performs summing output when multiple traffic threads

    switch (inSettings->mThreadMode) {
    case kMode_Server :
	if (isUDP(inSettings)) {
	    ireport->packet_handler = reporter_handle_packet_server_udp;
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_server_udp;
	    if (isIsochronous(inSettings))
		ireport->info.output_handler = udp_output_read_enhanced_triptime;
	    else if (isEnhanced(inSettings))
		ireport->info.output_handler = udp_output_read_enhanced;
	    else
		ireport->info.output_handler = udp_output_read;
	    if (ireport->GroupSumReport)
		ireport->GroupSumReport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_udp;
	    if (ireport->FullDuplexReport)
		ireport->FullDuplexReport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_udp;
	} else {
	    ireport->packet_handler = reporter_handle_packet_server_tcp;
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_server_tcp;
	    if (isTripTime(inSettings))
		ireport->info.output_handler = tcp_output_read_enhanced_triptime;
	    else if (isEnhanced(inSettings)) {
		ireport->info.output_handler = (isSumOnly(inSettings) ? NULL : tcp_output_read_enhanced);
		if (ireport->GroupSumReport) {
		    ireport->GroupSumReport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_tcp;
		    ireport->GroupSumReport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_read_enhanced : tcp_output_sum_read_enhanced);
		}
		if (ireport->FullDuplexReport)
		    ireport->FullDuplexReport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_tcp;
	    } else {
		ireport->info.output_handler = (isSumOnly(inSettings) ? NULL : tcp_output_read);
		if (ireport->GroupSumReport) {
		    ireport->GroupSumReport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_tcp;
		    ireport->GroupSumReport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_read : tcp_output_sum_read);
		}
		if (ireport->FullDuplexReport)
		    ireport->FullDuplexReport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_tcp;
	    }
	}
	break;
    case kMode_Client :
	ireport->packet_handler = reporter_handle_packet_client;
	if (isUDP(inSettings)) {
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_client_udp;
	    if (ireport->GroupSumReport) {
		ireport->GroupSumReport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_udp;
		ireport->GroupSumReport->info.output_handler = udp_output_sum_write_enhanced;
	    }
	    if (isIsochronous(inSettings)) {
		ireport->info.output_handler = udp_output_write_enhanced_isoch;
	    } else if (isEnhanced(inSettings)) {
		ireport->info.output_handler = udp_output_write_enhanced;
	    } else {
		ireport->info.output_handler = udp_output_write;
		if (ireport->GroupSumReport) {
		    ireport->GroupSumReport->info.output_handler = udp_output_sum_write;
		}
	    }
	    if (ireport->FullDuplexReport)
		ireport->FullDuplexReport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_udp;
	} else {
	    ireport->transfer_protocol_handler = reporter_transfer_protocol_client_tcp;
	    if (isEnhanced(inSettings)) {
		ireport->info.output_handler = (isSumOnly(inSettings) ? NULL : tcp_output_write_enhanced);
		if (ireport->GroupSumReport) {
		    ireport->GroupSumReport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_tcp;
		    ireport->GroupSumReport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_write_enhanced : tcp_output_sum_write_enhanced);
		}
	    } else {
		ireport->info.output_handler = (isSumOnly(inSettings) ? NULL : tcp_output_write);
		if (ireport->GroupSumReport) {
		    ireport->GroupSumReport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_tcp;
		    ireport->GroupSumReport->info.output_handler = (isSumOnly(inSettings) ? tcp_output_sumcnt_write : tcp_output_sum_write);
		}
	    }
	    if (ireport->FullDuplexReport)
		ireport->FullDuplexReport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_tcp;
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
	ireport->packet_handler = NULL;
    }

#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init data report %p using packetring=%p cond=%p", \
		 (void *)ireport, (void *)(ireport->packetring), (void *)(ireport->packetring->awake_producer));
#endif
    ireport->info.transferID = inSettings->mSock;

    switch (inSettings->mIntervalMode) {
    case kInterval_Time :
	{
	    ireport->info.ts.intervalTime.tv_sec = (long) (inSettings->mInterval / rMillion);
	    ireport->info.ts.intervalTime.tv_usec = (long) (inSettings->mInterval % rMillion);
	    ireport->transfer_interval_handler = reporter_condprint_time_interval_report;
	}
	break;
    case kInterval_Frames :
	if (isUDP(inSettings)) {
	    ireport->transfer_interval_handler = reporter_condprint_frame_interval_report_udp;
	} else {
	    ireport->transfer_interval_handler = reporter_condprint_frame_interval_report_tcp;
	}
	break;
    default :
	ireport->transfer_interval_handler = NULL;
	break;
    }
    if (inSettings->mThreadMode == kMode_Server) {
	ireport->info.sock_callstats.read.binsize = inSettings->mBufLen / 8;
	if (isRxHistogram(inSettings) && isUDP(inSettings)) {
	    char name[] = "T8";
	    ireport->info.latency_histogram =  histogram_init(inSettings->mRXbins,inSettings->mRXbinsize,0,\
							      pow(10,inSettings->mRXunits), \
							      inSettings->mRXci_lower, inSettings->mRXci_upper, ireport->info.transferID, name);
	}
	if (isRxHistogram(inSettings) && (isIsochronous(inSettings) || isTripTime(inSettings))) {
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
    // Fill out known fields for the connection report
    reporter_peerversion(creport, inSettings->peer_version_u, inSettings->peer_version_u);
    creport->peer = inSettings->peer;
    creport->size_peer = inSettings->size_peer;
    creport->local = inSettings->local;
    creport->size_local = inSettings->size_local;
    creport->connecttime = ct;
    if (isEnhanced(inSettings) && isTxStartTime(inSettings)) {
	creport->epochStartTime.tv_sec = inSettings->txstart_epoch.tv_sec;
	creport->epochStartTime.tv_usec = inSettings->txstart_epoch.tv_usec;
    } else if (isTripTime(inSettings)) {
	creport->epochStartTime.tv_sec = inSettings->accept_time.tv_sec;
	creport->epochStartTime.tv_usec = inSettings->accept_time.tv_usec;
    }
    // RJM FIX THIS
    if (isFQPacing(inSettings) && (inSettings->mThreadMode == kMode_Client)) {
	char tmpbuf[40];
	byte_snprintf(tmpbuf, sizeof(tmpbuf), inSettings->mFQPacingRate, 'a');
	tmpbuf[39]='\0';
        printf(client_fq_pacing,tmpbuf);
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
    thread_debug("Init server relay report %p size %ld", (void *)reporthdr, sizeof(struct ReportHeader));
#endif
    reporthdr->type = SERVER_RELAY_REPORT;
    reporthdr->ReportMode = inSettings->mReportMode;
    struct ServerRelay *sr_report = (struct ServerRelay *)&reporthdr->this_report;
    common_copy(&sr_report->info.common, inSettings);
    struct TransferInfo *stats = (struct TransferInfo *)&sr_report->info;
    stats->transferID = inSettings->mSock;
#if 0
    stats->jitter = ntohl(server->base.jitter1);
    stats->jitter += ntohl(server->base.jitter2) / (double)rMillion;
#ifdef HAVE_INT64_T
    stats->TotalLen = (((intmax_t) ntohl(server->base.total_len1)) << 32) + \
	ntohl(server->base.total_len2);
#else
    stats->TotalLen = (intmax_t) ntohl(server->base.total_len2);
#endif
    stats->startTime = 0;
    stats->endTime = ntohl(server->base.stop_sec);
    stats->endTime += ntohl(server->base.stop_usec) / (double)rMillion;
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
    if ((flags & HEADER_EXTEND) != 0) {
	stats->mEnhanced = 1;
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
	stats->IPGcnt = ntohl(server->extend.IPGcnt);
	stats->IPGsum = ntohl(server->extend.IPGsum);
    }
#endif
    sr_report->peer = inSettings->local;
    sr_report->size_peer = inSettings->size_local;
    sr_report->local = inSettings->peer;
    sr_report->size_local = inSettings->size_peer;
    return reporthdr;
}
