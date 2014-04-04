/*
 * Smqueue.cpp - In-memory queue manager for Short Messages (SMS's) for OpenBTS.
 * Written by John Gilmore, July 2009.
 *
 * Copyright 2009, 2014 Free Software Foundation, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * See the COPYING file in the main directory for details.
 */

#include "smqueue.h"
#include "smnet.h"
#include "smsc.h"
#include <time.h>
#include <osipparser2/osip_message.h>	/* from osipparser2 */
#include <iostream>
#include <fstream>
#include <cstdlib>			// strtol
#include <errno.h>
#include <sys/types.h>          // open
#include <sys/stat.h>           // open
#include <fcntl.h>          // open
#include <ctype.h>		// isdigit

#undef WARNING

#include <Globals.h>
#include <NodeManager.h>
#include <Logger.h>
#include <Timeval.h>

#include "QueuedMsgHdrs.h"
#include "SmqMessageHandler.h"
#include "SmqTest.h"

using namespace std;

using namespace SMqueue;


/* The global config table. */
// DAB
ConfigurationKeyMap getConfigurationKeys();
ConfigurationTable gConfig("/etc/OpenBTS/smqueue.db", "smqueue", getConfigurationKeys());

// Global Smqueue object
SMqueue::SMq smq;  // smq defined

/* The global CDR file. */
FILE * gCDRFile = NULL;

/** The remote node manager. */ 
NodeManager gNodeManager;

/** rate limiting timer */
Timeval spacingTimer;

/* We try to centralize most of the timeout values into this table.
   Occasionally the code might do something different where it knows
   better, but most state transitions just use this table to set the
   timeout that will fire if nothing happens in the new state for X
   seconds. */

/* Timeouts when going from NO_STATE into each subsequent state
 * Timeouts are in MS
 *  */
#define NT	3000*1000	/* msseconds = 50 minutes - "No Timeout" - for states
			   where we will only time out if something is really
			   broken. Changed to MS */
#define RT	300*1000	/* msseconds = 5 minutes - "Re Try" - for state
			   transitions where we're starting over from
			   scratch due to some error. Changed to MS */
#define TT	60*1000      /*  60 seconds the amount of time we add to a transaction
			    when we get a 100 TRYING message  Changed to MS */

/* Timeout when moving from this state to new state:
 NS  IS  RF  AF   WD  RD  AD   WS  RS  AS   WM  RM  AM   DM   WR  RH  AR  */
int timeouts_NO_STATE[STATE_MAX_PLUS_ONE] = {
 NT,  0,  0, NT,  NT,  0, NT,  NT,  0, NT,  NT,  0, NT,   0,  NT, NT, NT,};
int timeouts_INITIAL_STATE[STATE_MAX_PLUS_ONE] = {
  0,  0,  0, NT,  NT, NT, NT,  NT, NT, NT,  NT,  0, NT,   0,  NT, NT, NT,};
int timeouts_REQUEST_FROM_ADDRESS_LOOKUP[STATE_MAX_PLUS_ONE] = {
  0, NT, 10, 10,  NT,  0, NT,  NT, NT, NT,  NT, NT, NT,   0,   1,  0, NT,};
int timeouts_ASKED_FOR_FROM_ADDRESS_LOOKUP[STATE_MAX_PLUS_ONE] = {
  0, NT, 60, NT,  NT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,  NT, NT, NT,};
int timeouts_AWAITING_TRY_DESTINATION_IMSI[STATE_MAX_PLUS_ONE] = {
  0, NT, RT, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,  NT, NT, NT,};
int timeouts_REQUEST_DESTINATION_IMSI[STATE_MAX_PLUS_ONE] = {  // 5
  0, NT, RT, NT,  RT, NT, NT,  NT,  0, NT,  NT, NT, NT,   0,  NT, NT, NT,};
int timeouts_ASKED_FOR_DESTINATION_IMSI[STATE_MAX_PLUS_ONE] = {
  0, NT, RT, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,  NT, NT, NT,};
int timeouts_AWAITING_TRY_DESTINATION_SIPURL[STATE_MAX_PLUS_ONE] = {
  0, NT, RT, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,  NT, NT, NT,};
int timeouts_REQUEST_DESTINATION_SIPURL[STATE_MAX_PLUS_ONE] = {
  0, NT, RT, NT,  RT, NT, NT,  NT, NT, NT,  NT, 0, NT,  0,  NT, NT, NT,};
int timeouts_ASKED_FOR_DESTINATION_SIPURL[STATE_MAX_PLUS_ONE] = {  // 8
  0, NT, RT, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,  NT, NT, NT,};
int timeouts_AWAITING_TRY_MSG_DELIVERY[STATE_MAX_PLUS_ONE] = {
  0, NT, RT, NT,  RT, NT, NT,  NT, NT, NT,  75*1000,  0, NT,   0,  NT, NT, NT,};
int timeouts_REQUEST_MSG_DELIVERY[STATE_MAX_PLUS_ONE] = { // 10
//  0, NT, RT, NT,  RT, NT, NT,  NT, 75*1000, NT,  75*1000, 75, 15,   0,  NT, NT, NT,};
  0, NT, RT, NT,  RT, NT, NT,  NT,  15*1000, NT,  75*1000, 75*1000, 15*1000,   0,  NT, NT, NT,};
int timeouts_ASKED_FOR_MSG_DELIVERY[STATE_MAX_PLUS_ONE] = { // 11
  0, NT, RT, NT,  NT, NT, NT,  NT, NT, NT,  60*1000, 10*1000, TT,   0,  NT, NT, NT,};  // changed 11 -> 12 from NT to TT
int timeouts_DELETE_ME_STATE[STATE_MAX_PLUS_ONE] = { // 12
  0,  0,  0,  0,   0,  0,  0,   0,  0,  0,   0,  0,  0,   0,   0,  0,  0,};
int timeouts_AWAITING_REGISTER_HANDSET[STATE_MAX_PLUS_ONE] = {
  0, NT,  0, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,   1*1000,  0, NT,};
int timeouts_REGISTER_HANDSET[STATE_MAX_PLUS_ONE] = {
  0, NT,  0, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,   1*1000,  1*1000,  2*1000,};
int timeouts_ASKED_TO_REGISTER_HANDSET[STATE_MAX_PLUS_ONE] = {
  0, NT,  0, NT,  RT, NT, NT,  NT, NT, NT,  NT, NT, NT,   0,   1*1000,  1*1000, 10*1000,};
/* Timeout when moving from this state to new state:
 NS  IS  RF  AF   WD  RD  AD   WS  RS  AS   WM  RM  AM   DM   WR  RH  AR  */

#undef NT	/* No longer needed */
#undef RT

/* Index to all timeouts.  Keep in order!  */
int (*SMqueue::timeouts[STATE_MAX_PLUS_ONE])[STATE_MAX_PLUS_ONE] = {
	&timeouts_NO_STATE,
	&timeouts_INITIAL_STATE,
	&timeouts_REQUEST_FROM_ADDRESS_LOOKUP,
	&timeouts_ASKED_FOR_FROM_ADDRESS_LOOKUP, // 3

	&timeouts_AWAITING_TRY_DESTINATION_IMSI,
	&timeouts_REQUEST_DESTINATION_IMSI,
	&timeouts_ASKED_FOR_DESTINATION_IMSI,

	&timeouts_AWAITING_TRY_DESTINATION_SIPURL, //
	&timeouts_REQUEST_DESTINATION_SIPURL,  // 8
	&timeouts_ASKED_FOR_DESTINATION_SIPURL,

	&timeouts_AWAITING_TRY_MSG_DELIVERY,
	&timeouts_REQUEST_MSG_DELIVERY, //11
	&timeouts_ASKED_FOR_MSG_DELIVERY,

	&timeouts_DELETE_ME_STATE,

	&timeouts_AWAITING_REGISTER_HANDSET,
	&timeouts_REGISTER_HANDSET,
	&timeouts_ASKED_TO_REGISTER_HANDSET,
};


string SMqueue::sm_state_strings[STATE_MAX_PLUS_ONE] = {
	"No State",
	"Initial State",
	"Request From-Address Lookup",
	"Asked for From-Address",
	"Awaiting Try Destination IMSI",
	"Request Destination IMSI",
	"Asked for Destination IMSI",
	"Awaiting Try Destination SIP URL",
	"Request Destination SIP URL",
	"Asked for Destination SIP URL",
	"Awaiting Try Message Delivery",
	"Request Message Delivery",
	"Asked for Message Delivery",
	"Delete Me",
	"Awaiting Register Handset",
	"Register Handset",
	"Asked to Register Handset",
};




string SMqueue::sm_state_string(enum sm_state astate)
{
	if (astate < STATE_MAX_PLUS_ONE)
		return sm_state_strings[astate];
	else	return "Invalid State Number";
}

/* Global variables */
bool SMqueue::osip_initialized = false;	// Have we called lib's initializer?
struct osip *SMqueue::osipptr = NULL;	// Ptr to struct sorta used by library
const char *short_msg_pending::smp_my_ipaddress = NULL;	// Accessible copy
const char *short_msg_pending::smp_my_2nd_ipaddress = NULL;	// Accessible copy

bool print_as_we_validate = false;      // Debugging

/*
 * Associative map between target phone numbers (short codes) and
 * function pointers that implement those numbers.
 */
short_code_map_t short_code_map;  // *********** Defined here does not change after startup

/* Release memory from osip library */
void
osip_mem_release()
{
	if (osip_initialized) {
		osip_release (SMqueue::osipptr);
		SMqueue::osipptr = NULL;
		SMqueue::osip_initialized = false;
	}
}


/* ==== FIXME KLUDGE ====
 * Table of IMSIs and phone numbers, for translation.
 * This is only for test-bench use.  Real life uses the Home Location
 * Register (../HLR), currently implemented via Asterisk.
 */
static
struct imsi_phone { char imsi[4+15+1]; char phone[1+15+1]; } imsi_phone[] = {
//	{"IMSI310260550682564", "100"}, 	/* Siemens A52 */
	{"IMSI666410186585295", "+17074700741"},	/* Nokia 8890 */
	{"IMSI777100223456161", "+17074700746"},	/* Palm Treo */
	{{0}, {0}}
};



/* Functions */
enum sm_state
handle_sms_message(short_msg_pending *qmsg);

bool
check_to_user(char *user);

char *
read_short_msg_from_file(char *file);

/*
 * Utility functions
 */


// C++ wants you to use <string>.  Fuck that.  When we want dynamically
// allocated char *'s, then we'll use them.  strdup uses malloc, tho, so
// we need a "new/delete" oriented strdup.  Ok, simple.
char *
SMqueue::new_strdup(const char *orig)
{
	int len = strlen(orig);
	char *result = new char[len+1];
	strncpy(result, orig, len+1);
	return result;
}




void
increase_acked_msg_timeout(short_msg_pending *msg)
{
	time_t timeout = SMq::INCREASEACKEDMSGTMOMS;

	if (gConfig.defines("SIP.Timeout.ACKedMessageResend")) {
		timeout = gConfig.getNum("SIP.Timeout.ACKedMessageResend");
	}

	msg->set_state(msg->state, msg->msgettime() + timeout);
}


/*
 * We have received a SIP response message (with a status code like 200,
 * a message like "Okay", and various other fields).  Find the outgoing
 * message that matches this response, and do the right thing.  The response
 * message has already been parsed and validated.
 *
 * If the response says the message is delivered, delete both the response
 * and the message.
 *
 * In general, delete the response so it won't stay at the front of the
 * queue.
 */
void
SMq::handle_response(short_msg_p_list::iterator qmsgit)
{
	short_msg_pending *qmsg = &*qmsgit;
	short_msg_p_list::iterator sent_msg;
	short_msg_p_list resplist;

	// First, remove this response message from the queue.  That way,
	// when we search the queue, we won't find OURSELF.  We also don't
	// want the response hanging around in the queue anyway.

// Lock
	lockSortedList();
	resplist.splice(resplist.begin(), time_sorted_list, qmsgit);
	// We'll delete the list element on our way out of this function as
	// resplist goes out of scope.

	// figure out what message we're responding to.
	if (!find_queued_msg_by_tag(sent_msg, qmsg->qtag, qmsg->qtaghash)) {
		// No message in queue.
		LOG(NOTICE) << "Couldn't find message for response tag '"
		     << qmsg->qtag << "'; response is:" << endl
		     << qmsg->text;
		// no big problem, just ignore it.
		unlockSortedList();
		return;
	}

// Keep message locked so it won't get deleted while being modified

	// Check what kind of response we got, based on its status code.
	// FIXME, logfile output would be useful here.
	LOG(NOTICE) << "Got " << qmsg->parsed->status_code
	     << " response for sent msg '" << sent_msg->qtag << "' in state "
	     << sent_msg->state;

	if (   sent_msg->state != ASKED_FOR_MSG_DELIVERY
	    && sent_msg->state != REQUEST_MSG_DELIVERY
	    && sent_msg->state != REQUEST_DESTINATION_SIPURL
	    && sent_msg->state != AWAITING_TRY_MSG_DELIVERY) {
		LOG(ERR) << "*** That's not a pleasant state. ***";
		// Don't abort here -- if a msg gets forked, one fork
		// gets a redirect/reject, this puts us back into a lookup
		// state, then we get a response from another fork, don't die.
		// Just quietly keep going.
	}

	switch (qmsg->parsed->status_code / 100) {
	case 1: // 1xx -- interim response
		//While a 100 doesn't mean anything really,
		//we should increase the timeout because
		//we know the network worked
		increase_acked_msg_timeout(&(*sent_msg));
		break;

	case 2:	// 2xx -- success.
		// Done!  Extract original msg from queue, and toss it.
		sent_msg->parse();
		if (sent_msg->parsed &&
		    sent_msg->parsed->sip_method &&
		    0 == strcmp("REGISTER", sent_msg->parsed->sip_method)) {
			// This was a SIP REGISTER message, so we need
			// to free up the original SMS Shortcode message that
			// started the registration process.
			short_msg_p_list::iterator oldsms;

			if (!get_link(oldsms, sent_msg)) {
				LOG(NOTICE) << "Can't find SMS message for newly "
					"registered handset, linktag '"
				     << qmsg->linktag << "'.";
				// Assume this was a dup after a retry of
				// the REGISTER message -- thus this is a
				// second REGISTER response, after we already
				// handled the original reg SMS.  Ignore it.
			} else if (
			       oldsms->state == ASKED_TO_REGISTER_HANDSET
			    || oldsms->state == REGISTER_HANDSET
			    || oldsms->state == AWAITING_REGISTER_HANDSET) {
				// Go back in and rerun the SMS message,
				// now that we think we can reply to it.
				// Special code in registration processing
				// will notice it's a re-reg and just reply
				// with a welcome message.
				oldsms->set_state(INITIAL_STATE);
			} else {
				// Orig SMS exists, but not in a normal state.
				// Assume that the original SMS is in a
				// retry loop somewhere.  Ignore it.
				// Eventually it'll notice...?  FIXME.
			}
		}
		if (sent_msg->parsed &&
		    sent_msg->parsed->sip_method &&
		    0 == strcmp("MESSAGE", sent_msg->parsed->sip_method)) {
			sent_msg->write_cdr(my_hlr);
		}

		// Whether a response to a REGISTER or a MESSAGE, delete
		// the datagram that we sent, which has been responded to.
		LOG(INFO) << "Deleting sent message.";
		resplist.splice(resplist.begin(),
				time_sorted_list, sent_msg);
		resplist.pop_front();	// pop and delete the sent_msg.

		// FIXME, consider breaking loose any other messages for
		// the same destination now.
		break;

	case 4: // 4xx -- failure by client
		// 486 Busy - means we have to retry later.
		// 480 Temporarily Unavailable - means we have to retry later.
		// Most likely this means that a subscriber left network coverage
		// without unregistering from the network. Try again later.
		// Eventually we should have a hook for their return
		if (qmsg->parsed->status_code == 480 || qmsg->parsed->status_code == 486){
			increase_acked_msg_timeout(&(*sent_msg));
		}
		// Other 4xx codes mean the original message was bad.  Bounce it.
		else {
			ostringstream errmsg;
			errmsg << qmsg->parsed->status_code << " "
			       << qmsg->parsed->reason_phrase;
			sent_msg->set_state(
			    bounce_message((&*sent_msg), errmsg.str().c_str()));
		}
		break;

	case 5: // 5xx -- failure by server (poss. congestion)
		// FIXME, perhaps we should change its timeout value??  Shorter
		// or longer???
		LOG(WARNING) << "CONGESTION at OpenBTS\?\?!";
		increase_acked_msg_timeout(&(*sent_msg));
		break;

	case 3: // 3xx -- message ngConfigeeds redirection
	case 6: // 6xx -- message rejected (by this destination).
		// Try going back through looking up the destination again.
		sent_msg->set_state(REQUEST_DESTINATION_IMSI);
		break;

	default:
		LOG(WARNING) << "Unknown status code in SIP response.";
		break;
	}

//Unlock
	unlockSortedList();

	// On exit, we delete the response message we've been examining
	// when resplist goes out of scope.
} // handle_response

/*
 * Find a queued message, based on its tag value.  Return an iterator
 * that can be used to remove it from the list if desired.
 */
bool
SMq::find_queued_msg_by_tag(short_msg_p_list::iterator &mymsg,
			    const char *tag, int taghash)
{
	lockSortedList();
	short_msg_p_list::iterator x;
	for (x = time_sorted_list.begin(); x != time_sorted_list.end(); x++) {
		if (taghash == x->qtaghash && !strcmp (tag, x->qtag)) {
			mymsg = x;
			unlockSortedList();
		    return true;
		}
	}
	unlockSortedList();
	return false;
}

// Same, but figure out the taghash manually.
bool
SMq::find_queued_msg_by_tag(short_msg_p_list::iterator &mymsg,
			    const char *tag)
{
	return find_queued_msg_by_tag(mymsg, tag, mymsg->taghash_of(tag));
}


static bool relaxed_verify_relay(osip_list_t *vias, const char *host, const char *port)
{
	bool from_relay = false;

	if (vias && host && port) {
		int size = osip_list_size(vias);

		for (int i = 0; i < size; i++) {
			osip_via_t *via = (osip_via_t *)osip_list_get(vias, i);
			const char *via_host = via_get_host(via);
			const char *via_port = via_get_port(via);

			if (via_host && via_port &&
			    !strcasecmp(host, via_host) && !strcasecmp(port, via_port)) {
				from_relay = true;
				break;
			}
		}
	}

	return from_relay;
}

/*
 * Validate a short_msg by parsing it and then checking the parse
 * to make sure it has *everything* we need to process and forward it.
 * **ALL** validity checks on incoming short_msg's are centralized here.
 * Thus the rest of the code doesn't need to muck around checking
 * every possible thing -- it can just look at the parts of the message
 * that it needs to.
 *
 * Also, if the qtag hasn't been set yet, set it, based on the message's
 * fields.
 *
 * If we reject a message, return the SIP response code that we should
 * return to the sender, diagnosing the problem.  If the message is OK,
 * result is zero.
 * Return
 * 	0 Is okay
 * 	Error code if failed
 *
 */
int
short_msg_pending::validate_short_msg(SMq *manager, bool should_early_check)
{
	osip_message_t *p;
	__node_t *plist;
	osip_generic_param_t *param;
	char *user;
	size_t clen;
	char *fromtag;
	char *endptr;

    if (print_as_we_validate) {    // debugging
        make_text_valid();
        LOG(DEBUG) << "MSG = " << this->text;
    }

	if (!parsed_is_valid) {
		if (!parse()) {
			LOG(DEBUG) << "Invalid parse";
			return 400;
		}
	}

	/* The message has been parsed.  Now check that we like its
	   structure and contents.  */

	p = parsed;
	if (!p) {
		LOG(DEBUG) << "Parse is NULL";
		return 400;
	}

	if (!p->sip_version || 0 != strcmp("SIP/2.0", p->sip_version)) {
		LOG(DEBUG) << "Invalid SIP version";
		return 400;
	}
	// If it's an ack, check some things.  If it's a message,
	// check others.
	if (MSG_IS_RESPONSE(p)) {
		// It's an ack.
		if (p->status_code < 0
		    || !p->reason_phrase) {
			LOG(DEBUG) << "Status code invalid or no reason";
			return 400;
		}
		clen = 0;
		// Leave this test in for responses
		if (p->content_length && p->content_length->value) {
			errno = 0;
			clen = strtol(p->content_length->value, &endptr, 10);
			if (*endptr != '\0' || errno != 0) {
				LOG(DEBUG) << "Invalid Content Length";
				return 400;
			}
		} else {
				LOG(DEBUG) << "Content Length zero";
		}
		if (clen != 0) {
			LOG(DEBUG) << "ACK has a content length";
			return 400;	// Acks have no content!
		}

		if (p->bodies.nb_elt != 0) {
			LOG(DEBUG) << "ACK has a body";
			return 400;	// Acks have no bodies!
		}
	} else {
		// It's a message.
		if (!p->req_uri || !p->req_uri->scheme) {
			LOG(DEBUG) << "No scheme or uri";
			return 400;
		}
		if (0 != strcmp("sip", p->req_uri->scheme)) {
			LOG(DEBUG) << "Not SIP scheme";
			return 416;
		}
		if (!check_host_port(p->req_uri->host,p->req_uri->port)) {
			LOG(DEBUG) << "Host port check failed";
			return 484;
		}
		if (!p->sip_method) {
			LOG(DEBUG) << "SIP method not set";
			return 405;
		}
		if (0 == strcmp("MESSAGE", p->sip_method)) {
			user = p->req_uri->username;
			if (!user)
				return 484;
			if (!check_to_user(user))  // Check user does nothing
				return 484;
			// FIXME, URL versus To: might have different username
			// requirements?

			// FIXME, add support for more Content-Type's.
			if (!p->content_type || !p->content_type->type
			    || !p->content_type->subtype
			    || !( (0 == strcmp("text", p->content_type->type)
			          && 0 == strcmp("plain", p->content_type->subtype))
			        ||(0 == strcmp("application", p->content_type->type)
			          && 0 == strcmp("vnd.3gpp.sms", p->content_type->subtype))
					  )
			    ) {
				LOG(DEBUG) << "Content type not supported";
				return 415;
			}

			int len;
			if (p->content_length && p->content_length->value && ((len=strtol(p->content_length->value, NULL, 10)) > 0) ) {
				//If length is greater than zero check this
				// Bad if any of these are true nb_elt!=1,  node=0,   node->next!=0,   node->element=0
				if (p->bodies.nb_elt != 1  // p->bodies.nb_elt should always be one
					|| 0 == p->bodies.node
					|| 0 != p->bodies.node->next
					|| 0 == p->bodies.node->element) {
					LOG(DEBUG) << "Message entity-body too large";
					return 413;	// Request entity-body too large
				}
			} else {
				LOG(DEBUG) << "Content length is zero";
			}

			if (!p->cseq || !p->cseq->method
			    || 0 != strcmp("MESSAGE", p->cseq->method)
			    || !p->cseq->number) {  // FIXME, validate number??
				LOG(DEBUG) << "Invalid sequence number";
				return 400;
			}

		} else if (0 == strcmp("REGISTER", p->sip_method)) {
			// Null username is OK in register message.
			// Null content-type is OK.
			// Null message body also OK.
			if (!p->cseq || !p->cseq->method
			    || 0 != strcmp("REGISTER", p->cseq->method)
			    || !p->cseq->number) {  // FIXME, validate number??
				LOG(DEBUG) << "Invalid REGISTER";
				return 400;
			}

		} else {
			LOG(DEBUG) << "Unknown SIP datagram";
			return 405;	// Unknown SIP datagram type
		}
	}

	// accepts - no restrictions
	// accept_encodings - no restrictions?
	// accept_langauges - no restrictions?
	// alert_infos - no restrictions?
	// allows - no restrictions?
	// authentication_infos - no restrictions?
	// authorizations - no restrictions
	if (!p->call_id) {
		LOG(DEBUG) << "No call-id";
		return 400;
	}

	// Remove content length check
	// According to rfc3261 length can be 0 or greater  svg 03/20/2014

	// From address needs to exist but a tag in the from address is optional
	if (!p->from || !p->from->url
	    //|| p->from->gen_params.nb_elt < 1  // Message tag is not required
	    //|| !p->from->gen_params.node		 // Message tag is not required SVG 03/12/14
			)
	{
		LOG(DEBUG) << "Invalid from address in header from " << p->from << " fromurl " << p->from->url
				   << " nb.elt " << p->from->gen_params.nb_elt << " params.node " << p->from->gen_params.node;
		return 400;
	}

	fromtag = NULL;
	plist = (__node_t *) p->from->gen_params.node;
	if (plist) {
		do {
			param = (osip_generic_param_t *) plist->element;
			if (param) {
				LOG(DEBUG) << "Param " << param->gname << "=" << param->gvalue;
				if (!strcmp("tag", param->gname)) {
					LOG(DEBUG) << "Got from tag";
					fromtag = param->gvalue;
					break;  // Only need one
				}
				plist = (__node_t *)plist->next;
			} else {
				LOG(DEBUG) << "No element in list";  // This is valid
				break;
			}
		} while (plist); // while
		if (!fromtag) {
			LOG(DEBUG) << "No from tag";  // This is valid
		}
	}
	else {
		LOG(DEBUG) << "No node list element";  // Will not cause a problem. Not sure is this is normal
	}

	if (!p->mime_version) {
		;					// No mime version, OK
	} else if (!p->mime_version->value		// Version 1.0 is OK.
		   || 0 != strcmp("1.0", p->mime_version->value)) {
		LOG(DEBUG) << "Wrong mime version";
		return 415;				// Any other is NOPE.
	}
	// proxy_authenticates - no restrictions
	// proxy_authentication_infos -- no restriction
	// record_routes -- no restriction
	// require -- RFC 3261 sec 20.32: If received, reject this msg!
	//    Unfortunately, the parser doesn't even seem to recognize it!
	// routes -- no restrictions
	if (!p->to || !p->to->url || !p->to->url->scheme
	    || 0 != strcmp("sip", p->to->url->scheme)
	    || !check_host_port(p->to->url->host, p->to->url->port)
	    || !p->to->url->username
#if 0
		// Asterisk returns a tag on the To: line of its SIP REGISTER
		// responses.  This violates the RFC but we'd better allow it.
		// (We don't really care -- we just ignore it anyway.)
		// A from tag is not required
		// || p->to->gen_params.nb_elt != 0
#endif
                    ) {	// no tag field allowed; see
		LOG(DEBUG) << "Invalid To header";	// RFC 3261 sec 8.1.1.2
		return 400;
	}
	user = p->to->url->username;
	if (!check_to_user(user)) {
		LOG(DEBUG) << "Invalid To user";
		return 400;
	}
	// We need to see if this is a message form the relay. If it is, we can process a user lookup here
	// BUT ONLY IF the message is not a response (ACK) AND MUST BE a SIP MESSAGE.
	if (should_early_check && !MSG_IS_RESPONSE(p) && (0 == strcmp("MESSAGE", p->sip_method))
		&& (manager->my_network.msg_is_from_relay(srcaddr, srcaddrlen,
		manager->global_relay.c_str(), manager->global_relay_port.c_str()) ||
		(gConfig.getBool("SIP.GlobalRelay.RelaxedVerify") &&
		 relaxed_verify_relay(&p->vias, manager->global_relay.c_str(), manager->global_relay_port.c_str())
		))) {
		// We cannot deliver the message since we cannot resolve the TO
		if (!manager->to_is_deliverable(user)) {
			LOG(DEBUG) << "To address not deliverable";
			return 404;
		}
		// We need to reject the message, because we cannot resolve the FROM
		// TODO: Don't do this. From coming in through the relay is probably not resolvable.
		/*if (!manager->from_is_deliverable(p->from->url->username))
			return 403;*/
		from_relay = true;
		LOG(DEBUG) << "In bound message from " << p->from->url->username << " to " << user << " is from relay";
	}
	// The spec wants Via: line even on acks, but do we care?  FIXME.
	// if (p->vias.nb_elt < 1 || false) // ... FIXME )
	//	return 400;
	// www_authenticates - no restrictions
	// headers - ???
	// message_property ??? FIXME
	// message ??? FIXME
	// message_length ??? FIXME
	// application_data - no restrictions

	// Set the qtag from the parsed fields, if it hasn't been set yet.
	return (set_qtag());
}


// Set the qtag from the parsed fields, if it hasn't been set yet.
// Whenever we change any of these fields, we have to recalculate
// the qtag.
// FIXME, we assume that the CSEQ, Call-ID, and From tag all are
// components that, in combination, identify the message uniquely.
// (pat) There should not be a from-tag in a MESSAGE; the from- and to-tags are used only for INVITE.
// FIXME!  The spec is unpleasantly unclear about this.
// (pat) RFC3261 17.1.3 is clear: we are supposed to use either the via-branch or the call-id+cseq to identify the message.
// Result is 0 for success, or 3-digit integer error code if error.
// FIXME!  The Call-ID changes each time we re-send an SMS.
//  When we get a 200 "Delivered" message for ANY of those
//  different Call-ID's, we should accept that the msg was
//  delivered and delete it.  Therefore, remove the Call-ID
//  from the qtag.
int
short_msg_pending::set_qtag()
{
	osip_message_t *p;
	__node_t *plist;
	osip_generic_param_t *param;
	char *fromtag;
	int error;

	//LOG(DEBUG) << "Enter parse";
	if (!parsed_is_valid) {
		if (!parse()) {
			LOG(DEBUG) << "Parse failed in setqtag";
		return 400;
		}
	}
	p = parsed;

	if (!p->from) {
		LOG(DEBUG) << "No from address in setqtag";
		return 400;
	}
	plist = (__node_t *) p->from->gen_params.node;
	if (!plist) {
		LOG(DEBUG) << "No node in param list";
		return 400;
	}

	// Handle from tag which is optional
	fromtag = NULL;
	plist = (__node_t *) p->from->gen_params.node;
	if (plist) {
		do {
			param = (osip_generic_param_t *) plist->element;
			if (param) {
				LOG(DEBUG) << "Param " << param->gname << "=" << param->gvalue;
				if (!strcmp("tag", param->gname)) {
					LOG(DEBUG) << "Got from tag";
					fromtag = param->gvalue;
					break;  // Only need one
				}
				plist = (__node_t *)plist->next;
			} else {
				LOG(DEBUG) << "No element in list";  // This is valid
				break;
			}
		} while (plist); // while
		if (!fromtag) {
			LOG(DEBUG) << "No from tag";  // This is valid
		}
	}
	else {
		LOG(DEBUG) << "No node list element";  // Will not cause a problem. Not sure is this is normal
	}

	if (!p->call_id) {
		LOG(DEBUG) << "No callID";
		return 401;
	}

	if (!p->cseq) {
		LOG(DEBUG) << "No sequence number";
		return 402;
	}

	delete [] qtag;		// slag the old one, if any.

	int len = strlen(p->cseq->number) + 2	// crlf or --
#ifdef USE_CALL_ID_TAG
		+ strlen(p->call_id->number) + 1 // @
		+ strlen(p->call_id->host) + 2	// crlf or --
#endif
		+ strlen(fromtag) + 1;		// null at end
	qtag = new char[len];
	// There's probably some fancy C++ way to do this.  FIXME.
	strcpy(qtag, p->cseq->number);
	strcat(qtag, "--");
#ifdef USE_CALL_ID_TAG
	strcat(qtag, p->call_id->number);
	strcat(qtag, "@");
	strcat(qtag, p->call_id->host);
	strcat(qtag, "--");
#endif
	strcat(qtag, fromtag);
	// Check the length calculation, abort if bad.
	if (qtag[len-1] != '\0'
	 || qtag[len-2] == '\0') {
		LOG(DEBUG) << "qtag length check failed"; // Not sure why this is being done svg
		return 400;
	}

	// Set the taghash too.
	// FIXME, if we set this with a good hash function,
	// our linear searches will run much faster, avoiding
	// almost all strcmp's.  For now, punt easy.
	qtaghash = taghash_of(qtag);

	return 0;
}

/*
 * Hash a tag value for fast searches.
 */
// FIXME, if we set this with a good hash function,
// our linear searches will run much faster, avoiding
// almost all strcmp's.  For now, punt easy.
int
short_msg_pending::taghash_of (const char *fromtag)
{
	return fromtag[0];
}

/* Check the host and port number specified.
 * Currently, be conservative and only take localhost refs.
 * We know FIXME that this will have to be expanded...
 */
bool
short_msg_pending::check_host_port (char *host, char *port)
{
	static int warn_once;

	if (!host)
		return false;
	if (!strcmp ("localhost", host) ||
	    !strcmp ("127.0.0.1", host) ||
	    (smp_my_ipaddress && !strcmp (smp_my_ipaddress, host)) ||
	    (smp_my_2nd_ipaddress && !strcmp (smp_my_2nd_ipaddress, host))) {
		;
	} else {
		if (0 == warn_once++) {
			LOG(NOTICE) << "Accepting SIP Message from " << host <<
			        " for SMS delivery, even though it's not "
				"from localhost.";
		}
		// In theory we should be *routing* it to where it
		// specifies -- but the address is *probably* us, and
		// we have no way to tell what our own address is,
		// since (in a real configuration) we're behind a NAT
		// firewall at an address that never appears in ifconfig,
		// for example.  Must assume it's OK -- for now.  FIXME.
		return true;
	}
	// port can either be specified or not.
	// we could check whether it's all digits...  FIXME

	return true;
}

void short_msg_pending::write_cdr(SubscriberRegistry& hlr) const
{
	char * from = parsed->from->url->username;
	char * dest = parsed->to->url->username;
	time_t now = time(NULL);  // Need real time for CDR

	if (gCDRFile) {
		char * user = hlr.getIMSI2(from); // I am not a fan of this hlr call here. Probably a decent performance hit...
		// source, sourceIMSI, dest, date
		fprintf(gCDRFile,"%s,%s,%s,%s", from, user, dest, ctime(&now));
		fflush(gCDRFile);
	} else {
		LOG(ALERT) << "CDR file at " << gConfig.getStr("CDRFile").c_str() << " could not be created or opened!";
	}
}

/*
 * Check the username in the To: field, perhaps in the From: fiend,
 * perhaps in the URI in the MESSAGE line at the top...
 */
bool
check_to_user (char *user)
{
	// For now, don't check -- but port some checks up from the
	// code below in lookup_from_address.  FIXME.
	return true;
}

/*
	Main state machine for Short Message processing
	Called from SmqWriter on a periodic basis
	Processes message from the queue
*/


void SMq::process_timeout()
{
	time_t now = msgettime();
	short_msg_p_list::iterator qmsg;
	enum sm_state newstate;
	int msSMSRateLimit;

	lockSortedList();
	//LOG(DEBUG) << "Begin process_timeout";
	/* When we modify a timestamp below (in the set_state function),
	   we re-queue the message within the queue, so we have to
	   restart the iterator every time around the loop.   In effect,
	   we're always looking at the top thing on the list (thus the
	   earliest one in time).  */
	
		bool empty = false;
		qmsg = time_sorted_list.begin();
		if (qmsg == time_sorted_list.end())
			empty = true;
		if (empty) {
			unlockSortedList();
			//LOG(DEBUG) << "Message queue is empty";
			return;			/* Empty queue */
		}

		//LOG(DEBUG) << "Queue size " << time_sorted_list.size();
		if (qmsg->next_action_time > now) {
			unlockSortedList();
			//LOG(DEBUG) << "Not time to processs message";
			return;			/* Wait until later to do more */
		}

		// Got message to process from queue
		LOG(DEBUG) << "Process message from queue" << time_sorted_list.size();
#undef DEBUG_Q
#ifdef DEBUG_Q
	LOG(DEBUG) << "===== Top of process timeout";
	debug_dump();
	LOG(DEBUG) << "===== End of queue =====";
#else
	char timebuf[26+/*slop*/4];	//
	time_t now2 = time(NULL);  // Using actual time
	ctime_r(&now2, timebuf);
	timebuf[19] = '\0';	// Leave out space, year and newline

	LOG(INFO) << "=== " << timebuf+4 << " "
	 << time_sorted_list.size() << " queued; "
		 << sm_state_string(qmsg->state)
		 << " for " << qmsg->qtag;

#endif
		LOG(DEBUG) << "Processing message in queue, state "  << sm_state_string(qmsg->state);
		switch (qmsg->state) {
		case INITIAL_STATE:
			// This is the initial state in which a message
			// enters the system.  Here, the message could be
			// a SIP response as well as a SIP MESSAGE -- or
			// something we can't process like a SIP REGISTER
			// or garbage.  Well, actually, garbage was rejected
			// earlier before queue insertion.

			// From this point onward, we're going to assume
			// that the message has valid, reasonable headers
			// and contents for our purposes.  Centralize all
			// that checking in validate_short_msg().

			if (MSG_IS_REQUEST(qmsg->parsed)) {
				// It's a MESSAGE or invalid REGISTER.
				// We support only MESSAGE here.
				if (0 != strcmp(qmsg->parsed->sip_method, "MESSAGE")) {
					LOG(WARNING) << "Invalid incoming SIP message, method is "
					          << qmsg->parsed->sip_method;
					newstate = NO_STATE;
				} else {
					// Real messages go here
					// Check for short-code and handle it.
					// If handle_short_code() returns true, it sets newstate
					// on its own
					if (!handle_short_code(short_code_map, qmsg, newstate)) {
						// For non-special messages, look up who they're from.
						newstate = REQUEST_FROM_ADDRESS_LOOKUP;
						//newstate = verify_funds(qmsg);
					}
				}
				LOG(DEBUG) << "State from handle_short_code " << sm_state_string(qmsg->state);
				set_state(qmsg, newstate);
				break;

			} else { // It's a RESPONSE.

				handle_response(qmsg);
				// The RESPONSE has been deleted in handle_response().
				// We go back to the top of the loop.
				break;
			}
			break;

		case NO_STATE:
			// Messages in NO_STATE have errors in them.
			// Dump it to the log, and delete it, so the queue
			// won't build up.
			qmsg->make_text_valid();
			LOG(NOTICE) << "== This message had an error and is being deleted:"
			     << endl << "MSG = " << qmsg->text;
			// Fall thru into DELETE_ME_STATE!
		case DELETE_ME_STATE: {
			// This message should quietly go away.

			short_msg_p_list temp;
			// Extract the current sm from the time_sorted_list

			temp.splice(temp.begin(), time_sorted_list, qmsg);  // queue is already locked
			// When we remove it from the new "temp" list,
			// this entry will be deallocated.  qmsg still
			// points to its (dead) storage, so be careful
			// not to reference qmsg (I don't know a C++ way
			// to set it to NULL or delete it or something).
			temp.pop_front();
		    }
			break;

		default:
			LOG(ALERT) << "Message timed out with bad state "
			     << qmsg->state << " and message: " << qmsg->text;
			set_state(qmsg, INITIAL_STATE);
			// WTF? Shouldn't we proceed to NO_STATE aka "error state"?
		/* NO BREAK */
		case REQUEST_FROM_ADDRESS_LOOKUP:
			/* Ask to translate the IMSI in the From field
			   into the phone number.  */
			newstate = lookup_from_address (&*qmsg);  // Reads from the database
			set_state(qmsg, newstate);
			break;

		case REQUEST_DESTINATION_IMSI:
			/* Ask to translate the destination phone
			   number in the Request URI into an IMSI.  */
			newstate = lookup_uri_imsi(&*qmsg); // Reads from the database
			set_state(qmsg, newstate);
			break;

		case REQUEST_DESTINATION_SIPURL:
			/* Ask to translate the IMSI in the Request URI
			   into the host/port combo to send it to.  */
			newstate = lookup_uri_hostport(&*qmsg); // Reads from the database
			set_state(qmsg, newstate);
			break;

		case AWAITING_TRY_MSG_DELIVERY:
			/* We have waited awhile and now want to try
			   delivering the message again. */
			set_state(qmsg, REQUEST_MSG_DELIVERY);
			/* No Break */
		case REQUEST_MSG_DELIVERY:
			LOG(DEBUG) << "In delivery action time " << qmsg->next_action_time;
			qmsg->retries++;
			/* We are trying to deliver to the handset now (or
			   again after congestion).  */

			// Check for short-code and handle it.
			// If handle_short_code() returns true, it sets newstate
			// on its own
			if (!pack_sms_for_delivery(qmsg))
			{
				// Error...
				LOG(ERR) << "pack_sms_for_delivery returned non 0";
				set_state(qmsg, NO_STATE);
				break;
			}

			// make sure messages eventually get discarded
			if (gConfig.getNum("SMS.MaxRetries")) {
				if (qmsg->retries > gConfig.getNum("SMS.MaxRetries")) {
					LOG(INFO) << "MaxRetries: max retries exceeded, dropping message";
					set_state(qmsg, DELETE_ME_STATE);
					break;
				}

				LOG(INFO) << "MaxRetries: trying attempt #" << qmsg->retries;
			}

			// limit messages to once-per-timeout if enabled
			msSMSRateLimit = gConfig.getNum("SMS.RateLimit") * 1000;
			if (msSMSRateLimit > 0) {
				if (msSMSRateLimit >= spacingTimer.elapsed()) {
					LOG(INFO) << "RateLimit: trying too soon, not sending yet";
					qmsg->next_action_time += msSMSRateLimit; // Should this be set timeout svgfix
					break; // Delay the message
				}
				// Go ahead and process message
				LOG(INFO) << "RateLimit: enough time has elapsed, proceeding. Remaining queue size: " << time_sorted_list.size();  // No lock okay
				spacingTimer.now();
			}

			// debug_dump();
			// Only print delivering msg if delivering to non-
			// localhost.
			LOG(INFO) << "Delivering '"
				     << qmsg->qtag << "' from "
				     << qmsg->parsed->from->url->username
				     << " at "
				     << qmsg->parsed->req_uri->host
				     << ":" << qmsg->parsed->req_uri->port
				     << ".";

			// FIXME, if we can't deliver the datagram we
			// just do the same thing regardless of the result.
			LOG(DEBUG) << "Before deliver set state action time " << qmsg->next_action_time;
			// Try and send datagram
			if (my_network.deliver_msg_datagram(&*qmsg))
				set_state(qmsg, ASKED_FOR_MSG_DELIVERY);   // Message sent okay
			else
				set_state(qmsg, ASKED_FOR_MSG_DELIVERY); // This makes more sense than back to REQUEST_DESTINATION_SIPURL
			LOG(DEBUG) << "After deliver set state action time " << qmsg->next_action_time;
			break;

		case ASKED_FOR_MSG_DELIVERY:
			/* We sent the message to the handset, but never
			   got back an ack.  Must wait awhile to avoid
			   flooding the network or the user with dups. */
			set_state(qmsg, AWAITING_TRY_MSG_DELIVERY);
			break;

		case AWAITING_REGISTER_HANDSET:
			/* We got a shortcode SMS which succeeded in
			   associating a phone number with this IMSI.
			   Now we have to wait for that to take effect
			   in the HLR, before the next steps. */
			// See if the IMSI maps to a caller ID yet.
			if (!ready_to_register(qmsg)) {
				// Re-up our timeout
				set_state(qmsg, AWAITING_REGISTER_HANDSET);
				break;
			}
			set_state(qmsg, REGISTER_HANDSET);
			/* No Break */
		case REGISTER_HANDSET:
			/* We got a shortcode SMS which succeeded in
			   associating a phone number with this IMSI.
			   Now we have to associate the IMSI with the
			   IP addr:port of its cell site. */
			newstate = register_handset(qmsg);  // Puts a message in the queue
			set_state(qmsg, newstate);
			break;

		case ASKED_TO_REGISTER_HANDSET:
			/* We got a shortcode SMS which succeeded in
			   associating a phone number with this IMSI.
			   We asked to assoc the IMSI with the addr:port
			   of its cell, but the HLR hasn't answered. */
			// we asked to register; if we time out, go back
			// and try again.
			set_state(qmsg, AWAITING_REGISTER_HANDSET);
			break;
		} // switch

		unlockSortedList();

		//LOG(DEBUG) << "End process_timeout";

} // SMq::process_timeout on reader thread



/* 
 * Make it possible for one message to link to another (by name).
 * We use names rather than pointers because it's independent of the
 * memory management.  Cost = a search of the queue.
 */
// Set the linktag of "newmsg" to point to oldmsg.
void
SMq::set_linktag(short_msg_p_list::iterator newmsg,
		 short_msg_p_list::iterator oldmsg)
{
	if (!oldmsg->qtag) {
		oldmsg->set_qtag();
	}

	size_t len = strlen(oldmsg->qtag);
	newmsg->linktag = new char[1+len];
	strncpy(newmsg->linktag, oldmsg->qtag, len);
	newmsg->linktag[len] = '\0';
}

// Get the other message that this message links to.
// Result is true if found, false if not.
bool
SMq::get_link(short_msg_p_list::iterator &oldmsg,
	      short_msg_p_list::iterator qmsg)
{
	char *alink;

	alink = qmsg->linktag;
	if (!alink)
		return false;
	return find_queued_msg_by_tag(oldmsg, alink);
}


/*
 * Originate half of a short message
 * Put it in the queue and start handling it, but don't actually
 * finish it or send it; return it to the caller for further mucking with.
 *
 * In particular, the caller must set the:
 *    uri
 *    From:
 *    To:
 *    Content-Type and message body, if any.
 *
 * Method is which type of SIP packet (MESSAGE, RESPONSE, etc).
 * Result is a short_msg_p_list containing one short_msg_pending, with
 * the half-initialized message in it.
 * Return
 * 	0 = Failure  need to delete message
 * 	okay = msg_p_list
 */
short_msg_p_list *
SMq::originate_half_sm(string method)
{
	short_msg_p_list *smpl;
	short_msg_pending *response;
	osip_via_t *via;
	char *temp, *p, *mycallnum;
	const char *myhost;

	smpl = new short_msg_p_list (1);
	response = &*smpl->begin();	// Here's our short_msg_pending!
	response->initialize (0, NULL, true);

	osip_message_init(&response->parsed);
	response->parsed_is_valid = true;

	if (!have_register_call_id || method != "REGISTER") {
		// If it's a MESSAGE, or if it's the first REGISTER,
		// it needs a new Call-ID.
		myhost = my_ipaddress.c_str();
		mycallnum = my_network.new_call_number();

		osip_call_id_init(&response->parsed->call_id);
		p = (char *)osip_malloc (strlen(myhost)+1);
		strcpy(p, myhost);
		osip_call_id_set_host (response->parsed->call_id, p);
		p = (char *)osip_malloc (strlen(mycallnum)+1);
		strcpy(p, mycallnum);
		osip_call_id_set_number (response->parsed->call_id, p);
		if (method == "REGISTER") {
			// Save the new call-ID for all subsequent registers
			char *my_callid;
			if (osip_call_id_to_str (response->parsed->call_id,
			                          &my_callid)) {
				LOG(DEBUG) << "Parse call ID failed can't continue"; // Can't continue
				return NULL;
			}
			register_call_id = string(my_callid);
			osip_free (my_callid);
			register_call_seq = 0;
			have_register_call_id = true;
		}
	} else if (method == "REGISTER") {
		// Copy the saved call-ID
		osip_call_id_init(&response->parsed->call_id);
		osip_call_id_parse(response->parsed->call_id,
				   register_call_id.c_str());
	}

	ostringstream cseqline;
	unsigned int cseq;
	if (method == "REGISTER") {
		cseq = ++register_call_seq;
	} else {
		cseq = my_network.new_random_number();
		cseq &= 0xFFFF;		// for short readable numbers
	}
	cseqline << cseq << " " << method;
	osip_message_set_cseq(response->parsed, cseqline.str().c_str());

	osip_message_set_method (response->parsed, osip_strdup(method.c_str()));

	osip_message_set_via(response->parsed, "SIP/2.0/UDP x 1234;branch=123");
	// FIXME, don't assume UDP, allow TCP here too.
	osip_message_get_via (response->parsed, 0, &via);
	temp = via_get_host(via);
	osip_free(temp);
	via_set_host(via, osip_strdup(my_ipaddress.c_str()));
	temp = via_get_port(via);
	osip_free(temp);
	via_set_port(via, osip_strdup(my_udp_port.c_str()));

	// We've altered the text, and the parsed version controls.
	response->parsed_was_changed();

	// Return our half-baked message (in a list for easy std::list mgmt).
	return smpl;
}


/*
 * Originate a short message
 * Put it in the queue and start handling it.
 * From is a shortcode or phone number or something.
 * To is an IMSI or phone number,
 * msgtext is plain ASCII text.
 * firststate is REQUEST_DESTINATION_IMSI   if to is a phone number.
 *            or REQUEST_DESTINATION_SIPURL if to is an IMSI already.
 * Result is 0 for success, negative for error.
 * svgfix where is length of msgtext defined
 * Return
 *   0 = valid
 *   -1 failure is returned
 */
int
SMq::originate_sm(const char *from, const char *to, const char *msgtext,
		enum sm_state firststate)
{
	short_msg_p_list *smpl;
	short_msg_pending *response;
	int errcode;

	smpl = originate_half_sm("MESSAGE");
	if (smpl == NULL) {
		return -1;  // Returning -1 for error
	}

	response = &*smpl->begin();	// Here's our short_msg_pending!

	// Plain text SIP MESSAGE should be repacked before delivery
	response->need_repack = true;

	// For the tag, we cheat and reuse the cseq number.  
	// I don't see any reason not to...why do we have three different
	// tag fields scattered around?
	ostringstream fromline;
	fromline << from << "<sip:" << from << "@" << my_ipaddress << ">;tag=" 
		 << response->parsed->cseq->number;
	osip_message_set_from(response->parsed, fromline.str().c_str());

	ostringstream toline;
	toline << "<sip:" << to << "@" << my_ipaddress << ">";
	osip_message_set_to(response->parsed, toline.str().c_str());

	ostringstream uriline;
	uriline << "sip:" << to << "@" << my_ipaddress << ":" << gConfig.getStr("SIP.Default.BTSPort").c_str();
	osip_uri_init(&response->parsed->req_uri);
	osip_uri_parse(response->parsed->req_uri, uriline.str().c_str());

	osip_message_set_content_type(response->parsed, "text/plain");
	response->content_type = short_msg::TEXT_PLAIN;
    size_t len = strlen(msgtext);
    if (len > SMS_MESSAGE_MAX_LENGTH)
        len = SMS_MESSAGE_MAX_LENGTH;
	osip_message_set_body(response->parsed, msgtext, len);

	// We've altered the text and the parsed version controls.
	response->parsed_was_changed();

	// Now that we set the From tag, we have to create the queue tag.
	response->set_qtag();

	// Now turn it into a text and then parse it for validity
	response->make_text_valid();
	response->unparse();
	
	errcode = response->validate_short_msg(this, false);
	if (errcode == 0) {
		insert_new_message (*smpl, firststate); // originate_sm
	}
	else {
		LOG(DEBUG) << "Short message validate failed, error " << errcode;
	}

	delete smpl;
	return errcode ? -1 : 0;
}


void SMq::InitInsideReaderLoop() {
    // IP address:port of the Home Location Register that we send SIP
    // REGISTER messages to.
    smq.set_register_hostport(gConfig.getStr("Asterisk.address").c_str());

    // IP address:port of the global relay where we sent SIP messages
    // if we don't know where else to send them.
    string grIP = gConfig.getStr("SIP.GlobalRelay.IP");
    string grPort = gConfig.getStr("SIP.GlobalRelay.Port");
    string grContentType = gConfig.getStr("SIP.GlobalRelay.ContentType");
    if (grIP.length() && grPort.length() && grContentType.length()) {
        smq.set_global_relay(grIP.c_str(), grPort.c_str(), grContentType.c_str());
    } else {
        smq.set_global_relay("", "", "");
    }

    // Debug -- print all msgs in log
    print_as_we_validate = gConfig.getBool("Debug.print_as_we_validate");

    // system() calls in back grounded jobs hang if stdin is still open on tty.
    // So, close it.
    close(0);     // Shut off stdin in case we're in background
    open("/dev/null", O_RDONLY);   // fill it with nullity

    LOG(INFO) << "SIP.myPort UDP " << smq.my_udp_port;
    LOG(INFO) << "My own IP address is configured as " << smq.my_ipaddress;
    LOG(INFO) << "The HLR registry is at " << smq.my_register_hostport;

    savefile = gConfig.getStr("savefile").c_str();

    // Took out code that dumped queue file on each timeout svg
    // smq.debug_dump();

} // InitInsideReaderLoop


void SMq::CleaupAfterMainreaderLoop() {
    // Main loop has exited program has been terminated

    // The rest of this code never gets run (unless main_loop exits
    // based upon getting a "reboot" sms or signal or something).
    if (smq.reexec_smqueue) {
    	LOG(WARNING) << "====== Re-Execing! ======";
		if (!smq.save_queue_to_file(savefile)) {  //Save file on shutdown
		  LOG(ERR) << "OUCH!  Could not save queue to file " << savefile;
		}
		please_re_exec = true;

    } else {
		please_re_exec = false;
		LOG(NOTICE) << "====== Quitting! ======";
		if (!smq.save_queue_to_file(savefile)) {  //Save file on shutdown
			LOG(ERR) << "OUCH!  Could not save queue to file " << savefile;
		}
		// smq.debug_dump();
	}

    // Free up any OSIP stuff, to make valgrind squeaky clean.
    osip_mem_release();

    if (please_re_exec)
    	execvp(argv[0], argv);

} // CleaupAfterMainreaderLoop


/* Only called once */
enum sm_state
SMq::bounce_message(short_msg_pending *sent_msg, const char *errstr)
{
	ostringstream errmsg;
	char *username;
	std::string thetext;
	int status;

	username = sent_msg->parsed->to->url->username;
	thetext = sent_msg->get_text();

	LOG(NOTICE) << "Bouncing " << sent_msg->qtag << " from "
	     << sent_msg->parsed->from->url->username  // his phonenum
	     << " to " << username << ": " << errstr;

	errmsg << "Can't send your SMS to " << username << ": ";
	if (errstr)
		errmsg << errstr << ": " << thetext;
	else
		errmsg << "can't send: " << thetext;

	// Don't bounce a message from us - it makes endless loops.
	status = 1;
	if (0 != strcmp(gConfig.getStr("Bounce.Code").c_str(), sent_msg->parsed->from->url->username))
	{
		// But do bounce anything else.
		char *bounceto = sent_msg->parsed->from->url->username;
		bool bounce_to_imsi = 0 == strncmp("IMSI", bounceto, 4)
		                   || 0 == strncmp("imsi", bounceto, 4);
		status = originate_sm(gConfig.getStr("Bounce.Code").c_str(), // Read from a config
			     bounceto,  // to his phonenum or IMSI
			     errmsg.str().c_str(), // error msg
			     bounce_to_imsi? REQUEST_DESTINATION_SIPURL: // dest is IMSI
			                     REQUEST_DESTINATION_IMSI); // dest is phonenum
	}
	if (status == 0) {
		// Message is okay
	    return DELETE_ME_STATE;
	} else {
	    LOG(ERR) << "Error status should be 0, instead it is " << status;
	    return NO_STATE;	// Punt to debug.
	}
}

void SMq::InitBeforeMainLoop() {
    // Initialize
	// TODO : post WebUI NG MVP
	gNodeManager.start(45063);//, 31338);

	please_re_exec = false;
    stop_main_loop = false;
    reexec_smqueue = false;

	// Open the CDR file for appending.
	std::string CDRFilePath = gConfig.getStr("CDRFile");
	if (CDRFilePath.length()) {
		gCDRFile = fopen(CDRFilePath.c_str(),"a");
		if (!gCDRFile) {
		  LOG(ALERT) << "CDR file at " << CDRFilePath.c_str() << " could not be created or opened! errno " << errno << strerror(errno) << endl;
		}
	}

	// Set up short-code commands users can type
	init_smcommands(&short_code_map);

	if (gConfig.defines("SIP.Timeout.MessageResend")) {
    	int int1 = gConfig.getNum("SIP.Timeout.MessageResend");
    	LOG(DEBUG) << "Set SIP.Timeout.MessageResend value " <<  int1;
    	// timeouts_REQUEST_MSG_DELIVERY[REQUEST_DESTINATION_SIPURL] = int1;  // svgfix
    }

   if (gConfig.defines("SIP.Timeout.MessageBounce")) {
	  int int2 = gConfig.getNum("SIP.Timeout.MessageBounce");
	  LOG(DEBUG) << "Set SIP.Timeout.MessageBounce value " <<  int2;
	  timeouts_REQUEST_DESTINATION_IMSI[DELETE_ME_STATE] = int2;
   }

   //LOG(DEBUG) << "REQUEST_DESTINATION_SIPURL value " << REQUEST_DESTINATION_SIPURL;
   //LOG(DEBUG) << "Timeout from 8 to 11 " << *SMqueue::timeouts[REQUEST_MSG_DELIVERY][REQUEST_DESTINATION_SIPURL];
   //LOG(DEBUG) << "Timeout from 11 to 8 " << *SMqueue::timeouts[REQUEST_DESTINATION_SIPURL][REQUEST_MSG_DELIVERY];


   // Port number that we (smqueue) listen on.
   if (smq.init_listener(gConfig.getStr("SIP.myPort").c_str())) {
       LOG(INFO) << "Got VALID port for smqueue to listen on";
   } else {
       LOG(INFO) << "Failed to get port for smqueue to listen on";
   }

   // Restore message queue
   savefile = gConfig.getStr("savefile").c_str();
	// Load queue on start up
   if (!smq.read_queue_from_file(smq.savefile)) {  // Load queue file on startup
	   LOG(WARNING) << "Failed to read queue on startup from file " << smq.savefile;
   }

    // Set up Posix message queue limit
    FILE * gTempFile = NULL;
    int xTemp;
    gTempFile = fopen("/proc/sys/fs/mqueue/msg_max","w");
    if (!gTempFile) {
        LOG(ALERT) << "Could not open " << "/proc/sys/fs/mqueue/msg_max, errno " << errno << " " << strerror(errno) << endl;
	} else {
        xTemp = fprintf(gTempFile,"%d", MQ_MAX_NUM_OF_MESSAGES);
        if (xTemp == 0){
            LOG(ALERT) << "Could not write to " << "/proc/sys/fs/mqueue/msg_max, errno " << errno << " " << strerror(errno) << endl;
        }
        fclose(gTempFile);
   }

} // InitBeforeMainLoop




/*
 * Send a bounce message, based on an existing queued message.
 * Return the state to set the original bouncing message to.
 */
/*
 * See if the handset's imsi and phone number are in the HLR database yet,
 * since if it isn't, we can't register the imsi at its cell's host:port yet.
 */
bool
SMq::ready_to_register (short_msg_p_list::iterator qmsg)
{
	char *callerid, *imsi;

	qmsg->parse();
	if (!qmsg->parsed ||
	    !qmsg->parsed->from ||
	    !qmsg->parsed->from->url)
		return false;
	imsi = qmsg->parsed->from->url->username;
	callerid = my_hlr.getCLIDLocal(imsi);
	return (callerid != NULL);
}


/*
 * Register a handset's IMSI with its cell, by sending Asterisk
 * a SIP REGISTER message that we "relay" from the cell.
 * (We actually originate it, but we pretend that the cell sent it to us.
 *  Actually the cell sent us an incoming shortcode SMS from an
 *  unregistered phone, which is in the queue in REGISTER_HANDSET state.)
 *
 * Argument qmsg is the SMS message, with its From line still an IMSI.
 * We register "IMSI@HLRhost:HLRport" at the sip uri:
 *             "IMSI@cellhost:cellport".
 *
 * Puts a message in the queue on success
 * Return
 * 	Next state DELETE_ME_STATE on failure

 *
 */
enum sm_state
SMq::register_handset(short_msg_p_list::iterator qmsg)
{
	short_msg_p_list *smpl;
	short_msg_pending *response;
	int errcode;
	char *imsi;

	LOG(DEBUG) << "Send register handset message";

	smpl = originate_half_sm("REGISTER");
	if (smpl == NULL) {
		return DELETE_ME_STATE;  // Returning DELETE_ME_STATE will make sure message gets deleted
	}
	response = &*smpl->begin();	// Here's our short_msg_pending!

	// SIP REGISTER should not be repacked before delivery
	response->need_repack = false;

	imsi = qmsg->parsed->from->url->username;

	// The To: line is the long-term name being registered.
	ostringstream toline;
	toline << imsi << "<sip:" << imsi << "@" << my_register_hostport << ">";
	osip_message_set_to(response->parsed, toline.str().c_str());

	// The From: line is the same, plus a tag.
	// However, we steal the tag from our cseq, since we don't care
	// about it much.
	ostringstream fromline;
	fromline << toline.str() << ";tag=" << response->parsed->cseq->number;
	osip_message_set_from(response->parsed, fromline.str().c_str());

	// URI in the first line: insert SIP HLR's host/port.
	ostringstream uriline;
	uriline << "sip:" << my_register_hostport;
	osip_uri_init(&response->parsed->req_uri);
	osip_uri_parse(response->parsed->req_uri, uriline.str().c_str());

	// Contact: field specifies where we're registering from.
	ostringstream contactline;
	contactline << "<sip:" << imsi << "@";
	contactline << my_network.string_addr((struct sockaddr *)qmsg->srcaddr,
					      qmsg->srcaddrlen, true);
	contactline << ">;expires=3600";
	osip_message_set_contact(response->parsed, contactline.str().c_str());

	// We've altered the fields, and the parsed version controls.
	response->parsed_was_changed();

	// Now that we set the From tag, we have to create the queue tag.
	response->set_qtag();

	// Set the linktag of our REGISTER message to point to qmsg (SMS msg)
	set_linktag(smpl->begin(), qmsg);

	// Now turn it into a text and then parse it for validity
	response->make_text_valid();
	response->unparse();
	
	errcode = response->validate_short_msg(this, false);
	if (errcode != 0) {
		LOG(DEBUG) << "Register handset short message failed validation "  << errcode;
		delete smpl;
		return DELETE_ME_STATE;
	}

	// Pop new SIP REGISTER out of the smpl queue-of-one and
	// into the real queue, where it will very soon be delivered.
	insert_new_message (*smpl, REQUEST_MSG_DELIVERY); // In register_handset
	// We can't reference response, or *smpl, any more...

	delete smpl;

	// The next state of the original (SMS shortcode) message is...
	return ASKED_TO_REGISTER_HANDSET;
}


bool SMq::handle_short_code(const short_code_map_t &short_code_map,
                            short_msg_p_list::iterator qmsg,
									 sm_state &next_state)
{
	osip_body_t *bod1;
	std::string bods;
	short_func_t shortfn;
	short_code_map_t::const_iterator shortit;
	enum short_code_action sca;
	short_code_params params;
	int status;
	string short_code;

	short_code = qmsg->parsed->req_uri->username;
	shortit = short_code_map.find (short_code);

	if (shortit == short_code_map.end()) {
		return false;
	}

	/* Messages to certain addresses are special commands */
	shortfn = shortit->second;
	bods = qmsg->get_text();

	// Set up arguments and access pointers, then call
	// the short-code function to process it.
	params.scp_retries = qmsg->retries;
	params.scp_smq = this;
	params.scp_qmsg_it = qmsg;

	LOG(INFO) << "Short-code SMS "
	     << qmsg->parsed->req_uri->username
	     << " with text \"" << bods << "\"";

	sca = (*shortfn) (qmsg->parsed->from->url->username, // imsi
			bods.data(),  // msg text
			&params);

	// The short-code function asks us to do something when
	// it's done.  Do it.
	switch (sca) {
	case SCA_REPLY:
		LOG(INFO) << "Short-code replies: "
		     << params.scp_reply;
		status = originate_sm(
		  qmsg->parsed->req_uri->username,  // from shortcode
		  qmsg->parsed->from->url->username,// to his IMSI
		  params.scp_reply, REQUEST_DESTINATION_SIPURL);
		if (!status) {
			next_state = DELETE_ME_STATE;	 // Done!
			return true;
		}
		LOG(NOTICE) << "Reply failed " << status << "!";
		// NO BREAK
	default:
	case SCA_INTERNAL_ERROR:
		LOG(ERR) << "Error in short-code function "
		     << qmsg->parsed->req_uri->username
		     << "(" << bods << "): " << params.scp_reply;
		next_state = NO_STATE;
		return true;

	case SCA_EXEC_SMQUEUE:
		reexec_smqueue = true;
		stop_main_loop = true;
		next_state = DELETE_ME_STATE;
		return true;
			
	case SCA_QUIT_SMQUEUE:
		stop_main_loop = true;
		next_state = DELETE_ME_STATE;
		return true;

	case SCA_DONE:
		next_state = DELETE_ME_STATE;
		return true;

	case SCA_RETRY_AFTER_DELAY:
		// FIXME, timeout is implicit in set_state table,
		// rather than taken from params.scp_delay.
		qmsg->retries++;
		next_state = REQUEST_FROM_ADDRESS_LOOKUP;
		return true;

	case SCA_AWAIT_REGISTER:
		// We just linked the phone# to the IMSI, but we
		// have to wait til the HLR updates, before
		// we can link the IMSI to the originating IP address
		// and port number of its cell.
		next_state = AWAITING_REGISTER_HANDSET;
		return true;

	case SCA_REGISTER:
		next_state = register_handset(qmsg);
		return true;

	case SCA_TREAT_AS_ORDINARY:
		break;		// fall through into non-special case.

	case SCA_RESTART_PROCESSING:
		next_state = INITIAL_STATE;
		return true;
	}

	return false;
} // SMq::handle_short_code


/* Change the From address imsi to a valid phone number in + countrycode phonenum
   format.  Also add a Via: line about us.
   From address can contain imsi or phone number   ????
   fromusername contains imsi
*/
enum sm_state
SMq::lookup_from_address (short_msg_pending *qmsg)
{
	char *host = qmsg->parsed->from->url->host;
	bool got_phone = false;  // Never updated

	char *scheme = qmsg->parsed->from->url->scheme;
	if (!scheme) { LOG(ERR) << "no scheme";  return NO_STATE; }
	if (0 != strcmp("sip", scheme)) { LOG(ERR) << "scheme != sip"; return NO_STATE; }

	// Get from user name
	char *fromusername = qmsg->parsed->from->url->username;
	if (!fromusername) { LOG(ERR) << "No from user name"; return NO_STATE; }
	
	if (!host) { LOG(ERR) << "no hostname"; return NO_STATE; }

	// Insert a Via: line describing us, this makes us easier to trace,
	// and also allows a remote SIP agent to reply to us.  (Maybe?)
	osip_via_t *via;
	char *temp;
	osip_message_append_via(qmsg->parsed, "SIP/2.0/UDP x:1234;branch=123");
	// FIXME, don't assume UDP, allow TCP here too.
	osip_message_get_via (qmsg->parsed, 0, &via);
	temp = via_get_host(via);
	osip_free(temp);
	via_set_host(via, osip_strdup(my_ipaddress.c_str()));
	temp = via_get_port(via);
	osip_free(temp);
	via_set_port(via, osip_strdup(my_udp_port.c_str()));

	/* Username can be in various formats.  Check for formats that
	   we know about.  Anything else we punt.
	   This can be phone number or IMSI */
	/* TODO: Check for tel BM2011 */
	if (got_phone || fromusername[0] == '+' || isdigit(fromusername[0])) {
		/* We have a phone number.  This is what we want.
		   So we're done, and can move on to the next part
		   of processing the short_msg. */
		return REQUEST_DESTINATION_IMSI;
	}

	/* If we have "imsi" on the front, strip it.  */
	char *tryuser = fromusername;
	if ((fromusername[0] == 'i'||fromusername[0]=='I')
         && (fromusername[1] == 'm'||fromusername[1]=='M')
	 && (fromusername[2] == 's'||fromusername[2]=='S')
	 && (fromusername[3] == 'i'||fromusername[3]=='I')) {
		tryuser += 4;
	}

	/* http://en.wikipedia.org/wiki/International_Mobile_Subscriber_Identity */
	/* IMSI length check */
	size_t len = strlen (tryuser);
        if (len != 15 && len != 14) {
		LOG(ERR) << "Message does not have a valid IMSI!";
		/* This is not an IMSI.   Punt.  */
		return NO_STATE;
	}

	/* Look up the IMSI in the Home Location Register. */
	char *newfrom;

	newfrom = my_hlr.getCLIDLocal(fromusername);
	if (!newfrom) {
		/* ==================FIXME KLUDGE====================
		 * Here is our fake table of IMSIs and phone numbers
		 * ==================FIXME KLUDGE==================== */
		for (int i = 0; imsi_phone[i].imsi[0]; i++) {
			if (0 == strcmp(imsi_phone[i].imsi, fromusername)) {
				newfrom = strdup(imsi_phone[i].phone);
				break;
			}
		}
	}

	if (!newfrom) {
		LOG(NOTICE) << "Lookup IMSI <" << fromusername
		     << "> to phonenum failed.";
		LOG(DEBUG) << qmsg->text;
		//return bounce_message (qmsg,
		//	gConfig.getStr("Bounce.Message.IMSILookupFailed").c_str()
		//);
		// return NO_STATE;	// Put it into limbo for debug.
		return REQUEST_DESTINATION_IMSI;
	}

	/* We found it!  Translation done! 
	   Now the dance of freeing the old name and
	   inserting new name.  */
	char *p;
	char *q;

	osip_free (qmsg->parsed->from->url->username);
	osip_free (qmsg->parsed->from->displayname);
	p = (char *)osip_malloc (strlen(newfrom)+1);
	q = (char *)osip_malloc (strlen(newfrom)+1);
	strcpy(p, newfrom);
	strcpy(q, newfrom);
	qmsg->parsed->from->url->username = p;
	qmsg->parsed->from->displayname = q;
	qmsg->parsed_was_changed();

	free(newfrom);		// C interface uses free() not delete.
	return REQUEST_DESTINATION_IMSI;
}

/* Check to see if we can directly route the message. Return true if we can. */
bool SMq::to_is_deliverable(const char *to)
{
	bool isDeliverable = false;
	
	isDeliverable = (short_code_map.find(to) != short_code_map.end());
	if (!isDeliverable) {
		char *newdest = my_hlr.getIMSI2(to);
	
		if (newdest
	 	    && 0 != strncmp("imsi", newdest, 4) 
	 	    && 0 != strncmp("IMSI", newdest, 4)) {
			free(newdest);
			newdest = NULL;
		}

		isDeliverable = (newdest != NULL);

		if (newdest)
			free(newdest);
	}

	return isDeliverable;
}

/* Check to see if we know who the message is from. TODO: Uhhhhh... What if relay sends us a message. We don't know their from... */
bool
SMq::from_is_deliverable(const char *from)
{
	bool isDeliverable = false;
	char *newdest = my_hlr.getCLIDLocal(from);

	isDeliverable = (newdest != NULL);

	if (newdest)
		free(newdest);

	return isDeliverable;
}

/* Requirement: parse() is called before calling this */
bool
SMq::convert_content_type(short_msg_pending *message, short_msg::ContentType to_type)
{
	LOG(DEBUG) << "Converting content type from " << message->content_type << " to " << to_type;

	/*if ((to_type == short_msg::TEXT_PLAIN && message->content_type == short_msg::VND_3GPP_SMS) ||
	    (to_type == short_msg::VND_3GPP_SMS && message->content_type == short_msg::TEXT_PLAIN)) {
		message->convert_message(to_type);
	}*/
	message->convert_message(to_type);
	return true;
}

/*
 * Change the Request-URI's/phone number to a valid IMSI.
 *
 */
enum sm_state
SMq::lookup_uri_imsi (short_msg_pending *qmsg)
{
	qmsg->parse();

	char *scheme = qmsg->parsed->req_uri->scheme;
	if (!scheme) { LOG(ERR) << "No scheme"; return NO_STATE; }
	if (0 != strcmp("sip", scheme)) { LOG(ERR) << "scheme != sip"; return NO_STATE; }

#if 0
	char *host = qmsg->parsed->req_uri->host;
	if (!host) { LOG(ERR) << "no host!"; return NO_STATE; }
	if (0 != strcmp("127.0.0.1", host)
 	 && 0 != strcmp("localhost", host)
 	 && 0 != strcmp(my_ipaddress.c_str(), host)
 	 && 0 != strcmp(my_2nd_ipaddress.c_str(), host)) {
		LOG(ERR) << "host not valid";
		return NO_STATE;
	}
#endif

	char *username = qmsg->parsed->req_uri->username;
	if (!username) { LOG(ERR) << "No user name"; return NO_STATE; }
	
	/* Username can be in various formats.  Check for formats that
	   we know about.  Anything else we punt.  */
	if (username[0] == '+' || (   0 != strncmp("imsi", username, 4)
				   && 0 != strncmp("IMSI", username, 4))) {
		// We have a phone number.  It needs translation.

		char *newdest = my_hlr.getIMSI2(username);  // Get IMSI from phone number
		if (!newdest) {
			/* ==================FIXME KLUDGE====================
`				 * Here is our fake table of IMSIs and phone numbers
			 * ==================FIXME KLUDGE==================== */
			for (int i = 0; imsi_phone[i].phone[0]; i++) {
				if (0 == strcmp(imsi_phone[i].phone, username)) {
					newdest = strdup(imsi_phone[i].imsi);
					break;
				}
			}
		}

		// It had better say "imsi" if it's an IMSI, else
		// lookup_uri_hostport will get confused.
		if (newdest
                 && 0 != strncmp("imsi", newdest, 4) 
		 && 0 != strncmp("IMSI", newdest, 4)) {
			free(newdest);
			newdest = NULL;
		}

		if (!newdest) {
			/* Didn't find it in HLR or fake table.  Bitch. */
			// We have to return an error to the originator.
			LOG(NOTICE) << "Lookup phonenum '" << username << "' to IMSI failed";
			LOG(DEBUG) << "MSG = " << qmsg->text;

		    if (global_relay.c_str()[0] == '\0') {
			// TODO : disabled, to disconnect SR from smqueue
			// || !my_hlr.useGateway(username)) {
			// There's no global relay -- or the HLR says not to
			// use the global relay for it -- so send a bounce.
			LOG(WARNING) << "no global relay defined; bouncing message intended for " << username;
			return bounce_message (qmsg, gConfig.getStr("Bounce.Message.NotRegistered").c_str());
		    } else {
			// Send the message to our global relay.
			// We leave the username as a phone number, and
			// let it pass on to look up the destination
			// SIP URL (which is the global relay).
			//
			// However, the From address is at this point the
			// sender's local ph#.  Map it to the global ph#.
			LOG(INFO) << "using global SIP relay " << global_relay << " to route message to " << username;
			char *newfrom;
			newfrom = my_hlr.mapCLIDGlobal(
					qmsg->parsed->from->url->username);
			if (newfrom) {
				osip_free(qmsg->parsed->from->url->username);
				qmsg->parsed->from->url->username = 
					osip_strdup (newfrom);
			}
			convert_content_type(qmsg, global_relay_contenttype);
			// TODO do cost checks here for out-of-network, probably instead of in smsc shortcode or INITIAL_STATE
			return REQUEST_DESTINATION_SIPURL;
		    }
		}

		/* We found it!  Translation done! 
		   Now the dance of freeing the old name and
		   inserting new name.  */
		char *p;
		osip_free (qmsg->parsed->req_uri->username);
		p = (char *)osip_malloc (strlen(newdest)+1);
		strcpy(p, newdest);
		qmsg->parsed->req_uri->username = p;
		qmsg->parsed_was_changed();

		free(newdest);		// C interface uses free() not delete

		// TODO do cost checks here for in-network, probably instead of in smsc shortcode or INITIAL_STATE
		return REQUEST_DESTINATION_SIPURL;
	}

	/* If we have "imsi" on the front, scan past it.  */
	if (username[0] == 'i' && username[1] == 'm'
	 && username[2] == 's' && username[3] == 'i') {
		username += 4;
	}
	if (username[0] == 'I' && username[1] == 'M'
	 && username[2] == 'S' && username[3] == 'I') {
		username += 4;
	}

	/* http://en.wikipedia.org/wiki/International_Mobile_Subscriber_Identity */
	size_t len = strlen (username);
        if (len != 15 && len != 14) {
		LOG(ERR) << "Invalid IMSI: " << username;
		/* This is not an IMSI.   Punt.  */
		return NO_STATE;
	}

	/* We have an IMSI already.  Now figure out how to route it. */
	return REQUEST_DESTINATION_SIPURL;
}


/*
 * Look up the hostname and port number where we should send Short Messages
 * for the IMSI (or phone number, if we're using a global relay)
 * in the To address.  
 * 
 * This is also where we assign a new Call-ID to the message, so that
 * re-sends will use the same Call-ID, but re-locate's (looking up the
 * recipient's location again) will use a new one.
 */
/*
 * Helper function because C++ is fucked about types.
 * and the osip library doesn't keep its types straight.
 */
int
osip_via_clone2 (void *via, void **dest)
{
	return osip_via_clone ((const osip_via_t *)via,
			       (osip_via_t **)dest);
}


/*
 * After we received a datagram, send a SIP response message
 * telling the sender what we did with it.  (Unless the datagram we
 * received was already a SIP response message...)
 * svgfix return error on failure
 * Called in writer thread
 * Calls my_network.send_dgram
 */
void
SMq::respond_sip_ack(int errcode, SMqueue::short_msg_pending *smp,
		char *netaddr, size_t netaddrlen)
{
	string phrase;
	short_msg response;
	bool okay;

	LOG(DEBUG) << "Send SIP ACK message";

	if (!smp->parse()) {
		LOG(DEBUG) << "Short message parse failed";
		return;		// Don't ack invalid SIP messages
	}

	LOG(DEBUG) << "Short message parse returned success";
	if (MSG_IS_RESPONSE(smp->parsed)) {
		LOG(DEBUG) << "Ignore response message";
		return;		// Don't ack a response message, or we loop!
	}
	osip_message_init(&response.parsed);
	response.parsed_is_valid = true;

	// Copy over the CSeq, From, To, Call-ID, Via, etc.
	osip_to_clone(smp->parsed->to,     &response.parsed->to);
	osip_from_clone(smp->parsed->from, &response.parsed->from);
	osip_cseq_clone(smp->parsed->cseq, &response.parsed->cseq);
	osip_call_id_clone(smp->parsed->call_id, &response.parsed->call_id);
	osip_list_clone(&smp->parsed->vias, &response.parsed->vias,
			&osip_via_clone2);

	//don't add a new via header to a response! -kurtis
	//RFC 3261 8.2.6.2

	// Make a nice message.
	switch (errcode) {
	case 100:	phrase="Trying..."; break;
	case 200:	phrase="Okay!"; break;
	case 202:	phrase="Queued"; break;
	case 400:	phrase="Bad Request"; break;
	case 401:	phrase="Un Author Ized"; break;
	case 403:	phrase="Forbidden - first register, by texting your 10-digit phone number to 101."; break;
	case 404:	phrase="Phone Number Not Registered"; break;  // Not Found
	case 405:	phrase="Method Not Allowed";
			osip_message_set_allow(response.parsed, "MESSAGE");
			break;
	case 413:	phrase="Message Body Size Error"; break;
	case 415:	phrase="Unsupported Content Type";
			osip_message_set_accept(response.parsed, "text/plain, application/vnd.3gpp.sms");
			break;
	case 416:	phrase="Unsupported URI scheme (not SIP)"; break;
	case 480:	phrase="Recipient Temporarily Unavailable"; break;
	case 484:	phrase="Address Incomplete"; break;
	default:	phrase="Error Message Table Needs Updating"; break;
	}

	osip_message_set_status_code (response.parsed, errcode);
	osip_message_set_reason_phrase (response.parsed,
				        osip_strdup((char *)phrase.c_str()));

	// We've altered the text and the parsed version controls.
	response.parsed_was_changed();

	// Now turn it into a datagram and hustle it home.
	response.make_text_valid();
	LOG(INFO) << "Responding with \"" << errcode << " " << phrase << "\".";

	okay = my_network.send_dgram(response.text, strlen(response.text),
				     netaddr, netaddrlen);
	if (!okay)
		LOG(ERR) << "send_dgram had trouble sending the response err " << okay << " size " << strlen(response.text);
}

//
// The main loop that listens for incoming datagrams, handles them
// through the queue, and moves them toward transmission.
// Called by reader thread
// Main job
//
enum sm_state
SMq::lookup_uri_hostport (short_msg_pending *qmsg)
{

	qmsg->parse();

	char *imsi = qmsg->parsed->req_uri->username;
	char *p, *mycallnum;
	char *newhost, *newport;
	const char *myhost;

	if (!imsi) { LOG(ERR) << "No IMSI"; return NO_STATE; }

	/* Username can be in various formats.  Check for formats that
	   we know about.  Anything else we punt.  */
	if (imsi[0] == '+' || (0 != strncmp("imsi", imsi, 4)
			    && 0 != strncmp("IMSI", imsi, 4))) {
		LOG(DEBUG) << "We have a number: " << imsi;

		// We have a phone number.  It needs translation.
		newport = strdup(global_relay_port.c_str());
		newhost = strdup(global_relay.c_str());
		convert_content_type(qmsg, global_relay_contenttype);
		//qmsg->from_relay = true;
	} else {
		/* imsi is an IMSI at this point.  */
		LOG(DEBUG) << "We have an IMSI: " << imsi;
		newport = NULL;
		newhost = my_hlr.getRegistrationIP (imsi);
	}

	LOG(DEBUG) << "We are going to try to send to " << newhost << " on " << newport;

	if (newhost && newport == NULL) {
		// Break up returned "host:port" string.
		char *colon = strchr(newhost,':');
		if (colon) {
			newport = strdup(colon+1);
			*colon = '\0';
		}
	}

	// KLUDGE! KLUDGE! KLUDGE! for testing only
	if (!newhost) {
		newhost = strdup((char *)"127.0.0.1");
	}
	if (!newport) {
		newport = strdup((char*)gConfig.getStr("SIP.Default.BTSPort").c_str()); //(char *)"5062");
	}


	LOG(DEBUG) << "We will send to " << newhost << " on " << newport;

	/* We found it!  Translation done!
	   Now the dance of freeing the old ones and
	   inserting new ones.  */

	if (0 != strcmp (newhost, qmsg->parsed->req_uri->host))
	{
		osip_free (qmsg->parsed->req_uri->host);
		p = (char *)osip_malloc (strlen(newhost)+1);
		strcpy(p, newhost);
		qmsg->parsed->req_uri->host = p;
		qmsg->parsed_was_changed();
	}

	if (qmsg->parsed->req_uri->port != newport)
	{
		osip_free (qmsg->parsed->req_uri->port);
		p = newport;
		if (newport) {
			p = (char *)osip_malloc (strlen(newport)+1);
			strcpy(p, newport);
		}
		qmsg->parsed->req_uri->port = p;
		qmsg->parsed_was_changed();
	}

	// We've altered the message, it's a new message, and it needs
	// a new Call-ID so it won't be confused with the old message.
	myhost = my_ipaddress.c_str();
	mycallnum = my_network.new_call_number();

	if (!qmsg->parsed->call_id) {
		osip_call_id_init(&qmsg->parsed->call_id);
	}

	//rfc 3261 relaxes this, don't require host -kurtis
	if (osip_call_id_get_host (qmsg->parsed->call_id)){
		if (0 != strcmp(myhost,
				osip_call_id_get_host(qmsg->parsed->call_id))) {
			osip_free (osip_call_id_get_host(qmsg->parsed->call_id));
			p = (char *)osip_malloc (strlen(myhost)+1);
			strcpy(p, myhost);
			osip_call_id_set_host (qmsg->parsed->call_id, p);
			qmsg->parsed_was_changed();
		}
 	}

	if (0 != strcmp(mycallnum,
		 	osip_call_id_get_number (qmsg->parsed->call_id))) {
		osip_free (osip_call_id_get_number (qmsg->parsed->call_id));
		p = (char *)osip_malloc (strlen(mycallnum)+1);
		strcpy(p, mycallnum);
		osip_call_id_set_number (qmsg->parsed->call_id, p);
		qmsg->parsed_was_changed();
	}

	// Now that we changed the Call-ID, we have to update the queue tag.
	qmsg->set_qtag();

	// Both of these were dynamic storage; don't leak them.
	// (They were allocated by malloc() so we free with free().
	//  Note that osip_malloc isn't necessarily malloc -- and
	//  neither of them is necessarily  new  or  delete .)
	free(newhost);
	free(newport);

	// OK, we're done; next step is to deliver it to that host & port!
	return REQUEST_MSG_DELIVERY;
}

/*
 * The main loop get datagram from the network an put them into the queue for processing
 *
 */
void SMq::main_loop(int msTMO)
{
	int len;	// MUST be signed -- not size_t!
				// else we can't see -1 for errors...
	int timeout;
	short_msg_p_list *smpl;
	short_msg_pending *smp;
	char buffer[5000];
	//short_msg_p_list::iterator qmsg;
	//time_t now;
	int errcode;

	//LOG(DEBUG) << "Start SMq::main_loop (get SIP messages tmo:" << msTMO << ")";

	// No message at this point
	// READ
	// Read an entry from the network
	// This could wait a very long time for a message
	len = my_network.get_next_dgram((char *) buffer, (int) sizeof(buffer), msTMO);

	if (len < 0) {
		// Error.
		LOG(DEBUG) << "Error from get_next_dgram: " << strerror(errno);
		// Just continue...
		return;
	} else if (len == 0) {
		// Timeout.  Just push things along.
		LOG(DEBUG) << "Timeout wait for datagram";
		return;
	} else {
		LOG(DEBUG) << "Got incoming datagram length " << len;
		// We got a datagram.  Dump it into the queue, copying it.
		//
		// Here we do a bit of tricky memory allocation.  Rather
		// than make a short_msg_pending and then have to COPY it
		// into a short_msg_p_list (including malloc-ing all the
		// possible pointed-to stuff and then freeing all the original
		// strings and things), we make a short_msg_p_list
		// and create in it a single default element.  Then we fill
		// in that element as our new short_msg_pending.  This lets
		// us (soon) link it into the main message queue list, 
		// without ever copying it.
		// 
		// HOWEVER!  The implementation of std::list in GNU C++
		// (ver. 4.3.3) has a bug: it does not PERMIT a class to be
		// made into
		// a list UNLESS it allows copy-construction of its instances.
		// You get an extremely inscrutable error message deep
		// in the templated bowels of stl_list.h , referencing
		// the next non-comment line of this file.
		// THUS, we can't check at compile time to prevent the
		// making of copies of short_msg_pending's -- instead, we
		// have to do that check at runtime (allowing the default
		// newly-initialized one to be copied, but aborting with
		// any depth of stuff in it).

		smpl = new short_msg_p_list(1);
		smp = &*smpl->begin();	// Here's our short_msg_pending!
		smp->initialize (len, buffer, false);  // Just makes a copy
		smp->ms_to_sc = true;
		//LOG(DEBUG) << "Before insert new message smpl size " << smpl->size();

		if (my_network.recvaddrlen <= sizeof (smp->srcaddr)) {
			smp->srcaddrlen = my_network.recvaddrlen;
			memcpy(smp->srcaddr, my_network.src_addr, 
			       my_network.recvaddrlen);
		}

		errcode = smp->validate_short_msg(this, true);
		if (errcode == 0) {
			// Message good
			if (MSG_IS_REQUEST(smp->parsed)) {
				LOG(NOTICE) << "Got SMS rqst qtag '"
				     << smp->qtag << "' from "
				     << smp->parsed->from->url->username 
				     << " for "
				     << (smp->parsed->req_uri ? smp->parsed->req_uri->username : "");  // just shows smsc
			} else {
				LOG(INFO) << "Got SMS "
				     << smp->parsed->status_code
				     << " Response qtag '"
				     << smp->qtag <<  " for "
				     << (smp->parsed->req_uri ? smp->parsed->req_uri->username : "");  // Name that was sent to smqueue
			}

// **********************************************************************
// ****************** Insert a message in the queue *********************
			insert_new_message(*smpl); // Reader thread main_loop
			errcode = 202;
			// It's OK to reference "smp" here, whether it's in the
			// smpl list, or has been moved into the main time_sorted_list.
			queue_respond_sip_ack(errcode, smp, smp->srcaddr, smp->srcaddrlen); // Send respond_sip_ack message to writer thread
		} else {
			// Message is bad not inserted in queue
			LOG(WARNING) << "Received bad message, error " << errcode;
			queue_respond_sip_ack(errcode, smp, smp->srcaddr, smp->srcaddrlen); // SVG This might need to be removed for failed messages  Investigate
			// Don't log message data it's invalid and should not be accessed
		}

		// We won't leak memory if we didn't queue it up, since
		// the delete of smpl will delete anything still
		// in its list.
		delete smpl;  // List entry that got added
	} // got datagram

} // SMq::main_loop


/* Debug dump of SMq and mainly the queue. */
void SMq::debug_dump() {

	time_t now = msgettime();
	LOG(DEBUG) << "Dump message queue";
	lockSortedList();
	short_msg_p_list::iterator x = time_sorted_list.begin();
	for (; x != time_sorted_list.end(); ++x) {
		x->make_text_valid();
		LOG(DEBUG) << "=== State: " << sm_state_string(x->state) << "\t"
		     << (x->next_action_time - now) << endl << "MSG = "
		     << x->text;
	}
	unlockSortedList();
}


/*
 * Save queue to file.
 * 
 * We save in reverse timestamp order, to make it very fast to insert when
 * re-read.
 */
bool
SMq::save_queue_to_file(std::string qfile)
{
	ofstream ofile;
	unsigned howmany = 0;
	LOG(DEBUG) << "save_queue_to_file:" << qfile;

	ofile.open(qfile.c_str(), ios::out | ios::binary | ios::trunc);
	if (!ofile.is_open())
		return false;

	lockSortedList(); // Lock file during the write.  Check the time on this.
	// Example of what should be in the file
	// === 10 -506057005 127.0.0.1:5062 640 0 0
	// === state  next_action_time  network_address  length  ms_to_sc  need_repack  message_text

	short_msg_p_list::reverse_iterator x = time_sorted_list.rbegin();
	for (; x != time_sorted_list.rend(); ++x) {
		x->make_text_valid();
		ofile << "=== "
			<< (int) x->state << " "
		      << x->next_action_time << " "
		      << my_network.string_addr((struct sockaddr *)x->srcaddr, x->srcaddrlen, true) << " "
		      << strlen(x->text) << " "
		      << x->ms_to_sc << " "
		      << x->need_repack << endl
		      << x->text << endl << endl;
		howmany++;
		LOG(DEBUG) << "Write entry:" << howmany << " Len:" << strlen(x->text) << " MSG:" << x->text;
	}
	unlockSortedList();

	bool result = !ofile.fail();
	ofile.close();
	if (result) {
		LOG(INFO) << "Saved " << howmany << " queued messages to " << qfile;
	} else {
		LOG(ERR) << "FAILED to save " << howmany << " queued messages to " << qfile;
	}
	return result;
} //save_queue_to_file


/*
 * Read a new queue from file.
 */
bool
SMq::read_queue_from_file(std::string qfile)
{
	ifstream ifile;
	std::string equals;
	unsigned astate, atime, alength;
	unsigned ms_to_sc, need_repack;
	std::string short_code;
	std::string netaddrstr;
	sm_state mystate;
	time_t mytime;
	char *msgtext;
	unsigned howmany = 0, howmanyerrs = 0;
	char ignoreme;
	short_msg_p_list *smpl;
	short_msg_pending *smp;
	int errcode;
	LOG(DEBUG) << "read_queue_from_file:" << qfile;

	ifile.open(qfile.c_str(), ios::in | ios::binary);
	if (!ifile.is_open())
		return false;

	// === state  next_action_time  network_address  length  ms_to_sc  need_repack  message_text	// === state   next_action_time   network_address    length message_text  ms_to_sc    need_repack
	while (!ifile.eof()) {
		ifile >> equals >> astate >> atime;
		if ((equals != "===") || (ifile.eof())) {  // === is the beginning of a record
			LOG(DEBUG) << "End of smqueue file";
			break;  // End of file
		}
		// LOG(DEBUG) << "Get next entry";
		ifile >> netaddrstr;
		ifile >> alength;
		ifile >> ms_to_sc;
		ifile >> need_repack;
		//LOG(DEBUG) << "netaddrstr=" << netaddrstr << " length=" << alength << " ms_to_sc="
		//		<< ms_to_sc << " need_repack=" << need_repack;

		while (ifile.peek() == '\n')
			ignoreme = ifile.get();  // Skip over blank lines
		msgtext = new char[alength+2];
		// Get alength chars (or until null char, hope there are none)
		ifile.get(msgtext, alength+1, '\0');  // read the next record
		while (ifile.peek() == '\n')
			ignoreme = ifile.get();  // Skip blanks
		howmany++;
		mystate = (SMqueue::sm_state)astate;
		mytime = atime;
		
		smpl = new short_msg_p_list (1);
		smp = &*smpl->begin();	// Here's our short_msg_pending!
		smp->initialize (alength, msgtext, true);
		// We use the just-allocated msgtext; it gets freed after
		// delivery of message.

		// Restore saved state
		smp->ms_to_sc = ms_to_sc;
		smp->need_repack = need_repack;

		smp->srcaddrlen = 0;
		if (!my_network.parse_addr(netaddrstr.c_str(), smp->srcaddr, sizeof(smp->srcaddr), &smp->srcaddrlen)) {
			LOG(DEBUG) << "Parse Network address failed";
			continue;
		}
		errcode = smp->validate_short_msg(this, false);
		if (errcode == 0) {
			if (MSG_IS_REQUEST(smp->parsed)) {
				LOG(INFO) << "Read SMS '"
				     << smp->qtag << "' from "
				     << smp->parsed->from->url->username 
				     << " for "
				     << smp->parsed->req_uri->username
					  << " direction=" << (smp->ms_to_sc?"MS->SC":"SC->MS")
					  << " need_repack=" << (smp->need_repack?"true":"false");
				// Fixed error where invalid messages were getting put in the queue
				insert_new_message (*smpl, mystate, mytime); // In read_queue_from_file
			} else {
				LOG(DEBUG) << "Read bad SMS "
				     << smp->parsed->status_code
				     << " Response '"
				     << smp->qtag << "':" << msgtext;
				howmanyerrs++;
			}
		} else {
			LOG(WARNING) << "Received bad message, error " << errcode;
			// Don't log message data it's invalid and should not be accessed
			howmanyerrs++;
			// Continue to next message
		}
		delete smpl;
	}  // Message loop
	LOG(INFO) << "=== Read " << howmany << " messages total, " << howmanyerrs
	     << " bad ones.";

	// If errors clear file so we don't process again if a restart happens because the file is bad
	if (howmanyerrs != 0) ifile.clear();

	ifile.close();

	return true;
} // read_queue_from_file


/* Print net addr in hex.  Returns a static buffer.  */
char *
netaddr_fmt(char *srcaddr, unsigned len)
{
	static char buffer[999];
	char *bufp = buffer;

	buffer[0] = 0;
	*bufp++ = '=';
	while (len > 0) {
		snprintf(bufp, 3, "%02x", *(unsigned char *)srcaddr);
		bufp+= 2;
		len--;
	}
	return buffer;
}


/* Parse a string of hex into a net address and length */
bool
netaddr_parse(const char *str, char *addr, unsigned int *len)
{
	if (str[0] != '=')
		return false;
	str++;
	int xlen = strlen(str);
	int retlen = xlen/2;
	if (xlen != retlen*2)
		return false;
	*len = retlen;
	char *myaddr = addr;
	const char *strp = str;
	while (retlen > 0) {
		char blah[3];
		unsigned int mybyte;
		blah[0] = strp[0]; blah[1] = strp[1]; blah[2] = '\0';
		sscanf(blah, "%x", &mybyte);
		*myaddr++ = mybyte;
		strp+= 2;
		retlen--;
	}
	return true;
}



#if 0
/*
 * Read in a message from a file.  Return malloc'd char block of the whole
 * thing.
 */
char *
read_msg_text_from_file(char *fname, size_t *lptr)
{
	ifstream ifs;
	size_t length, got;
	char *mess;
	int i;

	ifs.open (fname, ios::in | ios::binary | ios::ate);
	if (!ifs.is_open())
		return NULL;
	length = ifs.tellg();	// we opened it at the end
	mess = new char[length+1];
	if (!mess) {
		ifs.close();
		*lptr = length;		// may as well return the length
		return NULL;		// even though we fail.
	}
	ifs.seekg (0, ios::beg);	// Now go back to the beginning.
	got = ifs.readsome (mess, length+1);
	mess[length] = '\0';		// terminate the string
	i = ifs.eof();			// We should be at EOF.
		// but due to bugs in iostreams, we aren't.  FIXME.
	i = ifs.bad();			// But we shouldn't have bad().
	ifs.close();
	if (i || got != length)
		return NULL;
	*lptr = length;
	return mess;
} // read_msg_text_from_file


// FIXME - this needs work to adjust to the new insert_new_message
// memory allocation paradigm.
short_msg_pending *
read_short_msg_pending_from_file(char *fname)
{
	short_msg_pending *smp;
	size_t length;
	char *sip_text;

	sip_text = read_msg_text_from_file (fname, &length);
	if (!sip_text)
		return NULL;

	// FIXME!  We should also be able to read the state, timeout, etc. svgfix
	// FIXME!  This'll be needed for writing/restoring the queue.
	
	smp = new short_msg_pending (length, sip_text, true);
	return smp;
}

#endif



/* Really simple first try */
int
main(int argc, char **argv)
{
	// Configure the logger.
	gLogInit("smqueue",gConfig.getStr("Log.Level").c_str(),LOG_LOCAL7);
	//gLogInitWithFile("smqueue", gConfig.getStr("Log.Level").c_str(), LOG_LOCAL7, "/var/log/smqueue.log");  // Only for dedug

	smq.stop_main_loop = false;

	if (argc > 1) {
		for (int argi = 0; argi < argc; argi++) {
			if (!strcmp(argv[argi], "--version") ||
				!strcmp(argv[argi], "-v")) {
				cout << gVersionString << endl;
			}
			if (!strcmp(argv[argi], "--gensql")) {
				cout << gConfig.getDefaultSQL(string(argv[0]), gVersionString) << endl;
			}
			if (!strcmp(argv[argi], "--gentex")) {
				cout << gConfig.getTeX(string(argv[0]), gVersionString) << endl;
			}
		} // for
		smq.argc = argc;
		smq.argv = argv;
		return 0;
	} // if

	LOG(ALERT) << "smqueue (re)starting";
	cout << "smqueue logs to syslogd facility LOCAL7, so there's not much to see here" << endl;

	// Start the reader and writer threads
	SmqMessageHandler::StartThreads();

	// Start tester threads
	StartTestThreads();  // Disabled in in function StartTestThreads

	// Don't let thread exit
	while (!smq.stop_main_loop) {
		msSleep(2000);
	}

	return 0;
}


ConfigurationKeyMap getConfigurationKeys()
{
	ConfigurationKeyMap map;
	ConfigurationKey *tmp;
	//Don't log in here log lib is not ready

	tmp = new ConfigurationKey("Asterisk.address","127.0.0.1:5060",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::IPANDPORT,
		"",
		false,
		"The Asterisk/SIP PBX IP address and port."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("Bounce.Code","101",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"The short code that bounced messages originate from."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("Bounce.Message.IMSILookupFailed","Cannot determine return address; bouncing message.  Text your phone number to 101 to register and try again.",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"The bounce message that is sent when the originating IMSI cannot be verified."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("Bounce.Message.NotRegistered","Phone not registered here.",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"Bounce message indicating that the destination phone is not registered."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("CDRFile","/var/lib/OpenBTS/smq.cdr",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::FILEPATH_OPT,// audited
		"",
		false,
		"Log CDRs here.  "
		"To enable, specify an absolute path to where the CDRs should be logged.  "
		"To disable, execute \"unconfig CDRFile\"."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("Debug.print_as_we_validate","0",
		"",
		ConfigurationKey::DEVELOPER,
		ConfigurationKey::BOOLEAN,
		"",
		false,
		"Generate lots of output during validation."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("savefile","/tmp/save",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::FILEPATH,
		"",
		false,
		"The file to save SMS messages to when exiting."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.DebugDump.Code","2336",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Short code to the application which dumps debug information to the log.  Intended for administrator use."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Info.Code","411",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Short code to the application which tells the sender their own number and registration status."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.QuickChk.Code","2337",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Short code to the application which tells the sender the how many messages are currently queued.  Intended for administrator use."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Code","101",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Short code to the application which registers the sender to the system."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Digits.Max","10",
		"digits",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::VALRANGE,
		"7:10",// educated guess
		false,
		"The maximum number of digits a phone number can have."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Digits.Min","7",
		"digits",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::VALRANGE,
		"7:10",// educated guess
		false,
		"The minimum number of digits a phone number must have."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Digits.Override","0",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::BOOLEAN,
		"",
		false,
		"Ignore phone number digit length checks."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.AlreadyA","Your phone is already registered as",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"First part of message sent during registration if the handset is already registered, followed by the current handset number."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.AlreadyB",".",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"Second part of message sent during registration if the handset is already registered."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.ErrorA","Error in assigning",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"First part of message sent during registration if the handset fails to register, followed by the attempted handset number."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.ErrorB","to IMSI",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"Second part of message sent during registration if the handset fails to register, followed by the handset IMSI."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.TakenA","The phone number",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"First part of message sent during registration if the handset fails to register because the desired number is already taken, followed by the attempted handset number."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.TakenB","is already in use. Try another, then call that one to talk to whoever took yours.",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"Second part of message sent during registration if the handset fails to register because the desired number is already taken."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.WelcomeA","Hello",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"First part of message sent during registration if the handset registers successfully, followed by the assigned handset number."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.Register.Msg.WelcomeB","! Text to 411 for system status.",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[ -~]+$",
		false,
		"Second part of message sent during registration if the handset registers successfully."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.SMSC.Code","smsc",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[a-zA-Z]+$",
		false,
		"The SMSC entry point. There is where OpenBTS sends SIP MESSAGES to."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.SMSC.Code","smsc",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[a-zA-Z]+$",
		false,
		"The SMSC entry point. There is where OpenBTS sends SIP MESSAGES to."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	// TODO : this should be made optional and default to off
	// TODO : safety check on .defines() vs .length()
	tmp = new ConfigurationKey("SC.WhiplashQuit.Code","314158",
		"",
		ConfigurationKey::DEVELOPER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Short code to the application which will make the server quit for valgrind leak checking.  Intended for developer use only."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.WhiplashQuit.Password","Snidely",
		"",
		ConfigurationKey::DEVELOPER,
		ConfigurationKey::STRING,
		"^[a-zA-Z0-9]+$",
		false,
		"Password which must be sent in the message to the application at SC.WhiplashQuit.Code."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.WhiplashQuit.SaveFile","testsave.txt",
		"",
		ConfigurationKey::DEVELOPER,
		ConfigurationKey::FILEPATH,
		"",
		false,
		"Contents of the queue will be dumped to this file when SC.WhiplashQuit.Code is activated."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.ZapQueued.Code","2338",
		"",
		ConfigurationKey::DEVELOPER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Short code to the application which will remove a message from the queue, by its tag.  "
			"If first char is \"-\", do not reply, just do it.  "
			"If argument is SC.ZapQueued.Password, then delete any queued message with timeout greater than 5000 seconds."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SC.ZapQueued.Password","6000",
		"",
		ConfigurationKey::DEVELOPER,
		ConfigurationKey::STRING,
		"^[a-zA-Z0-9]+$",
		false,
		"Password which must be sent in the message to the application at SC.ZapQueued.Code."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.Default.BTSPort","5062",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::PORT,
		"",
		false,
		"The default BTS port to try when none is available."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.GlobalRelay.ContentType","application/vnd.3gpp.sms",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::CHOICE,
		"application/vnd.3gpp.sms,"
			"text/plain",
		true,
		"The content type that the global relay expects."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.GlobalRelay.IP","",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::IPADDRESS_OPT,// audited
		"",
		true,
		"IP address of global relay to send unresolvable messages to.  "
			"By default, this is disabled.  "
			"To override, specify an IP address.  "
			"To disable again use \"unconfig SIP.GlobalRelay.IP\"."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.GlobalRelay.Port","",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::PORT_OPT,// audited
		"",
		true,
		"Port of global relay to send unresolvable messages to."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.GlobalRelay.RelaxedVerify","0",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::BOOLEAN,
		"",
		true,
		"Relax relay verification by only using SIP Header."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.Timeout.ACKedMessageResend","60",
		"seconds",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"45:360",// educated guess
		false,
		"Number of seconds to delay resending ACK messages."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.Timeout.MessageBounce","120",
		"seconds",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"45:360",// educated guess
		true,
		"Timeout, in seconds, between bounced message sending tries."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.Timeout.MessageResend","120",
		"seconds",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"45:360",// educated guess
		true,
		"Timeout, in seconds, between message sending tries."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SIP.myPort","5063",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::PORT,
		"",
		false,
		"The port that smqueue should bind to."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SMS.FakeSrcSMSC","0000",
		"",
		ConfigurationKey::CUSTOMER,
		ConfigurationKey::STRING,
		"^[0-9]{3,6}$",
		false,
		"Use this to fill in L4 SMSC address in SMS delivery."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SMS.HTTPGateway.Retries","5",
		"retries",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"2:8",// educated guess
		false,
		"Maximum retries for HTTP gateway attempt."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SMS.HTTPGateway.Timeout","5",
		"seconds",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"2:8",// educated guess
		false,
		"Timeout for HTTP gateway attempt in seconds."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SMS.HTTPGateway.URL","",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::STRING_OPT,// audited
		"^(http|https)://[[:alnum:]_.-]",
		false,
		"URL for HTTP API.  "
			"Used directly as a C format string with two \"%s\" substitutions.  "
			"First \"%s\" gets replaced with the destination number.  "
			"Second \"%s\" gets replaced with the URL-endcoded message body."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SMS.MaxRetries","2160",// by default 2160 * 120sec-per-retry = 3 days
		"",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"0:5040",
		false,
		"Messages will only be attempted to be sent this many times before giving up and being dropped. Set to 0 to allow infinite retries."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SMS.RateLimit","0",
		"seconds",
		ConfigurationKey::CUSTOMERTUNE,
		ConfigurationKey::VALRANGE,
		"0:15",
		false,
		"Limit delivery rate to one message every X seconds. Set to 0 to disable rate limiting."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	// TODO : pretty sure this isn't used anywhere...
	tmp = new ConfigurationKey("SubscriberRegistry.A3A8","../comp128",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::FILEPATH,
		"",
		false,
		"Path to the program that implements the A3/A8 algorithm."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SubscriberRegistry.db","/var/lib/asterisk/sqlite3dir/sqlite3.db",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::FILEPATH,
		"",
		false,
		"The location of the sqlite3 database holding the subscriber registry."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	tmp = new ConfigurationKey("SubscriberRegistry.UpstreamServer","",
		"",
		ConfigurationKey::CUSTOMERWARN,
		ConfigurationKey::STRING_OPT,// audited
		"",
		false,
		"URL of the subscriber registry HTTP interface on the upstream server.  "
			"By default, this feature is disabled.  "
			"To enable, specify a server URL eg: http://localhost/cgi/subreg.cgi.  "
			"To disable again, execute \"unconfig SubscriberRegistry.UpstreamServer\"."
	);
	map[tmp->getName()] = *tmp;
	delete tmp;

	return map;

}


