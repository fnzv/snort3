//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2011-2013 Sourcefire, Inc.
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

// gtp_parser.h author Hui Cao <hcao@sourcefire.com>

#ifndef GTP_PARSER_H
#define GTP_PARSER_H

#include "main/snort_types.h"

struct GTP_IEData
{
    uint16_t length;
    uint16_t shift;  /*shift relative to the header*/
    uint32_t msg_id;  /* used to associate to current msg */
};

struct GTPMsg
{
    uint8_t version;
    uint8_t msg_type;
    uint16_t msg_length;
    uint16_t header_len;
    uint8_t* gtp_header;
    GTP_IEData* info_elements;

    /* nothing after this point is zeroed ...*/
    uint32_t msg_id; /*internal state, new msg will have a new id*/
};

int gtp_parse(struct GTPMsg*, const uint8_t*, uint16_t);
void gtp_cleanInfoElements();

#endif

