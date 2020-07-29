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
 * Reporter.h
 * by Kevin Gibbs <kgibbs@nlanr.net>
 *
 * Since version 2.0 this handles all reporting.
 * ________________________________________________________________ */

#ifndef REPORTER_H
#define REPORTER_H

#include "headers.h"
#include "Mutex.h"
#include "histogram.h"
#include "packet_ring.h"

// forward declarations found in Settings.hpp
struct thread_Settings;
struct server_hdr;
#include "Settings.hpp"

#define NUM_REPORT_STRUCTS 10000

// If the minimum latency exceeds the boundaries below
// assume the clocks are not synched and suppress the
// latency output. Units are seconds
#define UNREALISTIC_LATENCYMINMIN -1
#define UNREALISTIC_LATENCYMINMAX 60

#ifdef __cplusplus
extern "C" {
#endif

extern struct Condition ReportCond;
extern struct Condition ReportsPending;

/*
 *
 * Used for end/end latency measurements
 *
 */
struct TransitStats {
    double maxTransit;
    double minTransit;
    double sumTransit;
    double lastTransit;
    double meanTransit;
    double m2Transit;
    double vdTransit;
    int cntTransit;
    double totmaxTransit;
    double totminTransit;
    double totsumTransit;
    int totcntTransit;
    double totmeanTransit;
    double totm2Transit;
    double totvdTransit;
};

struct MeanMinMaxStats {
    double max;
    double min;
    double sum;
    double last;
    double mean;
    double m2;
    double vd;
    int cnt;
    int err;
};

#define TCPREADBINCOUNT 8
struct ReadStats {
    int cntRead;
    int totcntRead;
    int bins[TCPREADBINCOUNT];
    int totbins[TCPREADBINCOUNT];
    int binsize;
};

struct WriteStats {
    int WriteCnt;
    int WriteErr;
    int TCPretry;
    int totWriteCnt;
    int totWriteErr;
    int totTCPretry;
    int lastTCPretry;
    int cwnd;
    int rtt;
    double meanrtt;
    int up_to_date;
};

struct IsochStats {
    int mFPS; //frames per second
    double mMean; //variable bit rate mean
    double mVariance; //vbr variance
    int mJitterBufSize; //Server jitter buffer size, units is frames
    intmax_t slipcnt;
    intmax_t framecnt;
    intmax_t framelostcnt;
    unsigned int mBurstInterval;
    unsigned int mBurstIPG; //IPG of packets within the burst
    int frameID;
};

/*
 * This struct contains all important information from the sending or
 * recieving thread.
 */
#define L2UNKNOWN  0x01
#define L2LENERR   0x02
#define L2CSUMERR  0x04

enum WriteErrType {
    WriteNoErr  = 0,
    WriteErrAccount,
    WriteErrFatal,
    WriteErrNoAccount,
};

enum TimestampType {
    INTERVAL  = 0,
    FINALPARTIAL,
    TOTAL,
};

struct L2Stats {
    intmax_t cnt;
    intmax_t unknown;
    intmax_t udpcsumerr;
    intmax_t lengtherr;
    intmax_t tot_cnt;
    intmax_t tot_unknown;
    intmax_t tot_udpcsumerr;
    intmax_t tot_lengtherr;
};


/*
 * The type field of ReporterData is a bitmask
 * with one or more of the following
 */
#define    TRANSFER_REPORT       0x00000001
#define    SERVER_RELAY_REPORT   0x00000002
#define    SETTINGS_REPORT       0x00000004
#define    CONNECTION_REPORT     0x00000008
#define    MULTIPLE_REPORT       0x00000010
#define    BIDIR_REPORT          0x00000020
#define    TRANSFER_FRAMEREPORTUDP  0x00000040
#define    TRANSFER_FRAMEREPORTTCP  0x00000080

union SendReadStats {
    struct ReadStats read;
    struct WriteStats write;
};

struct TransferInfo {
    void *reserved_delay;
    int transferID;
    int groupID;
    intmax_t cntError;
    intmax_t cntOutofOrder;
    intmax_t cntDatagrams;
    intmax_t IPGcnt;
    intmax_t IPGcnttot;
    intmax_t frameID;
    int socket;
    struct TransitStats transit;
    union SendReadStats sock_callstats;
    // Hopefully int64_t's
    uintmax_t TotalLen;
    double jitter;
    double startTime;
    double endTime;
    double IPGsum;
    double tripTime;
    double arrivalSum;
    double totarrivalSum;
    // chars
    char   mFormat;                 // -f
    char   mEnhanced;               // -e
    u_char mTTL;                    // -T
    char   mUDP;
    char   mTCP;
    int    free;  // A  misnomer - used by summing for a traffic thread counter
    struct histogram *latency_histogram;
    struct L2Stats l2counts;
    struct IsochStats isochstats;
    char   mIsochronous;                 // -e
    struct TransitStats frame;
    struct histogram *framelatency_histogram;
    int flags_extend; // rjm, clean up flags in reports with C++
};

struct ConnectionInfo {
    iperf_sockaddr peer;
    Socklen_t size_peer;
    iperf_sockaddr local;
    Socklen_t size_local;
    char *peerversion;
    int l2mode;
    double connecttime;
    double txholdbacktime;
    struct timeval epochStartTime;
    int winsize;
    int winsize_requested;
    int flags;
    int flags_extend;
    char mFormat;
    unsigned int WriteAckLen;
    enum ThreadMode mThreadMode;         // -s or -c
};

struct ReporterData {
    char*  mHost;                   // -c
    char*  mLocalhost;              // -B
    char*  mIfrname;
    char*  mIfrnametx;
    char*  mSSMMulticastStr;
    // int's
    int type;
    intmax_t cntError;
    intmax_t lastError;
    intmax_t cntOutofOrder;
    intmax_t lastOutofOrder;
    intmax_t cntDatagrams;
    intmax_t lastDatagrams;
    intmax_t PacketID;
    intmax_t matchframeID;
    uintmax_t TotalLen;
    uintmax_t lastTotal;

    int mBufLen;                    // -l
    int mMSS;                       // -M
    int mTCPWin;                    // -w
    intmax_t mUDPRate;            // -b or -u
    enum RateUnits mUDPRateUnits;        // -b is either bw or pps
    /*   flags is a BitMask of old bools
        bool   mBufLenSet;              // -l
        bool   mCompat;                 // -C
        bool   mDaemon;                 // -D
        bool   mDomain;                 // -V
        bool   mFileInput;              // -F or -I
        bool   mNodelay;                // -N
        bool   mPrintMSS;               // -m
        bool   mRemoveService;          // -R
        bool   mStdin;                  // -I
        bool   mStdout;                 // -o
        bool   mSuggestWin;             // -W
        bool   mUDP;
        bool   mMode_time;*/
    int flags;
    int flags_extend;
    // enums (which should be special int's)
    enum ThreadMode mThreadMode;         // -s or -c
    enum ReportMode mode;

    // doubles
    // shorts
    unsigned short mPort;           // -p
    // structs or miscellaneous
    struct TransferInfo info;
    struct ConnectionInfo connection;
    struct timeval startTime;
    struct timeval packetTime;
    struct timeval prevpacketTime;
    struct timeval nextTime;
    struct timeval intervalTime;
    struct timeval IPGstart;
    struct timeval clientStartTime;
    struct IsochStats isochstats;
    double TxSyncInterval;
    unsigned int FQPacingRate;
};


struct MultiHeader {
    int groupID;
    int threads;
    struct ReferenceMutex reference;
    int sockfd;
    struct ReporterData report;
    void (*transfer_protocol_sum_handler) (struct ReporterData *stats, int final);
};

struct ReportHeader {
    struct ReporterData report;
    struct MeanMinMaxStats connect_times;
    // function pointer for per packet processing
    void (*packet_handler) (struct ReportHeader *report, struct ReportStruct *packet);
    void (*transfer_protocol_handler) (struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
    void (*transfer_protocol_sum_handler) (struct ReporterData *stats, int final);
    void (*transfer_protocol_bidir_handler) (struct ReporterData *stats, int final);
    int (*transfer_interval_handler) (struct ReportHeader *reporthdr, struct ReportStruct *packet);
    struct MultiHeader *multireport;
    struct MultiHeader *bidirreport;
    struct ReportHeader *next;
    int reporter_thread_suspends; // used to detect CPU bound systems
    struct PacketRing *packetring;
};

typedef void* (* report_connection)( struct ConnectionInfo*, int );
typedef void (* report_settings)( struct ReporterData* );
typedef void (* report_statistics)( struct TransferInfo* );
typedef void (* report_serverstatistics)( struct ConnectionInfo *, struct TransferInfo* );

struct MultiHeader* InitSumReport( struct thread_Settings *agent, int inID);
void InitIndividualReport( struct thread_Settings *agent );
void InitConnectionReport( struct thread_Settings *agent );
void UpdateConnectionReport(struct thread_Settings *mSettings, struct ReportHeader *reporthdr);
void BarrierClient(struct BarrierMutex *barrier);
void PostReport(struct ReportHeader *agent);
void ReportPacket(struct ReportHeader *agent, struct ReportStruct *packet);
void CloseReport(struct ReportHeader *agent,  struct ReportStruct *packet);
void EndReport(struct ReportHeader *agent);
void FreeReport(struct ReportHeader *agent);
void FreeMultiReport (struct MultiHeader *multihdr);
struct TransferInfo* GetReport(struct ReportHeader *agent);
void ReportServerUDP(struct thread_Settings *agent, struct server_hdr *server);
struct ReportHeader *ReportSettings(struct thread_Settings *agent);
void ReportConnections(struct thread_Settings *agent );
void reporter_peerversion (struct thread_Settings *inSettings, int upper, int lower);
void reporter_dump_job_queue(void);
int IncrMultiHdrRefCounter(struct MultiHeader *multihdr);
int DecrMultiHdrRefCounter(struct MultiHeader *multihdr);

extern struct AwaitMutex reporter_state;
extern struct AwaitMutex threads_start;

extern report_connection connection_reports[];
extern report_settings settings_reports[];
extern report_statistics statistics_reports[];
extern report_serverstatistics serverstatistics_reports[];
extern report_statistics multiple_reports[];

#define SNBUFFERSIZE 120
extern char buffer[SNBUFFERSIZE]; // Buffer for printing


// Packet accounting routines
void reporter_handle_packet_null(struct ReportHeader *report, struct ReportStruct *packet);
void reporter_handle_packet_server_udp(struct ReportHeader *report, struct ReportStruct *packet);
void reporter_handle_packet_server_tcp(struct ReportHeader *report, struct ReportStruct *packet);
void reporter_handle_packet_client(struct ReportHeader *report, struct ReportStruct *packet);
void reporter_handle_packet_pps(struct ReporterData *data, struct TransferInfo *stats, struct ReportStruct *packet);

// Reporter's conditional print, right now only time based sampling, possibly add packet based
int reporter_condprint_time_interval_report(struct ReportHeader *reporthdr, struct ReportStruct *packet);
int reporter_condprint_packet_interval_report(struct ReportHeader *reporthdr, struct ReportStruct *packet);
int reporter_condprint_frame_interval_report_udp(struct ReportHeader *reporthdr, struct ReportStruct *packet);
int reporter_condprint_frame_interval_report_tcp(struct ReportHeader *reporthdr, struct ReportStruct *packet);
//void reporter_set_timestamps_time(struct ReporterData *stats, enum TimestampType);

// Reporter's interval ouput specialize routines
void reporter_transfer_protocol_null(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
//void reporter_transfer_protocol_reports(struct ReporterData *stats, struct ReportStruct *packet);
//void reporter_transfer_protocol_multireports(struct ReporterData *stats, struct ReportStruct *packet);
void reporter_transfer_protocol_client_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
void reporter_transfer_protocol_client_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
void reporter_transfer_protocol_server_tcp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);
void reporter_transfer_protocol_server_udp(struct ReporterData *stats, struct ReporterData *sumstats, struct ReporterData *bidirstats, int final);

// Reporter's sum ouput routines (per -P > 1)
void reporter_transfer_protocol_sum_client_tcp(struct ReporterData *stats, int final);
void reporter_transfer_protocol_sum_server_tcp(struct ReporterData *stats, int final);
void reporter_transfer_protocol_sum_client_udp(struct ReporterData *stats, int final);
void reporter_transfer_protocol_sum_server_udp(struct ReporterData *stats, int final);
void reporter_transfer_protocol_bidir_tcp(struct ReporterData *stats, int final);
void reporter_transfer_protocol_bidir_udp(struct ReporterData *stats, int final);
void reporter_connect_printf_tcp_final(struct ReportHeader *multihdr);


#ifdef __cplusplus
} /* end extern "C" */
#endif

#endif // REPORTER_H
