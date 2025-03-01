//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2002-2013 Sourcefire, Inc.
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
*   An abstracted interface to the Multi-Pattern Matching routines,
*   thats why we're passing 'void *' objects around.
*
*   Marc A Norton <mnorton@sourcefire.com>
*
*   Updates:
*   3/06 - Added AC_BNFA search
*/

#include "bnfa_search.h"

#include "main/snort_debug.h"
#include "main/snort_types.h"
#include "main/snort_config.h"
#include "utils/util.h"
#include "time/profiler.h"
#include "framework/mpse.h"

//-------------------------------------------------------------------------
// "ac_bnfa"
//-------------------------------------------------------------------------

class AcBnfaMpse : public Mpse
{
private:
    bnfa_struct_t* obj;

public:
    AcBnfaMpse(
        SnortConfig*,
        bool use_gc,
        void (* user_free)(void*),
        void (* tree_free)(void**),
        void (* list_free)(void**))
        : Mpse("ac_bnfa", use_gc)
    {
        obj=bnfaNew(user_free, tree_free, list_free);
        if ( obj ) obj->bnfaMethod = 1;
    }

    ~AcBnfaMpse()
    {
        if (obj)
            bnfaFree(obj);
    }

    void set_opt(int flag) override
    {
        if (obj)
            bnfaSetOpt(obj, flag);
    }

    int add_pattern(
        SnortConfig*, const uint8_t* P, unsigned m,
        bool noCase, bool negative, void* ID, int) override
    {
        return bnfaAddPattern(obj, P, m, noCase, negative, ID);
    }

    int prep_patterns(
        SnortConfig* sc, MpseBuild build_tree, MpseNegate neg_list) override
    {
        return bnfaCompile(sc, obj, build_tree, neg_list);
    }

    int _search(
        const uint8_t* T, int n, MpseMatch match,
        void* data, int* current_state) override
    {
        /* return is actually the state */
        return _bnfa_search_csparse_nfa(
            obj, T, n, match, data, 0 /* start-state */, current_state);
    }

    int print_info() override
    {
        bnfaPrintInfo(obj);
        return 0;
    }

    int get_pattern_count() override
    {
        return bnfaPatternCount(obj);
    }
};

//-------------------------------------------------------------------------
// api
//-------------------------------------------------------------------------

static Mpse* bnfa_ctor(
    SnortConfig* sc,
    class Module*,
    bool use_gc,
    void (* user_free)(void*),
    void (* tree_free)(void**),
    void (* list_free)(void**))
{
    return new AcBnfaMpse(sc, use_gc, user_free, tree_free, list_free);
}

static void bnfa_dtor(Mpse* p)
{
    delete p;
}

static void bnfa_init()
{
    bnfa_init_xlatcase();
    bnfaInitSummary();
}

static void bnfa_print()
{
    bnfaPrintSummary();
}

static const MpseApi bnfa_api =
{
    {
        PT_SEARCH_ENGINE,
        sizeof(MpseApi),
        SEAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        "ac_bnfa",
        "Aho-Corasick Binary NFA (low memory, high performance) MPSE",
        nullptr,
        nullptr
    },
    false,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    bnfa_ctor,
    bnfa_dtor,
    bnfa_init,
    bnfa_print,
};

const BaseApi* se_ac_bnfa = &bnfa_api.base;

