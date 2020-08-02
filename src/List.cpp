/*---------------------------------------------------------------
 * Copyrig h(c) 1999,2000,2001,2002,2003
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
 * List.cpp
 * rewrite by Robert McMahon
 * This is a list to hold active traffic and create sum groups
 * sum groups are traffic sessions from the same client host
 * -------------------------------------------------------------------
 */

#include "List.h"
#include "Mutex.h"
#include "SocketAddr.h"
#include "Reporter.h"

/*
 * Global table with active hosts, their sum reports and active thread counts
 */
static struct Iperf_Table active_table;
static bool Iperf_host_port_present (iperf_sockaddr *find);
static Iperf_ListEntry* Iperf_host_present (iperf_sockaddr *find);

#if HAVE_THREAD_DEBUG
static void active_table_show_entry(const char *action, Iperf_ListEntry *entry, int found) {
    assert(action != NULL);
    assert(entry != NULL);
    char tmpaddr[200];
    size_t len=200;
    unsigned short port = SockAddr_getPort(&(entry->host));
    SockAddr_getHostAddress(&(entry->host), tmpaddr, len);
    thread_debug("active table: %s %s port %d (found=%d) rootp=%p entryp=%p totcnt/activecnt/hostcnt = %d/%d/%d", \
		 action, tmpaddr, port, found, (void *) active_table.root, (void *) entry, active_table.total_count, \
		 active_table.count, entry->thread_count);
}
#endif


void Iperf_initialize_active_table (void) {
    Mutex_Initialize(&active_table.my_mutex);
    active_table.root = NULL;
}

/*
 * Add Entry add to the list and optionally update thread count,
 * return true if host is already in the table
 */
static void active_table_update (iperf_sockaddr *host, struct thread_Settings *agent) {
    assert(host != NULL);
    assert(agent != NULL);
    Iperf_ListEntry *this_entry = Iperf_host_present(host);
    if (this_entry == NULL) {
	this_entry = new Iperf_ListEntry();
	assert(this_entry != NULL);
	this_entry->host = *host;
	this_entry->next = active_table.root;
	this_entry->thread_count = 1;
	active_table.count++;
	active_table.total_count++;
	if (isDataReport(agent)) {
	    this_entry->sum_report = InitSumReport(agent, active_table.total_count);
	    agent->multihdr = this_entry->sum_report;
	    IncrMultiHdrRefCounter(agent->multihdr);
	} else {
	    agent->multihdr = NULL;
	}
	active_table.root = this_entry;
#if HAVE_THREAD_DEBUG
	active_table_show_entry("new insert", this_entry, 0);
#endif
    } else {
	this_entry->thread_count++;
	if (isDataReport(agent)) {
	    agent->multihdr = this_entry->sum_report;
	    IncrMultiHdrRefCounter(agent->multihdr);
	} else {
	    agent->multihdr = NULL;
	}
#if HAVE_THREAD_DEBUG
	active_table_show_entry("incr insert", this_entry, 1);
#endif
    }
}

void Iperf_push_host (iperf_sockaddr *host, struct thread_Settings *agent) {
    Mutex_Lock(&active_table.my_mutex);
    active_table_update(host, agent);
    Mutex_Unlock(&active_table.my_mutex);
}

bool Iperf_push_host_port_conditional (iperf_sockaddr *host, struct thread_Settings *agent) {
    bool rc = false;
    Mutex_Lock(&active_table.my_mutex);
    if (!Iperf_host_port_present(host)) {
	active_table_update(host, agent);
	rc = true;
    }
    Mutex_Unlock(&active_table.my_mutex);
    return (rc);
}

/*
 * Delete Entry del from the List
 */
void Iperf_remove_host (iperf_sockaddr *del) {
    // remove_list_entry(entry) {
    //     indirect = &head;
    //     while ((*indirect) != entry) {
    //	       indirect = &(*indirect)->next;
    //     }
    //     *indirect = entry->next
    Mutex_Lock(&active_table.my_mutex);
    Iperf_ListEntry **tmp = &active_table.root;
    while ((*tmp) && !(SockAddr_Hostare_Equal(&(*tmp)->host, del))) {
	tmp = &(*tmp)->next;
    }
    if (*tmp) {
	if (--(*tmp)->thread_count == 0) {
	    Iperf_ListEntry *remove = (*tmp);
	    active_table.count--;	    
#if HAVE_THREAD_DEBUG
	    active_table_show_entry("delete", remove, 1);
#endif
	    *tmp = remove->next;
	    delete remove;
	} else {
#if HAVE_THREAD_DEBUG
	    active_table_show_entry("decr", (*tmp), 1);
#endif
	}
    }
    Mutex_Unlock(&active_table.my_mutex);
}

/*
 * Destroy the List (cleanup function)
 */
void Iperf_destroy_active_table (void) {
    Iperf_ListEntry *itr1 = active_table.root, *itr2;
    while (itr1 != NULL) {
        itr2 = itr1->next;
        delete itr1;
        itr1 = itr2;
    }
    Mutex_Destroy(&active_table.my_mutex);
    active_table.root = NULL;
    active_table.count = 0;
    active_table.total_count = 0;
}

/*
 * Check if the exact Entry find is present
 */

bool Iperf_host_port_present (iperf_sockaddr *find) {
    Iperf_ListEntry *itr = active_table.root;
    bool rc = false;
    while (itr != NULL) {
#if HAVE_THREAD_DEBUG
	active_table_show_entry("UDP compare against host port", itr, 0);
#endif
        if (SockAddr_are_Equal(&itr->host, find)) {
#if HAVE_THREAD_DEBUG
	    active_table_show_entry("table match", itr, 0);
#endif
	    rc = true;
            break;
        } else {
#if HAVE_THREAD_DEBUG
	    active_table_show_entry("table miss", itr, 0);
#endif
	    itr = itr->next;
	}
    }
    return rc;
}

/*
 * Check if a Entry find is in the List or if any
 * Entry exists that has the same host as the
 * Entry find
 */
static Iperf_ListEntry* Iperf_host_present (iperf_sockaddr *find) {
    Iperf_ListEntry *itr = active_table.root;
    while (itr != NULL) {
#if HAVE_THREAD_DEBUG
	active_table_show_entry("check for host", itr, 0);
#endif
        if (SockAddr_Hostare_Equal(&itr->host, find)) {
#if HAVE_THREAD_DEBUG
	    active_table_show_entry("table match", itr, 0);
#endif
            break;
        } else {
#if HAVE_THREAD_DEBUG
	    active_table_show_entry("table miss", itr, 0);
#endif
	    itr = itr->next;
	}
    }
    return itr;
}
