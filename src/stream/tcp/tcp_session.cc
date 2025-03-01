//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

/*
 * stream_tcp.c authors:
 *     Martin Roesch <roesch@sourcefire.com>
 *     Steven Sturges <ssturges@sourcefire.com>
 *     Russ Combs <rcombs@sourcefire.com>
 */

/*
 * FIXITs:
 * - midstream ssn pickup (done, SAS 10/14/2005)
 * - syn flood protection (done, SAS 9/27/2005)
 *
 * - review policy anomaly detection
 *   + URG pointer (TODO)
 *   + data on SYN (done, SAS 10/12/2005)
 *   + data on FIN (done, SAS 10/12/2005)
 *   + data after FIN (done, SAS 10/13/2005)
 *   + window scaling/window size max (done, SAS 10/13/2005)
 *   + PAWS, TCP Timestamps (done, SAS 10/12/2005)
 *
 * - session shutdown/Reset handling (done, SAS)
 * - flush policy for Window/Consumed
 * - limit on number of overlapping packets (done, SAS)
 */

#include "tcp_session.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <assert.h>

#include "stream_tcp.h"
#include "tcp_module.h"

// TBD-EDM - these includes are for functions moved to a new file to group related functionality
// in specific files ... these functional groups will be further refactored as the stream tcp
// rewrite continues...
#include "tcp_events.h"
#include "tcp_debug_trace.h"

#include "stream/libtcp/tcp_state_handler.h"
#include "tcp_closed_state.h"
#include "tcp_listen_state.h"
#include "tcp_syn_sent_state.h"
#include "tcp_syn_recv_state.h"
#include "tcp_normalizers.h"
#include "tcp_reassemblers.h"
// TBD-EDM

#include "main/snort_types.h"
#include "main/snort_debug.h"
#include "main/snort_config.h"
#include "main/analyzer.h"
#include "detection/detect.h"
#include "detection/detection_util.h"
#include "hash/sfxhash.h"
#include "utils/util.h"
#include "utils/sflsq.h"
#include "utils/snort_bounds.h"
#include "time/packet_time.h"
#include "protocols/packet.h"
#include "protocols/packet_manager.h"
#include "protocols/tcp_options.h"
#include "protocols/tcp.h"
#include "protocols/eth.h"
#include "log/log_text.h"
#include "stream/stream.h"
#include "stream/stream_splitter.h"
#include "flow/flow_control.h"
#include "flow/session.h"
#include "flow/memcap.h"
#include "time/profiler.h"
#include "file_api/file_api.h"
#include "sfip/sf_ip.h"
#include "filters/sfrf.h"

using namespace tcp;

/*  M A C R O S  **************************************************/

#define STREAM_UNALIGNED       0
#define STREAM_ALIGNED         1

#define STREAM_DEFAULT_MAX_QUEUED_BYTES 1048576 /* 1 MB */
#define AVG_PKT_SIZE            400
#define STREAM_DEFAULT_MAX_QUEUED_SEGS (STREAM_DEFAULT_MAX_QUEUED_BYTES/AVG_PKT_SIZE)

#define STREAM_DEFAULT_MAX_SMALL_SEG_SIZE 0    /* disabled */
#define STREAM_DEFAULT_CONSEC_SMALL_SEGS 0     /* disabled */

#define SLAM_MAX 4

/*  P R O T O T Y P E S  ********************************************/

static int ProcessTcp(Flow*, TcpDataBlock*, StreamTcpConfig*);

/*  G L O B A L S  **************************************************/

static const char* const reassembly_policy_names[] = { "no policy", "first",
    "last", "linux", "old_linux", "bsd", "macos", "solaris", "irix",
    "hpux11", "hpux10", "windows", "win_2003", "vista", "proxy" };

#ifdef DEBUG
static const char* const state_names[] =
{
    "none",
    "listen",
    "syn_rcvd",
    "syn_sent",
    "established",
    "close_wait",
    "last_ack",
    "fin_wait_1",
    "closing",
    "fin_wait_2",
    "time_wait",
    "closed"
};

const char* const flush_policy_names[] =
{
    "ignore",
    "on-ack",
    "on-data"
};
#endif

//-------------------------------------------------------------------------
// flush policy stuff
//-------------------------------------------------------------------------

static inline void init_flush_policy(Flow*, TcpTracker* trk)
{
    if (!trk->splitter)
        trk->flush_policy = STREAM_FLPOLICY_IGNORE;
    else if (!trk->normalizer->is_tcp_ips_enabled() )
        trk->flush_policy = STREAM_FLPOLICY_ON_ACK;
    else
        trk->flush_policy = STREAM_FLPOLICY_ON_DATA;
}

void StreamUpdatePerfBaseState(SFBASE *sf_base, Flow *flow, char newState)
{
    if (!flow)
        return;

    uint32_t session_flags = flow->get_session_flags();
    switch (newState)
    {
        case TCP_STATE_SYN_SENT:
            if (!(session_flags & SSNFLAG_COUNTED_INITIALIZE))
            {
                sf_base->iSessionsInitializing++;
                session_flags |= SSNFLAG_COUNTED_INITIALIZE;
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (!(session_flags & SSNFLAG_COUNTED_ESTABLISH))
            {
                sf_base->iSessionsEstablished++;

                if (perfmon_config && (perfmon_config->perf_flags & SFPERF_FLOWIP))
                    UpdateFlowIPState(&sfFlow, &flow->client_ip, &flow->server_ip, SFS_STATE_TCP_ESTABLISHED);

                session_flags |= SSNFLAG_COUNTED_ESTABLISH;

                if ((session_flags & SSNFLAG_COUNTED_INITIALIZE)
                        && !(session_flags & SSNFLAG_COUNTED_CLOSING))
                {
                    assert(sf_base->iSessionsInitializing);
                    sf_base->iSessionsInitializing--;
                }
            }
            break;

        case TCP_STATE_CLOSING:
            if (!(session_flags & SSNFLAG_COUNTED_CLOSING))
            {
                sf_base->iSessionsClosing++;
                session_flags |= SSNFLAG_COUNTED_CLOSING;

                if (session_flags & SSNFLAG_COUNTED_ESTABLISH)
                {
                    assert(sf_base->iSessionsEstablished);
                    sf_base->iSessionsEstablished--;

                    if (perfmon_config  && (perfmon_config->perf_flags & SFPERF_FLOWIP))
                        UpdateFlowIPState(&sfFlow, &flow->client_ip, &flow->server_ip, SFS_STATE_TCP_CLOSED);
                }
                else if (session_flags & SSNFLAG_COUNTED_INITIALIZE)
                {
                    assert(sf_base->iSessionsInitializing);
                    sf_base->iSessionsInitializing--;
                }
            }
            break;

        case TCP_STATE_CLOSED:
            if ( session_flags & SSNFLAG_COUNTED_CLOSING )
            {
                assert(sf_base->iSessionsClosing);
                sf_base->iSessionsClosing--;
            }
            else if (session_flags & SSNFLAG_COUNTED_ESTABLISH)
            {
                assert(sf_base->iSessionsEstablished);
                sf_base->iSessionsEstablished--;

                if (perfmon_config && (perfmon_config->perf_flags & SFPERF_FLOWIP))
                    UpdateFlowIPState(&sfFlow, &flow->client_ip, &flow->server_ip, SFS_STATE_TCP_CLOSED);
            }
            else if (session_flags & SSNFLAG_COUNTED_INITIALIZE)
            {
                assert(sf_base->iSessionsInitializing);
                sf_base->iSessionsInitializing--;
            }
            break;

        default:
            break;
    }

    flow->update_session_flags( session_flags );
    sf_base->stream_mem_in_use = tcp_memcap->used();
}

//-------------------------------------------------------------------------
// config methods
//-------------------------------------------------------------------------

StreamTcpConfig::StreamTcpConfig()
{
    policy = StreamPolicy::OS_DEFAULT;
    reassembly_policy = ReassemblyPolicy::OS_DEFAULT;

    flags = 0;
    flush_factor = 0;

    session_timeout = STREAM_DEFAULT_SSN_TIMEOUT;
    max_window = 0;
    overlap_limit = 0;

    max_queued_bytes = STREAM_DEFAULT_MAX_QUEUED_BYTES;
    max_queued_segs = STREAM_DEFAULT_MAX_QUEUED_SEGS;

    max_consec_small_segs = STREAM_DEFAULT_CONSEC_SMALL_SEGS;
    max_consec_small_seg_size = STREAM_DEFAULT_MAX_SMALL_SEG_SIZE;

    hs_timeout = -1;
    footprint = 0;
    paf_max = 16384;
}

inline bool StreamTcpConfig::require_3whs()
{
    return hs_timeout >= 0;
}

inline bool StreamTcpConfig::midstream_allowed(Packet* p)
{
    if ((hs_timeout < 0) || (p->pkth->ts.tv_sec - packet_first_time() < hs_timeout))
        return true;

    return false;
}

//-------------------------------------------------------------------------
// when client ports are configured, that means c2s and is stored on the
// client side; when the session starts, the server policy is obtained from
// the client side because segments are stored on the receiving side.
//
// this could be improved further by storing the c2s policy on the server
// side and then obtaining server policy from the server on session
// startup.
//
// either way, this client / server distinction must be kept in mind to
// make sense of the code in this file.
//-------------------------------------------------------------------------

static void StreamPrintTcpConfig(StreamTcpConfig* config)
{
    LogMessage("Stream TCP Policy config:\n");
    LogMessage("    Reassembly Policy: %s\n",
            reassembly_policy_names[ static_cast<int>( config->reassembly_policy ) ] );
    LogMessage("    Timeout: %d seconds\n", config->session_timeout);

    if (config->max_window != 0)
        LogMessage("    Max TCP Window: %u\n", config->max_window);

    if (config->overlap_limit)
        LogMessage("    Limit on TCP Overlaps: %d\n", config->overlap_limit);

    if (config->max_queued_bytes != 0)
        LogMessage("    Maximum number of bytes to queue per session: %d\n", config->max_queued_bytes);

    if (config->max_queued_segs != 0)
        LogMessage("    Maximum number of segs to queue per session: %d\n", config->max_queued_segs);

    if (config->flags)
    {
        LogMessage("    Options:\n");
        if (config->flags & STREAM_CONFIG_IGNORE_ANY)
            LogMessage("        Ignore Any -> Any Rules: YES\n");

        if (config->flags & STREAM_CONFIG_NO_ASYNC_REASSEMBLY)
            LogMessage( "        Don't queue packets on one-sided sessions: YES\n");
    }

    if (config->hs_timeout < 0)
        LogMessage("    Require 3-Way Handshake: NO\n");
    else
        LogMessage("    Require 3-Way Handshake: after %d seconds\n", config->hs_timeout);

#ifdef REG_TEST
    LogMessage("    TCP Session Size: %lu\n",sizeof(TcpSession));
#endif
}

//-------------------------------------------------------------------------
// attribute table foo
//-------------------------------------------------------------------------

int StreamVerifyTcpConfig(SnortConfig*, StreamTcpConfig*)
{
    return 0;
}

#ifdef DEBUG_STREAM_EX
static void PrintStateMgr(StateMgr* s)
{
    LogMessage("StateMgr:\n");
    LogMessage("    state:          %s\n", state_names[s->state]);
    LogMessage("    state_queue:    %s\n", state_names[s->state_queue]);
    LogMessage("    expected_flags: 0x%X\n", s->expected_flags);
    LogMessage("    transition_seq: 0x%X\n", s->transition_seq);
    LogMessage("    stq_get_seq:    %d\n", s->stq_get_seq);
}

static void PrintTcpTracker(TcpTracker *s)
{
    LogMessage(" + TcpTracker +\n");
    LogMessage("    isn:                0x%X\n", s->isn);
    LogMessage("    ts_last:            %u\n", s->ts_last);
    LogMessage("    wscale:             %u\n", s->wscale);
    LogMessage("    mss:                0x%08X\n", s->mss);
    LogMessage("    l_unackd:           %X\n", s->l_unackd);
    LogMessage("    l_nxt_seq:          %X\n", s->l_nxt_seq);
    LogMessage("    l_window:           %u\n", s->l_window);
    LogMessage("    r_nxt_ack:          %X\n", s->r_nxt_ack);
    LogMessage("    r_win_base:         %X\n", s->r_win_base);
    LogMessage("    seglist_base_seq:   %X\n", s->reassembler->get_seglist_base_seq( ));
    LogMessage("    seglist:            %p\n", (void*)s->seglist);
    LogMessage("    seglist_tail:       %p\n", (void*)s->seglist_tail);
    LogMessage("    seg_count:          %d\n", s->reassembler->get_seg_count());
    LogMessage("    seg_bytes_total:    %d\n", s->reassembler->get_seg_bytes_total());
    LogMessage("    seg_bytes_logical:  %d\n", s->get_seg_bytes_logical() );

    PrintStateMgr(&s->s_mgr);
}

static void PrintTcpSession(TcpSession* ts)
{
    char buf[64];

    LogMessage("TcpSession:\n");
    sfip_ntop(&ts->flow->server_ip, buf, sizeof(buf));
    LogMessage("    server IP:          %s\n", buf);
    sfip_ntop(&ts->flow->client_ip, buf, sizeof(buf));
    LogMessage("    client IP:          %s\n", buf);

    LogMessage("    server port:        %d\n", ts->flow->server_port);
    LogMessage("    client port:        %d\n", ts->flow->client_port);

    LogMessage("    flags:              0x%X\n", ts->flow->get_session_flags());

    LogMessage("Client Tracker:\n");
    PrintTcpTracker(&ts->client);
    LogMessage("Server Tracker:\n");
    PrintTcpTracker(&ts->server);
}

static void PrintTcpDataBlock(TcpDataBlock* tdb)
{
    LogMessage("TcpDataBlock:\n");
    LogMessage("    seq:    0x%08X\n", tdb->seq);
    LogMessage("    ack:    0x%08X\n", tdb->ack);
    LogMessage("    win:    %d\n", tdb->win);
    LogMessage("    end:    0x%08X\n", tdb->end_seq);
}

static void PrintFlushMgr(FlushMgr* fm)
{
    if (fm == NULL)
        return;

    switch (fm->flush_policy)
    {
        case STREAM_FLPOLICY_IGNORE:
            DebugMessage( DEBUG_STREAM_STATE, "    IGNORE\n");
            break;

        case STREAM_FLPOLICY_ON_ACK:
            DebugMessage( DEBUG_STREAM_STATE, "    PROTOCOL\n");
            break;

        case STREAM_FLPOLICY_ON_DATA:
            DebugMessage( DEBUG_STREAM_STATE, "    PROTOCOL_IPS\n");
            break;
    }
}

#endif  // DEBUG_STREAM_EX


//-------------------------------------------------------------------------
// ssn ingress is client; ssn egress is server

//-------------------------------------------------------------------------

static inline int IsBetween(uint32_t low, uint32_t high, uint32_t cur)
{
    DebugFormat(DEBUG_STREAM_STATE, "(%X, %X, %X) = (low, high, cur)\n", low,high,cur);

    /* If we haven't seen anything, ie, low & high are 0, return true */
    if ((low == 0) && (low == high))
        return 1;

    return (SEQ_GEQ(cur, low) && SEQ_LEQ(cur, high));
}


// ack number must ack syn
static inline int ValidRstSynSent(TcpTracker *st, TcpDataBlock *tdb)
{
    return tdb->ack == st->l_unackd;
}


#ifdef S5_PEDANTIC
// From RFC 793:
//
//    Segment Receive  Test
//    Length  Window
//    ------- -------  -------------------------------------------
//
//       0       0     SEG.SEQ = RCV.NXT
//
//       0      >0     RCV.NXT =< SEG.SEQ < RCV.NXT+RCV.WND
//
//      >0       0     not acceptable
//
//      >0      >0     RCV.NXT =< SEG.SEQ < RCV.NXT+RCV.WND
//                     or RCV.NXT =< SEG.SEQ+SEG.LEN-1 < RCV.NXT+RCV.WND
//
static inline int ValidSeq( const Packet* p, Flow* flow, TcpTracker *st, TcpDataBlock *tdb )
{
    uint32_t win = st->normalizer->get_stream_window(flow, st, tdb);

    if ( !p->dsize )
    {
        if ( !win )
        {
            return ( tdb->seq == st->r_win_base );
        }
        return SEQ_LEQ(st->r_win_base, tdb->seq) &&
            SEQ_LT(tdb->seq, st->r_win_base+win);
    }
    if ( !win )
        return 0;

    if ( SEQ_LEQ(st->r_win_base, tdb->seq) &&
            SEQ_LT(tdb->seq, st->r_win_base+win) )
        return 1;

    return SEQ_LEQ(st->r_win_base, tdb->end_seq) &&
        SEQ_LT(tdb->end_seq, st->r_win_base+win);
}

#else
static inline int ValidSeq( TcpTracker *st, TcpDataBlock *tdb)
{
    int right_ok;
    uint32_t left_seq;

    DebugFormat(DEBUG_STREAM_STATE, "Checking end_seq (%X) > r_win_base (%X) && seq (%X) < r_nxt_ack(%X)\n",
            tdb->end_seq, st->r_win_base, tdb->seq, st->r_nxt_ack + st->normalizer->get_stream_window( tdb ));

    if (SEQ_LT(st->r_nxt_ack, st->r_win_base))
        left_seq = st->r_nxt_ack;
    else
        left_seq = st->r_win_base;

    if (tdb->pkt->dsize)
        right_ok = SEQ_GT(tdb->end_seq, left_seq);
    else
        right_ok = SEQ_GEQ(tdb->end_seq, left_seq);

    if (right_ok)
    {
        uint32_t win = st->normalizer->get_stream_window( tdb );

        if (SEQ_LEQ(tdb->seq, st->r_win_base + win))
        {
            DebugMessage(DEBUG_STREAM_STATE, "seq is within window!\n");
            return 1;
        } else
        {
            DebugMessage(DEBUG_STREAM_STATE, "seq is past the end of the window!\n");
        }
    }
    else
    {
        DebugMessage(DEBUG_STREAM_STATE, "end_seq is before win_base\n");
    }
    return 0;
}

#endif

static inline void UpdateSsn( TcpTracker *rcv, TcpTracker *snd, TcpDataBlock *tdb)
{
#if 0
    if (
            // FIXIT-L these checks are a hack to avoid off by one normalization
            // due to FIN ... if last segment filled a hole, r_nxt_ack is not at
            // end of data, FIN is ignored so sequence isn't bumped, and this
            // forces seq-- on ACK of FIN.  :(
        rcv->s_mgr.state == TCP_STATE_ESTABLISHED &&
            rcv->s_mgr.state_queue == TCP_STATE_NONE &&
            Normalize_IsEnabled(NORM_TCP_IPS) )
            {
            // walk the seglist until a gap or tdb->ack whichever is first
            // if a gap exists prior to ack, move ack back to start of gap
            TcpSegment* seg = snd->seglist;

            // FIXIT-L must check ack oob with empty seglist
            // FIXIT-L add lower gap bound to tracker for efficiency?
            while ( seg )
            {
            uint32_t seq = seg->seq + seg->size;
            if ( SEQ_LEQ(tdb->ack, seq) )
            break;

            seg = seg->next;

            if ( !seg || seg->seq > seq )
            {
                // normalize here
                tdb->ack = seq;
                tcph->th_ack = htonl(seq);
                p->packet_flags |= PKT_MODIFIED;
                break;
            }
            }
            }
#endif
    // ** if we don't see a segment, we can't track seq at ** below
    // so we update the seq by the ack if it is beyond next expected
    if (SEQ_GT(tdb->ack, rcv->l_unackd))
        rcv->l_unackd = tdb->ack;

    // ** this is how we track the last seq number sent
    // as is l_unackd is the "last left" seq recvd
    snd->l_unackd = tdb->seq;

    if (SEQ_GT(tdb->end_seq, snd->l_nxt_seq))
        snd->l_nxt_seq = tdb->end_seq;

    if (!SEQ_EQ(snd->r_win_base, tdb->ack))
    {
        snd->small_seg_count = 0;
    }
#ifdef S5_PEDANTIC
    if ( SEQ_GT(tdb->ack, snd->r_win_base) &&
            SEQ_LEQ(tdb->ack, snd->r_nxt_ack) )
#else
        if (SEQ_GT(tdb->ack, snd->r_win_base))
#endif
            snd->r_win_base = tdb->ack;

    snd->l_window = tdb->win;
}

static inline void SetupTcpDataBlock(TcpDataBlock* tdb, Packet* p)
{
    tdb->pkt = p;
    tdb->seq = ntohl(p->ptrs.tcph->th_seq);
    tdb->ack = ntohl(p->ptrs.tcph->th_ack);
    tdb->win = ntohs(p->ptrs.tcph->th_win);
    tdb->end_seq = tdb->seq + (uint32_t) p->dsize;
    tdb->ts = 0;

    if( p->ptrs.tcph->is_syn() )
    {
        tdb->end_seq++;
        if( !p->ptrs.tcph->is_ack() )
            EventInternal(INTERNAL_EVENT_SYN_RECEIVED);
    }
    // don't bump end_seq for fin here
    // we will bump if/when fin is processed

#ifdef DEBUG_STREAM_EX
    PrintTcpDataBlock(&tdb);
#endif
}


static void TcpSessionClear(Flow* flow, TcpSession* tcpssn, int freeApplicationData)
{
    // update stats
    if (tcpssn->tcp_init)
        tcpStats.trackers_released++;
    else if (tcpssn->lws_init)
        tcpStats.no_pickups++;
    else
        return;

    StreamUpdatePerfBaseState(&sfBase, tcpssn->flow, TCP_STATE_CLOSED);
    RemoveStreamSession(&sfBase);

    if (flow->get_session_flags() & SSNFLAG_PRUNED)
        CloseStreamSession(&sfBase, SESSION_CLOSED_PRUNED);
    else if (flow->get_session_flags() & SSNFLAG_TIMEDOUT)
        CloseStreamSession(&sfBase, SESSION_CLOSED_TIMEDOUT);
    else
        CloseStreamSession(&sfBase, SESSION_CLOSED_NORMALLY);

    tcpssn->set_splitter(true, nullptr);
    tcpssn->set_splitter(false, nullptr);

    DebugFormat(DEBUG_STREAM_STATE, "In TcpSessionClear, %lu bytes in use\n", tcp_memcap->used());

    if( tcpssn->client.reassembler )
    {
        DebugFormat(DEBUG_STREAM_STATE, "client has %d segs queued, freeing all.\n", tcpssn->client.reassembler->get_seg_count());
        tcpssn->client.reassembler->purge_segment_list( );
    }

    if( tcpssn->server.reassembler )
    {
        DebugFormat(DEBUG_STREAM_STATE, "server has %d segs queued, freeing all\n", tcpssn->server.reassembler->get_seg_count());
        tcpssn->server.reassembler->purge_segment_list( );
    }

    paf_clear(&tcpssn->client.paf_state);
    paf_clear(&tcpssn->server.paf_state);

    // update light-weight state
    if (freeApplicationData == 2)
        flow->restart(true);
    else
        flow->clear(freeApplicationData);

    // generate event for rate filtering
    EventInternal(INTERNAL_EVENT_SESSION_DEL);

    DebugFormat(DEBUG_STREAM_STATE, "After cleaning, %lu bytes in use\n", tcp_memcap->used());

    tcpssn->lws_init = tcpssn->tcp_init = false;
}


static void TcpSessionCleanup(Flow* flow, int freeApplicationData, Packet* p =
        nullptr)
{
    TcpSession* tcpssn = (TcpSession*) flow->session;
    // FIXIT - this function does both client & server sides...refactor to do one and
    // call for each
    if( tcpssn->client.reassembler != nullptr )
        tcpssn->client.reassembler->flush_queued_segments(flow, true, p);
    if( tcpssn->server.reassembler != nullptr )
        tcpssn->server.reassembler->flush_queued_segments(flow, true, p);
    TcpSessionClear(flow, tcpssn, freeApplicationData);
}


static uint32_t StreamGetMss( Packet* p, uint16_t* value )
{
    DebugMessage(DEBUG_STREAM_STATE, "Getting MSS...\n");

    TcpOptIterator iter(p->ptrs.tcph, p);
    for (const TcpOption& opt : iter)
    {
        if (opt.code == TcpOptCode::MAXSEG)
        {
            *value = EXTRACT_16BITS(opt.data);
            DebugFormat(DEBUG_STREAM_STATE, "Found MSS %u\n", *value);
            return TF_MSS;
        }
    }

    *value = 0;

    DebugMessage(DEBUG_STREAM_STATE, "No MSS...\n");

    return TF_NONE;
}

static uint32_t StreamGetWscale(Packet* p, uint16_t* value)
{
    DebugMessage(DEBUG_STREAM_STATE, "Getting wscale...\n");

    TcpOptIterator iter(p->ptrs.tcph, p);

    // using const because non-const is not supported
    for (const TcpOption& opt : iter)
    {
        if (opt.code == TcpOptCode::WSCALE)
        {
            *value = (uint16_t) opt.data[0];
            DebugFormat(DEBUG_STREAM_STATE, "Found wscale %d\n", *value);

            /* If scale specified in option is larger than 14,
             * use 14 because of limitation in the math of
             * shifting a 32bit value (max scaled window is 2^30th).
             *
             * See RFC 1323 for details.
             */
            if (*value > 14)
                *value = 14;

            return TF_WSCALE;
        }
    }

    *value = 0;
    DebugMessage(DEBUG_STREAM_STATE, "No wscale...\n");

    return TF_NONE;
}

static uint32_t StreamPacketHasWscale( Packet* p )
{
    uint16_t wscale;

    DebugMessage(DEBUG_STREAM_STATE, "Checking for wscale...\n");

    return StreamGetWscale(p, &wscale);
}

#if 0
static inline int IsWellFormed(Packet *p, TcpTracker *ts)
{
    return ( !ts->mss || (p->dsize <= ts->mss) );
}

#endif

static void FinishServerInit( TcpDataBlock* tdb, TcpSession* ssn )
{
    TcpTracker* server;
    TcpTracker* client;
    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;

    // FIXIT - can tcp session be null at this point?
    if( !ssn )
        return;

    server = &ssn->server;
    client = &ssn->client;

    server->l_window = tdb->win;
    server->l_unackd = tdb->seq + 1;
    server->l_nxt_seq = server->l_unackd;
    server->isn = tdb->seq;

    client->r_nxt_ack = tdb->end_seq;

    if( tcph->is_fin() )
        server->l_nxt_seq--;

    if( !( ssn->flow->session_state & STREAM_STATE_MIDSTREAM ) )
    {
        server->s_mgr.state = TCP_STATE_SYN_RCVD;
        client->reassembler->set_seglist_base_seq( server->l_unackd );
        client->r_win_base = tdb->end_seq;
    }
    else
    {
        client->reassembler->set_seglist_base_seq( tdb->seq );
        client->r_win_base = tdb->seq;
    }

    server->flags |= server->normalizer->get_tcp_timestamp(tdb, false);
    server->ts_last = tdb->ts;
    if (server->ts_last == 0)
        server->flags |= TF_TSTAMP_ZERO;
    else
        server->ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;

    server->flags |= StreamGetMss(tdb->pkt, &server->mss);
    server->flags |= StreamGetWscale(tdb->pkt, &server->wscale);

#ifdef DEBUG_STREAM_EX
    PrintTcpSession(ssn);
#endif
}

static inline void EndOfFileHandle(Packet* p, TcpSession* tcpssn)
{
    tcpssn->flow->call_handlers(p, true);
}

static inline bool flow_exceeds_config_thresholds( TcpTracker *rcv, TcpSession *tcpssn, TcpDataBlock *tdb,
        StreamTcpConfig* config )
{
    if (rcv->flush_policy == STREAM_FLPOLICY_IGNORE)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Ignoring segment due to IGNORE flush_policy\n");
        return true;
    }

    if( ( config->flags & STREAM_CONFIG_NO_ASYNC_REASSEMBLY ) && !tcpssn->flow->two_way_traffic() )
        return true;

    if( config->max_consec_small_segs && ( tdb->pkt->dsize < config->max_consec_small_seg_size ) )
    {
        rcv->small_seg_count++;

        if( rcv->small_seg_count > config->max_consec_small_segs )
        {
            /* Above threshold, log it...  in this TCP policy,
             * action controlled by preprocessor rule. */
            EventMaxSmallSegsExceeded();
            /* Reset counter, so we're not too noisy */
            rcv->small_seg_count = 0;
        }
    }

    if( config->max_queued_bytes
            && ( rcv->reassembler->get_seg_bytes_total() > config->max_queued_bytes ) )
    {
        tcpStats.max_bytes++;
        return true;
    }

    if( config->max_queued_segs
            && ( rcv->reassembler->get_seg_count() + 1 > config->max_queued_segs ) )
    {
        tcpStats.max_segs++;
        return true;
    }

    return false;
}

static void ProcessTcpStream(TcpTracker *rcv, TcpSession *tcpssn, TcpDataBlock *tdb,
        StreamTcpConfig* config)
{
    DebugFormat(DEBUG_STREAM_STATE, "In ProcessTcpStream(), %d bytes to queue\n", tdb->pkt->dsize);

    if (tdb->pkt->packet_flags & PKT_IGNORE)
        return;

#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
    tcpssn->SetPacketHeaderFoo( tdb->pkt );
#endif

    if( flow_exceeds_config_thresholds( rcv, tcpssn, tdb, config ) )
        return;

    DebugMessage(DEBUG_STREAM_STATE, "queuing segment\n");
    rcv->reassembler->queue_packet_for_reassembly( tdb );

    // Alert if overlap limit exceeded
    if( ( rcv->config->overlap_limit )
            && ( rcv->reassembler->get_overlap_count() > rcv->config->overlap_limit ) )
    {
        EventExcessiveOverlap();
        rcv->reassembler->set_overlap_count( 0 );
    }
}

static int ProcessTcpData(TcpTracker *listener, TcpSession *tcpssn,
        TcpDataBlock *tdb, StreamTcpConfig *config)
{
    PERF_PROFILE(s5TcpDataPerfStats);

    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;
    uint32_t seq = tdb->seq;

    if( tcph->is_syn() )
    {
        if (listener->normalizer->get_os_policy() == StreamPolicy::OS_MACOS)
            seq++;

        else
        {
            DebugMessage(DEBUG_STREAM_STATE, "Bailing, data on SYN, not MAC Policy!\n");
            listener->normalizer->trim_syn_payload( tdb );
            return STREAM_UNALIGNED;
        }
    }

    /* we're aligned, so that's nice anyway */
    if (seq == listener->r_nxt_ack)
    {
        /* check if we're in the window */
        if (listener->config->policy != StreamPolicy::OS_PROXY
                and listener->normalizer->get_stream_window( tdb ) == 0)
        {
            DebugMessage(DEBUG_STREAM_STATE, "Bailing, we're out of the window!\n");
            listener->normalizer->trim_win_payload( tdb );
            return STREAM_UNALIGNED;
        }

        /* move the ack boundry up, this is the only way we'll accept data */
        // FIXIT-L for ips, must move all the way to first hole or right end
        if (listener->s_mgr.state_queue == TCP_STATE_NONE)
            listener->r_nxt_ack = tdb->end_seq;

        if (tdb->pkt->dsize != 0)
        {
            if (!(tcpssn->flow->get_session_flags() & SSNFLAG_STREAM_ORDER_BAD))
                tdb->pkt->packet_flags |= PKT_STREAM_ORDER_OK;

            ProcessTcpStream(listener, tcpssn, tdb, config);
            /* set flags to session flags */

            return STREAM_ALIGNED;
        }
    }
    else
    {
        // pkt is out of order, do some target-based shizzle here...
        // NO, we don't want to simply bail.  Some platforms favor unack'd dup data over the
        // original data.  Let the reassembly policy decide how to handle the overlapping data.
        // See HP, Solaris, et al. for those that favor duplicate data over the original in
        // some cases.
        DebugFormat(DEBUG_STREAM_STATE, "out of order segment (tdb->seq: 0x%X l->r_nxt_ack: 0x%X!\n",
                tdb->seq, listener->r_nxt_ack);

        if (listener->s_mgr.state_queue == TCP_STATE_NONE)
        {
            /* check if we're in the window */
            if (listener->config->policy != StreamPolicy::OS_PROXY
                    and listener->normalizer->get_stream_window( tdb ) == 0)
            {
                DebugMessage(DEBUG_STREAM_STATE, "Bailing, we're out of the window!\n");
                listener->normalizer->trim_win_payload( tdb );
                return STREAM_UNALIGNED;
            }

            if ((listener->s_mgr.state == TCP_STATE_ESTABLISHED)
                    && (listener->flush_policy == STREAM_FLPOLICY_IGNORE))
            {
                if (SEQ_GT(tdb->end_seq, listener->r_nxt_ack))
                {
                    // set next ack so we are within the window going forward on this side.
                    // FIXIT-L for ips, must move all the way to first hole or right end
                    listener->r_nxt_ack = tdb->end_seq;
                }
            }
        }

        if (tdb->pkt->dsize != 0)
        {
            if (!(tcpssn->flow->get_session_flags() & SSNFLAG_STREAM_ORDER_BAD))
            {
                if (!SEQ_LEQ((tdb->seq + tdb->pkt->dsize), listener->r_nxt_ack))
                    tcpssn->flow->set_session_flags( SSNFLAG_STREAM_ORDER_BAD );
            }
            ProcessTcpStream(listener, tcpssn, tdb, config);
        }
    }

    return STREAM_UNALIGNED;
}

static void set_os_policy(Flow* flow, TcpSession* tcpssn)
{
    StreamPolicy client_os_policy = flow->ssn_policy ?
            static_cast<StreamPolicy>( flow->ssn_policy ) : tcpssn->client.config->policy;
    StreamPolicy server_os_policy = flow->ssn_policy ?
            static_cast<StreamPolicy>( flow->ssn_policy ) : tcpssn->server.config->policy;

    if( tcpssn->client.normalizer == nullptr )
        tcpssn->client.normalizer = TcpNormalizerFactory::create( client_os_policy, tcpssn,
                &tcpssn->client, &tcpssn->server );

    if( tcpssn->server.normalizer == nullptr )
        tcpssn->server.normalizer = TcpNormalizerFactory::create( server_os_policy, tcpssn,
                &tcpssn->server, &tcpssn->client );

    if( tcpssn->client.reassembler == nullptr )
        tcpssn->client.reassembler = TcpReassemblerFactory::create( tcpssn, &tcpssn->client, client_os_policy, false );

    if( tcpssn->server.reassembler == nullptr )
        tcpssn->server.reassembler = TcpReassemblerFactory::create( tcpssn, &tcpssn->server, server_os_policy, true );
}

/* Use a for loop and byte comparison, which has proven to be
 * faster on pipelined architectures compared to a memcmp (setup
 * for memcmp is slow).  Not using a 4 byte and 2 byte long because
 * there is no guarantee of memory alignment (and thus performance
 * issues similar to memcmp). */
static inline int ValidMacAddress(TcpTracker *talker, TcpTracker *listener, Packet *p)
{
    int i, j, ret = 0;

    if (!(p->proto_bits & PROTO_BIT__ETH))
        return 0;

    // if flag is set, gauranteed to have an eth layer
    const eth::EtherHdr* eh = layer::get_eth_layer(p);

    for (i = 0; i < 6; ++i)
    {
        if ((talker->mac_addr[i] != eh->ether_src[i]))
            break;
    }
    for (j = 0; j < 6; ++j)
    {
        if (listener->mac_addr[j] != eh->ether_dst[j])
            break;
    }

    // FIXIT-L make this swap check configurable
    if (i < 6 && j < 6)
    {
        if (!memcmp(talker->mac_addr, eh->ether_dst, 6)
                && !memcmp(listener->mac_addr, eh->ether_src, 6))
            // this is prolly a tap
            return 0;
    }

    if (i < 6)
    {
        if (p->packet_flags & PKT_FROM_CLIENT)
            ret |= EVENT_SESSION_HIJACK_CLIENT;
        else
            ret |= EVENT_SESSION_HIJACK_SERVER;
    }
    if (j < 6)
    {
        if (p->packet_flags & PKT_FROM_CLIENT)
            ret |= EVENT_SESSION_HIJACK_SERVER;
        else
            ret |= EVENT_SESSION_HIJACK_CLIENT;
    }
    return ret;
}

static inline void CopyMacAddr(Packet* p, TcpSession* tcpssn, int dir)
{
    int i;

    /* Not ethernet based, nothing to do */
    if (!(p->proto_bits & PROTO_BIT__ETH))
        return;

    // if flag is set, gauranteed to have an eth layer
    const eth::EtherHdr* eh = layer::get_eth_layer(p);

    if (dir == FROM_CLIENT)
    {
        /* Client is SRC */
        for (i = 0; i < 6; i++)
        {
            tcpssn->client.mac_addr[i] = eh->ether_src[i];
            tcpssn->server.mac_addr[i] = eh->ether_dst[i];
        }
    }
    else
    {
        /* Server is SRC */
        for (i = 0; i < 6; i++)
        {
            tcpssn->server.mac_addr[i] = eh->ether_src[i];
            tcpssn->client.mac_addr[i] = eh->ether_dst[i];
        }
    }
}

static void NewTcpSession(Packet* p, Flow* flow, StreamTcpConfig* dstPolicy, TcpSession* tss)
{
    Inspector* ins = flow->gadget;

    if (!ins)
        ins = flow->clouseau;

    if (ins)
    {
        stream.set_splitter(flow, true, ins->get_splitter(true));
        stream.set_splitter(flow, false, ins->get_splitter(false));
    }
    else
    {
        stream.set_splitter(flow, true, new AtomSplitter(true));
        stream.set_splitter(flow, false, new AtomSplitter(false));
    }

    {
        DebugMessage(DEBUG_STREAM_STATE, "adding TcpSession to lightweight session\n");
        flow->protocol = p->type();
        tss->flow = flow;

        /* New session, previous was marked as reset.  Clear the reset flag. */
        uint32_t session_flags = flow->clear_session_flags( SSNFLAG_RESET );
        if ((session_flags & SSNFLAG_CLIENT_SWAP) && !(session_flags & SSNFLAG_CLIENT_SWAPPED))
        {
            TcpTracker trk = tss->client;
            sfip_t ip = flow->client_ip;
            uint16_t port = flow->client_port;

            tss->client = tss->server;
            tss->server = trk;

            flow->client_ip = flow->server_ip;
            flow->server_ip = ip;

            flow->client_port = flow->server_port;
            flow->server_port = port;

            if (!flow->two_way_traffic())
            {
                if (session_flags & SSNFLAG_SEEN_CLIENT)
                {
                    session_flags ^= SSNFLAG_SEEN_CLIENT;
                    session_flags |= SSNFLAG_SEEN_SERVER;
                }
                else if (session_flags & SSNFLAG_SEEN_SERVER)
                {
                    session_flags ^= SSNFLAG_SEEN_SERVER;
                    session_flags |= SSNFLAG_SEEN_CLIENT;
                }
            }

            session_flags |= SSNFLAG_CLIENT_SWAPPED;
            flow->update_session_flags( session_flags );
        }
        init_flush_policy(flow, &tss->server);
        init_flush_policy(flow, &tss->client);

#ifdef DEBUG_STREAM_EX
        PrintTcpSession(tss);
#endif
        flow->set_expire(p, dstPolicy->session_timeout);

        AddStreamSession(&sfBase,
                flow->session_state & STREAM_STATE_MIDSTREAM ? SSNFLAG_MIDSTREAM : 0);

        StreamUpdatePerfBaseState(&sfBase, tss->flow, TCP_STATE_SYN_SENT);

        EventInternal(INTERNAL_EVENT_SESSION_ADD);

        tss->ecn = 0;
        assert(!tss->tcp_init);
        tss->tcp_init = true;

        tcpStats.trackers_created++;
    }
}

static void NewTcpSessionOnSyn(Flow* flow, TcpDataBlock* tdb, StreamTcpConfig* dstPolicy)
{
    PERF_PROFILE(s5TcpNewSessPerfStats);

    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;
    TcpSession* tss;

    /******************************************************************
     * start new sessions on proper SYN packets
     *****************************************************************/
    tss = (TcpSession*) flow->session;
    DebugMessage(DEBUG_STREAM_STATE, "Creating new session tracker on SYN!\n");

    flow->set_session_flags( SSNFLAG_SEEN_CLIENT );

    if (tcph->are_flags_set(TH_CWR | TH_ECE))
        flow->set_session_flags( SSNFLAG_ECN_CLIENT_QUERY );

    /* setup the stream trackers */
    /* Set the StreamTcpConfig for each direction (pkt from client) */
    tss->client.config = dstPolicy; // FIXIT-M use external binding for both dirs
    tss->server.config = dstPolicy; // (applies to all the blocks in this funk)
    set_os_policy(flow, tss);

    tss->client.s_mgr.state = TCP_STATE_SYN_SENT;
    tss->client.isn = tdb->seq;
    tss->client.l_unackd = tdb->seq + 1;
    tss->client.l_nxt_seq = tss->client.l_unackd;

    if (tdb->seq != tdb->end_seq)
        tss->client.l_nxt_seq += (tdb->end_seq - tdb->seq - 1);

    tss->client.l_window = tdb->win;
    tss->client.ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;

    tss->server.reassembler->set_seglist_base_seq( tss->client.l_unackd );
    tss->server.r_nxt_ack = tss->client.l_unackd;
    tss->server.r_win_base = tdb->seq + 1;
    tss->server.s_mgr.state = TCP_STATE_LISTEN;

    tss->client.flags |= tss->client.normalizer->get_tcp_timestamp(tdb, false);
    tss->client.ts_last = tdb->ts;
    if (tss->client.ts_last == 0)
        tss->client.flags |= TF_TSTAMP_ZERO;
    tss->client.flags |= StreamGetMss(tdb->pkt, &tss->client.mss);
    tss->client.flags |= StreamGetWscale(tdb->pkt, &tss->client.wscale);

    CopyMacAddr(tdb->pkt, tss, FROM_CLIENT);

    tcpStats.sessions_on_syn++;
    NewTcpSession(tdb->pkt, flow, dstPolicy, tss);
}

static void NewTcpSessionOnSynAck(Flow* flow, TcpDataBlock* tdb, StreamTcpConfig* dstPolicy)
{
    PERF_PROFILE(s5TcpNewSessPerfStats);

    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;
    TcpSession* tss;

    tss = (TcpSession*) flow->session;
    DebugMessage(DEBUG_STREAM_STATE, "Creating new session tracker on SYN_ACK!\n");

    flow->set_session_flags( SSNFLAG_SEEN_SERVER );
    if (tcph->are_flags_set(TH_CWR | TH_ECE))
        flow->set_session_flags( SSNFLAG_ECN_SERVER_REPLY );

    /* setup the stream trackers */
    /* Set the config for each direction (pkt from server) */
    tss->server.config = dstPolicy;
    tss->client.config = dstPolicy;
    set_os_policy(flow, tss);

    tss->server.s_mgr.state = TCP_STATE_SYN_RCVD;
    tss->server.isn = tdb->seq;
    tss->server.l_unackd = tdb->seq + 1;
    tss->server.l_nxt_seq = tss->server.l_unackd;
    tss->server.l_window = tdb->win;

    tss->server.reassembler->set_seglist_base_seq( tdb->ack );
    tss->server.r_win_base = tdb->ack;
    tss->server.r_nxt_ack = tdb->ack;
    tss->server.ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;

    tss->client.reassembler->set_seglist_base_seq( tss->server.l_unackd );
    tss->client.r_nxt_ack = tss->server.l_unackd;
    tss->client.r_win_base = tdb->seq + 1;
    tss->client.l_nxt_seq = tdb->ack;
    tss->client.isn = tdb->ack - 1;
    tss->client.s_mgr.state = TCP_STATE_SYN_SENT;

    tss->server.flags |= tss->server.normalizer->get_tcp_timestamp(tdb, false);
    tss->server.ts_last = tdb->ts;
    if (tss->server.ts_last == 0)
        tss->server.flags |= TF_TSTAMP_ZERO;
    tss->server.flags |= StreamGetMss(tdb->pkt, &tss->server.mss);
    tss->server.flags |= StreamGetWscale(tdb->pkt, &tss->server.wscale);

    CopyMacAddr(tdb->pkt, tss, FROM_SERVER);

    tcpStats.sessions_on_syn_ack++;
    NewTcpSession(tdb->pkt, flow, dstPolicy, tss);
}

static void NewTcpSessionOn3Way(Flow* flow, TcpDataBlock* tdb,
        StreamTcpConfig* dstPolicy)
{
    PERF_PROFILE(s5TcpNewSessPerfStats);

    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;
    TcpSession* tss;

    /******************************************************************
     * start new sessions on completion of 3-way (ACK only, no data)
     *****************************************************************/
    tss = ( TcpSession* ) flow->session;
    DebugMessage(DEBUG_STREAM_STATE, "Creating new session tracker on ACK!\n");

    flow->set_session_flags( SSNFLAG_SEEN_CLIENT );

    if (tcph->are_flags_set(TH_CWR | TH_ECE))
        flow->set_session_flags( SSNFLAG_ECN_CLIENT_QUERY );

    /* setup the stream trackers */
    /* Set the config for each direction (pkt from client) */
     tss->client.config = dstPolicy;
     tss->server.config = dstPolicy;
     set_os_policy( flow, tss );

    tss->client.s_mgr.state = TCP_STATE_ESTABLISHED;
    tss->client.isn = tdb->seq;
    tss->client.l_unackd = tdb->seq + 1;
    tss->client.l_nxt_seq = tss->client.l_unackd;
    tss->client.l_window = tdb->win;

    tss->client.ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;

    tss->server.reassembler->set_seglist_base_seq( tss->client.l_unackd );
    tss->server.r_nxt_ack = tss->client.l_unackd;
    tss->server.r_win_base = tdb->seq + 1;
    tss->server.s_mgr.state = TCP_STATE_ESTABLISHED;

    tss->client.flags |= tss->client.normalizer->get_tcp_timestamp(tdb, false);
    tss->client.ts_last = tdb->ts;
    if (tss->client.ts_last == 0)
        tss->client.flags |= TF_TSTAMP_ZERO;
    tss->client.flags |= StreamGetMss(tdb->pkt, &tss->client.mss);
    tss->client.flags |= StreamGetWscale(tdb->pkt, &tss->client.wscale);

    CopyMacAddr(tdb->pkt, tss, FROM_CLIENT);

    tcpStats.sessions_on_3way++;
    NewTcpSession(tdb->pkt, flow, dstPolicy, tss);
}

static void NewTcpSessionOnData(Flow* flow, TcpDataBlock* tdb, StreamTcpConfig* dstPolicy)
{
    PERF_PROFILE(s5TcpNewSessPerfStats);

    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;
    TcpSession* tss;

    tss = (TcpSession*) flow->session;
    DebugMessage(DEBUG_STREAM_STATE, "Creating new session tracker on data packet (ACK|PSH)!\n");

    /* Set the config for each direction (pkt from client) */
    tss->client.config = dstPolicy;
    tss->server.config = dstPolicy;
    set_os_policy(flow, tss);
    if (flow->ssn_state.direction == FROM_CLIENT)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Session direction is FROM_CLIENT\n");

        /* Sender is client (src port is higher) */
        flow->set_session_flags( SSNFLAG_SEEN_CLIENT );
        if (tcph->are_flags_set(TH_CWR | TH_ECE))
            flow->set_session_flags( SSNFLAG_ECN_CLIENT_QUERY );

        /* setup the stream trackers */
        tss->client.s_mgr.state = TCP_STATE_ESTABLISHED;
        tss->client.isn = tdb->seq;
        tss->client.l_unackd = tdb->seq;
        tss->client.l_nxt_seq = tss->client.l_unackd;
        tss->client.l_window = tdb->win;

        tss->client.ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;

        tss->server.reassembler->set_seglist_base_seq( tss->client.l_unackd );
        tss->server.r_nxt_ack = tss->client.l_unackd;
        tss->server.r_win_base = tdb->seq;
        tss->server.l_window = 0; /* reset later */

        /* Next server packet is what was ACKd */
        //tss->server.l_nxt_seq = tdb->ack + 1;
        tss->server.l_unackd = tdb->ack - 1;
        tss->server.s_mgr.state = TCP_STATE_ESTABLISHED;

        tss->client.flags |= tss->client.normalizer->get_tcp_timestamp(tdb, false);
        tss->client.ts_last = tdb->ts;
        if (tss->client.ts_last == 0)
            tss->client.flags |= TF_TSTAMP_ZERO;

        tss->client.flags |= StreamGetMss(tdb->pkt, &tss->client.mss);
        tss->client.flags |= StreamGetWscale(tdb->pkt, &tss->client.wscale);

        CopyMacAddr(tdb->pkt, tss, FROM_CLIENT);
    }
    else
    {
        DebugMessage(DEBUG_STREAM_STATE, "Session direction is FROM_SERVER\n");

        /* Sender is server (src port is lower) */
        flow->set_session_flags( SSNFLAG_SEEN_SERVER );

        /* setup the stream trackers */
        tss->server.s_mgr.state = TCP_STATE_ESTABLISHED;
        tss->server.isn = tdb->seq;
        tss->server.l_unackd = tdb->seq;
        tss->server.l_nxt_seq = tss->server.l_unackd;
        tss->server.l_window = tdb->win;

        tss->server.reassembler->set_seglist_base_seq( tdb->ack );
        tss->server.r_win_base = tdb->ack;
        tss->server.r_nxt_ack = tdb->ack;
        tss->server.ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;

        tss->client.reassembler->set_seglist_base_seq( tss->server.l_unackd );
        tss->client.r_nxt_ack = tss->server.l_unackd;
        tss->client.r_win_base = tdb->seq;
        tss->client.l_window = 0; /* reset later */
        tss->client.isn = tdb->ack - 1;
        tss->client.s_mgr.state = TCP_STATE_ESTABLISHED;

        tss->server.flags |= tss->server.normalizer->get_tcp_timestamp(tdb, 0);
        tss->server.ts_last = tdb->ts;
        if (tss->server.ts_last == 0)
            tss->server.flags |= TF_TSTAMP_ZERO;

        tss->server.flags |= StreamGetMss(tdb->pkt, &tss->server.mss);
        tss->server.flags |= StreamGetWscale(tdb->pkt, &tss->server.wscale);

        CopyMacAddr(tdb->pkt, tss, FROM_SERVER);
    }

    tcpStats.sessions_on_data++;
    NewTcpSession(tdb->pkt, flow, dstPolicy, tss);
}

static int ProcessTcp(Flow* flow, TcpDataBlock* tdb, StreamTcpConfig* config)
{
    PERF_PROFILE(s5TcpStatePerfStats);

    int retcode = ACTION_NOTHING;
    int eventcode = 0;
    int got_ts = 0;
    int new_ssn = 0;
    int ts_action = ACTION_NOTHING;
    const tcp::TCPHdr* tcph = tdb->pkt->ptrs.tcph;
    TcpSession* tcpssn = NULL;
    TcpTracker* talker = NULL;
    TcpTracker* listener = NULL;
    DEBUG_WRAP( const char* t = NULL; const char* l = NULL; );

    if (flow->protocol != PktType::TCP)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Lightweight session not TCP on TCP packet\n");
        return retcode;
    }

    tcpssn = ( TcpSession* ) flow->session;

    if (!tcpssn->tcp_init)
    {
        // FIXIT-L expected flow should be checked by flow_con before we
        // get here
        char ignore = flow_con->expected_flow(flow, tdb->pkt);

        if (ignore)
        {
            tcpssn->server.flush_policy = STREAM_FLPOLICY_IGNORE;
            tcpssn->client.flush_policy = STREAM_FLPOLICY_IGNORE;
            return retcode;
        }

        bool require3Way = config->require_3whs();
        bool allow_midstream = config->midstream_allowed(tdb->pkt);

        if (tcph->is_syn_only())
        {
            DebugMessage(DEBUG_STREAM_STATE, "Stream SYN PACKET, establishing lightweight session direction.\n");
            /* SYN packet from client */
            flow->ssn_state.direction = FROM_CLIENT;
            flow->session_state |= STREAM_STATE_SYN;

            if (require3Way || (StreamPacketHasWscale(tdb->pkt) & TF_WSCALE) || (tdb->pkt->dsize > 0))
            {
                /* Create TCP session if we
                 * 1) require 3-WAY HS, OR
                 * 2) client sent wscale option, OR
                 * 3) have data
                 */
                NewTcpSessionOnSyn( flow, tdb, config);
                new_ssn = 1;
                tcpssn->server.normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );
            }

            /* Nothing left todo here */
        }
        else if (tcph->is_syn_ack())
        {
            /* SYN-ACK from server */
            if ((flow->session_state == STREAM_STATE_NONE) || (flow->get_session_flags() & SSNFLAG_RESET))
            {
                DebugMessage(DEBUG_STREAM_STATE, "Stream SYN|ACK PACKET, establishing lightweight session direction.\n");
                flow->ssn_state.direction = FROM_SERVER;
            }

            flow->session_state |= STREAM_STATE_SYN_ACK;

            if (!require3Way || allow_midstream)
            {
                NewTcpSessionOnSynAck(flow, tdb, config);
                new_ssn = 1;
            }

            tcpssn->client.normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );
        }
        else if (tcph->is_ack() && !tcph->is_rst() && (flow->session_state & STREAM_STATE_SYN_ACK))
        {
            /* FIXIT: do we need to verify the ACK field is >= the seq of the SYN-ACK?
               3-way Handshake complete, create TCP session */
            flow->session_state |= STREAM_STATE_ACK | STREAM_STATE_ESTABLISHED;
            NewTcpSessionOn3Way(flow, tdb, config);
            new_ssn = 1;
            tcpssn->server.normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );
            StreamUpdatePerfBaseState(&sfBase, flow, TCP_STATE_ESTABLISHED);
        }
        else if (tdb->pkt->dsize && (!require3Way || allow_midstream))
        {
            /* create session on data, need to figure out direction, etc
               Assume from client, can update later */
            if (tdb->pkt->ptrs.sp > tdb->pkt->ptrs.dp)
                flow->ssn_state.direction = FROM_CLIENT;
            else
                flow->ssn_state.direction = FROM_SERVER;

            flow->session_state |= STREAM_STATE_MIDSTREAM;
            flow->set_session_flags( SSNFLAG_MIDSTREAM );

            NewTcpSessionOnData(flow, tdb, config);
            new_ssn = 1;
            if(flow->ssn_state.direction == FROM_CLIENT)
                tcpssn->server.normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );
            else
                tcpssn->client.normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );

            if (flow->session_state & STREAM_STATE_ESTABLISHED)
                StreamUpdatePerfBaseState(&sfBase, flow, TCP_STATE_ESTABLISHED);
        }
        else if (!tdb->pkt->dsize)
            // Do nothing.
            return retcode;
    }
    else
    {
        /* If session is already marked as established */
        if (!(flow->session_state & STREAM_STATE_ESTABLISHED)
                && (!config->require_3whs() || config->midstream_allowed(tdb->pkt)))
        {
            /* If not requiring 3-way Handshake... */

            /* TCP session created on TH_SYN above,
             * or maybe on SYN-ACK, or anything else */

            /* Need to update Lightweight session state */
            if (tcph->is_syn_ack())
            {
                /* SYN-ACK from server */
                if (flow->session_state != STREAM_STATE_NONE)
                {
                    flow->session_state |= STREAM_STATE_SYN_ACK;
                }
            }
            else if (tcph->is_ack() && (flow->session_state & STREAM_STATE_SYN_ACK))
            {
                flow->session_state |= STREAM_STATE_ACK | STREAM_STATE_ESTABLISHED;
                StreamUpdatePerfBaseState(&sfBase, flow, TCP_STATE_ESTABLISHED);
            }
        }
        if (tcph->is_syn())
            tcpssn->server.normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, config->require_3whs() );
    }

    if (tdb->pkt->packet_flags & PKT_FROM_SERVER)
    {
        DebugMessage(DEBUG_STREAM_STATE,  "Stream: Updating on packet from server\n");
        flow->set_session_flags( SSNFLAG_SEEN_SERVER );

        if (tcpssn->tcp_init)
        {
            talker = &tcpssn->server;
            listener = &tcpssn->client;
        }

        DEBUG_WRAP(
                t = "Server";
                l = "Client");

        if( talker && ( talker->s_mgr.state == TCP_STATE_LISTEN ) && tcph->is_syn_only() )
            eventcode |= EVENT_4WHS;

        /* If we picked this guy up midstream, finish the initialization */
        if ((flow->session_state & STREAM_STATE_MIDSTREAM) && !(flow->session_state & STREAM_STATE_ESTABLISHED))
        {
            FinishServerInit(tdb, tcpssn);
            if( tcph->are_flags_set( TH_ECE ) && ( flow->get_session_flags() & SSNFLAG_ECN_CLIENT_QUERY ) )
                flow->set_session_flags( SSNFLAG_ECN_SERVER_REPLY );

            if( flow->get_session_flags() & SSNFLAG_SEEN_CLIENT )
            {
                // should TCP state go to established too?
                flow->session_state |= STREAM_STATE_ESTABLISHED;
                flow->set_session_flags( SSNFLAG_ESTABLISHED );
                StreamUpdatePerfBaseState( &sfBase, flow, TCP_STATE_ESTABLISHED );
            }
        }
        if (!flow->inner_server_ttl)
            flow->set_ttl(tdb->pkt, false);
    }
    else
    {
        DebugMessage(DEBUG_STREAM_STATE, "Stream: Updating on packet from client\n");
        /* if we got here we had to see the SYN already... */
        flow->set_session_flags( SSNFLAG_SEEN_CLIENT );
        if (tcpssn->tcp_init)
        {
            talker = &tcpssn->client;
            listener = &tcpssn->server;
        }

        DEBUG_WRAP(
                t = "Server";
                l = "Client");

        if ((flow->session_state & STREAM_STATE_MIDSTREAM) && !(flow->session_state & STREAM_STATE_ESTABLISHED))
        {
            /* Midstream and seen server. */
            if (flow->get_session_flags() & SSNFLAG_SEEN_SERVER)
            {
                flow->session_state |= STREAM_STATE_ESTABLISHED;
                flow->set_session_flags( SSNFLAG_ESTABLISHED );
            }
        }
        if (!flow->inner_client_ttl)
            flow->set_ttl(tdb->pkt, true);
    }

    /*
     * check for SYN on reset session
     */
    if( ( flow->get_session_flags() & SSNFLAG_RESET ) && tcph->is_syn() )
    {
        if ( !tcpssn->tcp_init || ( listener->s_mgr.state == TCP_STATE_CLOSED )
                || ( talker->s_mgr.state == TCP_STATE_CLOSED ) )
        {
            /* Listener previously issued a reset
               Talker is re-SYN-ing */
            // FIXIT-L this leads to bogus 129:20
            TcpSessionCleanup(flow, 1);

            if( tcph->is_rst() )
            {
                /* FIXIT-M  In inline mode, only one of the normalizations
                 *           can occur.  If the first normalization
                 *           fires, there is nothing for the second normalization
                 *           to do.  However, in inline-test mode, since
                 *           nothing is actually normalized, both of the
                 *           following functions report that they 'would'
                 *           normalize. i.e., both functions increment their
                 *           count even though only one function can ever
                 *           perform a normalization.
                 */

                /* Got SYN/RST.  We're done. */
                listener->normalizer->trim_syn_payload( tdb );
                listener->normalizer->trim_rst_payload( tdb );
                return retcode | ACTION_RST;
            }
            else if (tcph->is_syn_only())
            {
                flow->ssn_state.direction = FROM_CLIENT;
                flow->session_state = STREAM_STATE_SYN;
                flow->set_ttl(tdb->pkt, true);
                NewTcpSessionOnSyn( flow, tdb, config);
                tcpStats.resyns++;
                new_ssn = 1;

                bool require3Way = config->require_3whs();
                listener = &tcpssn->server;
                talker = &tcpssn->client;
                listener->normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );
                flow->update_session_flags( SSNFLAG_SEEN_CLIENT );
            }
            else if (tcph->is_syn_ack())
            {
                if (config->midstream_allowed(tdb->pkt))
                {
                    flow->ssn_state.direction = FROM_SERVER;
                    flow->session_state = STREAM_STATE_SYN_ACK;
                    flow->set_ttl(tdb->pkt, false);
                    NewTcpSessionOnSynAck( flow, tdb, config);
                    tcpStats.resyns++;
                    tcpssn = (TcpSession*) flow->session;
                    new_ssn = 1;
                }

                bool require3Way = config->require_3whs();
                listener = &tcpssn->client;
                talker = &tcpssn->server;
                listener->normalizer->ecn_tracker( (tcp::TCPHdr *) tcph, require3Way );
                flow->update_session_flags( SSNFLAG_SEEN_SERVER );
            }
        }

        DebugMessage(DEBUG_STREAM_STATE, "Got SYN pkt on reset ssn, re-SYN-ing\n");
    }

    // FIXIT-L why flush here instead of just purge?
    // s5_ignored_session() may be disabling detection too soon if we really want to flush
    if (stream.ignored_session(flow, tdb->pkt))
    {
        if (talker && (talker->flags & TF_FORCE_FLUSH))
        {
            tcpssn->flush_talker(tdb->pkt);
            talker->flags &= ~TF_FORCE_FLUSH;
        }
        if (listener && (listener->flags & TF_FORCE_FLUSH))
        {
            tcpssn->flush_listener(tdb->pkt);
            listener->flags &= ~TF_FORCE_FLUSH;
        }
        tdb->pkt->packet_flags |= PKT_IGNORE;
        retcode |= ACTION_DISABLE_INSPECTION;
    }

    /* Handle data on SYN */
    if ((tdb->pkt->dsize) && tcph->is_syn())
    {
        /* MacOS accepts data on SYN, so don't alert if policy is MACOS */
        if (talker->normalizer->get_os_policy() != StreamPolicy::OS_MACOS)
        {
            // remove data on SYN
            listener->normalizer->trim_syn_payload( tdb );

            if (Normalize_GetMode(NORM_TCP_TRIM_SYN) == NORM_MODE_OFF)
            {
                DebugMessage(DEBUG_STREAM_STATE, "Got data on SYN packet, not processing it\n");
                //EventDataOnSyn(config);
                eventcode |= EVENT_DATA_ON_SYN;
                retcode |= ACTION_BAD_PKT;
            }
        }
    }

    if (!tcpssn->tcp_init)
    {
        LogTcpEvents(eventcode);
        return retcode;
    }

    DebugFormat(DEBUG_STREAM_STATE, "   %s [talker] state: %s\n", t, state_names[talker->s_mgr.state]);
    DebugFormat(DEBUG_STREAM_STATE, "   %s state: %s(%d)\n", l,  state_names[listener->s_mgr.state], listener->s_mgr.state);

    // may find better placement to eliminate redundant flag checks
    if( tcph->is_syn() )
        talker->s_mgr.sub_state |= SUB_SYN_SENT;
    if( tcph->is_ack() )
        talker->s_mgr.sub_state |= SUB_ACK_SENT;

    /*
     * process SYN ACK on unestablished sessions
     */
    if ((TCP_STATE_SYN_SENT == listener->s_mgr.state) && (TCP_STATE_LISTEN == talker->s_mgr.state))
    {
        if( tcph->is_ack() )
        {
            /*
             * make sure we've got a valid segment
             */
            if (!IsBetween(listener->l_unackd, listener->l_nxt_seq, tdb->ack))
            {
                DebugMessage(DEBUG_STREAM_STATE,  "Pkt ack is out of bounds, bailing!\n");
                inc_tcp_discards();
                listener->normalizer->trim_win_payload( tdb );
                LogTcpEvents(eventcode);
                return retcode | ACTION_BAD_PKT;
            }
        }

        talker->flags |= talker->normalizer->get_tcp_timestamp(tdb, false);
        if (tdb->ts == 0)
            talker->flags |= TF_TSTAMP_ZERO;

        /*
         * catch resets sent by server
         */
        if( tcph->is_rst() )
        {
            DebugMessage(DEBUG_STREAM_STATE, "got RST\n");

            listener->normalizer->trim_rst_payload( tdb );

            /* Reset is valid when in SYN_SENT if the
             * ack field ACKs the SYN.
             */
            if (ValidRstSynSent(listener, tdb))
            {
                DebugMessage(DEBUG_STREAM_STATE, "got RST, closing talker\n");
                /* Reset is valid */
                /* Mark session as reset... Leave it around so that any
                 * additional data sent from one side or the other isn't
                 * processed (and is dropped in inline mode).
                 */
                flow->set_session_flags( SSNFLAG_RESET );
                talker->s_mgr.state = TCP_STATE_CLOSED;
                StreamUpdatePerfBaseState(&sfBase, flow, TCP_STATE_CLOSING);
                /* Leave listener open, data may be in transit */
                LogTcpEvents(eventcode);
                return retcode | ACTION_RST;
            }
            /* Reset not valid. */
            DebugMessage(DEBUG_STREAM_STATE, "bad sequence number, bailing\n");
            inc_tcp_discards();
            eventcode |= EVENT_BAD_RST;
            listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
            LogTcpEvents(eventcode);
            return retcode;
        }

        // finish up server init
        if( tcph->is_syn() )
        {
            FinishServerInit(tdb, tcpssn);
            if (talker->flags & TF_TSTAMP)
            {
                talker->ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;
                talker->ts_last = tdb->ts;
            }

            DebugMessage(DEBUG_STREAM_STATE, "Finish server init got called!\n");
        }
        else
        {
            DebugMessage(DEBUG_STREAM_STATE, "Finish server init didn't get called!\n");
        }

        if( tcph->are_flags_set( TH_ECE ) && ( flow->get_session_flags() & SSNFLAG_ECN_CLIENT_QUERY ) )
            flow->set_session_flags( SSNFLAG_ECN_SERVER_REPLY );

        // explicitly set the state
        listener->s_mgr.state = TCP_STATE_SYN_SENT;
        DebugMessage(DEBUG_STREAM_STATE, "Accepted SYN ACK\n");
        LogTcpEvents(eventcode);
        return retcode;
    }

    /*
     * scale the window.  Only if BOTH client and server specified
     * wscale option as part of 3-way handshake.
     * This is per RFC 1323.
     */
    if ((talker->flags & TF_WSCALE) && (listener->flags & TF_WSCALE))
        tdb->win <<= talker->wscale;

    /* Check for session hijacking -- compare mac address to the ones
     * that were recorded at session startup. */
#ifdef DAQ_PKT_FLAG_PRE_ROUTING
    if (!(tdb->pkt->pkth->flags & DAQ_PKT_FLAG_PRE_ROUTING))
#endif
    {
        eventcode |= ValidMacAddress(talker, listener, tdb->pkt);
    }

    ts_action = listener->normalizer->handle_paws( tdb, &eventcode, &got_ts );

    // check RST validity
    if( tcph->is_rst() )
    {
        listener->normalizer->trim_rst_payload( tdb );

        if (listener->normalizer->validate_rst( tdb ))
        {
            DebugMessage(DEBUG_STREAM_STATE, "Got RST, bailing\n");

            if (listener->s_mgr.state == TCP_STATE_FIN_WAIT_1
                    || listener->s_mgr.state == TCP_STATE_FIN_WAIT_2
                    || listener->s_mgr.state == TCP_STATE_CLOSE_WAIT
                    || listener->s_mgr.state == TCP_STATE_CLOSING)
            {
                tcpssn->flush_talker(tdb->pkt);
                tcpssn->flush_listener(tdb->pkt);
                tcpssn->set_splitter(true, nullptr);
                tcpssn->set_splitter(false, nullptr);
                flow->free_application_data();
            }
            flow->set_session_flags( SSNFLAG_RESET );
            talker->s_mgr.state = TCP_STATE_CLOSED;
            talker->s_mgr.sub_state |= SUB_RST_SENT;
            StreamUpdatePerfBaseState(&sfBase, flow, TCP_STATE_CLOSING);

            if( listener->normalizer->is_tcp_ips_enabled() )
                listener->s_mgr.state = TCP_STATE_CLOSED;

            /* else for ids:
               leave listener open, data may be in transit */

            LogTcpEvents(eventcode);
            return retcode | ACTION_RST;
        }
        /* Reset not valid. */
        DebugMessage(DEBUG_STREAM_STATE, "bad sequence number, bailing\n");
        inc_tcp_discards();
        eventcode |= EVENT_BAD_RST;
        listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
        LogTcpEvents(eventcode);
        return retcode | ts_action;
    }
    else
    {
        /* check for valid seqeuence/retrans */
        if (listener->config->policy != StreamPolicy::OS_PROXY
                and (listener->s_mgr.state >= TCP_STATE_ESTABLISHED)
                and !ValidSeq( listener, tdb ))
        {
            DebugMessage(DEBUG_STREAM_STATE, "bad sequence number, bailing\n");
            inc_tcp_discards();
            listener->normalizer->trim_win_payload( tdb );
            LogTcpEvents(eventcode);
            return retcode | ts_action;
        }
    }

    if (ts_action != ACTION_NOTHING)
    {
        DebugMessage(DEBUG_STREAM_STATE, "bad timestamp, bailing\n");
        inc_tcp_discards();
        // this packet was normalized elsewhere
        LogTcpEvents(eventcode);
        return retcode | ts_action;
    }

    // update PAWS timestamps
    DebugFormat(DEBUG_STREAM_STATE, "PAWS update tdb->seq %lu > listener->r_win_base %lu\n",
            tdb->seq, listener->r_win_base);
    if (got_ts && SEQ_EQ(listener->r_win_base, tdb->seq))
    {
        if ((int32_t) (tdb->ts - talker->ts_last) >= 0||
                (uint32_t)tdb->pkt->pkth->ts.tv_sec >= talker->ts_last_pkt + PAWS_24DAYS)
        {
            DebugMessage(DEBUG_STREAM_STATE, "updating timestamps...\n");
            talker->ts_last = tdb->ts;
            talker->ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;
        }
    } else
    {
        DebugMessage(DEBUG_STREAM_STATE, "not updating timestamps...\n");
    }

    // check for repeat SYNs
    if( !new_ssn && tcph->is_syn_only() )
    {
        int action;
        if (!SEQ_EQ(tdb->seq, talker->isn) && listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK))
            action = ACTION_BAD_PKT;
        else if (talker->s_mgr.state >= TCP_STATE_ESTABLISHED)
            action = listener->normalizer->handle_repeated_syn( tdb );
        else
            action = ACTION_NOTHING;

        if (action != ACTION_NOTHING)
        {
            /* got a bad SYN on the session, alert! */
            eventcode |= EVENT_SYN_ON_EST;
            LogTcpEvents(eventcode);
            return retcode | action;
        }
    }

    // Check that the window is within the limits
    if (listener->config->policy != StreamPolicy::OS_PROXY)
    {
        if (listener->config->max_window && (tdb->win > listener->config->max_window))
        {
            DebugMessage(DEBUG_STREAM_STATE, "Got window that was beyond the allowed policy value, bailing\n");
            /* got a window too large, alert! */
            eventcode |= EVENT_WINDOW_TOO_LARGE;
            inc_tcp_discards();
            listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
            LogTcpEvents(eventcode);
            return retcode | ACTION_BAD_PKT;
        }
        else if ((tdb->pkt->packet_flags & PKT_FROM_CLIENT) && (tdb->win <= SLAM_MAX)
                && (tdb->ack == listener->isn + 1)
                && !( tcph->is_fin() | tcph->is_rst() )
                && !(flow->get_session_flags() & SSNFLAG_MIDSTREAM))
        {
            DebugMessage(DEBUG_STREAM_STATE, "Window slammed shut!\n");
            /* got a window slam alert! */
            eventcode |= EVENT_WINDOW_SLAM;
            inc_tcp_discards();

            if (listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK))
            {
                LogTcpEvents(eventcode);
                return retcode | ACTION_BAD_PKT;
            }
        }
    }

    if (talker->s_mgr.state_queue != TCP_STATE_NONE)
    {
        DebugFormat(DEBUG_STREAM_STATE,  "Found queued state transition on ack 0x%X, current 0x%X!\n",
                talker->s_mgr.transition_seq, tdb->ack);

        if (tdb->ack == talker->s_mgr.transition_seq)
        {
            DebugMessage(DEBUG_STREAM_STATE, "accepting transition!\n");
            talker->s_mgr.state = talker->s_mgr.state_queue;
            talker->s_mgr.state_queue = TCP_STATE_NONE;
        }
    }

    // process ACK flags
    if( tcph->is_ack() )
    {
        DebugMessage(DEBUG_STREAM_STATE, "Got an ACK...\n");
        DebugFormat(DEBUG_STREAM_STATE, " %s [listener] state: %s\n", l, state_names[listener->s_mgr.state]);

        switch (listener->s_mgr.state)
        {
            case TCP_STATE_SYN_SENT:
                break;

            case TCP_STATE_SYN_RCVD:
                DebugMessage(DEBUG_STREAM_STATE, "listener state is SYN_SENT...\n");
                if (IsBetween(listener->l_unackd, listener->l_nxt_seq, tdb->ack))
                {
                    UpdateSsn(listener, talker, tdb);
                    flow->set_session_flags( SSNFLAG_ESTABLISHED );
                    flow->session_state |= STREAM_STATE_ESTABLISHED;
                    listener->s_mgr.state = TCP_STATE_ESTABLISHED;
                    talker->s_mgr.state = TCP_STATE_ESTABLISHED;
                    StreamUpdatePerfBaseState(&sfBase, flow, TCP_STATE_ESTABLISHED);
                    /* Indicate this packet completes 3-way handshake */
                    tdb->pkt->packet_flags |= PKT_STREAM_TWH;
                }

                talker->flags |= got_ts;
                if (got_ts && SEQ_EQ(listener->r_nxt_ack, tdb->seq))
                {
                    talker->ts_last_pkt = tdb->pkt->pkth->ts.tv_sec;
                    talker->ts_last = tdb->ts;
                }

                break;

            case TCP_STATE_ESTABLISHED:
            case TCP_STATE_CLOSE_WAIT:
                UpdateSsn( listener, talker, tdb);
                break;

            case TCP_STATE_FIN_WAIT_1:
                UpdateSsn(listener, talker, tdb);

                DebugFormat(DEBUG_STREAM_STATE, "tdb->ack %X >= talker->r_nxt_ack %X\n", tdb->ack, talker->r_nxt_ack);

                if (SEQ_EQ(tdb->ack, listener->l_nxt_seq))
                {
                    if ((listener->normalizer->get_os_policy() == StreamPolicy::OS_WINDOWS) && (tdb->win == 0))
                    {
                        eventcode |= EVENT_WINDOW_SLAM;
                        inc_tcp_discards();

                        if (listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK))
                        {
                            LogTcpEvents(eventcode);
                            return retcode | ACTION_BAD_PKT;
                        }
                    }

                    listener->s_mgr.state = TCP_STATE_FIN_WAIT_2;

                    if( tcph->is_fin() )
                    {
                        DebugMessage(DEBUG_STREAM_STATE, "seq ok, setting state!\n");

                        if (talker->s_mgr.state_queue == TCP_STATE_NONE)
                        {
                            talker->s_mgr.state = TCP_STATE_LAST_ACK;
                            EndOfFileHandle(tdb->pkt, tcpssn);
                        }
                        if (flow->get_session_flags() & SSNFLAG_MIDSTREAM)
                        {
                            // FIXIT-L this should be handled below in fin section
                            // but midstream sessions fail the seq test
                            listener->s_mgr.state_queue = TCP_STATE_TIME_WAIT;
                            listener->s_mgr.transition_seq = tdb->end_seq;
                            listener->s_mgr.expected_flags = TH_ACK;
                        }
                    } else if (listener->s_mgr.state_queue == TCP_STATE_CLOSING)
                    {
                        listener->s_mgr.state_queue = TCP_STATE_TIME_WAIT;
                        listener->s_mgr.transition_seq = tdb->end_seq;
                        listener->s_mgr.expected_flags = TH_ACK;
                    }
                } else
                {
                    DebugMessage(DEBUG_STREAM_STATE, "bad ack!\n");
                }
                break;

            case TCP_STATE_FIN_WAIT_2:
                UpdateSsn(listener, talker, tdb);
                if (SEQ_GT(tdb->ack, listener->l_nxt_seq))
                {
                    eventcode |= EVENT_BAD_ACK;
                    LogTcpEvents(eventcode);
                    listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
                    return retcode | ACTION_BAD_PKT;
                }
                break;

            case TCP_STATE_CLOSING:
                UpdateSsn(listener, talker, tdb);
                if (SEQ_GEQ(tdb->end_seq, listener->r_nxt_ack))
                    listener->s_mgr.state = TCP_STATE_TIME_WAIT;
                break;

            case TCP_STATE_LAST_ACK:
                UpdateSsn( listener, talker, tdb);

                if (SEQ_EQ(tdb->ack, listener->l_nxt_seq))
                    listener->s_mgr.state = TCP_STATE_CLOSED;
                break;

            default:
                // FIXIT-L safe to ignore when inline?
                break;
        }

        talker->reassembler->flush_on_ack_policy( tdb->pkt );
    }

    // handle data in the segment
    if (tdb->pkt->dsize)
    {
        DebugFormat(DEBUG_STREAM_STATE, "   %s state: %s(%d) getting data\n",
                l, state_names[listener->s_mgr.state], listener->s_mgr.state);

        // FIN means only that sender is done talking, other side may continue yapping.
        if (TCP_STATE_FIN_WAIT_2 == talker->s_mgr.state ||
                TCP_STATE_TIME_WAIT == talker->s_mgr.state)
        {
            // data on a segment when we're not accepting data any more alert!
            //EventDataOnClosed(talker->config);
            eventcode |= EVENT_DATA_ON_CLOSED;
            retcode |= ACTION_BAD_PKT;
            listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
        }
        else if (TCP_STATE_CLOSED == talker->s_mgr.state)
        {
            // data on a segment when we're not accepting data any more alert!
            if (flow->get_session_flags() & SSNFLAG_RESET)
            {
                //EventDataAfterReset(listener->config);
                if (talker->s_mgr.sub_state & SUB_RST_SENT)
                    eventcode |= EVENT_DATA_AFTER_RESET;
                else
                    eventcode |= EVENT_DATA_AFTER_RST_RCVD;
            }
            else
            {
                //EventDataOnClosed(listener->config);
                eventcode |= EVENT_DATA_ON_CLOSED;
            }
            retcode |= ACTION_BAD_PKT;
            listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
        }
        else
        {
            DebugFormat(DEBUG_STREAM_STATE, "Queuing data on listener, t %s, l %s...\n",
                    flush_policy_names[talker->flush_policy], flush_policy_names[listener->flush_policy]);

            if (config->policy != StreamPolicy::OS_PROXY)
            {
                // these normalizations can't be done if we missed setup. and
                // window is zero in one direction until we've seen both sides.
                if (!(flow->get_session_flags() & SSNFLAG_MIDSTREAM))
                {
                    // sender of syn w/mss limits payloads from peer since we store mss on
                    // sender side, use listener mss same reasoning for window size
                    TcpTracker* st = listener;

                    // trim to fit in window and mss as needed
                    st->normalizer->trim_win_payload(tdb, (st->r_win_base + st->l_window) - st->r_nxt_ack);

                    if (st->mss)
                        st->normalizer->trim_mss_payload( tdb, st->mss );

                    st->normalizer->ecn_stripper( tdb->pkt );
                }
            }
            // dunno if this is RFC but fragroute testing expects it  for the record,
            // I've seen FTP data sessions that send data packets with no tcp flags set
            if ((tcph->th_flags != 0)
                    or (config->policy == StreamPolicy::OS_LINUX)
                    or (config->policy == StreamPolicy::OS_PROXY))
            {
                ProcessTcpData( listener, tcpssn, tdb, config);
            }
            else
            {
                eventcode |= EVENT_DATA_WITHOUT_FLAGS;
                listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
            }
        }

        listener->reassembler->flush_on_data_policy( tdb->pkt );
    }

    if( tcph->is_fin() )
    {
        DebugMessage(DEBUG_STREAM_STATE, "Got a FIN...\n");
        DebugFormat(DEBUG_STREAM_STATE,  "   %s state: %s(%d)\n", l, state_names[talker->s_mgr.state], talker->s_mgr.state);
        DebugFormat(DEBUG_STREAM_STATE, "checking ack (0x%X) vs nxt_ack (0x%X)\n", tdb->end_seq, listener->r_win_base);

        if (SEQ_LT(tdb->end_seq, listener->r_win_base))
        {
            DebugMessage(DEBUG_STREAM_STATE, "FIN inside r_win_base, bailing\n");
            goto dupfin;
        }
        else
        {
            // need substate since we don't change state immediately
            if ((talker->s_mgr.state >= TCP_STATE_ESTABLISHED)  && !(talker->s_mgr.sub_state & SUB_FIN_SENT))
            {
                talker->l_nxt_seq++;

                //--------------------------------------------------
                // FIXIT-L don't bump r_nxt_ack unless FIN is in seq
                // because it causes bogus 129:5 cases
                // but doing so causes extra gaps
                //if ( SEQ_EQ(tdb->end_seq, listener->r_nxt_ack) )
                listener->r_nxt_ack++;
                //--------------------------------------------------

                talker->s_mgr.sub_state |= SUB_FIN_SENT;

                if ((listener->flush_policy != STREAM_FLPOLICY_ON_ACK)
                        && (listener->flush_policy != STREAM_FLPOLICY_ON_DATA)
                        && listener->normalizer->is_tcp_ips_enabled() )
                {
                    tdb->pkt->packet_flags |= PKT_PDU_TAIL;
                }
            }
            switch (talker->s_mgr.state)
            {
                case TCP_STATE_SYN_RCVD:
                case TCP_STATE_ESTABLISHED:
                    if (talker->s_mgr.state_queue == TCP_STATE_CLOSE_WAIT)
                        talker->s_mgr.state_queue = TCP_STATE_CLOSING;

                    talker->s_mgr.state = TCP_STATE_FIN_WAIT_1;
                    EndOfFileHandle(tdb->pkt, tcpssn);

                    if (!tdb->pkt->dsize)
                        listener->reassembler->flush_on_data_policy( tdb->pkt );

                    StreamUpdatePerfBaseState(&sfBase, tcpssn->flow, TCP_STATE_CLOSING);
                    break;

                case TCP_STATE_CLOSE_WAIT:
                    talker->s_mgr.state = TCP_STATE_LAST_ACK;
                    break;

                case TCP_STATE_FIN_WAIT_1:
                    if (!tdb->pkt->dsize)
                        tcpssn->retransmit_handle( tdb->pkt );
                    break;

                default:
                    /* all other states stay where they are */
                    break;
            }

            if ((talker->s_mgr.state == TCP_STATE_FIN_WAIT_1) || (talker->s_mgr.state == TCP_STATE_LAST_ACK))
            {
                uint32_t end_seq = (flow->get_session_flags() & SSNFLAG_MIDSTREAM)
                    ? tdb->end_seq - 1 : tdb->end_seq;

                if ((listener->s_mgr.expected_flags == TH_ACK) && SEQ_GEQ(end_seq, listener->s_mgr.transition_seq))
                {
                    DebugMessage(DEBUG_STREAM_STATE, "FIN beyond previous, ignoring\n");
                    eventcode |= EVENT_BAD_FIN;
                    LogTcpEvents(eventcode);
                    listener->normalizer->packet_dropper(tdb, NORM_TCP_BLOCK);
                    return retcode | ACTION_BAD_PKT;
                }
            }

            switch (listener->s_mgr.state)
            {
                case TCP_STATE_ESTABLISHED:
                    listener->s_mgr.state_queue = TCP_STATE_CLOSE_WAIT;
                    listener->s_mgr.transition_seq = tdb->end_seq + 1;
                    listener->s_mgr.expected_flags = TH_ACK;
                    break;

                case TCP_STATE_FIN_WAIT_1:
                    listener->s_mgr.state_queue = TCP_STATE_CLOSING;
                    listener->s_mgr.transition_seq = tdb->end_seq + 1;
                    listener->s_mgr.expected_flags = TH_ACK;
                    break;

                case TCP_STATE_FIN_WAIT_2:
                    listener->s_mgr.state_queue = TCP_STATE_TIME_WAIT;
                    listener->s_mgr.transition_seq = tdb->end_seq + 1;
                    listener->s_mgr.expected_flags = TH_ACK;
                    break;
            }
        }
    }

dupfin:

    DebugFormat(DEBUG_STREAM_STATE, "   %s [talker] state: %s\n", t, state_names[talker->s_mgr.state]);
    DebugFormat(DEBUG_STREAM_STATE, "   %s state: %s(%d)\n", l, state_names[listener->s_mgr.state], listener->s_mgr.state);

    // handle TIME_WAIT timer stuff
    if ((talker->s_mgr.state == TCP_STATE_TIME_WAIT && listener->s_mgr.state == TCP_STATE_CLOSED)
            || (listener->s_mgr.state == TCP_STATE_TIME_WAIT && talker->s_mgr.state == TCP_STATE_CLOSED)
            || (listener->s_mgr.state == TCP_STATE_TIME_WAIT && talker->s_mgr.state ==  TCP_STATE_TIME_WAIT))
    {
        // The last ACK is a part of the session. Delete the session after processing is complete.
        LogTcpEvents(eventcode);
        TcpSessionCleanup(flow, 0, tdb->pkt);
        flow->session_state |= STREAM_STATE_CLOSED;
        return retcode | ACTION_LWSSN_CLOSED;
    }
    else if( listener->s_mgr.state == TCP_STATE_CLOSED
             && talker->s_mgr.state == TCP_STATE_SYN_SENT )
    {
        if( tcph->is_syn_only() )
            flow->set_expire(tdb->pkt, config->session_timeout);
    }

    LogTcpEvents(eventcode);
    return retcode;
}


//-------------------------------------------------------------------------
// TcpSession methods
//-------------------------------------------------------------------------

TcpSession::TcpSession(Flow* flow) :
    Session(flow), ecn(0), event_mask(0)
{
    lws_init = tcp_init = false;
}

TcpSession::~TcpSession()
{
    if (tcp_init)
    {
        TcpSessionClear(flow, (TcpSession*) flow->session, 1);
        // FIXIT - do these ever need to be freed, before session is reused for same flow
        // i think not...
        delete client.normalizer;
        delete server.normalizer;
        delete client.reassembler;
        delete server.reassembler;
    }
}

void TcpSession::reset()
{
    if (tcp_init)
        TcpSessionClear(flow, (TcpSession*) flow->session, 2);
}

bool TcpSession::setup(Packet*)
{

	TcpStateHandler* tsh = new TcpStateHandler;

    // FIXIT-L this it should not be necessary to reset here
    reset();

    lws_init = tcp_init = false;
    event_mask = 0;
    ecn = 0;

    memset(&client, 0, offsetof(TcpTracker, alerts));
    memset(&server, 0, offsetof(TcpTracker, alerts));

#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
    ingress_index = egress_index = 0;
    ingress_group = egress_group = 0;
    daq_flags = address_space_id = 0;
#endif

    // FIXIT - this is just temp...remove...
    delete tsh;
    tsh = new TcpClosedState;
    delete tsh;
    tsh = new TcpListenState;
    delete tsh;
    tsh = new TcpSynSentState;
    delete tsh;

    tcpStats.sessions++;
    return true;
}

void TcpSession::cleanup()
{
    // this flushes data and then calls TcpSessionClear()
    TcpSessionCleanup(flow, 1);
}

void TcpSession::clear()
{
    if( tcp_init )
        // this does NOT flush data
        TcpSessionClear(flow, this, 1);
}

void TcpSession::restart(Packet* p)
{
    // sanity check since this is called externally
    assert(p->ptrs.tcph);

    TcpTracker* talker, *listener;

    if (p->packet_flags & PKT_FROM_SERVER)
    {
        talker = &server;
        listener = &client;
    }
    else
    {
        talker = &client;
        listener = &server;
    }

    // FIXIT-H on data / on ack must be based on flush policy
    if (p->dsize > 0)
        listener->reassembler->flush_on_data_policy( p );

    if (p->ptrs.tcph->is_ack())
        talker->reassembler->flush_on_ack_policy( p );
}

void TcpSession::set_splitter(bool c2s, StreamSplitter* ss)
{
    TcpTracker* trk;

    if (c2s)
        trk = &server;
    else
        trk = &client;

    if (trk->splitter && tcp_init)
        delete trk->splitter;

    trk->splitter = ss;

    if (ss)
        paf_setup(&trk->paf_state);
    else
        trk->flush_policy = STREAM_FLPOLICY_IGNORE;
}

StreamSplitter* TcpSession::get_splitter(bool c2s)
{
    if (c2s)
        return server.splitter;

    return client.splitter;
}

void TcpSession::flush_server(Packet *p)
{
    int flushed;
    TcpTracker *flushTracker = &server;

    flushTracker->flags |= TF_FORCE_FLUSH;

    /* If this is a rebuilt packet, don't flush now because we'll
     * overwrite the packet being processed.
     */
    if (p->packet_flags & PKT_REBUILT_STREAM)
    {
        /* We'll check & clear the TF_FORCE_FLUSH next time through */
        return;
    }

    /* Need to convert the addresses to network order */
    flushed = flushTracker->reassembler->flush_stream( p, PKT_FROM_SERVER);

    if (flushed)
        flushTracker->reassembler->purge_flushed_ackd( );

    flushTracker->flags &= ~TF_FORCE_FLUSH;
}

void TcpSession::flush_client(Packet* p)
{
    int flushed;
    TcpTracker *flushTracker = &client;

    flushTracker->flags |= TF_FORCE_FLUSH;

    /* If this is a rebuilt packet, don't flush now because we'll
     * overwrite the packet being processed.
     */
    if (p->packet_flags & PKT_REBUILT_STREAM)
    {
        /* We'll check & clear the TF_FORCE_FLUSH next time through */
        return;
    }

    /* Need to convert the addresses to network order */
    flushed = flushTracker->reassembler->flush_stream(p, PKT_FROM_CLIENT);

    if (flushed)
        flushTracker->reassembler->purge_flushed_ackd( );

    flushTracker->flags &= ~TF_FORCE_FLUSH;
}

void TcpSession::flush_listener(Packet* p)
{
    TcpTracker *listener = NULL;
    uint32_t dir = 0;
    int flushed = 0;

    /* figure out direction of this packet -- we should've already
     * looked at it, so the packet_flags are already set. */
    if (p->packet_flags & PKT_FROM_SERVER)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Flushing listener on packet from server\n");
        listener = &client;
        /* dir of flush is the data from the opposite side */
        dir = PKT_FROM_SERVER;
    }
    else if (p->packet_flags & PKT_FROM_CLIENT)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Flushing listener on packet from client\n");
        listener = &server;
        /* dir of flush is the data from the opposite side */
        dir = PKT_FROM_CLIENT;
    }

    if (dir != 0)
    {
        listener->flags |= TF_FORCE_FLUSH;
        flushed = listener->reassembler->flush_stream( p, dir);

        if (flushed)
            listener->reassembler->purge_flushed_ackd( );

        listener->flags &= ~TF_FORCE_FLUSH;
    }
}

void TcpSession::flush_talker(Packet* p)
{
    TcpTracker *talker = NULL;
    uint32_t dir = 0;
    int flushed = 0;

    /* figure out direction of this packet -- we should've already
     * looked at it, so the packet_flags are already set. */
    if (p->packet_flags & PKT_FROM_SERVER)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Flushing talker on packet from server\n");
        talker = &server;
        /* dir of flush is the data from the opposite side */
        dir = PKT_FROM_CLIENT;
    }
    else if (p->packet_flags & PKT_FROM_CLIENT)
    {
        DebugMessage(DEBUG_STREAM_STATE, "Flushing talker on packet from client\n");
        talker = &client;
        /* dir of flush is the data from the opposite side */
        dir = PKT_FROM_SERVER;
    }

    if (dir != 0)
    {
        talker->flags |= TF_FORCE_FLUSH;
        flushed = talker->reassembler->flush_stream(p, dir);

        if (flushed)
            talker->reassembler->purge_flushed_ackd( );

        talker->flags &= ~TF_FORCE_FLUSH;
    }
}

// FIXIT add alert and check alerted go away when we finish
// packet / PDU split because PDU rules won't run on raw packets
bool TcpSession::add_alert(Packet* p, uint32_t gid, uint32_t sid)
{
    TcpTracker *st;
    StreamAlertInfo* ai;

    if (sfip_equals(p->ptrs.ip_api.get_src(), &flow->client_ip))
        st = &server;
    else
        st = &client;

    if (st->alert_count >= MAX_SESSION_ALERTS)
        return false;

    ai = st->alerts + st->alert_count;
    ai->gid = gid;
    ai->sid = sid;
    ai->seq = 0;

    st->alert_count++;

    return true;
}

bool TcpSession::check_alerted(Packet* p, uint32_t gid, uint32_t sid)
{
    /* If this is not a rebuilt packet, no need to check further */
    if (!(p->packet_flags & PKT_REBUILT_STREAM))
        return false;

    TcpTracker *st;

    if (sfip_equals(p->ptrs.ip_api.get_src(), &flow->client_ip))
        st = &server;
    else
        st = &client;

    for (int i = 0; i < st->alert_count; i++)
    {
        /*  This is a rebuilt packet and if we've seen this alert before,
         *  return that we have previously alerted on original packet.
         */
        if (st->alerts[i].gid == gid && st->alerts[i].sid == sid)
        {
            return true;
        }
    }

    return false;
}

int TcpSession::update_alert(Packet *p, uint32_t gid, uint32_t sid,
        uint32_t event_id, uint32_t event_second)
{
    TcpTracker *st;
    int i;
    uint32_t seq_num;

    if (sfip_equals(p->ptrs.ip_api.get_src(), &flow->client_ip))
        st = &server;
    else
        st = &client;

    seq_num = 0;

    for (i = 0; i < st->alert_count; i++)
    {
        StreamAlertInfo* ai = st->alerts + i;

        if (ai->gid == gid && ai->sid == sid && SEQ_EQ(ai->seq, seq_num))
        {
            ai->event_id = event_id;
            ai->event_second = event_second;
            return 0;
        }
    }

    return -1;
}

void TcpSession::set_extra_data(Packet* p, uint32_t xid)
{
    TcpTracker *st;

    if (sfip_equals(p->ptrs.ip_api.get_src(), &flow->client_ip))
        st = &server;
    else
        st = &client;

    st->reassembler->set_xtradata_mask( st->reassembler->get_xtradata_mask() | BIT(xid) );
}

void TcpSession::clear_extra_data(Packet* p, uint32_t xid)
{
    TcpTracker *st;

    if (sfip_equals(p->ptrs.ip_api.get_src(), &flow->client_ip))
        st = &server;
    else
        st = &client;

    if (xid)
        st->reassembler->set_xtradata_mask( st->reassembler->get_xtradata_mask() & ~BIT(xid) );
    else
        st->reassembler->set_xtradata_mask( 0 );
}

uint8_t TcpSession::get_reassembly_direction()
{
    uint8_t dir = SSN_DIR_NONE;

    if (server.flush_policy != STREAM_FLPOLICY_IGNORE)
    {
        dir |= SSN_DIR_FROM_CLIENT;
    }

    if (client.flush_policy != STREAM_FLPOLICY_IGNORE)
    {
        dir |= SSN_DIR_FROM_SERVER;
    }

    return dir;
}

bool TcpSession::is_sequenced(uint8_t dir)
{
    if (dir & SSN_DIR_FROM_CLIENT)
    {
        if (server.flags & (TF_MISSING_PREV_PKT | TF_MISSING_PKT))
            return false;
    }

    if (dir & SSN_DIR_FROM_SERVER)
    {
        if (client.flags & (TF_MISSING_PREV_PKT | TF_MISSING_PKT))
            return false;
    }

    return true;
}

/* This will falsely return SSN_MISSING_BEFORE on the first reassembed
 * packet if reassembly for this direction was set mid-session */
uint8_t TcpSession::missing_in_reassembled(uint8_t dir)
{
    if (dir & SSN_DIR_FROM_CLIENT)
    {
        if ((server.flags & TF_MISSING_PKT)
                && (server.flags & TF_MISSING_PREV_PKT))
            return SSN_MISSING_BOTH;
        else if (server.flags & TF_MISSING_PREV_PKT)
            return SSN_MISSING_BEFORE;
        else if (server.flags & TF_MISSING_PKT)
            return SSN_MISSING_AFTER;
    }
    else if (dir & SSN_DIR_FROM_SERVER)
    {
        if ((client.flags & TF_MISSING_PKT)
                && (client.flags & TF_MISSING_PREV_PKT))
            return SSN_MISSING_BOTH;
        else if (client.flags & TF_MISSING_PREV_PKT)
            return SSN_MISSING_BEFORE;
        else if (client.flags & TF_MISSING_PKT)
            return SSN_MISSING_AFTER;
    }

    return SSN_MISSING_NONE;
}

bool TcpSession::are_packets_missing(uint8_t dir)
{
    if (dir & SSN_DIR_FROM_CLIENT)
    {
        if (server.flags & TF_PKT_MISSED)
            return true;
    }

    if (dir & SSN_DIR_FROM_SERVER)
    {
        if (client.flags & TF_PKT_MISSED)
            return true;
    }

    return false;
}

void TcpSession::update_direction(char dir, const sfip_t* ip, uint16_t port)
{
    sfip_t tmpIp;
    uint16_t tmpPort;
    TcpTracker tmpTracker;

    if (sfip_equals(&flow->client_ip, ip) && (flow->client_port == port))
    {
        if ((dir == SSN_DIR_FROM_CLIENT) && (flow->ssn_state.direction == FROM_CLIENT))
        {
            /* Direction already set as client */
            return;
        }
    }
    else if (sfip_equals(&flow->server_ip, ip) && (flow->server_port == port))
    {
        if ((dir == SSN_DIR_FROM_SERVER) && (flow->ssn_state.direction == FROM_SERVER))
        {
            /* Direction already set as server */
            return;
        }
    }

    /* Swap them -- leave flow->ssn_state.direction the same */

    /* XXX: Gotta be a more efficient way to do this without the memcpy */
    tmpIp = flow->client_ip;
    tmpPort = flow->client_port;
    flow->client_ip = flow->server_ip;
    flow->client_port = flow->server_port;
    flow->server_ip = tmpIp;
    flow->server_port = tmpPort;

#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
    SwapPacketHeaderFoo( );
#endif
    memcpy(&tmpTracker, &client, sizeof(TcpTracker));
    memcpy(&client, &server, sizeof(TcpTracker));
    memcpy(&server, &tmpTracker, sizeof(TcpTracker));
}

#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
void TcpSession::SetPacketHeaderFoo( const Packet* p )
{
    if ( daq_flags & DAQ_PKT_FLAG_NOT_FORWARDING )
    {
        ingress_index = p->pkth->ingress_index;
        ingress_group = p->pkth->ingress_group;
        // ssn egress may be unknown, but will be correct
        egress_index = p->pkth->egress_index;
        egress_group = p->pkth->egress_group;
    }
    else if ( p->packet_flags & PKT_FROM_CLIENT )
    {
        ingress_index = p->pkth->ingress_index;
        ingress_group = p->pkth->ingress_group;
        // ssn egress not always correct here
    }
    else
    {
        // ssn ingress not always correct here
        egress_index = p->pkth->ingress_index;
        egress_group = p->pkth->ingress_group;
    }
    daq_flags = p->pkth->flags;
    address_space_id = p->pkth->address_space_id;
}

void TcpSession::GetPacketHeaderFoo( DAQ_PktHdr_t* pkth, uint32_t dir )
{
    if ( (dir & PKT_FROM_CLIENT) || (daq_flags & DAQ_PKT_FLAG_NOT_FORWARDING) )
    {
        pkth->ingress_index = ingress_index;
        pkth->ingress_group = ingress_group;
        pkth->egress_index = egress_index;
        pkth->egress_group = egress_group;
    }
    else
    {
        pkth->ingress_index = egress_index;
        pkth->ingress_group = egress_group;
        pkth->egress_index = ingress_index;
        pkth->egress_group = ingress_group;
    }
#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
    pkth->opaque = 0;
#endif
    pkth->flags = daq_flags;
    pkth->address_space_id = address_space_id;
}

void TcpSession::SwapPacketHeaderFoo( void )
{
    if ( egress_index != DAQ_PKTHDR_UNKNOWN )
    {
        int32_t save_ingress_index;
        int32_t save_ingress_group;

        save_ingress_index = ingress_index;
        save_ingress_group = ingress_group;
        ingress_index = egress_index;
        ingress_group = egress_group;
        egress_index = save_ingress_index;
        egress_group = save_ingress_group;
    }
}

#endif


/*
 * Main entry point for TCP
 */
int TcpSession::process(Packet* p)
{
    PERF_PROFILE(s5TcpPerfStats);

    TcpDataBlock tdb;
    int status;

    DEBUG_WRAP(
            char flagbuf[9];
            CreateTCPFlagString(p->ptrs.tcph, flagbuf);
            DebugFormat((DEBUG_STREAM|DEBUG_STREAM_STATE),
                "Got TCP Packet 0x%X:%d ->  0x%X:%d %s\nseq: 0x%X   ack:0x%X  dsize: %u\n",
                p->ptrs.ip_api.get_src(), p->ptrs.sp, p->ptrs.ip_api.get_dst(), p->ptrs.dp, flagbuf,
                ntohl(p->ptrs.tcph->th_seq), ntohl(p->ptrs.tcph->th_ack), p->dsize);
            );

    if (stream.blocked_session(flow, p)
            || (flow->session_state & STREAM_STATE_IGNORE))
        return ACTION_NOTHING;

    SetupTcpDataBlock(&tdb, p);

    StreamTcpConfig* config = get_tcp_cfg(flow->ssn_server);

    if (!lws_init)
    {
        // FIXIT most of this now looks out of place or redundant
        if (config->require_3whs())
        {
            if (p->ptrs.tcph->is_syn_only())
            {
                /* SYN only */
                flow->session_state = STREAM_STATE_SYN;
            }
            else
            {
                // If we're within the "startup" window, try to handle
                // this packet as midstream pickup -- allows for
                // connections that already existed before snort started.
                if (config->midstream_allowed(p))
                    goto midstream_pickup_allowed;

                // Do nothing with this packet since we require a 3-way ;)
                DEBUG_WRAP(
                        DebugMessage(DEBUG_STREAM_STATE, "Stream: Requiring 3-way Handshake, but failed to retrieve session"
                            " object for non SYN packet.\n");
                        );

                if (!p->ptrs.tcph->is_rst() && !(event_mask & EVENT_NO_3WHS))
                {
                    EventNo3whs();
                    event_mask |= EVENT_NO_3WHS;
                }

#ifdef REG_TEST
                S5TraceTCP(p, flow, &tdb, 1);
#endif
                return 0;
            }
        }
        else
        {
midstream_pickup_allowed: if (!p->ptrs.tcph->is_syn_ack()
                                  && !p->dsize && !(StreamPacketHasWscale(p) & TF_WSCALE))
                          {
#ifdef REG_TEST
                              S5TraceTCP(p, flow, &tdb, 1);
#endif
                              return 0;
                          }
        }
        lws_init = true;
    }
    /*
     * Check if the session is expired.
     * Should be done before we do something with the packet...
     * ie, Insert a packet, or handle state change SYN, FIN, RST, etc.
     */
    if (stream.expired_session(flow, p))
    {
        /* Session is timed out */
        if (flow->get_session_flags() & SSNFLAG_RESET)
        {
            /* If this one has been reset, delete the TCP
             * portion, and start a new. */
            TcpSessionCleanup(flow, 1);
        }
        else
        {
            DebugMessage(DEBUG_STREAM_STATE, "Stream TCP session timedout!\n");

            /* Not reset, simply time'd out.  Clean it up */
            TcpSessionCleanup(flow, 1);
        }
        tcpStats.timeouts++;
    }

    status = ProcessTcp(flow, &tdb, config);

    DebugMessage(DEBUG_STREAM_STATE, "Finished Stream TCP cleanly!\n---------------------------------------------------\n");

    if (!(status & ACTION_LWSSN_CLOSED))
    {
        flow->markup_packet_flags(p);
        flow->set_expire(p, config->session_timeout);
    }

    if (status & ACTION_DISABLE_INSPECTION)
    {
        DisableInspection(p);

        DebugFormat(DEBUG_STREAM_STATE, "Stream Ignoring packet from %d. Session marked as ignore\n",
                p->packet_flags & PKT_FROM_SERVER ? "server" : "client");
    }

    S5TraceTCP(p, flow, &tdb, 0);
    return 0;
}

void TcpSession::flush()
{
    if( ( server.reassembler->is_segment_pending_flush() ) || (client.reassembler->is_segment_pending_flush() ) )
    {
        server.reassembler->flush_queued_segments( flow, false );
        client.reassembler->flush_queued_segments( flow, false );
    }
}

void TcpSession::start_proxy()
{
    client.config->policy = StreamPolicy::OS_PROXY;
    server.config->policy = StreamPolicy::OS_PROXY;
}

//-------------------------------------------------------------------------
// tcp module stuff
//-------------------------------------------------------------------------

void TcpSession::set_memcap(Memcap& mc)
{
    tcp_memcap = &mc;
}

void TcpSession::sinit()
{
    s5_pkt = PacketManager::encode_new();
    //AtomSplitter::init();  // FIXIT-L PAF implement
}

void TcpSession::sterm()
{
    if (s5_pkt)
    {
        PacketManager::encode_delete(s5_pkt);
        s5_pkt = nullptr;
    }
}

void TcpSession::show(StreamTcpConfig* tcp_config)
{
    StreamPrintTcpConfig(tcp_config);
}

