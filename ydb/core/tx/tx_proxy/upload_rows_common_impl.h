#pragma once

#include <ydb/core/actorlib_impl/long_timer.h>

#include <ydb/core/tx/long_tx_service/public/events.h>
#include <ydb/core/grpc_services/local_rpc/local_rpc.h>
#include <ydb/core/formats/arrow/arrow_batch_builder.h>
#include <ydb/core/formats/arrow/converter.h>
#include <ydb/core/io_formats/arrow/scheme/scheme.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/base/path.h>
#include <ydb/core/base/feature_flags.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/scheme/scheme_type_info.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/tx_proxy/upload_rows_counters.h>
#include <ydb/core/formats/arrow/accessor/abstract/constructor.h>
#include <ydb/core/formats/arrow/size_calcer.h>

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <ydb/library/signals/owner.h>

#include <ydb/public/api/protos/ydb_status_codes.pb.h>
#include <ydb/public/api/protos/ydb_value.pb.h>

#define INCLUDE_YDB_INTERNAL_H
#include <ydb/public/sdk/cpp/src/client/impl/ydb_internal/make_request/make.h>
#undef INCLUDE_YDB_INTERNAL_H

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/library/ydb_issue/issue_helpers.h>

#include <boost/container_hash/hash_fwd.hpp>
#include <util/generic/size_literals.h>
#include <util/string/join.h>
#include <util/string/vector.h>

namespace NKikimr {

using namespace NActors;

struct TUpsertCost {
    static constexpr float OneRowCost(ui64 sz) {
        constexpr ui64 unitSize = 1_KB;
        constexpr ui64 unitSizeAdjust = unitSize - 1;

        return (sz + unitSizeAdjust) / unitSize;
    }

    static constexpr float BatchCost(ui64 batchSize, ui32 rows) {
        constexpr ui64 unitSize = 1_KB;

        return Max<ui64>(rows, batchSize / unitSize);
    }

    static constexpr float CostToRu(float cost) {
        constexpr float ruPerKB = 0.5f; // 0.5 ru for 1 KB

        return cost * ruPerKB;
    }
};

namespace {

class TRowWriter : public NArrow::IRowWriter {
public:
    TRowWriter(TVector<std::pair<TSerializedCellVec, TString>>& rows, ui32 keySize)
        : Rows(rows)
        , KeySize(keySize)
        , RowCost(0)
    {}

    void AddRow(const TConstArrayRef<TCell>& cells) override {
        ui64 sz = 0;
        for (const auto& cell : cells) {
            sz += cell.Size();
        }
        RowCost += TUpsertCost::OneRowCost(sz);

        TConstArrayRef<TCell> keyCells = cells.first(KeySize);
        TConstArrayRef<TCell> valueCells = cells.subspan(KeySize);

        TSerializedCellVec serializedKey(keyCells);
        Rows.emplace_back(std::move(serializedKey), TSerializedCellVec::Serialize(valueCells));
    }

    float GetRuCost() const {
        return TUpsertCost::CostToRu(RowCost);
    }

private:
    TVector<std::pair<TSerializedCellVec, TString>>& Rows;
    ui32 KeySize;
    float RowCost;
};

}

namespace NTxProxy {

TActorId DoLongTxWriteSameMailbox(const TActorContext& ctx, const TActorId& replyTo,
    const NLongTxService::TLongTxId& longTxId, const TString& dedupId,
    const TString& databaseName, const TString& path,
    std::shared_ptr<const NSchemeCache::TSchemeCacheNavigate> navigateResult, std::shared_ptr<arrow::RecordBatch> batch,
    std::shared_ptr<NYql::TIssues> issues);

template <NKikimrServices::TActivity::EType DerivedActivityType>
class TUploadRowsBase : public TActorBootstrapped<TUploadRowsBase<DerivedActivityType>> {
    using TBase = TActorBootstrapped<TUploadRowsBase<DerivedActivityType>>;
    using TThis = typename TBase::TThis;

private:
    using TTabletId = ui64;

    static constexpr TDuration DEFAULT_TIMEOUT = TDuration::Seconds(5*60);

    struct TShardUploadRetryState {
        // Contains basic request settings like table ids and columns
        NKikimrTxDataShard::TEvUploadRowsRequest Headers;
        TVector<std::pair<TString, TString>> Rows;
        ui64 LastOverloadSeqNo = 0;
        ui64 SentOverloadSeqNo = 0;
    };

    TActorId SchemeCache;
    TActorId LeaderPipeCache;
    TDuration Timeout;
    TInstant StartTime;
    std::optional<TInstant> StartCommitTime;
    TActorId TimeoutTimerActorId;

    TAutoPtr<NSchemeCache::TSchemeCacheRequest> ResolvePartitionsResult;
    std::shared_ptr<NSchemeCache::TSchemeCacheNavigate> ResolveNamesResult;
    TSerializedCellVec MinKey;
    TSerializedCellVec MaxKey;
    TVector<NScheme::TTypeInfo> KeyColumnTypes;
    TVector<NScheme::TTypeInfo> ValueColumnTypes;
    NSchemeCache::TSchemeCacheNavigate::EKind TableKind = NSchemeCache::TSchemeCacheNavigate::KindUnknown;
    bool IsIndexImplTable = false;
    THashSet<TTabletId> ShardRepliesLeft;
    THashMap<TTabletId, TShardUploadRetryState> ShardUploadRetryStates;
    TUploadStatus Status;
    std::shared_ptr<NYql::TIssues> Issues = std::make_shared<NYql::TIssues>();
    NLongTxService::TLongTxId LongTxId;
    TUploadCounters UploadCounters;
    TUploadCounters::TGuard UploadCountersGuard;

protected:
    enum class EUploadSource {
        ProtoValues = 0,
        ArrowBatch = 1,
        CSV = 2,
    };
public:
    // Positions of key and value fields in the request proto struct
    struct TFieldDescription {
        ui32 ColId;
        TString ColName;
        ui32 PositionInStruct;
        NScheme::TTypeInfo Type;
        i32 Typmod;
        bool NotNull = false;
    };
protected:
    TVector<TString> KeyColumnNames;
    TVector<TFieldDescription> KeyColumnPositions;
    TVector<TString> ValueColumnNames;
    TVector<TFieldDescription> ValueColumnPositions;

    // Additional schema info (for OLAP dst or source format)
    TVector<std::pair<TString, NScheme::TTypeInfo>> SrcColumns; // source columns in CSV could have any order
    TVector<std::pair<TString, NScheme::TTypeInfo>> YdbSchema;
    std::set<std::string> NotNullColumns;
    THashMap<ui32, size_t> Id2Position; // columnId -> its position in YdbSchema
    THashMap<TString, NScheme::TTypeInfo> ColumnsToConvert;
    THashMap<TString, NScheme::TTypeInfo> ColumnsToConvertInplace;

    bool WriteToTableShadow = false;
    bool AllowWriteToPrivateTable = false;
    bool AllowWriteToIndexImplTable = false;
    bool DiskQuotaExceeded = false;
    bool UpsertIfExists = false;

    std::shared_ptr<arrow::RecordBatch> Batch;
    float RuCost = 0.0;

    NWilson::TSpan Span;

    NSchemeCache::TSchemeCacheNavigate::EKind GetTableKind() const {
        return TableKind;
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return DerivedActivityType;
    }

    explicit TUploadRowsBase(TDuration timeout = TDuration::Max(), bool diskQuotaExceeded = false, NWilson::TSpan span = {})
        : TBase()
        , SchemeCache(MakeSchemeCacheID())
        , LeaderPipeCache(MakePipePerNodeCacheID(false))
        , Timeout((timeout && timeout <= DEFAULT_TIMEOUT) ? timeout : DEFAULT_TIMEOUT)
        , Status(Ydb::StatusIds::SUCCESS)
        , UploadCountersGuard(UploadCounters.BuildGuard(TMonotonic::Now()))
        , DiskQuotaExceeded(diskQuotaExceeded)
        , Span(std::move(span))
    {}

    void Bootstrap(const NActors::TActorContext& ctx) {
        StartTime = TAppData::TimeProvider->Now();
        OnBeforeStart(ctx);
        ResolveTable(GetTable(), ctx);
    }

    void Die(const NActors::TActorContext& ctx) override {
        for (auto& pr : ShardUploadRetryStates) {
            if (pr.second.SentOverloadSeqNo) {
                auto* msg = new TEvDataShard::TEvOverloadUnsubscribe(pr.second.SentOverloadSeqNo);
                ctx.Send(LeaderPipeCache, new TEvPipeCache::TEvForward(msg, pr.first, false), 0, 0, Span.GetTraceId());
            }
        }
        ctx.Send(LeaderPipeCache, new TEvPipeCache::TEvUnlink(0), 0, 0, Span.GetTraceId());
        if (TimeoutTimerActorId) {
            ctx.Send(TimeoutTimerActorId, new TEvents::TEvPoisonPill());
        }
        TBase::Die(ctx);
    }

protected:
    TInstant Deadline() const {
        return StartTime + Timeout;
    }

    const NSchemeCache::TSchemeCacheNavigate* GetResolveNameResult() const {
        return ResolveNamesResult.get();
    }

    const TKeyDesc* GetKeyRange() const {
        Y_ABORT_UNLESS(ResolvePartitionsResult->ResultSet.size() == 1);
        return ResolvePartitionsResult->ResultSet[0].KeyDescription.Get();
    }

    bool IsInfinityInJsonAllowed() const {
        if (TableKind != NSchemeCache::TSchemeCacheNavigate::KindColumnTable) {
            return false;
        }
        switch (AppDataVerified().ColumnShardConfig.GetDoubleOutOfRangeHandling()) {
            case NKikimrConfig::TColumnShardConfig_EJsonDoubleOutOfRangeHandlingPolicy_REJECT:
                return false;
            case NKikimrConfig::TColumnShardConfig_EJsonDoubleOutOfRangeHandlingPolicy_CAST_TO_INFINITY:
                return true;
        }
    }

private:
    virtual void OnBeforeStart(const TActorContext&) {
        // nothing by default
    }

    virtual void OnBeforePoison(const TActorContext&) {
        // nothing by default
    }

    virtual TString GetDatabase() = 0;
    virtual const TString& GetTable() = 0;
    virtual const TVector<std::pair<TSerializedCellVec, TString>>& GetRows() const = 0;
    virtual bool CheckAccess(TString& errorMessage) = 0;
    virtual TConclusion<TVector<std::pair<TString, Ydb::Type>>> GetRequestColumns() const = 0;
    virtual bool ExtractRows(TString& errorMessage) = 0;
    virtual bool ExtractBatch(TString& errorMessage) = 0;
    virtual void RaiseIssue(const NYql::TIssue& issue) = 0;
    virtual void SendResult(const NActors::TActorContext& ctx, const ::Ydb::StatusIds::StatusCode& status) = 0;
    virtual void AuditContextStart() {}
    virtual bool ValidateTable(TString& errorMessage) {
        Y_UNUSED(errorMessage);
        return true;
    }

    virtual EUploadSource GetSourceType() const {
        return EUploadSource::ProtoValues;
    }

    virtual const TString& GetSourceData() const {
        static const TString none;
        return none;
    }

    virtual const TString& GetSourceSchema() const {
        static const TString none;
        return none;
    }

private:
    void Handle(TEvents::TEvPoison::TPtr&, const TActorContext& ctx) {
        OnBeforePoison(ctx);
        Span.EndError("poison");
        Die(ctx);
    }

private:
    STFUNC(StateWaitResolveTable) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);
            HFunc(TEvents::TEvPoison, Handle);

            default:
                break;
        }
    }

    TStringBuilder LogPrefix() {
        return TStringBuilder() << "Bulk upsert to table '" << GetTable() << "' ";
    }

    static bool SameDstType(NScheme::TTypeInfo type1, NScheme::TTypeInfo type2, bool allowConvert) {
        bool res = (type1 == type2);
        if (!res && allowConvert) {
            auto arrowType1 = NArrow::GetArrowType(type1);
            auto arrowType2 = NArrow::GetArrowType(type2);
            if (arrowType1.ok() && arrowType2.ok()) {
                res = (arrowType1.ValueUnsafe()->id() == arrowType2.ValueUnsafe()->id());
            }
        }
        return res;
    }

    static bool SameOrConvertableDstType(NScheme::TTypeInfo type1, NScheme::TTypeInfo type2, bool allowConvert) {
        bool ok = SameDstType(type1, type2, allowConvert) || NArrow::TArrowToYdbConverter::NeedInplaceConversion(type1, type2);
        if (!ok && allowConvert) {
            ok = NArrow::TArrowToYdbConverter::NeedConversion(type1, type2);
        }
        return ok;
    }

    [[nodiscard]] TConclusionStatus BuildSchema(const NActors::TActorContext& ctx, bool makeYqbSchema) {
        Y_UNUSED(ctx);
        Y_ABORT_UNLESS(ResolveNamesResult);
        AFL_VERIFY(ResolveNamesResult->ResultSet.size() == 1);

        auto& entry = ResolveNamesResult->ResultSet.front();

        TVector<ui32> keyColumnIds;
        THashMap<TString, ui32> columnByName;
        THashSet<TString> keyColumnsLeft;
        THashSet<TString> notNullColumnsLeft = entry.NotNullColumns;
        SrcColumns.reserve(entry.Columns.size());
        THashSet<TString> HasInternalConversion;

        for (const auto& [_, colInfo] : entry.Columns) {
            ui32 id = colInfo.Id;
            auto& name = colInfo.Name;
            auto& type = colInfo.PType;
            SrcColumns.emplace_back(name, type); // TODO: is it in correct order ?

            columnByName[name] = id;
            i32 keyOrder = colInfo.KeyOrder;
            if (keyOrder != -1) {
                Y_ABORT_UNLESS(keyOrder >= 0);
                KeyColumnTypes.resize(Max<size_t>(KeyColumnTypes.size(), keyOrder + 1));
                KeyColumnTypes[keyOrder] = type;
                keyColumnIds.resize(Max<size_t>(keyColumnIds.size(), keyOrder + 1));
                keyColumnIds[keyOrder] = id;
                keyColumnsLeft.insert(name);
            }
        }

        if (entry.ColumnTableInfo) {
            for (const auto& colInfo : entry.ColumnTableInfo->Description.GetSchema().GetColumns()) {
                auto& name = colInfo.GetName();
                NArrow::NAccessor::TConstructorContainer accessor;
                if (colInfo.HasDataAccessorConstructor()) {
                    if (!accessor.DeserializeFromProto(colInfo.GetDataAccessorConstructor())) {
                        return TConclusionStatus::Fail("cannot parse accessor for column: " + name);
                    }
                    if (accessor->HasInternalConversion()) {
                        HasInternalConversion.emplace(name);
                    }
                }
            }
        }

        KeyColumnPositions.resize(KeyColumnTypes.size());
        KeyColumnNames.resize(KeyColumnTypes.size());

        auto reqColumns = GetRequestColumns();
        if (reqColumns.IsFail()) {
            return reqColumns;
        } else if (reqColumns->empty()) {
            for (auto& [name, typeInfo] : SrcColumns) {
                Ydb::Type ydbType;
                ProtoFromTypeInfo(typeInfo, ydbType);
                reqColumns->emplace_back(name, std::move(ydbType));
            }
        }

        for (size_t pos = 0; pos < reqColumns->size(); ++pos) {
            auto& name = (*reqColumns)[pos].first;
            const auto* cp = columnByName.FindPtr(name);
            if (!cp) {
                return TConclusionStatus::Fail(Sprintf("Unknown column: %s", name.c_str()));
            }
            i32 pgTypeMod = -1;
            const ui32 colId = *cp;
            auto& ci = *entry.Columns.FindPtr(colId);

            TString columnTypeName = NScheme::TypeName(ci.PType, ci.PTypeMod);

            const Ydb::Type& typeInProto = (*reqColumns)[pos].second;

            TString parseProtoError;
            NScheme::TTypeInfoMod inTypeInfoMod;
            if (!NScheme::TypeInfoFromProto(typeInProto, inTypeInfoMod, parseProtoError)){
                return TConclusionStatus::Fail(Sprintf("Type parse error for column %s: %s",
                    name.c_str(), parseProtoError.c_str()));
            }

            const NScheme::TTypeInfo& typeInRequest = inTypeInfoMod.TypeInfo;

            TString inTypeName = NScheme::TypeName(typeInRequest, typeInRequest.GetPgTypeMod(ci.PTypeMod));

                if (typeInProto.has_type_id()) {
                    bool sourceIsArrow = GetSourceType() != EUploadSource::ProtoValues;
                bool ok = SameOrConvertableDstType(typeInRequest, ci.PType, sourceIsArrow); // TODO
                    if (!ok) {
                    return TConclusionStatus::Fail(Sprintf("Type mismatch, got type %s for column %s, but expected %s",
                        inTypeName.c_str(), name.c_str(), columnTypeName.c_str()));
                    }
                    if (NArrow::TArrowToYdbConverter::NeedInplaceConversion(typeInRequest, ci.PType)) {
                        ColumnsToConvertInplace[name] = ci.PType;
                    }
                } else if (typeInProto.has_decimal_type()) {
                    if (typeInRequest != ci.PType) {
                    return TConclusionStatus::Fail(Sprintf("Type mismatch, got type %s for column %s, but expected %s",
                        inTypeName.c_str(), name.c_str(), columnTypeName.c_str()));
                    }
                } else if (typeInProto.has_pg_type()) {
                    bool ok = SameDstType(typeInRequest, ci.PType, false);
                    if (!ok) {
                    return TConclusionStatus::Fail(Sprintf("Type mismatch, got type %s for column %s, but expected %s",
                        inTypeName.c_str(), name.c_str(), columnTypeName.c_str()));
                    }
                    if (!ci.PTypeMod.empty() && NPg::TypeDescNeedsCoercion(typeInRequest.GetPgTypeDesc())) {
                        if (inTypeInfoMod.TypeMod != ci.PTypeMod) {
                            return TConclusionStatus::Fail(Sprintf("Typemod mismatch, got type %s for column %s, type mod %s, but expected %s",
                                inTypeName.c_str(), name.c_str(), inTypeInfoMod.TypeMod.c_str(), ci.PTypeMod.c_str()));
                        }

                        const auto result = NPg::BinaryTypeModFromTextTypeMod(inTypeInfoMod.TypeMod, typeInRequest.GetPgTypeDesc());
                        if (result.Error) {
                            return TConclusionStatus::Fail(Sprintf("Invalid typemod %s, got type %s for column %s, error %s",
                                inTypeInfoMod.TypeMod.c_str(), inTypeName.c_str(), name.c_str(), result.Error->c_str()));
                        }
                        pgTypeMod = result.Typmod;
                    }
                }

            bool notNull = entry.NotNullColumns.contains(ci.Name);
            if (notNull) {
                notNullColumnsLeft.erase(ci.Name);
                NotNullColumns.emplace(ci.Name);
            }

            if (ci.KeyOrder != -1) {
                KeyColumnPositions[ci.KeyOrder] = TFieldDescription{ci.Id, ci.Name, (ui32)pos, ci.PType, pgTypeMod, notNull};
                keyColumnsLeft.erase(ci.Name);
                KeyColumnNames[ci.KeyOrder] = ci.Name;
            } else {
                ValueColumnPositions.emplace_back(TFieldDescription{ci.Id, ci.Name, (ui32)pos, ci.PType, pgTypeMod, notNull});
                ValueColumnNames.emplace_back(ci.Name);
                ValueColumnTypes.emplace_back(ci.PType);
            }
        }

        std::unordered_set<std::string_view> UpdatingValueColumns;
        if (UpsertIfExists) {
            for(const auto& name: ValueColumnNames) {
                UpdatingValueColumns.emplace(name);
            }
        }

        for (const auto& index : entry.Indexes) {
            if (index.GetType() == NKikimrSchemeOp::EIndexTypeGlobalAsync &&
                AppData(ctx)->FeatureFlags.GetEnableBulkUpsertToAsyncIndexedTables()) {
                continue;
            }

            bool allowUpdate = UpsertIfExists;
            for(auto& column : index.GetKeyColumnNames()) {
                allowUpdate &= (UpdatingValueColumns.find(column) == UpdatingValueColumns.end());
                if (!allowUpdate) {
                    break;
                }
            }

            for(auto& column : index.GetDataColumnNames()) {
                allowUpdate &= (UpdatingValueColumns.find(column) == UpdatingValueColumns.end());
                if (!allowUpdate) {
                    break;
                }
            }

            if (!allowUpdate) {
                return TConclusionStatus::Fail("Only async-indexed tables are supported by BulkUpsert");
            }
        }

        if (makeYqbSchema) {
            Id2Position.clear();
            YdbSchema.resize(KeyColumnTypes.size() + ValueColumnTypes.size());

            for (size_t i = 0; i < KeyColumnPositions.size(); ++i) {
                ui32 columnId = KeyColumnPositions[i].ColId;
                Id2Position[columnId] = i;
                YdbSchema[i] = std::make_pair(KeyColumnNames[i], KeyColumnPositions[i].Type);
            }
            for (size_t i = 0; i < ValueColumnPositions.size(); ++i) {
                ui32 columnId = ValueColumnPositions[i].ColId;
                size_t position = KeyColumnPositions.size() + i;
                Id2Position[columnId] = position;
                YdbSchema[position] = std::make_pair(ValueColumnNames[i], ValueColumnPositions[i].Type);
            }

            for (const auto& [colName, colType] : YdbSchema) {
                if (HasInternalConversion.contains(colName)) {
                    continue;
                }
                if (NArrow::TArrowToYdbConverter::NeedDataConversion(colType)) {
                    ColumnsToConvert[colName] = colType;
                }
            }
        }

        if (!keyColumnsLeft.empty()) {
            return TConclusionStatus::Fail(Sprintf("Missing key columns: %s", JoinSeq(", ", keyColumnsLeft).c_str()));
        }

        if (!notNullColumnsLeft.empty() && UpsertIfExists) {
            // columns are not specified but upsert is executed in update mode
            // and we will not change these not null columns.
            notNullColumnsLeft.clear();
        }

        if (!notNullColumnsLeft.empty()) {
            return TConclusionStatus::Fail(Sprintf("Missing not null columns: %s", JoinSeq(", ", notNullColumnsLeft).c_str()));
        }

        return TConclusionStatus::Success();
    }

    void ResolveTable(const TString& table, const NActors::TActorContext& ctx) {
        // TODO: check all params;
        // Cerr << *Request->GetProtoRequest() << Endl;

        Span && Span.Event("ResolveTable", {{"table", table}});

        AuditContextStart();

        TAutoPtr<NSchemeCache::TSchemeCacheNavigate> request(new NSchemeCache::TSchemeCacheNavigate());
        NSchemeCache::TSchemeCacheNavigate::TEntry entry;
        entry.Path = ::NKikimr::SplitPath(table);
        if (entry.Path.empty()) {
            return ReplyWithError(
                Ydb::StatusIds::SCHEME_ERROR, TStringBuilder() << "Bulk upsert. Invalid table path specified: '" << table << "'", ctx);
        }
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
        entry.SyncVersion = true;
        entry.ShowPrivatePath = AllowWriteToPrivateTable;
        request->ResultSet.emplace_back(entry);
        ctx.Send(SchemeCache, new TEvTxProxySchemeCache::TEvNavigateKeySet(request), 0, 0, Span.GetTraceId());

        TimeoutTimerActorId = CreateLongTimer(ctx, Timeout,
            new IEventHandle(ctx.SelfID, ctx.SelfID, new TEvents::TEvWakeup()));

        TBase::Become(&TThis::StateWaitResolveTable);
    }

    void HandleTimeout(const TActorContext& ctx) {
        ShardRepliesLeft.clear();
        return ReplyWithError(Ydb::StatusIds::TIMEOUT,
            TStringBuilder() << "longTx " << LongTxId.ToString()
                             << " timed out, duration: " << (TAppData::TimeProvider->Now() - StartTime).Seconds() << " sec",
            ctx);
    }

    bool IsTimestampColumnsArePositive(const std::shared_ptr<arrow::RecordBatch>& batch, TString& error) {
        if (!batch) {
            return true;
        }
        for (int i = 0; i < batch->num_columns(); ++i) {
            std::shared_ptr<arrow::Array> column = batch->column(i);
            std::shared_ptr<arrow::DataType> type = column->type();
            std::string columnName = batch->schema()->field(i)->name();
            if (type->id() == arrow::Type::TIMESTAMP) {
                auto timestampArray = std::static_pointer_cast<arrow::TimestampArray>(column);
                for (int64_t j = 0; j < timestampArray->length(); ++j) {
                    if (timestampArray->IsValid(j)) {
                        int64_t timestampValue = timestampArray->Value(j);
                        if (timestampValue < 0) {
                            error = TStringBuilder{} << "Negative timestamp value found at column " << columnName << ", row " << j << ", value " << timestampValue;
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev, const TActorContext& ctx) {
        Span && Span.Event("DataSerialization");
        const NSchemeCache::TSchemeCacheNavigate& request = *ev->Get()->Request;

        Y_ABORT_UNLESS(request.ResultSet.size() == 1);
        const NSchemeCache::TSchemeCacheNavigate::TEntry& entry = request.ResultSet.front();

        if (entry.Status != NSchemeCache::TSchemeCacheNavigate::EStatus::Ok) {
            return ReplyWithError(entry.Status, ctx);
        }

        TableKind = entry.Kind;
        const bool isColumnTable = (TableKind == NSchemeCache::TSchemeCacheNavigate::KindColumnTable);
        IsIndexImplTable = (entry.TableKind != NSchemeCache::ETableKind::KindRegularTable);

        if (entry.TableId.IsSystemView() || entry.Kind == NSchemeCache::TSchemeCacheNavigate::KindSysView) {
            return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "is not supported. Table is a system view", ctx);
        }

        // TODO: fast fail for all tables?
        if (isColumnTable && HasAppData() && !AppDataVerified().ColumnShardConfig.GetProxyWritingEnabled()) {
            return ReplyWithError(TUploadStatus(Ydb::StatusIds::UNAVAILABLE, TUploadStatus::ECustomSubcode::DELIVERY_PROBLEM,
                                      "cannot perform writes: disabled by config"),
                ctx);
        }
        if (isColumnTable && DiskQuotaExceeded) {
            return ReplyWithError(TUploadStatus(Ydb::StatusIds::UNAVAILABLE, TUploadStatus::ECustomSubcode::DISK_QUOTA_EXCEEDED,
                                      "cannot perform writes: database is out of disk space"),
                ctx);
        }

        ResolveNamesResult.reset(ev->Get()->Request.Release());

        TString errorMessage;
        if (!ValidateTable(errorMessage)) {
            return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, errorMessage, ctx);
        }

        bool makeYdbSchema = isColumnTable || (GetSourceType() != EUploadSource::ProtoValues);
        {
            auto conclusion = BuildSchema(ctx, makeYdbSchema);
            if (conclusion.IsFail()) {
                return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, conclusion.GetErrorMessage(), ctx);
            }
        }

        switch (GetSourceType()) {
            case EUploadSource::ProtoValues:
            {
                if (!ExtractRows(errorMessage)) {
                    return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, errorMessage, ctx);
                }

                if (isColumnTable) {
                    // TUploadRowsRPCPublic::ExtractBatch() - converted JsonDocument, DynNumbers, ...
                    if (!ExtractBatch(errorMessage)) {
                        return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, errorMessage, ctx);
                    }
                } else {
                    FindMinMaxKeys();
                }
                break;
            }
            case EUploadSource::ArrowBatch:
            case EUploadSource::CSV:
            {
                if (isColumnTable) {
                    // TUploadColumnsRPCPublic::ExtractBatch() - NOT converted JsonDocument, DynNumbers, ...
                    if (!ExtractBatch(errorMessage)) {
                        return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, errorMessage, ctx);
                    }
                    if (!ColumnsToConvertInplace.empty()) {
                        auto convertResult = NArrow::InplaceConvertColumns(Batch, ColumnsToConvertInplace);
                        if (!convertResult.ok()) {
                            return ReplyWithError(Ydb::StatusIds::BAD_REQUEST,
                                TStringBuilder() << "Cannot convert arrow batch inplace:" << convertResult.status().ToString(), ctx);
                        }
                        Batch = *convertResult;
                    }
                    // Explicit types conversion
                    if (!ColumnsToConvert.empty()) {
                        auto convertResult = NArrow::ConvertColumns(Batch, ColumnsToConvert, IsInfinityInJsonAllowed());
                        if (!convertResult.ok()) {
                            return ReplyWithError(Ydb::StatusIds::BAD_REQUEST,
                                TStringBuilder() << "Cannot convert arrow batch:" << convertResult.status().ToString(), ctx);
                        }
                        Batch = *convertResult;
                    }
                } else {
                    // TUploadColumnsRPCPublic::ExtractBatch() - NOT converted JsonDocument, DynNumbers, ...
                    if (!ExtractBatch(errorMessage)) {
                        return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, errorMessage, ctx);
                    }
                    // Implicit types conversion inside ExtractRows(), in TArrowToYdbConverter
                    if (!ExtractRows(errorMessage)) {
                        return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, errorMessage, ctx);
                    }
                    FindMinMaxKeys();
                }

                // (re)calculate RuCost for batch variant if it's bigger then RuCost calculated in ExtractRows()
                Y_ABORT_UNLESS(Batch && Batch->num_rows() >= 0);
                ui32 numRows = Batch->num_rows();
                ui64 bytesSize = Max<ui64>(NArrow::GetBatchDataSize(Batch), GetSourceData().size());
                float batchRuCost = TUpsertCost::CostToRu(TUpsertCost::BatchCost(bytesSize, numRows));
                if (batchRuCost > RuCost) {
                    RuCost = batchRuCost;
                }

                break;
            }
        }

        TString error;
        if (!IsTimestampColumnsArePositive(Batch, error)) {
            return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, error, ctx);
        }

        if (Batch) {
            UploadCounters.OnRequest(Batch->num_rows());
        }

        if (TableKind == NSchemeCache::TSchemeCacheNavigate::KindTable) {
            ResolveShards(ctx);
        } else if (isColumnTable) {
            // Batch is already converted
            WriteToColumnTable(ctx);
        } else {
            return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "is not supported", ctx);
        }
    }

    void WriteToColumnTable(const NActors::TActorContext& ctx) {
        UploadCountersGuard.OnWritingStarted();
        TString accessCheckError;
        if (!CheckAccess(accessCheckError)) {
            return ReplyWithError(Ydb::StatusIds::UNAUTHORIZED, accessCheckError, ctx);
        }
        if (IsIndexImplTable && !AllowWriteToIndexImplTable) {
            return ReplyWithError(
                Ydb::StatusIds::BAD_REQUEST,
                "Writing to index implementation tables is not allowed.",
                ctx);
        }

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "starting LongTx");

        // Begin Long Tx for writing a batch into OLAP table
        TActorId longTxServiceId = NLongTxService::MakeLongTxServiceID(ctx.SelfID.NodeId());
        NKikimrLongTxService::TEvBeginTx::EMode mode = NKikimrLongTxService::TEvBeginTx::MODE_WRITE_ONLY;
        ctx.Send(longTxServiceId, new NLongTxService::TEvLongTxService::TEvBeginTx(GetDatabase(), mode), 0, 0, Span.GetTraceId());
        TBase::Become(&TThis::StateWaitBeginLongTx);
    }

    STFUNC(StateWaitBeginLongTx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NLongTxService::TEvLongTxService::TEvBeginTxResult, Handle);
            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);
            HFunc(TEvents::TEvPoison, Handle);
        }
    }

    void Handle(NLongTxService::TEvLongTxService::TEvBeginTxResult::TPtr& ev, const TActorContext& ctx) {
        const auto* msg = ev->Get();

        if (msg->Record.GetStatus() != Ydb::StatusIds::SUCCESS) {
            NYql::TIssues issues;
            NYql::IssuesFromMessage(msg->Record.GetIssues(), issues);
            for (const auto& issue: issues) {
                RaiseIssue(issue);
            }
            return ReplyWithResult(msg->Record.GetStatus(), ctx);
        }

        LongTxId = msg->GetLongTxId();

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, TStringBuilder() << "started LongTx '" << LongTxId.ToString() << "'");

        auto outputColumns = GetOutputColumns(ctx);
        if (!outputColumns.empty()) {
            if (!Batch) {
                return ReplyWithError(Ydb::StatusIds::BAD_REQUEST, "no data or conversion error", ctx);
            }

            auto batch = NArrow::TColumnOperator().ErrorIfAbsent().Extract(Batch, outputColumns);
            if (!batch) {
                for (auto& columnName : outputColumns) {
                    if (Batch->schema()->GetFieldIndex(columnName) < 0) {
                        return ReplyWithError(
                            Ydb::StatusIds::SCHEME_ERROR, TStringBuilder() << "no expected column '" << columnName << "' in data", ctx);
                    }
                }
                return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "cannot prepare data", ctx);
            }

            Y_ABORT_UNLESS(batch);

#if 1 // TODO: check we call ValidateFull() once over pipeline (upsert -> long tx -> shard insert)
            auto validationInfo = batch->ValidateFull();
            if (!validationInfo.ok()) {
                return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR,
                    TStringBuilder() << "bad batch in data: " + validationInfo.message() << "; order:" + JoinSeq(", ", outputColumns), ctx);
            }
#endif

            Batch = batch;
        }

        WriteBatchInLongTx(ctx);
    }

    std::vector<TString> GetOutputColumns(const NActors::TActorContext& ctx) {
        Y_ABORT_UNLESS(ResolveNamesResult);

        if (ResolveNamesResult->ErrorCount > 0) {
            ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "failed to get table schema", ctx);
            return {};
        }

        auto& entry = ResolveNamesResult->ResultSet[0];

        if (entry.Kind != NSchemeCache::TSchemeCacheNavigate::KindColumnTable) {
            ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "specified path is not a column table", ctx);
            return {};
        }

        if (!entry.ColumnTableInfo || !entry.ColumnTableInfo->Description.HasSchema()) {
            ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "column table has no schema", ctx);
            return {};
        }

        const auto& description = entry.ColumnTableInfo->Description;
        const auto& schema = description.GetSchema();

        std::vector<TString> outColumns;
        outColumns.reserve(YdbSchema.size());

        for (size_t i = 0; i < (size_t)schema.GetColumns().size(); ++i) {
            auto columnId = schema.GetColumns(i).GetId();
            if (!Id2Position.count(columnId)) {
                continue;
            }
            size_t position = Id2Position[columnId];
            outColumns.push_back(YdbSchema[position].first);
        }

        Y_ABORT_UNLESS(!outColumns.empty());
        return outColumns;
    }

    void WriteBatchInLongTx(const TActorContext& ctx) {
        Y_ABORT_UNLESS(ResolveNamesResult);
        Y_ABORT_UNLESS(Batch);

        TBase::Become(&TThis::StateWaitWriteBatchResult);
        ui32 batchNo = 0;
        TString dedupId = ToString(batchNo);
        DoLongTxWriteSameMailbox(
            ctx, ctx.SelfID, LongTxId, dedupId, GetDatabase(), GetTable(), ResolveNamesResult, Batch, Issues);
    }

    void RollbackLongTx(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, TStringBuilder() << "rolling back LongTx '" << LongTxId.ToString() << "'");

        TActorId longTxServiceId = NLongTxService::MakeLongTxServiceID(ctx.SelfID.NodeId());
        ctx.Send(longTxServiceId, new NLongTxService::TEvLongTxService::TEvRollbackTx(LongTxId), 0, 0, Span.GetTraceId());
    }

    STFUNC(StateWaitWriteBatchResult) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvCompleted, HandleWriteBatchResult);
            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);
            HFunc(TEvents::TEvPoison, Handle);
        }
    }

    void HandleWriteBatchResult(TEvents::TEvCompleted::TPtr& ev, const TActorContext& ctx) {
        Ydb::StatusIds::StatusCode status = (Ydb::StatusIds::StatusCode)ev->Get()->Status;
        if (status != Ydb::StatusIds::SUCCESS) {
            Y_ABORT_UNLESS(Issues);
            for (const auto& issue: *Issues) {
                RaiseIssue(issue);
            }
            return ReplyWithResult(status, ctx);
        } else {
            return ReplyWithResult(status, ctx);
        }
    }

    void CommitLongTx(const TActorContext& ctx) {
        UploadCountersGuard.OnCommitStarted();
        TActorId longTxServiceId = NLongTxService::MakeLongTxServiceID(ctx.SelfID.NodeId());
        ctx.Send(longTxServiceId, new NLongTxService::TEvLongTxService::TEvCommitTx(LongTxId), 0, 0, Span.GetTraceId());
        TBase::Become(&TThis::StateWaitCommitLongTx);
    }

    STFUNC(StateWaitCommitLongTx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NLongTxService::TEvLongTxService::TEvCommitTxResult, Handle);
            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);
            HFunc(TEvents::TEvPoison, Handle);
        }
    }

    void Handle(NLongTxService::TEvLongTxService::TEvCommitTxResult::TPtr& ev, const NActors::TActorContext& ctx) {
        UploadCountersGuard.OnCommitFinished();
        const auto* msg = ev->Get();

        if (msg->Record.GetStatus() == Ydb::StatusIds::SUCCESS) {
            // We are done with the transaction, forget it
            LongTxId = NLongTxService::TLongTxId();
        }

        NYql::TIssues issues;
        NYql::IssuesFromMessage(msg->Record.GetIssues(), issues);
        for (const auto& issue: issues) {
            RaiseIssue(issue);
        }
        return ReplyWithResult(msg->Record.GetStatus(), ctx);
    }

    void FindMinMaxKeys() {
        for (const auto& pair : GetRows()) {
             const auto& serializedKey = pair.first;

            if (MinKey.GetCells().empty()) {
                // Only for the first key
                MinKey = serializedKey;
                MaxKey = serializedKey;
            } else {
                // For all next keys
                if (CompareTypedCellVectors(serializedKey.GetCells().data(), MinKey.GetCells().data(),
                                            KeyColumnTypes.data(),
                                            serializedKey.GetCells().size(), MinKey.GetCells().size()) < 0)
                {
                    MinKey = serializedKey;
                } else if (CompareTypedCellVectors(serializedKey.GetCells().data(), MaxKey.GetCells().data(),
                                                   KeyColumnTypes.data(),
                                                   serializedKey.GetCells().size(), MaxKey.GetCells().size()) > 0)
                {
                    MaxKey = serializedKey;
                }
            }
        }
    }

    void ResolveShards(const NActors::TActorContext& ctx) {
        Span && Span.Event("ResolveShards");
        if (GetRows().empty()) {
            // We have already resolved the table and know it exists
            // No reason to resolve table range as well
            return ReplyIfDone(ctx);
        }

        Y_ABORT_UNLESS(ResolveNamesResult);

        auto& entry = ResolveNamesResult->ResultSet.front();

        // We are going to set all columns
        TVector<TKeyDesc::TColumnOp> columns;
        for (const auto& ci : entry.Columns) {
            TKeyDesc::TColumnOp op = { ci.second.Id, TKeyDesc::EColumnOperation::Set, ci.second.PType, 0, 0 };
            columns.push_back(op);
        }

        TTableRange range(MinKey.GetCells(), true, MaxKey.GetCells(), true, false);
        auto keyRange = MakeHolder<TKeyDesc>(entry.TableId, range, TKeyDesc::ERowOperation::Update, KeyColumnTypes, columns);

        TAutoPtr<NSchemeCache::TSchemeCacheRequest> request(new NSchemeCache::TSchemeCacheRequest());

        request->ResultSet.emplace_back(std::move(keyRange));

        TAutoPtr<TEvTxProxySchemeCache::TEvResolveKeySet> resolveReq(new TEvTxProxySchemeCache::TEvResolveKeySet(request));
        ctx.Send(SchemeCache, resolveReq.Release(), 0, 0, Span.GetTraceId());

        TBase::Become(&TThis::StateWaitResolveShards);
    }

    STFUNC(StateWaitResolveShards) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvTxProxySchemeCache::TEvResolveKeySetResult, Handle);
            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);
            HFunc(TEvents::TEvPoison, Handle);

            default:
                break;
        }
    }

    void Handle(TEvTxProxySchemeCache::TEvResolveKeySetResult::TPtr &ev, const TActorContext &ctx) {
        TEvTxProxySchemeCache::TEvResolveKeySetResult *msg = ev->Get();
        ResolvePartitionsResult = msg->Request;

        if (ResolvePartitionsResult->ErrorCount > 0) {
            return ReplyWithError(Ydb::StatusIds::SCHEME_ERROR, "unknown table", ctx);
        }

        TString accessCheckError;
        if (!CheckAccess(accessCheckError)) {
            return ReplyWithError(Ydb::StatusIds::UNAUTHORIZED, accessCheckError, ctx);
        }
        if (IsIndexImplTable && !AllowWriteToIndexImplTable) {
            return ReplyWithError(
                Ydb::StatusIds::BAD_REQUEST,
                "Writing to index implementation tables is not allowed.",
                ctx);
        }

        auto getShardsString = [] (const TVector<TKeyDesc::TPartitionInfo>& partitions) {
            TVector<ui64> shards;
            shards.reserve(partitions.size());
            for (auto& partition : partitions) {
                shards.push_back(partition.ShardId);
            }

            return JoinVectorIntoString(shards, ", ");
        };

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "Range shards: "
            << getShardsString(GetKeyRange()->GetPartitions()));

        MakeShardRequests(ctx);
    }

    void RetryShardRequest(ui64 shardId, TShardUploadRetryState* state, const TActorContext& ctx) {
        Y_ABORT_UNLESS(ShardRepliesLeft.contains(shardId));

        auto ev = std::make_unique<TEvDataShard::TEvUploadRowsRequest>();
        ev->Record = state->Headers;
        for (const auto& pr : state->Rows) {
            auto* row = ev->Record.AddRows();
            row->SetKeyColumns(pr.first);
            row->SetValueColumns(pr.second);
        }

        // Mark our request as supporting overload subscriptions
        ui64 seqNo = ++state->LastOverloadSeqNo;
        ev->Record.SetOverloadSubscribe(seqNo);
        state->SentOverloadSeqNo = seqNo;

        ctx.Send(LeaderPipeCache, new TEvPipeCache::TEvForward(ev.release(), shardId, true), IEventHandle::FlagTrackDelivery, 0, Span.GetTraceId());
    }

    void MakeShardRequests(const NActors::TActorContext& ctx) {
        Span && Span.Event("MakeShardRequests", {{"rows", long(GetRows().size())}});
        const auto* keyRange = GetKeyRange();

        Y_ABORT_UNLESS(!keyRange->GetPartitions().empty());

        // Group rows by shard id
        TVector<TShardUploadRetryState*> uploadRetryStates(keyRange->GetPartitions().size());
        TVector<std::unique_ptr<TEvDataShard::TEvUploadRowsRequest>> shardRequests(keyRange->GetPartitions().size());
        for (const auto& keyValue : GetRows()) {
            // Find partition for the key
            auto it = std::lower_bound(keyRange->GetPartitions().begin(), keyRange->GetPartitions().end(), keyValue.first.GetCells(),
                [this](const auto &partition, const auto& key) {
                    const auto& range = *partition.Range;
                    const int cmp = CompareBorders<true, false>(range.EndKeyPrefix.GetCells(), key,
                        range.IsInclusive || range.IsPoint, true, KeyColumnTypes);

                    return (cmp < 0);
                });

            size_t shardIdx = it - keyRange->GetPartitions().begin();

            auto* retryState = uploadRetryStates[shardIdx];
            if (!retryState) {
                TTabletId shardId = it->ShardId;
                retryState = uploadRetryStates[shardIdx] = &ShardUploadRetryStates[shardId];
            }

            TEvDataShard::TEvUploadRowsRequest* ev = shardRequests[shardIdx].get();
            if (!ev) {
                shardRequests[shardIdx].reset(new TEvDataShard::TEvUploadRowsRequest());
                ev = shardRequests[shardIdx].get();
                ev->Record.SetCancelDeadlineMs(Deadline().MilliSeconds());

                ev->Record.SetTableId(keyRange->TableId.PathId.LocalPathId);
                if (keyRange->TableId.SchemaVersion) {
                    ev->Record.SetSchemaVersion(keyRange->TableId.SchemaVersion);
                }
                for (const auto& fd : KeyColumnPositions) {
                    ev->Record.MutableRowScheme()->AddKeyColumnIds(fd.ColId);
                }
                for (const auto& fd : ValueColumnPositions) {
                    ev->Record.MutableRowScheme()->AddValueColumnIds(fd.ColId);
                }
                if (WriteToTableShadow) {
                    ev->Record.SetWriteToTableShadow(true);
                }
                if (UpsertIfExists) {
                    ev->Record.SetUpsertIfExists(true);
                }
                // Copy protobuf settings without rows
                retryState->Headers = ev->Record;
            }

            TString keyColumns = keyValue.first.GetBuffer();
            TString valueColumns = keyValue.second;

            // We expect to keep a reference to existing key and value data here
            uploadRetryStates[shardIdx]->Rows.emplace_back(keyColumns, valueColumns);

            auto* row = ev->Record.AddRows();
            row->SetKeyColumns(std::move(keyColumns));
            row->SetValueColumns(std::move(valueColumns));
        }

        // Send requests to the shards
        for (size_t idx = 0; idx < shardRequests.size(); ++idx) {
            auto& ev = shardRequests[idx];
            if (!ev)
                continue;

            TTabletId shardId = keyRange->GetPartitions()[idx].ShardId;

            LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "Sending request to shards " << shardId);

            // Mark our request as supporting overload subscriptions
            ui64 seqNo = ++uploadRetryStates[idx]->LastOverloadSeqNo;
            ev->Record.SetOverloadSubscribe(seqNo);
            uploadRetryStates[idx]->SentOverloadSeqNo = seqNo;

            ctx.Send(LeaderPipeCache, new TEvPipeCache::TEvForward(ev.release(), shardId, true), IEventHandle::FlagTrackDelivery, 0, Span.GetTraceId());

            auto res = ShardRepliesLeft.insert(shardId);
            if (!res.second) {
                LOG_CRIT_S(ctx, NKikimrServices::RPC_REQUEST, "Upload rows: shard " << shardId << "has already been added!");
            }
        }

        TBase::Become(&TThis::StateWaitResults);
        Span && Span.Event("WaitResults", {{"shardRequests", long(shardRequests.size())}});

        // Sanity check: don't break when we don't have any shards for some reason
        return ReplyIfDone(ctx);
    }

    void Handle(TEvents::TEvUndelivered::TPtr &ev, const TActorContext &ctx) {
        Y_UNUSED(ev);
        SetError(TUploadStatus(
            Ydb::StatusIds::INTERNAL_ERROR, "Internal error: pipe cache is not available, the cluster might not be configured properly"));

        ShardRepliesLeft.clear();

        return ReplyIfDone(ctx);
    }

    void Handle(TEvPipeCache::TEvDeliveryProblem::TPtr &ev, const TActorContext &ctx) {
        ctx.Send(SchemeCache, new TEvTxProxySchemeCache::TEvInvalidateTable(GetKeyRange()->TableId, TActorId()), 0, 0, Span.GetTraceId());

        SetError(TUploadStatus(Ydb::StatusIds::UNAVAILABLE, TUploadStatus::ECustomSubcode::DELIVERY_PROBLEM,
            Sprintf("Failed to connect to shard %" PRIu64, ev->Get()->TabletId)));
        ShardRepliesLeft.erase(ev->Get()->TabletId);

        return ReplyIfDone(ctx);
    }

    STFUNC(StateWaitResults) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvDataShard::TEvUploadRowsResponse, Handle);
            HFunc(TEvDataShard::TEvOverloadReady, Handle);
            HFunc(TEvents::TEvUndelivered, Handle);
            HFunc(TEvPipeCache::TEvDeliveryProblem, Handle);
            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);
            HFunc(TEvents::TEvPoison, Handle);

            default:
                break;
        }
    }

    void Handle(TEvDataShard::TEvUploadRowsResponse::TPtr& ev, const NActors::TActorContext& ctx) {
        const auto& shardResponse = ev->Get()->Record;

        ui64 shardId = shardResponse.GetTabletID();

        Span && Span.Event("TEvUploadRowsResponse", {{"shardId", long(shardId)}});

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "Upload rows: got "
                    << NKikimrTxDataShard::TError::EKind_Name((NKikimrTxDataShard::TError::EKind)shardResponse.GetStatus())
                    << " from shard " << shardResponse.GetTabletID());

        if (shardResponse.GetStatus() != NKikimrTxDataShard::TError::OK) {
            if (shardResponse.GetStatus() == NKikimrTxDataShard::TError::WRONG_SHARD_STATE ||
                shardResponse.GetStatus() == NKikimrTxDataShard::TError::SHARD_IS_BLOCKED) {
                ctx.Send(SchemeCache, new TEvTxProxySchemeCache::TEvInvalidateTable(GetKeyRange()->TableId, TActorId()), 0, 0, Span.GetTraceId());
            }

            if (auto* state = ShardUploadRetryStates.FindPtr(shardId)) {
                if (!shardResponse.HasOverloadSubscribed()) {
                    // Shard doesn't support overload subscriptions for this request
                    state->SentOverloadSeqNo = 0;
                } else if (shardResponse.GetOverloadSubscribed() == state->SentOverloadSeqNo) {
                    // Wait until shard notifies us it is possible to write again
                    LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "Upload rows: subscribed to overload change at shard " << shardId);
                    return;
                }
            }

            SetError(
                TUploadStatus(static_cast<NKikimrTxDataShard::TError::EKind>(shardResponse.GetStatus()), shardResponse.GetErrorDescription()));
        }

        // Notify the cache that we are done with the pipe
        ctx.Send(LeaderPipeCache, new TEvPipeCache::TEvUnlink(shardId), 0, 0, Span.GetTraceId());

        ShardRepliesLeft.erase(shardId);
        ShardUploadRetryStates.erase(shardId);

        return ReplyIfDone(ctx);
    }

    void Handle(TEvDataShard::TEvOverloadReady::TPtr& ev, const TActorContext& ctx) {
        auto& record = ev->Get()->Record;
        ui64 shardId = record.GetTabletID();
        ui64 seqNo = record.GetSeqNo();

        Span && Span.Event("TEvOverloadReady", {{"shardId", long(shardId)}});

        if (auto* state = ShardUploadRetryStates.FindPtr(shardId)) {
            if (state->SentOverloadSeqNo && state->SentOverloadSeqNo == seqNo && ShardRepliesLeft.contains(shardId)) {
                RetryShardRequest(shardId, state, ctx);
            }
        }
    }

    void SetError(const TUploadStatus& status) {
        if (Status.GetCode() != ::Ydb::StatusIds::SUCCESS) {
            return;
        }

        Status = status;
    }

    void ReplyIfDone(const NActors::TActorContext& ctx) {
        if (!ShardRepliesLeft.empty()) {
            LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, "Upload rows: waiting for " << ShardRepliesLeft.size() << " shards replies");
            return;
        }

        if (Status.GetErrorMessage()) {
            RaiseIssue(NYql::TIssue(LogPrefix() << *Status.GetErrorMessage()));
        }

        return ReplyWithResult(Status, ctx);
    }

    void ReplyWithError(const Ydb::StatusIds::StatusCode code, const TString& errorMessage, const TActorContext& ctx) {
        return ReplyWithError(TUploadStatus(code, errorMessage), ctx);
    }

    void ReplyWithError(const TUploadStatus& status, const TActorContext& ctx) {
        AFL_VERIFY(status.GetCode() != Ydb::StatusIds::SUCCESS);
        LOG_NOTICE_S(ctx, NKikimrServices::RPC_REQUEST, LogPrefix() << status.GetErrorMessage());

        SetError(status);

        Y_DEBUG_ABORT_UNLESS(ShardRepliesLeft.empty());
        return ReplyIfDone(ctx);
    }

    void ReplyWithResult(const TUploadStatus& status, const TActorContext& ctx) {
        UploadCountersGuard.OnReply(status);
        SendResult(ctx, status.GetCode());

        LOG_DEBUG_S(ctx, NKikimrServices::RPC_REQUEST, TStringBuilder() << "completed with status " << status.GetCode());

        if (LongTxId != NLongTxService::TLongTxId()) {
            // LongTxId is reset after successful commit
            // If it si still there it means we need to rollback
            RollbackLongTx(ctx);
        }
        Span.EndOk();

        Die(ctx);
    }
};

using TFieldDescription = NTxProxy::TUploadRowsBase<NKikimrServices::TActivity::GRPC_REQ>::TFieldDescription;

template <class TProto>
inline bool FillCellsFromProto(TVector<TCell>& cells, const TVector<TFieldDescription>& descr, const TProto& proto, TString& err,
    TMemoryPool& valueDataPool, const bool allowInfDouble = false) {
    cells.clear();
    cells.reserve(descr.size());

    for (auto& fd : descr) {
        if (proto.items_size() <= (int)fd.PositionInStruct) {
            err = "Invalid request";
            return false;
        }
        cells.push_back({});
        if (!CellFromProtoVal(
                fd.Type, fd.Typmod, &proto.Getitems(fd.PositionInStruct), false, cells.back(), err, valueDataPool, allowInfDouble)) {
            return false;
        }

        if (fd.NotNull && cells.back().IsNull()) {
            err = TStringBuilder() << "Received NULL value for not null column: " << fd.ColName;
            return false;
        }
    }

    return true;
}

} // namespace NTxProxy
} // namespace NKikimr
