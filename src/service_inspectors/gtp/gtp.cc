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

// gtp.cc author Hui Cao <hcao@sourcefire.com>
// This is the main entry point for this preprocessor

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gtp.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>

#ifndef WIN32
#include <strings.h>
#include <sys/time.h>
#endif

#include "main/snort_debug.h"
#include "main/snort_types.h"
#include "protocols/packet.h"
#include "time/profiler.h"

#include "gtp.h"
#include "gtp_inspect.h"
#include "gtp_parser.h"

#define GTPMSG_ZERO_LEN offsetof(GTPMsg, msg_id)

THREAD_LOCAL GTP_Stats gtp_stats;

// Analyzes GTP packets for anomalies/exploits.
static inline int GTP_Process(Packet* p, GTP_Roptions* pRopts)
{
    const uint8_t* gtp_buff =  p->data;
    static THREAD_LOCAL uint32_t msgId = 0;

    GTPMsg gtpMsg;
    memset(&gtpMsg, 0, GTPMSG_ZERO_LEN);

    /* msg_id is used to associate message with information elements
     * If msg_id matches, the information element in the info_elements
     * belongs to the message
     * Using msg_id avoids initializing info_elements for every message
     * Tabled based info_elements improves information element search performance */

    /* To avoid id overlap, clean table when msgId resets*/
    if ( msgId == 0)
        gtp_cleanInfoElements();

    gtpMsg.msg_id = ++msgId;

    int status = gtp_parse(&gtpMsg, gtp_buff, p->dsize);

    /*Update the session data*/
    pRopts->gtp_type = gtpMsg.msg_type;
    pRopts->gtp_version = gtpMsg.version;
    pRopts->gtp_infoElements = gtpMsg.info_elements;
    pRopts->gtp_header = gtpMsg.gtp_header;
    pRopts->msg_id = gtpMsg.msg_id;

    DEBUG_WRAP(DebugFormat(DEBUG_GTP, "GTP message version: %d\n", gtpMsg.version));
    DEBUG_WRAP(DebugFormat(DEBUG_GTP, "GTP message type: %d\n", gtpMsg.msg_type));

    return status;
}

static GTP_Roptions* GTPGetNewSession(Packet* packetp)
{
    GtpFlowData* gfd = new GtpFlowData;
    packetp->flow->set_application_data(gfd);

    GTP_Roptions* pRopts = &gfd->ropts;
    gtp_stats.sessions++;

    return pRopts;
}

// Main runtime entry point for GTP preprocessor.
void GTPmain(Packet* packetp)
{
    /* Attempt to get a previously allocated GTP block. */
    GtpFlowData* gfd = (GtpFlowData*)packetp->flow->get_application_data(GtpFlowData::flow_id);
    GTP_Roptions* pRopts = gfd ? &gfd->ropts : nullptr;

    if ( !pRopts )
    {
        pRopts = GTPGetNewSession(packetp);

        if ( !pRopts )
        {
            /* Could not get/create the session data for this packet. */
            DEBUG_WRAP(DebugMessage(DEBUG_GTP, "Create session error - not inspecting.\n"));
            return;
        }
    }

    GTP_Process(packetp, pRopts);
}

