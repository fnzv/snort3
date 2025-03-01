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
// packet_manager.cc author Josh Rosenbaum <jrosenba@cisco.com>

#include "protocols/packet_manager.h"

#include <vector>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <limits>
#include <type_traits> // static_assert

#include "framework/codec.h"
#include "managers/codec_manager.h"
#include "main/snort_config.h"
#include "main/thread.h"
#include "log/messages.h"
#include "detection/fp_detect.h"

#include "protocols/packet.h"
#include "protocols/protocol_ids.h"
#include "protocols/eth.h"
#include "protocols/icmp4.h"
#include "protocols/icmp6.h"
#include "time/profiler.h"
#include "parser/parser.h"

#include "codecs/codec_module.h"
#include "codecs/ip/checksum.h" /* FIXIT */
#include "utils/stats.h"
#include "log/text_log.h"
#include "main/snort_debug.h"
#include "stream/stream_api.h"
#include "packet_io/sfdaq.h"
#include "packet_io/active.h"

#ifdef PERF_PROFILING
THREAD_LOCAL ProfileStats decodePerfStats;
#endif

// Decoding statistics

// this may be my longer member declaration ... ever
THREAD_LOCAL std::array<PegCount,PacketManager::stat_offset +
CodecManager::s_protocols.size()> PacketManager::s_stats {
    { 0 }
};

//PacketManager::s_stats{{0}};
std::array<PegCount, PacketManager::s_stats.size()> PacketManager::g_stats;

// names which will be printed for the first three statistics
// in s_stats/g_stats
const std::array<const char*, PacketManager::stat_offset> PacketManager::stat_names =
{
    {
        "total",
        "other",
        "discards"
    }
};

// Encoder Foo
static THREAD_LOCAL Packet* encode_pkt = nullptr;
static THREAD_LOCAL PegCount total_rebuilt_pkts = 0;
static THREAD_LOCAL std::array<uint8_t, Codec::PKT_MAX> s_pkt {
    { 0 }
};
static THREAD_LOCAL uint8_t* dst_mac = nullptr;

//-------------------------------------------------------------------------
// Private helper functions
//-------------------------------------------------------------------------

static inline void push_layer(Packet* p,
    uint16_t prot_id,
    const uint8_t* hdr_start,
    uint32_t len)
{
    // We check to ensure num_layer < MAX_LAYERS before this function call
    Layer& lyr = p->layers[p->num_layers++];
    lyr.prot_id = prot_id;
    lyr.start = hdr_start;
    lyr.length = (uint16_t)len;
//    lyr.invalid_bits = p->byte_skip;  -- currently unused
}

void PacketManager::pop_teredo(Packet* p, RawData& raw)
{
    p->proto_bits &= ~PROTO_BIT__TEREDO;
    if ( SnortConfig::tunnel_bypass_enabled(TUNNEL_TEREDO) )
        Active::clear_tunnel_bypass();

    const uint8_t mapped_prot = CodecManager::s_proto_map[PROTO_TEREDO];
    s_stats[mapped_prot + stat_offset]--;
    p->num_layers--;

    const Layer& lyr = p->layers[p->num_layers];
    const uint16_t lyr_len = raw.data - lyr.start;
    raw.data = lyr.start;
    raw.len += lyr_len;
}

//-------------------------------------------------------------------------
// Initialization and setup
//-------------------------------------------------------------------------

Packet* PacketManager::encode_new(bool packet_data)
{
    Packet* p = (Packet*)SnortAlloc(sizeof(*p));
    Layer* lyr = new Layer[CodecManager::max_layers];

    if ( !p || !lyr)
        FatalError("encode_new() => Failed to allocate packet\n");

    if (!packet_data)
    {
        p->pkt = nullptr;
        p->pkth = nullptr;
    }
    else
    {
        uint8_t* b = (uint8_t*)SnortAlloc(sizeof(*p->pkth) + Codec::PKT_MAX + SPARC_TWIDDLE);

        if (!b)
            FatalError("encode_new() => Failed to allocate packet\n");

        p->pkth = (DAQ_PktHdr_t*)b;
        b += sizeof(*p->pkth);
        b += SPARC_TWIDDLE;
        p->pkt = b;
    }

    p->layers = lyr;
    return p;
}

void PacketManager::encode_delete(Packet* p)
{
    if (p)
    {
        if (p->pkth)
            free((void*)p->pkth);

        if (p->layers)
            delete[] p->layers;

        p->pkth = nullptr;
        p->layers = nullptr;
        free(p);
    }
}

// Assertions required for this code to work

//  Look below inside main decode() loop for these static_asserts
static_assert(CODEC_ENCAP_LAYER == (CODEC_UNSURE_ENCAP | CODEC_SAVE_LAYER),
    "If this is an encapsulated layer, you must also set UNSURE_ENCAP"
    " and SAVE_LAYER");

RawData::RawData(const DAQ_PktHdr_t* h, const uint8_t* p)
{
    pkth = h;
    data = p;
    len = h->caplen;
}

//-------------------------------------------------------------------------
// Encode/Decode functions
//-------------------------------------------------------------------------
void PacketManager::decode(
    Packet* p, const DAQ_PktHdr_t* pkthdr, const uint8_t* pkt, bool cooked)
{
    PERF_PROFILE(decodePerfStats);

    DecodeData unsure_encap_ptrs;

    uint8_t mapped_prot = CodecManager::grinder;
    uint16_t prev_prot_id = CodecManager::grinder_id;

    RawData raw(pkthdr, pkt);
    CodecData codec_data(FINISHED_DECODE);

    if ( cooked )
        codec_data.codec_flags |= CODEC_STREAM_REBUILT;

    // initialize all Packet information
    p->reset();
    p->pkth = pkthdr;
    p->pkt = pkt;
    p->ptrs.reset();
    layer::set_packet_pointer(p);

    s_stats[total_processed]++;

    // loop until the protocol id is no longer valid
    while (CodecManager::s_protocols[mapped_prot]->decode(raw, codec_data, p->ptrs))
    {
        DebugFormat(DEBUG_DECODE, "Codec %s (protocol_id: %u:"
            "ip header starts at: %p, length is %lu\n",
            CodecManager::s_protocols[mapped_prot]->get_name(),
            codec_data.next_prot_id, pkt, codec_data.lyr_len);

        /*
         * We only want the layer immediately following SAVE_LAYER to have the
         * UNSURE_ENCAP flag set.  So, if this is a SAVE_LAYER, zero out the
         * bit and the next time around, when this is no longer SAVE_LAYER,
         * we will zero out the UNSURE_ENCAP flag.
         */
        if (codec_data.codec_flags & CODEC_SAVE_LAYER)
        {
            codec_data.codec_flags &= ~CODEC_SAVE_LAYER;
            unsure_encap_ptrs = p->ptrs;
        }
        else
        {
            codec_data.codec_flags &= ~CODEC_UNSURE_ENCAP;
        }

        if (codec_data.proto_bits & (PROTO_BIT__IP | PROTO_BIT__IP6_EXT))
        {
            // FIXIT-M refactor when ip_proto's become an array
            if ( p->is_fragment() )
            {
                if ( prev_prot_id == IPPROTO_ID_FRAGMENT )
                {
                    const ip::IP6Frag* const fragh =
                        reinterpret_cast<const ip::IP6Frag*>(raw.data);
                    p->ip_proto_next = fragh->next();
                }
                else
                {
                    p->ip_proto_next = p->ptrs.ip_api.get_ip4h()->proto();
                }
            }
            else
            {
                p->ip_proto_next = (uint8_t)codec_data.next_prot_id;
            }
        }

        // If we have reached the MAX_LAYERS, we keep decoding
        // but no longer keep track of the layers.
        if ( p->num_layers == CodecManager::max_layers )
            SnortEventqAdd(GID_DECODE, DECODE_TOO_MANY_LAYERS);
        else
            push_layer(p, prev_prot_id, raw.data, codec_data.lyr_len);

        // internal statistics and record keeping
        s_stats[mapped_prot + stat_offset]++; // add correct decode for previous layer
        mapped_prot = CodecManager::s_proto_map[codec_data.next_prot_id];
        prev_prot_id = codec_data.next_prot_id;

        // set for next call
        const uint16_t curr_lyr_len = codec_data.lyr_len + codec_data.invalid_bytes;
        assert(curr_lyr_len <= raw.len);
        raw.len -= curr_lyr_len;
        raw.data += curr_lyr_len;
        p->proto_bits |= codec_data.proto_bits;
        codec_data.next_prot_id = FINISHED_DECODE;
        codec_data.lyr_len = 0;
        codec_data.invalid_bytes = 0;
        codec_data.proto_bits = 0;
    }

    DebugFormat(DEBUG_DECODE, "Codec %s (protocol_id: %hu: ip header"
        " starts at: %p, length is %lu\n",
        CodecManager::s_protocols[mapped_prot]->get_name(),
        prev_prot_id, pkt, (unsigned long)codec_data.lyr_len);

    s_stats[mapped_prot + stat_offset]++;

    // if the final protocol ID is not the default codec, a Codec failed
    if (prev_prot_id != FINISHED_DECODE)
    {
        if (codec_data.codec_flags & CODEC_UNSURE_ENCAP)
        {
            p->ptrs = unsure_encap_ptrs;

            switch (p->layers[p->num_layers-1].prot_id)
            {
            case IPPROTO_ID_ESP:
                // Hardcoding ESP because we trust iff the layer
                // immediately preceding the fail is ESP.
                p->ptrs.decode_flags |= DECODE_PKT_TRUST;
                break;

            case PROTO_TEREDO:
                // if we just decoded teredo and the next
                // layer fails, we made a mistake. Therefore,
                // remove this bit.
                pop_teredo(p, raw);
                break;
            } /* switch */
        }
        else
        {
            if ( (p->layers[p->num_layers-1].prot_id == PROTO_TEREDO) &&
                (prev_prot_id == IPPROTO_IPV6) )
            {
                pop_teredo(p, raw);
            }

            // if the codec exists, it failed
            if (CodecManager::s_proto_map[prev_prot_id])
            {
                s_stats[discards]++;
            }
            else
            {
                s_stats[other_codecs]++;

                if ( (MIN_UNASSIGNED_IP_PROTO <= prev_prot_id) &&
                    (prev_prot_id <= std::numeric_limits<uint8_t>::max()) &&
                    !(codec_data.codec_flags & CODEC_STREAM_REBUILT) )
                {
                    SnortEventqAdd(GID_DECODE, DECODE_IP_UNASSIGNED_PROTO);
                }
            }
        }
    }

    // set any final Packet fields
    p->data = raw.data;
    p->dsize = (uint16_t)raw.len;
    p->proto_bits |= codec_data.proto_bits;

    if ( !p->proto_bits )
        p->proto_bits = PROTO_BIT__OTHER;
}

//-------------------------------------------------------------------------
// encoders operate layer by layer:
//-------------------------------------------------------------------------

//-------------------------------------------------------------------------
// encoders:
// - raw pkt data only, no need for Packet stuff except to facilitate
//   encoding
// - don't include original options
// - inner layer differs from original (eg tcp data segment becomes rst)
// - must ensure proper ttl/hop limit for reverse direction
// - sparc twiddle must be factored in packet start for transmission
//
// iterate over decoded layers and encode the response packet.  actually
// make nested calls.  on the way in we setup invariant stuff and as we
// unwind the stack we finish up encoding in a more normal fashion (now
// the outer layer knows the length of the inner layer, etc.).
//
// when multiple responses are sent, both forwards and backwards directions,
// or multiple ICMP types (unreachable port, host, net), it may be possible
// to reuse the 1st encoding and just tweak it.  optimization for later
// consideration.

// pci is copied from in to out
// * addresses / ports are swapped if !fwd
// * options, etc. are stripped
// * checksums etc. are set
// * if next layer is udp, it is set to icmp unreachable w/udp
// * if next layer is tcp, it becomes a tcp rst or tcp fin w/opt data
//-------------------------------------------------------------------------

static inline uint8_t GetTTL(const Packet* const p, bool forward)
{
    char dir;
    uint8_t ttl;
    const bool outer = p->ptrs.ip_api.is_ip();

    if ( !p->flow )
        return 0;

    if ( p->packet_flags & PKT_FROM_CLIENT )
        dir = forward ? FROM_CLIENT : FROM_SERVER;
    else
        dir = forward ? FROM_SERVER : FROM_CLIENT;

    // outermost ip is considered to be outer here,
    // even if it is the only ip layer ...
    ttl = stream.get_session_ttl(p->flow, dir, outer);

    // if we don't get outer, we use inner
    if ( 0 == ttl && outer )
        ttl = stream.get_session_ttl(p->flow, dir, false);

    return ttl;
}

bool PacketManager::encode(const Packet* p,
    EncodeFlags flags,
    uint8_t lyr_start,
    uint8_t next_prot,
    Buffer& buf)
{
    if ( encode_pkt )
        p = encode_pkt;

    uint8_t ttl = GetTTL(p, (flags & ENC_FLAG_FWD));
    if ( ttl )
        flags |=  ENC_FLAG_TTL;
    else
        ttl = 0;

    if ( DAQ_GetInterfaceMode(p->pkth) == DAQ_MODE_INLINE )
        flags |= ENC_FLAG_INLINE;

    ip::IpApi tmp_api;
    EncState enc(tmp_api, flags, next_prot, ttl, p->dsize);

    const Layer* const lyrs = p->layers;
    int8_t outer_layer = lyr_start;
    int8_t inner_layer = lyr_start;

    // We need the IP layer associated with every protocol
    // so checksums can be computed.
    while (layer::set_inner_ip_api(p, tmp_api, inner_layer))
    {
        for (int i = outer_layer; i > inner_layer; --i)
        {
            const Layer& l = lyrs[i];
            uint8_t mapped_prot = i ? CodecManager::s_proto_map[l.prot_id] : CodecManager::grinder;
            if (!CodecManager::s_protocols[mapped_prot]->encode(l.start, l.length, enc, buf))
            {
                return false;
            }
        }
        outer_layer = inner_layer;
        // inner_layer is set in 'layer::set_inner_ip_api'
    }

    // Now, we can encode all of the layers between the DLT and
    // outermost IP layer
    tmp_api.reset();
    for (int i = outer_layer; i >= 0; --i)
    {
        const Layer& l = lyrs[i];
        uint8_t mapped_prot = i ? CodecManager::s_proto_map[l.prot_id] : CodecManager::grinder;

        if (!CodecManager::s_protocols[mapped_prot]->encode(l.start, l.length, enc, buf))
        {
            return false;
        }
    }

    return true;
}

const uint8_t* PacketManager::encode_response(
    TcpResponse type, EncodeFlags flags, const Packet* p, uint32_t& len,
    const uint8_t* const payload, uint32_t payload_len)
{
    Buffer buf(s_pkt.data(), s_pkt.size());

    switch (type)
    {
    case TcpResponse::FIN:
        if (payload && (payload_len > 0))
        {
            if (!buf.allocate(payload_len))
                return nullptr;

            memcpy(buf.data(), payload, payload_len);
            flags |= ENC_FLAG_PAY;
        }
        flags |= ENC_FLAG_FIN;
        break;

    case TcpResponse::PUSH:
        if (payload && (payload_len > 0))
        {
            if (!buf.allocate(payload_len))
                return nullptr;

            memcpy(buf.data(), payload, payload_len);
            flags |= ENC_FLAG_PAY;
        }
        flags |= ENC_FLAG_PSH;
        break;

    case TcpResponse::RST:      // No payload, so do nothing.
    default:     // future proof
        break;
    }

    // FIXIT-M  -- check flags if we should skip something
    if (encode(p, flags, p->num_layers-1, ENC_PROTO_UNSET, buf))
    {
        len = buf.size();
        return buf.data() + buf.off;
    }

    len = 0;
    return nullptr;
}

const uint8_t* PacketManager::encode_reject(UnreachResponse type,
    EncodeFlags flags, const Packet* p, uint32_t& len)
{
    Buffer buf(s_pkt.data(), s_pkt.size());

    if (p->is_ip4())
    {
        // FIXIT-M  -- check flags if we should skip something
        const int inner_ip_index = layer::get_inner_ip_lyr_index(p);
        assert(inner_ip_index >= 0);
        assert(inner_ip_index+1 < p->num_layers);

        /*  Building this packet from the inside out */
        if (!buf.allocate(icmp::ICMP_UNREACH_DATA_LEN))
            return nullptr;

        memcpy(buf.data(), p->layers[inner_ip_index+1].start, icmp::ICMP_UNREACH_DATA_LEN);

        const ip::IP4Hdr* const ip4h =
            reinterpret_cast<const ip::IP4Hdr*>(p->layers[inner_ip_index].start);
        const uint8_t ip_len = ip4h->hlen();

        if (!buf.allocate(ip_len))
            return nullptr;
        memcpy(buf.data(), ip4h, ip_len);

        // If this returns false, we're down pig creek.
        if (!buf.allocate(sizeof(icmp::Icmp4Base)))
            return nullptr;

        icmp::Icmp4Base* const icmph = reinterpret_cast<icmp::Icmp4Base*>(buf.data());
        icmph->type = icmp::IcmpType::DEST_UNREACH;
        icmph->csum = 0;
        icmph->opt32 = 0;

        switch (type)
        {
        case UnreachResponse::NET:
            icmph->code = icmp::IcmpCode::NET_UNREACH;
            break;
        case UnreachResponse::HOST:
            icmph->code = icmp::IcmpCode::HOST_UNREACH;
            break;
        case UnreachResponse::PORT:
            icmph->code = icmp::IcmpCode::PORT_UNREACH;
            break;
        case UnreachResponse::FWD:
            icmph->code = icmp::IcmpCode::PKT_FILTERED;
            break;
        default:     // future proofing
            icmph->code = icmp::IcmpCode::PORT_UNREACH;
        }

        icmph->csum = checksum::icmp_cksum((uint16_t*)buf.data(), buf.size());

        if (encode(p, flags, inner_ip_index, IPPROTO_ID_ICMPV4, buf))
        {
            len = buf.size();
            return buf.data() + buf.off;
        }

        len = 0;
        return nullptr;
    }
    else if (p->is_ip6())
    {
        // FIXIT-M  -- check flags if we should skip ip6_options
        const int inner_ip_index = layer::get_inner_ip_lyr_index(p);
        assert(inner_ip_index >= 0);
        assert(inner_ip_index+1 < p->num_layers);

        // FIXIT-L: copy up to minimum MTU worth of data
        // FIXIT-L  check if we have the full 8 bytes of data.
        if (!buf.allocate(icmp::ICMP_UNREACH_DATA_LEN))
            return nullptr;
        memcpy(buf.data(), p->layers[inner_ip_index+1].start, icmp::ICMP_UNREACH_DATA_LEN);

        // copy original ip header
        if (!buf.allocate(ip::IP6_HEADER_LEN))
            return nullptr;
        const ip::IP6Hdr* const ip6h = p->ptrs.ip_api.get_ip6h();
        memcpy(buf.data(), ip6h, ip::IP6_HEADER_LEN);

        if (!buf.allocate(sizeof(icmp::Icmp6Hdr)))
            return nullptr;

        icmp::Icmp6Hdr* const icmph = reinterpret_cast<icmp::Icmp6Hdr*>(buf.data());
        icmph->type = icmp::Icmp6Types::UNREACH;
        icmph->csum = 0;
        icmph->opt32 = 0;

        switch (type)
        {
        case UnreachResponse::NET:
            icmph->code = icmp::Icmp6Code::UNREACH_NET;
            break;
        case UnreachResponse::HOST:
            icmph->code = icmp::Icmp6Code::UNREACH_HOST;
            break;
        case UnreachResponse::PORT:
            icmph->code = icmp::Icmp6Code::UNREACH_PORT;
            break;
        case UnreachResponse::FWD:
            icmph->code = icmp::Icmp6Code::UNREACH_FILTER_PROHIB;
            break;
        default:     // future proofing
            icmph->code = icmp::Icmp6Code::UNREACH_PORT;
        }

        checksum::Pseudoheader6 ps6;
        const int ip_len = buf.size();
        memcpy(ps6.sip, ip6h->get_src()->u6_addr8, sizeof(ps6.sip));
        memcpy(ps6.dip, ip6h->get_dst()->u6_addr8, sizeof(ps6.dip));
        ps6.zero = 0;
        ps6.protocol = IPPROTO_ICMPV6;
        ps6.len = htons((uint16_t)(ip_len));

        icmph->csum = checksum::icmp_cksum((uint16_t*)buf.data(), ip_len, &ps6);

        if (encode(p, flags, inner_ip_index, IPPROTO_ICMPV6, buf))
        {
            len = buf.size();
            return buf.data() + buf.off;
        }

        len = 0;
        return nullptr;
    }
    else
    {
        return nullptr;
    }
}

static void set_hdr(
    const Packet* p, Packet* c, const DAQ_PktHdr_t* phdr, uint32_t opaque)
{
    c->reset();

#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
    if ( !phdr )
        phdr = p->pkth;

    DAQ_PktHdr_t* pkth = (DAQ_PktHdr_t*)c->pkth;
    pkth->ingress_index = phdr->ingress_index;
    pkth->ingress_group = phdr->ingress_group;
    pkth->egress_index = phdr->egress_index;
    pkth->egress_group = phdr->egress_group;
    pkth->flags = phdr->flags & (~DAQ_PKT_FLAG_HW_TCP_CS_GOOD);
    pkth->address_space_id = phdr->address_space_id;
    pkth->opaque = opaque;
#else
#ifdef HAVE_DAQ_ACQUIRE_WITH_META
    DAQ_PktHdr_t* pkth = (DAQ_PktHdr_t*)c->pkth;
    pkth->opaque = p->pkth->opaque;
#else
    UNUSED(p);
#endif
    UNUSED(phdr);
    UNUSED(opaque);
#endif
}

//-------------------------------------------------------------------------
// formatters:
// - these packets undergo detection
// - need to set Packet stuff except for frag which calls grinder
// - include original options except for frag inner ip
// - inner layer header is very similar but payload differs
// - original ttl is always used
//-------------------------------------------------------------------------

int PacketManager::format_tcp(
    EncodeFlags, const Packet* p, Packet* c, PseudoPacketType type,
    const DAQ_PktHdr_t* phdr, uint32_t opaque)
{
    c->reset();
    DAQ_PktHdr_t* pkth = (DAQ_PktHdr_t*)c->pkth;
    set_hdr(p, c, phdr, opaque);

    c->packet_flags |= PKT_PSEUDO;
    c->pseudo_type = type;
    c->user_policy_id = p->user_policy_id;  // cooked packet gets same policy as raw

    // setup pkt capture header
    pkth->caplen = 0;
    pkth->pktlen = 0;
    pkth->ts = p->pkth->ts;

    total_rebuilt_pkts++;  // update local counter
    return 0;
}

int PacketManager::encode_format(
    EncodeFlags f, const Packet* p, Packet* c, PseudoPacketType type,
    const DAQ_PktHdr_t* phdr, uint32_t opaque)
{
    int i;
    int len;
    bool update_ip4_len = false;
    uint8_t num_layers = p->num_layers;
    DAQ_PktHdr_t* pkth = (DAQ_PktHdr_t*)c->pkth;

    if ( num_layers <= 0 )
        return -1;

    c->reset();
    set_hdr(p, c, phdr, opaque);

    if ( f & ENC_FLAG_NET )
    {
        num_layers = layer::get_inner_ip_lyr_index(p) + 1;

        // FIXIT-L:  is this an extraneous check?
        if (num_layers == 0)
            return -1;
    }
    else if ( f & ENC_FLAG_DEF )
    {
        /*
         * By its definitinos, this flag means 'stop before innermost ip4
         * opts or ip6 frag header'. So, stop after the ip4 layer IP4 will format itself, and now
         * we ensure that the ip6_frag header is not copied too.
         */

        if ( p->is_ip6() )
        {
            num_layers = layer::get_inner_ip6_frag_index(p);
        }
        else
        {
            num_layers = layer::get_inner_ip_lyr_index(p) + 1;
            update_ip4_len = true;
        }

        if (num_layers <= 0)
            return -1;
    }

    // copy raw packet data to clone
    Layer* lyr = (Layer*)p->layers + num_layers - 1;
    len = lyr->start - p->pkt + lyr->length;
    memcpy((void*)c->pkt, p->pkt, len);

    if ( update_ip4_len )
    {
        ip::IP4Hdr* ip4h = reinterpret_cast<ip::IP4Hdr*>(const_cast<uint8_t*>(lyr->start));
        lyr->length = ip::IP4_HEADER_LEN;
        ip4h->set_ip_len(ip::IP4_HEADER_LEN);
        ip4h->set_hlen(ip::IP4_HEADER_LEN >> 2);
    }

    const bool reverse = !(f & ENC_FLAG_FWD);

    // set up and format layers
    for ( i = 0; i < num_layers; i++ )
    {
        const uint8_t* b = c->pkt + (p->layers[i].start - p->pkt); // == c->pkt + p->layers[i].len
        lyr = c->layers + i;

        lyr->prot_id = p->layers[i].prot_id;
        lyr->length = p->layers[i].length;
        lyr->start = (uint8_t*)b;

        // NOTE: this must always go from outer to inner
        //       to ensure a valid ip header
        uint8_t mapped_prot = i ? CodecManager::s_proto_map[lyr->prot_id] : CodecManager::grinder;

        CodecManager::s_protocols[mapped_prot]->format(
            reverse, const_cast<uint8_t*>(lyr->start), c->ptrs);
    }

    // setup payload info
    c->num_layers = num_layers;
    c->data = lyr->start + lyr->length;
    len = c->data - c->pkt;

    // len < ETHERNET_HEADER_LEN + VLAN_HEADER + ETHERNET_MTU
    assert((unsigned)len < Codec::PKT_MAX - c->max_dsize);

    c->proto_bits = p->proto_bits;
    c->ip_proto_next = p->ip_proto_next;
    c->packet_flags |= PKT_PSEUDO;
    c->pseudo_type = type;
    c->user_policy_id = p->user_policy_id;  // cooked packet gets same policy as raw

    // setup pkt capture header
    pkth->caplen = len;
    pkth->pktlen = len;
    pkth->ts = p->pkth->ts;

    layer::set_packet_pointer(c);  // set layer pointer to ensure lookin at the new packet
    total_rebuilt_pkts++;  // update local counter
    return 0;
}

//-------------------------------------------------------------------------
// updaters:  these functions set length and checksum fields, only needed
// when a packet is modified.  some packets only have replacements so only
// the checksums need to be updated.  we always set the length rather than
// checking each time if needed.
//-------------------------------------------------------------------------

static inline void add_flag(UpdateFlags& flags,
    UpdateFlags flag_to_add,
    const Packet* const p,
    decltype(Packet::packet_flags)pkt_flag)  // future proofing.
{
    if ( p->packet_flags & pkt_flag )
        flags |= flag_to_add;
}

void PacketManager::encode_update(Packet* p)
{
    uint32_t len = p->dsize;

    UpdateFlags flags = 0;
    add_flag(flags, UPD_COOKED, p, PKT_PSEUDO);
    add_flag(flags, UPD_MODIFIED, p, PKT_MODIFIED);
    add_flag(flags, UPD_RESIZED, p, PKT_RESIZED);
    add_flag(flags, UPD_REBUILT_FRAG, p, PKT_REBUILT_FRAG);

    int8_t outer_layer = p->num_layers-1;
    int8_t inner_layer = p->num_layers-1;
    const Layer* const lyr = p->layers;
    ip::IpApi tmp_api;

    // update the rest of the ip layers with the correct IP reference.
    while (layer::set_inner_ip_api(p, tmp_api, inner_layer))
    {
        for (int i = outer_layer; i > inner_layer; --i)
        {
            const Layer& l = lyr[i];
            uint8_t mapped_prot = i ?
                CodecManager::s_proto_map[l.prot_id] : CodecManager::grinder;

            CodecManager::s_protocols[mapped_prot]->update(
                tmp_api, flags, const_cast<uint8_t*>(l.start), l.length, len);
        }
        outer_layer = inner_layer;
        // inner_layer is set in 'layer::set_inner_ip_api'
    }

    tmp_api.reset();
    for (int i = outer_layer; i >= 0; --i)
    {
        const Layer& l = lyr[i];
        uint8_t mapped_prot = CodecManager::s_proto_map[l.prot_id];
        CodecManager::s_protocols[mapped_prot]->update(
            tmp_api, flags, const_cast<uint8_t*>(l.start), l.length, len);
    }

    // see IP6_Update() for an explanation of this ...
    // FIXIT-L is this second statement really necessary?
    // PKT_RESIZED include PKT_MODIFIED ... so get rid of that extra flag
    if ( !(p->packet_flags & PKT_MODIFIED)
        || (p->packet_flags & (PKT_RESIZED & ~PKT_MODIFIED))
        )
    {
        DAQ_PktHdr_t* pkth = const_cast<DAQ_PktHdr_t*>(p->pkth);
        pkth->caplen = len;
        pkth->pktlen = len;
    }
}

//-------------------------------------------------------------------------
// codec support and statistics
//-------------------------------------------------------------------------

void PacketManager::encode_set_dst_mac(uint8_t* mac)
{ dst_mac = mac; }

uint8_t* PacketManager::encode_get_dst_mac()
{ return dst_mac; }

uint64_t PacketManager::get_rebuilt_packet_count(void)
{ return total_rebuilt_pkts; }

void PacketManager::encode_set_pkt(Packet* p)
{ encode_pkt = p; }

uint16_t PacketManager::encode_get_max_payload(const Packet* p)
{
    if ( !p->num_layers )
        return 0;

    const Layer& l = p->layers[p->num_layers - 1];
    return ETHERNET_MTU - (l.start - p->layers[0].start) - l.length;
}

void PacketManager::dump_stats()
{
    std::vector<const char*> pkt_names;

    // zero out the default codecs
    g_stats[3] = 0;
    g_stats[CodecManager::s_proto_map[FINISHED_DECODE] + stat_offset] = 0;

    for (unsigned int i = 0; i < stat_names.size(); i++)
        pkt_names.push_back(stat_names[i]);

    for (int i = 0; CodecManager::s_protocols[i] != 0; i++)
        pkt_names.push_back(CodecManager::s_protocols[i]->get_name());

    show_percent_stats((PegCount*)&g_stats, &pkt_names[0],
        (unsigned int)pkt_names.size(), "codec");
}

void PacketManager::accumulate()
{
    static std::mutex stats_mutex;

    std::lock_guard<std::mutex> lock(stats_mutex);
    sum_stats(&g_stats[0], &s_stats[0], s_stats.size());

    // mutex is automatically unlocked
}

const char* PacketManager::get_proto_name(uint16_t protocol)
{ return CodecManager::s_protocols[CodecManager::s_proto_map[protocol]]->get_name(); }

const char* PacketManager::get_proto_name(uint8_t protocol)
{ return CodecManager::s_protocols[CodecManager::s_proto_map[protocol]]->get_name(); }

void PacketManager::log_protocols(TextLog* const text_log,
    const Packet* const p)
{
    uint8_t num_layers = p->num_layers;
    const Layer* const lyr = p->layers;

    if (num_layers != 0)
    {
        // Grinder is not in the layer array
        Codec* cd = CodecManager::s_protocols[CodecManager::grinder];

        TextLog_Print(text_log, "%-.6s(DLT):  ", cd->get_name());
        cd->log(text_log, lyr[0].start, lyr[0].length);

        for (int i = 1; i < num_layers; i++)
        {
            const uint16_t protocol = lyr[i].prot_id;
            const uint8_t codec_offset =  CodecManager::s_proto_map[protocol];
            cd = CodecManager::s_protocols[codec_offset];

            TextLog_NewLine(text_log);
            TextLog_Print(text_log, "%-.*s", 6, cd->get_name());

            // don't print the type if this is a custom type.  Look
            // in protocol_ids.h for more details.
            if (protocol <= 0xFF)
                TextLog_Print(text_log, "(0x%02x)", protocol);
            else if (protocol >= eth::MIN_ETHERTYPE)
                TextLog_Print(text_log, "(0x%04x)", protocol);

            TextLog_Puts(text_log, ":  ");
            cd->log(text_log, lyr[i].start, lyr[i].length);
        }
    }
}

