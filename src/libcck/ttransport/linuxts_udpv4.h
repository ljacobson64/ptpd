/* Copyright (c) 2016 Wojciech Owczarek,
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   linuxts_udpv4.h
 * @date   Sat Jan 9 16:14:10 2016
 *
 * @brief  structure definitions for the Linux SO_TIMESTAMPING UDP/IPv4 transport
 *
 */

#ifndef CCK_TTRANSPORT_LINUXTS_UDPV4_H_
#define CCK_TTRANSPORT_LINUXTS_UDPV4_H_

#include <libcck/ttransport.h>
#include <libcck/net_utils.h>
#include <libcck/transport_address.h>

typedef struct {
	TTsocketTimestampConfig tsConfig;
	LinuxInterfaceInfo lintInfo;
	CckInterfaceInfo intInfo;
} TTransportData_linuxts_udpv4;

/* private initialisation, method assignment etc. */
bool _setupTTransport_linuxts_udpv4(TTransport *self);

/* probe if interface @path supports @flags */
bool _probeTTransport_linuxts_udpv4(const char *path, const int flags);

/* transport configuration - any extra private settings can be implemented here */
typedef struct {
	TTransportConfig_udp_common common;
	TTransportConfig_linuxts_common linuxts;
} TTransportConfig_linuxts_udpv4;

/* initialisation / destruction of any extra data in our private config object */
void _initTTransportConfig_linuxts_udpv4(TTransportConfig_linuxts_udpv4 *myConfig, const int family);
void _freeTTransportConfig_linuxts_udpv4(TTransportConfig_linuxts_udpv4 *myConfig);

#endif /* CCK_TTRANSPORT_LINUXTS_UDPV4_H_ */
