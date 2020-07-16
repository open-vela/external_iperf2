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

// Create traffic and connect reports and bind them to the settings object
void InitTrafficReports (struct thread_Settings *mSettings) {
    // Note, this must be called in order as
    // the data report structures need to be
    // initialized first
    if (isDataReport(mSettings)) {
	Mutex_Lock(&clients_mutex);
	if ((mSettings->kMode_Client && (mSettings->mThreads > 1)) ||	\
	    (exist = Iperf_hostactive(&mSettings->peer, clients))) {
		InitSumGroupReport(mSettings);
		Iperf_pushback(&mSettings->peer);
	}
	if (isBiDir(mSettings)) {
	    InitBiDirReport(mSettings);
	}
	Mutex_Unlock(&clients_mutex);
    }
    if (isConnectionReport(mSettings)) {
	InitConnectionReport(mSettings);
    }
}


static inline struct MultiHeader *sumgroup_by_peerhost(iperf_sockaddr *peer) {
        Iperf_ListEntry *exist, *listtemp;

	exist = Iperf_hostpresent(&server->peer, clients);

    if ( exist != NULL ) {
	// Copy the multiheader
	listtemp->holder = exist->holder;
	server->multihdr = exist->holder;
	if (tempSettings && isBidir(tempSettings))
	    tempSettings->multihdr = listtemp->holder;
    } else {
	Mutex_Lock(&groupCond);
	groupID--;
	Mutex_Unlock( &groupCond );
	if (!server->multihdr) {
	    listtemp->holder = InitSumReport(server, groupID);
	    server->multihdr = listtemp->holder;
	    if (tempSettings && isBidir(tempSettings))
		tempSettings->multihdr = listtemp->holder;
	}
    }

    // Perform L2 setup if needed
    if (isUDP(mSettings) && (isL2LengthCheck(mSettings) || isL2LengthCheck(server))) {
	if (L2_setup() < 0) {
	    // L2 not allowed, abort this server try
	    mSettings->mSock = -1;
	}
    }
    // Store entry in connection list
    if (mSettings->mSock > 0) {
	Iperf_pushback(listtemp, &clients);
    } else {
	// Undo things done above
	// RJM clean this up later
	if (mSettings->mSock < 0) {
	    if (server && server->multihdr)
		free(server->multihdr);
	    if (server)
		delete server;
	    delete listtemp;
	}
    }
    Mutex_Unlock( &clients_mutex );
}

static void BindSumReport (struct thread_Settings *agent, int inID) {
    Mutex_Lock(&clients_mutex);
    Iperf_ListEntry *exist, *insert;
    if ((exist = Iperf_hostpresent(&agent->peer, clients))) {
	agent->multihdr = exist->holder;
    } else {
	agent->multihdr = InitSumReport(agent, inID);
	insert = new Iperf_ListEntry;
	memcpy(&insert->data, &agent->peer, sizeof(iperf_sockaddr));
	insert->holder = agent->multihdr;
	insert->server = agent;
	insert->next = NULL;
	Iperf_pushback(insert, &clients);
    }
    IncrMultiHdrRefCounter(agent->multihdr);
    Mutex_Unlock(&clients_mutex);
}

static void UnbindSumReport (struct ReportHeader *reporthdr) {
    assert(reporthdr->multireport);
    Mutex_Lock(&clients_mutex);
    struct MultiHeader multihdr = reporthdr->multireport;
    DecrMultiHdrRefCounter(multihdr);
    if (multihdr->reference.count == 0) {
	if (multihdr->transfer_protocol_sum_handler &&	\
	    (multihdr->reference.count == 0) && (multihdr->reference.maxcount > 1)) {
	    (*multihdr->transfer_protocol_sum_handler)(&multihdr->report, 1);
	}
	Iperf_ListEntry **tmp = root;
	while ((*tmp) && !(SockAddr_are_Equal((sockaddr*)&(*tmp)->data, (sockaddr*) del))) {
	    tmp = &(*tmp)->next;
	}
	if (*tmp) {
	    Iperf_ListEntry *remove = (*tmp);
	    *tmp = remove->next;
	    delete remove;
	}
	FreeMultiReport(multihdr);
    }
    reporthdr->multihdr = NULL;
    Mutex_Unlock(&clients_mutex);
}

struct MultiHeader* InitSumReport(struct thread_Settings *agent, int inID) {
    struct MultiHeader *multihdr = (struct MultiHeader *) calloc(1, sizeof(struct MultiHeader));
    if (multihdr != NULL) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Init multiheader sum report %p id=%d", (void *)multihdr, inID);
#endif
        agent->multihdr = multihdr;
	multihdr->groupID = inID;
	multihdr->reference.count = 0;
	multihdr->reference.maxcount = 0;
	Mutex_Initialize(&multihdr->reference.lock);
	multihdr->threads = 0;
	if (isMultipleReport(agent)) {
	    struct ReporterData *data = &multihdr->report;
	    data->type = TRANSFER_REPORT;
	    // Only initialize the interval time here
	    // The startTime and nextTime for summing reports will be set by
	    // the reporter thread in realtime
	    if ((agent->mInterval) && (agent->mIntervalMode == kInterval_Time)) {
		struct timeval *interval = &data->intervalTime;
		interval->tv_sec = (long) (agent->mInterval / rMillion);
		interval->tv_usec = (long) (agent->mInterval % rMillion);
	    } else {
		setNoMultReport(agent);
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

struct MultiHeader* InitBiDirReport (struct thread_Settings *agent, int inID) {
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
	    if ((agent->mInterval) && (agent->mIntervalMode == kInterval_Time)) {
	      struct timeval *interval = &data->intervalTime;
	      interval->tv_sec = (long) (agent->mInterval / rMillion);
	      interval->tv_usec = (long) (agent->mInterval % rMillion);
	    } else {
	      setNoMultReport(agent);
#ifdef HAVE_THREAD_DEBUG
	      thread_debug("BiDir report supressed on this thread");
#endif
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
void BarrierClient (struct BarrierMutex *barrier) {
#ifdef HAVE_THREAD
    assert(barrier);
    Condition_Lock(barrier->await);
    if (--barrier->count <= 0) {
	// store the barrier release timer
#ifdef HAVE_CLOCK_GETTIME
	struct timespec t1;
	clock_gettime(CLOCK_REALTIME, &t1);
	barrier->release_time.tv_sec  = t1.tv_sec;
	barrier->release_time.tv_usec = t1.tv_nsec / 1000;
#else
	gettimeofday(&barrier->release_time, NULL );
#endif
	// last one wake's up everyone else
	Condition_Broadcast(&barrier->await);
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Barrier BROADCAST on condition %p", (void *)&barrier->await);
#endif
    } else {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Barrier WAIT on condition %p count=%d", (void *)&barrier->await, barrier->count);
#endif
        Condition_Wait(&barrier->await);
    }
    Condition_Unlock(barrier->await);
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Barrier EXIT on condition %p", (void *)&barrier->await);
#endif
#endif // HAVE_THREAD
}

/*
 * InitReport is called by a transfer agent (client or
 * server) to setup the needed structures to communicate
 * traffic and connection information.  Also initialize
 * the report start time and next interval report time
 * Finally, in the case of parallel clients, have them all
 * synchronize on compeleting their connect()
 */

void IncrMultiHdrRefCounter (struct MultiHeader *multihdr) {
    assert(multihdr);
    if (multihdr) {
	Mutex_Lock(&multihdr->reference.lock);
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Sum multiheader %p ref=%d->%d", (void *)multihdr, multihdr->reference.count, (multihdr->reference.count + 1));
#endif
	multihdr->reference.count++;
	if (multihdr->reference.count > multihdr->reference.maxcount)
	    multihdr->reference.maxcount = multihdr->reference.count;
	Mutex_Unlock(&multihdr->reference.lock);
    }
}

int DecrMultiHdrRefCounter (struct MultiHeader *multihdr) {
    assert(multihdr);
    if (multihdr) {
	Mutex_Lock(&multihdr->reference.lock);
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Sum multiheader %p ref=%d->%d", (void *)multihdr, multihdr->reference.count, (multihdr->reference.count - 1));
#endif
	multihdr->reference.count--;
	Mutex_Unlock(&multihdr->reference.lock);
    }
}


void FreeReport (struct ReportHeader *reporthdr) {
    if (reporthdr) {
	if (reporthdr->packetring && reporthdr->report.TotalLen && \
	    !TimeZero(reporthdr->report.intervalTime) && (reporthdr->reporter_thread_suspends < 3)) {
	    fprintf(stdout, "WARN: this test was likley CPU bound (%d) (or may not be detecting the underlying network devices)\n", \
		    reporthdr->reporter_thread_suspends);
	}
	if (reporthdr->packetring) {
	    packetring_free(reporthdr->packetring);
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

void FreeMultiReport (struct MultiHeader *multihdr) {
    assert(multihdr);
    if (multihdr) {
#ifdef HAVE_THREAD_DEBUG
        thread_debug("Free multi report hdr=%p", (void *)multihdr);
#endif
	Condition_Destroy(&multihdr->reference.lock);
	Mutex_Destroy(&multihdr->reference.lock);
	free(multihdr);
    }
}

void InitDataReports (struct thread_Settings *mSettings) {
    /*
     * Create in one big chunk
     */
    struct ReportHeader *reporthdr = (struct ReportHeader *) calloc(1, sizeof(struct ReportHeader));
    struct ReporterData *data = NULL;

    if (reporthdr != NULL) {
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Job report %p uses multireport %p and bidirreport is %p", (void *)mSettings->reporthdr, (void *)mSettings->multihdr, (void *)mSettings->bidirhdr);
#endif
	mSettings->reporthdr = reporthdr;
	if(SockAddr_isZeroAddress(&mSettings->peer)) {
	    FAIL(1, "Binding sum report invoked and peer not set!!\n", mSettings);
	}
	BindSumReport(mSettings);
	reporthdr->multireport = mSettings->multihdr;
	reporthdr->bidirreport = mSettings->bidirhdr;
	if (reporthdr->bidirreport) {
	    reporthdr->bidirreport->report.info.transferID = mSettings->mSock;
	}
	data = &reporthdr->report;
	data->mThreadMode = mSettings->mThreadMode;
	reporthdr->packet_handler = NULL;

	if (!isConnectOnly(mSettings)) {
	    // Create a new packet ring which is used to communicate
	    // packet stats from the traffic thread to the reporter
	    // thread.  The reporter thread does all packet accounting
	    reporthdr->packetring = packetring_init((mSettings->numreportstructs ? mSettings->numreportstructs : NUM_REPORT_STRUCTS), \
						    &ReportCond, &mSettings->awake_me);
	    if (mSettings->numreportstructs)
	        fprintf (stdout, "[%3d] NUM_REPORT_STRUCTS override from %d to %d\n", mSettings->mSock, NUM_REPORT_STRUCTS, mSettings->numreportstructs);

	    // Set up the function vectors, there are three
	    // 1) packet_handler: does packet accounting per the test and protocol
	    // 2) transfer_protocol_handler: performs output, e.g. interval reports, per the test and protocol
	    // 3) transfer_protocol_sum_handler: performs summing output when multiple traffic threads

	    switch (data->mThreadMode) {
	    case kMode_Server :
		if (isUDP(mSettings)) {
		    reporthdr->packet_handler = reporter_handle_packet_server_udp;
		    reporthdr->transfer_protocol_handler = reporter_transfer_protocol_server_udp;
		    if (reporthdr->multireport)
			reporthdr->multireport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_udp;
		    if (reporthdr->bidirreport)
			reporthdr->bidirreport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_udp;
		} else {
		    reporthdr->packet_handler = reporter_handle_packet_server_tcp;
		    reporthdr->transfer_protocol_handler = reporter_transfer_protocol_server_tcp;
		    if (reporthdr->multireport)
		        reporthdr->multireport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_server_tcp;
		    if (reporthdr->bidirreport)
		        reporthdr->bidirreport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_tcp;
		}
		break;
	    case kMode_Client :
		reporthdr->packet_handler = reporter_handle_packet_client;
		if (isUDP(mSettings)) {
		    reporthdr->transfer_protocol_handler = reporter_transfer_protocol_client_udp;
		    if (reporthdr->multireport)
			reporthdr->multireport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_udp;
		    if (reporthdr->bidirreport)
			reporthdr->bidirreport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_udp;
		} else {
		    reporthdr->transfer_protocol_handler = reporter_transfer_protocol_client_tcp;
		    if (reporthdr->multireport) {
		        reporthdr->multireport->transfer_protocol_sum_handler = reporter_transfer_protocol_sum_client_tcp;
		    }
		    if (reporthdr->bidirreport)
		        reporthdr->bidirreport->transfer_protocol_sum_handler = reporter_transfer_protocol_bidir_tcp;
		}
		break;
	    case kMode_WriteAckClient :
	        reporthdr->packet_handler = reporter_handle_packet_null;
		reporthdr->transfer_protocol_handler = reporter_transfer_protocol_null;
		break;
	    case kMode_Unknown :
	    case kMode_Reporter :
	    case kMode_ReporterClient :
	    case kMode_Listener:
	    default:
		reporthdr->packet_handler = NULL;
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

	switch (mSettings->mIntervalMode) {
	case kInterval_Time :
	{
	    struct timeval *interval = &data->intervalTime;
	    interval->tv_sec = (long) (mSettings->mInterval / rMillion);
	    interval->tv_usec = (long) (mSettings->mInterval % rMillion);
	    reporthdr->transfer_interval_handler = reporter_condprint_time_interval_report;
	}
	break;
	case kInterval_Packets :
	    reporthdr->transfer_interval_handler = reporter_condprint_packet_interval_report;
	    break;
	case kInterval_Frames :
	    if (isUDP(mSettings)) {
	        reporthdr->transfer_interval_handler = reporter_condprint_frame_interval_report_udp;
	    } else {
	        reporthdr->transfer_interval_handler = reporter_condprint_frame_interval_report_tcp;
	    }
	    break;
	default :
	    reporthdr->transfer_interval_handler = NULL;
	    break;
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

    if (reporthdr == NULL) {
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Malloc connection report %p", reporthdr);
#endif

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
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Init connection report %p", reporthdr);
#endif

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
    if (isEnhanced(mSettings) && isTxStartTime(mSettings)) {
	data->connection.epochStartTime.tv_sec = mSettings->txstart_epoch.tv_sec;
	data->connection.epochStartTime.tv_usec = mSettings->txstart_epoch.tv_usec;
    } else if (isTripTime(mSettings)) {
	data->connection.epochStartTime.tv_sec = mSettings->accept_time.tv_sec;
	data->connection.epochStartTime.tv_usec = mSettings->accept_time.tv_usec;
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
    reporthdr->connect_times.min = FLT_MAX;
    reporthdr->connect_times.max = FLT_MIN;
    reporthdr->connect_times.vd = 0;
    reporthdr->connect_times.m2 = 0;
    reporthdr->connect_times.mean = 0;

    if (mSettings->mSock > 0)
	UpdateConnectionReport(mSettings, reporthdr);
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
