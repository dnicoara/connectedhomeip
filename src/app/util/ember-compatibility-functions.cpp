/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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

/**
 *    @file
 *          Contains the functions for compatibility with ember ZCL inner state
 *          when calling ember callbacks.
 */

#include <access/AccessControl.h>
#include <app/ClusterInfo.h>
#include <app/ConcreteAttributePath.h>
#include <app/GlobalAttributes.h>
#include <app/InteractionModelEngine.h>
#include <app/RequiredPrivilege.h>
#include <app/reporting/Engine.h>
#include <app/reporting/reporting.h>
#include <app/util/af.h>
#include <app/util/attribute-storage-null-handling.h>
#include <app/util/attribute-storage.h>
#include <app/util/attribute-table.h>
#include <app/util/ember-compatibility-functions.h>
#include <app/util/error-mapping.h>
#include <app/util/odd-sized-integers.h>
#include <app/util/util.h>
#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPTLV.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/SafeInt.h>
#include <lib/support/TypeTraits.h>
#include <platform/LockTracker.h>
#include <protocols/interaction_model/Constants.h>

#include <app-common/zap-generated/att-storage.h>
#include <app-common/zap-generated/attribute-type.h>

#include <zap-generated/endpoint_config.h>

#include <limits>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Compatibility;
using namespace chip::Access;

namespace chip {
namespace app {
namespace Compatibility {
namespace {
// On some apps, ATTRIBUTE_LARGEST can as small as 3, making compiler unhappy since data[kAttributeReadBufferSize] cannot hold
// uint64_t. Make kAttributeReadBufferSize at least 8 so it can fit all basic types.
constexpr size_t kAttributeReadBufferSize = (ATTRIBUTE_LARGEST >= 8 ? ATTRIBUTE_LARGEST : 8);
EmberAfClusterCommand imCompatibilityEmberAfCluster;
EmberApsFrame imCompatibilityEmberApsFrame;
EmberAfInterpanHeader imCompatibilityInterpanHeader;
CommandHandler * currentCommandObject;

// BasicType maps the type to basic int(8|16|32|64)(s|u) types.
EmberAfAttributeType BaseType(EmberAfAttributeType type)
{
    switch (type)
    {
    case ZCL_ACTION_ID_ATTRIBUTE_TYPE:  // Action Id
    case ZCL_FABRIC_IDX_ATTRIBUTE_TYPE: // Fabric Index
    case ZCL_BITMAP8_ATTRIBUTE_TYPE:    // 8-bit bitmap
    case ZCL_ENUM8_ATTRIBUTE_TYPE:      // 8-bit enumeration
    case ZCL_PERCENT_ATTRIBUTE_TYPE:    // Percentage
        static_assert(std::is_same<chip::Percent, uint8_t>::value,
                      "chip::Percent is expected to be uint8_t, change this when necessary");
        return ZCL_INT8U_ATTRIBUTE_TYPE;

    case ZCL_ENDPOINT_NO_ATTRIBUTE_TYPE:   // Endpoint Number
    case ZCL_GROUP_ID_ATTRIBUTE_TYPE:      // Group Id
    case ZCL_VENDOR_ID_ATTRIBUTE_TYPE:     // Vendor Id
    case ZCL_ENUM16_ATTRIBUTE_TYPE:        // 16-bit enumeration
    case ZCL_BITMAP16_ATTRIBUTE_TYPE:      // 16-bit bitmap
    case ZCL_STATUS_ATTRIBUTE_TYPE:        // Status Code
    case ZCL_PERCENT100THS_ATTRIBUTE_TYPE: // 100ths of a percent
        static_assert(std::is_same<chip::EndpointId, uint16_t>::value,
                      "chip::EndpointId is expected to be uint16_t, change this when necessary");
        static_assert(std::is_same<chip::GroupId, uint16_t>::value,
                      "chip::GroupId is expected to be uint16_t, change this when necessary");
        static_assert(std::is_same<chip::Percent100ths, uint16_t>::value,
                      "chip::Percent100ths is expected to be uint16_t, change this when necessary");
        return ZCL_INT16U_ATTRIBUTE_TYPE;

    case ZCL_CLUSTER_ID_ATTRIBUTE_TYPE: // Cluster Id
    case ZCL_ATTRIB_ID_ATTRIBUTE_TYPE:  // Attribute Id
    case ZCL_FIELD_ID_ATTRIBUTE_TYPE:   // Field Id
    case ZCL_EVENT_ID_ATTRIBUTE_TYPE:   // Event Id
    case ZCL_COMMAND_ID_ATTRIBUTE_TYPE: // Command Id
    case ZCL_TRANS_ID_ATTRIBUTE_TYPE:   // Transaction Id
    case ZCL_DEVTYPE_ID_ATTRIBUTE_TYPE: // Device Type Id
    case ZCL_DATA_VER_ATTRIBUTE_TYPE:   // Data Version
    case ZCL_BITMAP32_ATTRIBUTE_TYPE:   // 32-bit bitmap
    case ZCL_EPOCH_S_ATTRIBUTE_TYPE:    // Epoch Seconds
        static_assert(std::is_same<chip::ClusterId, uint32_t>::value,
                      "chip::Cluster is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::AttributeId, uint32_t>::value,
                      "chip::AttributeId is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::AttributeId, uint32_t>::value,
                      "chip::AttributeId is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::EventId, uint32_t>::value,
                      "chip::EventId is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::CommandId, uint32_t>::value,
                      "chip::CommandId is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::TransactionId, uint32_t>::value,
                      "chip::TransactionId is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::DeviceTypeId, uint32_t>::value,
                      "chip::DeviceTypeId is expected to be uint32_t, change this when necessary");
        static_assert(std::is_same<chip::DataVersion, uint32_t>::value,
                      "chip::DataVersion is expected to be uint32_t, change this when necessary");
        return ZCL_INT32U_ATTRIBUTE_TYPE;

    case ZCL_EVENT_NO_ATTRIBUTE_TYPE:  // Event Number
    case ZCL_FABRIC_ID_ATTRIBUTE_TYPE: // Fabric Id
    case ZCL_NODE_ID_ATTRIBUTE_TYPE:   // Node Id
    case ZCL_BITMAP64_ATTRIBUTE_TYPE:  // 64-bit bitmap
    case ZCL_EPOCH_US_ATTRIBUTE_TYPE:  // Epoch Microseconds
        static_assert(std::is_same<chip::EventNumber, uint64_t>::value,
                      "chip::EventNumber is expected to be uint64_t, change this when necessary");
        static_assert(std::is_same<chip::FabricId, uint64_t>::value,
                      "chip::FabricId is expected to be uint64_t, change this when necessary");
        static_assert(std::is_same<chip::NodeId, uint64_t>::value,
                      "chip::NodeId is expected to be uint64_t, change this when necessary");
        return ZCL_INT64U_ATTRIBUTE_TYPE;

    default:
        return type;
    }
}

} // namespace

void SetupEmberAfCommandSender(CommandSender * command, const ConcreteCommandPath & commandPath)
{
    Messaging::ExchangeContext * commandExchangeCtx = command->GetExchangeContext();

    imCompatibilityEmberApsFrame.clusterId           = commandPath.mClusterId;
    imCompatibilityEmberApsFrame.destinationEndpoint = commandPath.mEndpointId;
    imCompatibilityEmberApsFrame.sourceEndpoint      = 1; // source endpoint is fixed to 1 for now.
    imCompatibilityEmberApsFrame.sequence =
        (commandExchangeCtx != nullptr ? static_cast<uint8_t>(commandExchangeCtx->GetExchangeId() & 0xFF) : 0);

    if (commandExchangeCtx->IsGroupExchangeContext())
    {
        imCompatibilityEmberAfCluster.type = EMBER_INCOMING_MULTICAST;
    }
    else
    {
        imCompatibilityEmberAfCluster.type = EMBER_INCOMING_UNICAST;
    }

    imCompatibilityEmberAfCluster.commandId      = commandPath.mCommandId;
    imCompatibilityEmberAfCluster.apsFrame       = &imCompatibilityEmberApsFrame;
    imCompatibilityEmberAfCluster.interPanHeader = &imCompatibilityInterpanHeader;
    imCompatibilityEmberAfCluster.source         = commandExchangeCtx;

    emAfCurrentCommand = &imCompatibilityEmberAfCluster;
}

void SetupEmberAfCommandHandler(CommandHandler * command, const ConcreteCommandPath & commandPath)
{
    Messaging::ExchangeContext * commandExchangeCtx = command->GetExchangeContext();

    imCompatibilityEmberApsFrame.clusterId           = commandPath.mClusterId;
    imCompatibilityEmberApsFrame.destinationEndpoint = commandPath.mEndpointId;
    imCompatibilityEmberApsFrame.sourceEndpoint      = 1; // source endpoint is fixed to 1 for now.
    imCompatibilityEmberApsFrame.sequence =
        (commandExchangeCtx != nullptr ? static_cast<uint8_t>(commandExchangeCtx->GetExchangeId() & 0xFF) : 0);

    if (commandExchangeCtx->IsGroupExchangeContext())
    {
        imCompatibilityEmberAfCluster.type = EMBER_INCOMING_MULTICAST;
    }
    else
    {
        imCompatibilityEmberAfCluster.type = EMBER_INCOMING_UNICAST;
    }

    imCompatibilityEmberAfCluster.commandId      = commandPath.mCommandId;
    imCompatibilityEmberAfCluster.apsFrame       = &imCompatibilityEmberApsFrame;
    imCompatibilityEmberAfCluster.interPanHeader = &imCompatibilityInterpanHeader;
    imCompatibilityEmberAfCluster.source         = commandExchangeCtx;

    emAfCurrentCommand   = &imCompatibilityEmberAfCluster;
    currentCommandObject = command;
}

bool IMEmberAfSendDefaultResponseWithCallback(EmberAfStatus status)
{
    if (currentCommandObject == nullptr)
    {
        // We have no idea what we're supposed to respond to.
        return false;
    }

    chip::app::ConcreteCommandPath commandPath(imCompatibilityEmberApsFrame.destinationEndpoint,
                                               imCompatibilityEmberApsFrame.clusterId, imCompatibilityEmberAfCluster.commandId);

    CHIP_ERROR err = currentCommandObject->AddStatus(commandPath, ToInteractionModelStatus(status));
    return CHIP_NO_ERROR == err;
}

void ResetEmberAfObjects()
{
    emAfCurrentCommand   = nullptr;
    currentCommandObject = nullptr;
}

} // namespace Compatibility

namespace {
// Common buffer for ReadSingleClusterData & WriteSingleClusterData
uint8_t attributeData[kAttributeReadBufferSize];

template <typename T>
CHIP_ERROR attributeBufferToNumericTlvData(TLV::TLVWriter & writer, bool isNullable)
{
    typename NumericAttributeTraits<T>::StorageType value;
    memcpy(&value, attributeData, sizeof(value));
    TLV::Tag tag = TLV::ContextTag(to_underlying(AttributeDataIB::Tag::kData));
    if (isNullable && NumericAttributeTraits<T>::IsNullValue(value))
    {
        return writer.PutNull(tag);
    }

    if (!NumericAttributeTraits<T>::CanRepresentValue(isNullable, value))
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    return NumericAttributeTraits<T>::Encode(writer, tag, value);
}

} // anonymous namespace

Protocols::InteractionModel::Status ServerClusterCommandExists(const ConcreteCommandPath & aCommandPath)
{
    using Protocols::InteractionModel::Status;

    const EmberAfEndpointType * type = emberAfFindEndpointType(aCommandPath.mEndpointId);
    if (type == nullptr)
    {
        return Status::UnsupportedEndpoint;
    }

    const EmberAfCluster * cluster = emberAfFindClusterInType(type, aCommandPath.mClusterId, CLUSTER_MASK_SERVER);
    if (cluster == nullptr)
    {
        return Status::UnsupportedCluster;
    }

    for (const CommandId * cmd = cluster->clientGeneratedCommandList; cmd != nullptr; cmd++)
    {
        if (*cmd == aCommandPath.mCommandId)
        {
            return Status::Success;
        }
    }

    return Status::UnsupportedCommand;
}

namespace {

CHIP_ERROR ReadClusterDataVersion(const EndpointId & aEndpointId, const ClusterId & aClusterId, DataVersion & aDataVersion)
{
    DataVersion * version = emberAfDataVersionStorage(aEndpointId, aClusterId);
    if (version == nullptr)
    {
        ChipLogError(DataManagement, "Endpoint %" PRIx16 ", Cluster " ChipLogFormatMEI " not found in ReadClusterDataVersion!",
                     aEndpointId, ChipLogValueMEI(aClusterId));
        return CHIP_ERROR_NOT_FOUND;
    }
    aDataVersion = *version;
    return CHIP_NO_ERROR;
}

void IncreaseClusterDataVersion(const EndpointId & aEndpointId, const ClusterId & aClusterId)
{
    DataVersion * version = emberAfDataVersionStorage(aEndpointId, aClusterId);
    if (version == nullptr)
    {
        ChipLogError(DataManagement, "Endpoint %" PRIx16 ", Cluster " ChipLogFormatMEI " not found in IncreaseClusterDataVersion!",
                     aEndpointId, ChipLogValueMEI(aClusterId));
    }
    else
    {
        (*(version))++;
        ChipLogDetail(DataManagement, "Endpoint %" PRIx16 ", Cluster " ChipLogFormatMEI " update version to %" PRIx32, aEndpointId,
                      ChipLogValueMEI(aClusterId), *(version));
    }
    return;
}

CHIP_ERROR SendSuccessStatus(AttributeReportIB::Builder & aAttributeReport, AttributeDataIB::Builder & aAttributeDataIBBuilder)
{
    ReturnErrorOnFailure(aAttributeDataIBBuilder.EndOfAttributeDataIB().GetError());
    return aAttributeReport.EndOfAttributeReportIB().GetError();
}

CHIP_ERROR SendFailureStatus(const ConcreteAttributePath & aPath, AttributeReportIBs::Builder & aAttributeReports,
                             Protocols::InteractionModel::Status aStatus, TLV::TLVWriter * aReportCheckpoint)
{
    if (aReportCheckpoint != nullptr)
    {
        aAttributeReports.Rollback(*aReportCheckpoint);
    }
    return aAttributeReports.EncodeAttributeStatus(aPath, StatusIB(aStatus));
}

// This reader should never actually be registered; we do manual dispatch to it
// for the one attribute it handles.
class MandatoryGlobalAttributeReader : public AttributeAccessInterface
{
public:
    MandatoryGlobalAttributeReader(const EmberAfCluster * aCluster) :
        AttributeAccessInterface(MakeOptional(kInvalidEndpointId), kInvalidClusterId), mCluster(aCluster)
    {}

protected:
    const EmberAfCluster * mCluster;
};

class GlobalAttributeReader : public MandatoryGlobalAttributeReader
{
public:
    GlobalAttributeReader(const EmberAfCluster * aCluster) : MandatoryGlobalAttributeReader(aCluster) {}

    CHIP_ERROR Read(const ConcreteReadAttributePath & aPath, AttributeValueEncoder & aEncoder) override;
};

CHIP_ERROR GlobalAttributeReader::Read(const ConcreteReadAttributePath & aPath, AttributeValueEncoder & aEncoder)
{
    using namespace Clusters::Globals::Attributes;
    switch (aPath.mAttributeId)
    {
    case AttributeList::Id:
        return aEncoder.EncodeList([this](const auto & encoder) {
            const size_t count     = mCluster->attributeCount;
            bool addedExtraGlobals = false;
            for (size_t i = 0; i < count; ++i)
            {
                AttributeId id              = mCluster->attributes[i].attributeId;
                constexpr auto lastGlobalId = GlobalAttributesNotInMetadata[ArraySize(GlobalAttributesNotInMetadata) - 1];
                // We are relying on GlobalAttributesNotInMetadata not having
                // any gaps in their ids here, but for now they do because we
                // have no EventList support.  So this assert is not quite
                // right.  There should be a "- 1" on the right-hand side of the
                // equals sign.
                static_assert(lastGlobalId - GlobalAttributesNotInMetadata[0] == ArraySize(GlobalAttributesNotInMetadata),
                              "Ids in GlobalAttributesNotInMetadata not consecutive (except EventList)");
                if (!addedExtraGlobals && id > lastGlobalId)
                {
                    for (const auto & globalId : GlobalAttributesNotInMetadata)
                    {
                        ReturnErrorOnFailure(encoder.Encode(globalId));
                    }
                    addedExtraGlobals = true;
                }
                ReturnErrorOnFailure(encoder.Encode(id));
            }
            if (!addedExtraGlobals)
            {
                for (const auto & globalId : GlobalAttributesNotInMetadata)
                {
                    ReturnErrorOnFailure(encoder.Encode(globalId));
                }
            }
            return CHIP_NO_ERROR;
        });
    case ClientGeneratedCommandList::Id:
        return aEncoder.EncodeList([this](const auto & encoder) {
            for (const CommandId * cmd = mCluster->clientGeneratedCommandList; cmd != nullptr && *cmd != kInvalidCommandId; cmd++)
            {
                ReturnErrorOnFailure(encoder.Encode(*cmd));
            }
            return CHIP_NO_ERROR;
        });
    case ServerGeneratedCommandList::Id:
        return aEncoder.EncodeList([this](const auto & encoder) {
            for (const CommandId * cmd = mCluster->serverGeneratedCommandList; cmd != nullptr && *cmd != kInvalidCommandId; cmd++)
            {
                ReturnErrorOnFailure(encoder.Encode(*cmd));
            }
            return CHIP_NO_ERROR;
        });
    default:
        return CHIP_NO_ERROR;
    }
}

// Helper function for trying to read an attribute value via an
// AttributeAccessInterface.  On failure, the read has failed.  On success, the
// aTriedEncode outparam is set to whether the AttributeAccessInterface tried to encode a value.
CHIP_ERROR ReadViaAccessInterface(FabricIndex aAccessingFabricIndex, bool aIsFabricFiltered,
                                  const ConcreteReadAttributePath & aPath, AttributeReportIBs::Builder & aAttributeReports,
                                  AttributeValueEncoder::AttributeEncodeState * aEncoderState,
                                  AttributeAccessInterface * aAccessInterface, bool * aTriedEncode)
{
    AttributeValueEncoder::AttributeEncodeState state =
        (aEncoderState == nullptr ? AttributeValueEncoder::AttributeEncodeState() : *aEncoderState);
    DataVersion version = 0;
    ReturnErrorOnFailure(ReadClusterDataVersion(aPath.mEndpointId, aPath.mClusterId, version));
    AttributeValueEncoder valueEncoder(aAttributeReports, aAccessingFabricIndex, aPath, version, aIsFabricFiltered, state);
    CHIP_ERROR err = aAccessInterface->Read(aPath, valueEncoder);

    if (err != CHIP_NO_ERROR)
    {
        // If the err is not CHIP_NO_ERROR, means the encoding was aborted, then the valueEncoder may save its state.
        // The state is used by list chunking feature for now.
        if (aEncoderState != nullptr)
        {
            *aEncoderState = valueEncoder.GetState();
        }
        return err;
    }

    *aTriedEncode = valueEncoder.TriedEncode();
    return CHIP_NO_ERROR;
}

// Determine the appropriate status response for an unsupported attribute for
// the given path.  Must be called when the attribute is known to be unsupported
// (i.e. we found no attribute metadata for it).
Protocols::InteractionModel::Status UnsupportedAttributeStatus(const ConcreteAttributePath & aPath)
{
    using Protocols::InteractionModel::Status;

    const EmberAfEndpointType * type = emberAfFindEndpointType(aPath.mEndpointId);
    if (type == nullptr)
    {
        return Status::UnsupportedEndpoint;
    }

    const EmberAfCluster * cluster = emberAfFindClusterInType(type, aPath.mClusterId, CLUSTER_MASK_SERVER);
    if (cluster == nullptr)
    {
        return Status::UnsupportedCluster;
    }

    // Since we know the attribute is unsupported and the endpoint/cluster are
    // OK, this is the only option left.
    return Status::UnsupportedAttribute;
}

} // anonymous namespace

CHIP_ERROR ReadSingleClusterData(const SubjectDescriptor & aSubjectDescriptor, bool aIsFabricFiltered,
                                 const ConcreteReadAttributePath & aPath, AttributeReportIBs::Builder & aAttributeReports,
                                 AttributeValueEncoder::AttributeEncodeState * apEncoderState)
{
    ChipLogDetail(DataManagement,
                  "Reading attribute: Cluster=" ChipLogFormatMEI " Endpoint=%" PRIx16 " AttributeId=" ChipLogFormatMEI
                  " (expanded=%d)",
                  ChipLogValueMEI(aPath.mClusterId), aPath.mEndpointId, ChipLogValueMEI(aPath.mAttributeId), aPath.mExpanded);

    // Check attribute existence. This includes attributes with registered metadata, but also specially handled
    // mandatory global attributes (which just check for cluster on endpoint).

    const EmberAfCluster * attributeCluster            = nullptr;
    const EmberAfAttributeMetadata * attributeMetadata = nullptr;

    switch (aPath.mAttributeId)
    {
    case Clusters::Globals::Attributes::AttributeList::Id:
        FALLTHROUGH;
    case Clusters::Globals::Attributes::ClientGeneratedCommandList::Id:
        FALLTHROUGH;
    case Clusters::Globals::Attributes::ServerGeneratedCommandList::Id:
        attributeCluster = emberAfFindCluster(aPath.mEndpointId, aPath.mClusterId, CLUSTER_MASK_SERVER);
        break;
    default:
        attributeMetadata =
            emberAfLocateAttributeMetadata(aPath.mEndpointId, aPath.mClusterId, aPath.mAttributeId, CLUSTER_MASK_SERVER);
    }

    if (attributeCluster == nullptr && attributeMetadata == nullptr)
    {
        return SendFailureStatus(aPath, aAttributeReports, UnsupportedAttributeStatus(aPath), nullptr);
    }

    // Check access control. A failed check will disallow the operation, and may or may not generate an attribute report
    // depending on whether the path was expanded.

    {
        Access::RequestPath requestPath{ .cluster = aPath.mClusterId, .endpoint = aPath.mEndpointId };
        Access::Privilege requestPrivilege = RequiredPrivilege::ForReadAttribute(aPath);
        CHIP_ERROR err                     = Access::GetAccessControl().Check(aSubjectDescriptor, requestPath, requestPrivilege);
        if (err != CHIP_NO_ERROR)
        {
            // Grace period until ACLs are in place
            ChipLogError(DataManagement, "AccessControl: overriding DENY (for now)");
            err = CHIP_NO_ERROR;
        }
        if (err != CHIP_NO_ERROR)
        {
            ReturnErrorCodeIf(err != CHIP_ERROR_ACCESS_DENIED, err);
            if (aPath.mExpanded)
            {
                return CHIP_NO_ERROR;
            }
            else
            {
                return SendFailureStatus(aPath, aAttributeReports, Protocols::InteractionModel::Status::UnsupportedAccess, nullptr);
            }
        }
    }

    {
        // Special handling for mandatory global attributes: these are always for attribute list, using a special
        // reader (which can be lightweight constructed even from nullptr).
        GlobalAttributeReader reader(attributeCluster);
        AttributeAccessInterface * attributeOverride =
            (attributeCluster != nullptr) ? &reader : findAttributeAccessOverride(aPath.mEndpointId, aPath.mClusterId);
        if (attributeOverride)
        {
            bool triedEncode;
            ReturnErrorOnFailure(ReadViaAccessInterface(aSubjectDescriptor.fabricIndex, aIsFabricFiltered, aPath, aAttributeReports,
                                                        apEncoderState, attributeOverride, &triedEncode));
            ReturnErrorCodeIf(triedEncode, CHIP_NO_ERROR);
        }
    }

    // Read attribute using Ember, if it doesn't have an override.

    TLV::TLVWriter backup;
    aAttributeReports.Checkpoint(backup);

    AttributeReportIB::Builder & attributeReport = aAttributeReports.CreateAttributeReport();
    ReturnErrorOnFailure(aAttributeReports.GetError());

    AttributeDataIB::Builder & attributeDataIBBuilder = attributeReport.CreateAttributeData();
    ReturnErrorOnFailure(attributeDataIBBuilder.GetError());

    DataVersion version = 0;
    ReturnErrorOnFailure(ReadClusterDataVersion(aPath.mEndpointId, aPath.mClusterId, version));
    attributeDataIBBuilder.DataVersion(version);
    ReturnErrorOnFailure(attributeDataIBBuilder.GetError());

    AttributePathIB::Builder & attributePathIBBuilder = attributeDataIBBuilder.CreatePath();
    ReturnErrorOnFailure(attributeDataIBBuilder.GetError());

    attributePathIBBuilder.Endpoint(aPath.mEndpointId)
        .Cluster(aPath.mClusterId)
        .Attribute(aPath.mAttributeId)
        .EndOfAttributePathIB();
    ReturnErrorOnFailure(attributePathIBBuilder.GetError());

    EmberAfAttributeSearchRecord record;
    record.endpoint           = aPath.mEndpointId;
    record.clusterId          = aPath.mClusterId;
    record.clusterMask        = CLUSTER_MASK_SERVER;
    record.attributeId        = aPath.mAttributeId;
    EmberAfStatus emberStatus = emAfReadOrWriteAttribute(&record, &attributeMetadata, attributeData, sizeof(attributeData),
                                                         /* write = */ false);

    if (emberStatus == EMBER_ZCL_STATUS_SUCCESS)
    {
        EmberAfAttributeType attributeType = attributeMetadata->attributeType;
        bool isNullable                    = attributeMetadata->IsNullable();
        TLV::TLVWriter * writer            = attributeDataIBBuilder.GetWriter();
        VerifyOrReturnError(writer != nullptr, CHIP_NO_ERROR);
        TLV::Tag tag = TLV::ContextTag(to_underlying(AttributeDataIB::Tag::kData));
        switch (BaseType(attributeType))
        {
        case ZCL_NO_DATA_ATTRIBUTE_TYPE: // No data
            ReturnErrorOnFailure(writer->PutNull(tag));
            break;
        case ZCL_BOOLEAN_ATTRIBUTE_TYPE: // Boolean
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<bool>(*writer, isNullable));
            break;
        case ZCL_INT8U_ATTRIBUTE_TYPE: // Unsigned 8-bit integer
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<uint8_t>(*writer, isNullable));
            break;
        case ZCL_INT16U_ATTRIBUTE_TYPE: // Unsigned 16-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<uint16_t>(*writer, isNullable));
            break;
        }
        case ZCL_INT24U_ATTRIBUTE_TYPE: // Unsigned 24-bit integer
        {
            using IntType = OddSizedInteger<3, false>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT32U_ATTRIBUTE_TYPE: // Unsigned 32-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<uint32_t>(*writer, isNullable));
            break;
        }
        case ZCL_INT40U_ATTRIBUTE_TYPE: // Unsigned 40-bit integer
        {
            using IntType = OddSizedInteger<5, false>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT48U_ATTRIBUTE_TYPE: // Unsigned 48-bit integer
        {
            using IntType = OddSizedInteger<6, false>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT56U_ATTRIBUTE_TYPE: // Unsigned 56-bit integer
        {
            using IntType = OddSizedInteger<7, false>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT64U_ATTRIBUTE_TYPE: // Unsigned 64-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<uint64_t>(*writer, isNullable));
            break;
        }
        case ZCL_INT8S_ATTRIBUTE_TYPE: // Signed 8-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<int8_t>(*writer, isNullable));
            break;
        }
        case ZCL_INT16S_ATTRIBUTE_TYPE: // Signed 16-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<int16_t>(*writer, isNullable));
            break;
        }
        case ZCL_INT24S_ATTRIBUTE_TYPE: // Signed 24-bit integer
        {
            using IntType = OddSizedInteger<3, true>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT32S_ATTRIBUTE_TYPE: // Signed 32-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<int32_t>(*writer, isNullable));
            break;
        }
        case ZCL_INT40S_ATTRIBUTE_TYPE: // Signed 40-bit integer
        {
            using IntType = OddSizedInteger<5, true>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT48S_ATTRIBUTE_TYPE: // Signed 48-bit integer
        {
            using IntType = OddSizedInteger<6, true>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT56S_ATTRIBUTE_TYPE: // Signed 56-bit integer
        {
            using IntType = OddSizedInteger<7, true>;
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<IntType>(*writer, isNullable));
            break;
        }
        case ZCL_INT64S_ATTRIBUTE_TYPE: // Signed 64-bit integer
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<int64_t>(*writer, isNullable));
            break;
        }
        case ZCL_SINGLE_ATTRIBUTE_TYPE: // 32-bit float
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<float>(*writer, isNullable));
            break;
        }
        case ZCL_DOUBLE_ATTRIBUTE_TYPE: // 64-bit float
        {
            ReturnErrorOnFailure(attributeBufferToNumericTlvData<double>(*writer, isNullable));
            break;
        }
        case ZCL_CHAR_STRING_ATTRIBUTE_TYPE: // Char string
        {
            char * actualData  = reinterpret_cast<char *>(attributeData + 1);
            uint8_t dataLength = attributeData[0];
            if (dataLength == 0xFF)
            {
                if (isNullable)
                {
                    ReturnErrorOnFailure(writer->PutNull(tag));
                }
                else
                {
                    return CHIP_ERROR_INCORRECT_STATE;
                }
            }
            else
            {
                ReturnErrorOnFailure(writer->PutString(tag, actualData, dataLength));
            }
            break;
        }
        case ZCL_LONG_CHAR_STRING_ATTRIBUTE_TYPE: {
            char * actualData = reinterpret_cast<char *>(attributeData + 2); // The pascal string contains 2 bytes length
            uint16_t dataLength;
            memcpy(&dataLength, attributeData, sizeof(dataLength));
            if (dataLength == 0xFFFF)
            {
                if (isNullable)
                {
                    ReturnErrorOnFailure(writer->PutNull(tag));
                }
                else
                {
                    return CHIP_ERROR_INCORRECT_STATE;
                }
            }
            else
            {
                ReturnErrorOnFailure(writer->PutString(tag, actualData, dataLength));
            }
            break;
        }
        case ZCL_OCTET_STRING_ATTRIBUTE_TYPE: // Octet string
        {
            uint8_t * actualData = attributeData + 1;
            uint8_t dataLength   = attributeData[0];
            if (dataLength == 0xFF)
            {
                if (isNullable)
                {
                    ReturnErrorOnFailure(writer->PutNull(tag));
                }
                else
                {
                    return CHIP_ERROR_INCORRECT_STATE;
                }
            }
            else
            {
                ReturnErrorOnFailure(writer->Put(tag, chip::ByteSpan(actualData, dataLength)));
            }
            break;
        }
        case ZCL_LONG_OCTET_STRING_ATTRIBUTE_TYPE: {
            uint8_t * actualData = attributeData + 2; // The pascal string contains 2 bytes length
            uint16_t dataLength;
            memcpy(&dataLength, attributeData, sizeof(dataLength));
            if (dataLength == 0xFFFF)
            {
                if (isNullable)
                {
                    ReturnErrorOnFailure(writer->PutNull(tag));
                }
                else
                {
                    return CHIP_ERROR_INCORRECT_STATE;
                }
            }
            else
            {
                ReturnErrorOnFailure(writer->Put(tag, chip::ByteSpan(actualData, dataLength)));
            }
            break;
        }
        default:
            ChipLogError(DataManagement, "Attribute type 0x%x not handled", static_cast<int>(attributeType));
            emberStatus = EMBER_ZCL_STATUS_UNSUPPORTED_READ;
        }
    }

    Protocols::InteractionModel::Status imStatus = ToInteractionModelStatus(emberStatus);
    if (imStatus == Protocols::InteractionModel::Status::Success)
    {
        return SendSuccessStatus(attributeReport, attributeDataIBBuilder);
    }

    return SendFailureStatus(aPath, aAttributeReports, imStatus, &backup);
}

namespace {

template <typename T>
CHIP_ERROR numericTlvDataToAttributeBuffer(TLV::TLVReader & aReader, bool isNullable, uint16_t & dataLen)
{
    typename NumericAttributeTraits<T>::StorageType value;
    static_assert(sizeof(value) <= sizeof(attributeData), "Value cannot fit into attribute data");
    if (isNullable && aReader.GetType() == TLV::kTLVType_Null)
    {
        NumericAttributeTraits<T>::SetNull(value);
    }
    else
    {
        typename NumericAttributeTraits<T>::WorkingType val;
        ReturnErrorOnFailure(aReader.Get(val));
        VerifyOrReturnError(NumericAttributeTraits<T>::CanRepresentValue(isNullable, val), CHIP_ERROR_INVALID_ARGUMENT);
        NumericAttributeTraits<T>::WorkingToStorage(val, value);
    }
    dataLen = sizeof(value);
    memcpy(attributeData, &value, sizeof(value));
    return CHIP_NO_ERROR;
}

template <typename T>
CHIP_ERROR stringTlvDataToAttributeBuffer(TLV::TLVReader & aReader, bool isOctetString, bool isNullable, uint16_t & dataLen)
{
    const uint8_t * data = nullptr;
    T len;
    if (isNullable && aReader.GetType() == TLV::kTLVType_Null)
    {
        // Null is represented by an 0xFF or 0xFFFF length, respectively.
        len = std::numeric_limits<T>::max();
        memcpy(&attributeData[0], &len, sizeof(len));
        dataLen = sizeof(len);
    }
    else
    {
        VerifyOrReturnError((isOctetString && aReader.GetType() == TLV::TLVType::kTLVType_ByteString) ||
                                (!isOctetString && aReader.GetType() == TLV::TLVType::kTLVType_UTF8String),
                            CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(CanCastTo<T>(aReader.GetLength()), CHIP_ERROR_MESSAGE_TOO_LONG);
        ReturnErrorOnFailure(aReader.GetDataPtr(data));
        len = static_cast<T>(aReader.GetLength());
        VerifyOrReturnError(len != std::numeric_limits<T>::max(), CHIP_ERROR_MESSAGE_TOO_LONG);
        VerifyOrReturnError(len + sizeof(len) /* length at the beginning of data */ <= sizeof(attributeData),
                            CHIP_ERROR_MESSAGE_TOO_LONG);
        memcpy(&attributeData[0], &len, sizeof(len));
        memcpy(&attributeData[sizeof(len)], data, len);
        dataLen = static_cast<uint16_t>(len + sizeof(len));
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR prepareWriteData(const EmberAfAttributeMetadata * attributeMetadata, TLV::TLVReader & aReader, uint16_t & dataLen)
{
    EmberAfAttributeType expectedType = BaseType(attributeMetadata->attributeType);
    bool isNullable                   = attributeMetadata->IsNullable();
    switch (expectedType)
    {
    case ZCL_BOOLEAN_ATTRIBUTE_TYPE: // Boolean
        return numericTlvDataToAttributeBuffer<bool>(aReader, isNullable, dataLen);
    case ZCL_INT8U_ATTRIBUTE_TYPE: // Unsigned 8-bit integer
        return numericTlvDataToAttributeBuffer<uint8_t>(aReader, isNullable, dataLen);
    case ZCL_INT16U_ATTRIBUTE_TYPE: // Unsigned 16-bit integer
        return numericTlvDataToAttributeBuffer<uint16_t>(aReader, isNullable, dataLen);
    case ZCL_INT24U_ATTRIBUTE_TYPE: // Unsigned 24-bit integer
    {
        using IntType = OddSizedInteger<3, false>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT32U_ATTRIBUTE_TYPE: // Unsigned 32-bit integer
        return numericTlvDataToAttributeBuffer<uint32_t>(aReader, isNullable, dataLen);
    case ZCL_INT40U_ATTRIBUTE_TYPE: // Unsigned 40-bit integer
    {
        using IntType = OddSizedInteger<5, false>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT48U_ATTRIBUTE_TYPE: // Unsigned 48-bit integer
    {
        using IntType = OddSizedInteger<6, false>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT56U_ATTRIBUTE_TYPE: // Unsigned 56-bit integer
    {
        using IntType = OddSizedInteger<7, false>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT64U_ATTRIBUTE_TYPE: // Unsigned 64-bit integer
        return numericTlvDataToAttributeBuffer<uint64_t>(aReader, isNullable, dataLen);
    case ZCL_INT8S_ATTRIBUTE_TYPE: // Signed 8-bit integer
        return numericTlvDataToAttributeBuffer<int8_t>(aReader, isNullable, dataLen);
    case ZCL_INT16S_ATTRIBUTE_TYPE: // Signed 16-bit integer
        return numericTlvDataToAttributeBuffer<int16_t>(aReader, isNullable, dataLen);
    case ZCL_INT24S_ATTRIBUTE_TYPE: // Signed 24-bit integer
    {
        using IntType = OddSizedInteger<3, true>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT32S_ATTRIBUTE_TYPE: // Signed 32-bit integer
        return numericTlvDataToAttributeBuffer<int32_t>(aReader, isNullable, dataLen);
    case ZCL_INT40S_ATTRIBUTE_TYPE: // Signed 40-bit integer
    {
        using IntType = OddSizedInteger<5, true>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT48S_ATTRIBUTE_TYPE: // Signed 48-bit integer
    {
        using IntType = OddSizedInteger<6, true>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT56S_ATTRIBUTE_TYPE: // Signed 56-bit integer
    {
        using IntType = OddSizedInteger<7, true>;
        return numericTlvDataToAttributeBuffer<IntType>(aReader, isNullable, dataLen);
    }
    case ZCL_INT64S_ATTRIBUTE_TYPE: // Signed 64-bit integer
        return numericTlvDataToAttributeBuffer<int64_t>(aReader, isNullable, dataLen);
    case ZCL_SINGLE_ATTRIBUTE_TYPE: // 32-bit float
        return numericTlvDataToAttributeBuffer<float>(aReader, isNullable, dataLen);
    case ZCL_DOUBLE_ATTRIBUTE_TYPE: // 64-bit float
        return numericTlvDataToAttributeBuffer<double>(aReader, isNullable, dataLen);
    case ZCL_OCTET_STRING_ATTRIBUTE_TYPE: // Octet string
    case ZCL_CHAR_STRING_ATTRIBUTE_TYPE:  // Char string
        return stringTlvDataToAttributeBuffer<uint8_t>(aReader, expectedType == ZCL_OCTET_STRING_ATTRIBUTE_TYPE, isNullable,
                                                       dataLen);
    case ZCL_LONG_OCTET_STRING_ATTRIBUTE_TYPE: // Long octet string
    case ZCL_LONG_CHAR_STRING_ATTRIBUTE_TYPE:  // Long char string
        return stringTlvDataToAttributeBuffer<uint16_t>(aReader, expectedType == ZCL_LONG_OCTET_STRING_ATTRIBUTE_TYPE, isNullable,
                                                        dataLen);
    default:
        ChipLogError(DataManagement, "Attribute type %x not handled", static_cast<int>(expectedType));
        return CHIP_ERROR_INVALID_DATA_LIST;
    }
}
} // namespace

CHIP_ERROR WriteSingleClusterData(const SubjectDescriptor & aSubjectDescriptor, const ConcreteDataAttributePath & aPath,
                                  TLV::TLVReader & aReader, WriteHandler * apWriteHandler)
{
    const EmberAfAttributeMetadata * attributeMetadata =
        emberAfLocateAttributeMetadata(aPath.mEndpointId, aPath.mClusterId, aPath.mAttributeId, CLUSTER_MASK_SERVER);

    if (attributeMetadata == nullptr)
    {
        return apWriteHandler->AddStatus(aPath, UnsupportedAttributeStatus(aPath));
    }

    if (attributeMetadata->IsReadOnly())
    {
        return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::UnsupportedWrite);
    }

    {
        Access::RequestPath requestPath{ .cluster = aPath.mClusterId, .endpoint = aPath.mEndpointId };
        Access::Privilege requestPrivilege = RequiredPrivilege::ForWriteAttribute(aPath);
        CHIP_ERROR err                     = CHIP_NO_ERROR;
        if (!apWriteHandler->ACLCheckCacheHit({ aPath, requestPrivilege }))
        {
            err = Access::GetAccessControl().Check(aSubjectDescriptor, requestPath, requestPrivilege);
        }
        if (err != CHIP_NO_ERROR)
        {
            // Grace period until ACLs are in place
            ChipLogError(DataManagement, "AccessControl: overriding DENY (for now)");
            err = CHIP_NO_ERROR;
        }
        if (err != CHIP_NO_ERROR)
        {
            ReturnErrorCodeIf(err != CHIP_ERROR_ACCESS_DENIED, err);
            // TODO: when wildcard/group writes are supported, handle them to discard rather than fail with status
            return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::UnsupportedAccess);
        }
        apWriteHandler->CacheACLCheckResult({ aPath, requestPrivilege });
    }

    if (attributeMetadata->MustUseTimedWrite() && !apWriteHandler->IsTimedWrite())
    {
        return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::NeedsTimedInteraction);
    }

    if (aPath.mDataVersion.HasValue() &&
        !IsClusterDataVersionEqual(aPath.mEndpointId, aPath.mClusterId, aPath.mDataVersion.Value()))
    {
        ChipLogError(DataManagement, "Write Version mismatch for Endpoint %" PRIx16 ", Cluster " ChipLogFormatMEI,
                     aPath.mEndpointId, ChipLogValueMEI(aPath.mClusterId));
        return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::DataVersionMismatch);
    }

    if (auto * attrOverride = findAttributeAccessOverride(aPath.mEndpointId, aPath.mClusterId))
    {
        AttributeValueDecoder valueDecoder(aReader, aSubjectDescriptor);
        ReturnErrorOnFailure(attrOverride->Write(aPath, valueDecoder));

        if (valueDecoder.TriedDecode())
        {
            MatterReportingAttributeChangeCallback(aPath);
            return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::Success);
        }
    }

    CHIP_ERROR preparationError = CHIP_NO_ERROR;
    uint16_t dataLen            = 0;
    if ((preparationError = prepareWriteData(attributeMetadata, aReader, dataLen)) != CHIP_NO_ERROR)
    {
        ChipLogDetail(Zcl, "Failed to prepare data to write: %s", ErrorStr(preparationError));
        return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::InvalidValue);
    }

    if (dataLen > attributeMetadata->size)
    {
        ChipLogDetail(Zcl, "Data to write exceedes the attribute size claimed.");
        return apWriteHandler->AddStatus(aPath, Protocols::InteractionModel::Status::InvalidValue);
    }

    auto status = ToInteractionModelStatus(emberAfWriteAttributeExternal(aPath.mEndpointId, aPath.mClusterId, aPath.mAttributeId,
                                                                         CLUSTER_MASK_SERVER, attributeData,
                                                                         attributeMetadata->attributeType));
    return apWriteHandler->AddStatus(aPath, status);
}

bool IsClusterDataVersionEqual(EndpointId aEndpointId, ClusterId aClusterId, DataVersion aRequiredVersion)
{
    DataVersion * version = emberAfDataVersionStorage(aEndpointId, aClusterId);
    if (version == nullptr)
    {
        ChipLogError(DataManagement, "Endpoint %" PRIx16 ", Cluster " ChipLogFormatMEI " not found in IsClusterDataVersionEqual!",
                     aEndpointId, ChipLogValueMEI(aClusterId));
        return false;
    }
    else
    {
        return (*(version)) == aRequiredVersion;
    }
}

} // namespace app
} // namespace chip

void MatterReportingAttributeChangeCallback(EndpointId endpoint, ClusterId clusterId, AttributeId attributeId, uint8_t mask,
                                            EmberAfAttributeType type, uint8_t * data)
{
    IgnoreUnusedVariable(type);
    IgnoreUnusedVariable(data);
    IgnoreUnusedVariable(mask);

    MatterReportingAttributeChangeCallback(endpoint, clusterId, attributeId);
}

void MatterReportingAttributeChangeCallback(EndpointId endpoint, ClusterId clusterId, AttributeId attributeId)
{
    // Attribute writes have asserted this already, but this assert should catch
    // applications notifying about changes from their end.
    assertChipStackLockedByCurrentThread();

    ClusterInfo info;
    info.mClusterId   = clusterId;
    info.mAttributeId = attributeId;
    info.mEndpointId  = endpoint;

    IncreaseClusterDataVersion(endpoint, clusterId);
    InteractionModelEngine::GetInstance()->GetReportingEngine().SetDirty(info);

    // Schedule work to run asynchronously on the CHIP thread. The scheduled work won't execute until the current execution context
    // has completed. This ensures that we can 'gather up' multiple attribute changes that have occurred in the same execution
    // context without requiring any explicit 'start' or 'end' change calls into the engine to book-end the change.
    InteractionModelEngine::GetInstance()->GetReportingEngine().ScheduleRun();
}

void MatterReportingAttributeChangeCallback(const ConcreteAttributePath & aPath)
{
    return MatterReportingAttributeChangeCallback(aPath.mEndpointId, aPath.mClusterId, aPath.mAttributeId);
}
