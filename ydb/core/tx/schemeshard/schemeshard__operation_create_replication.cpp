#include "schemeshard__op_traits.h"
#include "schemeshard__operation_common.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_impl.h"

#include <ydb/core/mind/hive/hive.h>
#include <ydb/core/tx/replication/controller/public_events.h>

#define LOG_D(stream) LOG_DEBUG_S (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_I(stream) LOG_INFO_S  (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_W(stream) LOG_WARN_S  (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)

namespace NKikimr::NSchemeShard {

namespace {

struct IStrategy {
    virtual TPathElement::EPathType GetPathType() const = 0;
    virtual bool Validate(TProposeResponse& result, const NKikimrSchemeOp::TReplicationDescription& desc, const TOperationContext& context) const = 0;
    virtual void Proccess(NKikimrReplication::TReplicationConfig& config, const TString& owner) const = 0;
};

struct TReplicationStrategy : public IStrategy {
    TPathElement::EPathType GetPathType() const override {
        return TPathElement::EPathType::EPathTypeReplication;
    };

    bool Validate(TProposeResponse& result, const NKikimrSchemeOp::TReplicationDescription& desc, const TOperationContext&) const override {
        if (desc.GetConfig().HasTransferSpecific()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Wrong replication configuration");
            return true;
        }
        if (desc.HasState()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Cannot create replication with explicit state");
            return true;
        }

        return false;
    }

    void Proccess(NKikimrReplication::TReplicationConfig&, const TString&) const override {
    }
};

struct TTransferStrategy : public IStrategy {
    TPathElement::EPathType GetPathType() const override {
        return TPathElement::EPathType::EPathTypeTransfer;
    };

    bool Validate(TProposeResponse& result, const NKikimrSchemeOp::TReplicationDescription& desc, const TOperationContext& context) const override {
        if (!AppData()->FeatureFlags.GetEnableTopicTransfer()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Topic transfer creation is disabled");
            return true;
        }
        if (!desc.GetConfig().HasTransferSpecific()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Wrong transfer configuration");
            return true;
        }
        if (desc.HasState()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Cannot create transfer with explicit state");
            return true;
        }

        const auto& batching = desc.GetConfig().GetTransferSpecific().GetBatching();
        if (batching.HasBatchSizeBytes() && batching.GetBatchSizeBytes() > 1_GB) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Batch size must be less than or equal to 1Gb");
            return true;
        }
        if (batching.HasFlushIntervalMilliSeconds() && batching.GetFlushIntervalMilliSeconds() < TDuration::Seconds(1).MilliSeconds()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Flush interval must be greater than or equal to 1 second");
            return true;
        }
        if (batching.HasFlushIntervalMilliSeconds() && batching.GetFlushIntervalMilliSeconds() > TDuration::Hours(24).MilliSeconds()) {
            result.SetError(NKikimrScheme::StatusInvalidParameter, "Flush interval must be less than or equal to 24 hours");
            return true;
        }

        const auto& target = desc.GetConfig().GetTransferSpecific().GetTarget();
        auto targetPath = TPath::Resolve(target.GetDstPath(), context.SS);
        if (!targetPath.IsResolved() || targetPath.IsUnderDeleting() || targetPath->IsUnderMoving() || targetPath.IsDeleted()) {
            result.SetError(NKikimrScheme::StatusNotAvailable, TStringBuilder() << "The transfer destination path '" << target.GetDstPath() << "' not found");
            return true;
        }
        if (!targetPath->IsColumnTable() && !targetPath->IsTable()) {
            result.SetError(NKikimrScheme::StatusNotAvailable, TStringBuilder() << "The transfer destination path '" << target.GetDstPath() << "' isn`t a table");
            return true;
        }

        if (target.HasDirectoryPath()) {
            auto directoryPath = TPath::Resolve(target.GetDirectoryPath(), context.SS);
            if (!directoryPath.IsResolved() || directoryPath.IsUnderDeleting() || directoryPath->IsUnderMoving() || directoryPath.IsDeleted()) {
                result.SetError(NKikimrScheme::StatusNotAvailable, TStringBuilder() << "The transfer destination directory path '" << target.GetDirectoryPath() << "' not found");
                return true;
            }
        }

        if (!AppData()->TransferWriterFactory) {
            result.SetError(NKikimrScheme::StatusNotAvailable, "The transfer is only available in the Enterprise version");
            return true;
        }

        return false;
    }

    void Proccess(NKikimrReplication::TReplicationConfig& config, const TString& owner) const override {
        config.MutableTransferSpecific()->SetRunAsUser(owner);
    }
};

static constexpr TReplicationStrategy ReplicationStrategy;
static constexpr TTransferStrategy TransferStrategy;

class TConfigureParts: public TSubOperationState {
    TString DebugHint() const override {
        return TStringBuilder()
            << "TCreateReplication TConfigureParts"
            << " opId# " << OperationId << " ";
    }

public:
    explicit TConfigureParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {
            TEvHive::TEvCreateTabletReply::EventType,
        });
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState");

        auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxCreateReplication);
        const auto& pathId = txState->TargetPathId;

        Y_ABORT_UNLESS(context.SS->Replications.contains(pathId));
        auto alterData = context.SS->Replications.at(pathId)->AlterData;
        Y_ABORT_UNLESS(alterData);

        txState->ClearShardsInProgress();

        for (const auto& shard : txState->Shards) {
            Y_ABORT_UNLESS(shard.TabletType == ETabletType::ReplicationController);

            Y_ABORT_UNLESS(context.SS->ShardInfos.contains(shard.Idx));
            const auto tabletId = context.SS->ShardInfos.at(shard.Idx).TabletID;

            if (tabletId == InvalidTabletId) {
                LOG_D(DebugHint() << "Shard is not created yet"
                    << ": shardIdx# " << shard.Idx);
                context.OnComplete.WaitShardCreated(shard.Idx, OperationId);
            } else {
                auto ev = MakeHolder<NReplication::TEvController::TEvCreateReplication>();
                pathId.ToProto(ev->Record.MutablePathId());
                ev->Record.MutableOperationId()->SetTxId(ui64(OperationId.GetTxId()));
                ev->Record.MutableOperationId()->SetPartId(ui32(OperationId.GetSubTxId()));
                ev->Record.MutableConfig()->CopyFrom(alterData->Description.GetConfig());
                ev->Record.SetDatabase(TPath::Init(context.SS->RootPathId(), context.SS).PathString());

                LOG_D(DebugHint() << "Send TEvCreateReplication to controller"
                    << ": tabletId# " << tabletId
                    << ", ev# " << ev->ToString());
                context.OnComplete.BindMsgToPipe(OperationId, tabletId, pathId, ev.Release());
            }

            txState->ShardsInProgress.insert(shard.Idx);
        }

        return false;
    }

    bool HandleReply(NReplication::TEvController::TEvCreateReplicationResult::TPtr& ev, TOperationContext& context) override {
        LOG_I(DebugHint() << "HandleReply " << ev->Get()->ToString());

        const auto tabletId = TTabletId(ev->Get()->Record.GetOrigin());
        const auto status = ev->Get()->Record.GetStatus();

        switch (status) {
        case NKikimrReplication::TEvCreateReplicationResult::SUCCESS:
        case NKikimrReplication::TEvCreateReplicationResult::ALREADY_EXISTS:
            break;
        default:
            LOG_W(DebugHint() << "Ignoring unexpected TEvCreateReplicationResult"
                << " tabletId# " << tabletId
                << " status# " << static_cast<int>(status));
            return false;
        }

        auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxCreateReplication);
        Y_ABORT_UNLESS(txState->State == TTxState::ConfigureParts);

        const auto shardIdx = context.SS->MustGetShardIdx(tabletId);
        if (!txState->ShardsInProgress.erase(shardIdx)) {
            LOG_W(DebugHint() << "Ignoring duplicate TEvCreateReplicationResult");
            return false;
        }

        context.OnComplete.UnbindMsgFromPipe(OperationId, tabletId, txState->TargetPathId);

        if (!txState->ShardsInProgress.empty()) {
            return false;
        }

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
        context.OnComplete.ActivateTx(OperationId);

        return true;
    }

private:
    const TOperationId OperationId;

}; // TConfigureParts

class TPropose: public TSubOperationState {
    TString DebugHint() const override {
        return TStringBuilder()
            << "TCreateReplication TPropose"
            << " opId# " << OperationId << " ";
    }

public:
    explicit TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {
            TEvHive::TEvCreateTabletReply::EventType,
            NReplication::TEvController::TEvCreateReplicationResult::EventType,
        });
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState");

        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxCreateReplication);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const auto step = TStepId(ev->Get()->StepId);

        LOG_I(DebugHint() << "HandleReply TEvOperationPlan"
            << ": step# " << step);

        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxCreateReplication);
        const auto& pathId = txState->TargetPathId;

        Y_ABORT_UNLESS(context.SS->PathsById.contains(pathId));
        auto path = context.SS->PathsById.at(pathId);

        Y_ABORT_UNLESS(context.SS->Replications.contains(pathId));
        auto replication = context.SS->Replications.at(pathId);

        auto alterData = replication->AlterData;
        Y_ABORT_UNLESS(alterData);

        NIceDb::TNiceDb db(context.GetDB());

        path->StepCreated = step;
        context.SS->PersistCreateStep(db, pathId, step);

        context.SS->Replications[pathId] = alterData;
        context.SS->PersistReplicationAlterRemove(db, pathId);
        context.SS->PersistReplication(db, pathId, *alterData);

        Y_ABORT_UNLESS(context.SS->PathsById.contains(path->ParentPathId));
        auto parentPath = context.SS->PathsById.at(path->ParentPathId);

        ++parentPath->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath);

        context.SS->ClearDescribePathCaches(parentPath);
        context.OnComplete.PublishToSchemeBoard(OperationId, parentPath->PathId);

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

private:
   const TOperationId OperationId;

}; // TPropose

class TCreateReplication: public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::CreateParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return TTxState::ConfigureParts;
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::CreateParts:
            return MakeHolder<TCreateParts>(OperationId);
        case TTxState::ConfigureParts:
            return MakeHolder<TConfigureParts>(OperationId);
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

public:
    using TSubOperation::TSubOperation;

    explicit TCreateReplication(const TOperationId& id, TTxState::ETxState state, const IStrategy* strategy)
        : TSubOperation(id, state)
        , Strategy(strategy)
    {
    }

    explicit TCreateReplication(const TOperationId& id, const TTxTransaction& tx, const IStrategy* strategy)
        : TSubOperation(id, tx)
        , Strategy(strategy)
    {
    }

    THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
        const auto& workingDir = Transaction.GetWorkingDir();
        auto desc = Transaction.GetReplication();
        const auto& name = desc.GetName();
        const auto& acl = Transaction.GetModifyACL().GetDiffACL();
        const auto acceptExisted = !Transaction.GetFailOnExist();

        LOG_N("TCreateReplication Propose"
            << ": opId# " << OperationId
            << ", path# " << workingDir << "/" << name);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), ui64(context.SS->SelfTabletId()));

        const auto parentPath = TPath::Resolve(workingDir, context.SS);
        {
            const auto checks = parentPath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsCommonSensePath()
                .IsLikeDirectory()
                .FailOnRestrictedCreateInTempZone();

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        if (Strategy->Validate(*result, desc, context)) {
            return result;
        }

        auto path = parentPath.Child(name);
        {
            const auto checks = path.Check();
            checks
                .IsAtLocalSchemeShard();

            if (path.IsResolved()) {
                checks
                    .IsResolved()
                    .NotUnderDeleting()
                    .FailOnExist(Strategy->GetPathType(), acceptExisted);
            } else {
                checks
                    .NotEmpty()
                    .NotResolved();
            }

            if (checks) {
                checks
                    .IsValidLeafName(context.UserToken.Get())
                    .DepthLimit()
                    .PathsLimit()
                    .DirChildrenLimit()
                    .ShardsLimit(1)
                    .IsValidACL(acl);
            }

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                if (path.IsResolved()) {
                    result->SetPathCreateTxId(ui64(path->CreateTxId));
                    result->SetPathId(path->PathId.LocalPathId);
                }

                return result;
            }
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        TChannelsBindings channelsBindings;
        if (!context.SS->ResolveTabletChannels(0, parentPath.GetPathIdForDomain(), channelsBindings)) {
            result->SetError(NKikimrScheme::StatusInvalidParameter,
                "Unable to construct channel binding for replication controller with the storage pool");
            return result;
        }
 
        const auto& connectionParams = desc.GetConfig().GetSrcConnectionParams();
        if (connectionParams.HasCaCert() && !connectionParams.GetEnableSsl()) {
            result->SetError(NKikimrScheme::StatusInvalidParameter, "CA_CERT has no effect in non-secure mode");
            return result;
        }

        path.MaterializeLeaf(owner);
        path->CreateTxId = OperationId.GetTxId();
        path->LastTxId = OperationId.GetTxId();
        path->PathState = TPathElement::EPathState::EPathStateCreate;
        path->PathType = Strategy->GetPathType();
        result->SetPathId(path->PathId.LocalPathId);

        context.SS->IncrementPathDbRefCount(path->PathId);
        IncAliveChildrenDirect(OperationId, parentPath, context); // for correct discard of ChildrenExist prop
        parentPath.DomainInfo()->IncPathsInside(context.SS);

        if (connectionParams.GetCredentialsCase() == NKikimrReplication::TConnectionParams::CREDENTIALS_NOT_SET) {
            desc.MutableConfig()->MutableSrcConnectionParams()->MutableOAuthToken()->SetToken(BUILTIN_ACL_ROOT);
        }

        if (desc.GetConfig().GetConsistencySettings().GetLevelCase() == NKikimrReplication::TConsistencySettings::LEVEL_NOT_SET) {
            desc.MutableConfig()->MutableConsistencySettings()->MutableRow();
        }

        Strategy->Proccess(*desc.MutableConfig(), owner);

        desc.MutableState()->MutableStandBy();
        auto replication = TReplicationInfo::Create(std::move(desc));
        context.SS->Replications[path->PathId] = replication;
        context.SS->TabletCounters->Simple()[COUNTER_REPLICATION_COUNT].Add(1);

        replication->AlterData->ControllerShardIdx = context.SS->RegisterShardInfo(
            TShardInfo::ReplicationControllerInfo(OperationId.GetTxId(), path->PathId)
                .WithBindedChannels(channelsBindings));
        context.SS->TabletCounters->Simple()[COUNTER_REPLICATION_CONTROLLER_COUNT].Add(1);

        Y_ABORT_UNLESS(!context.SS->FindTx(OperationId));
        auto& txState = context.SS->CreateTx(OperationId, TTxState::TxCreateReplication, path->PathId);
        txState.Shards.emplace_back(replication->AlterData->ControllerShardIdx,
            ETabletType::ReplicationController, TTxState::CreateParts);
        txState.State = TTxState::CreateParts;

        path->IncShardsInside();
        parentPath.DomainInfo()->AddInternalShards(txState, context.SS);

        if (parentPath->HasActiveChanges()) {
            const auto parentTxId = parentPath->PlannedToCreate() ? parentPath->CreateTxId : parentPath->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }

        NIceDb::TNiceDb db(context.GetDB());

        if (!acl.empty()) {
            path->ApplyACL(acl);
        }
        context.SS->PersistPath(db, path->PathId);

        context.SS->PersistReplication(db, path->PathId, *replication);
        context.SS->PersistReplicationAlter(db, path->PathId, *replication->AlterData);

        Y_ABORT_UNLESS(txState.Shards.size() == 1);
        for (const auto& shard : txState.Shards) {
            Y_ABORT_UNLESS(context.SS->ShardInfos.contains(shard.Idx));
            const TShardInfo& shardInfo = context.SS->ShardInfos.at(shard.Idx);

            if (shard.Operation == TTxState::CreateParts) {
                context.SS->PersistShardMapping(db, shard.Idx, InvalidTabletId, path->PathId, OperationId.GetTxId(), shard.TabletType);
                context.SS->PersistChannelsBinding(db, shard.Idx, shardInfo.BindedChannels);
            }
        }

        context.SS->ChangeTxState(db, OperationId, txState.State);
        context.SS->PersistTxState(db, OperationId);
        context.SS->PersistUpdateNextPathId(db);
        context.SS->PersistUpdateNextShardIdx(db);

        ++parentPath->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentPath.Base());

        context.SS->ClearDescribePathCaches(parentPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, parentPath->PathId);

        context.SS->ClearDescribePathCaches(path.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, path->PathId);

        context.OnComplete.ActivateTx(OperationId);

        SetState(NextState());
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_ABORT("no AbortPropose for TCreateReplication");
    }

    void AbortUnsafe(TTxId txId, TOperationContext& context) override {
        LOG_N("TCreateReplication AbortUnsafe"
            << ": opId# " << OperationId
            << ", txId# " << txId);
        context.OnComplete.DoneOperation(OperationId);
    }

private:
    const IStrategy* Strategy;

}; // TCreateReplication

} // anonymous

using TTag = TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateReplication>;

namespace NOperation {

template <>
std::optional<TString> GetTargetName<TTag>(TTag, const TTxTransaction& tx) {
    return tx.GetReplication().GetName();
}

template <>
bool SetName<TTag>(TTag, TTxTransaction& tx, const TString& name) {
    tx.MutableReplication()->SetName(name);
    return true;
}

} // namespace NOperation

ISubOperation::TPtr CreateNewReplication(TOperationId id, const TTxTransaction& tx) {
    return MakeSubOperation<TCreateReplication>(id, tx, &ReplicationStrategy);
}

ISubOperation::TPtr CreateNewReplication(TOperationId id, TTxState::ETxState state) {
    return MakeSubOperation<TCreateReplication>(id, state, &ReplicationStrategy);
}

ISubOperation::TPtr CreateNewTransfer(TOperationId id, const TTxTransaction& tx) {
    return MakeSubOperation<TCreateReplication>(id, tx, &TransferStrategy);
}

ISubOperation::TPtr CreateNewTransfer(TOperationId id, TTxState::ETxState state) {
    return MakeSubOperation<TCreateReplication>(id, state, &TransferStrategy);
}

}
