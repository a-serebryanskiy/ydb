#pragma once
#include <ydb/core/kqp/common/simple/kqp_event_ids.h>
#include <ydb/core/protos/kqp.pb.h>
#include <ydb/core/protos/kqp_stats.pb.h>
#include <ydb/core/protos/kqp_physical.pb.h>
#include <yql/essentials/public/issue/yql_issue.h>
#include <ydb/public/api/protos/ydb_operation.pb.h>
#include <ydb/public/api/protos/ydb_query.pb.h>
#include <ydb/public/api/protos/ydb_status_codes.pb.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/library/operation_id/operation_id.h>

#include <ydb/library/actors/core/event_local.h>

#include <util/generic/maybe.h>

#include <google/protobuf/any.pb.h>

namespace NKikimr::NKqp {

enum EFinalizationStatus : i32 {
    FS_COMMIT,
    FS_ROLLBACK,
};

template <typename TEv, ui32 TEventType>
struct TEventWithDatabaseId : public NActors::TEventLocal<TEv, TEventType> {
    TEventWithDatabaseId(const TString& database)
        : Database(database)
    {}

    const TString& GetDatabase() const {
        return Database;
    }

    const TString& GetDatabaseId() const {
        return DatabaseId;
    }

    void SetDatabaseId(const TString& databaseId) {
        DatabaseId = databaseId;
    }

    const TString Database;
    TString DatabaseId;
};

struct TEvForgetScriptExecutionOperation : public TEventWithDatabaseId<TEvForgetScriptExecutionOperation, TKqpScriptExecutionEvents::EvForgetScriptExecutionOperation> {
    TEvForgetScriptExecutionOperation(const TString& database, const NOperationId::TOperationId& id)
        : TEventWithDatabaseId(database)
        , OperationId(id)
    {}

    const NOperationId::TOperationId OperationId;
};

struct TEvForgetScriptExecutionOperationResponse : public NActors::TEventLocal<TEvForgetScriptExecutionOperationResponse, TKqpScriptExecutionEvents::EvForgetScriptExecutionOperationResponse> {
    TEvForgetScriptExecutionOperationResponse(Ydb::StatusIds::StatusCode status,  NYql::TIssues issues) 
        : Status(status)
        , Issues(issues)
    {
    }
    
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvGetScriptExecutionOperation : public TEventWithDatabaseId<TEvGetScriptExecutionOperation, TKqpScriptExecutionEvents::EvGetScriptExecutionOperation> {
    TEvGetScriptExecutionOperation(const TString& database, const NOperationId::TOperationId& id)
        : TEventWithDatabaseId(database)
        , OperationId(id)
    {}

    NOperationId::TOperationId OperationId;
};

struct TEvGetScriptExecutionOperationQueryResponse : public NActors::TEventLocal<TEvGetScriptExecutionOperationQueryResponse, TKqpScriptExecutionEvents::EvGetScriptExecutionOperationQueryResponse> {
    TEvGetScriptExecutionOperationQueryResponse(bool ready, bool leaseExpired, std::optional<EFinalizationStatus> finalizationStatus, TActorId runScriptActorId,
        const TString& executionId, Ydb::StatusIds::StatusCode status, NYql::TIssues issues, Ydb::Query::ExecuteScriptMetadata metadata)
        : Ready(ready)
        , LeaseExpired(leaseExpired)
        , FinalizationStatus(finalizationStatus)
        , RunScriptActorId(runScriptActorId)
        , ExecutionId(executionId)
        , Status(status)
        , Issues(std::move(issues))
        , Metadata(std::move(metadata))
    {}

    bool Ready;
    bool LeaseExpired;
    std::optional<EFinalizationStatus> FinalizationStatus;
    TActorId RunScriptActorId;
    TString ExecutionId;
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
    Ydb::Query::ExecuteScriptMetadata Metadata;
};

struct TEvGetScriptExecutionOperationResponse : public NActors::TEventLocal<TEvGetScriptExecutionOperationResponse, TKqpScriptExecutionEvents::EvGetScriptExecutionOperationResponse> {
    TEvGetScriptExecutionOperationResponse(bool ready, Ydb::StatusIds::StatusCode status, NYql::TIssues issues, TMaybe<google::protobuf::Any> metadata)
        : Ready(ready)
        , Status(status)
        , Issues(std::move(issues))
        , Metadata(std::move(metadata))
    {}

    TEvGetScriptExecutionOperationResponse(Ydb::StatusIds::StatusCode status, NYql::TIssues issues)
        : Ready(false)
        , Status(status)
        , Issues(std::move(issues))
    {}

    bool Ready;
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
    TMaybe<google::protobuf::Any> Metadata;
};

struct TEvListScriptExecutionOperations : public TEventWithDatabaseId<TEvListScriptExecutionOperations, TKqpScriptExecutionEvents::EvListScriptExecutionOperations> {
    TEvListScriptExecutionOperations(const TString& database, const ui64 pageSize, const TString& pageToken)
        : TEventWithDatabaseId(database)
        , PageSize(pageSize)
        , PageToken(pageToken)
    {}

    ui64 PageSize;
    TString PageToken;
};

struct TEvListScriptExecutionOperationsResponse : public NActors::TEventLocal<TEvListScriptExecutionOperationsResponse, TKqpScriptExecutionEvents::EvListScriptExecutionOperationsResponse> {
    TEvListScriptExecutionOperationsResponse(Ydb::StatusIds::StatusCode status, NYql::TIssues issues, const TString& nextPageToken, std::vector<Ydb::Operations::Operation> operations)
        : Status(status)
        , Issues(std::move(issues))
        , NextPageToken(nextPageToken)
        , Operations(std::move(operations))
    {
    }

    TEvListScriptExecutionOperationsResponse(Ydb::StatusIds::StatusCode status, NYql::TIssues issues)
        : Status(status)
        , Issues(std::move(issues))
    {
    }

    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
    TString NextPageToken;
    std::vector<Ydb::Operations::Operation> Operations;
};

struct TEvScriptLeaseUpdateResponse : public NActors::TEventLocal<TEvScriptLeaseUpdateResponse, TKqpScriptExecutionEvents::EvScriptLeaseUpdateResponse> {
    TEvScriptLeaseUpdateResponse(bool executionEntryExists, TInstant currentDeadline, Ydb::StatusIds::StatusCode status, NYql::TIssues issues)
        : ExecutionEntryExists(executionEntryExists)
        , CurrentDeadline(currentDeadline)
        , Status(status)
        , Issues(std::move(issues))
    {
    }

    bool ExecutionEntryExists;
    TInstant CurrentDeadline;
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvCheckAliveRequest : public NActors::TEventPB<TEvCheckAliveRequest, NKikimrKqp::TEvCheckAliveRequest, TKqpScriptExecutionEvents::EvCheckAliveRequest> {
};

struct TEvCheckAliveResponse : public NActors::TEventPB<TEvCheckAliveResponse, NKikimrKqp::TEvCheckAliveResponse, TKqpScriptExecutionEvents::EvCheckAliveResponse> {
};

struct TEvCancelScriptExecutionOperation : public TEventWithDatabaseId<TEvCancelScriptExecutionOperation, TKqpScriptExecutionEvents::EvCancelScriptExecutionOperation> {
    TEvCancelScriptExecutionOperation(const TString& database, const NOperationId::TOperationId& id)
        : TEventWithDatabaseId(database)
        , OperationId(id)
    {}

    NOperationId::TOperationId OperationId;
};

struct TEvCancelScriptExecutionOperationResponse : public NActors::TEventLocal<TEvCancelScriptExecutionOperationResponse, TKqpScriptExecutionEvents::EvCancelScriptExecutionOperationResponse> {
    TEvCancelScriptExecutionOperationResponse() = default;

    TEvCancelScriptExecutionOperationResponse(Ydb::StatusIds::StatusCode status, NYql::TIssues issues = {})
        : Status(status)
        , Issues(std::move(issues))
    {
    }

    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvScriptExecutionFinished : public NActors::TEventLocal<TEvScriptExecutionFinished, TKqpScriptExecutionEvents::EvScriptExecutionFinished> {
    TEvScriptExecutionFinished(bool operationAlreadyFinalized, Ydb::StatusIds::StatusCode status, NYql::TIssues issues = {})
        : OperationAlreadyFinalized(operationAlreadyFinalized)
        , Status(status)
        , Issues(std::move(issues))
    {}

    bool OperationAlreadyFinalized;
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvSaveScriptResultMetaFinished : public NActors::TEventLocal<TEvSaveScriptResultMetaFinished, TKqpScriptExecutionEvents::EvSaveScriptResultMetaFinished> {
    TEvSaveScriptResultMetaFinished(Ydb::StatusIds::StatusCode status, NYql::TIssues issues = {})
        : Status(status)
        , Issues(std::move(issues))
    {
    }

    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvSaveScriptResultPartFinished : public NActors::TEventLocal<TEvSaveScriptResultPartFinished, TKqpScriptExecutionEvents::EvSaveScriptResultPartFinished> {
    TEvSaveScriptResultPartFinished(Ydb::StatusIds::StatusCode status, i64 savedSize, NYql::TIssues issues = {})
        : Status(status)
        , SavedSize(savedSize)
        , Issues(std::move(issues))
    {}

    Ydb::StatusIds::StatusCode Status;
    i64 SavedSize;
    NYql::TIssues Issues;
};

struct TEvSaveScriptResultFinished : public NActors::TEventLocal<TEvSaveScriptResultFinished, TKqpScriptExecutionEvents::EvSaveScriptResultFinished> {
    TEvSaveScriptResultFinished(Ydb::StatusIds::StatusCode status, size_t resultSetId, NYql::TIssues issues = {})
        : Status(status)
        , ResultSetId(resultSetId)
        , Issues(std::move(issues))
    {}

    Ydb::StatusIds::StatusCode Status;
    size_t ResultSetId = 0;
    NYql::TIssues Issues;
};

struct TEvFetchScriptResultsResponse : public NActors::TEventLocal<TEvFetchScriptResultsResponse, TKqpScriptExecutionEvents::EvFetchScriptResultsResponse> {
    TEvFetchScriptResultsResponse(Ydb::StatusIds::StatusCode status, std::optional<Ydb::ResultSet>&& resultSet, bool hasMoreResults, NYql::TIssues issues)
        : Status(status)
        , ResultSet(std::move(resultSet))
        , HasMoreResults(hasMoreResults)
        , Issues(std::move(issues))
    {}

    Ydb::StatusIds::StatusCode Status;
    std::optional<Ydb::ResultSet> ResultSet;
    bool HasMoreResults;
    NYql::TIssues Issues;
};

struct TEvSaveScriptExternalEffectRequest : public NActors::TEventLocal<TEvSaveScriptExternalEffectRequest, TKqpScriptExecutionEvents::EvSaveScriptExternalEffectRequest> {
    struct TDescription {
        TDescription(const TString& executionId, const TString& database, const TString& customerSuppliedId, const TString& userToken)
            : ExecutionId(executionId)
            , Database(database)
            , CustomerSuppliedId(customerSuppliedId)
            , UserToken(userToken)
        {}

        TString ExecutionId;
        TString Database;

        TString CustomerSuppliedId;
        TString UserToken;
        std::vector<NKqpProto::TKqpExternalSink> Sinks;
        std::vector<TString> SecretNames;
    };
    
    TEvSaveScriptExternalEffectRequest(const TString& executionId, const TString& database, const TString& customerSuppliedId, const TString& userToken)
        : Description(executionId, database, customerSuppliedId, userToken)
    {}

    TDescription Description;
};

struct TEvSaveScriptExternalEffectResponse : public NActors::TEventLocal<TEvSaveScriptExternalEffectResponse, TKqpScriptExecutionEvents::EvSaveScriptExternalEffectResponse> {
    TEvSaveScriptExternalEffectResponse(Ydb::StatusIds::StatusCode status, NYql::TIssues issues)
        : Status(status)
        , Issues(std::move(issues))
    {}

    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvScriptFinalizeRequest : public NActors::TEventLocal<TEvScriptFinalizeRequest, TKqpScriptExecutionEvents::EvScriptFinalizeRequest> {
    struct TDescription {
        TDescription(EFinalizationStatus finalizationStatus, const TString& executionId, const TString& database,
        Ydb::StatusIds::StatusCode operationStatus, Ydb::Query::ExecStatus execStatus, NYql::TIssues issues, std::optional<NKqpProto::TKqpStatsQuery> queryStats,
        std::optional<TString> queryPlan, std::optional<TString> queryAst, std::optional<ui64> leaseGeneration)
            : FinalizationStatus(finalizationStatus)
            , ExecutionId(executionId)
            , Database(database)
            , OperationStatus(operationStatus)
            , ExecStatus(execStatus)
            , Issues(std::move(issues))
            , QueryStats(std::move(queryStats))
            , QueryPlan(std::move(queryPlan))
            , QueryAst(std::move(queryAst))
            , LeaseGeneration(leaseGeneration)
        {}

        EFinalizationStatus FinalizationStatus;
        TString ExecutionId;
        TString Database;
        Ydb::StatusIds::StatusCode OperationStatus;
        Ydb::Query::ExecStatus ExecStatus;
        NYql::TIssues Issues;
        std::optional<NKqpProto::TKqpStatsQuery> QueryStats;
        std::optional<TString> QueryPlan;
        std::optional<TString> QueryAst;
        std::optional<ui64> LeaseGeneration;
        std::optional<TString> QueryAstCompressionMethod;
    };

    TEvScriptFinalizeRequest(EFinalizationStatus finalizationStatus, const TString& executionId, const TString& database,
        Ydb::StatusIds::StatusCode operationStatus, Ydb::Query::ExecStatus execStatus, NYql::TIssues issues = {}, std::optional<NKqpProto::TKqpStatsQuery> queryStats = std::nullopt,
        std::optional<TString> queryPlan = std::nullopt, std::optional<TString> queryAst = std::nullopt, std::optional<ui64> leaseGeneration = std::nullopt)
        : Description(finalizationStatus, executionId, database, operationStatus, execStatus, issues, queryStats, queryPlan, queryAst, leaseGeneration)
    {}

    TDescription Description;
};

struct TEvScriptFinalizeResponse : public NActors::TEventLocal<TEvScriptFinalizeResponse, TKqpScriptExecutionEvents::EvScriptFinalizeResponse> {
    explicit TEvScriptFinalizeResponse(const TString& executionId)
        : ExecutionId(executionId)
    {}

    TString ExecutionId;
};

struct TEvSaveScriptFinalStatusResponse : public NActors::TEventLocal<TEvSaveScriptFinalStatusResponse, TKqpScriptExecutionEvents::EvSaveScriptFinalStatusResponse> {
    bool ApplicateScriptExternalEffectRequired = false;
    bool OperationAlreadyFinalized = false;
    TString CustomerSuppliedId;
    TString UserToken;
    std::vector<NKqpProto::TKqpExternalSink> Sinks;
    std::vector<TString> SecretNames;
    Ydb::StatusIds::StatusCode Status;
    NYql::TIssues Issues;
};

struct TEvDescribeSecretsResponse : public NActors::TEventLocal<TEvDescribeSecretsResponse, TKqpScriptExecutionEvents::EvDescribeSecretsResponse> {
    struct TDescription {
        TDescription(Ydb::StatusIds::StatusCode status, NYql::TIssues issues)
            : Status(status)
            , Issues(std::move(issues))
        {}

        TDescription(const std::vector<TString>& secretValues)
            : SecretValues(secretValues)
            , Status(Ydb::StatusIds::SUCCESS)
        {}

        std::vector<TString> SecretValues;
        Ydb::StatusIds::StatusCode Status;
        NYql::TIssues Issues;
    };

    TEvDescribeSecretsResponse(const TDescription& description)
        : Description(description)
    {}

    TDescription Description;
};

struct TEvScriptExecutionsTablesCreationFinished : public NActors::TEventLocal<TEvScriptExecutionsTablesCreationFinished, TKqpScriptExecutionEvents::EvScriptExecutionsTableCreationFinished> {
    TEvScriptExecutionsTablesCreationFinished(bool success, NYql::TIssues issues)
        : Success(success)
        , Issues(std::move(issues))
    {}

    const bool Success;
    const NYql::TIssues Issues;
};

} // namespace NKikimr::NKqp
