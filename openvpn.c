/*
 * Copyright (C) 2010 James Bottomley <James.Bottomley@suse.de>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdarg.h>

#include "mtpd.h"

static int openvpn_connect(int argc, char **argv)
{
    char *args[argc + 5];
    int i;

    args[0] = "openvpn";
    /* for privilege separation, drop to user vpn */
    args[1] = "--user";
    args[2] = "vpn";

    for (i = 0; i < argc; i++)
	args[i+3] = argv[i];

    args[i+3] = NULL;
    
    start_daemon("openvpn", args, -1);

    return 0;
}

static void openvpn_shutdown(void)
{
}

struct protocol openvpn = {
    .name = "openvpn",
    .usage = "[openvpn args]",
    .connect = openvpn_connect,
    .process = NULL,
    .timeout = NULL,
    .shutdown = openvpn_shutdown,
};
