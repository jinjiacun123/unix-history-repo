/*
 * Copyright (C) 2004-2007  Internet Systems Consortium, Inc. ("ISC")
 * Copyright (C) 2000, 2001  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: lwdgabn.c,v 1.22 2007-06-19 23:46:59 tbox Exp $ */

/*! \file */

#include <config.h>

#include <stdlib.h>

#include <isc/netaddr.h>
#include <isc/sockaddr.h>
#include <isc/socket.h>
#include <isc/string.h>		/* Required for HP/UX (and others?) */
#include <isc/util.h>

#include <dns/adb.h>
#include <dns/events.h>
#include <dns/result.h>

#include <named/types.h>
#include <named/lwaddr.h>
#include <named/lwdclient.h>
#include <named/lwresd.h>
#include <named/lwsearch.h>
#include <named/sortlist.h>

#define NEED_V4(c)	((((c)->find_wanted & LWRES_ADDRTYPE_V4) != 0) \
			 && ((c)->v4find == NULL))
#define NEED_V6(c)	((((c)->find_wanted & LWRES_ADDRTYPE_V6) != 0) \
			 && ((c)->v6find == NULL))

static isc_result_t start_find(ns_lwdclient_t *);
static void restart_find(ns_lwdclient_t *);
static void init_gabn(ns_lwdclient_t *);

/*%
 * Destroy any finds.  This can be used to "start over from scratch" and
 * should only be called when events are _not_ being generated by the finds.
 */
static void
cleanup_gabn(ns_lwdclient_t *client) {
	ns_lwdclient_log(50, "cleaning up client %p", client);

	if (client->v6find != NULL) {
		if (client->v6find == client->v4find)
			client->v6find = NULL;
		else
			dns_adb_destroyfind(&client->v6find);
	}
	if (client->v4find != NULL)
		dns_adb_destroyfind(&client->v4find);
}

static void
setup_addresses(ns_lwdclient_t *client, dns_adbfind_t *find, unsigned int at) {
	dns_adbaddrinfo_t *ai;
	lwres_addr_t *addr;
	int af;
	const struct sockaddr *sa;
	isc_result_t result;

	if (at == DNS_ADBFIND_INET)
		af = AF_INET;
	else
		af = AF_INET6;

	ai = ISC_LIST_HEAD(find->list);
	while (ai != NULL && client->gabn.naddrs < LWRES_MAX_ADDRS) {
		sa = &ai->sockaddr.type.sa;
		if (sa->sa_family != af)
			goto next;

		addr = &client->addrs[client->gabn.naddrs];

		result = lwaddr_lwresaddr_fromsockaddr(addr, &ai->sockaddr);
		if (result != ISC_R_SUCCESS)
			goto next;

		ns_lwdclient_log(50, "adding address %p, family %d, length %d",
				 addr->address, addr->family, addr->length);

		client->gabn.naddrs++;
		REQUIRE(!LWRES_LINK_LINKED(addr, link));
		LWRES_LIST_APPEND(client->gabn.addrs, addr, link);

	next:
		ai = ISC_LIST_NEXT(ai, publink);
	}
}

typedef struct {
	isc_netaddr_t address;
	int rank;
} rankedaddress;

static int
addr_compare(const void *av, const void *bv) {
	const rankedaddress *a = (const rankedaddress *) av;
	const rankedaddress *b = (const rankedaddress *) bv;
	return (a->rank - b->rank);
}

static void
sort_addresses(ns_lwdclient_t *client) {
	unsigned int naddrs;
	rankedaddress *addrs;
	isc_netaddr_t remote;
	dns_addressorderfunc_t order;
	const void *arg;
	ns_lwresd_t *lwresd = client->clientmgr->listener->manager;
	unsigned int i;
	isc_result_t result;

	naddrs = client->gabn.naddrs;

	if (naddrs <= 1 || lwresd->view->sortlist == NULL)
		return;

	addrs = isc_mem_get(lwresd->mctx, sizeof(rankedaddress) * naddrs);
	if (addrs == NULL)
		return;

	isc_netaddr_fromsockaddr(&remote, &client->address);
	ns_sortlist_byaddrsetup(lwresd->view->sortlist,
				&remote, &order, &arg);
	if (order == NULL) {
		isc_mem_put(lwresd->mctx, addrs,
			    sizeof(rankedaddress) * naddrs);
		return;
	}
	for (i = 0; i < naddrs; i++) {
		result = lwaddr_netaddr_fromlwresaddr(&addrs[i].address,
						      &client->addrs[i]);
		INSIST(result == ISC_R_SUCCESS);
		addrs[i].rank = (*order)(&addrs[i].address, arg);
	}
	qsort(addrs, naddrs, sizeof(rankedaddress), addr_compare);
	for (i = 0; i < naddrs; i++) {
		result = lwaddr_lwresaddr_fromnetaddr(&client->addrs[i],
						      &addrs[i].address);
		INSIST(result == ISC_R_SUCCESS);
	}

	isc_mem_put(lwresd->mctx, addrs, sizeof(rankedaddress) * naddrs);
}

static void
generate_reply(ns_lwdclient_t *client) {
	isc_result_t result;
	int lwres;
	isc_region_t r;
	lwres_buffer_t lwb;
	ns_lwdclientmgr_t *cm;

	cm = client->clientmgr;
	lwb.base = NULL;

	ns_lwdclient_log(50, "generating gabn reply for client %p", client);

	/*
	 * We must make certain the client->find is not still active.
	 * If it is either the v4 or v6 answer, just set it to NULL and
	 * let the cleanup code destroy it.  Otherwise, destroy it now.
	 */
	if (client->find == client->v4find || client->find == client->v6find)
		client->find = NULL;
	else
		if (client->find != NULL)
			dns_adb_destroyfind(&client->find);

	/*
	 * perhaps there are some here?
	 */
	if (NEED_V6(client) && client->v4find != NULL)
		client->v6find = client->v4find;

	/*
	 * Run through the finds we have and wire them up to the gabn
	 * structure.
	 */
	LWRES_LIST_INIT(client->gabn.addrs);
	if (client->v4find != NULL)
		setup_addresses(client, client->v4find, DNS_ADBFIND_INET);
	if (client->v6find != NULL)
		setup_addresses(client, client->v6find, DNS_ADBFIND_INET6);

	/*
	 * If there are no addresses, try the next element in the search
	 * path, if there are any more.  Otherwise, fall through into
	 * the error handling code below.
	 */
	if (client->gabn.naddrs == 0) {
		do {
			result = ns_lwsearchctx_next(&client->searchctx);
			if (result == ISC_R_SUCCESS) {
				cleanup_gabn(client);
				result = start_find(client);
				if (result == ISC_R_SUCCESS)
					return;
			}
		} while (result == ISC_R_SUCCESS);
	}

	/*
	 * Render the packet.
	 */
	client->pkt.recvlength = LWRES_RECVLENGTH;
	client->pkt.authtype = 0; /* XXXMLG */
	client->pkt.authlength = 0;

	/*
	 * If there are no addresses, return failure.
	 */
	if (client->gabn.naddrs != 0)
		client->pkt.result = LWRES_R_SUCCESS;
	else
		client->pkt.result = LWRES_R_NOTFOUND;

	sort_addresses(client);

	lwres = lwres_gabnresponse_render(cm->lwctx, &client->gabn,
					  &client->pkt, &lwb);
	if (lwres != LWRES_R_SUCCESS)
		goto out;

	r.base = lwb.base;
	r.length = lwb.used;
	client->sendbuf = r.base;
	client->sendlength = r.length;
	result = ns_lwdclient_sendreply(client, &r);
	if (result != ISC_R_SUCCESS)
		goto out;

	NS_LWDCLIENT_SETSEND(client);

	/*
	 * All done!
	 */
	cleanup_gabn(client);

	return;

 out:
	cleanup_gabn(client);

	if (lwb.base != NULL)
		lwres_context_freemem(client->clientmgr->lwctx,
				      lwb.base, lwb.length);

	ns_lwdclient_errorpktsend(client, LWRES_R_FAILURE);
}

/*
 * Take the current real name, move it to an alias slot (if any are
 * open) then put this new name in as the real name for the target.
 *
 * Return success if it can be rendered, otherwise failure.  Note that
 * not having enough alias slots open is NOT a failure.
 */
static isc_result_t
add_alias(ns_lwdclient_t *client) {
	isc_buffer_t b;
	isc_result_t result;
	isc_uint16_t naliases;

	b = client->recv_buffer;

	/*
	 * Render the new name to the buffer.
	 */
	result = dns_name_totext(dns_fixedname_name(&client->target_name),
				 ISC_TRUE, &client->recv_buffer);
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Are there any open slots?
	 */
	naliases = client->gabn.naliases;
	if (naliases < LWRES_MAX_ALIASES) {
		client->gabn.aliases[naliases] = client->gabn.realname;
		client->gabn.aliaslen[naliases] = client->gabn.realnamelen;
		client->gabn.naliases++;
	}

	/*
	 * Save this name away as the current real name.
	 */
	client->gabn.realname = (char *)(b.base) + b.used;
	client->gabn.realnamelen = client->recv_buffer.used - b.used;

	return (ISC_R_SUCCESS);
}

static isc_result_t
store_realname(ns_lwdclient_t *client) {
	isc_buffer_t b;
	isc_result_t result;
	dns_name_t *tname;

	b = client->recv_buffer;

	tname = dns_fixedname_name(&client->target_name);
	result = ns_lwsearchctx_current(&client->searchctx, tname);
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Render the new name to the buffer.
	 */
	result = dns_name_totext(tname, ISC_TRUE, &client->recv_buffer);
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Save this name away as the current real name.
	 */
	client->gabn.realname = (char *) b.base + b.used;
	client->gabn.realnamelen = client->recv_buffer.used - b.used;

	return (ISC_R_SUCCESS);
}

static void
process_gabn_finddone(isc_task_t *task, isc_event_t *ev) {
	ns_lwdclient_t *client = ev->ev_arg;
	isc_eventtype_t evtype;
	isc_boolean_t claimed;

	ns_lwdclient_log(50, "find done for task %p, client %p", task, client);

	evtype = ev->ev_type;
	isc_event_free(&ev);

	/*
	 * No more info to be had?  If so, we have all the good stuff
	 * right now, so we can render things.
	 */
	claimed = ISC_FALSE;
	if (evtype == DNS_EVENT_ADBNOMOREADDRESSES) {
		if (NEED_V4(client)) {
			client->v4find = client->find;
			claimed = ISC_TRUE;
		}
		if (NEED_V6(client)) {
			client->v6find = client->find;
			claimed = ISC_TRUE;
		}
		if (client->find != NULL) {
			if (claimed)
				client->find = NULL;
			else
				dns_adb_destroyfind(&client->find);

		}
		generate_reply(client);
		return;
	}

	/*
	 * We probably don't need this find anymore.  We're either going to
	 * reissue it, or an error occurred.  Either way, we're done with
	 * it.
	 */
	if ((client->find != client->v4find)
	    && (client->find != client->v6find)) {
		dns_adb_destroyfind(&client->find);
	} else {
		client->find = NULL;
	}

	/*
	 * We have some new information we can gather.  Run off and fetch
	 * it.
	 */
	if (evtype == DNS_EVENT_ADBMOREADDRESSES) {
		restart_find(client);
		return;
	}

	/*
	 * An error or other strangeness happened.  Drop this query.
	 */
	cleanup_gabn(client);
	ns_lwdclient_errorpktsend(client, LWRES_R_FAILURE);
}

static void
restart_find(ns_lwdclient_t *client) {
	unsigned int options;
	isc_result_t result;
	isc_boolean_t claimed;

	ns_lwdclient_log(50, "starting find for client %p", client);

	/*
	 * Issue a find for the name contained in the request.  We won't
	 * set the bit that says "anything is good enough" -- we want it
	 * all.
	 */
	options = 0;
	options |= DNS_ADBFIND_WANTEVENT;
	options |= DNS_ADBFIND_RETURNLAME;

	/*
	 * Set the bits up here to mark that we want this address family
	 * and that we do not currently have a find pending.  We will
	 * set that bit again below if it turns out we will get an event.
	 */
	if (NEED_V4(client))
		options |= DNS_ADBFIND_INET;
	if (NEED_V6(client))
		options |= DNS_ADBFIND_INET6;

 find_again:
	INSIST(client->find == NULL);
	result = dns_adb_createfind(client->clientmgr->view->adb,
				    client->clientmgr->task,
				    process_gabn_finddone, client,
				    dns_fixedname_name(&client->target_name),
				    dns_rootname, 0, options, 0,
				    dns_fixedname_name(&client->target_name),
				    client->clientmgr->view->dstport,
				    &client->find);

	/*
	 * Did we get an alias?  If so, save it and re-issue the query.
	 */
	if (result == DNS_R_ALIAS) {
		ns_lwdclient_log(50, "found alias, restarting query");
		dns_adb_destroyfind(&client->find);
		cleanup_gabn(client);
		result = add_alias(client);
		if (result != ISC_R_SUCCESS) {
			ns_lwdclient_log(50,
					 "out of buffer space adding alias");
			ns_lwdclient_errorpktsend(client, LWRES_R_FAILURE);
			return;
		}
		goto find_again;
	}

	ns_lwdclient_log(50, "find returned %d (%s)", result,
			 isc_result_totext(result));

	/*
	 * Did we get an error?
	 */
	if (result != ISC_R_SUCCESS) {
		if (client->find != NULL)
			dns_adb_destroyfind(&client->find);
		cleanup_gabn(client);
		ns_lwdclient_errorpktsend(client, LWRES_R_FAILURE);
		return;
	}

	claimed = ISC_FALSE;

	/*
	 * Did we get our answer to V4 addresses?
	 */
	if (NEED_V4(client)
	    && ((client->find->query_pending & DNS_ADBFIND_INET) == 0)) {
		ns_lwdclient_log(50, "client %p ipv4 satisfied by find %p",
				 client, client->find);
		claimed = ISC_TRUE;
		client->v4find = client->find;
	}

	/*
	 * Did we get our answer to V6 addresses?
	 */
	if (NEED_V6(client)
	    && ((client->find->query_pending & DNS_ADBFIND_INET6) == 0)) {
		ns_lwdclient_log(50, "client %p ipv6 satisfied by find %p",
				 client, client->find);
		claimed = ISC_TRUE;
		client->v6find = client->find;
	}

	/*
	 * If we're going to get an event, set our internal pending flag
	 * and return.  When we get an event back we'll do the right
	 * thing, basically by calling this function again, perhaps with a
	 * new target name.
	 *
	 * If we have both v4 and v6, and we are still getting an event,
	 * we have a programming error, so die hard.
	 */
	if ((client->find->options & DNS_ADBFIND_WANTEVENT) != 0) {
		ns_lwdclient_log(50, "event will be sent");
		INSIST(client->v4find == NULL || client->v6find == NULL);
		return;
	}
	ns_lwdclient_log(50, "no event will be sent");
	if (claimed)
		client->find = NULL;
	else
		dns_adb_destroyfind(&client->find);

	/*
	 * We seem to have everything we asked for, or at least we are
	 * able to respond with things we've learned.
	 */

	generate_reply(client);
}

static isc_result_t
start_find(ns_lwdclient_t *client) {
	isc_result_t result;

	/*
	 * Initialize the real name and alias arrays in the reply we're
	 * going to build up.
	 */
	init_gabn(client);

	result = store_realname(client);
	if (result != ISC_R_SUCCESS)
		return (result);
	restart_find(client);
	return (ISC_R_SUCCESS);

}

static void
init_gabn(ns_lwdclient_t *client) {
	int i;

	/*
	 * Initialize the real name and alias arrays in the reply we're
	 * going to build up.
	 */
	for (i = 0; i < LWRES_MAX_ALIASES; i++) {
		client->aliases[i] = NULL;
		client->aliaslen[i] = 0;
	}
	for (i = 0; i < LWRES_MAX_ADDRS; i++) {
		client->addrs[i].family = 0;
		client->addrs[i].length = 0;
		memset(client->addrs[i].address, 0, LWRES_ADDR_MAXLEN);
		LWRES_LINK_INIT(&client->addrs[i], link);
	}

	client->gabn.naliases = 0;
	client->gabn.naddrs = 0;
	client->gabn.realname = NULL;
	client->gabn.aliases = client->aliases;
	client->gabn.realnamelen = 0;
	client->gabn.aliaslen = client->aliaslen;
	LWRES_LIST_INIT(client->gabn.addrs);
	client->gabn.base = NULL;
	client->gabn.baselen = 0;

	/*
	 * Set up the internal buffer to point to the receive region.
	 */
	isc_buffer_init(&client->recv_buffer, client->buffer, LWRES_RECVLENGTH);
}

/*
 * When we are called, we can be assured that:
 *
 *	client->sockaddr contains the address we need to reply to,
 *
 *	client->pkt contains the packet header data,
 *
 *	the packet "checks out" overall -- any MD5 hashes or crypto
 *	bits have been verified,
 *
 *	"b" points to the remaining data after the packet header
 *	was parsed off.
 *
 *	We are in a the RECVDONE state.
 *
 * From this state we will enter the SEND state if we happen to have
 * everything we need or we need to return an error packet, or to the
 * FINDWAIT state if we need to look things up.
 */
void
ns_lwdclient_processgabn(ns_lwdclient_t *client, lwres_buffer_t *b) {
	isc_result_t result;
	lwres_gabnrequest_t *req;
	ns_lwdclientmgr_t *cm;
	isc_buffer_t namebuf;

	REQUIRE(NS_LWDCLIENT_ISRECVDONE(client));

	cm = client->clientmgr;
	req = NULL;

	result = lwres_gabnrequest_parse(client->clientmgr->lwctx,
					 b, &client->pkt, &req);
	if (result != LWRES_R_SUCCESS)
		goto out;
	if (req->name == NULL)
		goto out;

	isc_buffer_init(&namebuf, req->name, req->namelen);
	isc_buffer_add(&namebuf, req->namelen);

	dns_fixedname_init(&client->target_name);
	dns_fixedname_init(&client->query_name);
	result = dns_name_fromtext(dns_fixedname_name(&client->query_name),
				   &namebuf, NULL, ISC_FALSE, NULL);
	if (result != ISC_R_SUCCESS)
		goto out;
	ns_lwsearchctx_init(&client->searchctx,
			    cm->listener->manager->search,
			    dns_fixedname_name(&client->query_name),
			    cm->listener->manager->ndots);
	ns_lwsearchctx_first(&client->searchctx);

	client->find_wanted = req->addrtypes;
	ns_lwdclient_log(50, "client %p looking for addrtypes %08x",
			 client, client->find_wanted);

	/*
	 * We no longer need to keep this around.
	 */
	lwres_gabnrequest_free(client->clientmgr->lwctx, &req);

	/*
	 * Start the find.
	 */
	result = start_find(client);
	if (result != ISC_R_SUCCESS)
		goto out;

	return;

	/*
	 * We're screwed.  Return an error packet to our caller.
	 */
 out:
	if (req != NULL)
		lwres_gabnrequest_free(client->clientmgr->lwctx, &req);

	ns_lwdclient_errorpktsend(client, LWRES_R_FAILURE);
}
