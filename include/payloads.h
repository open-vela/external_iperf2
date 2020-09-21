/*---------------------------------------------------------------
 * Copyrighta (c) 2019
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
 * payloads.h
 * Iperf 2 packet or socket payloads/protocols
 *
 * by Robert J. McMahon (rjmcmahon@rjmcmahon.com, bob.mcmahon@broadcom.com)
 * -------------------------------------------------------------------
 */
#ifndef PAYLOADSC_H
#define PAYLOADSC_H

#ifdef __cplusplus
extern "C" {
#endif
/*
 * Message header flags
 *
 * base flags, keep compatible with older versions
 */
#define HEADER_VERSION1     0x80000000
#define HEADER_EXTEND_ACK   0x40000000
#define HEADER_UDPTESTS     0x20000000
#define HEADER_SEQNO64B     0x08000000
#define HEADER_VERSION2     0x04000000
#define HEADER_LEN_BIT      0x00010000
#define SERVER_HEADER_EXTEND 0x40000000
#define RUN_NOW             0x00000001

// newer flags available per HEADER_EXTEND
// Below flags are used to pass test settings in *every* UDP packet
// and not just during the header exchange
#define HEADER_ISOCH          0x0001
#define HEADER_L2ETHPIPV6     0x0002
#define HEADER_L2LENCHECK     0x0004
#define HEADER_NOUDPFIN       0x0008
#define HEADER_TRIPTIME       0x0010
#define HEADER_TOSSTARTTIME   0x0020
#define HEADER_ISOCH_SETTINGS 0x0040
#define HEADER_UNITS_PPS      0x0080
#define HEADER_BWSET          0x0100
#define HEADER_FQRATESET      0x0200
#define HEADER_REVERSE        0x0400
#define HEADER_BIDIR          0x0800

// later features
#define HDRXACKMAX 2500000 // default 2.5 seconds, units microseconds
#define HDRXACKMIN   10000 // default 10 ms, units microseconds

/*
 * Structures used for test messages which
 * are exchanged between the client and the Server/Listener
 */
enum MsgType {
    CLIENTHDR = 0x1,
    CLIENTHDRACK,
    CLIENTTCPHDR,
    SERVERHDR,
    SERVERHDRACK
};

/*
 * Structures below will be passed as network i/o
 * between the client, listener and server
 * and must be packed by the compilers
 * Align on 32 bits (4 bytes)
 */
#pragma pack(push,4)
struct UDP_datagram {
// used to reference the 4 byte ID number we place in UDP datagrams
// Support 64 bit seqno on machines that support them
    uint32_t id;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t id2;
};

struct hdr_typelen {
    int32_t type;
    int32_t length;
};

struct TCP_datagram {
// used to reference write ids and timestamps in TCP payloads
    struct hdr_typelen typelen;
    uint32_t id;
    uint32_t id2;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t reserved1;
    uint32_t reserved2;
};

/*
 * The client_hdr structure is sent from clients
 * to servers to alert them of things that need
 * to happen. Order must be perserved in all
 * future releases for backward compatibility.
 * 1.7 has flags, numThreads, mPort, and bufferlen
 */
struct client_hdr_v1 {
    /*
     * flags is a bitmap for different options
     * the most significant bits are for determining
     * which information is available. So 1.7 uses
     * 0x80000000 and the next time information is added
     * the 1.7 bit will be set and 0x40000000 will be
     * set signifying additional information. If no
     * information bits are set then the header is ignored.
     * The lowest order diferentiates between dualtest and
     * tradeoff modes, wheither the speaker needs to start
     * immediately or after the audience finishes.
     */
    int32_t flags;
    int32_t numThreads;
    int32_t mPort;
    int32_t mBufLen;
    int32_t mWinBand;
    int32_t mAmount;
};

struct client_hdrext_isoch_settings {
    int32_t FPSl;
    int32_t FPSu;
    int32_t Meanl;
    int32_t Meanu;
    int32_t Variancel;
    int32_t Varianceu;
    int32_t BurstIPGl;
    int32_t BurstIPGu;
};

struct client_hdrext {
    int16_t upperflags;
    int16_t lowerflags;
    uint32_t version_u;
    uint32_t version_l;
    uint16_t reserved;
    uint16_t tos;
    int32_t Rate;
    int32_t UDPRateUnits;
    int32_t Realtime;
};

struct client_hdrext_starttime_fq {
    uint32_t reserved;
    uint32_t fqrate;
    uint32_t start_tv_sec;
    uint32_t start_tv_usec;
};

/*
 * TCP Isoch/burst payload structure
 *
 *                 0      7 8     15 16    23 24    31
 *                +--------+--------+--------+--------+
 *            1   |        type                       |
 *                +--------+--------+--------+--------+
 *            2   |        len                        |
 *                +--------+--------+--------+--------+
 *            3   |        flags                      |
 *                +--------+--------+--------+--------+
 *            4   |        isoch burst period (s)     |
 *                +--------+--------+--------+--------+
 *            5   |        isoch burst period (us)    |
 *                +--------+--------+--------+--------+
 *            6   |        isoch start timestamp (s)  |
 *                +--------+--------+--------+--------+
 *            7   |        isoch start timestamp (us) |
 *                +--------+--------+--------+--------+
 *            8   |        burst id                   |
 *                +--------+--------+--------+--------+
 *            9   |        burtsize                   |
 *                +--------+--------+--------+--------+
 *           10   |        burst bytes remaining      |
 *                +--------+--------+--------+--------+
 *           11   |        seqno lower                |
 *                +--------+--------+--------+--------+
 *           12   |        seqno upper                |
 *                +--------+--------+--------+--------+
 *           13   |        tv_sec (write)             |
 *                +--------+--------+--------+--------+
 *           14   |        tv_usec (write)            |
 *                +--------+--------+--------+--------+
 *           15   |        tv_sec (read)              |
 *                +--------+--------+--------+--------+
 *           16   |        tv_usec (read)             |
 *                +--------+--------+--------+--------+
 *           17   |        tv_sec (write-ack)         |
 *                +--------+--------+--------+--------+
 *           18   |        tv_usec (write-ack)        |
 *                +--------+--------+--------+--------+
 *           19   |        tv_sec (read-ack)          |
 *                +--------+--------+--------+--------+
 *           20   |        tv_usec (read-ack)         |
 *                +--------+--------+--------+--------+
 *           21   |        reserved                   |
 *                +--------+--------+--------+--------+
 *           22   |        reserved                   |
 *                +--------+--------+--------+--------+
 *           23   |        reserved                   |
 *                +--------+--------+--------+--------+
 *           24   |        reserved                   |
 *                +--------+--------+--------+--------+
 *
 */
struct TCP_oneway_triptime {
    uint32_t write_tv_sec;
    uint32_t write_tv_usec;
    uint32_t read_tv_sec;
    uint32_t read_tv_usec;
};

struct TCP_burst_payload {
    uint32_t flags;
    struct hdr_typelen typelen;
    uint32_t start_tv_sec;
    uint32_t start_tv_usec;
    struct TCP_oneway_triptime send_tt;
    uint32_t burst_period_s;
    uint32_t burst_period_us;
    uint32_t burst_id;
    uint32_t burst_size;
    uint32_t seqno_lower;
    uint32_t seqno_upper;
    struct TCP_oneway_triptime writeacktt;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
    uint32_t reserved4;
};

/*
 * UDP Full Isoch payload structure
 *
 *                 0      7 8     15 16    23 24    31
 *                +--------+--------+--------+--------+
 *      0x00  1   |          seqno lower              |
 *                +--------+--------+--------+--------+
 *      0x04  2   |             tv_sec                |
 *                +--------+--------+--------+--------+
 *      0x08  3   |             tv_usec               |
 *                +--------+--------+--------+--------+
 *            4   |          seqno upper              |
 *                +--------+--------+--------+--------+
 *            5   |         flags (v1)                |
 *                +--------+--------+--------+--------+
 *            6   |         numThreads (v1)           |
 *                +--------+--------+--------+--------+
 *            7   |         mPort (v1)                |
 *                +--------+--------+--------+--------+
 *            8   |         bufferLen (v1)            |
 *                +--------+--------+--------+--------+
 *            9   |         mWinBand (v1)             |
 *                +--------+--------+--------+--------+
 *            10  |         mAmount (v1)              |
 *                +--------+--------+--------+--------+
 *            11  |   up flags      |   low flags     |
 *                +--------+--------+--------+--------+
 *            12  |        iperf version major        |
 *                +--------+--------+--------+--------+
 *            13  |        iperf version minor        |
 *                +--------+--------+--------+--------+
 *            14  |        reserved          |  TOS   |
 *                +--------+--------+--------+--------+
 *            15  |        rate                       |
 *                +--------+--------+--------+--------+
 *            16  |        rate units                 |
 *                +--------+--------+--------+--------+
 *            17  |        realtime   (0.13)          |
 *                +--------+--------+--------+--------+
 *            18  |        isoch burst period (us)    |
 *                +--------+--------+--------+--------+
 *            19  |        isoch start timestamp (s)  |
 *                +--------+--------+--------+--------+
 *            20  |        isoch start timestamp (us) |
 *                +--------+--------+--------+--------+
 *            21  |        isoch prev frameid         |
 *                +--------+--------+--------+--------+
 *            22  |        isoch frameid              |
 *                +--------+--------+--------+--------+
 *            23  |        isoch burtsize             |
 *                +--------+--------+--------+--------+
 *            24  |        isoch bytes remaining      |
 *                +--------+--------+--------+--------+
 *            25  |        isoch reserved             |
 *                +--------+--------+--------+--------+
 *            26  |        reserved (0.14 start)      |
 *                +--------+--------+--------+--------+
 *            27  |        fqrate                     |
 *                +--------+--------+--------+--------+
 *            28  |        start tv_sec  (0.14)       |
 *                +--------+--------+--------+--------+
 *            29  |        start tv_usec              |
 *                +--------+--------+--------+--------+
 *            30  |        FPSl                       |
 *                +--------+--------+--------+--------+
 *            31  |        FPSu                       |
 *                +--------+--------+--------+--------+
 *            32  |        Meanl                      |
 *                +--------+--------+--------+--------+
 *            33  |        Meanu                      |
 *                +--------+--------+--------+--------+
 *            34  |        Variancel                  |
 *                +--------+--------+--------+--------+
 *            35  |        Varianceu                  |
 *                +--------+--------+--------+--------+
 *            36  |        BurstIPGl                  |
 *                +--------+--------+--------+--------+
 *            37  |        BurstIPG                   |
 *                +--------+--------+--------+--------+
 *
 */
struct isoch_payload {
    uint32_t burstperiod; //period units microseconds
    uint32_t start_tv_sec;
    uint32_t start_tv_usec;
    uint32_t prevframeid;
    uint32_t frameid;
    uint32_t burstsize;
    uint32_t remaining;
    uint32_t reserved;
};

struct client_udp_testhdr {
    struct UDP_datagram seqno_ts;
    struct client_hdr_v1 base;
    struct client_hdrext extend;
    struct isoch_payload isoch;
    struct client_hdrext_starttime_fq start_fq;
    struct client_hdrext_isoch_settings isoch_settings;
};

struct client_hdr_ack {
    struct hdr_typelen typelen;
    uint32_t flags;
    uint32_t version_u;
    uint32_t version_l;
    uint32_t reserved1;
    uint32_t reserved2;
};

/*
 * TCP first payload structure
 *
 *                 0      7 8     15 16    23 24    31
 *                +--------+--------+--------+--------+
 *      0x00  1   |         flags (v1)                |
 *                +--------+--------+--------+--------+
 *            2   |         numThreads (v1)           |
 *                +--------+--------+--------+--------+
 *            3   |         mPort (v1)                |
 *                +--------+--------+--------+--------+
 *            4   |         bufferLen (v1)            |
 *                +--------+--------+--------+--------+
 *            5   |         mWinBand (v1)             |
 *                +--------+--------+--------+--------+
 *            6   |         mAmount (v1)              |
 *                +--------+--------+--------+--------+
 *            7   |        type (0.13)                |
 *                +--------+--------+--------+--------+
 *            8   |        len  (0.13)                |
 *                +--------+--------+--------+--------+
 *            9   |        flags (0.13)               |
 *                +--------+--------+--------+--------+
 *            10  |        iperf version major        |
 *                +--------+--------+--------+--------+
 *            11  |        iperf version minor        |
 *                +--------+--------+--------+--------+
 *            12  |        reserved          | TOS    |
 *                +--------+--------+--------+--------+
 *            13  |        rate                       |
 *                +--------+--------+--------+--------+
 *            14  |        rate units                 |
 *                +--------+--------+--------+--------+
 *            15  |        realtime   (0.13)          |
 *                +--------+--------+--------+--------+
 *            16  |        reserved (0.14 start)      |
 *                +--------+--------+--------+--------+
 *            17  |        fqrate                     |
 *                +--------+--------+--------+--------+
 *            18  |        start tv_sec (0.14)        |
 *                +--------+--------+--------+--------+
 *            19  |        start tv_usec              |
 *                +--------+--------+--------+--------+
 *            20  |        FPSl                       |
 *                +--------+--------+--------+--------+
 *            21  |        FPSu                       |
 *                +--------+--------+--------+--------+
 *            22  |        Meanl                      |
 *                +--------+--------+--------+--------+
 *            23  |        Meanu                      |
 *                +--------+--------+--------+--------+
 *            24  |        Variancel                  |
 *                +--------+--------+--------+--------+
 *            25  |        Varianceu                  |
 *                +--------+--------+--------+--------+
 *            26  |        BurstIPGl                  |
 *                +--------+--------+--------+--------+
 *            27  |        BurstIPG                   |
 *                +--------+--------+--------+--------+
 */
struct client_tcp_testhdr {
    struct client_hdr_v1 base;
    struct client_hdrext extend;
    struct client_hdrext_starttime_fq start_fq;
    struct client_hdrext_isoch_settings isoch_settings;
};

/*
 * The server_hdr structure facilitates the server
 * report of jitter and loss on the client side.
 * It piggy_backs on the existing clear to close
 * packet.
 */
struct server_hdr_v1 {
    /*
     * flags is a bitmap for different options
     * the most significant bits are for determining
     * which information is available. So 1.7 uses
     * 0x80000000 and the next time information is added
     * the 1.7 bit will be set and 0x40000000 will be
     * set signifying additional information. If no
     * information bits are set then the header is ignored.
     */
    int32_t flags;
    int32_t total_len1;
    int32_t total_len2;
    int32_t stop_sec;
    int32_t stop_usec;
    int32_t error_cnt;
    int32_t outorder_cnt;
    int32_t datagrams;
    int32_t jitter1;
    int32_t jitter2;
};

struct server_hdr_extension {
    int32_t minTransit1;
    int32_t minTransit2;
    int32_t maxTransit1;
    int32_t maxTransit2;
    int32_t sumTransit1;
    int32_t sumTransit2;
    int32_t meanTransit1;
    int32_t meanTransit2;
    int32_t m2Transit1;
    int32_t m2Transit2;
    int32_t vdTransit1;
    int32_t vdTransit2;
    int32_t cntTransit;
    int32_t cntIPG;
    int32_t IPGsum;
};

  // Extension for 64bit datagram counts
struct server_hdr_extension2 {
    int32_t error_cnt2;
    int32_t outorder_cnt2;
    int32_t datagrams2;
};

struct server_hdr {
    struct server_hdr_v1 base;
    struct server_hdr_extension extend;
    struct server_hdr_extension2 extend2;
};

#pragma pack(pop)

#define SIZEOF_UDPHDRMSG_V1 (sizeof(struct client_hdrv1) + sizeof(struct UDP_datagram))
#define SIZEOF_UDPHDRMSG_EXT (sizeof(struct client_udp_testhdr))
#define SIZEOF_TCPHDRMSG_V1 (sizeof(struct client_hdr_v1))
#define SIZEOF_TCPHDRMSG_EXT (sizeof(struct client_tcp_testhdr))
#define MBUFALLOCSIZE (((int) sizeof(struct client_udp_testhdr) > mSettings->mBufLen) ? (int) sizeof(struct client_udp_testhdr) : mSettings->mBufLen)
#ifdef __cplusplus
} /* end extern "C" */
#endif

#endif // PAYLOADS
