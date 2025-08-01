#include <cmath>
#include <library/cpp/svnversion/svnversion.h>
#include <util/system/info.h>
#include <util/system/hostname.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/bridge.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/interconnect/interconnect.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>
#include <ydb/core/base/nameservice.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/util/cpuinfo.h>
#include <ydb/core/util/tuples.h>

#include <util/string/split.h>
#include <contrib/libs/protobuf/src/google/protobuf/util/message_differencer.h>

using namespace NActors;

namespace NKikimr {
namespace NNodeWhiteboard {

class TNodeWhiteboardService : public TActorBootstrapped<TNodeWhiteboardService> {
    struct TEvPrivate {
        enum EEv {
            EvUpdateRuntimeStats = EventSpaceBegin(TEvents::ES_PRIVATE),
            EvCleanupDeadTablets,
            EvSendListNodes,
            EvEnd
        };

        static_assert(EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE), "expected EvEnd < EventSpaceEnd");

        struct TEvUpdateRuntimeStats : TEventLocal<TEvUpdateRuntimeStats, EvUpdateRuntimeStats> {};
        struct TEvCleanupDeadTablets : TEventLocal<TEvCleanupDeadTablets, EvCleanupDeadTablets> {};
        struct TEvSendListNodes : TEventLocal<TEvSendListNodes, EvSendListNodes> {};
    };
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::NODE_WHITEBOARD_SERVICE;
    }

    void Bootstrap(const TActorContext &ctx) {
        TIntrusivePtr<::NMonitoring::TDynamicCounters> tabletsGroup = GetServiceCounters(AppData(ctx)->Counters, "tablets");
        TIntrusivePtr<::NMonitoring::TDynamicCounters> introspectionGroup = tabletsGroup->GetSubgroup("type", "introspection");
        TabletIntrospectionData.Reset(NTracing::CreateTraceCollection(introspectionGroup));

        SystemStateInfo.SetHost(FQDNHostName());
        if (const TString& nodeName = AppData(ctx)->NodeName; !nodeName.empty()) {
            SystemStateInfo.SetNodeName(nodeName);
        }
        SystemStateInfo.SetNumberOfCpus(NSystemInfo::NumberOfCpus());
        SystemStateInfo.SetRealNumberOfCpus(NKikimr::RealNumberOfCpus());
        auto version = GetProgramRevision();
        if (!version.empty()) {
            SystemStateInfo.SetVersion(version);
            TIntrusivePtr<NMonitoring::TDynamicCounters> utils = GetServiceCounters(AppData(ctx)->Counters, "utils");
            TIntrusivePtr<NMonitoring::TDynamicCounters> versionCounter = utils->GetSubgroup("revision", version);
            *versionCounter->GetCounter("version", false) = 1;
            TIntrusivePtr<NMonitoring::TDynamicCounters> nodeCounter = utils->GetSubgroup("NodeCount", version);
            *nodeCounter->GetCounter("NodeCount", false) = 1;
        }

        SystemStateInfo.SetStartTime(ctx.Now().MilliSeconds());
        ctx.Send(ctx.SelfID, new TEvPrivate::TEvUpdateRuntimeStats());

        auto utils = NKikimr::GetServiceCounters(NKikimr::AppData()->Counters, "utils");
        UserTime = utils->GetCounter("Process/UserTime", true);
        SysTime = utils->GetCounter("Process/SystemTime", true);
        MinorPageFaults = utils->GetCounter("Process/MinorPageFaults", true);
        MajorPageFaults = utils->GetCounter("Process/MajorPageFaults", true);
        NumThreads = utils->GetCounter("Process/NumThreads", false);
        auto group = utils->GetSubgroup("subsystem", "whiteboard");
        MaxClockSkewWithPeerUsCounter = group->GetCounter("MaxClockSkewWithPeerUs");
        MaxClockSkewPeerIdCounter = group->GetCounter("MaxClockSkewPeerId");

        ctx.Schedule(TDuration::Seconds(60), new TEvPrivate::TEvCleanupDeadTablets());
        Become(&TNodeWhiteboardService::StateFunc);
    }

protected:
    std::unordered_map<std::pair<TTabletId, TFollowerId>, NKikimrWhiteboard::TTabletStateInfo> TabletStateInfo;
    std::unordered_map<TString, NKikimrWhiteboard::TNodeStateInfo> NodeStateInfo;
    std::unordered_map<ui32, NKikimrWhiteboard::TPDiskStateInfo> PDiskStateInfo;
    std::unordered_map<TVDiskID, NKikimrWhiteboard::TVDiskStateInfo, THash<TVDiskID>> VDiskStateInfo;
    std::unordered_map<ui32, NKikimrWhiteboard::TBSGroupStateInfo> BSGroupStateInfo;

    bool IsBridgeCluster = false;
    NKikimrWhiteboard::TBridgeInfo BridgeInfo;
    NKikimrWhiteboard::TBridgeNodesInfo BridgeNodesInfo;
    TInstant BridgeInfoChangeTime;
    static constexpr TDuration SendListNodesPeriod = TDuration::Seconds(15);

    i64 MaxClockSkewWithPeerUs;
    ui32 MaxClockSkewPeerId;
    float MaxNetworkUtilization = 0.0;
    ui64 SumNetworkWriteThroughput = 0;
    NKikimrWhiteboard::TSystemStateInfo SystemStateInfo;
    THolder<NTracing::ITraceCollection> TabletIntrospectionData;

    NMonitoring::TDynamicCounters::TCounterPtr MaxClockSkewWithPeerUsCounter;
    NMonitoring::TDynamicCounters::TCounterPtr MaxClockSkewPeerIdCounter;
    NMonitoring::TDynamicCounters::TCounterPtr UserTime;
    ui64 SavedUserTime = 0;
    NMonitoring::TDynamicCounters::TCounterPtr SysTime;
    ui64 SavedSysTime = 0;
    NMonitoring::TDynamicCounters::TCounterPtr MinorPageFaults;
    ui64 SavedMinorPageFaults = 0;
    NMonitoring::TDynamicCounters::TCounterPtr MajorPageFaults;
    ui64 SavedMajorPageFaults = 0;
    NMonitoring::TDynamicCounters::TCounterPtr NumThreads;

    TSystemThreadsMonitor ThreadsMonitor;

    template <typename PropertyType>
    static ui64 GetDifference(PropertyType a, PropertyType b) {
        return static_cast<ui64>(std::abs(static_cast<std::make_signed_t<PropertyType>>(b) -
                                          static_cast<std::make_signed_t<PropertyType>>(a)));
    }

    static TString GetProgramRevision() {
        TString version = GetTag();
        if (version.empty()) {
            version = GetBranch();
        }

        if (!version.empty() && version.StartsWith("tags/releases/")) {
            TVector<TString> parts = StringSplitter(version).Split('/');
            auto rIt = parts.rbegin();
            if (rIt == parts.rend())
                return {};

            version = *rIt;
            rIt++;

            if (rIt != parts.rend() && !rIt->empty()) {
                version = (*rIt) + '-' + version;
            }

            return version;
        }

        version = GetBranch();
        auto pos = version.rfind('/');
        if (pos != TString::npos) {
            version = version.substr(pos + 1);
        }

        TString commitId = GetProgramCommitId();
        if (!commitId.empty()) {
            if (commitId.size() > 7) {
                commitId = commitId.substr(0, 7);
            }

            version = version + '.' + commitId;
        }

        return version;
    }

    static ui64 GetDifference(double a, double b) {
        return static_cast<ui64>(std::fabs(b - a));
    }

    static ui64 GetDifference(float a, float b) {
        return static_cast<ui64>(std::fabs(b - a));
    }

    static ui64 GetDifference(bool a, bool b) {
        return static_cast<ui64>(std::abs(static_cast<int>(b) - static_cast<int>(a)));
    }

    template <typename PropertyType>
    static int MergeProtoField(
            const ::google::protobuf::Reflection& reflectionTo,
            const ::google::protobuf::Reflection& reflectionFrom,
            ::google::protobuf::Message& protoTo,
            const ::google::protobuf::Message& protoFrom,
            const ::google::protobuf::FieldDescriptor* field,
            PropertyType (::google::protobuf::Reflection::* getter)(const ::google::protobuf::Message&, const ::google::protobuf::FieldDescriptor*) const,
            void (::google::protobuf::Reflection::* setter)(::google::protobuf::Message*, const ::google::protobuf::FieldDescriptor*, PropertyType) const,
            PropertyType defaultVal) {
        int modified = 0;
        bool has = reflectionTo.HasField(protoTo, field);
        PropertyType newVal = (reflectionFrom.*getter)(protoFrom, field);
        if (!has) {
            if (field->has_default_value() && newVal == defaultVal) {
                reflectionTo.ClearField(&protoTo, field);
            } else {
                (reflectionTo.*setter)(&protoTo, field, newVal);
            }
            modified = 100;
        } else {
            PropertyType oldVal = (reflectionTo.*getter)(protoTo, field);
            if (oldVal != newVal) {
                if (field->has_default_value() && newVal == defaultVal) {
                    reflectionTo.ClearField(&protoTo, field);
                } else {
                    (reflectionTo.*setter)(&protoTo, field, newVal);
                }
                const auto& options(field->options());
                if (options.HasExtension(NKikimrWhiteboard::InsignificantChangeAmount)) {
                    ui64 insignificantChangeAmount = options.GetExtension(NKikimrWhiteboard::InsignificantChangeAmount);
                    if (GetDifference(oldVal, newVal) > insignificantChangeAmount) {
                        modified = 100;
                    }
                } else if (options.HasExtension(NKikimrWhiteboard::InsignificantChangePercent)) {
                    ui32 insignificantChangePercent = options.GetExtension(NKikimrWhiteboard::InsignificantChangePercent);
                    if (oldVal != PropertyType() && GetDifference(oldVal, newVal) * 100 / oldVal > insignificantChangePercent) {
                        modified = 100;
                    }
                } else {
                    modified = 100;
                }
            }
        }
        return modified;
    }

    static int CheckedMerge(::google::protobuf::Message& protoTo, const ::google::protobuf::Message& protoFrom) {
        using namespace ::google::protobuf;
        int modified = 0;
        const Descriptor& descriptor = *protoTo.GetDescriptor();
        const Reflection& reflectionTo = *protoTo.GetReflection();
        const Reflection& reflectionFrom = *protoFrom.GetReflection();
        int fieldCount = descriptor.field_count();
        for (int index = 0; index < fieldCount; ++index) {
            const FieldDescriptor* field = descriptor.field(index);
            if (field->is_repeated()) {
                FieldDescriptor::CppType type = field->cpp_type();
                int size = reflectionFrom.FieldSize(protoFrom, field);
                if (size != 0 && reflectionTo.FieldSize(protoTo, field) != size) {
                    reflectionTo.ClearField(&protoTo, field);
                    for (int i = 0; i < size; ++i) {
                        switch (type) {
                        case FieldDescriptor::CPPTYPE_INT32:
                            reflectionTo.AddInt32(&protoTo, field, reflectionFrom.GetRepeatedInt32(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_INT64:
                            reflectionTo.AddInt64(&protoTo, field, reflectionFrom.GetRepeatedInt64(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_UINT32:
                            reflectionTo.AddUInt32(&protoTo, field, reflectionFrom.GetRepeatedUInt32(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_UINT64:
                            reflectionTo.AddUInt64(&protoTo, field, reflectionFrom.GetRepeatedUInt64(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_DOUBLE:
                            reflectionTo.AddDouble(&protoTo, field, reflectionFrom.GetRepeatedDouble(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_FLOAT:
                            reflectionTo.AddFloat(&protoTo, field, reflectionFrom.GetRepeatedFloat(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_BOOL:
                            reflectionTo.AddBool(&protoTo, field, reflectionFrom.GetRepeatedBool(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_ENUM:
                            reflectionTo.AddEnum(&protoTo, field, reflectionFrom.GetRepeatedEnum(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_STRING:
                            reflectionTo.AddString(&protoTo, field, reflectionFrom.GetRepeatedString(protoFrom, field, i));
                            break;
                        case FieldDescriptor::CPPTYPE_MESSAGE:
                            reflectionTo.AddMessage(&protoTo, field)->CopyFrom(reflectionFrom.GetRepeatedMessage(protoFrom, field, i));
                            break;
                        }
                    }
                    modified += 100;
                } else {
                    for (int i = 0; i < size; ++i) {
                        switch (type) {
                        case FieldDescriptor::CPPTYPE_INT32: {
                            auto val = reflectionFrom.GetRepeatedInt32(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedInt32(protoTo, field, i)) {
                                reflectionTo.SetRepeatedInt32(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_INT64: {
                            auto val = reflectionFrom.GetRepeatedInt64(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedInt64(protoTo, field, i)) {
                                reflectionTo.SetRepeatedInt64(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_UINT32: {
                            auto val = reflectionFrom.GetRepeatedUInt32(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedUInt32(protoTo, field, i)) {
                                reflectionTo.SetRepeatedUInt32(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_UINT64: {
                            auto val = reflectionFrom.GetRepeatedUInt64(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedUInt64(protoTo, field, i)) {
                                reflectionTo.SetRepeatedUInt64(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_DOUBLE: {
                            auto val = reflectionFrom.GetRepeatedDouble(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedDouble(protoTo, field, i)) {
                                reflectionTo.SetRepeatedDouble(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_FLOAT: {
                            auto val = reflectionFrom.GetRepeatedFloat(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedFloat(protoTo, field, i)) {
                                reflectionTo.SetRepeatedFloat(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_BOOL: {
                            auto val = reflectionFrom.GetRepeatedBool(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedBool(protoTo, field, i)) {
                                reflectionTo.SetRepeatedBool(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_ENUM: {
                            auto val = reflectionFrom.GetRepeatedEnum(protoFrom, field, i);
                            if (val->number() != reflectionTo.GetRepeatedEnum(protoTo, field, i)->number()) {
                                reflectionTo.SetRepeatedEnum(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_STRING: {
                            auto val = reflectionFrom.GetRepeatedString(protoFrom, field, i);
                            if (val != reflectionTo.GetRepeatedString(protoTo, field, i)) {
                                reflectionTo.SetRepeatedString(&protoTo, field, i, val);
                                modified += 100;
                            }
                            break;
                        }
                        case FieldDescriptor::CPPTYPE_MESSAGE:
                            modified += CheckedMerge(*reflectionTo.MutableRepeatedMessage(&protoTo, field, i), reflectionFrom.GetRepeatedMessage(protoFrom, field, i));
                            break;
                        }
                    }
                }
            } else {
                if (reflectionFrom.HasField(protoFrom, field)) {
                    FieldDescriptor::CppType type = field->cpp_type();
                    switch (type) {
                    case FieldDescriptor::CPPTYPE_INT32: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetInt32, &Reflection::SetInt32, field->default_value_int32());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_INT64: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetInt64, &Reflection::SetInt64, field->default_value_int64());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_UINT32: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetUInt32, &Reflection::SetUInt32, field->default_value_uint32());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_UINT64: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetUInt64, &Reflection::SetUInt64, field->default_value_uint64());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_DOUBLE: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetDouble, &Reflection::SetDouble, field->default_value_double());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_FLOAT: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetFloat, &Reflection::SetFloat, field->default_value_float());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_BOOL: {
                        modified += MergeProtoField(reflectionTo, reflectionFrom, protoTo, protoFrom, field, &Reflection::GetBool, &Reflection::SetBool, field->default_value_bool());
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_ENUM: {
                        bool has = reflectionTo.HasField(protoTo, field);
                        auto val = reflectionFrom.GetEnum(protoFrom, field);
                        if (!has || reflectionTo.GetEnum(protoTo, field)->number() != val->number()) {
                            if (field->has_default_value() && val->number() == field->default_value_enum()->number()) {
                                reflectionTo.ClearField(&protoTo, field);
                            } else {
                                reflectionTo.SetEnum(&protoTo, field, val);
                            }
                            modified += 100;
                        }
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_STRING: {
                        bool has = reflectionTo.HasField(protoTo, field);
                        auto val = reflectionFrom.GetString(protoFrom, field);
                        if (!has || reflectionTo.GetString(protoTo, field) != val) {
                            if (field->has_default_value() && field->default_value_string() == val) {
                                reflectionTo.ClearField(&protoTo, field);
                            } else {
                                reflectionTo.SetString(&protoTo, field, val);
                            }
                            modified += 100;
                        }
                        break;
                    }
                    case FieldDescriptor::CPPTYPE_MESSAGE:
                        modified += CheckedMerge(*reflectionTo.MutableMessage(&protoTo, field), reflectionFrom.GetMessage(protoFrom, field));
                        break;
                    }
                }
            }
        }
        return modified;
    }

    static void CopyField(::google::protobuf::Message& protoTo,
                          const ::google::protobuf::Message& protoFrom,
                          const ::google::protobuf::Reflection& reflectionTo,
                          const ::google::protobuf::Reflection& reflectionFrom,
                          const ::google::protobuf::FieldDescriptor* field) {
        using namespace ::google::protobuf;
        if (field->is_repeated()) {
            FieldDescriptor::CppType type = field->cpp_type();
            int size = reflectionFrom.FieldSize(protoFrom, field);
            if (size != 0) {
                reflectionTo.ClearField(&protoTo, field);
                for (int i = 0; i < size; ++i) {
                    switch (type) {
                    case FieldDescriptor::CPPTYPE_INT32:
                        reflectionTo.AddInt32(&protoTo, field, reflectionFrom.GetRepeatedInt32(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_INT64:
                        reflectionTo.AddInt64(&protoTo, field, reflectionFrom.GetRepeatedInt64(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_UINT32:
                        reflectionTo.AddUInt32(&protoTo, field, reflectionFrom.GetRepeatedUInt32(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_UINT64:
                        reflectionTo.AddUInt64(&protoTo, field, reflectionFrom.GetRepeatedUInt64(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_DOUBLE:
                        reflectionTo.AddDouble(&protoTo, field, reflectionFrom.GetRepeatedDouble(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_FLOAT:
                        reflectionTo.AddFloat(&protoTo, field, reflectionFrom.GetRepeatedFloat(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_BOOL:
                        reflectionTo.AddBool(&protoTo, field, reflectionFrom.GetRepeatedBool(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_ENUM:
                        reflectionTo.AddEnum(&protoTo, field, reflectionFrom.GetRepeatedEnum(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_STRING:
                        reflectionTo.AddString(&protoTo, field, reflectionFrom.GetRepeatedString(protoFrom, field, i));
                        break;
                    case FieldDescriptor::CPPTYPE_MESSAGE:
                        reflectionTo.AddMessage(&protoTo, field)->CopyFrom(reflectionFrom.GetRepeatedMessage(protoFrom, field, i));
                        break;
                    }
                }
            }
        } else {
            if (reflectionFrom.HasField(protoFrom, field)) {
                FieldDescriptor::CppType type = field->cpp_type();
                switch (type) {
                case FieldDescriptor::CPPTYPE_INT32:
                    reflectionTo.SetInt32(&protoTo, field, reflectionFrom.GetInt32(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_INT64:
                    reflectionTo.SetInt64(&protoTo, field, reflectionFrom.GetInt64(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_UINT32:
                    reflectionTo.SetUInt32(&protoTo, field, reflectionFrom.GetUInt32(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_UINT64:
                    reflectionTo.SetUInt64(&protoTo, field, reflectionFrom.GetUInt64(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_DOUBLE:
                    reflectionTo.SetDouble(&protoTo, field, reflectionFrom.GetDouble(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_FLOAT:
                    reflectionTo.SetFloat(&protoTo, field, reflectionFrom.GetFloat(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_BOOL:
                    reflectionTo.SetBool(&protoTo, field, reflectionFrom.GetBool(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_ENUM:
                    reflectionTo.SetEnum(&protoTo, field, reflectionFrom.GetEnum(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_STRING:
                    reflectionTo.SetString(&protoTo, field, reflectionFrom.GetString(protoFrom, field));
                    break;
                case FieldDescriptor::CPPTYPE_MESSAGE:
                    reflectionTo.MutableMessage(&protoTo, field)->CopyFrom(reflectionFrom.GetMessage(protoFrom, field));
                    break;
                }
            }
        }
    }

    static void SelectiveCopy(::google::protobuf::Message& protoTo, const ::google::protobuf::Message& protoFrom, const ::google::protobuf::RepeatedField<int>& fields) {
        using namespace ::google::protobuf;
        const Descriptor& descriptor = *protoTo.GetDescriptor();
        const Reflection& reflectionTo = *protoTo.GetReflection();
        const Reflection& reflectionFrom = *protoFrom.GetReflection();
        for (auto fieldNumber : fields) {
            const FieldDescriptor* field = descriptor.FindFieldByNumber(fieldNumber);
            if (field) {
                CopyField(protoTo, protoFrom, reflectionTo, reflectionFrom, field);
            }
        }
    }

    template<typename TMessage, typename TRequest>
    static void Copy(TMessage& to, const TMessage& from, const TRequest& request) {
        if (request.FieldsRequiredSize() > 0) {
            if (request.FieldsRequiredSize() == 1 && request.GetFieldsRequired(0) == -1) { // all fields
                to.CopyFrom(from);
            } else {
                SelectiveCopy(to, from, request.GetFieldsRequired());
            }
        } else {
            SelectiveCopy(to, from, GetDefaultWhiteboardFields<TMessage>());
        }
    }

    void SetRole(TStringBuf roleName) {
        for (const auto& role : SystemStateInfo.GetRoles()) {
            if (role == roleName) {
                return;
            }
        }
        SystemStateInfo.AddRoles(TString(roleName));
        SystemStateInfo.SetChangeTime(TActivationContext::Now().MilliSeconds());
    }

    STRICT_STFUNC(StateFunc,
        HFunc(TEvWhiteboard::TEvTabletStateUpdate, Handle);
        HFunc(TEvWhiteboard::TEvTabletStateRequest, Handle);
        HFunc(TEvWhiteboard::TEvNodeStateUpdate, Handle);
        HFunc(TEvWhiteboard::TEvNodeStateDelete, Handle);
        HFunc(TEvWhiteboard::TEvNodeStateRequest, Handle);
        HFunc(TEvWhiteboard::TEvPDiskStateUpdate, Handle);
        HFunc(TEvWhiteboard::TEvPDiskStateRequest, Handle);
        HFunc(TEvWhiteboard::TEvPDiskStateDelete, Handle);
        HFunc(TEvWhiteboard::TEvVDiskStateUpdate, Handle);
        HFunc(TEvWhiteboard::TEvVDiskStateGenerationChange, Handle);
        HFunc(TEvWhiteboard::TEvVDiskStateDelete, Handle);
        HFunc(TEvWhiteboard::TEvVDiskStateRequest, Handle);
        HFunc(TEvWhiteboard::TEvVDiskDropDonors, Handle);
        HFunc(TEvWhiteboard::TEvBSGroupStateUpdate, Handle);
        HFunc(TEvWhiteboard::TEvBSGroupStateDelete, Handle);
        HFunc(TEvWhiteboard::TEvBSGroupStateRequest, Handle);
        HFunc(TEvWhiteboard::TEvSystemStateUpdate, Handle);
        HFunc(TEvWhiteboard::TEvMemoryStatsUpdate, Handle);
        HFunc(TEvWhiteboard::TEvSystemStateAddEndpoint, Handle);
        HFunc(TEvWhiteboard::TEvSystemStateAddRole, Handle);
        HFunc(TEvWhiteboard::TEvSystemStateSetTenant, Handle);
        HFunc(TEvWhiteboard::TEvSystemStateRemoveTenant, Handle);
        HFunc(TEvWhiteboard::TEvSystemStateRequest, Handle);
        hFunc(TEvWhiteboard::TEvIntrospectionData, Handle);
        HFunc(TEvWhiteboard::TEvTabletLookupRequest, Handle);
        HFunc(TEvWhiteboard::TEvTraceLookupRequest, Handle);
        HFunc(TEvWhiteboard::TEvTraceRequest, Handle);
        HFunc(TEvWhiteboard::TEvSignalBodyRequest, Handle);
        HFunc(TEvWhiteboard::TEvBridgeInfoUpdate, Handle);
        HFunc(TEvInterconnect::TEvNodesInfo, Handle);
        HFunc(TEvWhiteboard::TEvBridgeInfoRequest, Handle);
        HFunc(TEvPrivate::TEvSendListNodes, Handle);
        HFunc(TEvPrivate::TEvUpdateRuntimeStats, Handle);
        HFunc(TEvPrivate::TEvCleanupDeadTablets, Handle);
    )

    void Handle(TEvWhiteboard::TEvTabletStateUpdate::TPtr &ev, const TActorContext &ctx) {
        auto tabletId(std::make_pair(ev->Get()->Record.GetTabletId(), ev->Get()->Record.GetFollowerId()));
        auto& tabletStateInfo = TabletStateInfo[tabletId];
        if (ev->Get()->Record.HasGeneration() && tabletStateInfo.GetGeneration() > ev->Get()->Record.GetGeneration()) {
            return; // skip updates from previous generations
        }
        if (CheckedMerge(tabletStateInfo, ev->Get()->Record) >= 100) {
            tabletStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
        }
    }

    bool ShouldReportClockSkew(const NKikimrWhiteboard::TNodeStateInfo &info, const TActorContext &ctx) {
        if (!info.GetSameScope()) {
            return false;
        }
        if (!IsBridgeMode(ctx)) {
            return true;
        }
        return SystemStateInfo.GetLocation().GetBridgePileName() == info.GetPeerBridgePileName();
    }

    void Handle(TEvWhiteboard::TEvNodeStateUpdate::TPtr &ev, const TActorContext &ctx) {
        auto& nodeStateInfo = NodeStateInfo[ev->Get()->Record.GetPeerName()];
        ui64 previousChangeTime = nodeStateInfo.GetChangeTime();
        ui64 currentChangeTime = ctx.Now().MilliSeconds();
        ui64 previousBytesWritten = nodeStateInfo.GetBytesWritten();
        ui64 currentBytesWritten = ev->Get()->Record.GetBytesWritten();
        if (previousChangeTime && previousBytesWritten < currentBytesWritten && previousChangeTime < currentChangeTime) {
            nodeStateInfo.SetWriteThroughput((currentBytesWritten - previousBytesWritten) * 1000 / (currentChangeTime - previousChangeTime));
        } else {
            nodeStateInfo.ClearWriteThroughput();
        }
        if (ShouldReportClockSkew(ev->Get()->Record, ctx)) {
            i64 skew = ev->Get()->Record.GetClockSkewUs();
            if (abs(skew) > abs(MaxClockSkewWithPeerUs)) {
                MaxClockSkewWithPeerUs = skew;
                MaxClockSkewPeerId = ev->Get()->Record.GetPeerNodeId();
            }
        }
        // TODO: need better way to calculate network utilization
        MaxNetworkUtilization = std::max(MaxNetworkUtilization, ev->Get()->Record.GetUtilization());
        SumNetworkWriteThroughput += nodeStateInfo.GetWriteThroughput();
        nodeStateInfo.MergeFrom(ev->Get()->Record);
        nodeStateInfo.SetChangeTime(currentChangeTime);
    }

    void Handle(TEvWhiteboard::TEvNodeStateDelete::TPtr &ev, const TActorContext &ctx) {
        auto& nodeStateInfo = NodeStateInfo[ev->Get()->Record.GetPeerName()];
        if (nodeStateInfo.HasConnected()) {
            nodeStateInfo.ClearConnected();
            nodeStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
        }
    }

    void Handle(TEvWhiteboard::TEvPDiskStateUpdate::TPtr &ev, const TActorContext &ctx) {
        auto& pDiskStateInfo = PDiskStateInfo[ev->Get()->Record.GetPDiskId()];
        if (CheckedMerge(pDiskStateInfo, ev->Get()->Record) >= 100) {
            pDiskStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
        }
        SetRole("Storage");
    }

    void Handle(TEvWhiteboard::TEvVDiskStateUpdate::TPtr &ev, const TActorContext &ctx) {
        auto& record = ev->Get()->Record;
        const auto& key = VDiskIDFromVDiskID(record.GetVDiskId());
        if (ev->Get()->Initial) {
            auto& value = VDiskStateInfo[key];
            value = record;
            value.SetChangeTime(ctx.Now().MilliSeconds());
        } else if (const auto it = VDiskStateInfo.find(key); it != VDiskStateInfo.end() &&
                it->second.GetInstanceGuid() == record.GetInstanceGuid()) {
            auto& value = it->second;

            if (CheckedMerge(value, record) >= 100) {
                value.SetChangeTime(ctx.Now().MilliSeconds());
            }
        }
    }

    void Handle(TEvWhiteboard::TEvVDiskStateDelete::TPtr &ev, const TActorContext &) {
        VDiskStateInfo.erase(VDiskIDFromVDiskID(ev->Get()->Record.GetVDiskId()));
    }

    void Handle(TEvWhiteboard::TEvVDiskStateGenerationChange::TPtr &ev, const TActorContext &) {
        auto *msg = ev->Get();
        if (const auto it = VDiskStateInfo.find(msg->VDiskId); it != VDiskStateInfo.end() &&
                it->second.GetInstanceGuid() == msg->InstanceGuid) {
            auto node = VDiskStateInfo.extract(it);
            node.key().GroupGeneration = msg->Generation;
            VDiskStateInfo.insert(std::move(node));
        }
    }

    void Handle(TEvWhiteboard::TEvVDiskDropDonors::TPtr& ev, const TActorContext& ctx) {
        auto& msg = *ev->Get();
        if (const auto it = VDiskStateInfo.find(msg.VDiskId); it != VDiskStateInfo.end() &&
                it->second.GetInstanceGuid() == msg.InstanceGuid) {
            auto& value = it->second;
            bool change = false;

            if (msg.DropAllDonors) {
                change = !value.GetDonors().empty();
                value.ClearDonors();
            } else {
                for (const auto& donor : msg.DropDonors) {
                    auto *donors = value.MutableDonors();
                    for (int i = 0; i < donors->size(); ++i) {
                        auto& x = donors->at(i);
                        if (x.GetNodeId() == donor.GetNodeId() && x.GetPDiskId() == donor.GetPDiskId() && x.GetVSlotId() == donor.GetVSlotId()) {
                            donors->DeleteSubrange(i, 1);
                            change = true;
                            break;
                        }
                    }
                }
            }

            if (change) {
                value.SetChangeTime(ctx.Now().MilliSeconds());
            }
        }
    }

    void Handle(TEvWhiteboard::TEvBSGroupStateUpdate::TPtr &ev, const TActorContext &ctx) {
        const auto& from = ev->Get()->Record;
        auto& to = BSGroupStateInfo[from.GetGroupID()];
        int modified = 0;
        if (from.GetNoVDisksInGroup() && to.GetGroupGeneration() <= from.GetGroupGeneration()) {
            modified += 100 * (2 - to.GetVDiskIds().empty() - to.GetVDiskNodeIds().empty());
            to.ClearVDiskIds();
            to.ClearVDiskNodeIds();
        }
        modified += CheckedMerge(to, from);
        if (modified >= 100) {
            to.SetChangeTime(ctx.Now().MilliSeconds());
        }
    }

    void Handle(TEvWhiteboard::TEvBSGroupStateDelete::TPtr &ev, const TActorContext &) {
        ui32 groupId = ev->Get()->Record.GetGroupID();
        BSGroupStateInfo.erase(groupId);
    }

    void Handle(TEvWhiteboard::TEvBridgeInfoUpdate::TPtr &ev, const TActorContext &ctx) {
        BridgeInfo.Swap(&ev->Get()->Record);
        BridgeInfoChangeTime = ctx.Now();
        if (!IsBridgeCluster) {
            ctx.Send(SelfId(), new TEvPrivate::TEvSendListNodes);
        }
        IsBridgeCluster = true;
    }

    void Handle(TEvInterconnect::TEvNodesInfo::TPtr &ev, const TActorContext &ctx) {
        NKikimrWhiteboard::TBridgeNodesInfo newInfo;
        const auto& pileMap = ev->Get()->PileMap;
        if (!pileMap) {
            return;
        }
        for (const auto& pile : *pileMap) {
            auto* pileInfo = newInfo.MutablePiles()->Add();
            for (const auto nodeId : pile) {
                pileInfo->MutableNodeIds()->Add(nodeId);
            }
        }
        if (!google::protobuf::util::MessageDifferencer::Equals(newInfo, BridgeNodesInfo)) {
            BridgeNodesInfo.Swap(&newInfo);
            BridgeInfoChangeTime = ctx.Now();
        }
    }

    void Handle(TEvPrivate::TEvSendListNodes::TPtr &, const TActorContext &ctx) {
        ctx.Send(GetNameserviceActorId(), new TEvInterconnect::TEvListNodes);
        ctx.Schedule(SendListNodesPeriod, new TEvPrivate::TEvSendListNodes);
    }

    void Handle(TEvWhiteboard::TEvSystemStateUpdate::TPtr &ev, const TActorContext &ctx) {
        if (CheckedMerge(SystemStateInfo, ev->Get()->Record)) {
            SystemStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
        }
    }

    void Handle(TEvWhiteboard::TEvMemoryStatsUpdate::TPtr &ev, const TActorContext &ctx) {
        const auto& memoryStats = ev->Get()->Record;

        // Note: copy stats to sys info fields for backward compatibility
        if (memoryStats.HasAnonRss()) {
            SystemStateInfo.SetMemoryUsed(memoryStats.GetAnonRss());
        } else if (memoryStats.HasAllocatedMemory()) {
            SystemStateInfo.SetMemoryUsed(memoryStats.GetAllocatedMemory());
        } else {
            SystemStateInfo.ClearMemoryUsed();
        }
        if (memoryStats.HasAllocatedMemory()) {
            SystemStateInfo.SetMemoryUsedInAlloc(memoryStats.GetAllocatedMemory());
        } else {
            SystemStateInfo.ClearMemoryUsedInAlloc();
        }
        if (memoryStats.HasSharedCacheConsumption()) {
            SystemStateInfo.MutableSharedCacheStats()->SetUsedBytes(memoryStats.GetSharedCacheConsumption());
        } else {
            SystemStateInfo.MutableSharedCacheStats()->ClearUsedBytes();
        }
        if (memoryStats.HasSharedCacheLimit()) {
            SystemStateInfo.MutableSharedCacheStats()->SetLimitBytes(memoryStats.GetSharedCacheLimit());
        } else {
            SystemStateInfo.MutableSharedCacheStats()->ClearLimitBytes();
        }

        SystemStateInfo.MutableMemoryStats()->Swap(&ev->Get()->Record);

        // Note: there is no big reason (and an easy way) to compare the previous and the new memory stats
        // and allocated memory stat is expected to change every time
        // so always update change time unconditionally
        SystemStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
    }

    void Handle(TEvWhiteboard::TEvSystemStateAddEndpoint::TPtr &ev, const TActorContext &ctx) {
        auto& endpoint = *SystemStateInfo.AddEndpoints();
        endpoint.SetName(ev->Get()->Name);
        endpoint.SetAddress(ev->Get()->Address);
        std::sort(SystemStateInfo.MutableEndpoints()->begin(), SystemStateInfo.MutableEndpoints()->end(), [](const auto& a, const auto& b) {
            return a.GetName() < b.GetName();
        });
        SystemStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
    }

    void Handle(TEvWhiteboard::TEvSystemStateAddRole::TPtr &ev, const TActorContext &ctx) {
        const auto& roles = SystemStateInfo.GetRoles();
        if (Find(roles, ev->Get()->Role) == roles.end()) {
            SystemStateInfo.AddRoles(ev->Get()->Role);
            SystemStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
        }
    }

    void Handle(TEvWhiteboard::TEvSystemStateSetTenant::TPtr &ev, const TActorContext &ctx) {
        const auto& tenants = SystemStateInfo.GetTenants();
        if (Find(tenants, ev->Get()->Tenant) == tenants.end()) {
            SystemStateInfo.ClearTenants();
            SystemStateInfo.AddTenants(ev->Get()->Tenant);
            SystemStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
            SetRole("Tenant");
        }
    }

    void Handle(TEvWhiteboard::TEvSystemStateRemoveTenant::TPtr &ev, const TActorContext &ctx) {
        auto& tenants = *SystemStateInfo.MutableTenants();
        auto itTenant = Find(tenants, ev->Get()->Tenant);
        if (itTenant != tenants.end()) {
            tenants.erase(itTenant);
            SystemStateInfo.SetChangeTime(ctx.Now().MilliSeconds());
        }
    }

    void UpdateSystemState() {
        NKikimrWhiteboard::EFlag eFlag = NKikimrWhiteboard::EFlag::Green;
        ui32 badDisks = 0;
        double maxDiskUsage = 0;
        for (const auto& pr : PDiskStateInfo) {
            if (pr.second.HasState()) {
                switch (pr.second.GetState()) {
                case NKikimrBlobStorage::TPDiskState::InitialFormatReadError:
                case NKikimrBlobStorage::TPDiskState::InitialSysLogReadError:
                case NKikimrBlobStorage::TPDiskState::InitialSysLogParseError:
                case NKikimrBlobStorage::TPDiskState::InitialCommonLogReadError:
                case NKikimrBlobStorage::TPDiskState::InitialCommonLogParseError:
                case NKikimrBlobStorage::TPDiskState::CommonLoggerInitError:
                case NKikimrBlobStorage::TPDiskState::OpenFileError:
                    eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Yellow);
                    ++badDisks;
                    break;
                default:
                    break;
                }
            }
            if (pr.second.HasAvailableSize() && pr.second.GetTotalSize() != 0) {
                double avail = (double)pr.second.GetAvailableSize() / pr.second.GetTotalSize();
                if (avail <= 0.04) {
                    eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Orange);
                } else if (avail <= 0.08) {
                    eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Yellow);
                }
                maxDiskUsage = std::max(maxDiskUsage, 1.0 - avail);
            }
        }
        if (PDiskStateInfo.size() > 0) {
            SystemStateInfo.SetMaxDiskUsage(maxDiskUsage);
        }
        if (eFlag == NKikimrWhiteboard::EFlag::Yellow && badDisks > 1) {
            eFlag = NKikimrWhiteboard::EFlag::Orange;
        }
        for (const auto& pr : VDiskStateInfo) {
            eFlag = std::max(eFlag, pr.second.GetDiskSpace());
            if (pr.second.GetDiskSpace() >= NKikimrWhiteboard::EFlag::Red) {
                eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Orange);
            } else if (pr.second.GetDiskSpace() > NKikimrWhiteboard::EFlag::Green) {
                eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Yellow);
            }
            if (pr.second.GetSatisfactionRank().GetFreshRank().GetFlag() > NKikimrWhiteboard::EFlag::Green) {
                eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Yellow);
            }
            if (pr.second.GetSatisfactionRank().GetLevelRank().GetFlag() > NKikimrWhiteboard::EFlag::Green) {
                eFlag = std::max(eFlag, NKikimrWhiteboard::EFlag::Yellow);
            }
        }
        if (SystemStateInfo.HasGRpcState()) {
            eFlag = std::max(eFlag, std::max(SystemStateInfo.GetGRpcState(), NKikimrWhiteboard::EFlag::Orange));
        }
        for (const auto& stats : SystemStateInfo.GetPoolStats()) {
            double usage = stats.GetUsage();
            NKikimrWhiteboard::EFlag flag = NKikimrWhiteboard::EFlag::Grey;
            if (usage >= 0.99) {
                flag = NKikimrWhiteboard::EFlag::Red;
            } else if (usage >= 0.95) {
                flag = NKikimrWhiteboard::EFlag::Orange;
            } else if (usage >= 0.90) {
                flag = NKikimrWhiteboard::EFlag::Yellow;
            } else  {
                flag = NKikimrWhiteboard::EFlag::Green;
            }
            if (stats.GetName() == "User") {
                flag = std::min(flag, NKikimrWhiteboard::EFlag::Orange);
            } else if (stats.GetName() == "IO") {
                flag = std::min(flag, NKikimrWhiteboard::EFlag::Yellow);
            } else if (stats.GetName() == "Batch") {
                flag = std::min(flag, NKikimrWhiteboard::EFlag::Green);
            }
            eFlag = std::max(eFlag, flag);
        }
        if (!SystemStateInfo.HasSystemState() || SystemStateInfo.GetSystemState() != eFlag) {
            SystemStateInfo.SetSystemState(eFlag);
            SystemStateInfo.SetChangeTime(TActivationContext::Now().MilliSeconds());
        }
    }

    static std::unordered_set<TTabletId> BuildIndex(const ::google::protobuf::RepeatedField<::NProtoBuf::uint64>& array) {
        std::unordered_set<TTabletId> result;
        result.reserve(array.size());
        for (auto id : array) {
            result.insert(id);
        }
        return result;
    }

    void Handle(TEvWhiteboard::TEvTabletStateRequest::TPtr &ev, const TActorContext &ctx) {
        auto now = TMonotonic::Now();
        const auto& request = ev->Get()->Record;
        auto matchesFilter = [
            changedSince = request.has_changedsince() ? request.changedsince() : 0,
            filterTabletId = BuildIndex(request.filtertabletid()),
            filterTenantId = request.has_filtertenantid() ? NKikimr::TSubDomainKey(request.filtertenantid()) : NKikimr::TSubDomainKey()
        ](const NKikimrWhiteboard::TTabletStateInfo& tabletStateInfo) {
            return tabletStateInfo.changetime() >= changedSince
                && (filterTabletId.empty() || filterTabletId.count(tabletStateInfo.tabletid()))
                && (!filterTenantId || filterTenantId == NKikimr::TSubDomainKey(tabletStateInfo.tenantid()));
        };
        std::unique_ptr<TEvWhiteboard::TEvTabletStateResponse> response = std::make_unique<TEvWhiteboard::TEvTabletStateResponse>();
        auto& record = response->Record;
        if (request.format() == "packed5") {
            std::vector<const NKikimrWhiteboard::TTabletStateInfo*> matchedTablets;
            for (const auto& [tabletId, tabletInfo] : TabletStateInfo) {
                if (matchesFilter(tabletInfo)) {
                    matchedTablets.push_back(&tabletInfo);
                }
            }
            TEvWhiteboard::TEvTabletStateResponsePacked5* ptr = response->AllocatePackedResponse(matchedTablets.size());
            for (auto tabletInfo : matchedTablets) {
                ptr->TabletId = tabletInfo->tabletid();
                ptr->FollowerId = tabletInfo->followerid();
                ptr->Generation = tabletInfo->generation();
                ptr->Type = tabletInfo->type();
                ptr->State = tabletInfo->state();
                ++ptr;
            }
        } else {
            if (request.groupby().empty()) {
                for (const auto& pr : TabletStateInfo) {
                    if (matchesFilter(pr.second)) {
                        NKikimrWhiteboard::TTabletStateInfo& tabletStateInfo = *record.add_tabletstateinfo();
                        Copy(tabletStateInfo, pr.second, request);
                    }
                }
            } else if (request.groupby() == "Type,State" || request.groupby() == "NodeId,Type,State") { // the only supported group-by for now
                std::unordered_map<std::pair<NKikimrTabletBase::TTabletTypes::EType,
                    NKikimrWhiteboard::TTabletStateInfo::ETabletState>, NKikimrWhiteboard::TTabletStateInfo> stateGroupBy;
                for (const auto& [id, stateInfo] : TabletStateInfo) {
                    if (!matchesFilter(stateInfo)) {
                        continue;
                    }
                    NKikimrWhiteboard::TTabletStateInfo& state = stateGroupBy[{stateInfo.type(), stateInfo.state()}];
                    auto count = state.count();
                    if (count == 0) {
                        state.set_type(stateInfo.type());
                        state.set_state(stateInfo.state());
                    }
                    state.set_count(count + 1);
                }
                for (auto& pr : stateGroupBy) {
                    NKikimrWhiteboard::TTabletStateInfo& tabletStateInfo = *record.add_tabletstateinfo();
                    tabletStateInfo = std::move(pr.second);
                }
            }
        }
        response->Record.set_responsetime(ctx.Now().MilliSeconds());
        response->Record.set_processduration((TMonotonic::Now() - now).MicroSeconds());
        ctx.Send(ev->Sender, response.release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvNodeStateRequest::TPtr &ev, const TActorContext &ctx) {
        const auto& request = ev->Get()->Record;
        ui64 changedSince = request.HasChangedSince() ? request.GetChangedSince() : 0;
        TAutoPtr<TEvWhiteboard::TEvNodeStateResponse> response = new TEvWhiteboard::TEvNodeStateResponse();
        auto& record = response->Record;
        for (const auto& pr : NodeStateInfo) {
            if (pr.second.GetChangeTime() >= changedSince) {
                NKikimrWhiteboard::TNodeStateInfo &nodeStateInfo = *record.AddNodeStateInfo();
                Copy(nodeStateInfo, pr.second, request);
            }
        }
        response->Record.SetResponseTime(ctx.Now().MilliSeconds());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

//    void Handle(TEvWhiteboard::TEvNodeStateRequest::TPtr &ev, const TActorContext &ctx) {
//        TAutoPtr<TEvWhiteboard::TEvNodeStateResponse> response = new TEvWhiteboard::TEvNodeStateResponse();
//        auto& record = response->Record;
//        const TIntrusivePtr<::NMonitoring::TDynamicCounters> &counters = AppData(ctx)->Counters;
//        TIntrusivePtr<::NMonitoring::TDynamicCounters> interconnectCounters = GetServiceCounters(counters, "interconnect");
//        interconnectCounters->EnumerateSubgroups([&record, &interconnectCounters](const TString &name, const TString &value) -> void {
//            NKikimrWhiteboard::TNodeStateInfo &nodeStateInfo = *record.AddNodeStateInfo();
//            TIntrusivePtr<::NMonitoring::TDynamicCounters> peerCounters = interconnectCounters->GetSubgroup(name, value);
//            ::NMonitoring::TDynamicCounters::TCounterPtr connectedCounter = peerCounters->GetCounter("Connected");
//            nodeStateInfo.SetPeerName(value);
//            nodeStateInfo.SetConnected(connectedCounter->Val());
//        });
//        response->Record.SetResponseTime(ctx.Now().MilliSeconds());
//        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
//    }

    void Handle(TEvWhiteboard::TEvPDiskStateRequest::TPtr &ev, const TActorContext &ctx) {
        const auto& request = ev->Get()->Record;
        ui64 changedSince = request.HasChangedSince() ? request.GetChangedSince() : 0;
        TAutoPtr<TEvWhiteboard::TEvPDiskStateResponse> response = new TEvWhiteboard::TEvPDiskStateResponse();
        auto& record = response->Record;
        for (const auto& pr : PDiskStateInfo) {
            if (pr.second.GetChangeTime() >= changedSince) {
                NKikimrWhiteboard::TPDiskStateInfo &pDiskStateInfo = *record.AddPDiskStateInfo();
                Copy(pDiskStateInfo, pr.second, request);
            }
        }
        response->Record.SetResponseTime(ctx.Now().MilliSeconds());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvPDiskStateDelete::TPtr &ev, const TActorContext &) {
        auto pdiskId = ev->Get()->Record.GetPDiskId();

        auto it = PDiskStateInfo.find(pdiskId);
        if (it != PDiskStateInfo.end()) {
            PDiskStateInfo.erase(it);
        }
    }

    void Handle(TEvWhiteboard::TEvVDiskStateRequest::TPtr &ev, const TActorContext &ctx) {
        const auto& request = ev->Get()->Record;
        ui64 changedSince = request.HasChangedSince() ? request.GetChangedSince() : 0;
        TAutoPtr<TEvWhiteboard::TEvVDiskStateResponse> response = new TEvWhiteboard::TEvVDiskStateResponse();
        auto& record = response->Record;
        for (const auto& pr : VDiskStateInfo) {
            if (pr.second.GetChangeTime() >= changedSince) {
                NKikimrWhiteboard::TVDiskStateInfo &vDiskStateInfo = *record.AddVDiskStateInfo();
                Copy(vDiskStateInfo, pr.second, request);
            }
        }
        response->Record.SetResponseTime(ctx.Now().MilliSeconds());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvBSGroupStateRequest::TPtr &ev, const TActorContext &ctx) {
        const auto& request = ev->Get()->Record;
        ui64 changedSince = request.HasChangedSince() ? request.GetChangedSince() : 0;
        TAutoPtr<TEvWhiteboard::TEvBSGroupStateResponse> response = new TEvWhiteboard::TEvBSGroupStateResponse();
        auto& record = response->Record;
        for (const auto& pr : BSGroupStateInfo) {
            if (pr.second.GetChangeTime() >= changedSince) {
                NKikimrWhiteboard::TBSGroupStateInfo &bSGroupStateInfo = *record.AddBSGroupStateInfo();
                Copy(bSGroupStateInfo, pr.second, request);
            }
        }
        response->Record.SetResponseTime(ctx.Now().MilliSeconds());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvBridgeInfoRequest::TPtr &ev, const TActorContext &ctx) {
        const auto& request = ev->Get()->Record;
        std::unique_ptr<TEvWhiteboard::TEvBridgeInfoResponse> response(new TEvWhiteboard::TEvBridgeInfoResponse);
        auto& record = response->Record;

        record.SetIsBridgeCluster(IsBridgeCluster);
        if (IsBridgeCluster) {
            ui64 changedSince = request.HasChangedSince() ? request.GetChangedSince() : 0;
            if (BridgeInfoChangeTime.MilliSeconds() >= changedSince) {
                record.MutableBridgeInfo()->CopyFrom(BridgeInfo);
                record.MutableBridgeNodesInfo()->CopyFrom(BridgeNodesInfo);
            }
        }
        record.SetResponseTime(ctx.Now().MilliSeconds());
        ctx.Send(ev->Sender, response.release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvSystemStateRequest::TPtr &ev, const TActorContext &ctx) {
        const auto& request = ev->Get()->Record;
        ui64 changedSince = request.HasChangedSince() ? request.GetChangedSince() : 0;
        TAutoPtr<TEvWhiteboard::TEvSystemStateResponse> response = new TEvWhiteboard::TEvSystemStateResponse();
        auto& record = response->Record;
        if (SystemStateInfo.GetChangeTime() >= changedSince) {
            NKikimrWhiteboard::TSystemStateInfo &systemStateInfo = *record.AddSystemStateInfo();
            Copy(systemStateInfo, SystemStateInfo, request);
        }
        response->Record.SetResponseTime(ctx.Now().MilliSeconds());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvIntrospectionData::TPtr &ev) {
        TEvWhiteboard::TEvIntrospectionData *msg = ev->Get();
        TabletIntrospectionData->AddTrace(msg->TabletId, msg->Trace.Release());
    }

    void Handle(TEvWhiteboard::TEvTabletLookupRequest::TPtr &ev, const TActorContext &ctx) {
        THolder<TEvWhiteboard::TEvTabletLookupResponse> response = MakeHolder<TEvWhiteboard::TEvTabletLookupResponse>();
        auto& record = response->Record;
        TVector<ui64> tabletIDs;
        TabletIntrospectionData->GetTabletIDs(tabletIDs);
        for (auto id : tabletIDs) {
            record.AddTabletIDs(id);
        }
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvTraceLookupRequest::TPtr &ev, const TActorContext &ctx) {
        ui64 tabletID = ev->Get()->Record.GetTabletID();
        THolder<TEvWhiteboard::TEvTraceLookupResponse> response = MakeHolder<TEvWhiteboard::TEvTraceLookupResponse>();
        auto& record = response->Record;
        TVector<NTracing::TTraceID> tabletTraces;
        TabletIntrospectionData->GetTraces(tabletID, tabletTraces);
        for (auto& tabletTrace : tabletTraces) {
            TraceIDFromTraceID(tabletTrace, record.AddTraceIDs());
        }
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvTraceRequest::TPtr &ev, const TActorContext &ctx) {
        auto& requestRecord = ev->Get()->Record;
        ui64 tabletID = requestRecord.GetTabletID();
        NTracing::TTraceID traceID = NTracing::TraceIDFromTraceID(requestRecord.GetTraceID());

        THolder<TEvWhiteboard::TEvTraceResponse> response = MakeHolder<TEvWhiteboard::TEvTraceResponse>();
        auto& responseRecord = response->Record;
        auto trace = TabletIntrospectionData->GetTrace(tabletID, traceID);
        NTracing::TTraceInfo traceInfo = {
            ctx.SelfID.NodeId(),
            tabletID,
            traceID,
            NTracing::TTimestampInfo(
                static_cast<NTracing::TTimestampInfo::EMode>(requestRecord.GetMode()),
                static_cast<NTracing::TTimestampInfo::EPrecision>(requestRecord.GetPrecision())
            )
        };
        TStringStream str;
        if (trace) {
            trace->OutHtml(str, traceInfo);
        } else {
            str << "Trace not found.";
        }
        responseRecord.SetTrace(str.Str());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void Handle(TEvWhiteboard::TEvSignalBodyRequest::TPtr &ev, const TActorContext &ctx) {
        auto& requestRecord = ev->Get()->Record;
        ui64 tabletID = requestRecord.GetTabletID();
        NTracing::TTraceID traceID = NTracing::TraceIDFromTraceID(requestRecord.GetTraceID());
        TString signalID = requestRecord.GetSignalID();

        THolder<TEvWhiteboard::TEvSignalBodyResponse> response = MakeHolder<TEvWhiteboard::TEvSignalBodyResponse>();
        auto& responseRecord = response->Record;
        auto trace = TabletIntrospectionData->GetTrace(tabletID, traceID);
        TStringStream str;
        if (trace) {
            trace->OutSignalHtmlBody(
                str,
                NTracing::TTimestampInfo(
                    static_cast<NTracing::TTimestampInfo::EMode>(requestRecord.GetMode()),
                    static_cast<NTracing::TTimestampInfo::EPrecision>(requestRecord.GetPrecision())
                ),
                signalID);
        } else {
            str << "Trace not found.";
        }
        responseRecord.SetSignalBody(str.Str());
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    static TVector<double> GetLoadAverage() {
        TVector<double> loadAvg(3);
        loadAvg.resize(NSystemInfo::LoadAverage(loadAvg.data(), loadAvg.size()));
        return loadAvg;
    }

    void Handle(TEvPrivate::TEvUpdateRuntimeStats::TPtr &, const TActorContext &ctx) {
        static constexpr int UPDATE_PERIOD_SECONDS = 15;
        static constexpr TDuration UPDATE_PERIOD = TDuration::Seconds(UPDATE_PERIOD_SECONDS);
        auto now = TActivationContext::Now();

        {
            NKikimrWhiteboard::TSystemStateInfo systemStatsUpdate;
            TVector<double> loadAverage = GetLoadAverage();
            for (double d : loadAverage) {
                systemStatsUpdate.AddLoadAverage(d);
            }
            if (CheckedMerge(SystemStateInfo, systemStatsUpdate)) {
                SystemStateInfo.SetChangeTime(now.MilliSeconds());
            }
        }

        {
            MaxClockSkewWithPeerUsCounter->Set(abs(MaxClockSkewWithPeerUs));
            MaxClockSkewPeerIdCounter->Set(MaxClockSkewPeerId);

            SystemStateInfo.SetMaxClockSkewWithPeerUs(MaxClockSkewWithPeerUs);
            SystemStateInfo.SetMaxClockSkewPeerId(MaxClockSkewPeerId);
            MaxClockSkewWithPeerUs = 0;
        }

        {
            SystemStateInfo.SetNetworkUtilization(MaxNetworkUtilization);
            MaxNetworkUtilization = 0;
        }
        {
            SystemStateInfo.SetNetworkWriteThroughput(SumNetworkWriteThroughput / UPDATE_PERIOD_SECONDS);
            SumNetworkWriteThroughput = 0;
        }
        auto threadPools = ThreadsMonitor.GetThreadPools(now);
        SystemStateInfo.ClearThreads();
        for (const auto& threadPool : threadPools) {
            auto* threadInfo = SystemStateInfo.AddThreads();
            threadInfo->SetName(threadPool.Name);
            threadInfo->SetThreads(threadPool.Threads);
            threadInfo->SetSystemUsage(threadPool.SystemUsage);
            threadInfo->SetUserUsage(threadPool.UserUsage);
            threadInfo->SetMajorPageFaults(threadPool.MajorPageFaults);
            threadInfo->SetMinorPageFaults(threadPool.MinorPageFaults);
            for (const auto& state : threadPool.States) {
                threadInfo->MutableStates()->emplace(state.first, state.second);
            }
        }
        UpdateSystemState();
        ctx.Schedule(UPDATE_PERIOD, new TEvPrivate::TEvUpdateRuntimeStats());
    }

    void Handle(TEvPrivate::TEvCleanupDeadTablets::TPtr &, const TActorContext &ctx) {
        auto it = TabletStateInfo.begin();
        ui64 deadDeadline = (ctx.Now() - TDuration::Minutes(10)).MilliSeconds();
        ui64 deletedDeadline = (ctx.Now() - TDuration::Hours(1)).MilliSeconds();
        while (it != TabletStateInfo.end()) {
            const auto& tabletInfo = it->second;
            NKikimrWhiteboard::TTabletStateInfo::ETabletState state = tabletInfo.GetState();
            switch (state) {
            case NKikimrWhiteboard::TTabletStateInfo::Dead:
                if (tabletInfo.GetChangeTime() < deadDeadline) {
                    it = TabletStateInfo.erase(it);
                } else {
                    ++it;
                }
                break;
            case NKikimrWhiteboard::TTabletStateInfo::Deleted:
                if (tabletInfo.GetChangeTime() < deletedDeadline) {
                    it = TabletStateInfo.erase(it);
                } else {
                    ++it;
                }
                break;
            default:
                ++it;
                break;
            }
        }
        ctx.Schedule(TDuration::Seconds(60), new TEvPrivate::TEvCleanupDeadTablets());
    }
};

template<typename TMessage>
::google::protobuf::RepeatedField<int> InitDefaultWhiteboardFields() {
    using namespace ::google::protobuf;
    const Descriptor& descriptor = *TMessage::GetDescriptor();
    ::google::protobuf::RepeatedField<int> defaultFields;
    int fieldCount = descriptor.field_count();
    for (int index = 0; index < fieldCount; ++index) {
        const FieldDescriptor* field = descriptor.field(index);
        const auto& options(field->options());
        if (options.HasExtension(NKikimrWhiteboard::DefaultField)) {
            if (options.GetExtension(NKikimrWhiteboard::DefaultField)) {
                defaultFields.Add(field->number());
            }
        }
    }
    return defaultFields;
}

template<typename TMessage>
::google::protobuf::RepeatedField<int> GetDefaultWhiteboardFields() {
    static ::google::protobuf::RepeatedField<int> defaultFields = InitDefaultWhiteboardFields<TMessage>();
    return defaultFields;
}

IActor* CreateNodeWhiteboardService() {
    return new TNodeWhiteboardService();
}

} // NNodeWhiteboard
} // NKikimr
