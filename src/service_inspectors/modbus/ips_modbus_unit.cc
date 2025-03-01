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

// ips_modbus_unit.cc author Russ Combs <rucombs@cisco.com>

#include "main/snort_types.h"
#include "main/snort_debug.h"
#include "detection/detection_defines.h"
#include "framework/ips_option.h"
#include "framework/module.h"
#include "hash/sfhashfcn.h"
#include "protocols/packet.h"
#include "time/profiler.h"

#include "modbus.h"

static const char* s_name = "modbus_unit";

//-------------------------------------------------------------------------
// version option
//-------------------------------------------------------------------------

static THREAD_LOCAL ProfileStats modbus_unit_prof;

class ModbusUnitOption : public IpsOption
{
public:
    ModbusUnitOption(uint8_t u) : IpsOption(s_name)
    { unit = u; }

    uint32_t hash() const override;
    bool operator==(const IpsOption&) const override;

    int eval(Cursor&, Packet*) override;

public:
    uint8_t unit;
};

uint32_t ModbusUnitOption::hash() const
{
    uint32_t a = unit, b = 0, c = 0;

    mix_str(a, b, c, get_name());
    finalize(a,b,c);

    return c;
}

bool ModbusUnitOption::operator==(const IpsOption& ips) const
{
    if ( strcmp(get_name(), ips.get_name()) )
        return false;

    ModbusUnitOption& rhs = (ModbusUnitOption&)ips;
    return ( unit == rhs.unit );
}

int ModbusUnitOption::eval(Cursor&, Packet* p)
{
    PERF_PROFILE(modbus_unit_prof);

    if ( !p->flow )
        return DETECTION_OPTION_NO_MATCH;

    if ( !p->is_full_pdu() )
        return DETECTION_OPTION_NO_MATCH;

    ModbusFlowData* mfd = 
        (ModbusFlowData*)p->flow->get_application_data(ModbusFlowData::flow_id);

    if ( mfd and unit == mfd->ssn_data.unit )
        return DETECTION_OPTION_MATCH;

    return DETECTION_OPTION_NO_MATCH;
}

//-------------------------------------------------------------------------
// module
//-------------------------------------------------------------------------

static const Parameter s_params[] =
{
    { "~", Parameter::PT_INT, "0:255", nullptr,
      "modbus unit ID" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define s_help \
    "rule option to check modbus unit ID"

class ModbusUnitModule : public Module
{
public:
    ModbusUnitModule() : Module(s_name, s_help, s_params) { }

    bool set(const char*, Value&, SnortConfig*) override;

    ProfileStats* get_profile() const override
    { return &modbus_unit_prof; }

public:
    uint8_t unit;
};

bool ModbusUnitModule::set(const char*, Value& v, SnortConfig*)
{
    if ( !v.is("~") )
        return false;

    unit = v.get_long();
    return true;
}

//-------------------------------------------------------------------------
// api
//-------------------------------------------------------------------------

static Module* mod_ctor()
{
    return new ModbusUnitModule;
}

static void mod_dtor(Module* m)
{
    delete m;
}

static IpsOption* opt_ctor(Module* m, OptTreeNode*)
{
    ModbusUnitModule* mod = (ModbusUnitModule*)m;
    return new ModbusUnitOption(mod->unit);
}

static void opt_dtor(IpsOption* p)
{
    delete p;
}

static const IpsApi ips_api =
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
    0, PROTO_BIT__TCP,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    opt_ctor,
    opt_dtor,
    nullptr
};

const BaseApi* ips_modbus_unit = &ips_api.base;

