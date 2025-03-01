// main.cc author Russ Combs <rucombs@cisco.com>
// unit test main

#include "stream/tcp/tcp_module.h"
#include "stream/tcp/tcp_normalizers.h"
#include "protocols/tcp_options.h"
#include "stream/tcp/tcp_defs.h"

#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>
#include <CppUTestExt/MockSupport.h>

NormMode mockNormMode = NORM_MODE_ON;
bool norm_enabled = true;
THREAD_LOCAL SFBASE sfBase;
THREAD_LOCAL TcpStats tcpStats;
THREAD_LOCAL void *snort_conf = nullptr;

Flow::Flow( void ) {}
Flow::~Flow( void ) {}

class FlowMock : public Flow
{
public:

};

TcpSession::TcpSession( Flow* ) : Session( flow ) { }
TcpSession::~TcpSession( void ) { }
bool TcpSession::setup(Packet*){ return true; }
void TcpSession::update_direction(char, sfip_t const*, unsigned short){ }
int TcpSession::process(Packet*){ return 0; }
void TcpSession::restart(Packet*){ }
void TcpSession::clear(){ }
void TcpSession::cleanup(){ }
bool TcpSession::add_alert(Packet*, unsigned int, unsigned int){ return true; }
bool TcpSession::check_alerted(Packet*, unsigned int, unsigned int){ return true; }
int TcpSession::update_alert(Packet*, unsigned int, unsigned int, unsigned int, unsigned int){ return 0; }
void TcpSession::flush_client(Packet*){ }
void TcpSession::flush_server(Packet*){ }
void TcpSession::flush_talker(Packet*){ }
void TcpSession::flush_listener(Packet*){ }
void TcpSession::set_splitter(bool, StreamSplitter*){ }
StreamSplitter* TcpSession::get_splitter(bool){ return nullptr; }
void TcpSession::set_extra_data(Packet*, unsigned int){ }
void TcpSession::clear_extra_data(Packet*, unsigned int){ }
bool TcpSession::is_sequenced(unsigned char){ return true; }
bool TcpSession::are_packets_missing(unsigned char){ return false; }
uint8_t TcpSession::get_reassembly_direction(){ return 0; }
uint8_t  TcpSession::missing_in_reassembled(unsigned char){ return 0; }

class TcpSessionMock : public TcpSession
{
public:
    TcpSessionMock( Flow* flow ) : TcpSession( flow ) { }
    ~TcpSessionMock( void ) { }

    TcpTracker client;
    TcpTracker server;
};

class Active
{
public:
    static void drop_packet(const Packet*, bool force = false);

};

void Active::drop_packet(const Packet* , bool ) { }

bool Normalize_IsEnabled(NormFlags )
{
    return norm_enabled;
}

NormMode Normalize_GetMode(NormFlags )
{
    if( norm_enabled )
        return mockNormMode;
    else
        return NORM_MODE_OFF;
}

TEST_GROUP(tcp_normalizers)
{
    //Flow* flow = nullptr;
    //TcpSession* session = nullptr;

    void setup()
    {
    }

    void teardown()
    {
    }
};

TEST(tcp_normalizers, os_policy)
{
    StreamPolicy os_policy;
    Flow* flow = new FlowMock;
    TcpSession* session = new TcpSessionMock( flow );

    for( os_policy = StreamPolicy::OS_FIRST; os_policy <= StreamPolicy::OS_PROXY; ++os_policy )
    {
        TcpNormalizer* normalizer = TcpNormalizerFactory::create( os_policy, session,
            &session->client, &session->server );
        CHECK( normalizer->get_os_policy() == os_policy );

        delete normalizer;
    }

    delete flow;
    delete session;
}

TEST(tcp_normalizers, paws_fudge_config)
{
    StreamPolicy os_policy;
    Flow* flow = new FlowMock;
    TcpSession* session = new TcpSessionMock( flow );

    for( os_policy = StreamPolicy::OS_FIRST; os_policy <= StreamPolicy::OS_PROXY; ++os_policy )
    {
        TcpNormalizer* normalizer = TcpNormalizerFactory::create( os_policy, session,
            &session->client, &session->server );

        switch ( os_policy )
        {
        case StreamPolicy::OS_LINUX:
            CHECK( normalizer->get_paws_ts_fudge() == 1 );
            break;

        default:
            CHECK( normalizer->get_paws_ts_fudge() == 0 );
            break;
        }

        delete normalizer;
    }

    delete flow;
    delete session;
}

TEST(tcp_normalizers, paws_drop_zero_ts_config)
{
    StreamPolicy os_policy;
    Flow* flow = new FlowMock;
    TcpSession* session = new TcpSessionMock( flow );

    for( os_policy = StreamPolicy::OS_FIRST; os_policy <= StreamPolicy::OS_PROXY; ++os_policy )
    {
        TcpNormalizer* normalizer = TcpNormalizerFactory::create( os_policy, session,
            &session->client, &session->server );

        switch ( os_policy )
        {
        case StreamPolicy::OS_OLD_LINUX:
        case StreamPolicy::OS_SOLARIS:
        case StreamPolicy::OS_WINDOWS:
        case StreamPolicy::OS_WINDOWS2K3:
        case StreamPolicy::OS_VISTA:
            CHECK( !normalizer->is_paws_drop_zero_ts() );
            break;

        default:
            CHECK( normalizer->is_paws_drop_zero_ts() );
            break;
        }

        delete normalizer;
    }

    delete flow;
    delete session;
}

TEST(tcp_normalizers, norm_options_enabled)
{
    StreamPolicy os_policy;
    Flow* flow = new FlowMock;
    TcpSession* session = new TcpSessionMock( flow );

    norm_enabled = true;
    for( os_policy = StreamPolicy::OS_FIRST; os_policy <= StreamPolicy::OS_PROXY; ++os_policy )
    {
        TcpNormalizer* normalizer = TcpNormalizerFactory::create( os_policy, session,
            &session->client, &session->server );

        CHECK( normalizer->get_opt_block() == NORM_MODE_ON );
        CHECK( normalizer->get_strip_ecn() == NORM_MODE_ON );
        CHECK( normalizer->get_tcp_block() == NORM_MODE_ON );
        CHECK( normalizer->get_trim_syn() == NORM_MODE_ON );
        CHECK( normalizer->get_trim_rst() == NORM_MODE_ON );
        CHECK( normalizer->get_trim_mss() == NORM_MODE_ON );
        CHECK( normalizer->get_trim_win() == NORM_MODE_ON );
        CHECK( normalizer->is_tcp_ips_enabled() );

        delete normalizer;
    }

    norm_enabled = false;
    for( os_policy = StreamPolicy::OS_FIRST; os_policy <= StreamPolicy::OS_PROXY; ++os_policy )
    {
        TcpNormalizer* normalizer = TcpNormalizerFactory::create( os_policy, session,
            &session->client, &session->server );

        CHECK( normalizer->get_opt_block() == NORM_MODE_OFF );
        CHECK( normalizer->get_strip_ecn() == NORM_MODE_OFF );
        CHECK( normalizer->get_tcp_block() == NORM_MODE_OFF );
        CHECK( normalizer->get_trim_syn() == NORM_MODE_OFF );
        CHECK( normalizer->get_trim_rst() == NORM_MODE_OFF );
        CHECK( normalizer->get_trim_mss() == NORM_MODE_OFF );
        CHECK( normalizer->get_trim_win() == NORM_MODE_OFF );
        CHECK( !normalizer->is_tcp_ips_enabled() );
        delete normalizer;
    }

    delete flow;
    delete session;
}

int main(int argc, char** argv)
{
    //MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();
    return CommandLineTestRunner::RunAllTests(argc, argv);
}

