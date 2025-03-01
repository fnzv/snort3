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
// ips_tag.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main/snort_types.h"
#include "main/snort_debug.h"
#include "protocols/packet.h"
#include "time/profiler.h"
#include "hash/sfhashfcn.h"
#include "detection/detection_defines.h"
#include "framework/ips_option.h"
#include "framework/parameter.h"
#include "framework/module.h"
#include "framework/range.h"
#include "protocols/tcp.h"

#define s_name "window"

static THREAD_LOCAL ProfileStats tcpWinPerfStats;

class TcpWinOption : public IpsOption
{
public:
    TcpWinOption(const RangeCheck& c) :
        IpsOption(s_name)
    { config = c; }

    uint32_t hash() const override;
    bool operator==(const IpsOption&) const override;

    int eval(Cursor&, Packet*) override;

private:
    RangeCheck config;
};

//-------------------------------------------------------------------------
// class methods
//-------------------------------------------------------------------------

uint32_t TcpWinOption::hash() const
{
    uint32_t a,b,c;

    a = config.op;
    b = config.min;
    c = config.max;

    mix_str(a,b,c,get_name());
    finalize(a,b,c);

    return c;
}

bool TcpWinOption::operator==(const IpsOption& ips) const
{
    if ( strcmp(get_name(), ips.get_name()) )
        return false;

    TcpWinOption& rhs = (TcpWinOption&)ips;
    return ( config == rhs.config );
}

int TcpWinOption::eval(Cursor&, Packet* p)
{
    PERF_PROFILE(tcpWinPerfStats);

    if (!p->ptrs.tcph)
        return DETECTION_OPTION_NO_MATCH;

    if ( config.eval(p->ptrs.tcph->th_win) )
        return DETECTION_OPTION_MATCH;

    return DETECTION_OPTION_NO_MATCH;
}

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter s_params[] =
{
    { "~range", Parameter::PT_STRING, nullptr, nullptr,
      "check if packet payload size is 'size | min<>max | <max | >min'" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define s_help \
    "rule option to check TCP window field"

class WindowModule : public Module
{
public:
    WindowModule() : Module(s_name, s_help, s_params) { }

    bool begin(const char*, int, SnortConfig*) override;
    bool set(const char*, Value&, SnortConfig*) override;

    ProfileStats* get_profile() const override
    { return &tcpWinPerfStats; }

    RangeCheck data;
};

bool WindowModule::begin(const char*, int, SnortConfig*)
{
    data.init();
    return true;
}

bool WindowModule::set(const char*, Value& v, SnortConfig*)
{
    if ( !v.is("~range") )
        return false;

    return data.parse(v.get_string());
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new WindowModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

static IpsOption* window_ctor(Module* p, OptTreeNode*)
{
    WindowModule* m = (WindowModule*)p;
    return new TcpWinOption(m->data);
}

static void window_dtor(IpsOption* p)
{
    delete p;
}

static const IpsApi window_api =
{
    {
        PT_IPS_OPTION,
        sizeof(IpsApi),
        IPSAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        s_name,
        s_help,
        mod_ctor,
        mod_dtor
    },
    OPT_TYPE_DETECTION,
    1, PROTO_BIT__TCP,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    window_ctor,
    window_dtor,
    nullptr
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &window_api.base,
    nullptr
};
#else
const BaseApi* ips_window = &window_api.base;
#endif

