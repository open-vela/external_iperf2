/*---------------------------------------------------------------
 * Copyright (c) 2021
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
 * gettcpinfo.c
 * Suppport for tcp info in a portable way
 *
 * by Robert J. McMahon (rjmcmahon@rjmcmahon.com, bob.mcmahon@broadcom.com)
 * -------------------------------------------------------------------
 */
#include "headers.h"
#include "packetring.h"
#ifdef HAVE_THREAD_DEBUG
// needed for thread_debug
#include "Thread.h"
#endif

#if (HAVE_STRUCT_TCP_INFO_TCPI_TOTAL_RETRANS) && (HAVE_DECL_TCP_INFO)
inline void gettcpinfo (int sock, struct ReportStruct *sample) {
    assert(sample);
    struct tcp_info tcp_info_buf;
    socklen_t tcp_info_length = sizeof(struct tcp_info);
    sample->tcpstats.isValid  = false;
    if ((sock > 0) &&							\
	!(getsockopt(sock, IPPROTO_TCP, TCP_INFO, &tcp_info_buf, &tcp_info_length) < 0)) {
        sample->tcpstats.cwnd = tcp_info_buf.tcpi_snd_cwnd * tcp_info_buf.tcpi_snd_mss / 1024;
	sample->tcpstats.rtt = tcp_info_buf.tcpi_rtt;
	sample->tcpstats.retry_tot = tcp_info_buf.tcpi_total_retrans;
	sample->tcpstats.isValid  = true;
    } else {
        sample->tcpstats.cwnd = -1;
	sample->tcpstats.rtt = 0;
	sample->tcpstats.retry_tot = 0;
    }
}
#elif HAVE_DECL_TCP_CONNECTION_INFO
inline void gettcpinfo (int sock, struct ReportStruct *sample) {
    assert(sample);
    struct tcp_connection_info tcp_info_buf;
    socklen_t tcp_connection_info_length = sizeof(struct tcp_connection_info);

    sample->tcpstats.isValid  = false;
    if ((sock > 0) &&				\
	!(getsockopt(sock, IPPROTO_TCP, TCP_CONNECTION_INFO, &tcp_info_buf, &tcp_connection_info_length) < 0)) {
        sample->tcpstats.cwnd = tcp_info_buf.tcpi_snd_cwnd / 1024;
//	sample->tcpstats.rtt = tcp_info_buf.tcpi_rttcur * 1000; /current rtt units ms
	sample->tcpstats.rtt = tcp_info_buf.tcpi_srtt * 1000; //average rtt units ms
	sample->tcpstats.retry_tot = tcp_info_buf.tcpi_txretransmitpackets;
	sample->tcpstats.isValid = true;
    } else {
        sample->tcpstats.cwnd = -1;
	sample->tcpstats.rtt = 0;
	sample->tcpstats.retry_tot = 0;
    }
}
#else
inline void gettcpinfo (struct ReporterData *data, struct ReportStruct *sample) {
    sample->tcpstats.rtt = 1;
    sample->tcpstats.isValid  = false;
};
#endif
