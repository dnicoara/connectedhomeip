/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <inet/InetInterface.h>
#include <lib/core/CHIPConfig.h>
#include <lib/core/CHIPError.h>
#include <lib/dnssd/Advertiser.h>
#include <lib/dnssd/DnssdCache.h>
#include <lib/dnssd/Resolver.h>
#include <lib/dnssd/ResolverProxy.h>
#include <lib/dnssd/platform/Dnssd.h>
#include <platform/CHIPDeviceConfig.h>

// Enable detailed mDNS logging for publish
#undef DETAIL_LOGGING
// #define DETAIL_LOGGING

namespace chip {
namespace Dnssd {

class DiscoveryImplPlatform : public ServiceAdvertiser, public Resolver
{
public:
    // Members that implement both ServiceAdveriser and Resolver interfaces.
    CHIP_ERROR Init(Inet::EndPointManager<Inet::UDPEndPoint> *) override { return InitImpl(); }
    void Shutdown() override;

    // Members that implement ServiceAdvertiser interface.
    CHIP_ERROR RemoveServices() override;
    CHIP_ERROR Advertise(const OperationalAdvertisingParameters & params) override;
    CHIP_ERROR Advertise(const CommissionAdvertisingParameters & params) override;
    CHIP_ERROR FinalizeServiceUpdate() override;
    CHIP_ERROR GetCommissionableInstanceName(char * instanceName, size_t maxLength) override;

    // Members that implement Resolver interface.
    void SetResolverDelegate(ResolverDelegate * delegate) override { mResolverProxy.SetResolverDelegate(delegate); }
    CHIP_ERROR ResolveNodeId(const PeerId & peerId, Inet::IPAddressType type) override;
    CHIP_ERROR FindCommissionableNodes(DiscoveryFilter filter = DiscoveryFilter()) override;
    CHIP_ERROR FindCommissioners(DiscoveryFilter filter = DiscoveryFilter()) override;
    bool ResolveNodeIdFromInternalCache(const PeerId & peerId, Inet::IPAddressType type) override;

    static DiscoveryImplPlatform & GetInstance();

private:
    DiscoveryImplPlatform();

    DiscoveryImplPlatform(const DiscoveryImplPlatform &) = delete;
    DiscoveryImplPlatform & operator=(const DiscoveryImplPlatform &) = delete;

    CHIP_ERROR InitImpl();
    CHIP_ERROR PublishUnprovisionedDevice(chip::Inet::IPAddressType addressType, chip::Inet::InterfaceId interface);
    CHIP_ERROR PublishProvisionedDevice(chip::Inet::IPAddressType addressType, chip::Inet::InterfaceId interface);

    static void HandleDnssdInit(void * context, CHIP_ERROR initError);
    static void HandleDnssdError(void * context, CHIP_ERROR initError);
    static void HandleDnssdPublish(void * context, const char * type, CHIP_ERROR error);
    static CHIP_ERROR GenerateRotatingDeviceId(char rotatingDeviceIdHexBuffer[], size_t & rotatingDeviceIdHexBufferSize);
    CHIP_ERROR PublishService(const char * serviceType, TextEntry * textEntries, size_t textEntrySize, const char ** subTypes,
                              size_t subTypeSize, const OperationalAdvertisingParameters & params);
    CHIP_ERROR PublishService(const char * serviceType, TextEntry * textEntries, size_t textEntrySize, const char ** subTypes,
                              size_t subTypeSize, const CommissionAdvertisingParameters & params);
    CHIP_ERROR PublishService(const char * serviceType, TextEntry * textEntries, size_t textEntrySize, const char ** subTypes,
                              size_t subTypeSize, uint16_t port, const chip::ByteSpan & mac, DnssdServiceProtocol procotol,
                              PeerId peerId);

    OperationalAdvertisingParameters mOperationalNodeAdvertisingParams;
    CommissionAdvertisingParameters mCommissionableNodeAdvertisingParams;
    CommissionAdvertisingParameters mCommissionerNodeAdvertisingParams;
    bool mIsOperationalNodePublishing    = false;
    bool mIsCommissionableNodePublishing = false;
    bool mIsCommissionerNodePublishing   = false;
    uint8_t mCommissionableInstanceName[sizeof(uint64_t)];

    bool mDnssdInitialized = false;

    ResolverProxy mResolverProxy;

    static DiscoveryImplPlatform sManager;
};

} // namespace Dnssd
} // namespace chip
