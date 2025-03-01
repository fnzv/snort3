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

// modules.cc author Russ Combs <rucombs@cisco.com>

#include "modules.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <string.h>

#include <string>
using namespace std;

#include "framework/module.h"
#include "managers/module_manager.h"
#include "managers/plugin_manager.h"
#include "main.h"
#include "main/snort.h"
#include "main/snort_config.h"
#include "main/snort_module.h"
#include "main/thread.h"
#include "parser/parser.h"
#include "parser/parse_conf.h"
#include "parser/config_file.h"
#include "parser/cmd_line.h"
#include "parser/parse_ip.h"
#include "file_api/file_service.h"
#include "file_api/libs/file_config.h"
#include "filters/sfthd.h"
#include "filters/sfrf.h"
#include "filters/rate_filter.h"
#include "codecs/codec_module.h"
#include "time/ppm_module.h"
#include "time/profiler.h"
#include "target_based/sftarget_data.h"
#include "detection/fp_config.h"
#include "detection/signature.h"
#include "filters/detection_filter.h"
#include "filters/sfthreshold.h"
#include "sfip/sf_ip.h"
#include "stream/stream_api.h"
#include "utils/stats.h"
#include "target_based/snort_protocols.h"

//-------------------------------------------------------------------------
// detection module
//-------------------------------------------------------------------------

static const Parameter detection_params[] =
{
    { "asn1", Parameter::PT_INT, "1:", "256",
      "maximum decode nodes" },

    { "pcre_enable", Parameter::PT_BOOL, nullptr, "true",
      "disable pcre pattern matching" },

    { "pcre_match_limit", Parameter::PT_INT, "-1:1000000", "1500",
      "limit pcre backtracking, -1 = max, 0 = off" },

    { "pcre_match_limit_recursion", Parameter::PT_INT, "-1:10000", "1500",
      "limit pcre stack consumption, -1 = max, 0 = off" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define detection_help \
    "configure general IPS rule processing parameters"

class DetectionModule : public Module
{
public:
    DetectionModule() : Module("detection", detection_help, detection_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
    const PegInfo* get_pegs() const override { return pc_names; }
};

bool DetectionModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("asn1") )
        sc->asn1_mem = v.get_long();

    else if ( v.is("pcre_enable") )
        v.update_mask(sc->run_flags, RUN_FLAG__NO_PCRE, true);

    else if ( v.is("pcre_match_limit") )
        sc->pcre_match_limit = v.get_long();

    else if ( v.is("pcre_match_limit_recursion") )
        sc->pcre_match_limit_recursion = v.get_long();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// event queue module
//-------------------------------------------------------------------------

static const Parameter event_queue_params[] =
{
    { "max_queue", Parameter::PT_INT, "1:", "8",
      "maximum events to queue" },

    { "log", Parameter::PT_INT, "1:", "3",
      "maximum events to log" },

    { "order_events", Parameter::PT_ENUM,
      "priority|content_length", "content_length",
      "criteria for ordering incoming events" },

    { "process_all_events", Parameter::PT_BOOL, nullptr, "false",
      "process just first action group or all action groups" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define event_queue_help \
    "configure event queue parameters"

class EventQueueModule : public Module
{
public:
    EventQueueModule() : Module("event_queue", event_queue_help, event_queue_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool EventQueueModule::set(const char*, Value& v, SnortConfig* sc)
{
    EventQueueConfig* eq = sc->event_queue_config;

    if ( v.is("max_queue") )
        eq->max_events = v.get_long();

    else if ( v.is("log") )
        eq->log_events = v.get_long();

    else if ( v.is("order_events") )
    {
        if ( v.get_long() )
            eq->order = SNORT_EVENTQ_CONTENT_LEN;
        else
            eq->order = SNORT_EVENTQ_PRIORITY;
    }
    else if ( v.is("process_all_events") )
        eq->process_all_events = v.get_bool();

    else
        return false;

    if ( eq->max_events < eq->log_events )
        eq->max_events = eq->log_events;

    return true;
}

//-------------------------------------------------------------------------
// search engine module
//-------------------------------------------------------------------------

static const char* get_search_methods()
{ return PluginManager::get_available_plugins(PT_SEARCH_ENGINE); }

static const Parameter search_engine_params[] =
{
    { "bleedover_port_limit", Parameter::PT_INT, "1:", "1024",
      "maximum ports in rule before demotion to any-any port group" },

    { "bleedover_warnings_enabled", Parameter::PT_BOOL, nullptr, "false",
      "print warning if a rule is demoted to any-any port group" },

    { "enable_single_rule_group", Parameter::PT_BOOL, nullptr, "false",
      "put all rules into one group" },

    { "debug", Parameter::PT_BOOL, nullptr, "false",
      "print verbose fast pattern info" },

    { "debug_print_nocontent_rule_tests", Parameter::PT_BOOL, nullptr, "false",
      "print rule group info during packet evaluation" },

    { "debug_print_rule_group_build_details", Parameter::PT_BOOL, nullptr, "false",
      "print rule group info during compilation" },

    { "debug_print_rule_groups_uncompiled", Parameter::PT_BOOL, nullptr, "false",
      "prints uncompiled rule group information" },

    { "debug_print_rule_groups_compiled", Parameter::PT_BOOL, nullptr, "false",
      "prints compiled rule group information" },

    { "debug_print_fast_pattern", Parameter::PT_BOOL, nullptr, "false",
      "print fast pattern info for each rule" },

    { "max_pattern_len", Parameter::PT_INT, "0:", "0",
      "truncate patterns when compiling into state machine (0 means no maximum)" },

    { "max_queue_events", Parameter::PT_INT, nullptr, "5",
      "maximum number of matching fast pattern states to queue per packet" },

    { "inspect_stream_inserts", Parameter::PT_BOOL, nullptr, "false",
      "inspect reassembled payload - disabling is good for performance, bad for detection" },

    { "search_method", Parameter::PT_DYNAMIC, (void*)get_search_methods, "ac_bnfa_q",
      "set fast pattern algorithm - choose available search engine" },

    { "split_any_any", Parameter::PT_BOOL, nullptr, "false",
      "evaluate any-any rules separately to save memory" },

    { "search_optimize", Parameter::PT_BOOL, nullptr, "false",
      "tweak state machine construction for better performance" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define search_engine_help \
    "configure fast pattern matcher"

class SearchEngineModule : public Module
{
public:
    SearchEngineModule() : Module("search_engine", search_engine_help, search_engine_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool SearchEngineModule::set(const char*, Value& v, SnortConfig* sc)
{
    FastPatternConfig* fp = sc->fast_pattern_config;

    if ( v.is("bleedover_port_limit") )
        fp->set_bleed_over_port_limit(v.get_long());

    else if ( v.is("bleedover_warnings_enabled") )
    {
        if ( v.get_bool() )
            fp->set_bleed_over_warnings();  // FIXIT-L these should take arg
    }
    else if ( v.is("enable_single_rule_group") )
    {
        if ( v.get_bool() )
            fp->set_single_rule_group();
    }
    else if ( v.is("debug") )
    {
        if ( v.get_bool() )
            fp->set_debug_mode();
    }
    else if ( v.is("debug_print_nocontent_rule_tests") )
    {
        if ( v.get_bool() )
            fp->set_debug_print_nc_rules();
    }
    else if ( v.is("debug_print_rule_group_build_details") )
    {
        if ( v.get_bool() )
            fp->set_debug_print_rule_group_build_details();
    }
    else if ( v.is("debug_print_rule_groups_uncompiled") )
    {
        if ( v.get_bool() )
            fp->set_debug_print_rule_groups_uncompiled();
    }
    else if ( v.is("debug_print_rule_groups_compiled") )
    {
        if ( v.get_bool() )
            fp->set_debug_print_rule_groups_compiled();
    }
    else if ( v.is("debug_print_fast_pattern") )
        fp->set_debug_print_fast_patterns(v.get_bool());

    else if ( v.is("max_pattern_len") )
        fp->set_max_pattern_len(v.get_long());

    else if ( v.is("max_queue_events") )
        fp->set_max_queue_events(v.get_long());

    else if ( v.is("inspect_stream_inserts") )
        fp->set_stream_insert(v.get_bool());

    else if ( v.is("search_method") )
    {
        if ( fp->set_detect_search_method(v.get_string()) )
            return false;
    }
    else if ( v.is("split_any_any") )
        fp->set_split_any_any(v.get_long());

    else if ( v.is("search_optimize") )
        fp->set_search_opt(v.get_long());

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// profile module
//-------------------------------------------------------------------------

#ifdef PERF_PROFILING
static const Parameter profile_rule_params[] =
{
    { "count", Parameter::PT_INT, "-1:", "-1",
      "print results to given level (-1 = all, 0 = off)" },

    { "sort", Parameter::PT_ENUM,
      "checks | avg_ticks | total_ticks | matches | no_matches | "
      "avg_ticks_per_match | avg_ticks_per_no_match",
      "avg_ticks", "sort by given field" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter profile_module_params[] =
{
    { "count", Parameter::PT_INT, "-1:", "-1",
      "print results to given level (-1 = all, 0 = off)" },

    { "sort", Parameter::PT_ENUM,
      "checks | avg_ticks | total_ticks", "avg_ticks",
      "sort by given field" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter profile_params[] =
{
    { "rules", Parameter::PT_TABLE, profile_rule_params, nullptr,
      "" },

    { "modules", Parameter::PT_TABLE, profile_module_params, nullptr,
      "" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define profile_help \
    "configure profiling of rules and/or modules (requires --enable-perf-profiling)"

class ProfileModule : public Module
{
public:
    ProfileModule() : Module("profile", profile_help, profile_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
};

bool ProfileModule::begin(const char* fqn, int, SnortConfig* sc)
{
    if ( !strcmp(fqn, "profile.rules") )
        sc->profile_rules->count = -1;

    else if ( !strcmp(fqn, "profile.modules") )
        sc->profile_modules->count = -1;

    return true;
}

bool ProfileModule::set(const char* fqn, Value& v, SnortConfig* sc)
{
    ProfileConfig* p;
    const char* spr = "profile.rules";
    const char* spp = "profile.modules";

    if ( !strncmp(fqn, spr, strlen(spr)) )
        p = sc->profile_rules;

    else if ( !strncmp(fqn, spp, strlen(spp)) )
        p = sc->profile_modules;

    else
        return false;

    if ( v.is("count") )
        p->count = v.get_long();

    else if ( v.is("sort") )
        p->sort = static_cast<ProfileSort>(v.get_long() + 1);

    else
        return false;

    return true;
}
#endif

//-------------------------------------------------------------------------
// classification module
//-------------------------------------------------------------------------
// FIXIT-L signature.{h,cc} has type and name confused
// the keys here make more sense

#define classifications_help \
    "define rule categories with priority"

static const Parameter classification_params[] =
{
    { "name", Parameter::PT_STRING, nullptr, nullptr,
      "name used with classtype rule option" },

    { "priority", Parameter::PT_INT, "0:", "1",
      "default priority for class" },

    { "text", Parameter::PT_STRING, nullptr, nullptr,
      "description of class" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class ClassificationsModule : public Module
{
public:
    ClassificationsModule() :
        Module("classifications", classifications_help, classification_params, true) { }

    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    string name;
    string text;
    int priority;
};

bool ClassificationsModule::begin(const char*, int, SnortConfig*)
{
    name.erase();
    text.erase();
    priority = 1;
    return true;
}

bool ClassificationsModule::end(const char*, int idx, SnortConfig* sc)
{
    if ( idx )
        AddClassification(sc, name.c_str(), text.c_str(), priority);
    return true;
}

bool ClassificationsModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("name") )
        name = v.get_string();

    else if ( v.is("priority") )
        priority = v.get_long();

    else if ( v.is("text") )
        text = v.get_string();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// reference module
//-------------------------------------------------------------------------

#define reference_help \
    "define reference systems used in rules"

static const Parameter reference_params[] =
{
    { "name", Parameter::PT_STRING, nullptr, nullptr,
      "name used with reference rule option" },

    { "url", Parameter::PT_STRING, nullptr, nullptr,
      "where this reference is defined" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class ReferencesModule : public Module
{
public:
    ReferencesModule() :
        Module("references", reference_help, reference_params, true) { }

    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    string name;
    string url;
};

bool ReferencesModule::begin(const char*, int, SnortConfig*)
{
    name.erase();
    url.erase();
    return true;
}

bool ReferencesModule::end(const char*, int idx, SnortConfig* sc)
{
    if ( idx )
        ReferenceSystemAdd(sc, name.c_str(), url.c_str());
    return true;
}

bool ReferencesModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("name") )
        name = v.get_string();

    else if ( v.is("url") )
        url = v.get_string();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// alerts module
//-------------------------------------------------------------------------

static const Parameter alerts_params[] =
{
    // FIXIT-L move to fast, full, syslog and delete from here
    { "alert_with_interface_name", Parameter::PT_BOOL, nullptr, "false",
      "include interface in alert info (fast, full, or syslog only)" },

    { "default_rule_state", Parameter::PT_BOOL, nullptr, "true",
      "enable or disable ips rules" },

    { "detection_filter_memcap", Parameter::PT_INT, "0:", "1048576",
      "set available memory for filters" },

    { "event_filter_memcap", Parameter::PT_INT, "0:", "1048576",
      "set available memory for filters" },

    { "order", Parameter::PT_STRING, nullptr, "pass drop alert log",
      "change the order of rule action application" },

    { "rate_filter_memcap", Parameter::PT_INT, "0:", "1048576",
      "set available memory for filters" },

    { "reference_net", Parameter::PT_STRING, nullptr, nullptr,
      "set the CIDR for homenet "
      "(for use with -l or -B, does NOT change $HOME_NET in IDS mode)" },

    { "stateful", Parameter::PT_BOOL, nullptr, "false",
      "don't alert w/o established session (note: rule action still taken)" },

    { "tunnel_verdicts", Parameter::PT_STRING, nullptr, nullptr,
      "let DAQ handle non-allow verdicts for GTP|Teredo|6in4|4in6 traffic" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define alerts_help \
    "configure alerts"

class AlertsModule : public Module
{
public:
    AlertsModule() : Module("alerts", alerts_help, alerts_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool AlertsModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("alert_with_interface_name") )
        v.update_mask(sc->output_flags, OUTPUT_FLAG__ALERT_IFACE);

    else if ( v.is("default_rule_state") )
        sc->default_rule_state = v.get_bool();

    else if ( v.is("detection_filter_memcap") )
        sc->detection_filter_config->memcap = v.get_long();

    else if ( v.is("event_filter_memcap") )
        sc->threshold_config->memcap = v.get_long();

    else if ( v.is("order") )
        OrderRuleLists(sc, v.get_string());

    else if ( v.is("rate_filter_memcap") )
        sc->rate_filter_config->memcap = v.get_long();

    else if ( v.is("reference_net") )
        return ( sfip_pton(v.get_string(), &sc->homenet) == SFIP_SUCCESS );

    else if ( v.is("stateful") )
        v.update_mask(sc->run_flags, RUN_FLAG__ASSURE_EST);

    else if ( v.is("tunnel_verdicts") )
        ConfigTunnelVerdicts(sc, v.get_string());

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// output module
//-------------------------------------------------------------------------

static const Parameter output_event_trace_params[] =
{
    { "max_data", Parameter::PT_INT, "0:65535", "0",
      "maximum amount of packet data to capture" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter output_params[] =
{
    { "dump_chars_only", Parameter::PT_BOOL, nullptr, "false",
      "turns on character dumps (same as -C)" },

    { "dump_payload", Parameter::PT_BOOL, nullptr, "false",
      "dumps application layer (same as -d)" },

    { "dump_payload_verbose", Parameter::PT_BOOL, nullptr, "false",
      "dumps raw packet starting at link layer (same as -X)" },

    { "log_ipv6_extra_data", Parameter::PT_BOOL, nullptr, "false",
      "log IPv6 source and destination addresses as unified2 extra data records" },

    { "event_trace", Parameter::PT_TABLE, output_event_trace_params, nullptr,
      "" },

    { "quiet", Parameter::PT_BOOL, nullptr, "false",
      "suppress non-fatal information (still show alerts, same as -q)" },

    { "logdir", Parameter::PT_STRING, nullptr, ".",
      "where to put log files (same as -l)" },

    { "obfuscate", Parameter::PT_BOOL, nullptr, "false",
      "obfuscate the logged IP addresses (same as -O)" },

    { "show_year", Parameter::PT_BOOL, nullptr, "false",
      "include year in timestamp in the alert and log files (same as -y)" },

    { "tagged_packet_limit", Parameter::PT_INT, "0:", "256",
      "maximum number of packets tagged for non-packet metrics" },

    { "verbose", Parameter::PT_BOOL, nullptr, "false",
      "be verbose (same as -v)" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define output_help \
    "configure general output parameters"

class OutputModule : public Module
{
public:
    OutputModule() : Module("output", output_help, output_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool OutputModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("dump_chars_only") )
        v.update_mask(sc->output_flags, OUTPUT_FLAG__CHAR_DATA);

    else if ( v.is("dump_payload") )
        v.update_mask(sc->output_flags, OUTPUT_FLAG__APP_DATA);

    else if ( v.is("dump_payload_verbose") )
        v.update_mask(sc->output_flags, OUTPUT_FLAG__VERBOSE_DUMP);

    else if ( v.is("log_ipv6_extra_data") )
        // FIXIT-M move to output|logging_flags
        sc->log_ipv6_extra = v.get_bool() ? 0 : 1;

    else if ( v.is("quiet") )
        v.update_mask(sc->logging_flags, LOGGING_FLAG__QUIET);

    else if ( v.is("logdir") )
        sc->log_dir = v.get_string();

    else if ( v.is("max_data") )
        sc->event_trace_max = v.get_long();

    else if ( v.is("obfuscate") )
        v.update_mask(sc->output_flags, OUTPUT_FLAG__OBFUSCATE);

    else if ( v.is("show_year") )
        v.update_mask(sc->output_flags, OUTPUT_FLAG__INCLUDE_YEAR);

    else if ( v.is("tagged_packet_limit") )
        sc->tagged_packet_limit = v.get_long();

    else if ( v.is("verbose") )
        v.update_mask(sc->logging_flags, LOGGING_FLAG__VERBOSE);

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// active module
//-------------------------------------------------------------------------

static const Parameter active_params[] =
{
    { "attempts", Parameter::PT_INT, "0:20", "0",
      "number of TCP packets sent per response (with varying sequence numbers)" },

    { "device", Parameter::PT_STRING, nullptr, nullptr,
      "use 'ip' for network layer responses or 'eth0' etc for link layer" },

    { "dst_mac", Parameter::PT_STRING, nullptr, nullptr,
      "use format '01:23:45:67:89:ab'" },

    { "max_responses", Parameter::PT_INT, "0:", "0",
      "maximum number of responses" },

    { "min_interval", Parameter::PT_INT, "1:", "255",
      "minimum number of seconds between responses" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define active_help \
    "configure responses"

class ActiveModule : public Module
{
public:
    ActiveModule() : Module("active", active_help, active_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool ActiveModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("attempts") )
        sc->respond_attempts = v.get_long();

    else if ( v.is("device") )
        sc->respond_device = v.get_string();

    else if ( v.is("dst_mac") )
        ConfigDstMac(sc, v.get_string());

    else if ( v.is("max_responses") )
        sc->max_responses = v.get_long();

    else if ( v.is("min_interval") )
        sc->min_interval = v.get_long();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// packets module
//-------------------------------------------------------------------------

static const Parameter packets_params[] =
{
    { "address_space_agnostic", Parameter::PT_BOOL, nullptr, "false",
      "determines whether DAQ address space info is used to track fragments and connections" },

    { "bpf_file", Parameter::PT_STRING, nullptr, nullptr,
      "file with BPF to select traffic for Snort" },

    { "enable_inline_init_failopen", Parameter::PT_BOOL, nullptr, "true",
      "whether to pass traffic during later stage of initialization to avoid drops" },

    { "limit", Parameter::PT_INT, "0:", "0",
      "maximum number of packets to process before stopping (0 is unlimited)" },

    { "skip", Parameter::PT_INT, "0:", "0",
      "number of packets to skip before before processing" },

    { "vlan_agnostic", Parameter::PT_BOOL, nullptr, "false",
      "determines whether VLAN info is used to track fragments and connections" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define packets_help \
    "configure basic packet handling"

class PacketsModule : public Module
{
public:
    PacketsModule() : Module("packets", packets_help, packets_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool PacketsModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("address_space_agnostic") )
        sc->addressspace_agnostic = v.get_long();

    else if ( v.is("bpf_file") )
        sc->bpf_file = v.get_string();

    else if ( v.is("enable_inline_init_failopen") )
        v.update_mask(sc->run_flags, RUN_FLAG__DISABLE_FAILOPEN, true);

    else if ( v.is("limit") )
        sc->pkt_cnt = v.get_long();

    else if ( v.is("skip") )
        sc->pkt_skip = v.get_long();

    else if ( v.is("vlan_agnostic") )
        sc->vlan_agnostic = v.get_long();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// daq module
//-------------------------------------------------------------------------

static const Parameter daq_params[] =
{
    // FIXIT-L should be a list?
    { "dir", Parameter::PT_STRING, nullptr, nullptr,
      "directory where to search for DAQ plugins" },

    { "mode", Parameter::PT_SELECT, "passive | inline | read-file", nullptr,
      "set mode of operation" },

    { "no_promisc", Parameter::PT_BOOL, nullptr, "false",
      "whether to put DAQ device into promiscuous mode" },

    // FIXIT-L no default; causes
    // ERROR: setting DAQ to pcap but pcap already selected.
    { "type", Parameter::PT_STRING, nullptr, nullptr,
      "select type of DAQ" },

    // FIXIT-L should be a list?
    { "vars", Parameter::PT_STRING, nullptr, nullptr,
      "comma separated list of name=value DAQ-specific parameters" },

    { "snaplen", Parameter::PT_INT, "0:65535", "deflt",
      "set snap length (same as -P)" },

    { "decode_data_link", Parameter::PT_BOOL, nullptr, "false",
      "display the second layer header info" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define daq_help \
    "configure packet acquisition interface"

class DaqModule : public Module
{
public:
    DaqModule() : Module("daq", daq_help, daq_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
    const PegInfo* get_pegs() const override { return daq_names; }
};

bool DaqModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("dir") )
        ConfigDaqDir(sc, v.get_string());

    else if ( v.is("mode") )
        ConfigDaqMode(sc, v.get_string());

    else if ( v.is("no_promisc") )
        v.update_mask(sc->run_flags, RUN_FLAG__NO_PROMISCUOUS);

    else if ( v.is("type") )
        ConfigDaqType(sc, v.get_string());

    else if ( v.is("vars") )
    {
        string tok;
        v.set_first_token();

        while ( v.get_next_csv_token(tok) )
            ConfigDaqVar(sc, tok.c_str());
    }
    else if ( v.is("decode_data_link") )
    {
        if ( v.get_bool() )
            ConfigDecodeDataLink(sc, "");
    }
    else if ( v.is("snaplen") )
        sc->pkt_snaplen = v.get_long();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// attribute_table module
//-------------------------------------------------------------------------

static const Parameter attribute_table_params[] =
{
    { "max_hosts", Parameter::PT_INT, "32:207551", "1024",
      "maximum number of hosts in attribute table" },

    { "max_services_per_host", Parameter::PT_INT, "1:65535", "8",
      "maximum number of services per host entry in attribute table" },

    { "max_metadata_services", Parameter::PT_INT, "1:256", "8",
      "maximum number of services in rule metadata" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

const char* attribute_table_help =
    "configure hosts loading";

class AttributeTableModule : public Module
{
public:
    AttributeTableModule() :
        Module("attribute_table", attribute_table_help, attribute_table_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool AttributeTableModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("max_hosts") )
        sc->max_attribute_hosts = v.get_long();

    else if ( v.is("max_services_per_host") )
        sc->max_attribute_services_per_host = v.get_long();

    else if ( v.is("max_metadata_services") )
        sc->max_metadata_services = v.get_long();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// network module
//-------------------------------------------------------------------------

static const Parameter network_params[] =
{
    { "checksum_drop", Parameter::PT_MULTI,
      "all | ip | noip | tcp | notcp | udp | noudp | icmp | noicmp | none", "none",
      "drop if checksum is bad" },

    { "checksum_eval", Parameter::PT_MULTI,
      "all | ip | noip | tcp | notcp | udp | noudp | icmp | noicmp | none", "none",
      "checksums to verify" },

    { "decode_drops", Parameter::PT_BOOL, nullptr, "false",
      "enable dropping of packets by the decoder" },

    { "id", Parameter::PT_INT, "0:65535", "0",
      "correlate unified2 events with configuration" },

    { "min_ttl", Parameter::PT_INT, "1:255", "1",
      "alert / normalize packets with lower ttl / hop limit "
      "(you must enable rules and / or normalization also)" },

    { "new_ttl", Parameter::PT_INT, "1:255", "1",
      "use this value for responses and when normalizing" },

    { "layers", Parameter::PT_INT, "3:255", "40",
      "The maximum number of protocols that Snort can correctly decode" },

    { "max_ip6_extensions", Parameter::PT_INT, "0:255", "0",
      "The number of IP6 options Snort will process for a given IPv6 layer. "
      "If this limit is hit, rule 116:456 may fire.  0 = unlimited" },

    { "max_ip_layers", Parameter::PT_INT, "0:255", "0",
      "The maximum number of IP layers Snort will process for a given packet "
      "If this limit is hit, rule 116:293 may fire.  0 = unlimited" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define network_help \
    "configure basic network parameters"

class NetworkModule : public Module
{
public:
    NetworkModule() : Module("network", network_help, network_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool NetworkModule::set(const char*, Value& v, SnortConfig* sc)
{
    NetworkPolicy* p = get_network_policy();

    if ( v.is("checksum_drop") )
        ConfigChecksumDrop(sc, v.get_string());

    else if ( v.is("checksum_eval") )
        ConfigChecksumMode(sc, v.get_string());

    else if ( v.is("decode_drops") )
        p->decoder_drop = v.get_bool();

    else if ( v.is("id") )
        p->user_policy_id = v.get_long();

    else if ( v.is("min_ttl") )
        p->min_ttl = (uint8_t)v.get_long();

    else if ( v.is("new_ttl") )
        p->new_ttl = (uint8_t)v.get_long();

    else if (v.is("layers"))
        sc->num_layers = (uint8_t)v.get_long();

    else if (v.is("max_ip6_extensions"))
        sc->max_ip6_extensions = (uint8_t)v.get_long();

    else if (v.is("max_ip_layers"))
        sc->max_ip_layers = (uint8_t)v.get_long();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// detection policy module
//-------------------------------------------------------------------------

static const Parameter ips_params[] =
{
    { "enable_builtin_rules", Parameter::PT_BOOL, nullptr, "false",
      "enable events from builtin rules w/o stubs" },

    { "id", Parameter::PT_INT, "0:65535", "0",
      "correlate unified2 events with configuration" },

    { "include", Parameter::PT_STRING, nullptr, nullptr,
      "legacy snort rules and includes" },

    // FIXIT-L no default; it breaks initialization by -Q
    { "mode", Parameter::PT_ENUM, "tap | inline | inline-test", nullptr,
      "set policy mode" },

    { "rules", Parameter::PT_STRING, nullptr, nullptr,
      "snort rules and includes" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define ips_help \
    "configure IPS rule processing"

class IpsModule : public Module
{
public:
    IpsModule() : Module("ips", ips_help, ips_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
};

bool IpsModule::set(const char*, Value& v, SnortConfig*)
{
    IpsPolicy* p = get_ips_policy();

    if ( v.is("enable_builtin_rules") )
        p->enable_builtin_rules = v.get_bool();

    else if ( v.is("id") )
        p->user_policy_id = v.get_long();

    else if ( v.is("include") )
        p->include = v.get_string();

    else if ( v.is("mode") )
        p->policy_mode = (PolicyMode)v.get_long();

    else if ( v.is("rules") )
        p->rules = v.get_string();

    else
        return false;

    return true;
}

//-------------------------------------------------------------------------
// process module
//-------------------------------------------------------------------------

static const Parameter thread_pinning_params[] =
{
    { "cpu", Parameter::PT_INT, "0:127", "0",
      "pin the associated source/thread to this cpu" },

    { "source", Parameter::PT_STRING, nullptr, nullptr,
      "set cpu affinity for this source (either pcap or <iface>" },

    { "thread", Parameter::PT_INT, "0:", "0",
      "set cpu affinity for the <cur_thread_num> thread that runs" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter process_params[] =
{
    { "chroot", Parameter::PT_STRING, nullptr, nullptr,
      "set chroot directory (same as -t)" },

    { "threads", Parameter::PT_LIST, thread_pinning_params, nullptr,
      "thread pinning parameters" },

    { "daemon", Parameter::PT_BOOL, nullptr, "false",
      "fork as a daemon (same as -D)" },

    { "dirty_pig", Parameter::PT_BOOL, nullptr, "false",
      "shutdown without internal cleanup" },

    { "set_gid", Parameter::PT_STRING, nullptr, nullptr,
      "set group ID (same as -g)" },

    { "set_uid", Parameter::PT_STRING, nullptr, nullptr,
      "set user ID (same as -u)" },

    { "umask", Parameter::PT_STRING, nullptr, nullptr,
      "set process umask (same as -m)" },

    { "utc", Parameter::PT_BOOL, nullptr, "false",
      "use UTC instead of local time for timestamps" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define process_help \
    "configure basic process setup"

class ProcessModule : public Module
{
public:
    ProcessModule() : Module("process", process_help, process_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    std::string source;
    int thread;
    int cpu;
};

bool ProcessModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("daemon") )
    {
        if ( v.get_bool() )
            ConfigDaemon(sc, "");
    }
    else if ( v.is("chroot") )
        ConfigChrootDir(sc, v.get_string());

    else if ( v.is("dirty_pig") )
    {
        if ( v.get_bool() )
            ConfigDirtyPig(sc, "");
    }
    else if ( v.is("set_gid") )
        ConfigSetGid(sc, v.get_string());

    else if ( v.is("set_uid") )
        ConfigSetUid(sc, v.get_string());

    else if ( v.is("umask") )
        ConfigUmask(sc, v.get_string());

    else if ( v.is("utc") )
    {
        if ( v.get_bool() )
            ConfigUtc(sc, "");
    }
    else if (v.is("cpu"))
        cpu = v.get_long();

    else if (v.is("source"))
        source = v.get_string();

    else if (v.is("thread"))
        thread = v.get_long();

    else
        return false;

    return true;
}

bool ProcessModule::begin(const char*, int, SnortConfig*)
{
    source.clear();
    thread = -1;
    cpu = -1;
    return true;
}

bool ProcessModule::end(const char* fqn, int idx, SnortConfig* sc)
{
    if ( !idx )
        return true;

    if (!strcmp(fqn, "process.threads"))
    {
        if (cpu == -1)
        {
            ParseError("%s - cpu(%d) for thread (%d) and source (%s) "
                "must be an integer in the range of 0 < cpu < max_cpus", fqn, cpu);
            return false;
        }
        else if ((source.empty()) && (thread == -1))
        {
            ParseError("%s - must have either a source or a thread", fqn);
            return false;
        }
        else if ((!source.empty()) && (thread >= 0))
        {
            ParseError("%s - cannot set both thread(%d) and source(%s)",
                fqn, thread, source.c_str());
            return false;
        }
        else if (!source.empty())
            set_cpu_affinity(sc, source, cpu);

        else
            set_cpu_affinity(sc, thread, cpu);
    }

    return true;
}

//-------------------------------------------------------------------------
// file_id module
//-------------------------------------------------------------------------

static const Parameter file_magic_params[] =
{
    { "content", Parameter::PT_STRING, nullptr, nullptr,
      "file magic content" },

    { "offset", Parameter::PT_INT, "0:", "0",
      "file magic offset" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter file_rule_params[] =
{
    { "rev", Parameter::PT_INT, "0:", "0",
      "rule revision" },

    { "msg", Parameter::PT_STRING, nullptr, nullptr,
      "information about the file type" },

    { "type", Parameter::PT_STRING, nullptr, nullptr,
      "file type name" },

    { "id", Parameter::PT_INT, "0:", "0",
      "file type id" },

    { "category", Parameter::PT_STRING, nullptr, nullptr,
      "file type category" },

    { "version", Parameter::PT_STRING, nullptr, nullptr,
      "file type version" },

    { "magic", Parameter::PT_LIST, file_magic_params, nullptr,
        "list of file magic rules" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter file_id_params[] =
{
    { "type_depth", Parameter::PT_INT, "0:", "1460",
      "stop type ID at this point" },

    { "signature_depth", Parameter::PT_INT, "0:", "10485760",
      "stop signature at this point" },

    { "block_timeout", Parameter::PT_INT, "0:", "86400",
      "stop blocking after this many seconds" },

    { "lookup_timeout", Parameter::PT_INT, "0:", "2",
      "give up on lookup after this many seconds" },

    { "block_timeout_lookup", Parameter::PT_BOOL, nullptr, "false",
      "block if lookup times out" },

    { "enable_type", Parameter::PT_BOOL, nullptr, "false",
      "enable type ID" },

    { "enable_signature", Parameter::PT_BOOL, nullptr, "false",
      "enable signature calculation" },

    { "enable_capture", Parameter::PT_BOOL, nullptr, "false",
      "enable file capture" },

    { "show_data_depth", Parameter::PT_INT, "0:", "100",
      "print this many octets" },

    { "file_rules", Parameter::PT_LIST, file_rule_params, nullptr,
        "list of file magic rules" },

    { "trace_type", Parameter::PT_BOOL, nullptr, "false",
      "enable runtime dump of type info" },

    { "trace_signature", Parameter::PT_BOOL, nullptr, "false",
      "enable runtime dump of signature info" },

    { "trace_stream", Parameter::PT_BOOL, nullptr, "false",
      "enable runtime dump of file data" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define file_id_help \
    "configure file identification"

class FileIdModule : public Module
{
public:
    FileIdModule() : Module("file_id", file_id_help, file_id_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;
private:
    FileMagicRule rule;
    FileMagicData magic;
};

bool FileIdModule::set(const char*, Value& v, SnortConfig* sc)
{
    FileConfig* fc = sc->file_config;
    assert(fc);

    if ( v.is("type_depth") )
        fc->file_type_depth = v.get_long();

    else if ( v.is("signature_depth") )
        fc->file_signature_depth = v.get_long();

    else if ( v.is("block_timeout") )
        fc->file_block_timeout = v.get_long();

    else if ( v.is("lookup_timeout") )
        fc->file_lookup_timeout = v.get_long();

    else if ( v.is("block_timeout_lookup") )
        fc->block_timeout_lookup = v.get_bool();

    else if ( v.is("enable_type") )
    {
        if ( v.get_bool() )
            FileService::enable_file_type();
    }
    else if ( v.is("enable_signature") )
    {
        if ( v.get_bool() )
            FileService::enable_file_signature();
    }
    else if ( v.is("enable_capture") )
    {
        if ( v.get_bool() )
            FileService::enable_file_capture();
    }
    else if ( v.is("show_data_depth") )
        FileConfig::show_data_depth = v.get_long();

    else if ( v.is("trace_type") )
        FileConfig::trace_type = v.get_bool();

    else if ( v.is("trace_signature") )
        FileConfig::trace_signature = v.get_bool();

    else if ( v.is("trace_stream") )
        FileConfig::trace_stream = v.get_bool();

    else if ( v.is("file_rules") )
        return true;

    else if ( v.is("rev") )
        rule.rev = v.get_long();

    else if ( v.is("msg") )
        rule.message = v.get_string();

    else if ( v.is("type") )
        rule.type = v.get_string();

    else if ( v.is("id") )
        rule.id = v.get_long();

    else if ( v.is("category") )
        rule.category = v.get_string();

    else if ( v.is("version") )
        rule.version = v.get_string();

    else if ( v.is("magic") )
        return true;

    else if ( v.is("content") )
        magic.content_str = v.get_string();

    else if ( v.is("offset") )
        magic.offset = v.get_long();

    else
        return false;

    return true;
}

bool FileIdModule::begin(const char* fqn, int idx, SnortConfig* sc)
{
    FileConfig* fc = sc->file_config;

    if (!fc)
    {
        fc = new FileConfig;
        sc->file_config = fc;
    }

    if (!idx)
        return true;

    if ( !strcmp(fqn, "file_id.file_rules") )
    {
        rule.clear();
    }

    else if ( !strcmp(fqn, "file_id.file_rules.magic") )
    {
        magic.clear();
    }

    return true;
}

bool FileIdModule::end(const char* fqn, int idx, SnortConfig* sc)
{
    FileConfig* fc = sc->file_config;

    if (!idx)
        return true;

    if ( !strcmp(fqn, "file_id.file_rules") )
    {
        fc->process_file_rule(rule);
        //fc->print_file_rule(rule);
    }

    else if ( !strcmp(fqn, "file_id.file_rules.magic") )
    {
        fc->process_file_magic(magic);
        rule.file_magics.push_back(magic);
    }

    return true;
}
//-------------------------------------------------------------------------
// suppress module
//-------------------------------------------------------------------------

static const Parameter suppress_params[] =
{
    { "gid", Parameter::PT_INT, "0:", "0",
      "rule generator ID" },

    { "sid", Parameter::PT_INT, "0:", "0",
      "rule signature ID" },

    { "track", Parameter::PT_ENUM, "by_src | by_dst", nullptr,
      "suppress only matching source or destination addresses" },

    { "ip", Parameter::PT_STRING, nullptr, nullptr,
      "restrict suppression to these addresses according to track" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define suppress_help \
    "configure event suppressions"

class SuppressModule : public Module
{
public:
    SuppressModule() : Module("suppress", suppress_help, suppress_params, true) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    THDX_STRUCT thdx;
};

bool SuppressModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("gid") )
        thdx.gen_id = v.get_long();

    else if ( v.is("sid") )
        thdx.sig_id = v.get_long();

    else if ( v.is("track") )
        thdx.tracking = v.get_long() + 1;

    else if ( v.is("ip") )
        thdx.ip_address = sfip_var_from_string(v.get_string());

    else
        return false;

    return true;
}

bool SuppressModule::begin(const char*, int, SnortConfig*)
{
    memset(&thdx, 0, sizeof(thdx));
    thdx.type = THD_TYPE_SUPPRESS;
    thdx.priority = THD_PRIORITY_SUPPRESS;
    thdx.tracking = THD_TRK_NONE;
    return true;
}

bool SuppressModule::end(const char*, int idx, SnortConfig* sc)
{
    if ( idx && sfthreshold_create(sc, sc->threshold_config, &thdx) )
    {
        ParseError("bad suppress configuration [%d]", idx);
        return false;
    }
    return true;
}

//-------------------------------------------------------------------------
// event_filter module
//-------------------------------------------------------------------------

static const Parameter event_filter_params[] =
{
    { "gid", Parameter::PT_INT, "0:", "1",
      "rule generator ID" },

    { "sid", Parameter::PT_INT, "0:", "1",
      "rule signature ID" },

    { "type", Parameter::PT_ENUM, "limit | threshold | both", nullptr,
      "1st count events | every count events | once after count events" },

    { "track", Parameter::PT_ENUM, "by_src | by_dst", nullptr,
      "filter only matching source or destination addresses" },

    { "count", Parameter::PT_INT, "-1:", "0",
      "number of events in interval before tripping; -1 to disable" },

    { "seconds", Parameter::PT_INT, "0:", "0",
      "count interval" },

    { "ip", Parameter::PT_STRING, nullptr, nullptr,
      "restrict filter to these addresses according to track" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define event_filter_help \
    "configure thresholding of events"

class EventFilterModule : public Module
{
public:
    EventFilterModule() :
        Module("event_filter", event_filter_help, event_filter_params, true) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    THDX_STRUCT thdx;
};

bool EventFilterModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("gid") )
        thdx.gen_id = v.get_long();

    else if ( v.is("sid") )
        thdx.sig_id = v.get_long();

    else if ( v.is("track") )
        thdx.tracking = v.get_long() + 1;

    else if ( v.is("ip") )
        thdx.ip_address = sfip_var_from_string(v.get_string());

    else if ( v.is("count") )
        thdx.count = v.get_long();

    else if ( v.is("seconds") )
        thdx.seconds = v.get_long();

    else if ( v.is("type") )
        thdx.type = v.get_long();

    else
        return false;

    return true;
}

bool EventFilterModule::begin(const char*, int, SnortConfig*)
{
    memset(&thdx, 0, sizeof(thdx));
    thdx.priority = THD_PRIORITY_SUPPRESS;
    thdx.tracking = THD_TRK_NONE;
    return true;
}

bool EventFilterModule::end(const char*, int idx, SnortConfig* sc)
{
    if ( idx && sfthreshold_create(sc, sc->threshold_config, &thdx) )
    {
        ParseError("bad event_filter configuration [%d]", idx);
        return false;
    }
    return true;
}

//-------------------------------------------------------------------------
// rate_filter module
//-------------------------------------------------------------------------

static const Parameter rate_filter_params[] =
{
    { "gid", Parameter::PT_INT, "0:", "1",
      "rule generator ID" },

    { "sid", Parameter::PT_INT, "0:", "1",
      "rule signature ID" },

    { "track", Parameter::PT_ENUM, "by_src | by_dst | by_rule", "by_src",
      "filter only matching source or destination addresses" },

    { "count", Parameter::PT_INT, "0:", "1",
      "number of events in interval before tripping" },

    { "seconds", Parameter::PT_INT, "0:", "1",
      "count interval" },

    { "new_action", Parameter::PT_SELECT,
      // FIXIT-L this list should be defined globally
      "alert | drop | log | pass | | reject | sdrop", "alert",
      "take this action on future hits until timeout" },

    { "timeout", Parameter::PT_INT, "0:", "1",
      "count interval" },

    { "apply_to", Parameter::PT_STRING, nullptr, nullptr,
      "restrict filter to these addresses according to track" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define rate_filter_help \
    "configure rate filters (which change rule actions)"

class RateFilterModule : public Module
{
public:
    RateFilterModule() : Module("rate_filter", rate_filter_help, rate_filter_params, true) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    tSFRFConfigNode thdx;
};

bool RateFilterModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("gid") )
        thdx.gid = v.get_long();

    else if ( v.is("sid") )
        thdx.sid = v.get_long();

    else if ( v.is("track") )
        thdx.tracking = (SFRF_TRACK)(v.get_long() + 1);

    else if ( v.is("count") )
        thdx.count = v.get_long();

    else if ( v.is("seconds") )
        thdx.seconds = v.get_long();

    else if ( v.is("timeout") )
        thdx.timeout = v.get_long();

    else if ( v.is("apply_to") )
        thdx.applyTo = sfip_var_from_string(v.get_string());

    else if ( v.is("new_action") )
        thdx.newAction = (RuleType)(v.get_long() + 1);

    else
        return false;

    return true;
}

bool RateFilterModule::begin(const char*, int, SnortConfig*)
{
    memset(&thdx, 0, sizeof(thdx));
    return true;
}

bool RateFilterModule::end(const char*, int idx, SnortConfig* sc)
{
    if ( idx && RateFilter_Create(sc, sc->rate_filter_config,  &thdx) )
    {
        ParseError("bad rate_filter configuration [%d]", idx);
        return false;
    }
    return true;
}

//-------------------------------------------------------------------------
// rule_state module
//-------------------------------------------------------------------------

static const Parameter rule_state_params[] =
{
    { "gid", Parameter::PT_INT, "0:", "0",
      "rule generator ID" },

    { "sid", Parameter::PT_INT, "0:", "0",
      "rule signature ID" },

    { "enable", Parameter::PT_BOOL, nullptr, "true",
      "enable or disable rule in all policies" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define rule_state_help \
    "enable/disable specific IPS rules"

class RuleStateModule : public Module
{
public:
    RuleStateModule() : Module("rule_state", rule_state_help, rule_state_params) { }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    RuleState state;
};

bool RuleStateModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("gid") )
        state.gid = v.get_long();

    else if ( v.is("sid") )
        state.sid = v.get_long();

    else if ( v.is("enable") )
        state.state = v.get_bool();

    else
        return false;

    return true;
}

bool RuleStateModule::begin(const char*, int, SnortConfig*)
{
    memset(&state, 0, sizeof(state));
    return true;
}

bool RuleStateModule::end(const char*, int idx, SnortConfig* sc)
{
    if ( idx )
        AddRuleState(sc, state);
    return true;
}

//-------------------------------------------------------------------------
// hosts module
//-------------------------------------------------------------------------

static const Parameter service_params[] =
{
    { "name", Parameter::PT_STRING, nullptr, nullptr,
      "service identifier" },

    { "proto", Parameter::PT_ENUM, "tcp | udp", "tcp",
      "ip protocol" },

    { "port", Parameter::PT_PORT, nullptr, nullptr,
      "port number" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

static const Parameter hosts_params[] =
{
    { "ip", Parameter::PT_ADDR, nullptr, "0.0.0.0/32",
      "hosts address / cidr" },

    { "frag_policy", Parameter::PT_ENUM, IP_POLICIES, nullptr,
      "defragmentation policy" },

    { "tcp_policy", Parameter::PT_ENUM, TCP_POLICIES, nullptr,
      "tcp reassembly policy" },

    { "services", Parameter::PT_LIST, service_params, nullptr,
      "list of service parameters" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define hosts_help \
    "configure hosts"

class HostsModule : public Module
{
public:
    HostsModule() : Module("hosts", hosts_help, hosts_params, true)
    { app = nullptr; host = nullptr; }
    ~HostsModule() { assert(!host && !app); }

    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

private:
    ApplicationEntry* app;
    HostAttributeEntry* host;
};

bool HostsModule::set(const char*, Value& v, SnortConfig*)
{
    if ( host and v.is("ip") )
        v.get_addr(host->ipAddr);

    else if ( host and v.is("frag_policy") )
        host->hostInfo.fragPolicy = v.get_long() + 1;

    else if ( host and v.is("tcp_policy") )
        host->hostInfo.streamPolicy = v.get_long() + 1;

    else if ( app and v.is("name") )
        app->protocol = AddProtocolReference(v.get_string());

    else if ( app and v.is("proto") )
        app->ipproto = AddProtocolReference(v.get_string());

    else if ( app and v.is("port") )
        app->port = v.get_long();

    else
        return false;

    return true;
}

bool HostsModule::begin(const char* fqn, int idx, SnortConfig*)
{
    if ( idx && !strcmp(fqn, "hosts.services") )
        app = SFAT_CreateApplicationEntry();

    else if ( idx && !strcmp(fqn, "hosts") )
        host = SFAT_CreateHostEntry();

    return true;
}

bool HostsModule::end(const char* fqn, int idx, SnortConfig*)
{
    if ( idx && !strcmp(fqn, "hosts.services") )
    {
        SFAT_AddService(host, app);
        app = nullptr;
    }
    else if ( idx && !strcmp(fqn, "hosts") )
    {
        SFAT_AddHost(host);
        host = nullptr;
    }

    return true;
}

#if 0
//-------------------------------------------------------------------------
// xxx module - used as copy/paste template
//-------------------------------------------------------------------------

static const RuleMap xxx_rules[] =
{
    { SID, "STR" },
    { 0, 0, nullptr }
};

static const Parameter xxx_params[] =
{
    { "name", Parameter::PT_INT, "range", "deflt",
      "help" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

#define xxx_help \
    "configure "

class XXXModule : public Module
{
public:
    XXXModule() : Module("xxx", xxx_help, xxx_params) { }
    const RuleMap* get_rules() { return xxx_rules; }
    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;
};

bool XXXModule::set(const char*, Value& v, SnortConfig* sc)
{
    if ( v.is("name") )
        sc->pkt_cnt = v.get_long();

    else
        return false;

    return true;
}

bool XXXModule::begin(const char*, int, SnortConfig*)
{
    return true;
}

bool XXXModule::end(const char*, int, SnortConfig*)
{
    return true;
}

#endif

//-------------------------------------------------------------------------
// module manager stuff - move to framework/module_manager.cc
//-------------------------------------------------------------------------

void module_init()
{
    // parameters must be settable regardless of sequence
    // since Lua calls this by table hash key traversal
    // (which is effectively random)
    // so module interdependencies must come after this phase
    ModuleManager::add_module(get_snort_module());

    // these modules are not policy specific
    ModuleManager::add_module(new ClassificationsModule);
    ModuleManager::add_module(new DaqModule);
    ModuleManager::add_module(new DetectionModule);
    ModuleManager::add_module(new PacketsModule);
    ModuleManager::add_module(new ProcessModule);
#ifdef PERF_PROFILING
    ModuleManager::add_module(new ProfileModule);
#endif
    ModuleManager::add_module(new ReferencesModule);
    ModuleManager::add_module(new RuleStateModule);
    ModuleManager::add_module(new SearchEngineModule);
    ModuleManager::add_module(new CodecModule);

    // these could but prolly shouldn't be policy specific
    // or should be broken into policy and non-policy parts
    ModuleManager::add_module(new AlertsModule);
    ModuleManager::add_module(new EventQueueModule);
    ModuleManager::add_module(new OutputModule);

    // these modules could be in traffic policy
    ModuleManager::add_module(new ActiveModule);
    ModuleManager::add_module(new FileIdModule);

#ifdef PPM_MGR
    ModuleManager::add_module(new PpmModule);
#endif

    // these modules should be in ips policy
    ModuleManager::add_module(new EventFilterModule);
    ModuleManager::add_module(new RateFilterModule);
    ModuleManager::add_module(new SuppressModule);

    // these are preliminary policies
    ModuleManager::add_module(new NetworkModule);
    ModuleManager::add_module(new IpsModule);

    // these modules replace config and hosts.xml
    ModuleManager::add_module(new AttributeTableModule);
    ModuleManager::add_module(new HostsModule);
}

