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
 * Listener.cpp
 * by Mark Gates <mgates@nlanr.net>
 * &  Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * rewritten by Robert McMahon
 * -------------------------------------------------------------------
 * Listener sets up a socket listening on the server host. For each
 * connected socket that accept() returns, this creates a Server
 * socket and spawns a thread for it.
 *
 * Changes to the latest version. Listener will run as a daemon
 * Multicast Server is now Multi-threaded
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <stdio.h>
 *   <string.h>
 *   <errno.h>
 *
 *   <sys/types.h>
 *   <unistd.h>
 *
 *   <netdb.h>
 *   <netinet/in.h>
 *   <sys/socket.h>
 * ------------------------------------------------------------------- */
#define HEADERS()

#include "headers.h"
#include "Listener.hpp"
#include "SocketAddr.h"
#include "PerfSocket.hpp"
#include "List.h"
#include "util.h"
#include "version.h"
#include "Locale.h"
#include "SocketAddr.h"
#include "payloads.h"
#include "delay.h"
#if (defined HAVE_SSM_MULTICAST) && (defined HAVE_NET_IF_H)
#include <net/if.h>
#endif
/* -------------------------------------------------------------------
 * Stores local hostname and socket info.
 * ------------------------------------------------------------------- */

Listener::Listener (thread_Settings *inSettings) {
    mClients = inSettings->mThreads;
    ListenSocket = INVALID_SOCKET;
    /*
     * These thread settings are stored in three places
     *
     * 1) Listener thread
     * 2) Reporter Thread (per the ReportSettings())
     * 3) Server thread
     */
    mSettings = inSettings;
    // alloc and initialize the buffer (mBuf) used for test messages in the payload
    mBuf = new char[MBUFALLOCSIZE]; // defined in payloads.h
    FAIL_errno( mBuf == NULL, "No memory for buffer\n", mSettings );
    // Open the listen socket
    my_listen();
} // end Listener

/* -------------------------------------------------------------------
 * Delete memory (buffer).
 * ------------------------------------------------------------------- */
Listener::~Listener () {
#if HAVE_THREAD_DEBUG
    thread_debug("Listener destructor close sock=%d", ListenSocket);
#endif
    if (ListenSocket != INVALID_SOCKET) {
        int rc = close(ListenSocket);
        WARN_errno( rc == SOCKET_ERROR, "listener close" );
    }
    DELETE_ARRAY(mBuf);
} // end ~Listener

/* -------------------------------------------------------------------
 * This is the main Listener thread loop, listens and accepts new
 * connections and starts traffic threads
 *
 * Flow is
 * o) suspend on traffic done for single client case
 * o) hang a select() then accept() on the listener socket
 * o) read or, more accurately, peak the socket for initial messages
 * o) determine and set server's settings flags
 * o) instantiate new settings for listener's clients if needed
 * o) instantiate and bind sum and bidir report objects as needed
 * o) start the threads needed
 *
 * ------------------------------------------------------------------- */
void Listener::Run (void) {
    // mCount is set True if -P was passed to the server
    int mCount = ((mSettings->mThreads != 0) ?  mSettings->mThreads : -1);

    // This is a listener launched by the client per -r or -d
    if (mSettings->clientListener) {
	SockAddr_remoteAddr(mSettings);
    }
    bool mMode_Time = isServerModeTime(mSettings) && !isDaemon(mSettings);
    if (mMode_Time) {
	mEndTime.setnow();
	mEndTime.add(mSettings->mAmount / 100.0);
    }
    Timestamp now;
#define SINGLECLIENTDELAY_DURATION 50000 // units is microseconds
    while (!sInterupted && (isSingleClient(mSettings) || mCount)) {
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Listener main loop port %d ", mSettings->mPort);
#endif
	now.setnow();
	if(mMode_Time && mEndTime.before(now)) {
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Listener port %d (loop timer expired)", mSettings->mPort);
#endif
	    break;
	}
	// Serialize in the event -1 or SingleClient is set
	if (isSingleClient(mSettings)) {
	    // Start with a delay in the event some traffic
	    // threads are pending to be scheduled and haven't
	    // had a chance to update the traffic thread count.
	    // An event system between listener thread and traffic threads
	    // might better but also more complex. This delay
	    // really should be good enough unless the os scheduler sucks
	    delay_loop(SINGLECLIENTDELAY_DURATION);
	    if (thread_numtrafficthreads() > 0) {
#ifdef HAVE_THREAD_DEBUG
		thread_debug("Listener single client loop");
#endif
		continue;
	    }
	}
	// Use a select() with a timeout if -t is set
	if (mMode_Time) {
	    // Hang a select w/timeout on the listener socket
	    struct timeval timeout;
	    timeout.tv_sec = mSettings->mAmount / 100;
	    timeout.tv_usec = (mSettings->mAmount % 100) * 10000;
	    fd_set set;
	    FD_ZERO(&set);
	    FD_SET(ListenSocket, &set);
	    if (select(ListenSocket + 1, &set, NULL, NULL, &timeout) > 0) {
		if (!setsock_blocking(mSettings->mSock, 0)) {
		    WARN(1, "Failed setting socket to non-blocking mode");
		}
	    } else {
#ifdef HAVE_THREAD_DEBUG
		thread_debug("Listener select timeout");
#endif
		continue;
	    }
	} else if (!setsock_blocking(mSettings->mSock, 1)) {
	    WARN(1, "Failed setting socket to blocking mode");
	}
	// Instantiate another settings object to be used by the server thread
	Settings_Copy(mSettings, &server);
	FAIL(!server, "Failed memory allocation for server settings", mSettings);
	server->mThreadMode = kMode_Server;
	if (!isDataReport(mSettings))
	    setNoDataReport(server);

	// accept a new socket and assign it to the server thread
	int accept_sock = my_accept(server);
	if (!(accept_sock > 0)) {
	    assert(server != mSettings);
#ifdef HAVE_THREAD_DEBUG
	    thread_debug("Listener thread accept fail %d", accept_sock);
#endif
	    Settings_Destroy(server);
	    continue;
	}
#ifdef HAVE_THREAD_DEBUG
	thread_debug("Listener thread accepted server sock=%d", server->mSock);
#endif
	// Decrement the -P counter, commonly usd to kill the listener
	// after one test, i.e. -s -P 1
	if (mCount > 0) {
	    mCount--;
	}
	// These are some exception cases where the accepted socket shouldn't have been
	// accepted but the accept() was first required to figure this out
	//
	// 1) When a client started the listener per -d or -r (but not --reverse.)
	//    What's done here is to see if the server peer opening the
	//    socket matches the expected peer per a compare of the ip addresses
	//    For the case of a *client Listener* the server and  host must match
	//    Note: it's a good idea to prefer --reverse and full duplex socket vs this
	//    -d,-r legacy approach. Still support it though in the name of legacy usage
	//
	// 2) The peer is using a V6 address but the listener/server didn't get -V (for v6) on
	//    it's command line
	//
	if ((mSettings->clientListener && SockAddr_Hostare_Equal(&mSettings->peer, &server->peer)) || \
	    (!isIPV6(mSettings) && SockAddr_isIPv6(&server->peer))) {
	    // Not allowed, reset things and restart the loop
	    close(server->mSock);
	    // Don't forget to delete the UDP entry (inserted in my_accept)
	    if (isUDP(server))
		Iperf_remove_host(&server->peer);
	    assert(server != mSettings);
	    Settings_Destroy(server);
	    continue;
	}
	// isCompat is a version 1.7 test, basically it indicates there is nothing
	// in the first messages so don't try to process them. Later iperf versions use
	// the first message to convey test request and test settings information.  This flag
	// is also used for threads that are children so-to-speak, e.g. a -d or -r client,
	// which cannot have test flags otherwise there would be "test setup recursion"
	if (!isCompat(mSettings)) {
	    // Time to read the very first packet received (per UDP) or the test flags (TCP)
	    // to get the client's requested test information.
	    // Note 1: It's important to know that this will also populate mBuf with
	    // enough information for the listener to perform test info exchange later in the code
	    // Note 2: The mBuf read is a peek so the server's traffic thread started later
	    // will also process the first message from an accounting perspective.
	    // This is required for accurate traffic statistics
	    apply_client_settings(server);
	    // server settings flags should now be set per the client's first message exchange
	    // so the server setting's flags per the client can now be checked
	    if (isUDP(server) && (isL2LengthCheck(mSettings) || isL2LengthCheck(server))) {
		if (!L2_setup(server, server->mSock)) {
		    // Requested L2 testing but L2 setup failed
		    close(server->mSock);
		    Iperf_remove_host(&server->peer);
		    assert(server != mSettings);
		    Settings_Destroy(server);
		    continue;
		}
	    }
	    // Read any more test settings and test values (not just the flags) and instantiate
	    // any settings objects for client threads (e.g. bidir or full duplex)
	    // This will set the listener_client_settings to NULL if
	    // there is no need for the Listener to start a client
	    //
	    // Note: the packet payload pointer for this information has different
	    // offsets per TCP or UDP. Basically, TCP starts at byte 0 but UDP
	    // has to skip over the UDP seq no, etc.
	    //
	    thread_Settings *listener_client_settings = NULL;
#if 0
	    Settings_GenerateClientSettings(server, &listener_client_settings, \
					    (UDP ? (struct client_hdr *) (((struct UDP_datagram*)mBuf) + 1) \
					     : (struct client_hdr *) mBuf));
#endif
	    // This is the case when --write-ack was used on the client requesting
	    // application level acknowledgements. Useful for end/end or app/app latency testing
	    if (isWriteAck(server)) {
		thread_Settings *writeackthread;
		Settings_Copy(server, &writeackthread);
		server->ackring = packetring_init(ACKRING_DEFAULTSIZE, &server->awake_me, &writeackthread->awake_me);
		writeackthread->ackring = server->ackring;
		writeackthread->mThreadMode = kMode_WriteAckServer;
#if HAVE_THREAD_DEBUGg
		thread_debug("Write acknowledgements enabled for read bytecount=%d (%p)", server->mWriteAckLen, (void *) writeackthread);
#endif
		thread_start(writeackthread);
	    }
            // --bidir is following iperf3 naming, it's basically a full duplex test using the same socket
	    // this is slightly different than the legacy iperf2's -d and -r.
	    if (listener_client_settings && isBidir(listener_client_settings)) {
		setBidir(server);
		if (isDataReport(server)) {
		    listener_client_settings->bidirhdr = InitSumReport(server, groupID);
		    server->bidirhdr = listener_client_settings->bidirhdr;
#if HAVE_THREAD_DEBUG
		    thread_debug("BiDir report client=%p/%p server=%p/%p", (void *) listener_client_settings, (void *) listener_client_settings->bidirhdr, (void *) server, (void *) server->bidirhdr);
#endif
		}
		listener_client_settings->mThreadMode=kMode_Client;
		thread_start(listener_client_settings);
	    } else if (isServerReverse(server)) {
		// --reverse is used to get through firewalls.  The client initiates the connect
		// but the server and client change roles with respect to traffic, i.e. the server sends
		// and the client receives
		server->mThreadMode=kMode_Client;
	    }
	    // set up starting information for clients
	    if (listener_client_settings  && !isBidir(listener_client_settings)) {
		// client init will also handle -P instantiations if needed
		client_init(listener_client_settings);
		if (listener_client_settings->mMode == kTest_DualTest) {
#ifdef HAVE_THREAD
		    server->runNow =  listener_client_settings;
#else
		    server->runNext = listener_client_settings;
#endif
		} else {
		    server->runNext =  listener_client_settings;
		}
	    }
	}
	// Now start the server side traffic threads
	if (isUDP(mSettings) && isSingleUDP(mSettings)) {
	    UDPSingleServer(server);
	} else {
	    thread_start_all(server);
	    if (isSingleClient(mSettings))
		delay_loop(SINGLECLIENTDELAY_DURATION);
	}
    }
#ifdef HAVE_THREAD_DEBUG
    thread_debug("Listener exiting port/sig/threads %d/%d/%d", mSettings->mPort, sInterupted, mCount);
#endif
} // end Run

/* -------------------------------------------------------------------
 * Setup a socket listening on a port.
 * For TCP, this calls bind() and listen().
 * For UDP, this just calls bind().
 * If inLocalhost is not null, bind to that address rather than the
 * wildcard server address, specifying what incoming interface to
 * accept connections on.
 * ------------------------------------------------------------------- */
void Listener::my_listen (void) {
    int rc;

    SockAddr_localAddr(mSettings);

    // create an AF_INET socket for the accepts
    // for the case of L2 testing and UDP, a new AF_PACKET
    // will be created to supercede this one
    int type = (isUDP( mSettings )  ?  SOCK_DGRAM  :  SOCK_STREAM);
    int domain = (SockAddr_isIPv6(&mSettings->local) ?
#ifdef HAVE_IPV6
		  AF_INET6
#else
		  AF_INET
#endif
		  : AF_INET);

#ifdef WIN32
    if (SockAddr_isMulticast(&mSettings->local)) {
	// Multicast on Win32 requires special handling
	ListenSocket = WSASocket( domain, type, 0, 0, 0, WSA_FLAG_MULTIPOINT_C_LEAF | WSA_FLAG_MULTIPOINT_D_LEAF );
	WARN_errno( ListenSocket == INVALID_SOCKET, "socket" );

    } else
#endif
	{
	    ListenSocket = socket(domain, type, 0 );
	    WARN_errno(ListenSocket == INVALID_SOCKET, "socket");
	}
    mSettings->mSock = ListenSocket;

    SetSocketOptions(mSettings);

    // reuse the address, so we can run if a former server was killed off
    int boolean = 1;
    Socklen_t len = sizeof(boolean);
    rc = setsockopt(ListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*) &boolean, len);
    // bind socket to server address
#ifdef WIN32
    if (SockAddr_isMulticast( &mSettings->local)) {
	// Multicast on Win32 requires special handling
	rc = WSAJoinLeaf( ListenSocket, (sockaddr*) &mSettings->local, mSettings->size_local,0,0,0,0,JL_BOTH);
	WARN_errno( rc == SOCKET_ERROR, "WSAJoinLeaf (aka bind)" );
    } else
#endif
	{
	    rc = bind(ListenSocket, (sockaddr*) &mSettings->local, mSettings->size_local);
	    FAIL_errno(rc == SOCKET_ERROR, "bind", mSettings);
	}

    // update the reporter thread
    if (isReport(mSettings)) {
        struct ReportHeader *report_settings = ReportSettings(mSettings);
	assert(report_settings != NULL);
	// disable future settings reports, listener should only do it once
	unsetReport(mSettings);
	UpdateConnectionReport(mSettings, report_settings);
	PostReport(report_settings);
    }

    // listen for connections (TCP only).
    // use large (INT_MAX) backlog allowing multiple simultaneous connections
    if (!isUDP(mSettings)) {
	rc = listen(ListenSocket, INT_MAX);
	WARN_errno( rc == SOCKET_ERROR, "listen" );
    } else {
#ifndef WIN32
	// if UDP and multicast, join the group
	if (SockAddr_isMulticast(&mSettings->local)) {
#ifdef HAVE_MULTICAST
	    my_multicast_join();
#else
	    fprintf(stderr, "Multicast not supported");
#endif // HAVE_MULTICAST
	}
    }
#endif
} // end my_listen()

/* -------------------------------------------------------------------
 * Joins the multicast group or source and group (SSM S,G)
 *
 * taken from: https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.1.0/com.ibm.zos.v2r1.hale001/ipv6d0141001708.htm
 *
 * Multicast function	                                        IPv4	                   IPv6	                Protocol-independent
 * ==================                                           ====                       ====                 ====================
 * Level of specified option on setsockopt()/getsockopt()	IPPROTO_IP	           IPPROTO_IPV6	IPPROTO_IP or IPPROTO_IPV6
 * Join a multicast group	                                IP_ADD_MEMBERSHIP          IPV6_JOIN_GROUP	MCAST_JOIN_GROUP
 * Leave a multicast group or leave all sources of that
 *   multicast group	                                        IP_DROP_MEMBERSHIP	   IPV6_LEAVE_GROUP	MCAST_LEAVE_GROUP
 * Select outbound interface for sending multicast datagrams	IP_MULTICAST_IF	IPV6_MULTICAST_IF	NA
 * Set maximum hop count	                                IP_MULTICAST_TTL	   IPV6_MULTICAST_HOPS	NA
 * Enable multicast loopback	                                IP_MULTICAST_LOOP	   IPV6_MULTICAST_LOOP	NA
 * Join a source multicast group	                        IP_ADD_SOURCE_MEMBERSHIP   NA	                MCAST_JOIN_SOURCE_GROUP
 * Leave a source multicast group	                        IP_DROP_SOURCE_MEMBERSHIP  NA	                MCAST_LEAVE_SOURCE_GROUP
 * Block data from a source to a multicast group	        IP_BLOCK_SOURCE   	   NA	                MCAST_BLOCK_SOURCE
 * Unblock a previously blocked source for a multicast group	IP_UNBLOCK_SOURCE	   NA	                MCAST_UNBLOCK_SOURCE
 *
 *
 * Reminder:  The os will decide which version of IGMP or MLD to use.   This may be controlled by system settings, e.g.:
 *
 * [rmcmahon@lvnvdb0987:~/Code/ssm/iperf2-code] $ sysctl -a | grep mld | grep force
 * net.ipv6.conf.all.force_mld_version = 0
 * net.ipv6.conf.default.force_mld_version = 0
 * net.ipv6.conf.lo.force_mld_version = 0
 * net.ipv6.conf.eth0.force_mld_version = 0
 *
 * [rmcmahon@lvnvdb0987:~/Code/ssm/iperf2-code] $ sysctl -a | grep igmp | grep force
 * net.ipv4.conf.all.force_igmp_version = 0
 * net.ipv4.conf.default.force_igmp_version = 0
 * net.ipv4.conf.lo.force_igmp_version = 0
 * net.ipv4.conf.eth0.force_igmp_version = 0
 *
 * ------------------------------------------------------------------- */
void Listener::my_multicast_join(void) {
    // This is the older mulitcast join code.  Both SSM and binding the
    // an interface requires the newer socket options.  Using the older
    // code here will maintain compatiblity with previous iperf versions
    if (!isSSMMulticast(mSettings) && !mSettings->mIfrname) {
	if (!SockAddr_isIPv6(&mSettings->local)) {
	    struct ip_mreq mreq;
	    memcpy( &mreq.imr_multiaddr, SockAddr_get_in_addr( &mSettings->local ), \
		    sizeof(mreq.imr_multiaddr));
	    mreq.imr_interface.s_addr = htonl( INADDR_ANY );
	    int rc = setsockopt( ListenSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				 (char*) &mreq, sizeof(mreq));
	    WARN_errno( rc == SOCKET_ERROR, "multicast join" );
	} else {
#ifdef HAVE_IPV6_MULTICAST
	    struct ipv6_mreq mreq;
	    memcpy( &mreq.ipv6mr_multiaddr, SockAddr_get_in6_addr( &mSettings->local ), \
		    sizeof(mreq.ipv6mr_multiaddr));
	    mreq.ipv6mr_interface = 0;
	    int rc = setsockopt( ListenSocket, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, \
				 (char*) &mreq, sizeof(mreq));
	    WARN_errno( rc == SOCKET_ERROR, "multicast v6 join" );
#else
	    fprintf(stderr, "Unfortunately, IPv6 multicast is not supported on this platform\n");
#endif
	}
    } else {
#ifdef HAVE_SSM_MULTICAST
	// Here it's either an SSM S,G multicast join or a *,G with an interface specifier
	// Use the newer socket options when these are specified
	socklen_t socklen = sizeof(struct sockaddr_storage);
	int iface=0;
	int rc;

#ifdef HAVE_NET_IF_H
	/* Set the interface or any */
	if (mSettings->mIfrname) {
	    iface = if_nametoindex(mSettings->mIfrname);
	    FAIL_errno(!iface, "mcast if_nametoindex",mSettings);
	} else {
	    iface = 0;
	}
#endif

        if (isIPV6(mSettings)) {
#ifdef HAVE_IPV6_MULTICAST
	    if (mSettings->mSSMMulticastStr) {
		struct group_source_req group_source_req;
		struct sockaddr_in6 *group;
		struct sockaddr_in6 *source;

		memset(&group_source_req, 0, sizeof(struct group_source_req));

		group_source_req.gsr_interface = iface;
		group=(struct sockaddr_in6*)&group_source_req.gsr_group;
		source=(struct sockaddr_in6*)&group_source_req.gsr_source;
		source->sin6_family = AF_INET6;
		group->sin6_family = AF_INET6;
		/* Set the group */
		rc=getsockname(ListenSocket,(struct sockaddr *)group, &socklen);
		FAIL_errno( rc == SOCKET_ERROR, "mcast join source group getsockname",mSettings );
		group->sin6_port = 0;    /* Ignored */

		/* Set the source, apply the S,G */
		rc=inet_pton(AF_INET6, mSettings->mSSMMulticastStr,&source->sin6_addr);
		FAIL_errno( rc != 1, "mcast v6 join source group pton",mSettings );
		source->sin6_port = 0;    /* Ignored */
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN
		source->sin6_len = group->sin6_len;
#endif
		rc = -1;
#if HAVE_DECL_MCAST_JOIN_SOURCE_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IPV6,MCAST_JOIN_SOURCE_GROUP, &group_source_req,
			    sizeof(group_source_req));
#endif
		FAIL_errno( rc == SOCKET_ERROR, "mcast v6 join source group",mSettings);
	    } else {
		struct group_req group_req;
		struct sockaddr_in6 *group;

		memset(&group_req, 0, sizeof(struct group_req));

		group_req.gr_interface = iface;
		group=(struct sockaddr_in6*)&group_req.gr_group;
		group->sin6_family = AF_INET6;
		/* Set the group */
		rc=getsockname(ListenSocket,(struct sockaddr *)group, &socklen);
		FAIL_errno( rc == SOCKET_ERROR, "mcast v6 join group getsockname",mSettings );
		group->sin6_port = 0;    /* Ignored */
		rc = -1;
#if HAVE_DECL_MCAST_JOIN_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IPV6,MCAST_JOIN_GROUP, &group_req,
				sizeof(group_source_req));
#endif
		FAIL_errno( rc == SOCKET_ERROR, "mcast v6 join group",mSettings);
	    }
#else
	    fprintf(stderr, "Unfortunately, IPv6 multicast is not supported on this platform\n");
#endif
	} else {
	    if (mSettings->mSSMMulticastStr) {
		struct sockaddr_in *group;
		struct sockaddr_in *source;

		// Fill out both structures because we don't which one will succeed
		// and both may need to be tried
#ifdef HAVE_STRUCT_IP_MREQ_SOURCE
		struct ip_mreq_source imr;
		memset (&imr, 0, sizeof (imr));
#endif
#ifdef HAVE_STRUCT_GROUP_SOURCE_REQ
		struct group_source_req group_source_req;
		memset(&group_source_req, 0, sizeof(struct group_source_req));
		group_source_req.gsr_interface = iface;
		group=(struct sockaddr_in*)&group_source_req.gsr_group;
		source=(struct sockaddr_in*)&group_source_req.gsr_source;
#else
		struct sockaddr_in imrgroup;
		struct sockaddr_in imrsource;
		group = &imrgroup;
		source = &imrsource;
#endif
		source->sin_family = AF_INET;
		group->sin_family = AF_INET;
		/* Set the group */
		rc=getsockname(ListenSocket,(struct sockaddr *)group, &socklen);
		FAIL_errno( rc == SOCKET_ERROR, "mcast join source group getsockname",mSettings );
		group->sin_port = 0;    /* Ignored */

		/* Set the source, apply the S,G */
		rc=inet_pton(AF_INET,mSettings->mSSMMulticastStr,&source->sin_addr);
		FAIL_errno(rc != 1, "mcast join source pton",mSettings );
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
		source->sin_len = group->sin_len;
#endif
		source->sin_port = 0;    /* Ignored */
		rc = -1;

#if HAVE_DECL_MCAST_JOIN_SOURCE_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IP,MCAST_JOIN_SOURCE_GROUP, &group_source_req,
				sizeof(group_source_req));
#endif

#if HAVE_DECL_IP_ADD_SOURCE_MEMBERSHIP
#ifdef HAVE_STRUCT_IP_MREQ_SOURCE
		// Some operating systems will have MCAST_JOIN_SOURCE_GROUP but still fail
		// In those cases try the IP_ADD_SOURCE_MEMBERSHIP
		if (rc < 0) {
#ifdef HAVE_STRUCT_IP_MREQ_SOURCE_IMR_MULTIADDR_S_ADDR
		    imr.imr_multiaddr = ((const struct sockaddr_in *)group)->sin_addr;
		    imr.imr_sourceaddr = ((const struct sockaddr_in *)source)->sin_addr;
#else
		    // Some Android versions declare mreq_source without an s_addr
		    imr.imr_multiaddr = ((const struct sockaddr_in *)group)->sin_addr.s_addr;
		    imr.imr_sourceaddr = ((const struct sockaddr_in *)source)->sin_addr.s_addr;
#endif
		    rc = setsockopt (ListenSocket, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, (char*)(&imr), sizeof (imr));
		}
#endif
#endif
		FAIL_errno( rc == SOCKET_ERROR, "mcast join source group",mSettings);
	    } else {
		struct group_req group_req;
		struct sockaddr_in *group;

		memset(&group_req, 0, sizeof(struct group_req));

		group_req.gr_interface = iface;
		group=(struct sockaddr_in*)&group_req.gr_group;
		group->sin_family = AF_INET;
		/* Set the group */
		rc=getsockname(ListenSocket,(struct sockaddr *)group, &socklen);
		FAIL_errno( rc == SOCKET_ERROR, "mcast join group getsockname",mSettings );
		group->sin_port = 0;    /* Ignored */
		rc = -1;
#if HAVE_DECL_MCAST_JOIN_GROUP
		rc = setsockopt(ListenSocket,IPPROTO_IP,MCAST_JOIN_GROUP, &group_req,
				sizeof(group_source_req));
#endif
		FAIL_errno( rc == SOCKET_ERROR, "mcast join group",mSettings);
	    }
	}
#else
	fprintf(stderr, "Unfortunately, SSM is not supported on this platform\n");
	exit(-1);
#endif
    }
}
// end my_multicast_join()

bool Listener::L2_setup (thread_Settings *server, int sockfd) {
#if defined(HAVE_LINUX_FILTER_H) && defined(HAVE_AF_PACKET)
    //
    //  Supporting parallel L2 UDP threads is a bit tricky.  Below are some notes as to why and the approach used.
    //
    //  The primary issues for UDP are:
    //
    //  1) We want the listener thread to hand off the flow to a server thread and not be burdened by that flow
    //  2) For -P support, the listener thread neads to detect new flows which will share the same UDP port
    //     and UDP is stateless
    //
    //  The listener thread needs to detect new traffic flows and hand them to a new server thread, and then
    //  rehang a listen/accept.  For standard iperf the "flow routing" is done using connect() per the ip quintuple.
    //  The OS will then route established connected flows to the socket descriptor handled by a server thread and won't
    //  burden the listener thread with these packets.
    //
    //  For L2 verification, we have to create a two sockets that will exist for the life of the flow.  A
    //  new packet socket (AF_PACKET) will receive L2 frames and bypasses
    //  the OS network stack.  The original AF_INET socket will still send up packets
    //  to the network stack.
    //
    //  When using packet sockets there is inherent packet duplication, the hand off to a server
    //  thread is not so straight forward as packets will continue being sent up to the listener thread
    //  (technical problem is that packet sockets do not support connect() which binds the IP quintuple as the
    //  forwarding key) Since the Listener uses recvfrom(), there is no OS mechanism to detect new flows nor
    //  to drop packets.  The listener can't listen on quintuple based connected flows because the client's source
    //  port is unknown.  Therefore the Listener thread will continue to receive packets from all established
    //  flows sharing the same dst port which will impact CPU utilization and hence performance.
    //
    //  The technique used to address this is to open an AF_PACKET socket and leave the AF_INET socket open.
    //  (This also aligns with BSD based systems)  The original AF_INET socket will remain in the (connected)
    //  state so the network stack has it's connected state.  A cBPF is then used to cause the kernel to fast drop
    //  those packets.  A cBPF is set up to drop such packets.  The test traffic will then only come over the
    //  packet (raw) socket and not the  AF_INET socket. If we were to try to close the original AF_INET socket
    //  (vs leave it open w/the fast drop cBPF) then the existing traffic will be sent up by the network stack
    //  to he Listener thread, flooding it with packets, again something we want to avoid.
    //
    //  On the packet (raw) socket itself, we do two more things to better handle performance
    //
    //  1)  Use a full quintuple cBPF allowing the kernel to filter packets (allow) per the quintuple
    //  2)  Use the packet fanout option to assign a CBPF to a socket and hence to a single server thread minimizing
    //      duplication (reduce all cBPF's filtering load)
    //
    struct sockaddr *p = (sockaddr *)&server->peer;
    struct sockaddr *l = (sockaddr *)&server->local;
    int rc = 0;

    //
    // Establish a packet (raw) socket to be used by the server thread giving it full L2 packets
    //
    struct sockaddr s;
    socklen_t len = sizeof(s);
    getpeername(sockfd, &s, &len);
    if (isIPV6(server)) {
	server->mSock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IPV6));
	WARN_errno(server->mSock == INVALID_SOCKET, "ip6 packet socket (AF_PACKET)");
	server->l4offset = IPV6HDRLEN + sizeof(struct ether_header);
    } else {
	server->mSock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
	WARN_errno(server->mSock == INVALID_SOCKET, "ip packet socket (AF_PACKET)");
	unsetIPV6(server);
	server->l4offset = sizeof(struct iphdr) + sizeof(struct ether_header);
    }
    // Didn't get a valid socket, return now
    if (server->mSock < 0) {
	return false;
    }
    // More per thread settings based on using a packet socket
    server->l4payloadoffset = server->l4offset + sizeof(struct udphdr);
    server->recvflags = MSG_TRUNC;
    // The original AF_INET socket only exists to keep the connected state
    // in the OS for this flow. Fast drop packets there as
    // now packets will use the AF_PACKET (raw) socket
    // Also, store the original AF_INET socket descriptor so it can be
    // closed in the Server's destructor.  (Note: closing the
    // socket descriptors will also free the cBPF.)
    //
    server->mSockDrop = sockfd;
    rc = SockAddr_Drop_All_BPF(sockfd);
    WARN_errno( rc == SOCKET_ERROR, "l2 all drop bpf");

    // Now optimize packet flow up the raw socket
    // Establish the flow BPF to forward up only "connected" packets to this raw socket
    if (l->sa_family == AF_INET6) {
#ifdef HAVE_IPV6
	struct in6_addr *v6peer = SockAddr_get_in6_addr(&server->peer);
	struct in6_addr *v6local = SockAddr_get_in6_addr(&server->local);
	if (isIPV6(server)) {
	    rc = SockAddr_v6_Connect_BPF(server->mSock, v6local, v6peer, ((struct sockaddr_in6 *)(l))->sin6_port, ((struct sockaddr_in6 *)(p))->sin6_port);
	    WARN_errno( rc == SOCKET_ERROR, "l2 connect ipv6 bpf");
	} else {
	    // This is an ipv4 address in a v6 family (structure), just pull the lower 32 bits for the v4 addr
	    rc = SockAddr_v4_Connect_BPF(server->mSock, (uint32_t) v6local->s6_addr32[3], (uint32_t) v6peer->s6_addr32[3], ((struct sockaddr_in6 *)(l))->sin6_port, ((struct sockaddr_in6 *)(p))->sin6_port);
	    WARN_errno( rc == SOCKET_ERROR, "l2 v4in6 connect ip bpf");
	}
#else
	fprintf(stderr, "Unfortunately, IPv6 is not supported on this platform\n");
	return false;
#endif /* HAVE_IPV6 */
    } else {
	rc = SockAddr_v4_Connect_BPF(server->mSock, ((struct sockaddr_in *)(l))->sin_addr.s_addr, ((struct sockaddr_in *)(p))->sin_addr.s_addr, ((struct sockaddr_in *)(l))->sin_port, ((struct sockaddr_in *)(p))->sin_port);
	WARN_errno( rc == SOCKET_ERROR, "l2 connect ip bpf");
    }
    if (rc < 0)
	return false;
    else
	return true;
#else
    fprintf(stderr, "Client requested --l2checks but not supported on this platform\n");
    return false;
#endif
}


/* ------------------------------------------------------------------------
 * Do the equivalent of an accept() call for UDP sockets. This checks
 * a listening UDP socket for new or first received datagram
 * ------------------------------------------------------------------- ----*/
int Listener::udp_accept (thread_Settings *server) {
    assert(server != NULL);
    int rc;
    assert(ListenSocket > 0);
    // Start with a thread_rest - this allows the server thread
    // a shot at processing the
    // Preset the server socket to INVALID, hang recvfrom on the Listener's socket
    // The INVALID socket is used to keep the while loop going
    server->mSock = INVALID_SOCKET;
    // Hang a 0 byte read with MSG_PEEK to get the sock addr struct populated
    rc = recvfrom(ListenSocket, NULL, 0, MSG_PEEK, \
		  (struct sockaddr*) &server->peer, &server->size_peer);
#if HAVE_THREAD_DEBUG
    {
	char tmpaddr[200];
	size_t len=200;
	unsigned short port = SockAddr_getPort(&server->peer);
	SockAddr_getHostAddress(&server->peer, tmpaddr, len);
	thread_debug("rcvfrom peer: %s port %d len=%d", tmpaddr, port, rc);
    }
#endif
    FAIL_errno(rc == SOCKET_ERROR, "recvfrom", mSettings);
    if (!(rc < 0) && !sInterupted) {
	// Handle connection for UDP sockets
	if (Iperf_push_host_port_conditional(&server->peer, server)) {
	    // We have a new UDP flow (based upon key of quintuple)
	    // so let's hand off this socket
	    // to the server and create a new listener socket
	    server->mSock = ListenSocket;
	    ListenSocket = INVALID_SOCKET;
	    // This connect() will allow the OS to only
	    // send packets with the ip quintuple up to the server
	    // socket and, hence, to the server thread (yet to be created)
	    // This connect() routing is only supported with AF_INET or AF_INET6 sockets,
	    // e.g. AF_PACKET sockets can't do this.  We'll handle packet sockets later
	    // All UDP accepts here will use AF_INET.  This is intentional and needed
	    rc = connect(server->mSock, (struct sockaddr*) &server->peer, server->size_peer);
	    FAIL_errno(rc == SOCKET_ERROR, "connect UDP", mSettings);
	    my_listen(); // This will set ListenSocket to a new sock fd
	    if (isConnectionReport(server)) {
		// InitConnectionReport(server);
	    }
	}
    }
    return server->mSock;
}
/* -------------------------------------------------------------------
 * This is called by the Listener thread main loop, return a socket or error
 * ------------------------------------------------------------------- */
int Listener::my_accept (thread_Settings *server) {
    assert(server != NULL);
#ifdef HAVE_THREAD_DEBUG
    if (isUDP(server)) {
	thread_debug("Listener thread listening for UDP (sock=%d)", ListenSocket);
    } else {
	thread_debug("Listener thread listening for TCP (sock=%d)", ListenSocket);
    }
#endif
    server->size_peer = sizeof(iperf_sockaddr);
    server->accept_time.tv_sec = 0;
    server->accept_time.tv_usec = 0;
    if (isUDP(server)) {
	server->mSock = udp_accept(server);
	// note udp_accept will update the active host table
    } else {
	// accept a TCP  connection
	server->mSock = accept(ListenSocket, (sockaddr*) &server->peer, &server->size_peer);
	if (server->mSock > 0)
	    Iperf_push_host(&server->peer, server);
    }
    if (server->mSock > 0) {
	Timestamp now;
	server->accept_time.tv_sec = now.getSecs();
	server->accept_time.tv_usec = now.getUsecs();
	if (isConnectionReport(server)) {
	    // InitConnectionReport(server);
	}
    }
    return server->mSock;
} // end my_accept

// Read deep enough into the packet to get the client settings
// Read the headers but don't pull them from the queue in order to
// preserve server thread accounting, i.e. these exchanges will
// be part of traffic accounting
void Listener::apply_client_settings (thread_Settings *server) {
    assert(server != NULL);
    assert(mBuf != NULL);
    int n, peeklen;
    // Set the receive timeout for the very first read based upon the -t
    // and not -i.
    if (isModeTime(server)) {
#ifdef WIN32
	// Windows SO_RCVTIMEO uses ms
	DWORD timeout = (double) sorcvtimer / 1e3;
#else
	struct timeval timeout;
	timeout.tv_sec = mSettings->mAmount / 100;
	timeout.tv_usec = (mSettings->mAmount % 100) * 10000;
#endif // WIN32
	if (setsockopt( server->mSock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0 ) {
	    WARN_errno( server->mSock == SO_RCVTIMEO, "socket" );
	}
    }
    if (isUDP(server)) {
	peeklen = sizeof(struct client_udphdr);
	n = recvn(server->mSock, mBuf, peeklen, MSG_PEEK);
	FAIL_errno((n < peeklen), "read flags", server);
	struct client_udphdr *hdr = (struct client_udphdr *) mBuf;
	uint32_t flags = ntohl(hdr->base.flags);
	if ((flags & HEADER_UDPTESTS) != 0) {
	    uint16_t testflags = ntohs(hdr->udp.testflags);
	    // Handle stateless flags
	    if ((testflags & HEADER_UDP_ISOCH) != 0) {
		setIsochronous(server);
	    }
	    if ((testflags & HEADER_L2ETHPIPV6) != 0) {
		setIPV6(server);
	    } else {
		unsetIPV6(server);
	    }
	    if ((testflags & HEADER_L2LENCHECK) != 0) {
		setL2LengthCheck(server);
	    }
	    if ((testflags & HEADER_NOUDPFIN) != 0) {
		setNoUDPfin(server);
	    }
	    if ((testflags & HEADER_PKTTRIPTIME) != 0) {
		setTripTime(server);
	    }
	    setSeqNo64b(server);
	    reporter_peerversion(server, ntohl(hdr->udp.version_u), ntohl(hdr->udp.version_l));
	}
    } else {
	n = recvn(server->mSock, mBuf, sizeof(uint32_t), MSG_PEEK);
	FAIL_errno((n != sizeof(uint32_t)), "read tcp flags", server);
	struct client_tcphdr *hdr = (struct client_tcphdr *) mBuf;
	uint32_t flags = ntohl(hdr->base.flags);
	peeklen = 0;
	if (flags & HEADER_EXTEND) {
	    peeklen = sizeof(struct client_tcphdr);
	} else if (flags & HEADER_VERSION1) {
	    peeklen = sizeof(struct client_hdr_v1);
	}
	if ((flags & HEADER_TRIPTIME) != 0 ) {
	    setTripTime(server);
	}
	if (peeklen && ((n = recvn(server->mSock, mBuf, peeklen, MSG_PEEK)) != peeklen)) {
	    FAIL_errno(1, "read tcp test info", server);
	}
	if (flags & HEADER_EXTEND)
	    reporter_peerversion(server, ntohl(hdr->extend.version_u), ntohl(hdr->extend.version_l));
    }
    // Handle flags that require an ack back to the client
    if (!isMulticast(mSettings)) {
	client_test_ack(server);
    }
}

int Listener::client_test_ack(thread_Settings *server) {
    client_hdr_ack ack;
    int sotimer = 0;
    int optflag;
    ack.typelen.type  = htonl(CLIENTHDRACK);
    ack.typelen.length = htonl(sizeof(client_hdr_ack));
    ack.flags = 0;
    ack.reserved1 = 0;
    ack.reserved2 = 0;
    ack.version_u = htonl(IPERF_VERSION_MAJORHEX);
    ack.version_l = htonl(IPERF_VERSION_MINORHEX);
    int rc = 1;
    // This is a version 2.0.10 or greater client
    // write back to the client so it knows the server
    // version
    if (!isUDP(server)) {
	// sotimer units microseconds convert
	if (server->mInterval) {
	    sotimer = (int) ((server->mInterval * 1e6) / 4);
	} else if (isModeTime(server)) {
	    sotimer = (int) ((server->mAmount * 1000) / 4);
	}
	if (sotimer > HDRXACKMAX) {
	    sotimer = HDRXACKMAX;
	} else if (sotimer < HDRXACKMIN) {
	    sotimer = HDRXACKMIN;
	}
#ifdef WIN32
	// Windows SO_RCVTIMEO uses ms
	DWORD timeout = (double) sotimer / 1e3;
#else
	struct timeval timeout;
	timeout.tv_sec = sotimer / 1000000;
	timeout.tv_usec = sotimer % 1000000;
#endif
	if ((rc = setsockopt(server->mSock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout))) < 0 ) {
	    WARN_errno( rc < 0, "setsockopt SO_SNDTIMEO");
	}
#ifdef TCP_NODELAY
	optflag=1;
	// Disable Nagle to reduce latency of this intial message
	if ((rc = setsockopt(server->mSock, IPPROTO_TCP, TCP_NODELAY, (char *)&optflag, sizeof(int))) < 0 ) {
	    WARN_errno(rc < 0, "tcpnodelay" );
	}
#endif
    }
    if ((rc = send(server->mSock, (const char*)&ack, sizeof(client_hdr_ack),0)) < 0) {
	WARN_errno( rc <= 0, "send_ack" );
	rc = 0;
    }
    // Re-nable Nagle
    optflag=0;
    if (!isUDP( server ) && (rc = setsockopt( server->mSock, IPPROTO_TCP, TCP_NODELAY, (char *)&optflag, sizeof(int))) < 0 ) {
	WARN_errno(rc < 0, "tcpnodelay" );
    }
    return rc;
}

void Listener::UDPSingleServer(thread_Settings *server) {
    assert(server != NULL);
    Settings_Destroy(server);
    fprintf(stderr, "UDP single server or non threaded code not implemented\n");
}
