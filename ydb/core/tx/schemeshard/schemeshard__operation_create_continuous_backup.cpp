#include "schemeshard__backup_collection_common.h"
#include "schemeshard__operation_common.h"
#include "schemeshard__operation_create_cdc_stream.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_impl.h"

#include <ydb/core/engine/mkql_proto.h>
#include <ydb/core/scheme/scheme_types_proto.h>

#define LOG_D(stream) LOG_DEBUG_S (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_I(stream) LOG_INFO_S  (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)

namespace NKikimr::NSchemeShard {

TVector<ISubOperation::TPtr> CreateNewContinuousBackup(TOperationId opId, const TTxTransaction& tx, TOperationContext& context) {
    Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpCreateContinuousBackup);

    LOG_D("CreateNewContinuousBackup"
        << ": opId# " << opId
        << ", tx# " << tx.ShortDebugString());

    const auto acceptExisted = !tx.GetFailOnExist();
    const auto workingDirPath = TPath::Resolve(tx.GetWorkingDir(), context.SS);
    const auto& cbOp = tx.GetCreateContinuousBackup();
    const auto& tableName = cbOp.GetTableName();

    TString streamName;
    if (cbOp.GetContinuousBackupDescription().HasStreamName()) {
        streamName = cbOp.GetContinuousBackupDescription().GetStreamName();
    } else {
        streamName = NBackup::ToX509String(TlsActivationContext->AsActorContext().Now()) + "_continuousBackupImpl";
    }

    const auto checksResult = NCdc::DoNewStreamPathChecks(context, opId, workingDirPath, tableName, streamName, acceptExisted);
    if (std::holds_alternative<ISubOperation::TPtr>(checksResult)) {
        return {std::get<ISubOperation::TPtr>(checksResult)};
    }

    const auto [tablePath, streamPath] = std::get<NCdc::TStreamPaths>(checksResult);

    // TODO check that table doesn't have continuous backup already

    Y_ABORT_UNLESS(context.SS->Tables.contains(tablePath.Base()->PathId));
    auto table = context.SS->Tables.at(tablePath.Base()->PathId);

    TString errStr;
    if (!context.SS->CheckApplyIf(tx, errStr)) {
        return {CreateReject(opId, NKikimrScheme::StatusPreconditionFailed, errStr)};
    }

    if (!context.SS->CheckLocks(tablePath.Base()->PathId, tx, errStr)) {
        return {CreateReject(opId, NKikimrScheme::StatusMultipleModifications, errStr)};
    }

    TVector<TString> boundaries;
    const auto& partitions = table->GetPartitions();
    boundaries.reserve(partitions.size() - 1);

    for (ui32 i = 0; i < partitions.size(); ++i) {
        const auto& partition = partitions.at(i);
        if (i != partitions.size() - 1) {
            boundaries.push_back(partition.EndOfRange);
        }
    }

    NKikimrSchemeOp::TCreateCdcStream createCdcStreamOp;
    createCdcStreamOp.SetTableName(tableName);
    auto& streamDescription = *createCdcStreamOp.MutableStreamDescription();
    streamDescription.SetName(streamName);
    streamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeNewImage);
    streamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);

    TVector<ISubOperation::TPtr> result;

    NCdc::DoCreateStream(result, createCdcStreamOp, opId, workingDirPath, tablePath, acceptExisted, false);
    NCdc::DoCreatePqPart(result, createCdcStreamOp, opId, streamPath, streamName, table, boundaries, acceptExisted);

    return result;
}

} // namespace NKikimr::NSchemeShard
