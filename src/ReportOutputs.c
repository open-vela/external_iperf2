/*---------------------------------------------------------------
 * Copyright (c) 2020
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
 * reporter output routines
 *
 * by Robert J. McMahon (rjmcmahon@rjmcmahon.com, bob.mcmahon@broadcom.com)
 * -------------------------------------------------------------------
 */
#include <math.h>
#include "headers.h"
#include "Settings.hpp"
#include "Reporter.h"
#include "Locale.h"
#include "SocketAddr.h"

#define SNBUFFERSIZE 256
#define SNBUFFEREXTENDSIZE 512
static char outbuffer[SNBUFFERSIZE]; // Buffer for printing
static char outbufferext[SNBUFFEREXTENDSIZE]; // Buffer for printing
static char llaw_buf[100];
static const int true = 1;
static int tcp_client_header_printed = 0;
static int tcp_server_header_printed = 0;
static int udp_client_header_printed = 0;
static int udp_server_header_printed = 0;

static inline void _print_stats_common (struct TransferInfo *stats) {
    assert(stats!=NULL);
    byte_snprintf(outbuffer, sizeof(outbuffer), (double) stats->cntBytes, toupper((int)stats->common->Format));
    if (stats->ts.iEnd < SMALLEST_INTERVAL_SEC) {
        stats->cntBytes = 0;
    }
    byte_snprintf(outbufferext, sizeof(outbufferext), (double)stats->cntBytes / (stats->ts.iEnd - stats->ts.iStart), \
		  stats->common->Format);
    outbuffer[sizeof(outbuffer)-1]='\0';
    outbufferext[sizeof(outbufferext)-1]='\0';
}

static inline void _output_outoforder(struct TransferInfo *stats) {
    if (stats->cntOutofOrder > 0) {
	printf(report_outoforder,
	       stats->transferID, stats->ts.iStart,
	       stats->ts.iEnd, stats->cntOutofOrder);
    }
    if (stats->l2counts.cnt) {
	printf(report_l2statistics,
	       stats->transferID, stats->ts.iStart,
	       stats->ts.iEnd, stats->l2counts.cnt, stats->l2counts.lengtherr,
	       stats->l2counts.udpcsumerr, stats->l2counts.unknown);
    }
}

//
//  Little's law is L = lambda * W, where L is queue depth,
//  lambda the arrival rate and W is the processing time
//
static inline void set_llawbuf(double lambda, double meantransit) {
    double L  = lambda * meantransit;
    byte_snprintf(llaw_buf, sizeof(llaw_buf), L, 'A');
    llaw_buf[sizeof(llaw_buf)-1] = '\0';
}

//TCP Output
void tcp_output_read (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	stats->header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}
//TCP read or server output
void tcp_output_read_enhanced (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	stats->header_printed = true;
	printf(report_bw_read_enhanced_header, (stats->sock_callstats.read.binsize/1024.0));
    }
    _print_stats_common(stats);
    printf(report_bw_read_enhanced_format,
	   stats->transferID, stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.read.cntRead,
	   stats->sock_callstats.read.bins[0],
	   stats->sock_callstats.read.bins[1],
	   stats->sock_callstats.read.bins[2],
	   stats->sock_callstats.read.bins[3],
	   stats->sock_callstats.read.bins[4],
	   stats->sock_callstats.read.bins[5],
	   stats->sock_callstats.read.bins[6],
	   stats->sock_callstats.read.bins[7]);
}
void tcp_output_read_enhanced_triptime (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	stats->header_printed = true;
	printf(report_bw_read_enhanced_netpwr_header, (stats->sock_callstats.read.binsize/1024.0));
    }
    double meantransit = (stats->transit.sumTransit / stats->transit.cntTransit);
    double lambda = (stats->arrivalSum > 0.0) ? ((double)stats->cntBytes / stats->arrivalSum) : 0.0;
    set_llawbuf(lambda, meantransit);
    if (stats->cntBytes) {
    printf(report_bw_read_enhanced_netpwr_format,
	   stats->transferID, stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.read.cntRead,
	   stats->sock_callstats.read.bins[0],
	   stats->sock_callstats.read.bins[1],
	   stats->sock_callstats.read.bins[2],
	   stats->sock_callstats.read.bins[3],
	   stats->sock_callstats.read.bins[4],
	   stats->sock_callstats.read.bins[5],
	   stats->sock_callstats.read.bins[6],
	   stats->sock_callstats.read.bins[7],
	   (meantransit * 1e3),
	   stats->transit.minTransit*1e3,
	   stats->transit.maxTransit*1e3,
	   (stats->transit.cntTransit < 2) ? 0 : sqrt(stats->transit.m2Transit / (stats->transit.cntTransit - 1)) / 1e3,
	   stats->transit.cntTransit,
	   stats->transit.cntTransit ? (long) ((double)stats->cntBytes / (double) stats->transit.cntTransit) : 0,
	   llaw_buf,
	   (meantransit > 0.0) ? (NETPOWERCONSTANT * ((double) stats->cntBytes) / (double) (stats->ts.iEnd - stats->ts.iStart) / meantransit) : NAN);
    } else {
	printf(report_bw_read_enhanced_format,
	   stats->transferID, stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.read.cntRead,
	   stats->sock_callstats.read.bins[0],
	   stats->sock_callstats.read.bins[1],
	   stats->sock_callstats.read.bins[2],
	   stats->sock_callstats.read.bins[3],
	   stats->sock_callstats.read.bins[4],
	   stats->sock_callstats.read.bins[5],
	   stats->sock_callstats.read.bins[6],
	   stats->sock_callstats.read.bins[7],
	   (meantransit * 1e3),
	   stats->transit.minTransit*1e3,
	   stats->transit.maxTransit*1e3,
	   (stats->transit.cntTransit < 2) ? 0 : sqrt(stats->transit.m2Transit / (stats->transit.cntTransit - 1)) / 1e3,
	   stats->transit.cntTransit,
	   stats->transit.cntTransit ? (long) ((double)stats->cntBytes / (double) stats->transit.cntTransit) : 0,
	   llaw_buf,
	   (meantransit > 0.0) ? (NETPOWERCONSTANT * ((double) stats->cntBytes) / (double) (stats->ts.iEnd - stats->ts.iStart) / meantransit) : NAN);
    }
    if (stats->framelatency_histogram) {
	histogram_print(stats->framelatency_histogram, stats->ts.iStart, stats->ts.iEnd);
    }
}

//TCP write or client output
void tcp_output_write (struct TransferInfo *stats) {
    if (!tcp_client_header_printed) {
	tcp_client_header_printed = true;
	printf(report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_format, stats->transferID,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}

void tcp_output_write_enhanced (struct TransferInfo *stats) {
    if (!tcp_client_header_printed) {
	tcp_client_header_printed = true;
	printf(report_bw_write_enhanced_header);
    }
    _print_stats_common(stats);
#ifndef HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS
    printf(report_sum_bw_write_enhanced_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.write.WriteCnt,
	   stats->sock_callstats.write.WriteErr,
	   stats->sock_callstats.write.TCPretry);
#else
    double netpower = 0;
    if (stats->sock_callstats.write.rtt > 0) {
	netpower = NETPOWERCONSTANT * (((double)stats->cntBytes / (double) (stats->ts.iEnd - stats->ts.iStart)) / (1e-6 * stats->sock_callstats.write.rtt));
    }
    if (stats->sock_callstats.write.cwnd > 0) {
	printf(report_bw_write_enhanced_format,
	       stats->transferID, stats->ts.iStart, stats->ts.iEnd,
	       outbuffer, outbufferext,
	       stats->sock_callstats.write.WriteCnt,
	       stats->sock_callstats.write.WriteErr,
	       stats->sock_callstats.write.TCPretry,
	       stats->sock_callstats.write.cwnd,
	       stats->sock_callstats.write.rtt,
	       netpower);
    } else {
	printf(report_bw_write_enhanced_nocwnd_format,
	       stats->transferID, stats->ts.iStart, stats->ts.iEnd,
	       outbuffer, outbufferext,
	       stats->sock_callstats.write.WriteCnt,
	       stats->sock_callstats.write.WriteErr,
	       stats->sock_callstats.write.TCPretry,
	       stats->sock_callstats.write.rtt,
	       netpower);
    }
#endif
}

//UDP output
void udp_output_read (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	printf("%s", report_bw_jitter_loss_header);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    printf( report_bw_jitter_loss_format, stats->transferID,
	    stats->ts.iStart, stats->ts.iEnd,
	    outbuffer, outbufferext,
	    stats->jitter*1000.0, stats->cntError, stats->cntDatagrams,
	    (100.0 * stats->cntError) / stats->cntDatagrams);
    _output_outoforder(stats);
}

void udp_output_read_enhanced (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	printf("%s", report_bw_jitter_loss_enhanced_header);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    if (!stats->cntIPG) {
	printf(report_bw_jitter_loss_suppress_enhanced_format, stats->transferID,
	       stats->ts.iStart, stats->ts.iEnd,
	       outbuffer, outbufferext,
	       0.0, stats->cntError,
	       stats->cntDatagrams,
	       0.0,0.0,0.0,0.0,0.0,0.0);
    } else {
	double meantransit = (stats->transit.sumTransit / stats->transit.cntTransit);
	double lambda = (stats->arrivalSum > 0.0) ? ((double)stats->cntBytes / stats->arrivalSum) : 0.0;
	set_llawbuf(lambda, meantransit);
	printf(report_bw_jitter_loss_enhanced_format, stats->transferID,
	       stats->ts.iStart, stats->ts.iEnd,
	       outbuffer, outbufferext,
	       stats->jitter*1000.0, stats->cntError, stats->cntDatagrams,
	       (100.0 * stats->cntError) / stats->cntDatagrams,
	       (meantransit * 1e3),
	       stats->transit.minTransit*1e3,
	       stats->transit.maxTransit*1e3,
	       (stats->transit.cntTransit < 2) ? 0 : sqrt(stats->transit.m2Transit / (stats->transit.cntTransit - 1)) / 1e3,
	       (stats->cntIPG / stats->IPGsum),
	       llaw_buf,
	       (meantransit > 0.0) ? (NETPOWERCONSTANT * ((double)stats->cntBytes) / (double) (stats->ts.iEnd - stats->ts.iStart) / meantransit) : 0);
    }
    _output_outoforder(stats);
}
void udp_output_read_enhanced_triptime (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	printf("%s", report_bw_jitter_loss_enhanced_isoch_header);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    if (!stats->cntIPG) {
	printf(report_bw_jitter_loss_suppress_enhanced_format, stats->transferID,
	       stats->ts.iStart, stats->ts.iEnd,
	       outbuffer, outbufferext,
	       0.0, stats->cntError,
	       stats->cntDatagrams,
	       0.0,0.0,0.0,0.0,0.0,0.0);

    } else {
	// If the min latency is out of bounds of a realistic value
	// assume the clocks are not synched and suppress the
	// latency output
	if ((stats->transit.minTransit > UNREALISTIC_LATENCYMINMAX) ||
	    (stats->transit.minTransit < UNREALISTIC_LATENCYMINMIN)) {
	    printf(report_bw_jitter_loss_suppress_enhanced_format, stats->transferID,
		   stats->ts.iStart, stats->ts.iEnd,
		   outbuffer, outbufferext,
		   stats->jitter*1000.0, stats->cntError, stats->cntDatagrams,
		   (100.0 * stats->cntError) / stats->cntDatagrams,
		   (stats->cntIPG / stats->IPGsum));
	} else {
	    double meantransit = (stats->transit.sumTransit / stats->transit.cntTransit);
	    double lambda = (stats->arrivalSum > 0.0) ? ((double)stats->cntBytes / stats->arrivalSum) : 0.0;
	    set_llawbuf(lambda, meantransit);
	    printf(report_bw_jitter_loss_enhanced_isoch_format, stats->transferID,
		   stats->ts.iStart, stats->ts.iEnd,
		   outbuffer, outbufferext,
		   stats->jitter*1e3, stats->cntError, stats->cntDatagrams,
		   (100.0 * stats->cntError) / stats->cntDatagrams,
		   (meantransit * 1e3),
		   stats->transit.minTransit*1e3,
		   stats->transit.maxTransit*1e3,
		   (stats->transit.cntTransit < 2) ? 0 : sqrt(stats->transit.m2Transit / (stats->transit.cntTransit - 1)) / 1e3,
		   (stats->cntIPG / stats->IPGsum),
		   ((stats->arrivalSum > 0.0) ? llaw_buf : 0),
		   ((meantransit > 0.0) ? (NETPOWERCONSTANT * ((double) stats->cntBytes) / (double) (stats->ts.iEnd - stats->ts.iStart) / meantransit) : 0),
		   stats->isochstats.framecnt, stats->isochstats.framelostcnt);
	}
    }
    _output_outoforder(stats);
}
void udp_output_write (struct TransferInfo *stats) {
    if(!stats->header_printed ) {
	printf("%s", report_bw_header);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    printf(report_bw_format, stats->transferID,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}

void udp_output_write_enhanced (struct TransferInfo *stats) {
    if(!stats->header_printed ) {
	printf("%s", report_bw_pps_enhanced_format);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    printf(report_bw_pps_enhanced_format, stats->transferID,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.write.WriteCnt,
	   stats->sock_callstats.write.WriteErr,
	   (stats->cntIPG ? (stats->cntIPG / stats->IPGsum) : 0.0));
}
void udp_output_write_enhanced_isoch (struct TransferInfo *stats) {
    // UDP Client reporting
    if(!stats->header_printed) {
	printf("%s", report_bw_pps_enhanced_isoch_header);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    printf(report_bw_pps_enhanced_isoch_format, stats->transferID,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.write.WriteCnt,
	   stats->sock_callstats.write.WriteErr,
	   (stats->cntIPG ? (stats->cntIPG / stats->IPGsum) : 0.0),
	   stats->isochstats.framecnt,
	   stats->isochstats.framelostcnt, stats->isochstats.slipcnt);
}

// Sum reports
void udp_output_sum_read(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf(report_sum_bw_jitter_loss_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->jitter*1000.0, stats->cntError, stats->cntDatagrams,
	   (100.0 * stats->cntError) / stats->cntDatagrams);
    if (stats->cntOutofOrder > 0) {
	printf(report_sum_outoforder,
	       stats->ts.iStart,
	       stats->ts.iEnd, stats->cntOutofOrder);
    }
}
void udp_output_sum_write(struct TransferInfo *stats) {
    if (!udp_client_header_printed) {
	udp_client_header_printed = true;
	printf(report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_sum_bw_jitter_loss_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->jitter*1000.0, stats->cntError, stats->cntDatagrams,
	   (100.0 * stats->cntError) / stats->cntDatagrams);
    printf(report_sum_datagrams, stats->cntDatagrams);
}
void udp_output_sum_read_enhanced(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf( report_sum_bw_pps_enhanced_format,
	    stats->ts.iStart, stats->ts.iEnd,
	    outbuffer, outbufferext,
	    stats->cntError, stats->cntDatagrams,
	    (stats->cntIPG ? (stats->cntIPG / stats->IPGsum) : 0.0));
}
void udp_output_sum_write_enhanced(struct TransferInfo *stats) {
    if (!tcp_client_header_printed) {
	tcp_client_header_printed = true;
	printf(report_bw_header);
    }
    _print_stats_common(stats);
    printf( report_sum_bw_pps_enhanced_format,
	    stats->ts.iStart, stats->ts.iEnd,
	    outbuffer, outbufferext,
	    stats->sock_callstats.write.WriteCnt,
	    stats->sock_callstats.write.WriteErr,
	    (stats->cntIPG ? (stats->cntIPG / stats->IPGsum) : 0.0));
    printf(report_sum_datagrams, stats->cntDatagrams);
}
void tcp_output_sum_read(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf(report_sum_bw_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}
void tcp_output_sum_write(struct TransferInfo *stats) {
    if (!tcp_client_header_printed) {
	tcp_client_header_printed = true;
	printf(report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_sum_bw_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}
void tcp_output_sumcnt_write(struct TransferInfo *stats) {
    if (!tcp_client_header_printed) {
	tcp_client_header_printed = true;
	printf(report_bw_sumcnt_header);
    }
    _print_stats_common(stats);
    printf(report_sumcnt_bw_format, stats->threadcnt,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}
void tcp_output_sum_read_enhanced(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf(report_sum_bw_read_enhanced_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.read.cntRead,
	   stats->sock_callstats.read.bins[0],
	   stats->sock_callstats.read.bins[1],
	   stats->sock_callstats.read.bins[2],
	   stats->sock_callstats.read.bins[3],
	   stats->sock_callstats.read.bins[4],
	   stats->sock_callstats.read.bins[5],
	   stats->sock_callstats.read.bins[6],
	   stats->sock_callstats.read.bins[7]);
}
void tcp_output_sum_write_enhanced(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf(report_sum_bw_write_enhanced_format,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.write.WriteCnt,
	   stats->sock_callstats.write.WriteErr,
	   stats->sock_callstats.write.TCPretry);
}
void tcp_output_sumcnt_write_enhanced (struct TransferInfo *stats) {
    if (!tcp_client_header_printed) {
	tcp_client_header_printed = true;
	printf(report_bw_write_sumcnt_enhanced_header);
    }
    _print_stats_common(stats);
    printf(report_sumcnt_bw_write_enhanced_format, stats->threadcnt,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->sock_callstats.write.WriteCnt,
	   stats->sock_callstats.write.WriteErr,
	   stats->sock_callstats.write.TCPretry);
}
/*
 * Report the client or listener Settings in default style
 */
void reporter_output_settings (struct ReportHeader *reporthdr) {
#if 0
    assert(reporthdr != NULL);
    struct ReportSettings *data = (struct ReportSettings *) reporthdr->this_report;
    int pid =  (int)  getpid();
    printf("%s", separator_line);
    if (data->common->ThreadMode == kMode_Listener) {
        printf(isEnhanced(data->common) ? server_pid_port : server_port,
	       (isUDP(data->common) ? "UDP" : "TCP"), data->common->Port, pid );
    } else if (!data->common->Ifrnametx) {
	printf(isEnhanced(data->common) ? client_pid_port : client_port, data->common->Host,
	       (isUDP(data->common) ? "UDP" : "TCP"), data->common->Port, pid);
    } else {
	printf(client_pid_port_dev, data->common->Host,
	       (isUDP(data->common) ? "UDP" : "TCP"), data->common->Port, pid, data->common->Ifrnametx);
    }
    if (data->common->Localhost != NULL) {
	if (isEnhanced(data->common) && !SockAddr_isMulticast(&data->local)) {
	    if (data->common->Ifrname)
		printf(bind_address_iface, data->common->Localhost, data->common->Ifrname);
	    else
		printf(bind_address, data->common->Localhost);
	}
	if ((data->common->ThreadMode != kMode_Client) && SockAddr_isMulticast(&data->local)) {
	    if(!data->common->SSMMulticastStr)
		if (!data->common->Ifrname)
		    printf(join_multicast, data->common->Localhost );
		else
		    printf(join_multicast_starg_dev, data->common->Localhost,data->common->Ifrname);
	    else if(!data->common->Ifrname)
		printf(join_multicast_sg, data->common->SSMMulticastStr, data->common->Localhost);
	    else
		printf(join_multicast_sg_dev, data->common->SSMMulticastStr, data->common->Localhost, data->common->Ifrname);
        }
    }
    if (isIsochronous(data->common)) {
	int len;
	char meanbuf[40];
	char variancebuf[40];
	byte_snprintf(meanbuf, sizeof(meanbuf), data->isochstats.mMean, 'a');
	byte_snprintf(variancebuf, sizeof(variancebuf), data->isochstats.mVariance, 'a');
	meanbuf[39]='\0'; variancebuf[39]='\0';
	printf(client_udp_isochronous, data->isochstats.mFPS, meanbuf, variancebuf, (data->isochstats.mBurstInterval/1000.0), (data->isochstats.mBurstIPG/1000.0));
	if ((data->isochstats.mMean / data->isochstats.mFPS) < ((double) (sizeof(struct UDP_datagram) + sizeof(struct client_hdr_v1) + sizeof(struct client_hdr_udp_isoch_tests)))) {
	    fprintf(stderr, "Warning: Requested mean too small to carry isoch payload, code will auto adjust payload sizes\n");
	}
    } else if (isUDP(data->common)) {
	    if (data->common->ThreadMode != kMode_Listener) {
		double delay_target;
		if (data->common->UDPRateUnits == kRate_BW) {
		    delay_target = (double) (data->common->BufLen * 8000000.0 / data->common->UDPRate);
		} else {
		    delay_target = (1e6 / data->common->UDPRate);
		}
#ifdef HAVE_CLOCK_NANOSLEEP
		printf(client_datagram_size, data->common->BufLen, delay_target);
#else
  #ifdef HAVE_KALMAN
		printf(client_datagram_size_kalman, data->common->BufLen, delay_target);
  #else
		printf(client_datagram_size, data->common->BufLen, delay_target);
  #endif
#endif
	    } else {
		printf(server_datagram_size, data->common->BufLen);
	    }
	    if (SockAddr_isMulticast(&data->connection.peer)) {
		printf(multicast_ttl, data->common.TTL);
	    }
	} else if (isEnhanced(data->common)) {
	    byte_snprintf(buffer, sizeof(buffer), data->common->BufLen, toupper( (int)data->info.mFormat));
	    buffer[(sizeof(buffer)-1)] = '\0';
	    printf("%s: %s\n", ((data->common->ThreadMode == kMode_Client) ?
				client_write_size : server_read_size), buffer);
	}
    if (isFQPacing(data) && (data->common->ThreadMode == kMode_Client)) {
	char tmpbuf[40];
	byte_snprintf(tmpbuf, sizeof(tmpbuf), data->FQPacingRate, 'a');
	tmpbuf[39]='\0';
        printf(client_fq_pacing,tmpbuf);
    }
    byte_snprintf(buffer, sizeof(buffer), data->connection.winsize, toupper( (int)data->info.mFormat));
	    buffer[(sizeof(buffer)-1)] = '\0';
	    printf("%s: %s", (isUDP( data ) ? udp_buffer_size : tcp_window_size), buffer );
    if (data->connection.winsize_requested == 0 ) {
        printf(" %s", window_default );
    } else if (data->connection.winsize != data->connection.winsize_requested) {
        byte_snprintf( buffer, sizeof(buffer), data->connection.winsize_requested,
                       toupper( (int)data->info.mFormat));
	    buffer[(sizeof(buffer)-1)] = '\0';
	printf( warn_window_requested, buffer );
    }
    printf( "\n%s", separator_line );
#endif
}

void reporter_connect_printf_tcp_final (struct ReportHeader *reporthdr) {
#if 0
    struct TransferInfo *stats = &data->info;
    if (reporthdr->connect_times.cnt > 1) {
        double variance = (reporthdr->connect_times.cnt < 2) ? 0 : sqrt(reporthdr->connect_times.m2 / (reporthdr->connect_times.cnt - 1));
        fprintf(stdout, "[ CT] final connect times (min/avg/max/stdev) = %0.3f/%0.3f/%0.3f/%0.3f ms (tot/err) = %d/%d\n", \
		reporthdr->connect_times.min,  \
	        (reporthdr->connect_times.sum / reporthdr->connect_times.cnt), \
		reporthdr->connect_times.max, variance,  \
		(reporthdr->connect_times.cnt + reporthdr->connect_times.err), \
		reporthdr->connect_times.err);
    }
#endif
}

void reporter_peerversion (struct thread_Settings *inSettings, int upper, int lower) {
#if 0
    int rel, major, minor, alpha;
    inSettings->peerversion[0] = '\0';

    rel = (upper & 0xFFFF0000) >> 16;
    major = (upper & 0x0000FFFF);
    minor = (lower & 0xFFFF0000) >> 16;
    alpha = (lower & 0x0000000F);
    sprintf(inSettings->peerversion," (peer %d.%d.%d)", rel, major, minor);
    switch(alpha) {
    case 0:
	sprintf(inSettings->peerversion + strlen(inSettings->peerversion) - 1,"-alpha)");
	break;
    case 1:
	sprintf(inSettings->peerversion + strlen(inSettings->peerversion) - 1,"-beta)");
	break;
    case 2:
	sprintf(inSettings->peerversion + strlen(inSettings->peerversion) - 1,"-rc)");
	break;
    case 3:
	break;
    default:
	sprintf(inSettings->peerversion + strlen(inSettings->peerversion) - 1, "-unk)");
    }
#endif
}
void reporter_print_connection_report(struct ConnectionInfo *report) {
}
void reporter_print_settings_report(struct ReportSettings *report) {
}
void reporter_print_server_relay_report(struct TransferInfo *repor) {
}
