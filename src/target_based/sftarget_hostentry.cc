//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2006-2013 Sourcefire, Inc.
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

// sftarget_hostentry.c author Steven Sturges

#include "sftarget_hostentry.h"

#if 0
static bool hasService(const HostAttributeEntry* host_entry,
    int ipprotocol, int protocol, int application)
{
    ApplicationEntry* service;

    if (!host_entry)
        return false;

    for (service = host_entry->services; service; service = service->next)
    {
        if (ipprotocol && (service->ipproto == ipprotocol))
        {
            if (protocol && (service->protocol == protocol))
            {
                if (!application)
                {
                    /* match of ipproto, proto.  application not speicifed */
                    return true;
                }
            }
            else if (!protocol)
            {
                /* match of ipproto.  protocol not speicifed */
                return true;
            }
        }
        /* No ipprotocol specified, huh? */
    }

    return false;
}

static bool hasClient(const HostAttributeEntry* host_entry,
    int ipprotocol, int protocol, int application)
{
    ApplicationEntry* client;

    if (!host_entry)
        return false;

    for (client = host_entry->clients; client; client = client->next)
    {
        if (ipprotocol && (client->ipproto == ipprotocol))
        {
            if (protocol && (client->protocol == protocol))
            {
                if (!application)
                {
                    /* match of ipproto, proto.  application not speicifed */
                    return true;
                }
            }
            else if (!protocol)
            {
                /* match of ipproto.  protocol not speicifed */
                return true;
            }
        }
        /* No ipprotocol specified, huh? */
    }

    return false;
}

bool hasProtocol(const HostAttributeEntry* host_entry,
    int ipprotocol, int protocol, int application)
{
    if ( hasService(host_entry, ipprotocol, protocol, application) )
        return true;

    if ( hasClient(host_entry, ipprotocol, protocol, application) )
        return true;

    return false;
}
#endif

int getApplicationProtocolId(const HostAttributeEntry* host_entry,
    int ipprotocol,
    uint16_t port,
    char direction)
{
    ApplicationEntry* application;

    if (!host_entry)
        return 0;

    if (direction == SFAT_SERVICE)
    {
        for (application = host_entry->services; application; application = application->next)
        {
            if (application->ipproto == ipprotocol)
            {
                if ((uint16_t)application->port == port)
                {
                    return application->protocol;
                }
            }
        }
    }

    /* FIXIT: client? doesn't make much sense in terms of specific port */

    return 0;
}

