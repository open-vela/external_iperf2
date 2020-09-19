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
static char outbufferext2[SNBUFFEREXTENDSIZE]; // Buffer for printing
static char llaw_buf[100];
static const int true = 1;
static int tcp_client_header_printed = 0;
static int tcp_server_header_printed = 0;
static int tcp_bidir_header_printed = 0;
static int udp_client_header_printed = 0;
static int udp_server_header_printed = 0;
static int report_bw_read_enhanced_netpwr_header_printed = 0;

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
void tcp_output_bidir_sum (struct TransferInfo *stats) {
    if (!tcp_bidir_header_printed) {
	tcp_bidir_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_sum_bidir_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}

void tcp_output_bidir_sum_enhanced (struct TransferInfo *stats) {
    if (!tcp_bidir_header_printed) {
	tcp_bidir_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_sum_bidir_enhanced_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}

void tcp_output_bidir_read (struct TransferInfo *stats) {
    if (!tcp_bidir_header_printed) {
	tcp_bidir_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_sum_bidir_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}

void tcp_output_bidir_read_enhanced (struct TransferInfo *stats) {
    if (!tcp_bidir_header_printed) {
	tcp_bidir_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_sum_bidir_enhanced_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}
void tcp_output_bidir_write (struct TransferInfo *stats) {
    if (!tcp_bidir_header_printed) {
	tcp_bidir_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_sum_bidir_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}

void tcp_output_bidir_write_enhanced (struct TransferInfo *stats) {
    if (!tcp_bidir_header_printed) {
	tcp_bidir_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_sum_bidir_enhanced_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}

void tcp_output_read (struct TransferInfo *stats) {
    if (!tcp_server_header_printed) {
	tcp_server_header_printed = true;
	printf("%s", report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_bw_format, stats->transferID, stats->ts.iStart, stats->ts.iEnd, outbuffer, outbufferext);
}
//TCP read or server output
void tcp_output_read_enhanced (struct TransferInfo *stats) {
    if (!tcp_server_header_printed) {
	tcp_server_header_printed = true;
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
    if(!report_bw_read_enhanced_netpwr_header_printed) {
	report_bw_read_enhanced_netpwr_header_printed = true;
	printf(report_bw_read_enhanced_netpwr_header, (stats->sock_callstats.read.binsize/1024.0));
    }
    double meantransit = (stats->transit.cntTransit > 0) ? (stats->transit.sumTransit / stats->transit.cntTransit) : 0;
    double lambda = (stats->IPGsum > 0.0) ? ((double)stats->cntBytes / stats->IPGsum) : 0.0;
    set_llawbuf(lambda, meantransit);
    _print_stats_common(stats);
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
    printf(report_bw_write_enhanced_format,
	   stats->transferID, stats->ts.iStart, stats->ts.iEnd,
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
    if (!udp_server_header_printed && !stats->header_printed) {
	udp_server_header_printed = true;
	printf("%s", report_bw_jitter_loss_header);
    }
    _print_stats_common(stats);
    printf(report_bw_jitter_loss_format, stats->transferID,
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
	double meantransit = (stats->transit.cntTransit > 0) ? (stats->transit.sumTransit / stats->transit.cntTransit) : 0;
	double lambda = (stats->IPGsum > 0.0) ? ((double)stats->cntBytes / stats->IPGsum) : 0.0;
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
	    double lambda = (stats->IPGsum > 0.0) ? ((double)stats->cntBytes / stats->IPGsum) : 0.0;
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
		   ((stats->IPGsum > 0.0) ? llaw_buf : 0),
		   ((meantransit > 0.0) ? (NETPOWERCONSTANT * ((double) stats->cntBytes) / (double) (stats->ts.iEnd - stats->ts.iStart) / meantransit) : 0),
		   stats->isochstats.framecnt, stats->isochstats.framelostcnt);
	}
    }
    _output_outoforder(stats);
}
void udp_output_write (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	printf("%s", report_bw_header);
	stats->header_printed = true;
    }
    _print_stats_common(stats);
    printf(report_bw_format, stats->transferID,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}

void udp_output_write_enhanced (struct TransferInfo *stats) {
    if(!stats->header_printed) {
	printf("%s", report_bw_pps_enhanced_header);
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
void udp_output_sumcnt_read(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf(report_sumcnt_bw_jitter_loss_format, stats->threadcnt,
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
}
void udp_output_sumcnt_write(struct TransferInfo *stats) {
    if (!udp_client_header_printed) {
	udp_client_header_printed = true;
	printf(report_bw_header);
    }
    _print_stats_common(stats);
    printf(report_sumcnt_bw_jitter_loss_format, stats->threadcnt,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext,
	   stats->jitter*1000.0, stats->cntError, stats->cntDatagrams,
	   (100.0 * stats->cntError) / stats->cntDatagrams);
}
void udp_output_sum_read_enhanced(struct TransferInfo *stats) {
    _print_stats_common(stats);
    printf(report_sum_bw_pps_enhanced_format,
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
    printf(report_sum_bw_pps_enhanced_format,
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
void tcp_output_sumcnt_read(struct TransferInfo *stats) {
    if (!tcp_server_header_printed) {
	tcp_server_header_printed = true;
	printf(report_bw_sumcnt_header);
    }
    _print_stats_common(stats);
    printf(report_sumcnt_bw_format, stats->threadcnt,
	   stats->ts.iStart, stats->ts.iEnd,
	   outbuffer, outbufferext);
}
void tcp_output_sumcnt_read_enhanced (struct TransferInfo *stats) {
    if (!tcp_server_header_printed) {
	tcp_server_header_printed = true;
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
static void output_window_size (struct ReportSettings *report) {
    int winsize = getsock_tcp_windowsize(report->common->socket, (report->common->ThreadMode != kMode_Client ? 0 : 1));
    byte_snprintf(outbuffer, sizeof(outbuffer), winsize, toupper(report->common->Format));
    outbuffer[(sizeof(outbuffer)-1)] = '\0';
    printf("%s: %s", (isUDP(report->common) ? udp_buffer_size : tcp_window_size), outbuffer);
    if (report->common->winsize_requested == 0) {
        printf(" %s", window_default);
    } else if (winsize != report->common->winsize_requested) {
        byte_snprintf(outbuffer, sizeof(outbuffer), report->common->winsize_requested,
                       toupper((int)report->common->Format));
	outbuffer[(sizeof(outbuffer)-1)] = '\0';
	printf(warn_window_requested, outbuffer);
    }
}
static void reporter_output_listener_settings (struct ReportSettings *report) {
    printf(isEnhanced(report->common) ? server_pid_port : server_port,
	   (isUDP(report->common) ? "UDP" : "TCP"), report->common->Port, report->pid);
    if (report->common->Localhost != NULL) {
	if (isEnhanced(report->common) && !SockAddr_isMulticast(&report->local)) {
	    if (report->common->Ifrname)
		printf(bind_address_iface, report->common->Localhost, report->common->Ifrname);
	    else
		printf(bind_address, report->common->Localhost);
	}
	if (SockAddr_isMulticast(&report->local)) {
	    if(!report->common->SSMMulticastStr)
		if (!report->common->Ifrname)
		    printf(join_multicast, report->common->Localhost);
		else
		    printf(join_multicast_starg_dev, report->common->Localhost,report->common->Ifrname);
	    else if(!report->common->Ifrname)
		printf(join_multicast_sg, report->common->SSMMulticastStr, report->common->Localhost);
	    else
		printf(join_multicast_sg_dev, report->common->SSMMulticastStr, report->common->Localhost, report->common->Ifrname);
        }
    }
    if (isEnhanced(report->common)) {
	byte_snprintf(outbuffer, sizeof(outbuffer), report->common->BufLen, toupper((int)report->common->Format));
	outbuffer[(sizeof(outbuffer)-1)] = '\0';
	printf("%s: %s\n", server_read_size, outbuffer);
    }
    if (isCongestionControl(report->common) && report->common->Congestion) {
	fprintf(stdout, "TCP congestion control set to %s\n", report->common->Congestion);
    }
    output_window_size(report);
    printf("\n");
}
static void reporter_output_client_settings (struct ReportSettings *report) {
    if (!report->common->Ifrnametx) {
	printf(isEnhanced(report->common) ? client_pid_port : client_port, report->common->Host,
	       (isUDP(report->common) ? "UDP" : "TCP"), report->common->Port, report->pid, \
	       (!report->common->threads ? 1 : report->common->threads));
    } else {
	printf(client_pid_port_dev, report->common->Host,
	       (isUDP(report->common) ? "UDP" : "TCP"), report->common->Port, report->pid, \
	       report->common->Ifrnametx, (!report->common->threads ? 1 : report->common->threads));
    }
    if (isEnhanced(report->common)) {
	byte_snprintf(outbuffer, sizeof(outbuffer), report->common->BufLen, toupper((int)report->common->Format));
	outbuffer[(sizeof(outbuffer)-1)] = '\0';
	printf("%s: %s\n", client_write_size, outbuffer);
    }
    if (isIsochronous(report->common)) {
	char meanbuf[40];
	char variancebuf[40];
	byte_snprintf(meanbuf, sizeof(meanbuf), report->isochstats.mMean, 'a');
	byte_snprintf(variancebuf, sizeof(variancebuf), report->isochstats.mVariance, 'a');
	meanbuf[39]='\0'; variancebuf[39]='\0';
	printf(client_isochronous, report->isochstats.mFPS, meanbuf, variancebuf, (report->isochstats.mBurstInterval/1000.0), (report->isochstats.mBurstIPG/1000.0));
#if 0
	if ((report->isochstats.mMean / report->isochstats.mFPS) < ((double) (sizeof(struct UDP_reportgram) + sizeof(struct client_hdr_v1) + sizeof(struct client_hdr_udp_isoch_tests)))) {
	    fprintf(stderr, "Warning: Requested mean too small to carry isoch payload, code will auto adjust payload sizes\n");
	}
#endif
    }
    if (isFQPacing(report->common)) {
	byte_snprintf(outbuffer, sizeof(outbuffer), report->common->FQPacingRate, 'a');
	outbuffer[(sizeof(outbuffer)-1)] = '\0';
        printf(client_fq_pacing,outbuffer);
    }
    if (isCongestionControl(report->common) && report->common->Congestion) {
	fprintf(stdout, "TCP congestion control set to %s\n", report->common->Congestion);
    }
    output_window_size(report);
    printf("\n");
}
#if 0
    assert(reporthdr != NULL);
    if (data->common->ThreadMode == kMode_Listener) {
    } else  {
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
	    byte_snprintf(buffer, sizeof(buffer), data->common->BufLen, toupper((int)data->info.mFormat));
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
    byte_snprintf(buffer, sizeof(buffer), data->connection.winsize, toupper((int)data->info.mFormat));
	    buffer[(sizeof(buffer)-1)] = '\0';
	    printf("%s: %s", (isUDP(data) ? udp_buffer_size : tcp_window_size), buffer);
    if (data->connection.winsize_requested == 0) {
        printf(" %s", window_default);
    } else if (data->connection.winsize != data->connection.winsize_requested) {
        byte_snprintf(buffer, sizeof(buffer), data->connection.winsize_requested,
                       toupper((int)data->info.mFormat));
	    buffer[(sizeof(buffer)-1)] = '\0';
	printf(warn_window_requested, buffer);
    }
}
#endif

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

void reporter_print_connection_report(struct ConnectionInfo *report) {
    assert(report->common);
    // copy the inet_ntop into temp buffers, to avoid overwriting
    char local_addr[REPORT_ADDRLEN];
    char remote_addr[REPORT_ADDRLEN];
    struct sockaddr *local = ((struct sockaddr*)&report->local);
    struct sockaddr *peer = ((struct sockaddr*)&report->peer);
    outbuffer[0]='\0';
    outbufferext[0]='\0';
    outbufferext2[0]='\0';
    char *b = &outbuffer[0];
    if ((report->common->winsize_requested > 0) && (report->winsize != report->common->winsize_requested)) {
	byte_snprintf(outbufferext, sizeof(outbufferext), report->winsize, 'A');
	byte_snprintf(outbufferext2, sizeof(outbufferext2), report->common->winsize_requested, 'A');
	int len = snprintf(NULL, 0, " (WARN: winsize=%s req=%s)", outbufferext, outbufferext2);
	snprintf(b, len, " (WARN: winsize=%s req=%s)", outbufferext, outbufferext2);
    }
    b += strlen(b);
    if (isIsochronous(report->common)) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (isoch)");
	b += strlen(b);
    }
    if (isBidir(report->common)) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (bidir)");
	b += strlen(b);
    } else if (isServerReverse(report->common) || isReverse(report->common)) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (reverse)");
	b += strlen(b);
	if (isFQPacing(report->common)) {
	    snprintf(b, SNBUFFERSIZE-strlen(b), " (fq)");
	    b += strlen(b);
	}
    }
    if (isTripTime(report->common)) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (trip-times)");
	b += strlen(b);
    }
    if (isL2LengthCheck(report->common)) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (l2mode)");
	b += strlen(b);
    }
    if (isUDP(report->common) && isNoUDPfin(report->common)) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (no-udp-fin)");
	b += strlen(b);
    }
    if (!isUDP(report->common) && (report->common->socket > 0) && (isPrintMSS(report->common) || isEnhanced(report->common)))  {
	int inMSS = getsock_tcp_mss(report->common->socket);
	if (isPrintMSS(report->common) && (inMSS <= 0)) {
	    printf(report_mss_unsupported, report->common->socket);
	} else {
	    snprintf(b, SNBUFFERSIZE-strlen(b), " (%s%d)", "MSS=", inMSS);
	    b += strlen(b);
	}
    }
    if (report->peerversion) {
	snprintf(b, SNBUFFERSIZE-strlen(b), "%s", report->peerversion);
	b += strlen(b);
    }
    if (report->connecttime > 0) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (ct=%4.2f ms)", report->connecttime);;
	b += strlen(b);
    }
    if (report->txholdbacktime > 0) {
	snprintf(b, SNBUFFERSIZE-strlen(b), " (ht=%4.2f s)", report->txholdbacktime);;
    }
    if (local->sa_family == AF_INET) {
	inet_ntop(AF_INET, &((struct sockaddr_in*)local)->sin_addr, local_addr, REPORT_ADDRLEN);
    }
#ifdef HAVE_IPV6
    else {
	inet_ntop(AF_INET6, &((struct sockaddr_in6*)local)->sin6_addr, local_addr, REPORT_ADDRLEN);
    }
#endif
    if (peer->sa_family == AF_INET) {
	inet_ntop(AF_INET, &((struct sockaddr_in*)peer)->sin_addr, remote_addr, REPORT_ADDRLEN);
    }
#ifdef HAVE_IPV6
    else {
	inet_ntop(AF_INET6, &((struct sockaddr_in6*)peer)->sin6_addr, remote_addr, REPORT_ADDRLEN);
    }
#endif
#ifdef HAVE_IPV6
    if (isEnhanced(report->common) && report->common->Ifrname && (strlen(report->common->Ifrname) < SNBUFFERSIZE-strlen(b))) {
	printf(report_peer_dev, report->common->socket, local_addr, report->common->Ifrname, \
	       (local->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)local)->sin_port) : \
		ntohs(((struct sockaddr_in6*)local)->sin6_port)), \
	       remote_addr, (peer->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)peer)->sin_port) : \
			     ntohs(((struct sockaddr_in6*)peer)->sin6_port)), outbuffer);
    } else {
	printf(report_peer, report->common->socket, local_addr, \
	       (local->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)local)->sin_port) : \
		ntohs(((struct sockaddr_in6*)local)->sin6_port)), \
	       remote_addr, (peer->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)peer)->sin_port) : \
			     ntohs(((struct sockaddr_in6*)peer)->sin6_port)), outbuffer);
    }
#else
    if (isEnhanced(report->common) && report->common->Ifrname  && (strlen(report->common->Ifrname) < SNBUFFERSIZE-strlen(b))) {
	printf(report_peer_dev, report->common->socket, local_addr, report->common->Ifrname, \
	       local_addr, (local->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)local)->sin_port) : 0), \
	       remote_addr, (peer->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)peer)->sin_port) :  0), \
	       outbuffer);
    } else {
	printf(report_peer, report->common->socket, \
	       local_addr, (local->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)local)->sin_port) : 0), \
	       remote_addr, (peer->sa_family == AF_INET ? ntohs(((struct sockaddr_in*)peer)->sin_port) :  0), \
	       outbuffer);
    }
#endif
    if ((report->epochStartTime.tv_sec) && (report->common->ThreadMode == kMode_Client) && !isServerReverse(report->common)) {
	struct tm ts;
	char start_timebuf[80];
#ifdef HAVE_CLOCK_GETTIME
	// Format time, "ddd yyyy-mm-dd hh:mm:ss zzz"
	ts = *localtime(&report->epochStartTime.tv_sec);
	strftime(start_timebuf, sizeof(start_timebuf), "%Y-%m-%d %H:%M:%S", &ts);
	struct timespec t1;
	clock_gettime(CLOCK_REALTIME, &t1);
	ts = *localtime(&t1.tv_sec);
	char now_timebuf[80];
	strftime(now_timebuf, sizeof(now_timebuf), "%Y-%m-%d %H:%M:%S (%Z)", &ts);
	printf(client_report_epoch_start_current, report->common->socket, start_timebuf, report->epochStartTime.tv_sec, report->epochStartTime.tv_usec, now_timebuf);
#else
	// Format time, "ddd yyyy-mm-dd hh:mm:ss zzz"
	ts = *localtime(&report->epochStartTime.tv_sec);
	strftime(start_timebuf, sizeof(start_timebuf), "%Y-%m-%d %H:%M:%S (%Z)", &ts);
	printf(client_report_epoch_start, report->common->socket, start_timebuf, report->epochStartTime.tv_sec, report->epochStartTime.tv_usec);
#endif
	fflush(stdout);
    }
}
// end ReportPeer


void reporter_print_settings_report(struct ReportSettings *report) {
    assert(report != NULL);
    report->pid =  (int)  getpid();
    printf("%s", separator_line);
    if (report->common->ThreadMode == kMode_Listener) {
	reporter_output_listener_settings(report);
    } else {
	reporter_output_client_settings(report);
    }
    printf("%s", separator_line);
}

void reporter_peerversion (struct ConnectionInfo *report, uint32_t upper, uint32_t lower) {
    if (!upper || !lower) {
	report->peerversion[0]='\0';
    } else {
	int rel, major, minor, alpha;
	rel = (upper & 0xFFFF0000) >> 16;
	major = (upper & 0x0000FFFF);
	minor = (lower & 0xFFFF0000) >> 16;
	alpha = (lower & 0x0000000F);
	snprintf(report->peerversion, (PEERVERBUFSIZE-10), " (peer %d.%d.%d)", rel, major, minor);
	switch(alpha) {
	case 0:
	    sprintf(report->peerversion + strlen(report->peerversion) - 1,"-alpha)");
	    break;
	case 1:
	    sprintf(report->peerversion + strlen(report->peerversion) - 1,"-beta)");
	    break;
	case 2:
	    sprintf(report->peerversion + strlen(report->peerversion) - 1,"-rc)");
	    break;
	case 3:
	    break;
	default:
	    sprintf(report->peerversion + strlen(report->peerversion) - 1, "-unk)");
	}
	report->peerversion[PEERVERBUFSIZE-1]='\0';
    }
}

void reporter_print_server_relay_report (struct ServerRelay *report) {
    printf(server_reporting, report->info.transferID);
    udp_output_read(&report->info);
}
