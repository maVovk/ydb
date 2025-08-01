#pragma once

#include "client_common.h"

#include <yt/yt_proto/yt/client/chunk_client/proto/data_statistics.pb.h>

#include <yt/yt/client/query_tracker_client/public.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

struct TQueryTrackerOptions
{
    std::string QueryTrackerStage = NQueryTrackerClient::ProductionStage;
};

DEFINE_ENUM(EContentType,
    ((RawInlineData)   (0))
    ((Url)   (1))
);

struct TQueryFile
    : public NYTree::TYsonStruct
{
    TString Name;
    TString Content;
    EContentType Type;

    REGISTER_YSON_STRUCT(TQueryFile);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TQueryFile)

struct TQuerySecret
    : public NYTree::TYsonStruct
{
    TString Id;
    TString Category;
    TString Subcategory;
    TString YPath;

    REGISTER_YSON_STRUCT(TQuerySecret);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TQuerySecret)

struct TStartQueryOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    NYTree::INodePtr Settings;
    bool Draft = false;
    NYTree::IMapNodePtr Annotations;
    std::vector<TQueryFilePtr> Files;
    std::optional<std::string> AccessControlObject; // COMPAT(mpereskokova)
    std::optional<std::vector<std::string>> AccessControlObjects;
    std::vector<TQuerySecretPtr> Secrets;
};

struct TAbortQueryOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    std::optional<TString> AbortMessage;
};

struct TGetQueryResultOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{ };

struct TReadQueryResultOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    std::optional<std::vector<std::string>> Columns;
    std::optional<i64> LowerRowIndex;
    std::optional<i64> UpperRowIndex;
};

struct TGetQueryOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    NYTree::TAttributeFilter Attributes;
    NTransactionClient::TTimestamp Timestamp = NTransactionClient::NullTimestamp;
};

struct TListQueriesOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    std::optional<TInstant> FromTime;
    std::optional<TInstant> ToTime;
    std::optional<TInstant> CursorTime;
    EOperationSortDirection CursorDirection = EOperationSortDirection::Past;
    std::optional<std::string> UserFilter;

    std::optional<NQueryTrackerClient::EQueryState> StateFilter;
    std::optional<NQueryTrackerClient::EQueryEngine> EngineFilter;
    std::optional<TString> SubstrFilter;
    ui64 Limit = 100;

    NYTree::TAttributeFilter Attributes;
};

struct TQuery
{
    NQueryTrackerClient::TQueryId Id;
    std::optional<NQueryTrackerClient::EQueryEngine> Engine;
    std::optional<TString> Query;
    std::optional<NYson::TYsonString> Files;
    std::optional<TInstant> StartTime;
    std::optional<TInstant> FinishTime;
    NYson::TYsonString Settings;
    std::optional<std::string> User;
    std::optional<TString> AccessControlObject; // COMPAT(mpereskokova)
    std::optional<NYson::TYsonString> AccessControlObjects;
    std::optional<NQueryTrackerClient::EQueryState> State;
    std::optional<i64> ResultCount;
    NYson::TYsonString Progress;
    std::optional<TError> Error;
    NYson::TYsonString Annotations;
    NYTree::IAttributeDictionaryPtr OtherAttributes;
    std::optional<NYson::TYsonString> Secrets;
};

void Serialize(const TQuery& query, NYson::IYsonConsumer* consumer);

struct TQueryResult
{
    NQueryTrackerClient::TQueryId Id;
    i64 ResultIndex;
    TError Error;
    NTableClient::TTableSchemaPtr Schema;
    NChunkClient::NProto::TDataStatistics DataStatistics;
    bool IsTruncated;
    NYson::TYsonString FullResult;
};

void Serialize(const TQueryResult& queryResult, NYson::IYsonConsumer* consumer);

struct TListQueriesResult
{
    std::vector<TQuery> Queries;
    bool Incomplete = false;
    NTransactionClient::TTimestamp Timestamp;
};

struct TAlterQueryOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    NYTree::IMapNodePtr Annotations;
    std::optional<std::string> AccessControlObject; // COMPAT(mpereskokova)
    std::optional<std::vector<std::string>> AccessControlObjects;
};

struct TGetQueryTrackerInfoOptions
    : public TTimeoutOptions
    , public TQueryTrackerOptions
{
    NYTree::TAttributeFilter Attributes;
    NYTree::INodePtr Settings;
};

struct TGetQueryTrackerInfoResult
{
    std::string QueryTrackerStage;
    std::string ClusterName;
    NYson::TYsonString SupportedFeatures;
    std::vector<std::string> AccessControlObjects;
    std::vector<std::string> Clusters;
    NYson::TYsonString EnginesInfo;
};

////////////////////////////////////////////////////////////////////////////////

struct IQueryTrackerClient
{
    virtual ~IQueryTrackerClient() = default;

    virtual TFuture<NQueryTrackerClient::TQueryId> StartQuery(
        NQueryTrackerClient::EQueryEngine engine,
        const TString& query,
        const TStartQueryOptions& options = {}) = 0;

    virtual TFuture<void> AbortQuery(
        NQueryTrackerClient::TQueryId queryId,
        const TAbortQueryOptions& options = {}) = 0;

    virtual TFuture<TQueryResult> GetQueryResult(
        NQueryTrackerClient::TQueryId queryId,
        i64 resultIndex = 0,
        const TGetQueryResultOptions& options = {}) = 0;

    virtual TFuture<IUnversionedRowsetPtr> ReadQueryResult(
        NQueryTrackerClient::TQueryId queryId,
        i64 resultIndex = 0,
        const TReadQueryResultOptions& options = {}) = 0;

    virtual TFuture<TQuery> GetQuery(
        NQueryTrackerClient::TQueryId queryId,
        const TGetQueryOptions& options = {}) = 0;

    virtual TFuture<TListQueriesResult> ListQueries(const TListQueriesOptions& options = {}) = 0;

    virtual TFuture<void> AlterQuery(
        NQueryTrackerClient::TQueryId queryId,
        const TAlterQueryOptions& options = {}) = 0;

    virtual TFuture<TGetQueryTrackerInfoResult> GetQueryTrackerInfo(const TGetQueryTrackerInfoOptions& options = {}) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi

