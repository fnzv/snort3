//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2012-2013 Sourcefire, Inc.
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

// file_service.h author author Hui Cao <huica@cisco.com>

#ifndef FILE_SERVICE_H
#define FILE_SERVICE_H

// This provides a wrapper to start/stop file service

#include <sys/types.h>
#include "main/snort_types.h"

class SO_PUBLIC FileService
{
public:
    // This must be called when snort restarts
    static void init(void);

    // Called after permission is dropped
    static void post_init(void);

    // This must be called when snort exits
    static void close(void);

    static void enable_file_type();
    static void enable_file_signature ();
    static void enable_file_capture();
    static bool is_file_type_id_enabled() {return file_type_id_enabled;};
    static bool is_file_signature_enabled() {return file_signature_enabled;};
    static bool is_file_capture_enabled() {return file_capture_enabled;};
    static bool is_file_service_enabled();
    static int64_t get_max_file_depth();

private:
    static void start_file_processing(void);
    static bool file_type_id_enabled;
    static bool file_signature_enabled;
    static bool file_capture_enabled;
    static bool file_processing_initiated;

};
#endif

