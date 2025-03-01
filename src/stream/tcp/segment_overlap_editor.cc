//--------------------------------------------------------------------------
// Copyright (C) 2015-2015 Cisco and/or its affiliates. All rights reserved.
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

// segment_overlap_editor.cc author davis mcpherson <davmcphe@@cisco.com>
// Created on: Oct 11, 2015

#include "main/snort_debug.h"
#include "protocols/packet.h"

#include "tcp_module.h"
#include "tcp_events.h"
#include "tcp_normalizer.h"
#include "tcp_reassembler.h"
#include "segment_overlap_editor.h"

bool SegmentOverlapEditor::is_segment_retransmit( void )
{
    // Don't want to count retransmits as overlaps or do anything
    // else with them.  Account for retransmits of multiple PDUs
    // in one segment.
    if( right->is_retransmit( rdata, rsize, rseq ) )
    {
        rdata += right->payload_size;
        rsize -= right->payload_size;
        rseq += right->payload_size;

        seq += right->payload_size;
        left = right;
        right = right->next;

        if( rsize == 0 )
        {
            // All data was retransmitted
            session->retransmit_process( tdb->pkt );
            keep_segment = false;
        }

        return true;
    }

    return false;
}

int SegmentOverlapEditor::eval_left( void )
{
    int rc = STREAM_INSERT_OK;

    if( left )
       rc = insert_left_overlap();

    return rc;
}

int SegmentOverlapEditor::eval_right( void )
{
    while( right && SEQ_LT( right->seq, seq_end ) )
    {
        trunc_len = 0;
        overlap = ( int ) ( seq_end - right->seq );

        DebugFormat(DEBUG_STREAM_STATE, "right overlap(%d): len: %d right->seq: 0x%X seq: 0x%X\n",
                overlap, len, right->seq, seq);

        // Treat sequence number overlap as a retransmission, only check right side since
        //  left side happens rarely
        session->retransmit_handle( tdb->pkt );

        if( overlap < right->payload_size )
        {
            if( right->is_retransmit( rdata, rsize, rseq ) )
            {
                // All data was retransmitted
                session->retransmit_process( tdb->pkt );
                keep_segment = false;
            }
            else
            {
                tcpStats.overlaps++;
                overlap_count++;
                insert_right_overlap();
            }

            break;
        }
        else  // Full overlap
        {
            // Don't want to count retransmits as overlaps or do anything
            // else with them.  Account for retransmits of multiple PDUs
            // in one segment.
            if( is_segment_retransmit() )
                continue;

            tcpStats.overlaps++;
            overlap_count++;
            int rc = insert_full_overlap();
            if( rc != STREAM_INSERT_OK )
                return rc;
        }
    }

    return STREAM_INSERT_OK;
}

void SegmentOverlapEditor::drop_old_segment( void )
{
    DebugFormat(DEBUG_STREAM_STATE, "full right overlap, dropping old segment at seq %d, size %d\n",
            right->seq, right->payload_size);

    TcpSegment* drop_seg = right;
    right = right->next;
    delete_reassembly_segment( drop_seg );
}

int SegmentOverlapEditor::generate_bad_segment_event( void )
{
    DebugFormat(DEBUG_STREAM_STATE, "bad segment: overlap with invalid sequence number"
             "(seq: %X  seq_end: %X overlap: %lu\n", seq, seq_end, overlap);
    EventBadSegment();
    inc_tcp_discards();
    return STREAM_INSERT_ANOMALY;
}

int SegmentOverlapEditor::left_overlap_keep_first( void )
{
    int rc = STREAM_INSERT_OK;

    DebugFormat(DEBUG_STREAM_STATE, "left overlap %d\n", overlap);

    len = tdb->pkt->dsize;
    overlap = left->seq + left->payload_size - seq;
    if( overlap > 0 )
    {
        // NOTE that overlap will always be less than left->size since
        // seq is always greater than left->seq
        tcpStats.overlaps++;
        overlap_count++;

        DebugMessage(DEBUG_STREAM_STATE, "left overlap, honoring old data\n");
        if( SEQ_LT( left->seq, tdb->seq ) && SEQ_GT( left->seq + left->payload_size, tdb->seq + tdb->pkt->dsize ) )
        {
            if (tcp_ips_data == NORM_MODE_ON)
            {
                unsigned offset = tdb->seq - left->seq;
                memcpy( ( uint8_t* ) tdb->pkt->data, left->payload + offset, tdb->pkt->dsize );
                tdb->pkt->packet_flags |= PKT_MODIFIED;
            }
            normStats[PC_TCP_IPS_DATA][tcp_ips_data]++;
            sfBase.iPegs[PERF_COUNT_TCP_IPS_DATA][tcp_ips_data]++;
        }
        else if( SEQ_LT( left->seq, tdb->seq ) )
        {
            if( tcp_ips_data == NORM_MODE_ON )
            {
                unsigned offset = tdb->seq - left->seq;
                unsigned length = left->seq + left->payload_size - tdb->seq;
                memcpy( ( uint8_t* ) tdb->pkt->data, left->payload + offset, length );
                tdb->pkt->packet_flags |= PKT_MODIFIED;
            }

            normStats[PC_TCP_IPS_DATA][tcp_ips_data]++;
            sfBase.iPegs[PERF_COUNT_TCP_IPS_DATA][tcp_ips_data]++;
        }

        seq += overlap;
        if( SEQ_LEQ( seq_end, seq ) )
            return generate_bad_segment_event();
    }

    return rc;
}

int SegmentOverlapEditor::left_overlap_trim_first(void)
{
    int rc = STREAM_INSERT_OK;

    DebugFormat(DEBUG_STREAM_STATE, "left overlap %d\n", overlap);

    len = tdb->pkt->dsize;
    overlap = left->seq + left->payload_size - seq;
    if( overlap > 0 )
    {
        tcpStats.overlaps++;
        overlap_count++;

        if( SEQ_LT( left->seq, seq ) && SEQ_GEQ( left->seq + left->payload_size, seq + len )  )
        {
            // existing packet overlaps new on both sides.  Drop the new data.
            DebugMessage(DEBUG_STREAM_STATE, "left overlap, honoring old data\n");
            seq += overlap;
            if( SEQ_LEQ( seq_end, seq ) )
                return generate_bad_segment_event();
        }

        /* Otherwise, trim the old data accordingly */
        left->payload_size -= ( int16_t ) overlap;
        seg_bytes_logical -= overlap;
        DebugMessage(DEBUG_STREAM_STATE, "left overlap, honoring new data\n");
    }

    return rc;
}

int SegmentOverlapEditor::left_overlap_keep_last( void )
{
    int rc = STREAM_INSERT_OK;

    DebugFormat(DEBUG_STREAM_STATE, "left overlap %d\n", overlap);

    len = tdb->pkt->dsize;
    overlap = left->seq + left->payload_size - seq;
    if( overlap > 0 )
    {
        tcpStats.overlaps++;
        overlap_count++;

        /* True "Last" policy" */
        if( SEQ_LT( left->seq, seq ) && SEQ_GT( left->seq + left->payload_size, seq + len ) )
        {
            /* New data is overlapped on both sides by existing data.  Existing data needs to be
             * split and the new data inserted in the middle.
             * Need to duplicate left. Adjust that seq by + (seq + len) and
             * size by - (seq + len - left->seq).
             */
            int rc = dup_reassembly_segment( tdb->pkt, left, &right );
            if( rc != STREAM_INSERT_OK )
                return rc;

            left->payload_size -= ( int16_t ) overlap;
            seg_bytes_logical -= overlap;

            right->seq = seq + len;
            uint16_t delta = ( int16_t ) ( right->seq - left->seq );
            right->payload_size -= delta;
            right->payload += delta;
            seg_bytes_logical -= delta;
        }
        else
        {
            left->payload_size -= (int16_t) overlap;
            seg_bytes_logical -= overlap;
        }

    }

    return rc;
}

void SegmentOverlapEditor::right_overlap_truncate_existing( void )
{
    DebugMessage(DEBUG_STREAM_STATE, "Got partial right overlap\n");
    if ( SEQ_EQ( right->seq, seq ) && ( reassembly_policy != ReassemblyPolicy::OS_LAST ) )
    {
        slide = ( right->seq + right->payload_size - seq );
        seq += slide;
    }
    else
    {
        /* partial overlap */
        right->seq += overlap;
        right->payload += overlap;
        right->payload_size -= (int16_t) overlap;
        seg_bytes_logical -= overlap;
        total_bytes_queued -= overlap;
    }
}

void SegmentOverlapEditor::right_overlap_truncate_new( void )
{
    if (tcp_ips_data == NORM_MODE_ON)
    {
        unsigned offset = right->seq - tdb->seq;
        unsigned length = tdb->seq + tdb->pkt->dsize - right->seq;
        memcpy( ( uint8_t* ) tdb->pkt->data + offset, right->payload, length );
        tdb->pkt->packet_flags |= PKT_MODIFIED;
    }

    normStats[PC_TCP_IPS_DATA][tcp_ips_data]++;
    sfBase.iPegs[PERF_COUNT_TCP_IPS_DATA][tcp_ips_data]++;
    trunc_len = overlap;
}

// REASSEMBLY_POLICY_FIRST:
// REASSEMBLY_POLICY_VISTA:
int SegmentOverlapEditor::full_right_overlap_truncate_new( void )
{
    DebugMessage(DEBUG_STREAM_STATE, "Got full right overlap, truncating new\n");

    if( tcp_ips_data == NORM_MODE_ON )
    {
        unsigned offset = right->seq - tdb->seq;
        memcpy( ( uint8_t* ) tdb->pkt->data + offset, right->payload, right->payload_size );
        tdb->pkt->packet_flags |= PKT_MODIFIED;
    }

    normStats[PC_TCP_IPS_DATA][tcp_ips_data]++;
    sfBase.iPegs[PERF_COUNT_TCP_IPS_DATA][tcp_ips_data]++;

    if( SEQ_EQ( right->seq, seq ) )
    {
        /* Overlap is greater than or equal to right->size
         * slide gets set before insertion */
        seq += right->payload_size;
        left = right;
        right = right->next;

        /* Adjusted seq is fully overlapped */
        if( SEQ_EQ( seq, seq_end ) )
            return generate_bad_segment_event();
    }
    else
    {
        /* seq is less than right->seq,  trunc length is reset to 0 at beginning of loop */
        trunc_len = overlap;

        /* insert this one, and see if we need to chunk it up
          Adjust slide so that is correct relative to orig seq */
        slide = seq - tdb->seq;
        int rc = add_reassembly_segment( tdb, len, slide, trunc_len, seq, left );
        if( rc != STREAM_INSERT_OK )
            return rc;

        // Set seq to end of right since overlap was greater than or equal to right->size and
        // inserted seq has been truncated to beginning of right and reset trunc length to 0
        // since we may fall out of loop if next right is NULL
        seq = right->seq + right->payload_size;
        left = right;
        right = right->next;
        trunc_len = 0;
    }

    return STREAM_INSERT_OK;
}

// REASSEMBLY_POLICY_WINDOWS:
// REASSEMBLY_POLICY_WINDOWS2K3:
// REASSEMBLY_POLICY_BSD:
// REASSEMBLY_POLICY_MACOS:
int SegmentOverlapEditor::full_right_overlap_os1( void )
{
    if( SEQ_GEQ( seq_end, right->seq + right->payload_size )  && SEQ_LT( seq, right->seq ) )
    {
        drop_old_segment();
    }
    else
    {
        int rc = full_right_overlap_truncate_new();
        if( rc != STREAM_INSERT_OK )
            return rc;
    }

    return STREAM_INSERT_OK;
}

// REASSEMBLY_POLICY_LINUX:
// REASSEMBLY_POLICY_HPUX10:
// REASSEMBLY_POLICY_IRIX:
int SegmentOverlapEditor::full_right_overlap_os2( void )
{
    if( SEQ_GEQ( seq_end, right->seq + right->payload_size ) && SEQ_LT( seq, right->seq ) )
    {
        drop_old_segment();
    }
    else if( SEQ_GT( seq_end, right->seq + right->payload_size ) && SEQ_EQ( seq, right->seq ) )
    {
        drop_old_segment();
    }
    else
    {
        int rc = full_right_overlap_truncate_new();
        if( rc != STREAM_INSERT_OK )
            return rc;
    }

    return STREAM_INSERT_OK;
}

// REASSEMBLY_POLICY_HPUX11:
// REASSEMBLY_POLICY_SOLARIS:
int SegmentOverlapEditor::full_right_overlap_os3( void )
{
    // If this packet is wholly overlapping and the same size as a previous one and we have not
    // received the one immediately preceeding, we take the FIRST.
    if( SEQ_EQ( right->seq, seq ) && ( right->payload_size == len )
            && ( left && !SEQ_EQ( left->seq + left->payload_size, seq ) ) )
    {
        trunc_len += overlap;
        if( SEQ_LEQ( ( int )( seq_end - trunc_len ), seq ) )
            return generate_bad_segment_event();
    }
    else
    {
        drop_old_segment();
    }

    return STREAM_INSERT_OK;
}

//  REASSEMBLY_POLICY_OLD_LINUX:
//  REASSEMBLY_POLICY_LAST:
int SegmentOverlapEditor::full_right_overlap_os4( void )
{
    drop_old_segment();
    return STREAM_INSERT_OK;
}

int SegmentOverlapEditor::full_right_overlap_os5( void )
{
    return full_right_overlap_truncate_new();
}
