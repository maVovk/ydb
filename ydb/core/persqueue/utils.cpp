#include "utils.h"

#include <deque>

#include <util/generic/algorithm.h>
#include <util/string/builder.h>
#include <ydb/library/yverify_stream/yverify_stream.h>

namespace NKikimr::NPQ {

ui64 TopicPartitionReserveSize(const NKikimrPQ::TPQTabletConfig& config) {
    if (!config.HasMeteringMode()) {
        // Only for federative and dedicated installations
        return 0;
    }
    if (NKikimrPQ::TPQTabletConfig::METERING_MODE_REQUEST_UNITS == config.GetMeteringMode()) {
        return 0;
    }
    if (config.GetPartitionConfig().HasStorageLimitBytes()) {
        return config.GetPartitionConfig().GetStorageLimitBytes();
    }
    return config.GetPartitionConfig().GetLifetimeSeconds() * config.GetPartitionConfig().GetWriteSpeedInBytesPerSecond();
}

ui64 TopicPartitionReserveThroughput(const NKikimrPQ::TPQTabletConfig& config) {
    if (!config.HasMeteringMode()) {
        // Only for federative and dedicated installations
        return 0;
    }
    if (NKikimrPQ::TPQTabletConfig::METERING_MODE_REQUEST_UNITS == config.GetMeteringMode()) {
        return 0;
    }
    return config.GetPartitionConfig().GetWriteSpeedInBytesPerSecond();
}

bool MirroringEnabled(const NKikimrPQ::TPQTabletConfig& config) {
    return config.GetPartitionConfig().HasMirrorFrom();
}

bool SplitMergeEnabled(const NKikimrPQ::TPQTabletConfig& config) {
    return config.has_partitionstrategy() && config.partitionstrategy().has_partitionstrategytype() && config.partitionstrategy().partitionstrategytype() != ::NKikimrPQ::TPQTabletConfig_TPartitionStrategyType::TPQTabletConfig_TPartitionStrategyType_DISABLED;
}

size_t CountActivePartitions(const ::google::protobuf::RepeatedPtrField< ::NKikimrPQ::TPQTabletConfig_TPartition >& partitions) {
    return std::count_if(partitions.begin(), partitions.end(), [](const auto& p) {
        return p.GetStatus() == ::NKikimrPQ::ETopicPartitionStatus::Active;
    });
}

static constexpr ui64 PUT_UNIT_SIZE = 40960u; // 40Kb

ui64 PutUnitsSize(const ui64 size) {
    ui64 putUnitsCount = size / PUT_UNIT_SIZE;
    if (size % PUT_UNIT_SIZE != 0) {
        ++putUnitsCount;
    }
    return putUnitsCount;
}

bool IsImportantClient(const NKikimrPQ::TPQTabletConfig& config, const TString& consumerName) {
    for (const auto& i : config.GetPartitionConfig().GetImportantClientId()) {
        if (consumerName == i) {
            return true;
        }
    }

    return false;
}

void Migrate(NKikimrPQ::TPQTabletConfig& config) {
    // if ReadRules isn`t empty than it is old configuration format
    // when modify new format (add or alter a consumer) readRules is cleared
    if (config.ReadRulesSize()) {
        config.ClearConsumers();

        for(size_t i = 0; i < config.ReadRulesSize(); ++i) {
            auto* consumer = config.AddConsumers();

            consumer->SetName(config.GetReadRules(i));
            if (i < config.ReadFromTimestampsMsSize()) {
                consumer->SetReadFromTimestampsMs(config.GetReadFromTimestampsMs(i));
            }
            if (i < config.ConsumerFormatVersionsSize()) {
                consumer->SetFormatVersion(config.GetConsumerFormatVersions(i));
            }
            if (i < config.ConsumerCodecsSize()) {
                auto& src = config.GetConsumerCodecs(i);
                auto* dst = consumer->MutableCodec();
                dst->CopyFrom(src);
            }
            if (i < config.ReadRuleServiceTypesSize()) {
                consumer->SetServiceType(config.GetReadRuleServiceTypes(i));
            }
            if (i < config.ReadRuleVersionsSize()) {
                consumer->SetVersion(config.GetReadRuleVersions(i));
            }
            if (i < config.ReadRuleGenerationsSize()) {
                consumer->SetGeneration(config.GetReadRuleGenerations(i));
            }
            consumer->SetImportant(IsImportantClient(config, consumer->GetName()));
        }

        config.ClearReadRules();
        config.ClearReadFromTimestampsMs();
        config.ClearConsumerFormatVersions();
        config.ClearConsumerCodecs();
        config.ClearReadRuleServiceTypes();
        config.ClearReadRuleVersions();
        config.ClearReadRuleGenerations();
    }

    if (!config.PartitionsSize()) {
        for (const auto partitionId : config.GetPartitionIds()) {
            config.AddPartitions()->SetPartitionId(partitionId);
        }
    }

    if (!config.AllPartitionsSize()) {
        for (const auto& partition : config.GetPartitions()) {
            config.AddAllPartitions()->CopyFrom(partition);
        }
    }
}

bool HasConsumer(const NKikimrPQ::TPQTabletConfig& config, const TString& consumerName) {
    for (auto& cons : config.GetConsumers()) {
        if (cons.GetName() == consumerName) {
            return true;
        }
    }

    return false;
}

size_t ConsumerCount(const NKikimrPQ::TPQTabletConfig& config) {
    return config.ConsumersSize();
}

const NKikimrPQ::TPQTabletConfig::TPartition* GetPartitionConfig(const NKikimrPQ::TPQTabletConfig& config, const ui32 partitionId) {
    for(const auto& p : config.GetPartitions()) {
        if (partitionId == p.GetPartitionId()) {
            return &p;
        }
    }
    return nullptr;
}

TPartitionGraph::TPartitionGraph() {
}

TPartitionGraph::TPartitionGraph(std::unordered_map<ui32, Node>&& partitions) {
    Partitions = std::move(partitions);
}

const TPartitionGraph::Node* TPartitionGraph::GetPartition(ui32 id) const {
    auto it = Partitions.find(id);
    if (it == Partitions.end()) {
        return nullptr;
    }
    return &it->second;
}

std::set<ui32> TPartitionGraph::GetActiveChildren(ui32 id) const {
    const auto* p = GetPartition(id);
    if (!p) {
        return {};
    }

    std::deque<const Node*> queue;
    queue.push_back(p);

    std::set<ui32> result;
    while(!queue.empty()) {
        const auto* n = queue.front();
        queue.pop_front();

        if (n->DirectChildren.empty()) {
            result.emplace(n->Id);
        } else {
            queue.insert(queue.end(), n->DirectChildren.begin(), n->DirectChildren.end());
        }
    }

    return result;
}

void Travers0(std::deque<const TPartitionGraph::Node*>& queue, const std::function<bool (ui32 id)>& func) {
    while(!queue.empty()) {
        auto* node = queue.front();
        queue.pop_front();

        if (func(node->Id)) {
            queue.insert(queue.end(), node->DirectChildren.begin(), node->DirectChildren.end());
        }
    }
}

void TPartitionGraph::Travers(const std::function<bool (ui32 id)>& func) const {
    std::deque<const Node*> queue;

    for (auto& [id, n] : Partitions) {
        if (!n.IsRoot()) {
            continue;
        }

        if (!func(id)) {
            continue;
        }

        queue.insert(queue.end(), n.DirectChildren.begin(), n.DirectChildren.end());
    }

    Travers0(queue, func);
}

void TPartitionGraph::Travers(ui32 id, const std::function<bool (ui32 id)>& func, bool includeSelf) const {
    auto* n = GetPartition(id);
    if (!n) {
        return;
    }

    if (includeSelf && !func(id)) {
        return;
    }

    std::deque<const Node*> queue;
    queue.insert(queue.end(), n->DirectChildren.begin(), n->DirectChildren.end());

    Travers0(queue, func);
}

bool TPartitionGraph::Empty() const {
    return Partitions.empty();
}

TPartitionGraph::operator bool() const {
    return !Empty();
}

TString TPartitionGraph::DebugString() const {
    TStringBuilder sb;
    sb << "{";
    for (const auto& [k,_] : Partitions) {
        sb << k << ", ";
    }
    sb << "}";
    return sb;
}

template<typename TPartition>
inline int GetPartitionId(TPartition p) {
    return p.GetPartitionId();
}

template<>
inline int GetPartitionId(NKikimrPQ::TUpdateBalancerConfig::TPartition p) {
    return p.GetPartition();
}

template<typename TPartition, typename TCollection = ::google::protobuf::RepeatedPtrField<TPartition>>
std::unordered_map<ui32, TPartitionGraph::Node> BuildGraph(const TCollection& partitions) {
    std::unordered_map<ui32, TPartitionGraph::Node> result;

    if (0 == partitions.size()) {
        return result;
    }

    for (const auto& p : partitions) {
        result.emplace(GetPartitionId(p), TPartitionGraph::Node(GetPartitionId(p), p.GetTabletId(), p.GetKeyRange().GetFromBound(), p.GetKeyRange().GetToBound()));
    }

    std::deque<TPartitionGraph::Node*> queue;

    for (const auto& p : partitions) {
        auto& node = result[GetPartitionId(p)];

        node.DirectChildren.reserve(p.ChildPartitionIdsSize());
        for (auto id : p.GetChildPartitionIds()) {
            node.DirectChildren.push_back(&result[id]);
        }

        node.DirectParents.reserve(p.ParentPartitionIdsSize());
        for (auto id : p.GetParentPartitionIds()) {
            node.DirectParents.push_back(&result[id]);
        }

        if (p.GetParentPartitionIds().empty()) {
            queue.push_back(&node);
        }
    }

    while (!queue.empty()) {
        auto* n = queue.front();
        queue.pop_front();

        bool allCompleted = true;
        for (auto* c : n->DirectParents) {
            if (c->AllParents.empty() && !c->DirectParents.empty()) {
                allCompleted = false;
                break;
            }
        }

        if (allCompleted) {
            for (auto* c : n->DirectParents) {
                n->AllParents.insert(c->AllParents.begin(), c->AllParents.end());
                n->AllParents.insert(c);
            }
            queue.insert(queue.end(), n->DirectChildren.begin(), n->DirectChildren.end());
        }
    }

    for (auto& [_, node] : result) {
        queue.push_back(&node);

        while (!queue.empty()) {
            auto* current = queue.front();
            queue.pop_front();

            for (auto* child : current->DirectChildren) {
                if (node.AllChildren.insert(child).second) {
                    queue.push_back(child);
                }
            }
        }
    }

    return result;
}

TPartitionGraph::Node::Node(ui32 id, ui64 tabletId, const TString& from, const TString& to)
    : Id(id)
    , TabletId(tabletId)
    , From(from)
    , To(to) {
}

bool TPartitionGraph::Node::IsRoot() const {
    return DirectParents.empty();
}

bool TPartitionGraph::Node::IsParent(ui32 partitionId) const {
    return AnyOf(DirectParents, [=](const auto& p) {
        return p->Id == partitionId;
    });
}

TPartitionGraph MakePartitionGraph(const NKikimrPQ::TPQTabletConfig& config) {
    return TPartitionGraph(BuildGraph<NKikimrPQ::TPQTabletConfig::TPartition>(config.GetAllPartitions()));
}

TPartitionGraph MakePartitionGraph(const NKikimrPQ::TUpdateBalancerConfig& config) {
    return TPartitionGraph(BuildGraph<NKikimrPQ::TUpdateBalancerConfig::TPartition>(config.GetPartitions()));
}

TPartitionGraph MakePartitionGraph(const NKikimrSchemeOp::TPersQueueGroupDescription& config) {
    return TPartitionGraph(BuildGraph<NKikimrSchemeOp::TPersQueueGroupDescription::TPartition>(config.GetPartitions()));
}

TPartitionGraph::TPtr MakeSharedPartitionGraph(const NKikimrPQ::TPQTabletConfig& config) {
    return std::make_shared<TPartitionGraph>(MakePartitionGraph(config));
}

TPartitionGraph::TPtr MakeSharedPartitionGraph(const NKikimrSchemeOp::TPersQueueGroupDescription& config) {
    return std::make_shared<TPartitionGraph>(MakePartitionGraph(config));
}

void TLastCounter::Use(const TString& value, const TInstant& now) {
    const auto full = MaxValueCount == Values.size();
    if (!Values.empty() && Values[0].Value == value) {
        auto& v0 = Values[0];
        if (v0.LastUseTime < now) {
            v0.LastUseTime = now;
            if (full && Values[1].LastUseTime != now) {
                Values.push_back(std::move(v0));
                Values.pop_front();
            }
        }
    } else if (full && Values[1].Value == value) {
        Values[1].LastUseTime = now;
    } else if (!full || Values[0].LastUseTime < now) {
        if (full) {
            Values.pop_front();
        }
        Values.push_back(Data{now, value});
    }
}

size_t TLastCounter::Count(const TInstant& expirationTime) {
    return std::count_if(Values.begin(), Values.end(), [&](const auto& i) {
        return i.LastUseTime >= expirationTime;
    });
}

const TString& TLastCounter::LastValue() const {
    return Values.back().Value;
}

} // NKikimr::NPQ
