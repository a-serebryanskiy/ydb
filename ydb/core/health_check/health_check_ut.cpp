#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/unittest/tests_data.h>
#include <ydb/core/testlib/test_client.h>
#include <ydb/public/lib/deprecated/kicli/kicli.h>

#include <ydb/core/mind/hive/hive_events.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>
#include <ydb/core/blobstorage/base/blobstorage_events.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include "health_check.cpp"

#include <util/stream/null.h>

namespace NKikimr {

using namespace NSchemeShard;
using namespace Tests;
using namespace NSchemeCache;
using namespace NNodeWhiteboard;

#ifdef NDEBUG
#define Ctest Cnull
#else
#define Ctest Cerr
#endif

Y_UNIT_TEST_SUITE(THealthCheckTest) {
    void BasicTest(IEventBase* ev) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);

        auto settings = TServerSettings(port);
        settings.SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);

        TClient client(settings);
        NClient::TKikimr kikimr(client.GetClientConfig());
        client.InitRootScheme();

        TTestActorRuntime* runtime = server.GetRuntime();
        TActorId sender = runtime->AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        runtime->Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, ev, 0));
        NHealthCheck::TEvSelfCheckResult* result = runtime->GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle);

        UNIT_ASSERT(result != nullptr);
    }

    Y_UNIT_TEST(Basic) {
        BasicTest(new NHealthCheck::TEvSelfCheckRequest());
    }

    Y_UNIT_TEST(BasicNodeCheckRequest) {
        BasicTest(new NHealthCheck::TEvNodeCheckRequest());
    }

    const int GROUP_START_ID = 0x80000000;
    const int VCARD_START_ID = 55;
    const int PDISK_START_ID = 42;
    const int DEFAULT_GROUP_GENERATION = 3;

    const TPathId SUBDOMAIN_KEY = {7000000000, 1};
    const TPathId SERVERLESS_DOMAIN_KEY = {7000000000, 2};
    const TPathId SHARED_DOMAIN_KEY = {7000000000, 3};
    const TString STORAGE_POOL_NAME = "/Root:test";

    struct TTestVSlotInfo {
        std::optional<NKikimrBlobStorage::EVDiskStatus> Status;
        ui32 Generation = DEFAULT_GROUP_GENERATION;
        std::optional<NKikimrBlobStorage::TPDiskState::E> PDiskState;
        NKikimrBlobStorage::EDriveStatus PDiskStatus = NKikimrBlobStorage::ACTIVE;

        TTestVSlotInfo(std::optional<NKikimrBlobStorage::EVDiskStatus> status = NKikimrBlobStorage::READY,
                       ui32 generation = DEFAULT_GROUP_GENERATION)
            : Status(status)
            , Generation(generation)
        {
        }

        TTestVSlotInfo(NKikimrBlobStorage::EVDiskStatus status, NKikimrBlobStorage::TPDiskState::E pDiskState)
            : Status(status)
            , PDiskState(pDiskState)
        {
        }

        TTestVSlotInfo(NKikimrBlobStorage::EVDiskStatus status, NKikimrBlobStorage::EDriveStatus pDiskStatus = NKikimrBlobStorage::ACTIVE)
            : Status(status)
            , PDiskStatus(pDiskStatus)
        {
        }
    };

    using TVDisks = TVector<TTestVSlotInfo>;

    void ChangeDescribeSchemeResult(TEvSchemeShard::TEvDescribeSchemeResult::TPtr* ev, ui64 size = 20000000, ui64 quota = 90000000) {
        auto record = (*ev)->Get()->MutableRecord();
        auto pool = record->mutable_pathdescription()->mutable_domaindescription()->add_storagepools();
        pool->set_name("/Root:test");
        pool->set_kind("kind");

        auto domain = record->mutable_pathdescription()->mutable_domaindescription();
        domain->mutable_diskspaceusage()->mutable_tables()->set_totalsize(size);
        if (quota == 0) {
            domain->clear_databasequotas();
        } else {
            domain->mutable_databasequotas()->set_data_size_hard_quota(quota);
        }
        domain->SetShardsLimit(quota);
        domain->SetShardsInside(size);
    }

    void AddGroupsInControllerSelectGroupsResult(TEvBlobStorage::TEvControllerSelectGroupsResult::TPtr* ev,  int groupCount) {
        auto& pbRecord = (*ev)->Get()->Record;
        auto pbMatchGroups = pbRecord.mutable_matchinggroups(0);

        auto sample = pbMatchGroups->groups(0);
        pbMatchGroups->ClearGroups();

        auto groupId = GROUP_START_ID;
        for (int i = 0; i < groupCount; i++) {
            auto group = pbMatchGroups->add_groups();
            group->CopyFrom(sample);
            group->set_groupid(groupId++);
        }
    };

    void AddGroupVSlotInControllerConfigResponse(TEvBlobStorage::TEvControllerConfigResponse::TPtr* ev, const int groupCount, const int vslotCount) {
        auto& pbRecord = (*ev)->Get()->Record;
        auto pbConfig = pbRecord.mutable_response()->mutable_status(0)->mutable_baseconfig();

        auto groupSample = pbConfig->group(0);
        auto vslotSample = pbConfig->vslot(0);
        auto vslotIdSample = pbConfig->group(0).vslotid(0);
        pbConfig->clear_group();
        pbConfig->clear_vslot();
        for (auto& pdisk: *pbConfig->mutable_pdisk()) {
            pdisk.mutable_pdiskmetrics()->set_state(NKikimrBlobStorage::TPDiskState::Normal);
        }

        auto groupId = GROUP_START_ID;
        for (int i = 0; i < groupCount; i++) {

            auto group = pbConfig->add_group();
            group->CopyFrom(groupSample);
            group->set_groupid(groupId);
            group->set_erasurespecies(NHealthCheck::TSelfCheckRequest::BLOCK_4_2);
            group->set_operatingstatus(NKikimrBlobStorage::TGroupStatus::DEGRADED);

            group->clear_vslotid();
            auto vslotId = VCARD_START_ID;
            for (int j = 0; j < vslotCount; j++) {
                auto vslot = pbConfig->add_vslot();
                vslot->CopyFrom(vslotSample);
                vslot->set_vdiskidx(vslotId);
                vslot->set_groupid(groupId);
                vslot->set_failrealmidx(vslotId);
                vslot->set_status("ERROR");
                vslot->mutable_vslotid()->set_vslotid(vslotId);

                auto slotId = group->add_vslotid();
                slotId->CopyFrom(vslotIdSample);
                slotId->set_vslotid(vslotId);

                vslotId++;
            }
            groupId++;
        }
    };

    void AddGroupsToSysViewResponse(NSysView::TEvSysView::TEvGetGroupsResponse::TPtr* ev, size_t groupCount = 1, bool addStatic = true) {
        auto& record = (*ev)->Get()->Record;
        auto entrySample = record.entries(0);
        record.clear_entries();

        auto addGroup = [&](size_t groupId, size_t poolId) {
            auto* entry = record.add_entries();
            entry->CopyFrom(entrySample);
            entry->mutable_key()->set_groupid(groupId);
            entry->mutable_info()->set_erasurespeciesv2(NHealthCheck::TSelfCheckRequest::BLOCK_4_2);
            entry->mutable_info()->set_storagepoolid(poolId);
            entry->mutable_info()->set_generation(DEFAULT_GROUP_GENERATION);
        };

        if (addStatic) {
            addGroup(0, 0);
        }

        auto groupId = GROUP_START_ID;
        for (size_t i = 0; i < groupCount; ++i) {
            addGroup(groupId++, 1);
        }
    }

    void AddBridgeGroupsToSysViewResponse(NSysView::TEvSysView::TEvGetGroupsResponse::TPtr* ev, size_t groupCount = 1, size_t pileCount = 2) {
        auto& record = (*ev)->Get()->Record;
        auto entrySample = record.entries(0);
        record.clear_entries();

        auto addGroup = [&](size_t groupId, size_t poolId, std::optional<size_t> proxyGroup, ui32 pileId = 0) {
            auto* entry = record.add_entries();
            entry->CopyFrom(entrySample);
            entry->mutable_key()->set_groupid(groupId);
            if (proxyGroup) {
                entry->mutable_info()->set_erasurespeciesv2(NHealthCheck::TSelfCheckRequest::BLOCK_4_2);
            } else {
                entry->mutable_info()->set_erasurespeciesv2(NHealthCheck::TSelfCheckRequest::NONE);
            }
            entry->mutable_info()->set_storagepoolid(poolId);
            entry->mutable_info()->set_generation(DEFAULT_GROUP_GENERATION);
            if (proxyGroup) {
                entry->mutable_info()->set_proxygroupid(*proxyGroup);
                entry->mutable_info()->set_bridgepileid(pileId);
            }
        };

        auto groupId = GROUP_START_ID;
        for (size_t i = 0; i < groupCount; ++i) {
            size_t proxyGroup = groupId;
            addGroup(groupId++, 1, {});
            for (size_t j = 0; j < pileCount; ++j) {
                addGroup(groupId++, 1, proxyGroup, j);
            }
        }
    }

    void AddVSlotsToSysViewResponse(NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr* ev, size_t groupCount,
                                    const TVDisks& vslots, ui32 groupStartId = GROUP_START_ID,
                                    bool rewrite = true, bool withPdisk = false) {
        auto& record = (*ev)->Get()->Record;
        auto entrySample = record.entries(0);
        if (rewrite) {
            record.clear_entries();
        }

        auto groupId = groupStartId;
        const auto *descriptor = NKikimrBlobStorage::EVDiskStatus_descriptor();
        for (size_t i = 0; i < groupCount; ++i) {
            static int vslotId;
            if (rewrite) {
                vslotId = VCARD_START_ID;
            }
            auto pdiskId = PDISK_START_ID;
            for (const auto& vslot : vslots) {
                auto* entry = record.add_entries();
                entry->CopyFrom(entrySample);
                entry->mutable_key()->set_vslotid(vslotId);
                if (withPdisk) {
                    entry->mutable_key()->set_pdiskid(pdiskId);
                }
                entry->mutable_info()->set_groupid(groupId);
                entry->mutable_info()->set_failrealm(vslotId);
                if (vslot.Status) {
                    entry->mutable_info()->set_statusv2(descriptor->FindValueByNumber(*vslot.Status)->name());
                }
                entry->mutable_info()->set_groupgeneration(vslot.Generation);
                entry->mutable_info()->set_vdisk(vslotId);
                ++vslotId;
                ++pdiskId;
            }
            ++groupId;
        }
    }

    void AddStoragePoolsToSysViewResponse(NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr* ev) {
        auto& record = (*ev)->Get()->Record;
        record.clear_entries();
        auto* entry = record.add_entries();
        entry->mutable_key()->set_storagepoolid(1);
        entry->mutable_info()->set_name(STORAGE_POOL_NAME);
    }

    void AddPDisksToSysViewResponse(NSysView::TEvSysView::TEvGetPDisksResponse::TPtr* ev, const TVDisks& vslots, double occupancy) {
        auto& record = (*ev)->Get()->Record;
        auto entrySample = record.entries(0);
        record.clear_entries();
        auto pdiskId = PDISK_START_ID;
        const size_t totalSize = 3'200'000'000'000ull;
        const auto *descriptorState = NKikimrBlobStorage::TPDiskState::E_descriptor();
        const auto *descriptorStatusV2 = NKikimrBlobStorage::EDriveStatus_descriptor();
        for (const auto& vslot : vslots) {
            auto* entry = record.add_entries();
            entry->CopyFrom(entrySample);
            entry->mutable_key()->set_pdiskid(pdiskId);
            entry->mutable_info()->set_totalsize(totalSize);
            entry->mutable_info()->set_availablesize((1 - occupancy) * totalSize);
            if (vslot.PDiskState) {
                entry->mutable_info()->set_state(descriptorState->FindValueByNumber(*vslot.PDiskState)->name());
            }
            entry->mutable_info()->set_statusv2(descriptorStatusV2->FindValueByNumber(vslot.PDiskStatus)->name());
            ++pdiskId;
        }
    }

    void SetPDisksStateNormalToSysViewResponse(NSysView::TEvSysView::TEvGetPDisksResponse::TPtr* ev) {
        auto& record = (*ev)->Get()->Record;
        const auto *descriptor = NKikimrBlobStorage::TPDiskState::E_descriptor();
        const TString state = descriptor->FindValueByNumber(NKikimrBlobStorage::TPDiskState::Normal)->name();
        for (auto& entry: *record.mutable_entries()) {
            entry.mutable_info()->set_state(state);
        }
    }

    void AddGroupVSlotInControllerConfigResponseWithStaticGroup(TEvBlobStorage::TEvControllerConfigResponse::TPtr* ev,
        const NKikimrBlobStorage::TGroupStatus::E groupStatus, const TVDisks& vslots)
    {
        auto& pbRecord = (*ev)->Get()->Record;
        auto pbConfig = pbRecord.mutable_response()->mutable_status(0)->mutable_baseconfig();

        auto groupSample = pbConfig->group(0);
        auto vslotSample = pbConfig->vslot(0);
        auto vslotIdSample = pbConfig->group(0).vslotid(0);

        for (auto& pdisk: *pbConfig->mutable_pdisk()) {
            pdisk.mutable_pdiskmetrics()->set_state(NKikimrBlobStorage::TPDiskState::Normal);
        }

        pbConfig->clear_group();

        auto staticGroup = pbConfig->add_group();
        staticGroup->CopyFrom(groupSample);
        staticGroup->set_groupid(0);
        staticGroup->set_storagepoolid(0);
        staticGroup->set_operatingstatus(groupStatus);
        staticGroup->set_erasurespecies(NHealthCheck::TSelfCheckRequest::BLOCK_4_2);
        staticGroup->set_groupgeneration(DEFAULT_GROUP_GENERATION);

        auto group = pbConfig->add_group();
        group->CopyFrom(groupSample);
        group->set_groupid(GROUP_START_ID);
        group->set_storagepoolid(1);
        group->set_operatingstatus(groupStatus);
        group->set_erasurespecies(NHealthCheck::TSelfCheckRequest::BLOCK_4_2);
        group->set_groupgeneration(DEFAULT_GROUP_GENERATION);

        group->clear_vslotid();
        auto vslotId = VCARD_START_ID;

        for (const auto& vslotInfo : vslots) {
            auto vslot = pbConfig->add_vslot();
            vslot->CopyFrom(vslotSample);
            vslot->set_vdiskidx(vslotId);
            vslot->set_groupid(GROUP_START_ID);
            vslot->set_failrealmidx(vslotId);
            vslot->mutable_vslotid()->set_vslotid(vslotId);

            auto slotId = group->add_vslotid();
            slotId->CopyFrom(vslotIdSample);
            slotId->set_vslotid(vslotId);

            if (vslotInfo.Status) {
                const auto *descriptor = NKikimrBlobStorage::EVDiskStatus_descriptor();
                vslot->set_status(descriptor->FindValueByNumber(*vslotInfo.Status)->name());
            }
            vslot->set_groupgeneration(vslotInfo.Generation);

            vslotId++;
        }

        auto spStatus = pbRecord.mutable_response()->mutable_status(1);
        spStatus->clear_storagepool();
        auto sPool = spStatus->add_storagepool();
        sPool->set_storagepoolid(1);
        sPool->set_name(STORAGE_POOL_NAME);
    };

    void AddVSlotInVDiskStateResponse(TEvWhiteboard::TEvVDiskStateResponse::TPtr* ev, int groupCount, int vslotCount, ui32 groupStartId = GROUP_START_ID) {
        auto& pbRecord = (*ev)->Get()->Record;

        auto sample = pbRecord.vdiskstateinfo(0);
        pbRecord.clear_vdiskstateinfo();

        auto groupId = groupStartId;
        for (int i = 0; i < groupCount; i++) {
            auto slotId = VCARD_START_ID;
            for (int j = 0; j < vslotCount; j++) {
                auto state = pbRecord.add_vdiskstateinfo();
                state->CopyFrom(sample);
                state->mutable_vdiskid()->set_vdisk(slotId++);
                state->mutable_vdiskid()->set_groupid(groupId);
                state->set_vdiskstate(NKikimrWhiteboard::EVDiskState::SyncGuidRecovery);
                state->set_nodeid(1);
            }
            groupId++;
        }
    }

    void ChangeGroupStateResponse(NNodeWhiteboard::TEvWhiteboard::TEvBSGroupStateResponse::TPtr* ev) {
        for (auto& groupInfo : *(*ev)->Get()->Record.mutable_bsgroupstateinfo()) {
            groupInfo.set_erasurespecies(NHealthCheck::TSelfCheckRequest::BLOCK_4_2);
        }
    }

    void SetLongHostValue(TEvInterconnect::TEvNodesInfo::TPtr* ev) {
        auto nodes = MakeIntrusive<TIntrusiveVector<TEvInterconnect::TNodeInfo>>((*ev)->Get()->Nodes);
        TString host(1000000, 'a');
        for (auto it = nodes->begin(); it != nodes->end(); ++it) {
            it->Host = host;
        }
        auto newEv = IEventHandle::Downcast<TEvInterconnect::TEvNodesInfo>(
            new IEventHandle((*ev)->Recipient, (*ev)->Sender, new TEvInterconnect::TEvNodesInfo(nodes))
        );
        ev->Swap(newEv);
    }

    Ydb::Monitoring::SelfCheckResult RequestHc(size_t const groupNumber, size_t const vdiscPerGroupNumber, bool const isMergeRecords = false, bool const largeSizeVdisksIssues = false) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResult(x);
                    break;
                }
                case TEvBlobStorage::EvControllerSelectGroupsResult: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerSelectGroupsResult::TPtr*>(&ev);
                    AddGroupsInControllerSelectGroupsResult(x, groupNumber);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponse(x, groupNumber, vdiscPerGroupNumber);
                    break;
                }
                case TEvInterconnect::EvNodesInfo: {
                    if (largeSizeVdisksIssues) {
                        auto *x = reinterpret_cast<TEvInterconnect::TEvNodesInfo::TPtr*>(&ev);
                        SetLongHostValue(x);
                    }
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, groupNumber, TVDisks{vdiscPerGroupNumber, NKikimrBlobStorage::EVDiskStatus::ERROR});
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x, groupNumber, false);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_merge_records(isMergeRecords);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        return runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
    }

    void CheckHcResult(Ydb::Monitoring::SelfCheckResult& result, int const groupNumber, int const vdiscPerGroupNumber, bool const isMergeRecords = false) {
        int groupIssuesCount = 0;
        int groupIssuesNumber = !isMergeRecords ? groupNumber : 1;
        for (const auto& issue_log : result.Getissue_log()) {
            if (issue_log.type() == "STORAGE_GROUP" && issue_log.location().storage().pool().name() == "/Root:test") {
                if (!isMergeRecords || groupNumber == 1) {
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.location().storage().pool().group().id_size(), 1);
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.listed(), 0);
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.count(), 0);
                } else {
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.location().storage().pool().group().id_size(), groupNumber);
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.listed(), groupNumber);
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.count(), groupNumber);
                }
                groupIssuesCount++;
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(groupIssuesCount, groupIssuesNumber);

        int issueVdiscNumber = !isMergeRecords ? vdiscPerGroupNumber : 1;

        int issueVdiscCount = 0;
        for (const auto& issue_log : result.issue_log()) {
            if (issue_log.type() == "VDISK" && issue_log.location().storage().pool().name() == "/Root:test") {
                issueVdiscCount++;
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(issueVdiscCount, issueVdiscNumber);
    }

    bool HasTabletIssue(const Ydb::Monitoring::SelfCheckResult& result) {
       for (const auto& issue_log : result.issue_log()) {
            if (issue_log.level() == 4 && issue_log.type() == "TABLET") {
                return true;
            }
        }
        return false;
    }

    void ListingTest(int const groupNumber, int const vdiscPerGroupNumber, bool const isMergeRecords = false) {
        auto result = RequestHc(groupNumber, vdiscPerGroupNumber, isMergeRecords);
        CheckHcResult(result, groupNumber, vdiscPerGroupNumber, isMergeRecords);
    }

    Ydb::Monitoring::SelfCheckResult RequestHcWithVdisks(const NKikimrBlobStorage::TGroupStatus::E groupStatus, const TVDisks& vdisks, bool forStaticGroup = false, double occupancy = 0) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResult(x);
                    break;
                }
                case TEvBlobStorage::EvControllerSelectGroupsResult: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerSelectGroupsResult::TPtr*>(&ev);
                    AddGroupsInControllerSelectGroupsResult(x, 1);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, groupStatus, vdisks);
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    if (forStaticGroup) {
                        AddVSlotsToSysViewResponse(x, 1, vdisks, 0, true, true);
                    } else {
                        AddVSlotsToSysViewResponse(x, 1, vdisks, GROUP_START_ID, true, true);
                    }
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    AddPDisksToSysViewResponse(x, vdisks, occupancy);
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NNodeWhiteboard::TEvWhiteboard::EvVDiskStateResponse: {
                    auto *x = reinterpret_cast<NNodeWhiteboard::TEvWhiteboard::TEvVDiskStateResponse::TPtr*>(&ev);
                    if (forStaticGroup) {
                        AddVSlotInVDiskStateResponse(x, 1, vdisks.size(), 0);
                    } else {
                        AddVSlotInVDiskStateResponse(x, 1, vdisks.size());
                    }
                    break;
                }
                case NNodeWhiteboard::TEvWhiteboard::EvBSGroupStateResponse: {
                    auto* x = reinterpret_cast<NNodeWhiteboard::TEvWhiteboard::TEvBSGroupStateResponse::TPtr*>(&ev);
                    ChangeGroupStateResponse(x);
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_merge_records(true);

        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        return runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
    }

    Ydb::Monitoring::SelfCheckResult RequestHcWithBridgeVdisks(const TVector<TVDisks>& groupVdisks) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto bridgeInfo = std::make_shared<TBridgeInfo>();
        bridgeInfo->Piles.push_back(TBridgeInfo::TPile{.BridgePileId = TBridgePileId::FromValue(0), .State = NKikimrBridge::TClusterState::SYNCHRONIZED});
        bridgeInfo->Piles.push_back(TBridgeInfo::TPile{.BridgePileId = TBridgePileId::FromValue(1), .State = NKikimrBridge::TClusterState::SYNCHRONIZED});

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResult(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    bool first = true;
                    size_t groupId = GROUP_START_ID;
                    for (const auto& vdisks : groupVdisks) {
                        AddVSlotsToSysViewResponse(x, 1, vdisks, ++groupId, std::exchange(first, false));
                    }
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddBridgeGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NNodeWhiteboard::TEvWhiteboard::EvBSGroupStateResponse: {
                    auto* x = reinterpret_cast<NNodeWhiteboard::TEvWhiteboard::TEvBSGroupStateResponse::TPtr*>(&ev);
                    ChangeGroupStateResponse(x);
                    break;
                }
                case TEvBlobStorage::EvNodeWardenStorageConfig: {
                    auto* x = reinterpret_cast<TEvNodeWardenStorageConfig::TPtr*>(&ev);
                    (*x)->Get()->BridgeInfo = bridgeInfo;
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_merge_records(true);

        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        return runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
    }

    void CheckHcResultHasIssuesWithStatus(Ydb::Monitoring::SelfCheckResult& result, const TString& type,
                                          const Ydb::Monitoring::StatusFlag::Status expectingStatus, ui32 total,
                                          std::string_view pool = "/Root:test") {
        int issuesCount = 0;
        for (const auto& issue_log : result.Getissue_log()) {
            if (issue_log.type() == type && issue_log.location().storage().pool().name() == pool && issue_log.status() == expectingStatus) {
                issuesCount++;
            }
        }
        UNIT_ASSERT_VALUES_EQUAL_C(issuesCount, total, "Wrong issues count for " << type << " with expecting status " << expectingStatus);
    }

    void StorageTest(ui64 usage, ui64 quota, ui64 storageIssuesNumber, Ydb::Monitoring::StatusFlag::Status status = Ydb::Monitoring::StatusFlag::GREEN) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResult(x, usage, quota);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        NHealthCheck::TEvSelfCheckResult* result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle);

        int storageIssuesCount = 0;
        for (const auto& issue_log : result->Result.Getissue_log()) {
            if (issue_log.type() == "STORAGE" && issue_log.reason_size() == 0 && issue_log.status() == status) {
                storageIssuesCount++;
            }
        }

        UNIT_ASSERT_VALUES_EQUAL(storageIssuesCount, storageIssuesNumber);
    }

    void ShardsQuotaTest(ui64 usage, ui64 quota, ui64 storageIssuesNumber, Ydb::Monitoring::StatusFlag::Status status = Ydb::Monitoring::StatusFlag::GREEN) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResult(x, usage, quota);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        NHealthCheck::TEvSelfCheckResult* result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle);

        int storageIssuesCount = 0;
        for (const auto& issue_log : result->Result.Getissue_log()) {
            Ctest << issue_log.ShortDebugString() << Endl;
            if (issue_log.type() == "COMPUTE_QUOTA" && issue_log.reason_size() == 0 && issue_log.status() == status) {
                storageIssuesCount++;
            }
        }

        UNIT_ASSERT_VALUES_EQUAL(storageIssuesCount, storageIssuesNumber);
    }

    Y_UNIT_TEST(OneIssueListing) {
        ListingTest(1, 1);
    }

    Y_UNIT_TEST(Issues100GroupsListing) {
        ListingTest(100, 1);
    }

    Y_UNIT_TEST(Issues100VCardListing) {
        ListingTest(1, 100);
    }

    Y_UNIT_TEST(Issues100Groups100VCardListing) {
        ListingTest(5, 5);
    }

    Y_UNIT_TEST(Issues100GroupsMerging) {
        ListingTest(100, 1, true);
    }

    Y_UNIT_TEST(Issues100VCardMerging) {
        ListingTest(1, 100, true);
    }

    Y_UNIT_TEST(Issues100Groups100VCardMerging) {
        ListingTest(100, 100, true);
    }

    Y_UNIT_TEST(YellowGroupIssueWhenPartialGroupStatus) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{NKikimrBlobStorage::ERROR});
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::YELLOW, 1);
    }

    Y_UNIT_TEST(BlueGroupIssueWhenPartialGroupStatusAndReplicationDisks) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{NKikimrBlobStorage::REPLICATING});
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::BLUE, 1);
    }

    Y_UNIT_TEST(OrangeGroupIssueWhenDegradedGroupStatus) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::DEGRADED, TVDisks{2, NKikimrBlobStorage::ERROR});
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 1);
    }

    Y_UNIT_TEST(RedGroupIssueWhenDisintegratedGroupStatus) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::DISINTEGRATED, TVDisks{3, NKikimrBlobStorage::ERROR});
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 1);
    }

    Y_UNIT_TEST(StaticGroupIssue) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{NKikimrBlobStorage::ERROR}, /*forStatic*/ true);
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::YELLOW, 1, "static");
    }

    Y_UNIT_TEST(GreenStatusWhenCreatingGroup) {
        std::optional<NKikimrBlobStorage::EVDiskStatus> emptyStatus;
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{8, emptyStatus});
        Cerr << result.ShortDebugString() << Endl;
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);
    }

    Y_UNIT_TEST(GreenStatusWhenInitPending) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{8, NKikimrBlobStorage::INIT_PENDING});
        Cerr << result.ShortDebugString() << Endl;
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);
    }

    Y_UNIT_TEST(IgnoreOtherGenerations) {
        TVDisks vdisks;
        vdisks.emplace_back(NKikimrBlobStorage::ERROR, DEFAULT_GROUP_GENERATION - 1);
        vdisks.emplace_back(NKikimrBlobStorage::READY, DEFAULT_GROUP_GENERATION);
        vdisks.emplace_back(NKikimrBlobStorage::ERROR, DEFAULT_GROUP_GENERATION + 1);
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, vdisks);
        Cerr << result.ShortDebugString() << Endl;
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);
    }

    Y_UNIT_TEST(OnlyDiskIssueOnSpaceIssues) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{3, NKikimrBlobStorage::READY}, false, 0.9);
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::YELLOW, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 0);
        CheckHcResultHasIssuesWithStatus(result, "PDISK", Ydb::Monitoring::StatusFlag::YELLOW, 3, "");
    }

    Y_UNIT_TEST(OnlyDiskIssueOnInitialPDisks) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{3, {NKikimrBlobStorage::READY, NKikimrBlobStorage::TPDiskState::DeviceIoError}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::YELLOW, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 0);
        CheckHcResultHasIssuesWithStatus(result, "PDISK", Ydb::Monitoring::StatusFlag::RED, 3, "");
    }

    Y_UNIT_TEST(OnlyDiskIssueOnFaultyPDisks) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::PARTIAL, TVDisks{3, {NKikimrBlobStorage::READY, NKikimrBlobStorage::FAULTY}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::YELLOW, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 0);
        CheckHcResultHasIssuesWithStatus(result, "PDISK", Ydb::Monitoring::StatusFlag::RED, 3, "");
    }

    Y_UNIT_TEST(BridgeGroupNoIssues) {
        auto result = RequestHcWithBridgeVdisks({2, TVDisks{8, NKikimrBlobStorage::READY}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::YELLOW, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 0);
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 0);
    }

    Y_UNIT_TEST(BridgeGroupDegradedInBothPiles) {
        auto result = RequestHcWithBridgeVdisks({2, TVDisks{2, NKikimrBlobStorage::ERROR}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 1);
    }

    Y_UNIT_TEST(BridgeGroupDegradedInOnePile) {
        auto result = RequestHcWithBridgeVdisks({TVDisks{2, NKikimrBlobStorage::ERROR}, TVDisks{NKikimrBlobStorage::REPLICATING}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 1);
    }

    Y_UNIT_TEST(BridgeGroupDeadInOnePile) {
        auto result = RequestHcWithBridgeVdisks({TVDisks{3, NKikimrBlobStorage::ERROR}, TVDisks{8, NKikimrBlobStorage::READY}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::ORANGE, 1);
    }

    Y_UNIT_TEST(BridgeGroupDeadInBothPiles) {
        auto result = RequestHcWithBridgeVdisks({2, TVDisks{3, NKikimrBlobStorage::ERROR}});
        Cerr << result.ShortDebugString() << Endl;
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 1);
    }

    /* HC currently infers group status on its own, so it's never unknown
    Y_UNIT_TEST(RedGroupIssueWhenUnknownGroupStatus) {
        auto result = RequestHcWithVdisks(NKikimrBlobStorage::TGroupStatus::UNKNOWN, {});
        CheckHcResultHasIssuesWithStatus(result, "STORAGE_GROUP", Ydb::Monitoring::StatusFlag::RED, 1);
    }
    */

    Y_UNIT_TEST(StorageLimit95) {
        StorageTest(95, 100, 1, Ydb::Monitoring::StatusFlag::RED);
    }

    Y_UNIT_TEST(StorageLimit87) {
        StorageTest(87, 100, 1, Ydb::Monitoring::StatusFlag::ORANGE);
    }

    Y_UNIT_TEST(StorageLimit80) {
        StorageTest(80, 100, 1, Ydb::Monitoring::StatusFlag::YELLOW);
    }

    Y_UNIT_TEST(StorageLimit50) {
        StorageTest(50, 100, 0, Ydb::Monitoring::StatusFlag::GREEN);
    }

    Y_UNIT_TEST(StorageNoQuota) {
        StorageTest(100, 0, 0, Ydb::Monitoring::StatusFlag::GREEN);
    }

    void CheckHcProtobufSizeIssue(Ydb::Monitoring::SelfCheckResult& result, const Ydb::Monitoring::StatusFlag::Status expectingStatus, ui32 total) {
        int issuesCount = 0;
        for (const auto& issue_log : result.Getissue_log()) {
            if (issue_log.type() == "HEALTHCHECK_STATUS" && issue_log.status() == expectingStatus) {
                issuesCount++;
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(issuesCount, total);
        UNIT_ASSERT_LT(result.ByteSizeLong(), 50_MB);
    }

    Y_UNIT_TEST(ProtobufBelowLimitFor10VdisksIssues) {
        auto result = RequestHc(1, 100);
        CheckHcProtobufSizeIssue(result, Ydb::Monitoring::StatusFlag::YELLOW, 0);
        CheckHcProtobufSizeIssue(result, Ydb::Monitoring::StatusFlag::ORANGE, 0);
        CheckHcProtobufSizeIssue(result, Ydb::Monitoring::StatusFlag::RED, 0);
    }

    Y_UNIT_TEST(ProtobufUnderLimitFor70LargeVdisksIssues) {
        auto result = RequestHc(1, 70, false, true);
        CheckHcProtobufSizeIssue(result, Ydb::Monitoring::StatusFlag::RED, 1);
    }

    Y_UNIT_TEST(ProtobufUnderLimitFor100LargeVdisksIssues) {
        auto result = RequestHc(1, 100, false, true);
        CheckHcProtobufSizeIssue(result, Ydb::Monitoring::StatusFlag::RED, 1);
    }

    void ClearLoadAverage(TEvWhiteboard::TEvSystemStateResponse::TPtr* ev) {
        auto *systemStateInfo = (*ev)->Get()->Record.MutableSystemStateInfo();
        for (NKikimrWhiteboard::TSystemStateInfo &state : *systemStateInfo) {
            auto &loadAverage = *state.MutableLoadAverage();
            for (int i = 0; i < loadAverage.size(); ++i) {
                loadAverage[i] = 0;
            }
        }
    }

    void ChangeNavigateKeyResultServerless(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr* ev,
        NKikimrSubDomains::EServerlessComputeResourcesMode serverlessComputeResourcesMode,
        TTestActorRuntime& runtime)
    {
        TSchemeCacheNavigate::TEntry& entry((*ev)->Get()->Request->ResultSet.front());
        TString path = CanonizePath(entry.Path);
        if (path == "/Root/serverless" || entry.TableId.PathId == SERVERLESS_DOMAIN_KEY) {
            entry.Status = TSchemeCacheNavigate::EStatus::Ok;
            entry.Kind = TSchemeCacheNavigate::EKind::KindExtSubdomain;
            entry.Path = {"Root", "serverless"};
            entry.DomainInfo = MakeIntrusive<TDomainInfo>(SERVERLESS_DOMAIN_KEY, SHARED_DOMAIN_KEY);
            entry.DomainInfo->ServerlessComputeResourcesMode = serverlessComputeResourcesMode;
        } else if (path == "/Root/shared" || entry.TableId.PathId == SHARED_DOMAIN_KEY) {
            entry.Status = TSchemeCacheNavigate::EStatus::Ok;
            entry.Kind = TSchemeCacheNavigate::EKind::KindExtSubdomain;
            entry.Path = {"Root", "shared"};
            entry.DomainInfo = MakeIntrusive<TDomainInfo>(SHARED_DOMAIN_KEY, SHARED_DOMAIN_KEY);
            auto domains = runtime.GetAppData().DomainsInfo;
            ui64 hiveId = domains->GetHive();
            entry.DomainInfo->Params.SetHive(hiveId);
        }
    }

    void ChangeDescribeSchemeResultServerless(NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr* ev) {
        auto record = (*ev)->Get()->MutableRecord();
        if (record->path() == "/Root/serverless" || record->path() == "/Root/shared") {
            record->set_status(NKikimrScheme::StatusSuccess);
            auto pool = record->mutable_pathdescription()->mutable_domaindescription()->add_storagepools();
            pool->set_name(STORAGE_POOL_NAME);
            pool->set_kind("kind");
        }
    }

    void ChangeGetTenantStatusResponse(NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr* ev, const TString &path) {
        auto *operation = (*ev)->Get()->Record.MutableResponse()->mutable_operation();
        Ydb::Cms::GetDatabaseStatusResult getTenantStatusResult;
        getTenantStatusResult.set_path(path);
        operation->mutable_result()->PackFrom(getTenantStatusResult);
        operation->set_status(Ydb::StatusIds::SUCCESS);
    }

    void AddPathsToListTenantsResponse(NConsole::TEvConsole::TEvListTenantsResponse::TPtr* ev, const TVector<TString> &paths) {
        Ydb::Cms::ListDatabasesResult listTenantsResult;
        (*ev)->Get()->Record.GetResponse().operation().result().UnpackTo(&listTenantsResult);
        for (const auto &path : paths) {
            listTenantsResult.Addpaths(path);
        }
        (*ev)->Get()->Record.MutableResponse()->mutable_operation()->mutable_result()->PackFrom(listTenantsResult);
    }

    void ChangeResponseHiveNodeStats(TEvHive::TEvResponseHiveNodeStats::TPtr* ev, ui32 sharedDynNodeId = 0,
        ui32 exclusiveDynNodeId = 0)
    {
        auto &record = (*ev)->Get()->Record;
        if (sharedDynNodeId) {
            auto *sharedNodeStats = record.MutableNodeStats()->Add();
            sharedNodeStats->SetNodeId(sharedDynNodeId);
            sharedNodeStats->MutableNodeDomain()->SetSchemeShard(SHARED_DOMAIN_KEY.OwnerId);
            sharedNodeStats->MutableNodeDomain()->SetPathId(SHARED_DOMAIN_KEY.LocalPathId);
        }

        if (exclusiveDynNodeId) {
            auto *exclusiveNodeStats = record.MutableNodeStats()->Add();
            exclusiveNodeStats->SetNodeId(exclusiveDynNodeId);
            exclusiveNodeStats->MutableNodeDomain()->SetSchemeShard(SERVERLESS_DOMAIN_KEY.OwnerId);
            exclusiveNodeStats->MutableNodeDomain()->SetPathId(SERVERLESS_DOMAIN_KEY.LocalPathId);
        }
    }

    void AddBadServerlessTablet(TEvHive::TEvResponseHiveInfo::TPtr* ev) {
        auto &record = (*ev)->Get()->Record;
        auto* tablet = record.MutableTablets()->Add();
        tablet->SetTabletID(1);
        tablet->MutableObjectDomain()->SetSchemeShard(SERVERLESS_DOMAIN_KEY.OwnerId);
        tablet->MutableObjectDomain()->SetPathId(SERVERLESS_DOMAIN_KEY.LocalPathId);
        tablet->SetRestartsPerPeriod(500);
    }

    Y_UNIT_TEST(SpecificServerless) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 sharedDynNodeId = runtime.GetNodeId(1);

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeShared, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, sharedDynNodeId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root/serverless";
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Ctest << result.ShortDebugString();
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);

        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
        const auto &database_status = result.database_status(0);
        UNIT_ASSERT_VALUES_EQUAL(database_status.name(), "/Root/serverless");
        UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(sharedDynNodeId));

        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
    }

    Y_UNIT_TEST(SpecificServerlessWithExclusiveNodes) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 sharedDynNodeId = runtime.GetNodeId(1);
        ui32 exclusiveDynNodeId = runtime.GetNodeId(2);

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeExclusive, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, sharedDynNodeId, exclusiveDynNodeId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root/serverless";
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString();
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);

        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
        const auto &database_status = result.database_status(0);
        UNIT_ASSERT_VALUES_EQUAL(database_status.name(), "/Root/serverless");
        UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(exclusiveDynNodeId));

        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
    }

    Y_UNIT_TEST(IgnoreServerlessWhenNotSpecific) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 sharedDynNodeId = runtime.GetNodeId(1);

        bool firstConsoleResponse = true;
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                 case NConsole::TEvConsole::EvListTenantsResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvListTenantsResponse::TPtr*>(&ev);
                    AddPathsToListTenantsResponse(x, { "/Root/serverless", "/Root/shared" });
                    break;
                }
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    if (!firstConsoleResponse) {
                        ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    } else {
                        firstConsoleResponse = false;
                        ChangeGetTenantStatusResponse(x, "/Root/shared");
                    }
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeShared, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, sharedDynNodeId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Ctest << result.ShortDebugString();
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);

        bool databaseFoundInResult = false;
        for (const auto &database_status : result.database_status()) {
            if (database_status.name() == "/Root/serverless") {
                databaseFoundInResult = true;
            }
        }
        UNIT_ASSERT(!databaseFoundInResult);
    }

    Y_UNIT_TEST(ServerlessBadTablets) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 sharedDynNodeId = runtime.GetNodeId(1);

        bool firstConsoleResponse = true;
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                 case NConsole::TEvConsole::EvListTenantsResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvListTenantsResponse::TPtr*>(&ev);
                    AddPathsToListTenantsResponse(x, { "/Root/serverless", "/Root/shared" });
                    break;
                }
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    if (!firstConsoleResponse) {
                        ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    } else {
                        firstConsoleResponse = false;
                        ChangeGetTenantStatusResponse(x, "/Root/shared");
                    }
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeShared, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, sharedDynNodeId);
                    break;
                }
                case TEvHive::EvResponseHiveInfo: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveInfo::TPtr*>(&ev);
                    AddBadServerlessTablet(x);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Ctest << result.ShortDebugString();
        UNIT_ASSERT(HasTabletIssue(result));
    }

    Y_UNIT_TEST(DontIgnoreServerlessWithExclusiveNodesWhenNotSpecific) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 sharedDynNodeId = runtime.GetNodeId(1);
        ui32 exclusiveDynNodeId = runtime.GetNodeId(2);

        bool firstConsoleResponse = true;
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvListTenantsResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvListTenantsResponse::TPtr*>(&ev);
                    AddPathsToListTenantsResponse(x, { "/Root/serverless", "/Root/shared" });
                    break;
                }
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    if (firstConsoleResponse) {
                        firstConsoleResponse = false;
                        ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    } else {
                        ChangeGetTenantStatusResponse(x, "/Root/shared");
                    }
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeExclusive, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, sharedDynNodeId, exclusiveDynNodeId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString();
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);

        bool databaseFoundInResult = false;
        for (const auto &database_status : result.database_status()) {
            if (database_status.name() == "/Root/serverless") {
                databaseFoundInResult = true;
                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(exclusiveDynNodeId));

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
            }
        }
        UNIT_ASSERT(databaseFoundInResult);
    }

    Y_UNIT_TEST(ServerlessWhenTroublesWithSharedNodes) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeShared, runtime);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root/serverless";
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString();
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::EMERGENCY);

        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
        const auto &database_status = result.database_status(0);
        UNIT_ASSERT_VALUES_EQUAL(database_status.name(), "/Root/serverless");
        UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::RED);

        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::RED);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 0);

        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
    }

    Y_UNIT_TEST(ServerlessWithExclusiveNodesWhenTroublesWithSharedNodes) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 staticNode = runtime.GetNodeId(0);
        ui32 exclusiveDynNodeId = runtime.GetNodeId(1);

        bool firstConsoleResponse = true;
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvListTenantsResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvListTenantsResponse::TPtr*>(&ev);
                    AddPathsToListTenantsResponse(x, { "/Root/serverless", "/Root/shared" });
                    break;
                }
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    if (firstConsoleResponse) {
                        firstConsoleResponse = false;
                        ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    } else {
                        ChangeGetTenantStatusResponse(x, "/Root/shared");
                    }
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeExclusive, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, 0, exclusiveDynNodeId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::EMERGENCY);

        bool serverlessDatabaseFoundInResult = false;
        bool sharedDatabaseFoundInResult = false;
        bool rootDatabaseFoundInResult = false;

        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 3);
        for (const auto &database_status : result.database_status()) {
            if (database_status.name() == "/Root/serverless") {
                serverlessDatabaseFoundInResult = true;

                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(exclusiveDynNodeId));

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
            } else if (database_status.name() == "/Root/shared") {
                sharedDatabaseFoundInResult = true;

                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::RED);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::RED);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 0);

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
            } else if (database_status.name() == "/Root") {
                rootDatabaseFoundInResult = true;

                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(staticNode));

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), "static");
            }
        }
        UNIT_ASSERT(serverlessDatabaseFoundInResult);
        UNIT_ASSERT(sharedDatabaseFoundInResult);
        UNIT_ASSERT(rootDatabaseFoundInResult);
    }

    Y_UNIT_TEST(SharedWhenTroublesWithExclusiveNodes) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 staticNode = runtime.GetNodeId(0);
        ui32 sharedDynNodeId = runtime.GetNodeId(1);

        bool firstConsoleResponse = true;
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvListTenantsResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvListTenantsResponse::TPtr*>(&ev);
                    AddPathsToListTenantsResponse(x, { "/Root/serverless", "/Root/shared" });
                    break;
                }
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    if (firstConsoleResponse) {
                        firstConsoleResponse = false;
                        ChangeGetTenantStatusResponse(x, "/Root/serverless");
                    } else {
                        ChangeGetTenantStatusResponse(x, "/Root/shared");
                    }
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    ChangeNavigateKeyResultServerless(x, NKikimrSubDomains::EServerlessComputeResourcesModeExclusive, runtime);
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    ChangeResponseHiveNodeStats(x, sharedDynNodeId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    ChangeDescribeSchemeResultServerless(x);
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    SetPDisksStateNormalToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString();
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::EMERGENCY);

        bool serverlessDatabaseFoundInResult = false;
        bool sharedDatabaseFoundInResult = false;
        bool rootDatabaseFoundInResult = false;

        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 3);
        for (const auto &database_status : result.database_status()) {
            if (database_status.name() == "/Root/serverless") {
                serverlessDatabaseFoundInResult = true;

                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::RED);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::RED);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 0);

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
            } else if (database_status.name() == "/Root/shared") {
                sharedDatabaseFoundInResult = true;

                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(sharedDynNodeId));

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), STORAGE_POOL_NAME);
            } else if (database_status.name() == "/Root") {
                rootDatabaseFoundInResult = true;

                UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::GREEN);

                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(staticNode));

                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), "static");
            }
        }
        UNIT_ASSERT(serverlessDatabaseFoundInResult);
        UNIT_ASSERT(sharedDatabaseFoundInResult);
        UNIT_ASSERT(rootDatabaseFoundInResult);
    }

    Y_UNIT_TEST(NoStoragePools) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        ui32 dynNodeId = runtime.GetNodeId(1);

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    ChangeGetTenantStatusResponse(x, "/Root/database");
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    TSchemeCacheNavigate::TEntry& entry((*x)->Get()->Request->ResultSet.front());
                    TString path = CanonizePath(entry.Path);
                    if (path == "/Root/database" || entry.TableId.PathId == SUBDOMAIN_KEY) {
                        entry.Status = TSchemeCacheNavigate::EStatus::Ok;
                        entry.Kind = TSchemeCacheNavigate::EKind::KindExtSubdomain;
                        entry.Path = {"Root", "database"};
                        entry.DomainInfo = MakeIntrusive<TDomainInfo>(SUBDOMAIN_KEY, SUBDOMAIN_KEY);
                        auto domains = runtime.GetAppData().DomainsInfo;
                        ui64 hiveId = domains->GetHive();
                        entry.DomainInfo->Params.SetHive(hiveId);
                    }
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    auto &record = (*x)->Get()->Record;
                    auto *nodeStats = record.MutableNodeStats()->Add();
                    nodeStats->SetNodeId(dynNodeId);
                    nodeStats->MutableNodeDomain()->SetSchemeShard(SUBDOMAIN_KEY.OwnerId);
                    nodeStats->MutableNodeDomain()->SetPathId(SUBDOMAIN_KEY.LocalPathId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    auto record = (*x)->Get()->MutableRecord();
                    if (record->path() == "/Root/database") {
                        record->set_status(NKikimrScheme::StatusSuccess);
                        // no pools
                    }
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root/database";
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString();

        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::EMERGENCY);
        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
        const auto &database_status = result.database_status(0);
        UNIT_ASSERT_VALUES_EQUAL(database_status.name(),  "/Root/database");
        UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::RED);

        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::GREEN);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(dynNodeId));

        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::RED);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 0);
    }

    Y_UNIT_TEST(NoBscResponse) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        auto &dynamicNameserviceConfig = runtime.GetAppData().DynamicNameserviceConfig;
        dynamicNameserviceConfig->MaxStaticNodeId = runtime.GetNodeId(server.StaticNodes() - 1);
        dynamicNameserviceConfig->MinDynamicNodeId = runtime.GetNodeId(server.StaticNodes());
        dynamicNameserviceConfig->MaxDynamicNodeId = runtime.GetNodeId(server.StaticNodes() + server.DynamicNodes() - 1);

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            auto type = ev->GetTypeRewrite();
            if (EventSpaceBegin(TKikimrEvents::ES_SYSTEM_VIEW) <= type && type <= EventSpaceEnd(TKikimrEvents::ES_SYSTEM_VIEW)) {
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root";
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString() << Endl;

        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::EMERGENCY);

        bool bscTabletIssueFoundInResult = false;
        for (const auto &issue_log : result.issue_log()) {
            if (issue_log.level() == 3 && issue_log.type() == "SYSTEM_TABLET") {
                UNIT_ASSERT_VALUES_EQUAL(issue_log.location().compute().tablet().id().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(issue_log.location().compute().tablet().id()[0], ToString(MakeBSControllerID()));
                UNIT_ASSERT_VALUES_EQUAL(issue_log.location().compute().tablet().type(), "BSController");
                bscTabletIssueFoundInResult = true;
            }
        }
        UNIT_ASSERT(bscTabletIssueFoundInResult);

        UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
        const auto &database_status = result.database_status(0);

        UNIT_ASSERT_VALUES_EQUAL(database_status.name(), "/Root");
        UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::RED);

        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), Ydb::Monitoring::StatusFlag::RED);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::RED);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].id(), "static");
    }

    Y_UNIT_TEST(ShardsLimit999) {
        ShardsQuotaTest(999, 1000, 1, Ydb::Monitoring::StatusFlag::RED);
    }

    Y_UNIT_TEST(ShardsLimit995) {
        ShardsQuotaTest(995, 1000, 1, Ydb::Monitoring::StatusFlag::ORANGE);
    }

    Y_UNIT_TEST(ShardsLimit905) {
        ShardsQuotaTest(905, 1000, 1, Ydb::Monitoring::StatusFlag::YELLOW);
    }

    Y_UNIT_TEST(ShardsLimit800) {
        ShardsQuotaTest(805, 1000, 0, Ydb::Monitoring::StatusFlag::GREEN);
    }

    Y_UNIT_TEST(ShardsNoLimit) {
        ShardsQuotaTest(105, 0, 0, Ydb::Monitoring::StatusFlag::GREEN);
    }

    Y_UNIT_TEST(TestTabletIsDead) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);

        TClient client(settings);

        TTestActorRuntime* runtime = server.GetRuntime();
        TActorId sender = runtime->AllocateEdgeActor();

        // only have local on dynamic nodes
        runtime->Send(new IEventHandle(MakeLocalID(runtime->GetNodeId(0)), sender, new TEvents::TEvPoisonPill()));
        runtime->Send(new IEventHandle(MakeLocalID(runtime->GetNodeId(1)), sender, new TEvents::TEvPoisonPill()));

        server.SetupDynamicLocalService(2, "Root");
        server.StartPQTablets(1);
        server.DestroyDynamicLocalService(2);
        runtime->AdvanceCurrentTime(TDuration::Minutes(5));

        TAutoPtr<IEventHandle> handle;
        runtime->Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, new NHealthCheck::TEvSelfCheckRequest(), 0));
        auto result = runtime->GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Cerr << result.ShortDebugString();

        UNIT_ASSERT(HasTabletIssue(result));
    }

    Y_UNIT_TEST(TestBootingTabletIsNotDead) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);

        TClient client(settings);

        TTestActorRuntime* runtime = server.GetRuntime();
        TActorId sender = runtime->AllocateEdgeActor();

        auto blockBoot = runtime->AddObserver<NHive::TEvPrivate::TEvProcessBootQueue>([](auto&& ev) { ev.Reset(); });

        server.SetupDynamicLocalService(2, "Root");
        server.StartPQTablets(1, false);
        runtime->AdvanceCurrentTime(TDuration::Minutes(5));

        TAutoPtr<IEventHandle> handle;
        runtime->Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, new NHealthCheck::TEvSelfCheckRequest(), 0));
        auto result = runtime->GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Cerr << result.ShortDebugString();

        UNIT_ASSERT(!HasTabletIssue(result));
    }

    Y_UNIT_TEST(TestReBootingTabletIsDead) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(2)
                .SetDynamicNodeCount(2)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);

        TClient client(settings);

        TTestActorRuntime* runtime = server.GetRuntime();
        runtime->SetLogPriority(NKikimrServices::HIVE, NActors::NLog::PRI_TRACE);
        TActorId sender = runtime->AllocateEdgeActor();

        // only have local on dynamic nodes
        runtime->Send(new IEventHandle(MakeLocalID(runtime->GetNodeId(0)), sender, new TEvents::TEvPoisonPill()));
        runtime->Send(new IEventHandle(MakeLocalID(runtime->GetNodeId(1)), sender, new TEvents::TEvPoisonPill()));


        server.SetupDynamicLocalService(2, "Root");
        server.StartPQTablets(1, true);
        server.SetupDynamicLocalService(3, "Root");
        auto blockBoot = runtime->AddObserver<NHive::TEvPrivate::TEvProcessBootQueue>([](auto&& ev) { ev.Reset(); });
        server.DestroyDynamicLocalService(2);
        runtime->AdvanceCurrentTime(TDuration::Minutes(5));

        TAutoPtr<IEventHandle> handle;
        runtime->Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, new NHealthCheck::TEvSelfCheckRequest(), 0));
        auto result = runtime->GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Cerr << result.ShortDebugString();

        UNIT_ASSERT(HasTabletIssue(result));
    }

    Y_UNIT_TEST(TestStoppedTabletIsNotDead) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);

        TClient client(settings);

        TTestActorRuntime* runtime = server.GetRuntime();
        TActorId sender = runtime->AllocateEdgeActor();
        runtime->SetLogPriority(NKikimrServices::HIVE, NActors::NLog::PRI_TRACE);

        // kill local so that tablet cannot start
        runtime->Send(new IEventHandle(MakeLocalID(runtime->GetNodeId(0)), sender, new TEvents::TEvPoisonPill()));

        ui64 tabletId = server.StartPQTablets(1, false).front();
        runtime->DispatchEvents({}, TDuration::MilliSeconds(50));
        runtime->AdvanceCurrentTime(TDuration::Minutes(5));

        {
            TAutoPtr<IEventHandle> handle;
            runtime->Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, new NHealthCheck::TEvSelfCheckRequest(), 0));
            auto result = runtime->GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
            Cerr << result.ShortDebugString() << Endl;

            UNIT_ASSERT(HasTabletIssue(result));
        }

        runtime->SendToPipe(72057594037968897, sender, new TEvHive::TEvStopTablet(tabletId, sender), 0, GetPipeConfigWithRetries());
        TAutoPtr<IEventHandle> handle;
        runtime->GrabEdgeEvent<TEvHive::TEvStopTabletResult>(handle);

        {
            TAutoPtr<IEventHandle> handle;
            runtime->Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, new NHealthCheck::TEvSelfCheckRequest(), 0));
            auto result = runtime->GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
            Cerr << result.ShortDebugString() << Endl;

            UNIT_ASSERT(!HasTabletIssue(result));
        }
    }

    void SendHealthCheckConfigUpdate(TTestActorRuntime &runtime, const TActorId& sender, const NKikimrConfig::THealthCheckConfig &cfg) {
        auto *event = new NConsole::TEvConsole::TEvConfigureRequest;

        event->Record.AddActions()->MutableRemoveConfigItems()->MutableCookieFilter()->AddCookies("cookie");

        auto &item = *event->Record.AddActions()->MutableAddConfigItem()->MutableConfigItem();
        item.MutableConfig()->MutableHealthCheckConfig()->CopyFrom(cfg);
        item.SetCookie("cookie");

        runtime.SendToPipe(MakeConsoleID(), sender, event, 0, GetPipeConfigWithRetries());

        TAutoPtr<IEventHandle> handle;
        auto record = runtime.GrabEdgeEvent<NConsole::TEvConsole::TEvConfigureResponse>(handle)->Record;
        UNIT_ASSERT_VALUES_EQUAL(record.MutableStatus()->GetCode(), Ydb::StatusIds::SUCCESS);
    }

    void ChangeNodeRestartsPerPeriod(TTestActorRuntime &runtime, const TActorId& sender, const ui32 restartsYellow, const ui32 restartsOrange) {
        NKikimrConfig::TAppConfig ext;
        auto &cfg = *ext.MutableHealthCheckConfig();
        cfg.MutableThresholds()->SetNodeRestartsYellow(restartsYellow);
        cfg.MutableThresholds()->SetNodeRestartsOrange(restartsOrange);
        SendHealthCheckConfigUpdate(runtime, sender, cfg);
    }

    void TestConfigUpdateNodeRestartsPerPeriod(TTestActorRuntime &runtime, const TActorId& sender, const ui32 restartsYellow, const ui32 restartsOrange, const ui32 nodeId, Ydb::Monitoring::StatusFlag::Status expectedStatus) {
        ChangeNodeRestartsPerPeriod(runtime, sender, restartsYellow, restartsOrange);

        TAutoPtr<IEventHandle> handle;
        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root/database";

        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        Ctest << result.ShortDebugString() << Endl;

        const auto &database_status = result.database_status(0);
        UNIT_ASSERT_VALUES_EQUAL(database_status.name(), "/Root/database");
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().overall(), expectedStatus);
        UNIT_ASSERT_VALUES_EQUAL(database_status.compute().nodes()[0].id(), ToString(nodeId));
    }

    Y_UNIT_TEST(HealthCheckConfigUpdate) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");

        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();
        TActorId sender = runtime.AllocateEdgeActor();

        const ui32 nodeRestarts = 10;
        const ui32 nodeId = runtime.GetNodeId(1);
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NConsole::TEvConsole::EvGetTenantStatusResponse: {
                    auto *x = reinterpret_cast<NConsole::TEvConsole::TEvGetTenantStatusResponse::TPtr*>(&ev);
                    ChangeGetTenantStatusResponse(x, "/Root/database");
                    break;
                }
                case TEvTxProxySchemeCache::EvNavigateKeySetResult: {
                    auto *x = reinterpret_cast<TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr*>(&ev);
                    TSchemeCacheNavigate::TEntry& entry((*x)->Get()->Request->ResultSet.front());
                    const TString path = CanonizePath(entry.Path);
                    if (path == "/Root/database" || entry.TableId.PathId == SUBDOMAIN_KEY) {
                        entry.Status = TSchemeCacheNavigate::EStatus::Ok;
                        entry.Kind = TSchemeCacheNavigate::EKind::KindExtSubdomain;
                        entry.Path = {"Root", "database"};
                        entry.DomainInfo = MakeIntrusive<TDomainInfo>(SUBDOMAIN_KEY, SUBDOMAIN_KEY);
                        auto domains = runtime.GetAppData().DomainsInfo;
                        ui64 hiveId = domains->GetHive();
                        entry.DomainInfo->Params.SetHive(hiveId);
                    }
                    break;
                }
                case TEvHive::EvResponseHiveNodeStats: {
                    auto *x = reinterpret_cast<TEvHive::TEvResponseHiveNodeStats::TPtr*>(&ev);
                    auto &record = (*x)->Get()->Record;
                    record.ClearNodeStats();
                    auto *nodeStats = record.MutableNodeStats()->Add();
                    nodeStats->SetNodeId(nodeId);
                    nodeStats->SetRestartsPerPeriod(nodeRestarts);
                    nodeStats->MutableNodeDomain()->SetSchemeShard(SUBDOMAIN_KEY.OwnerId);
                    nodeStats->MutableNodeDomain()->SetPathId(SUBDOMAIN_KEY.LocalPathId);
                    break;
                }
                case TEvSchemeShard::EvDescribeSchemeResult: {
                    auto *x = reinterpret_cast<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult::TPtr*>(&ev);
                    auto record = (*x)->Get()->MutableRecord();
                    if (record->path() == "/Root/database") {
                        record->set_status(NKikimrScheme::StatusSuccess);
                        // no pools
                    }
                    break;
                }
                case TEvBlobStorage::EvControllerConfigResponse: {
                    auto *x = reinterpret_cast<TEvBlobStorage::TEvControllerConfigResponse::TPtr*>(&ev);
                    AddGroupVSlotInControllerConfigResponseWithStaticGroup(x, NKikimrBlobStorage::TGroupStatus::FULL, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    AddVSlotsToSysViewResponse(x, 1, TVDisks(1));
                    break;
                }
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    AddGroupsToSysViewResponse(x);
                    break;
                }
                case NSysView::TEvSysView::EvGetStoragePoolsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetStoragePoolsResponse::TPtr*>(&ev);
                    AddStoragePoolsToSysViewResponse(x);
                    break;
                }
                case TEvWhiteboard::EvSystemStateResponse: {
                    auto *x = reinterpret_cast<TEvWhiteboard::TEvSystemStateResponse::TPtr*>(&ev);
                    ClearLoadAverage(x);
                    break;
                }
                case TEvInterconnect::EvNodesInfo: {
                    auto *x = reinterpret_cast<TEvInterconnect::TEvNodesInfo::TPtr*>(&ev);
                    auto nodes = MakeIntrusive<TIntrusiveVector<TEvInterconnect::TNodeInfo>>((*x)->Get()->Nodes);
                    if (!nodes->empty()) {
                        nodes->erase(nodes->begin() + 1, nodes->end());
                        nodes->begin()->NodeId = nodeId;
                    }
                    auto newEv = IEventHandle::Downcast<TEvInterconnect::TEvNodesInfo>(
                        new IEventHandle((*x)->Recipient, (*x)->Sender, new TEvInterconnect::TEvNodesInfo(nodes))
                    );
                    x->Swap(newEv);
                    break;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TestConfigUpdateNodeRestartsPerPeriod(runtime, sender, nodeRestarts + 5, nodeRestarts + 10, nodeId, Ydb::Monitoring::StatusFlag::GREEN);
        TestConfigUpdateNodeRestartsPerPeriod(runtime, sender, nodeRestarts / 2, nodeRestarts + 5, nodeId, Ydb::Monitoring::StatusFlag::YELLOW);
        TestConfigUpdateNodeRestartsPerPeriod(runtime, sender, nodeRestarts / 5, nodeRestarts / 2, nodeId, Ydb::Monitoring::StatusFlag::ORANGE);
    }

    void LayoutCorrectTest(bool layoutCorrect) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");

        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();
        TActorId sender = runtime.AllocateEdgeActor();

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NSysView::TEvSysView::EvGetGroupsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetGroupsResponse::TPtr*>(&ev);
                    auto& record = (*x)->Get()->Record;
                    for (auto& entry : *record.mutable_entries()) {
                        entry.mutable_info()->set_layoutcorrect(layoutCorrect);
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TAutoPtr<IEventHandle> handle;
        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        if (layoutCorrect) {
            UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);
            UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
            const auto &database_status = result.database_status(0);

            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::GREEN);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].overall(), Ydb::Monitoring::StatusFlag::GREEN);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].groups().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].groups()[0].overall(), Ydb::Monitoring::StatusFlag::GREEN);
        } else {
            UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::MAINTENANCE_REQUIRED);
            UNIT_ASSERT_VALUES_EQUAL(result.database_status_size(), 1);
            const auto &database_status = result.database_status(0);

            UNIT_ASSERT_VALUES_EQUAL(database_status.overall(), Ydb::Monitoring::StatusFlag::ORANGE);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().overall(), Ydb::Monitoring::StatusFlag::ORANGE);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].overall(), Ydb::Monitoring::StatusFlag::ORANGE);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].groups().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(database_status.storage().pools()[0].groups()[0].overall(), Ydb::Monitoring::StatusFlag::ORANGE);

            for (const auto &issue_log : result.issue_log()) {
                if (issue_log.level() == 1 && issue_log.type() == "DATABASE") {
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.location().database().name(), "/Root");
                } else if (issue_log.level() == 2 && issue_log.type() == "STORAGE") {
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.location().database().name(), "/Root");
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.message(), "Storage has no redundancy");
                } else if (issue_log.level() == 3 && issue_log.type() == "STORAGE_POOL") {
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.location().storage().pool().name(), "static");
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.message(), "Pool has no redundancy");
                } else if (issue_log.level() == 4 && issue_log.type() == "STORAGE_GROUP") {
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.location().storage().pool().name(), "static");
                    UNIT_ASSERT_VALUES_EQUAL(issue_log.message(), "Group layout is incorrect");
                }
            }
        }
    }

    Y_UNIT_TEST(LayoutIncorrect) {
        LayoutCorrectTest(false);
    }

    Y_UNIT_TEST(LayoutCorrect) {
        LayoutCorrectTest(true);
    }

    Y_UNIT_TEST(UnknowPDiskState) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");

        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();
        TActorId sender = runtime.AllocateEdgeActor();

        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case NSysView::TEvSysView::EvGetVSlotsResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetVSlotsResponse::TPtr*>(&ev);
                    auto& record = (*x)->Get()->Record;
                    for (auto& entry : *record.mutable_entries()) {
                        entry.mutable_info()->set_statusv2("ERROR");
                    }
                    break;
                }
                case NSysView::TEvSysView::EvGetPDisksResponse: {
                    auto* x = reinterpret_cast<NSysView::TEvSysView::TEvGetPDisksResponse::TPtr*>(&ev);
                    auto& record = (*x)->Get()->Record;
                    for (auto& entry : *record.mutable_entries()) {
                        entry.mutable_info()->set_state("UNKNOWN_STATE?");
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);

        TAutoPtr<IEventHandle> handle;
        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        bool pdiskIssueFoundInResult = false;
        for (const auto &issue_log : result.issue_log()) {
            if (issue_log.type() == "PDISK") {
                pdiskIssueFoundInResult = true;
            }
        }
        UNIT_ASSERT(!pdiskIssueFoundInResult);
    }

    Y_UNIT_TEST(TestSystemStateRetriesAfterReceivingResponse) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        std::optional<TActorId> targetActor;
        auto observerFunc = [&](TAutoPtr<IEventHandle>& ev) {
            switch (ev->GetTypeRewrite()) {
                case TEvWhiteboard::EvSystemStateResponse: {
                    if (ev->Cookie == 1) {
                        if (!targetActor) {
                            targetActor = ev->Recipient;
                            runtime.Send(ev.Release());
                            runtime.Send(new IEventHandle(
                                *targetActor,
                                sender,
                                new NHealthCheck::TEvPrivate::TEvRetryNodeWhiteboard(1, TEvWhiteboard::TEvSystemStateRequest::EventType)
                            ));

                        }
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    break;
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        runtime.SetObserverFunc(observerFunc);
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, new NHealthCheck::TEvSelfCheckRequest(), 0));

        auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;
        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::GOOD);
    }

    Y_UNIT_TEST(CLusterNotBootstrapped) {
        TPortManager tp;
        ui16 port = tp.GetPort(2134);
        ui16 grpcPort = tp.GetPort(2135);
        auto settings = TServerSettings(port)
                .SetNodeCount(1)
                .SetDynamicNodeCount(1)
                .SetUseRealThreads(false)
                .SetDomainName("Root");
        TServer server(settings);
        server.EnableGRpc(grpcPort);
        TClient client(settings);
        TTestActorRuntime& runtime = *server.GetRuntime();
        auto config = std::make_shared<NKikimrBlobStorage::TStorageConfig>();
        config->MutableSelfManagementConfig()->SetEnabled(true);

        auto observer = runtime.AddObserver<TEvNodeWardenStorageConfig>([&](TEvNodeWardenStorageConfig::TPtr& ev) {
            ev->Get()->Config = config;
        });

        TActorId sender = runtime.AllocateEdgeActor();
        TAutoPtr<IEventHandle> handle;

        auto *request = new NHealthCheck::TEvSelfCheckRequest;
        request->Request.set_return_verbose_status(true);
        request->Database = "/Root";
        runtime.Send(new IEventHandle(NHealthCheck::MakeHealthCheckID(), sender, request, 0));
        const auto result = runtime.GrabEdgeEvent<NHealthCheck::TEvSelfCheckResult>(handle)->Result;

        Ctest << result.ShortDebugString() << Endl;

        UNIT_ASSERT_VALUES_EQUAL(result.self_check_result(), Ydb::Monitoring::SelfCheck::MAINTENANCE_REQUIRED);
        UNIT_ASSERT(std::ranges::any_of(result.issue_log(), [](auto&& issue) { return issue.message() == "Cluster is not bootstrapped"; }));
    }
}
}
