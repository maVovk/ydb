#include <ydb/core/kqp/runtime/scheduler/tree/dynamic.h>
#include <ydb/core/kqp/runtime/scheduler/tree/snapshot.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/core/kqp/common/events/events.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/kqp/runtime/scheduler/kqp_compute_scheduler_service.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status_codes.h>

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>

namespace NKikimr::NKqp::NScheduler {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {
    void ExamineSnapshot(NHdrf::NSnapshot::TTreeElement* snapshot) {
        if (snapshot->IsRoot()) {
            Cout << "Recalculating FairShare" << Endl;
            snapshot->UpdateBottomUp(100);
            snapshot->UpdateTopDown(true);
        }

        Cout << "TreeElement ";
        if (snapshot->IsPool()) {
            Cout << "pool ";
        } else if (!snapshot->IsRoot()) {
            Cout << "query ";
        }

        if (snapshot->IsPool()) {
            Cout << std::get<NScheduler::NHdrf::TPoolId>(snapshot->GetId());
        } else {
            Cout << std::get<NScheduler::NHdrf::TQueryId>(snapshot->GetId());
        }
        Cout << " fair share " << snapshot->FairShare << " demand " << snapshot->Demand << Endl;

        snapshot->ForEachChild<NHdrf::NSnapshot::TTreeElement>([&](NHdrf::NSnapshot::TTreeElement* child, size_t) {
            ExamineSnapshot(child);
        });
    } 
}  // namespace

Y_UNIT_TEST_SUITE(TKqpSchedulerService) {

    // Y_UNIT_TEST(TestSnapshot) {
    //     TKikimrSettings settings = TKikimrSettings().SetUseRealThreads(false);
    //     settings.AppConfig.MutableTableServiceConfig()->MutableComputeSchedulerSettings()->SetAccountDefaultPool(true);

    //     TKikimrRunner kikimr(settings);
    //     auto db = kikimr.RunCall([&] { return kikimr.GetTableClient(); } );
    //     auto session = kikimr.RunCall([&] { return db.CreateSession().GetValueSync().GetSession(); } );

    //     auto prepareResult = kikimr.RunCall([&] { return session.PrepareDataQuery(Q_(R"(
    //             SELECT COUNT(*), SUM(Key), SUM(Value2) FROM `/Root/TwoShard`;
    //         )")).GetValueSync();
    //     });
    //     UNIT_ASSERT_VALUES_EQUAL_C(prepareResult.GetStatus(), EStatus::SUCCESS, prepareResult.GetIssues().ToString());
    //     auto dataQuery = prepareResult.GetQuery();

    //     auto& runtime = *kikimr.GetTestServer().GetRuntime();

    //     TActorId schedulerId;
    //     NHdrf::NSnapshot::TRootPtr lastSnapshot;

    //     THashMap<TString, THashMap<TString, TVector<NHdrf::TId>>> compGraph;
    //     THashMap<NHdrf::TId, NHdrf::NDynamic::TQueryPtr> queryMap;

    //     runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
    //         {
    //             TStringStream ss;
    //             ss << "Got " << ev->GetTypeName() << " " << ev->Recipient << " " << ev->Sender << Endl;
    //             Cerr << ss.Str();
    //         }

    //         if (ev->GetTypeRewrite() == NScheduler::TEvDumpSnapshotResponse::EventType) {
    //             Cerr << "Got Snapshot dump" << Endl;
    //             lastSnapshot = ev->Get<NScheduler::TEvDumpSnapshotResponse>()->Snapshot;
    //             ExamineSnapshot(lastSnapshot.get());
    //             return TTestActorRuntime::EEventAction::DROP;
    //         }
    //         if (ev->GetTypeRewrite() == NScheduler::TEvAddDatabase::EventType) {
    //             auto databaseId = ev->Get<NScheduler::TEvAddDatabase>()->Id;
    //             Cerr << "Adding database " << databaseId << " dumping snapshot" <<  Endl;
    //             compGraph[databaseId] = THashMap<TString, TVector<NHdrf::TId>>();
    //         }
    //         if (ev->GetTypeRewrite() == NScheduler::TEvAddPool::EventType) {
    //             auto pool = ev->Get<NScheduler::TEvAddPool>();
    //             Cerr << "Adding pool " << pool->PoolId << Endl;
    //             compGraph[pool->DatabaseId][pool->PoolId] = TVector<NHdrf::TId>();
    //         }
    //         if (ev->GetTypeRewrite() == NScheduler::TEvAddQuery::EventType) {
    //             auto query = ev->Get<NScheduler::TEvAddQuery>();
    //             Cerr << "Adding query " << query->QueryId << " to db " << query->DatabaseId << " and pool " << query->PoolId << Endl;

    //             compGraph[query->DatabaseId][query->PoolId].push_back(query->QueryId);
    //             queryMap[query->QueryId] = nullptr;
    //         }
    //         if (ev->GetTypeRewrite() == NScheduler::TEvQueryResponse::EventType) {
    //             auto query = ev->Get<NScheduler::TEvQueryResponse>()->Query;
    //             queryMap[query->GetId()] = query;
    //             schedulerId = ev->Sender;

    //             Cerr << "Created query " << query->GetId() << Endl;
    //         }
    //         if (ev->GetTypeRewrite() == NYql::NDq::TEvDqCompute::TEvState::EventType) {
    //             runtime.Send(new IEventHandle(schedulerId, ev->Sender, new NScheduler::TEvDumpSnapshot()));
    //         }

    //         return TTestActorRuntime::EEventAction::PROCESS;
    //     });

    //     auto future = kikimr.RunInThreadPool([&] { return dataQuery.Execute(TTxControl::BeginTx().CommitTx(), TExecDataQuerySettings()).GetValueSync(); });

    //     TDispatchOptions opts;
    //     opts.FinalEvents.emplace_back([&](IEventHandle& ev) {
    //         return ev.GetTypeRewrite() == TEvKqpExecuter::TEvTxResponse::EventType;
    //     });
    //     runtime.DispatchEvents(opts);

    //     auto result = runtime.WaitFuture(future);
    //     // UNIT_ASSERT(result.IsSuccess());
    //     UNIT_ASSERT(false);
    // }
}

} // namespace NKikimr
