//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
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
// nhttp_stream_splitter.cc author Tom Peters <thopeter@cisco.com>

#include <assert.h>
#include <sys/types.h>

#include "file_api/file_flows.h"
#include "nhttp_enum.h"
#include "nhttp_field.h"
#include "nhttp_test_manager.h"
#include "nhttp_test_input.h"
#include "nhttp_cutter.h"
#include "nhttp_inspect.h"
#include "nhttp_stream_splitter.h"

using namespace NHttpEnums;

// Convenience function. All housekeeping that must be done before we can return FLUSH to stream.
void NHttpStreamSplitter::prepare_flush(NHttpFlowData* session_data, uint32_t* flush_offset,
    SectionType section_type, uint32_t num_flushed, uint32_t num_excess, int32_t num_head_lines,
    bool is_broken_chunk, uint32_t num_good_chunks) const
{
    session_data->section_type[source_id] = section_type;
    session_data->num_excess[source_id] = num_excess;
    session_data->num_head_lines[source_id] = num_head_lines;
    session_data->is_broken_chunk[source_id] = is_broken_chunk;
    session_data->num_good_chunks[source_id] = num_good_chunks;

#ifdef REG_TEST
    if (NHttpTestManager::use_test_input())
    {
        NHttpTestManager::get_test_input_source()->flush(num_flushed);
    }
    else
#endif

    *flush_offset = num_flushed;
}

NHttpCutter* NHttpStreamSplitter::get_cutter(SectionType type,
    const NHttpFlowData* session_data) const
{
    switch (type)
    {
    case SEC_REQUEST:
        return (NHttpCutter*)new NHttpRequestCutter;
    case SEC_STATUS:
        return (NHttpCutter*)new NHttpStatusCutter;
    case SEC_HEADER:
    case SEC_TRAILER:
        return (NHttpCutter*)new NHttpHeaderCutter;
    case SEC_BODY_CL:
        return (NHttpCutter*)new NHttpBodyClCutter(session_data->data_length[source_id]);
    case SEC_BODY_CHUNK:
        return (NHttpCutter*)new NHttpBodyChunkCutter;
    case SEC_BODY_OLD:
        return (NHttpCutter*)new NHttpBodyOldCutter;
    default:
        assert(false);
        return nullptr;
    }
}

void NHttpStreamSplitter::chunk_spray(NHttpFlowData* session_data, uint8_t* buffer,
    const uint8_t* data, unsigned length) const
{
    ChunkState& curr_state = session_data->chunk_state[source_id];
    uint32_t& expected = session_data->chunk_expected_length[source_id];
    bool& is_broken_chunk = session_data->is_broken_chunk[source_id];
    uint32_t& num_good_chunks = session_data->num_good_chunks[source_id];

    if (is_broken_chunk && (num_good_chunks == 0))
        curr_state = CHUNK_BAD;

    for (uint32_t k=0; k < length; k++)
    {
        switch (curr_state)
        {
        case CHUNK_NUMBER:
            if (data[k] == '\r')
                curr_state = CHUNK_HCRLF;
            else if (data[k] == ';')
                curr_state = CHUNK_OPTIONS;
            else if (is_sp_tab[data[k]])
                curr_state = CHUNK_WHITESPACE;
            else
                expected = expected * 16 + as_hex[data[k]];
            break;
        case CHUNK_OPTIONS:
        case CHUNK_WHITESPACE:
            // No practical difference between white space and options in reassemble()
            if (data[k] == '\r')
                curr_state = CHUNK_HCRLF;
            break;
        case CHUNK_HCRLF:
            if (expected > 0)
                curr_state = CHUNK_DATA;
            else
            {
                // Terminating zero-length chunk
                assert(k+1 == length);
                curr_state = CHUNK_NUMBER;
            }
            break;
        case CHUNK_DATA:
          {
            const uint32_t skip_amount = (length-k <= expected) ? length-k : expected;
            const bool at_start = (session_data->body_octets[source_id] == 0) &&
                (session_data->section_offset[source_id] == 0);
            decompress_copy(buffer, session_data->section_offset[source_id], data+k, skip_amount,
                session_data->compression[source_id], session_data->compress_stream[source_id],
                at_start, session_data->infractions[source_id], session_data->events[source_id]);
            if ((expected -= skip_amount) == 0)
                curr_state = CHUNK_DCRLF1;
            k += skip_amount-1;
            break;
          }
        case CHUNK_DCRLF1:
            curr_state = CHUNK_DCRLF2;
            break;
        case CHUNK_DCRLF2:
            if (is_broken_chunk && (--num_good_chunks == 0))
                curr_state = CHUNK_BAD;
            else
            {
                curr_state = CHUNK_NUMBER;
                expected = 0;
            }
            break;
        case CHUNK_BAD:
          {
            const uint32_t skip_amount = length-k;
            const bool at_start = (session_data->body_octets[source_id] == 0) &&
                (session_data->section_offset[source_id] == 0);
            decompress_copy(buffer, session_data->section_offset[source_id], data+k, skip_amount,
                session_data->compression[source_id], session_data->compress_stream[source_id],
                at_start, session_data->infractions[source_id], session_data->events[source_id]);
            k += skip_amount-1;
            break;
          }
        case CHUNK_ZEROS:
            // Not a possible state in reassemble(). Here to avoid compiler warning.
            assert(false);
            break;
        }
    }
}

void NHttpStreamSplitter::decompress_copy(uint8_t* buffer, uint32_t& offset, const uint8_t* data,
    uint32_t length, NHttpEnums::CompressId& compression, z_stream*& compress_stream,
    bool at_start, NHttpInfractions& infractions, NHttpEventGen& events)
{
    if ((compression == CMP_GZIP) || (compression == CMP_DEFLATE))
    {
        compress_stream->next_in = (Bytef*)data;
        compress_stream->avail_in = length;
        compress_stream->next_out = buffer + offset;
        compress_stream->avail_out = MAX_OCTETS - offset;
        int ret_val = inflate(compress_stream, Z_SYNC_FLUSH);

        if ((ret_val == Z_OK) || (ret_val == Z_STREAM_END))
        {
            offset = MAX_OCTETS - compress_stream->avail_out;
            if (compress_stream->avail_in > 0)
            {
                // There are two ways not to consume all the input
                if (ret_val == Z_STREAM_END)
                {
                    // The zipped data stream ended but there is more input data
                    infractions += INF_GZIP_EARLY_END;
                    events.create_event(EVENT_GZIP_FAILURE);
                    const uInt num_copy =
                        (compress_stream->avail_in <= compress_stream->avail_out) ?
                        compress_stream->avail_in : compress_stream->avail_out;
                    memcpy(buffer + offset, data, num_copy);
                    offset += num_copy;
                }
                else
                {
                    assert(compress_stream->avail_out == 0);
                    // The data expanded too much
                    infractions += INF_GZIP_OVERRUN;
                    events.create_event(EVENT_GZIP_OVERRUN);
                }
                compression = CMP_NONE;
                inflateEnd(compress_stream);
                delete compress_stream;
                compress_stream = nullptr;
            }
            return;
        }
        else if ((compression == CMP_DEFLATE) && at_start && (ret_val == Z_DATA_ERROR))
        {
            // Some incorrect implementations of deflate don't use the expected header. Feed a
            // dummy header to zlib and retry the inflate.
            static constexpr char zlib_header[2] = { 0x78, 0x01 };

            inflateReset(compress_stream);
            compress_stream->next_in = (Bytef*)zlib_header;
            compress_stream->avail_in = sizeof(zlib_header);
            inflate(compress_stream, Z_SYNC_FLUSH);

            // Start over at the beginning
            decompress_copy(buffer, offset, data, length, compression, compress_stream, false,
                infractions, events);
            return;
        }
        else
        {
            infractions += INF_GZIP_FAILURE;
            events.create_event(EVENT_GZIP_FAILURE);
            compression = CMP_NONE;
            inflateEnd(compress_stream);
            delete compress_stream;
            compress_stream = nullptr;
            // Since we failed to uncompress the data, fall through
        }
    }

    // The following precaution is necessary because mixed compressed and uncompressed data can
    // cause the buffer to overrun even though we are not decompressing right now
    if (length > MAX_OCTETS - offset)
    {
        length = MAX_OCTETS - offset;
        infractions += INF_GZIP_OVERRUN;
        events.create_event(EVENT_GZIP_OVERRUN);
    }
    memcpy(buffer + offset, data, length);
    offset += length;
}

StreamSplitter::Status NHttpStreamSplitter::scan(Flow* flow, const uint8_t* data, uint32_t length,
    uint32_t, uint32_t* flush_offset)
{
    assert(length <= MAX_OCTETS);

    // This is the session state information we share with NHttpInspect and store with stream. A
    // session is defined by a TCP connection. Since scan() is the first to see a new TCP
    // connection the new flow data object is created here.
    NHttpFlowData* session_data = (NHttpFlowData*)flow->get_application_data(
        NHttpFlowData::nhttp_flow_id);
    if (session_data == nullptr)
    {
        flow->set_application_data(session_data = new NHttpFlowData);
    }
    assert(session_data != nullptr);

    const SectionType type = session_data->type_expected[source_id];

    if (type == SEC_ABORT)
        return StreamSplitter::ABORT;

#ifdef REG_TEST
    if (NHttpTestManager::use_test_input())
    {
        // This block substitutes a completely new data buffer supplied by the test tool in place
        // of the "real" data. It also rewrites the buffer length.
        *flush_offset = length;
        uint8_t* test_data = nullptr;
        NHttpTestManager::get_test_input_source()->scan(test_data, length, source_id,
            session_data->seq_num);
        if (length == 0)
            return StreamSplitter::FLUSH;
        data = test_data;
    }
    else if (NHttpTestManager::use_test_output())
    {
        printf("Scan from flow data %" PRIu64 " direction %d length %u\n", session_data->seq_num,
            source_id, length);
        fflush(stdout);
    }
#endif

    assert(!session_data->tcp_close[source_id]);

    NHttpCutter*& cutter = session_data->cutter[source_id];
    if (cutter == nullptr)
    {
        cutter = get_cutter(type, session_data);
        assert(cutter != nullptr);
    }
    const uint32_t max_length = MAX_OCTETS - cutter->get_octets_seen();
    const ScanResult cut_result = cutter->cut(data, (length <= max_length) ? length :
        max_length, session_data->infractions[source_id], session_data->events[source_id],
        session_data->section_size_target[source_id], session_data->section_size_max[source_id]);
    switch (cut_result)
    {
    case SCAN_NOTFOUND:
        if (cutter->get_octets_seen() == MAX_OCTETS)
        {
            session_data->infractions[source_id] += INF_ENDLESS_HEADER;
            // FIXIT-H suspicion that this alert is never generated.
            session_data->events[source_id].create_event(EVENT_LOSS_OF_SYNC);
            // FIXIT-H need to process this data not just discard it.
            session_data->type_expected[source_id] = SEC_ABORT;
            delete cutter;
            cutter = nullptr;
            return StreamSplitter::ABORT;
        }
        // Incomplete headers wait patiently for more data
#ifdef REG_TEST
        if (NHttpTestManager::use_test_input())
            return StreamSplitter::FLUSH;
        else
#endif
        return StreamSplitter::SEARCH;
    case SCAN_ABORT:
    case SCAN_END: // FIXIT-H need StreamSplitter::END
        session_data->type_expected[source_id] = SEC_ABORT;
        delete cutter;
        cutter = nullptr;
        return StreamSplitter::ABORT;
    case SCAN_DISCARD:
    case SCAN_DISCARD_PIECE:
        prepare_flush(session_data, flush_offset, SEC_DISCARD, cutter->get_num_flush(), 0, 0,
            false, 0);
        if (cut_result == SCAN_DISCARD)
        {
            delete cutter;
            cutter = nullptr;
        }
        return StreamSplitter::FLUSH;
    case SCAN_FOUND:
    case SCAN_FOUND_PIECE:
      {
        const uint32_t flush_octets = cutter->get_num_flush();
        prepare_flush(session_data, flush_offset, type, flush_octets, cutter->get_num_excess(),
            cutter->get_num_head_lines(), cutter->get_is_broken_chunk(),
            cutter->get_num_good_chunks());
        if (cut_result == SCAN_FOUND)
        {
            delete cutter;
            cutter = nullptr;
        }
        return StreamSplitter::FLUSH;
      }
    default:
        assert(false);
        return StreamSplitter::ABORT;
    }
}

const StreamBuffer* NHttpStreamSplitter::reassemble(Flow* flow, unsigned total, unsigned,
    const uint8_t* data, unsigned len, uint32_t flags, unsigned& copied)
{
    static THREAD_LOCAL StreamBuffer nhttp_buf;

    copied = len;

    assert(total <= MAX_OCTETS);

    NHttpFlowData* session_data = (NHttpFlowData*)flow->get_application_data(
        NHttpFlowData::nhttp_flow_id);
    assert(session_data != nullptr);

#ifdef REG_TEST
    if (NHttpTestManager::use_test_output())
    {
        if (NHttpTestManager::use_test_input())
        {
            if (!(flags & PKT_PDU_TAIL))
            {
                return nullptr;
            }
            bool tcp_close;
            uint8_t* test_buffer;
            NHttpTestManager::get_test_input_source()->reassemble(&test_buffer, len, source_id,
                tcp_close);
            if (tcp_close)
            {
                finish(flow);
            }
            if (test_buffer == nullptr)
            {
                // Source ID does not match test data, no test data was flushed, or there is no
                // more test data
                return nullptr;
            }
            data = test_buffer;
            total = len;
        }
        else
        {
            printf("Reassemble from flow data %" PRIu64 " direction %d total %u length %u\n",
                session_data->seq_num, source_id, total, len);
            fflush(stdout);
        }
    }
#endif

    if (session_data->section_type[source_id] == SEC__NOTCOMPUTE)
    {   // FIXIT-M In theory this check should not be necessary
        return nullptr;
    }

    // FIXIT-P stream should be enhanced to do discarding for us. For now flush-then-discard here
    // is how scan() handles things we don't need to examine.
    if (session_data->section_type[source_id] == SEC_DISCARD)
    {
#ifdef REG_TEST
        if (NHttpTestManager::use_test_output())
        {
            fprintf(NHttpTestManager::get_output_file(), "Discarded %u octets\n\n", len);
            fflush(NHttpTestManager::get_output_file());
        }
#endif
        if (flags & PKT_PDU_TAIL)
        {
            session_data->section_type[source_id] = SEC__NOTCOMPUTE;

            // When we are skipping through a message body beyond flow depth this is the end of
            // the line. Here we do the message section's normal job of updating the flow for the
            // next stage.
            if (session_data->cutter[source_id] == nullptr)
            {
                if (session_data->type_expected[source_id] == SEC_BODY_CL)
                {
                    session_data->type_expected[source_id] = (source_id == SRC_CLIENT) ?
                        SEC_REQUEST : SEC_STATUS;
                    session_data->half_reset(source_id);
                }
                else if (session_data->type_expected[source_id] == SEC_BODY_CHUNK)
                {
                    session_data->trailer_prep(source_id);
                }
            }
        }
        return nullptr;
    }

    uint8_t*& buffer = session_data->section_buffer[source_id];

    const bool is_body = (session_data->section_type[source_id] == SEC_BODY_CHUNK) ||
                         (session_data->section_type[source_id] == SEC_BODY_CL) ||
                         (session_data->section_type[source_id] == SEC_BODY_OLD);
    if (buffer == nullptr)
    {
        if (is_body)
        {
            buffer = NHttpInspect::body_buffer;
        }
        else
        {
            buffer = new uint8_t[total];
        }
    }

    if (session_data->section_type[source_id] != SEC_BODY_CHUNK)
    {
        const bool at_start = (session_data->body_octets[source_id] == 0) &&
             (session_data->section_offset[source_id] == 0);
        decompress_copy(buffer, session_data->section_offset[source_id], data, len,
            session_data->compression[source_id], session_data->compress_stream[source_id],
            at_start, session_data->infractions[source_id], session_data->events[source_id]);
    }
    else
    {
        chunk_spray(session_data, buffer, data, len);
    }

    if (flags & PKT_PDU_TAIL)
    {
        const Field& send_to_detection = my_inspector->process(buffer,
            session_data->section_offset[source_id] - session_data->num_excess[source_id], flow,
            source_id, !is_body);

        session_data->section_offset[source_id] = 0;

        // Buffers are reset to nullptr without delete[] because NHttpMsgSection holds the pointer
        // and is responsible
        if (send_to_detection.length > 0)
        {
            nhttp_buf.data = send_to_detection.start;
            nhttp_buf.length = send_to_detection.length;
            assert((nhttp_buf.length <= MAX_OCTETS) && (nhttp_buf.length != 0));
            buffer = nullptr;
#ifdef REG_TEST
            if (NHttpTestManager::use_test_output())
            {
                fprintf(NHttpTestManager::get_output_file(), "Sent to detection %u octets\n\n",
                    nhttp_buf.length);
                fflush(NHttpTestManager::get_output_file());
            }
#endif
            return &nhttp_buf;
        }
        my_inspector->clear(session_data, source_id);
        buffer = nullptr;
    }
    return nullptr;
}

bool NHttpStreamSplitter::finish(Flow* flow)
{
    NHttpFlowData* session_data = (NHttpFlowData*)flow->get_application_data(
        NHttpFlowData::nhttp_flow_id);

    assert(session_data != nullptr);

#ifdef REG_TEST
    if (NHttpTestManager::use_test_output() && !NHttpTestManager::use_test_input())
    {
        printf("Finish from flow data %" PRIu64 " direction %d\n", session_data->seq_num,
            source_id);
        fflush(stdout);
    }
#endif

    if (session_data->type_expected[source_id] == SEC_ABORT)
    {
        return false;
    }

    session_data->tcp_close[source_id] = true;

    // If there is leftover data for which we returned PAF_SEARCH and never flushed, we need to set
    // up to process because it is about to go to reassemble(). But we don't support partial start
    // lines.
    if ((session_data->section_type[source_id] == SEC__NOTCOMPUTE) &&
        (session_data->cutter[source_id] != nullptr)               &&
        (session_data->cutter[source_id]->get_octets_seen() > 0))
    {
        if ((session_data->type_expected[source_id] == SEC_REQUEST) ||
            (session_data->type_expected[source_id] == SEC_STATUS))
        {
            session_data->infractions[source_id] += INF_PARTIAL_START;
            session_data->events[source_id].create_event(EVENT_LOSS_OF_SYNC);
            return false;
        }

        uint32_t not_used;
        prepare_flush(session_data, &not_used, session_data->type_expected[source_id], 0, 0,
            session_data->cutter[source_id]->get_num_head_lines() + 1,
            session_data->cutter[source_id]->get_is_broken_chunk(),
            session_data->cutter[source_id]->get_num_good_chunks());
        return true;
    }

    // If there is no more data to process we need to wrap up file processing right now
    if ((session_data->section_type[source_id] == SEC__NOTCOMPUTE) &&
        (session_data->file_depth_remaining[source_id] > 0)        &&
        (session_data->cutter[source_id] != nullptr)               &&
        (session_data->cutter[source_id]->get_octets_seen() == 0))
    {
        if (source_id == SRC_SERVER)
        {
            FileFlows* file_flows = FileFlows::get_file_flows(flow);
            file_flows->file_process(nullptr, 0, SNORT_FILE_END, false);
        }
        else
        {
            session_data->mime_state->process_mime_data(flow, nullptr, 0, true,
                SNORT_FILE_END);
            delete session_data->mime_state;
            session_data->mime_state = nullptr;
        }
        return false;
    }

    return true;
}

