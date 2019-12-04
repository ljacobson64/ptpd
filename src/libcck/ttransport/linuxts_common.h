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
 * @file   linuxts_common.h
 * @date   Sat Jan 9 16:14:10 2016
 *
 * @brief  structure definitions for Linux SO_TIMESTAMPING transports
 *
 */

#ifndef CCK_TTRANSPORT_LINUXTS_COMMON_H_
#define CCK_TTRANSPORT_LINUXTS_COMMON_H_

#include <libcck/ttransport.h>
#include <libcck/net_utils.h>
#include <libcck/net_utils_linux.h>
#include <libcck/transport_address.h>

#define TT_MSGFLAGS_TXTIMESTAMP			0x80

#define LINUXTS_TXTIMESTAMP_BACKOFF_US		10
#define LINUXTS_TXTIMESTAMP_TIMEOUT_US		1500
#define LINUXTS_TXTIMESTAMP_RETRIES		6
#define LINUXTS_TXTIMESTAMP_BACKOFF_MULTIPLIER	2.0

typedef struct {
    uint32_t swTsModes;
    uint32_t hwTsModes;
    uint32_t tsModes;
    uint32_t rxFilter;
    uint32_t txType;
    uint32_t oneStepTxType;
    bool hwTimestamping;
    bool swTimestamping;
    bool txTimestamping;
    bool oneStep;
} LinuxTsInfo;

typedef struct {
    int txBackoff;
    int txTimeout;
    uint8_t txRetries;
    double txMultiplier;
} TTransportConfig_linuxts_common;

/* initialisation / destruction of any extra data in our private config object */
void _initTTransportConfig_linuxts_common(TTransportConfig_linuxts_common *myConfig, const int family);
void _freeTTransportConfig_linuxts_common(TTransportConfig_linuxts_common *myConfig);

bool probeLinuxTs(const char *path, const int family, const int caps);
void getLinuxTsInfo(LinuxTsInfo *output, const struct ethtool_ts_info *source, const char *ifName, const int family, const bool preferHw);
ClockDriver * getLinuxClockDriver(const TTransport *transport, const LinuxInterfaceInfo *intInfo);
void getLinuxTxTimestamp(TTransport *transport, TTransportMessage *txMessage, TTransportConfig_linuxts_common *cConfig);
bool initTimestamping_linuxts_common(TTransport *transport, TTsocketTimestampConfig *tsConfig, LinuxInterfaceInfo *intInfo, const char *ifName, const bool quiet);
char* getStatusLine_linuxts(const TTransport *transport, const CckInterfaceInfo *info, const LinuxInterfaceInfo *linfo, char *buf, const size_t len);
char* getInfoLine_linuxts(const TTransport *transport, const CckInterfaceInfo *info, const LinuxInterfaceInfo *linfo, char *buf, const size_t len);

#endif /* CCK_TTRANSPORT_LINUXTS_COMMON_H_ */

