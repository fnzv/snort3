//--------------------------------------------------------------------------
// Copyright (C) 2015-2015 Cisco and/or its affiliates. All rights reserved.
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

// tcp_reassemblers.cc author davis mcpherson <davmcphe@@cisco.com>
// Created on: Oct 9, 2015

#include "tcp_module.h"
#include "tcp_events.h"
#include "tcp_session.h"
#include "tcp_reassemblers.h"

class TcpReassemblerFirst : public TcpReassembler
{
public:
    TcpReassemblerFirst( TcpSession* session, TcpTracker* tracker, bool server ) :
        TcpReassembler( session, tracker, StreamPolicy::OS_FIRST, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_new( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os5( );
    }
};

class TcpReassemblerLast : public TcpReassembler
{
public:
    TcpReassemblerLast( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_LAST, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_last( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os4( );
    }
};

class TcpReassemblerLinux : public TcpReassembler
{
public:
    TcpReassemblerLinux( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_LINUX, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os2( );
    }
};

class TcpReassemblerOldLinux : public TcpReassembler
{
public:
    TcpReassemblerOldLinux( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_OLD_LINUX, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os4( );
    }
};

class TcpReassemblerBSD : public TcpReassembler
{
public:
    TcpReassemblerBSD( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_BSD, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os1( );
    }
};

class TcpReassemblerMacOS : public TcpReassembler
{
public:
    TcpReassemblerMacOS( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_MACOS, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os1( );
    }
};

class TcpReassemblerSolaris : public TcpReassembler
{
public:
    TcpReassemblerSolaris( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_SOLARIS, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_trim_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_new( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os3( );
    }
};

class TcpReassemblerIrix : public TcpReassembler
{
public:
    TcpReassemblerIrix( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_IRIX, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os2( );
    }

};

class TcpReassemblerHpux11 : public TcpReassembler
{
public:
    TcpReassemblerHpux11( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_HPUX11, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_trim_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_new( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os3( );
    }
};

class TcpReassemblerHpux10 : public TcpReassembler
{
public:
    TcpReassemblerHpux10( TcpSession* session, TcpTracker* tracker, bool server ) :
        TcpReassembler( session, tracker, StreamPolicy::OS_HPUX10, server )
    {  }

private:
    int insert_left_overlap(void) override
            {
        return left_overlap_keep_first( );
            }

    void insert_right_overlap(void) override
            {
        right_overlap_truncate_existing( );
            }

    int insert_full_overlap(void) override
            {
        return full_right_overlap_os2( );
            }
};

class TcpReassemblerWindows : public TcpReassembler
{
public:
    TcpReassemblerWindows( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_WINDOWS, server )
    {  }

private:
    int insert_left_overlap(void) override
     {
         return left_overlap_keep_first( );
     }

     void insert_right_overlap(void) override
     {
         right_overlap_truncate_existing( );
     }

     int insert_full_overlap(void) override
     {
         return full_right_overlap_os1( );
     }
};

class TcpReassemblerWindows2K3 : public TcpReassembler
{
public:
    TcpReassemblerWindows2K3( TcpSession* session, TcpTracker* tracker, bool server) :
            TcpReassembler( session, tracker, StreamPolicy::OS_WINDOWS2K3, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_existing( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os1( );
    }
};

class TcpReassemblerVista : public TcpReassembler
{
public:
    TcpReassemblerVista( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassembler( session, tracker, StreamPolicy::OS_VISTA, server )
    {  }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_new( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os5 ( );
    }
};

class TcpReassemblerProxy : public TcpReassemblerFirst
{
public:
    TcpReassemblerProxy( TcpSession* session, TcpTracker* tracker, bool server ) :
            TcpReassemblerFirst( session, tracker, server )
    {
        tcp_ips_data = NORM_MODE_OFF;
    }

private:
    int insert_left_overlap(void) override
    {
        return left_overlap_keep_first( );
    }

    void insert_right_overlap(void) override
    {
        right_overlap_truncate_new( );
    }

    int insert_full_overlap(void) override
    {
        return full_right_overlap_os5( );
    }
};

TcpReassembler* TcpReassemblerFactory::create( TcpSession* session, TcpTracker* tracker,
        StreamPolicy os_policy, bool server )
{
    NormMode tcp_ips_data = Normalize_GetMode(NORM_TCP_IPS);

    if (tcp_ips_data == NORM_MODE_ON)
        return new TcpReassemblerFirst( session, tracker, server );
    else
    {
        switch (os_policy)
        {
        case StreamPolicy::OS_FIRST:
            return new TcpReassemblerFirst( session, tracker, server );
            break;

        case StreamPolicy::OS_LAST:
            return new TcpReassemblerLast( session, tracker, server );
            break;

        case StreamPolicy::OS_LINUX:
            return new TcpReassemblerLinux( session, tracker, server );
            break;

        case StreamPolicy::OS_OLD_LINUX:
            return new TcpReassemblerOldLinux( session, tracker, server );
            break;

        case StreamPolicy::OS_BSD:
            return new TcpReassemblerBSD( session, tracker, server );
            break;

        case StreamPolicy::OS_MACOS:
            return new TcpReassemblerMacOS( session, tracker, server );
            break;

        case StreamPolicy::OS_SOLARIS:
            return new TcpReassemblerSolaris( session, tracker, server );
            break;

        case StreamPolicy::OS_IRIX:
            return new TcpReassemblerIrix( session, tracker, server );
            break;

        case StreamPolicy::OS_HPUX11:
            return new TcpReassemblerHpux11( session, tracker, server );
            break;

        case StreamPolicy::OS_HPUX10:
            return new TcpReassemblerHpux10( session, tracker, server );
            break;

        case StreamPolicy::OS_WINDOWS:
            return new TcpReassemblerWindows( session, tracker, server );
            break;

        case StreamPolicy::OS_WINDOWS2K3:
            return new TcpReassemblerWindows2K3( session, tracker, server );
            break;

        case StreamPolicy::OS_VISTA:
            return new TcpReassemblerVista( session, tracker, server );
            break;

        case StreamPolicy::OS_PROXY:
            return new TcpReassemblerProxy( session, tracker, server );
            break;

        default:
            return new TcpReassemblerBSD( session, tracker, server );
            break;
        }

    }
}

