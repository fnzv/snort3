//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2013-2013 Sourcefire, Inc.
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

#include "loggers.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "framework/logger.h"

// to ensure PacketManager::log_protocols() is built into Snort++
extern const BaseApi* log_codecs;

#ifdef LINUX
extern const BaseApi* alert_sf_socket;
#endif

#ifdef STATIC_LOGGERS
extern const BaseApi* alert_csv;
extern const BaseApi* alert_fast;
extern const BaseApi* alert_full;
extern const BaseApi* alert_syslog;
extern const BaseApi* log_hext;
extern const BaseApi* log_pcap;
extern const BaseApi* eh_unified2;
#endif

const BaseApi* loggers[] =
{
    // loggers
    log_codecs,

#ifdef LINUX
    alert_sf_socket,
#endif

#ifdef STATIC_LOGGERS
    // alerters
    alert_csv,
    alert_fast,
    alert_full,
    alert_syslog,

    // loggers
    log_hext,
    log_pcap,

    // both
    eh_unified2,
#endif

    nullptr
};

