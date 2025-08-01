#include "abstract.h"
#include "events.h"

#include <ydb/library/actors/core/log.h>

namespace NKikimr::NOlap::NGroupedMemoryManager {

TAllocationGuard::~TAllocationGuard() {
    if (TlsActivationContext && !Released) {
        if (Stage) {
            Stage->Free(Memory, true);
        }
        NActors::TActivationContext::AsActorContext().Send(
            ActorId, std::make_unique<NEvents::TEvExternal::TEvFinishTask>(ProcessId, ScopeId, AllocationId));
    }
}

void TAllocationGuard::Update(const ui64 newVolume, const bool notify) {
    AFL_VERIFY(!Released);
    if (Stage) {
        Stage->UpdateVolume(Memory, newVolume, true);
    }
    if (notify && TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(
            ActorId, std::make_unique<NEvents::TEvExternal::TEvTaskUpdated>(ProcessId, ScopeId, AllocationId));
    }
    Memory = newVolume;
}

bool IAllocation::OnAllocated(std::shared_ptr<TAllocationGuard>&& guard, const std::shared_ptr<NGroupedMemoryManager::IAllocation>& allocation) {
    AFL_VERIFY(!Allocated);
    Allocated = true;
    AFL_VERIFY(allocation);
    AFL_VERIFY(guard);
    return DoOnAllocated(std::move(guard), allocation);
}

TGroupGuard::~TGroupGuard() {
    if (TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(
            ActorId, std::make_unique<NEvents::TEvExternal::TEvFinishGroup>(ProcessId, ExternalScopeId, GroupId));
    }
}

TGroupGuard::TGroupGuard(const NActors::TActorId& actorId, const ui64 processId, const ui64 externalScopeId, const ui64 groupId)
    : ActorId(actorId)
    , ProcessId(processId)
    , ExternalScopeId(externalScopeId)
    , GroupId(groupId) {
    if (TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(
            ActorId, std::make_unique<NEvents::TEvExternal::TEvStartGroup>(ProcessId, ExternalScopeId, GroupId));
    }
}

TProcessGuard::~TProcessGuard() {
    if (TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(ActorId, std::make_unique<NEvents::TEvExternal::TEvFinishProcess>(ProcessId));
    }
}

TProcessGuard::TProcessGuard(const NActors::TActorId& actorId, const ui64 processId, const std::vector<std::shared_ptr<TStageFeatures>>& stages)
    : ActorId(actorId)
    , ProcessId(processId) {
    if (TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(ActorId, std::make_unique<NEvents::TEvExternal::TEvStartProcess>(ProcessId, stages));
    }
}

TScopeGuard::~TScopeGuard() {
    if (TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(ActorId, std::make_unique<NEvents::TEvExternal::TEvFinishProcessScope>(ProcessId, ScopeId));
    }
}

TScopeGuard::TScopeGuard(const NActors::TActorId& actorId, const ui64 processId, const ui64 scopeId)
    : ActorId(actorId)
    , ProcessId(processId)
    , ScopeId(scopeId) {
    if (TlsActivationContext) {
        NActors::TActivationContext::AsActorContext().Send(ActorId, std::make_unique<NEvents::TEvExternal::TEvStartProcessScope>(ProcessId, ScopeId));
    }
}

}   // namespace NKikimr::NOlap::NGroupedMemoryManager
