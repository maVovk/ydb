#include "ut_helpers/ut_backup_restore_common.h"

#include <ydb/public/api/protos/ydb_import.pb.h>

#include <ydb/core/backup/common/checksum.h>
#include <ydb/core/base/localdb.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/metering/metering.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/tablet/resource_broker.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/testlib/audit_helpers/audit_helper.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/schemeshard/schemeshard_billing_helpers.h>
#include <ydb/core/tx/schemeshard/schemeshard_private.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>
#include <ydb/core/util/aws.h>
#include <ydb/core/wrappers/ut_helpers/s3_mock.h>
#include <ydb/core/ydb_convert/table_description.h>

#include <yql/essentials/types/binary_json/write.h>
#include <yql/essentials/types/dynumber/dynumber.h>
#include <yql/essentials/types/uuid/uuid.h>

#include <library/cpp/string_utils/quote/quote.h>
#include <library/cpp/testing/hook/hook.h>

#include <contrib/libs/double-conversion/double-conversion/ieee.h>
#include <contrib/libs/zstd/include/zstd.h>

#include <util/datetime/base.h>
#include <util/generic/size_literals.h>
#include <util/generic/vector.h>
#include <util/string/builder.h>
#include <util/string/join.h>
#include <util/string/printf.h>

#include <regex>

using namespace NKikimr::NSchemeShard;
using namespace NKikimr::NWrappers::NTestHelpers;
using namespace NKikimr;
using namespace NKikimrSchemeOp;
using namespace NSchemeShardUT_Private;

using namespace NKikimr::Tests;

namespace {

    Y_TEST_HOOK_BEFORE_RUN(InitAwsAPI) {
        NKikimr::InitAwsAPI();
    }

    Y_TEST_HOOK_AFTER_RUN(ShutdownAwsAPI) {
        NKikimr::ShutdownAwsAPI();
    }

    const TString EmptyYsonStr = R"([[[[];%false]]])";

    TString GenerateScheme(const TPathDescription& pathDesc) {
        UNIT_ASSERT(pathDesc.HasTable());
        const auto& tableDesc = pathDesc.GetTable();

        Ydb::Table::CreateTableRequest scheme;
        NKikimrMiniKQL::TType mkqlKeyType;

        scheme.mutable_primary_key()->CopyFrom(tableDesc.GetKeyColumnNames());
        FillColumnDescription(scheme, mkqlKeyType, tableDesc);
        FillIndexDescription(scheme, tableDesc);
        FillStorageSettings(scheme, tableDesc);
        FillColumnFamilies(scheme, tableDesc);
        FillAttributes(scheme, pathDesc);
        FillTableBoundary(scheme, tableDesc, mkqlKeyType);
        FillPartitioningSettings(scheme, tableDesc);
        FillKeyBloomFilter(scheme, tableDesc);
        FillReadReplicasSettings(scheme, tableDesc);

        TString result;
        UNIT_ASSERT(google::protobuf::TextFormat::PrintToString(scheme, &result));

        return result;
    }

    TString GenerateScheme(const NKikimrScheme::TEvDescribeSchemeResult& describeResult) {
        UNIT_ASSERT(describeResult.HasPathDescription());
        return GenerateScheme(describeResult.GetPathDescription());
    }

    TString GenerateTableDescription(const NKikimrScheme::TEvDescribeSchemeResult& describeResult) {
        UNIT_ASSERT(describeResult.HasPathDescription());
        UNIT_ASSERT(describeResult.GetPathDescription().HasTable());
        const auto& tableDesc = describeResult.GetPathDescription().GetTable();

        TTableDescription scheme;
        scheme.MutableColumns()->CopyFrom(tableDesc.GetColumns());
        scheme.MutableKeyColumnNames()->CopyFrom(tableDesc.GetKeyColumnNames());

        TString schemeStr;
        UNIT_ASSERT(google::protobuf::TextFormat::PrintToString(scheme, &schemeStr));

        return schemeStr;
    }

    struct TDataWithChecksum {
        TString Data;
        TString Checksum;

        TDataWithChecksum() = default;

        TDataWithChecksum(TString&& data)
            : Data(std::move(data))
            , Checksum(NBackup::ComputeChecksum(Data))
        {}

        TDataWithChecksum(const char* data)
            : TDataWithChecksum(TString(data))
        {}

        TDataWithChecksum& operator=(const TString& data) {
            Data = data;
            Checksum = NBackup::ComputeChecksum(Data);
            return *this;
        }

        operator TString() const {
            return Data;
        }

        operator bool() const {
            return !Data.empty();
        }
    };

    struct TTestData {
        TDataWithChecksum RawData;
        TString Data; // RawData after compression/encryption
        TString YsonStr;
        EDataFormat DataFormat = EDataFormat::Csv;
        ECompressionCodec CompressionCodec;

        TTestData(TString csvData, TString ysonStr, ECompressionCodec codec = ECompressionCodec::None)
            : RawData(std::move(csvData))
            , Data(RawData)
            , YsonStr(std::move(ysonStr))
            , CompressionCodec(codec)
        {
        }

        TString Ext() const {
            TStringBuilder result;

            switch (DataFormat) {
            case EDataFormat::Csv:
                result << ".csv";
                break;
            case EDataFormat::Invalid:
                UNIT_ASSERT_C(false, "Invalid data format");
                break;
            }

            switch (CompressionCodec) {
            case ECompressionCodec::None:
                break;
            case ECompressionCodec::Zstd:
                result << ".zst";
                break;
            case ECompressionCodec::Invalid:
                UNIT_ASSERT_C(false, "Invalid compression codec");
                break;
            }

            return result;
        }
    };

    struct TImportChangefeed {
        TDataWithChecksum Changefeed;
        TDataWithChecksum Topic;
    };

    struct TTestDataWithScheme {
        TDataWithChecksum Metadata = R"({"version": 0})";
        EPathType Type = EPathTypeTable;
        TDataWithChecksum Scheme;
        TDataWithChecksum CreationQuery;
        TDataWithChecksum Permissions;
        TImportChangefeed Changefeed;
        TVector<TTestData> Data;
        TDataWithChecksum Topic;

        TTestDataWithScheme() = default;

        TTestDataWithScheme(TString&& scheme, TVector<TTestData>&& data)
            : Scheme(std::move(scheme))
            , Data(std::move(data))
        {
        }
    };

    TTestData GenerateTestData(const TString& keyPrefix, ui32 count) {
        TStringBuilder csv;
        TStringBuilder yson;

        for (ui32 i = 1; i <= count; ++i) {
            // csv
            if (keyPrefix) {
                csv << "\"" << keyPrefix << i << "\",";
            } else {
                csv << i << ",";
            }

            csv << "\"" << "value" << i << "\"" << Endl;

            // yson
            if (i == 1) {
                yson << "[[[[";
            } else {
                yson << ";";
            }

            yson << "["
                << "[\"" << keyPrefix << i << "\"];"
                << "[\"" << "value" << i << "\"]"
            << "]";

            if (i == count) {
                yson << "];\%false]]]";
            }
        }

        return TTestData(std::move(csv), std::move(yson));
    }

    TString ZstdCompress(const TStringBuf src) {
        TString compressed;
        compressed.resize(ZSTD_compressBound(src.size()));

        const auto res = ZSTD_compress(compressed.Detach(), compressed.size(), src.data(), src.size(), ZSTD_CLEVEL_DEFAULT);
        UNIT_ASSERT_C(!ZSTD_isError(res), "Zstd error: " << ZSTD_getErrorName(res));
        compressed.resize(res);

        return compressed;
    }

    TTestData GenerateZstdTestData(const TString& keyPrefix, ui32 count, ui32 rowsPerFrame = 0) {
        auto data = GenerateTestData(keyPrefix, count);
        if (!rowsPerFrame) {
            rowsPerFrame = count;
        }

        TString compressed;
        ui32 start = 0;
        ui32 rowsInFrame = 0;

        for (ui32 i = 0; i < data.Data.size(); ++i) {
            const auto c = data.Data[i];
            const bool last = i == data.Data.size() - 1;

            if (last) {
                UNIT_ASSERT(c == '\n');
            }

            if (c == '\n') {
                if (++rowsInFrame == rowsPerFrame || last) {
                    compressed.append(ZstdCompress(TStringBuf(&data.Data[start], i + 1 - start)));

                    start = i + 1;
                    rowsInFrame = 0;
                }
            }
        }

        data.Data = std::move(compressed);
        data.CompressionCodec = ECompressionCodec::Zstd;

        return data;
    }

    TTestData GenerateTestData(ECompressionCodec codec, const TString& keyPrefix, ui32 count) {
        switch (codec) {
        case ECompressionCodec::None:
            return GenerateTestData(keyPrefix, count);
        case ECompressionCodec::Zstd:
            return GenerateZstdTestData(keyPrefix, count);
        case ECompressionCodec::Invalid:
            UNIT_ASSERT_C(false, "Invalid compression codec");
            Y_ABORT("unreachable");
        }
    }

    TTestDataWithScheme GenerateTestData(
        const TTypedScheme& typedScheme,
        const TVector<std::pair<TString, ui64>>& shardsConfig = {{"a", 1}},
        const TString& permissions = "",
        const TString& metadata = R"({"version": 0})"
    ) {
        TTestDataWithScheme result;
        result.Type = typedScheme.Type;
        result.Permissions = permissions;
        result.Metadata = metadata;

        switch (typedScheme.Type) {
        case EPathTypeTable:
            result.Scheme = typedScheme.Scheme;
            for (const auto& [keyPrefix, count] : shardsConfig) {
                result.Data.emplace_back(GenerateTestData(keyPrefix, count));
            }
            break;
        case EPathTypeView:
            result.CreationQuery = typedScheme.Scheme;
            break;
        case EPathTypeCdcStream:
            result.Changefeed.Changefeed = typedScheme.Scheme;
            result.Changefeed.Topic = typedScheme.Attributes.GetTopicDescription();
            break;
        case EPathTypePersQueueGroup:
            result.Topic = typedScheme.Scheme;
            break;
        default:
            UNIT_FAIL("cannot create sample test data for the scheme object type: " << typedScheme.Type);
            return {};
        }

        return result;
    }

    THashMap<TString, TString> ConvertTestData(const THashMap<TString, TTestDataWithScheme>& data) {
        THashMap<TString, TString> result;

        for (const auto& [prefix, item] : data) {
            bool withChecksum = item.Metadata.Data != R"({"version": 0})";

            auto metadataKey = prefix + "/metadata.json";
            result.emplace(metadataKey, item.Metadata);
            if (withChecksum) {
                result.emplace(NBackup::ChecksumKey(metadataKey), item.Metadata.Checksum);
            }

            switch (item.Type) {
            case EPathTypeTable: {
                auto schemeKey = prefix + "/scheme.pb";
                result.emplace(schemeKey, item.Scheme);
                if (withChecksum) {
                    result.emplace(NBackup::ChecksumKey(schemeKey), item.Scheme.Checksum);
                }
                break;
            }
            case EPathTypeView: {
                auto viewKey = prefix + "/create_view.sql";
                result.emplace(viewKey, item.CreationQuery);
                if (withChecksum) {
                    result.emplace(NBackup::ChecksumKey(viewKey), item.CreationQuery.Checksum);
                }
                break;
            }
            case EPathTypeCdcStream: {
                auto changefeedKey = prefix +  "/changefeed_description.pb";
                auto topicKey = prefix +  "/topic_description.pb";
                result.emplace(changefeedKey, item.Changefeed.Changefeed);
                result.emplace(topicKey, item.Changefeed.Topic);
                if (withChecksum) {
                    result.emplace(NBackup::ChecksumKey(changefeedKey), item.Changefeed.Changefeed.Checksum);
                    result.emplace(NBackup::ChecksumKey(topicKey), item.Changefeed.Topic.Checksum);
                }
                break;
            }
            case EPathTypePersQueueGroup: {
                auto topicKey = prefix + "/create_topic.pb";
                result.emplace(topicKey, item.Topic);
                if (withChecksum) {
                    result.emplace(NBackup::ChecksumKey(topicKey), item.Topic.Checksum);
                }
                break;
            }
            default:
                UNIT_FAIL("cannot determine key for the scheme object type: " << item.Type);
                return {};
            }

            if (item.Permissions) {
                auto permissionsKey = prefix + "/permissions.pb";
                result.emplace(permissionsKey, item.Permissions);
                if (withChecksum) {
                    result.emplace(NBackup::ChecksumKey(permissionsKey), item.Permissions.Checksum);
                }
            }

            for (ui32 i = 0; i < item.Data.size(); ++i) {
                const auto& data = item.Data.at(i);
                result.emplace(Sprintf("%s/data_%02d%s", prefix.data(), i, data.Ext().c_str()), data.Data);
                if (withChecksum) {
                    auto rawDataKey = Sprintf("%s/data_%02d.csv", prefix.data(), i);
                    result.emplace(NBackup::ChecksumKey(rawDataKey), data.RawData.Checksum);
                }
            }
        }

        return result;
    }

    THashMap<TString, TString> ConvertTestData(const TTestDataWithScheme& data) {
        return ConvertTestData({{"", data}});
    }

    struct TReadKeyDesc {
        TString Name;
        TString Type;
        TString Atom;
    };

    using TDelayFunc = std::function<bool(TAutoPtr<IEventHandle>&)>;

    auto SetDelayObserver(TTestActorRuntime& runtime, THolder<IEventHandle>& delayed, TDelayFunc delayFunc) {
        return runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (delayFunc(ev)) {
                delayed.Reset(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });
    }

    void WaitForDelayed(TTestActorRuntime& runtime, THolder<IEventHandle>& delayed, TTestActorRuntime::TEventObserver prevObserver) {
        if (!delayed) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&delayed](IEventHandle&) -> bool {
                return bool(delayed);
            });
            runtime.DispatchEvents(opts);
        }

        runtime.SetObserverFunc(prevObserver);
    }

} // anonymous

Y_UNIT_TEST_SUITE(TRestoreTests) {
    void RestoreNoWait(TTestBasicRuntime& runtime, ui64& txId,
            ui16 port, THolder<TS3Mock>& s3Mock, TVector<TTestData>&& data, ui32 readBatchSize = 128) {

        const auto desc = DescribePath(runtime, "/MyRoot/Table", true, true);
        UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

        s3Mock.Reset(new TS3Mock(ConvertTestData({GenerateScheme(desc), std::move(data)}), TS3Mock::TSettings(port)));
        UNIT_ASSERT(s3Mock->Start());

        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);

        TestRestore(runtime, ++txId, "/MyRoot", Sprintf(R"(
            TableName: "Table"
            TableDescription {
                %s
            }
            S3Settings {
                Endpoint: "localhost:%d"
                Scheme: HTTP
                Limits {
                    ReadBatchSize: %d
                }
            }
        )", GenerateTableDescription(desc).data(), port, readBatchSize));
    }

    void Restore(TTestBasicRuntime& runtime, TTestEnv& env, const TString& creationScheme, TVector<TTestData>&& data, ui32 readBatchSize = 128) {
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", creationScheme);
        env.TestWaitNotification(runtime, txId);

        TPortManager portManager;
        THolder<TS3Mock> s3Mock;

        RestoreNoWait(runtime, txId, portManager.GetPort(), s3Mock, std::move(data), readBatchSize);
        env.TestWaitNotification(runtime, txId);
    }

    void Restore(TTestBasicRuntime& runtime, const TString& creationScheme, TVector<TTestData>&& data, ui32 readBatchSize = 128) {
        TTestEnv env(runtime, TTestEnvOptions().EnableParameterizedDecimal(true));
        Restore(runtime, env, creationScheme, std::move(data), readBatchSize);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnSingleShardTable) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(Codec, "a", 1);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    bool CheckDefaultFromSequence(const TTableDescription& desc) {
        for (const auto& column: desc.GetColumns()) {
            if (column.GetName() == "key") {
                switch (column.GetDefaultValueCase()) {
                    case TColumnDescription::kDefaultFromSequence: {
                        const auto& fromSequence = column.GetDefaultFromSequence();
                        return fromSequence == "myseq";
                    }
                    default: break;
                }
                break;
            }
        }
        return false;
    }

    bool CheckDefaultFromLiteral(const TTableDescription& desc) {
        for (const auto& column: desc.GetColumns()) {
            if (column.GetName() == "value") {
                switch (column.GetDefaultValueCase()) {
                    case TColumnDescription::kDefaultFromLiteral: {
                        const auto& fromLiteral = column.GetDefaultFromLiteral();

                        TString str;
                        google::protobuf::TextFormat::PrintToString(fromLiteral, &str);

                        TString result = R"(type {
  optional_type {
    item {
      type_id: UTF8
    }
  }
}
value {
  items {
    text_value: "value1"
  }
}
)";
                        return str == result;
                    }
                    default: break;
                }
                break;
            }
        }
        return false;
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedWithDefaultFromLiteral) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(Codec, "a", 1);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns {
                Name: "value"
                Type: "Utf8"
                DefaultFromLiteral {
                    type {
                        optional_type {
                            item {
                                type_id: UTF8
                            }
                        }
                    }
                    value {
                        items {
                            text_value: "value1"
                        }
                    }
                }
            }
            KeyColumnNames: ["key"]
        )", {data});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);

        const auto desc = DescribePath(runtime, "/MyRoot/Table", true, true);
        UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

        const auto& table = desc.GetPathDescription().GetTable();

        UNIT_ASSERT_C(CheckDefaultFromLiteral(table), "Invalid default value");
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnMultiShardTable) {
        TTestBasicRuntime runtime;

        const auto a = GenerateTestData(Codec, "a", 1);
        const auto b = GenerateTestData(Codec, "b", 1);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
            SplitBoundary {
              KeyPrefix {
                Tuple { Optional { Text: "b" } }
              }
            }
        )", {a, b});

        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(a.YsonStr, content);
        }
        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(b.YsonStr, content);
        }
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnLargeData) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(Codec, "", 100);
        UNIT_ASSERT(data.Data.size() > 128);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    void ShouldSucceedOnMultipleFrames(ui32 batchSize) {
        TTestBasicRuntime runtime;

        const auto data = GenerateZstdTestData("a", 3, 2);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data}, batchSize);

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST(ShouldSucceedOnMultipleFramesStandardBatch) {
        ShouldSucceedOnMultipleFrames(128);
    }

    Y_UNIT_TEST(ShouldSucceedOnMultipleFramesSmallBatch) {
        ShouldSucceedOnMultipleFrames(7);
    }

    Y_UNIT_TEST(ShouldSucceedOnMultipleFramesTinyBatch) {
        ShouldSucceedOnMultipleFrames(1);
    }

    Y_UNIT_TEST(ShouldSucceedOnSmallBuffer) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.GetAppData().ZstdBlockSizeForTest = 16;
        runtime.GetAppData().DataShardConfig.SetRestoreReadBufferSizeLimit(16);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        bool uploadResponseDropped = false;
        runtime.SetObserverFunc([&uploadResponseDropped](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvDataShard::EvS3UploadRowsResponse) {
                uploadResponseDropped = true;
                return TTestActorRuntime::EEventAction::DROP;
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TPortManager portManager;
        THolder<TS3Mock> s3Mock;
        const auto data = GenerateZstdTestData("a", 2);
        const ui32 batchSize = 1;
        RestoreNoWait(runtime, txId, portManager.GetPort(), s3Mock, {data}, batchSize);

        if (!uploadResponseDropped) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&uploadResponseDropped](IEventHandle&) -> bool {
                return uploadResponseDropped;
            });
            runtime.DispatchEvents(opts);
        }

        TMaybe<NKikimrTxDataShard::TShardOpResult> result;
        runtime.SetObserverFunc([&result](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvDataShard::EvSchemaChanged) {
                const auto& record = ev->Get<TEvDataShard::TEvSchemaChanged>()->Record;
                if (record.HasOpResult()) {
                    result = record.GetOpResult();
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        });

        RebootTablet(runtime, TTestTxConfig::FakeHiveTablets, runtime.AllocateEdgeActor());

        if (!result) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&result](IEventHandle&) -> bool {
                return result.Defined();
            });
            runtime.DispatchEvents(opts);
        }

        UNIT_ASSERT_VALUES_EQUAL(result->GetBytesProcessed(), 16);
        UNIT_ASSERT_VALUES_EQUAL(result->GetRowsProcessed(), 2);

        env.TestWaitNotification(runtime, txId);
        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST(ShouldNotDecompressEntirePortionAtOnce) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        runtime.GetAppData().ZstdBlockSizeForTest = 113; // one row

        ui32 uploadRowsCount = 0;
        runtime.SetObserverFunc([&uploadRowsCount](TAutoPtr<IEventHandle>& ev) {
            uploadRowsCount += ui32(ev->GetTypeRewrite() == TEvDataShard::EvS3UploadRowsResponse);
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        const auto data = GenerateZstdTestData(TString(100, 'a'), 2); // 2 rows, 1 row = 113b
        // ensure that one decompressed row is bigger than entire compressed file
        UNIT_ASSERT(data.Data.size() < *runtime.GetAppData().ZstdBlockSizeForTest);

        Restore(runtime, env, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data}, data.Data.size());

        UNIT_ASSERT_VALUES_EQUAL(uploadRowsCount, 2);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldExpandBuffer) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(Codec, "a", 2);
        const ui32 batchSize = 1;

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data}, batchSize);

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST(ShouldSucceedOnSupportedDatatypes) {
        TTestBasicRuntime runtime;

        TString csv = TStringBuilder()
            << "1," // key
            << "-100500," // int32
            << "100500," // uint32
            << "-200500," // int64
            << "200500," // uint64
            << "255," // uint8
            << "1," // bool
            << "1.1234," // double
            << "-1.123," // float
            << "2020-08-12T00:00:00.000000Z," // date
            << "2020-08-12T12:34:56.000000Z," // datetime
            << "2020-08-12T12:34:56.123456Z," // timestamp
            << "-300500," // interval
            << "-18486," // negative date32
            << "-1597235696," // negative datetime64
            << "-1597235696123456," // negative timestamp64
            << "-300500," // negative interval64
            << "3.321," // decimal
            << "555555555555555.123456789," // decimal(35,10)
            << ".3321e1," // dynumber
            << "\"" << CGIEscapeRet("lorem ipsum") << "\"," // string
            << "\"" << CGIEscapeRet("lorem ipsum dolor sit amet") << "\"," // utf8
            << "\"" << CGIEscapeRet(R"({"key": "value"})") << "\"," // json
            << "\"" << CGIEscapeRet(R"({"key": "value"})") << "\"," // jsondoc
            << "65df1ec1-a97d-47b2-ae56-3c023da6ee8c"
        << Endl;

        TString yson = TStringBuilder() << "[[[[["
            << "[%true];" // bool
            << "[\"" << -18486 << "\"];" // date32
            << "[\"" << TInstant::ParseIso8601("2020-08-12T00:00:00.000000Z").Days() << "\"];" // date
            << "[\"" << -1597235696 << "\"];" // datetime64
            << "[\"" << TInstant::ParseIso8601("2020-08-12T12:34:56.000000Z").Seconds() << "\"];" // datetime
            << "[\"" << "555555555555555.123456789" << "\"];" // decimal(35,10)
            << "[\"" << "3.321" << "\"];" // decimal
            << "[\"" << 1.1234 << "\"];" // double
            << "[\"" << ".3321e1" << "\"];" // dynumber
            << "[\"" << -1.123f << "\"];" // float
            << "[\"" << -100500 << "\"];" // int32
            << "[\"" << -200500 << "\"];" // int64
            << "[\"" << -300500 << "\"];" // interval64
            << "[\"" << -300500 << "\"];" // interval
            << "[\"" << "{\\\"key\\\": \\\"value\\\"}" << "\"];" // json
            << "[\"" << "{\\\"key\\\":\\\"value\\\"}" << "\"];" // jsondoc
            << "[\"" << 1 << "\"];" // key
            << "[\"" << "lorem ipsum" << "\"];" // string
            << "[\"" << -1597235696123456 << "\"];" // timestamp64
            << "[\"" << TInstant::ParseIso8601("2020-08-12T12:34:56.123456Z").MicroSeconds() << "\"];" // timestamp
            << "[\"" << 100500 << "\"];" // uint32
            << "[\"" << 200500 << "\"];" // uint64
            << "[\"" << 255 << "\"];" // uint8
            << "[\"" << "lorem ipsum dolor sit amet" << "\"];" // utf8
            << "[[\"" << "wR7fZX2pskeuVjwCPabujA==" << "\"]]" // uuid
        << "]];\%false]]]";

        const auto data = TTestData(std::move(csv), std::move(yson));

        Restore(runtime, R"_(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "int32_value" Type: "Int32" }
            Columns { Name: "uint32_value" Type: "Uint32" }
            Columns { Name: "int64_value" Type: "Int64" }
            Columns { Name: "uint64_value" Type: "Uint64" }
            Columns { Name: "uint8_value" Type: "Uint8" }
            Columns { Name: "bool_value" Type: "Bool" }
            Columns { Name: "double_value" Type: "Double" }
            Columns { Name: "float_value" Type: "Float" }
            Columns { Name: "date_value" Type: "Date" }
            Columns { Name: "datetime_value" Type: "Datetime" }
            Columns { Name: "timestamp_value" Type: "Timestamp" }
            Columns { Name: "interval_value" Type: "Interval" }
            Columns { Name: "date32_value" Type: "Date32" }
            Columns { Name: "datetime64_value" Type: "Datetime64" }
            Columns { Name: "timestamp64_value" Type: "Timestamp64" }
            Columns { Name: "interval64_value" Type: "Interval64" }
            Columns { Name: "decimal_value" Type: "Decimal" }
            Columns { Name: "decimal35_value" Type: "Decimal(35,10)" }
            Columns { Name: "dynumber_value" Type: "DyNumber" }
            Columns { Name: "string_value" Type: "String" }
            Columns { Name: "utf8_value" Type: "Utf8" }
            Columns { Name: "json_value" Type: "Json" }
            Columns { Name: "jsondoc_value" Type: "JsonDocument" }
            Columns { Name: "uuid_value" Type: "Uuid" }
            KeyColumnNames: ["key"]
        )_", {data}, data.Data.size() + 1);

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {
            "key",
            "int32_value",
            "uint32_value",
            "int64_value",
            "uint64_value",
            "uint8_value",
            "bool_value",
            "double_value",
            "float_value",
            "date_value",
            "datetime_value",
            "timestamp_value",
            "interval_value",
            "date32_value",
            "datetime64_value",
            "timestamp64_value",
            "interval64_value",
            "decimal_value",
            "decimal35_value",
            "dynumber_value",
            "string_value",
            "utf8_value",
            "json_value",
            "jsondoc_value",
            "uuid_value",
        });
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST(ShouldRestoreSpecialFpValues) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "double_value" Type: "Double" }
            Columns { Name: "float_value" Type: "Float" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto writeRow = [&](ui64 key, double doubleValue, float floatValue) {
            NKikimrMiniKQL::TResult result;
            TString error;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, TTestTxConfig::FakeHiveTablets, Sprintf(R"(
                (
                    (let key '( '('key (Uint32 '%lu) ) ) )
                    (let row '( '('double_value (Double '%lf ) ) '('float_value (Float '%f) ) ) )
                    (return (AsList (UpdateRow '__user__Original key row) ))
                )
            )", key, doubleValue, floatValue), result, error);

            UNIT_ASSERT_VALUES_EQUAL_C(status, NKikimrProto::EReplyStatus::OK, error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
        };

        using double_conversion::Double;

        writeRow(1, Double::NaN(), static_cast<float>(Double::NaN()));
        writeRow(2, -Double::NaN(), static_cast<float>(-Double::NaN()));
        writeRow(3, Double::Infinity(), static_cast<float>(Double::Infinity()));
        writeRow(4, -Double::Infinity(), static_cast<float>(-Double::Infinity()));

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");
    }

    Y_UNIT_TEST(ShouldRestoreDefaultValuesFromLiteral) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Utf8" }
            Columns {
                Name: "value"
                Type: "Utf8"
                DefaultFromLiteral {
                    type {
                        optional_type {
                            item {
                                type_id: UTF8
                            }
                        }
                    }
                    value {
                        items {
                            text_value: "value1"
                        }
                    }
                }
            }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");

        const auto desc = DescribePath(runtime, "/MyRoot/Restored", true, true);
        UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

        const auto& table = desc.GetPathDescription().GetTable();

        UNIT_ASSERT_C(CheckDefaultFromLiteral(table), "Invalid default value");
    }

    Y_UNIT_TEST(ShouldRestoreDefaultValuesFromSequence) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Original"
                Columns { Name: "key" Type: "Uint64" DefaultFromSequence: "myseq" }
                Columns { Name: "value" Type: "Uint64" }
                KeyColumnNames: ["key"]
            }
            SequenceDescription {
                Name: "myseq"
            }
        )");

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");

        const auto desc = DescribePath(runtime, "/MyRoot/Restored", true, true);
        UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

        const auto& table = desc.GetPathDescription().GetTable();

        UNIT_ASSERT_C(CheckDefaultFromSequence(table), "Invalid default value");
    }

    Y_UNIT_TEST(ShouldRestoreSequence) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::SEQUENCEPROXY, NActors::NLog::PRI_TRACE);

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Original"
                Columns { Name: "key" Type: "Uint64" DefaultFromSequence: "myseq" }
                Columns { Name: "value" Type: "Uint64" }
                KeyColumnNames: ["key"]
            }
            SequenceDescription {
                Name: "myseq"
            }
        )");
        env.TestWaitNotification(runtime, txId);

        i64 value = DoNextVal(runtime, "/MyRoot/Original/myseq");
        UNIT_ASSERT_VALUES_EQUAL(value, 1);

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");

        const auto desc = DescribePath(runtime, "/MyRoot/Restored", true, true);
        UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

        const auto& table = desc.GetPathDescription().GetTable();

        value = DoNextVal(runtime, "/MyRoot/Restored/myseq");
        UNIT_ASSERT_VALUES_EQUAL(value, 2);

        UNIT_ASSERT_C(CheckDefaultFromSequence(table), "Invalid default value");
    }

    Y_UNIT_TEST(ShouldRestoreSequenceWithOverflow) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::SEQUENCEPROXY, NActors::NLog::PRI_TRACE);

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Original"
                Columns { Name: "key" Type: "Uint64" DefaultFromSequence: "myseq" }
                Columns { Name: "value" Type: "Uint64" }
                KeyColumnNames: ["key"]
            }
            SequenceDescription {
                Name: "myseq"
                MinValue: 1
                MaxValue: 2
            }
        )");
        env.TestWaitNotification(runtime, txId);

        i64 value = DoNextVal(runtime, "/MyRoot/Original/myseq");
        UNIT_ASSERT_VALUES_EQUAL(value, 1);

        value = DoNextVal(runtime, "/MyRoot/Original/myseq");
        UNIT_ASSERT_VALUES_EQUAL(value, 2);

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");

        const auto desc = DescribePath(runtime, "/MyRoot/Restored", true, true);
        UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

        const auto& table = desc.GetPathDescription().GetTable();

        value = DoNextVal(runtime, "/MyRoot/Restored/myseq", Ydb::StatusIds::SCHEME_ERROR);

        UNIT_ASSERT_C(CheckDefaultFromSequence(table), "Invalid default value");
    }

    Y_UNIT_TEST(ShouldRestoreTableWithVolatilePartitioningMerge) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::SEQUENCEPROXY, NActors::NLog::PRI_TRACE);

        // Create table with 2 tablets
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
            PartitionConfig {
                PartitioningPolicy {
                    MinPartitionsCount: 2
                    MaxPartitionsCount: 2
                }
            }
            SplitBoundary {
              KeyPrefix {
                Tuple { Optional { Uint32: 2 } }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // Upload data
        const auto firstTablet = TTestTxConfig::FakeHiveTablets;
        const auto secondTablet = TTestTxConfig::FakeHiveTablets + 1;
        UpdateRow(runtime, "Original", 1, "valueA", firstTablet);
        UpdateRow(runtime, "Original", 2, "valueB", secondTablet);

        // Add delay after copying tables
        ui64 copyTablesTxId;
        auto prevObserver = runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto* msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
                if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables) {
                    copyTablesTxId = msg->Record.GetTxId();
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TBlockEvents<TEvSchemeShard::TEvNotifyTxCompletionResult> delay(runtime, [&](auto& ev) {
            return ev->Get()->Record.GetTxId() == copyTablesTxId;
        });

        // Start exporting table
        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        const ui64 exportId = txId;

        // Wait for delay after copying tables
        runtime.WaitFor("delay after copying tables", [&]{ return delay.size() >= 1; });

        // Merge 2 tablets in 1 during the delay
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            PartitionConfig {
                PartitioningPolicy {
                    MinPartitionsCount: 1
                    MaxPartitionsCount: 1
                }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestSplitTable(runtime, ++txId, "/MyRoot/Original", Sprintf(R"(
            SourceTabletId: %lu
            SourceTabletId: %lu
        )", firstTablet, secondTablet));
        env.TestWaitNotification(runtime, txId);

        // Finish the delay and continue exporting
        delay.Unblock();
        env.TestWaitNotification(runtime, exportId);

        // Check export
        TestGetExport(runtime, exportId, "/MyRoot");

        // Restore table
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        const ui64 importId = txId;
        env.TestWaitNotification(runtime, importId);

        // Check import
        TestGetImport(runtime, importId, "/MyRoot");

        // Check partitioning in restored table
        TestDescribeResult(DescribePath(runtime, "/MyRoot/Restored", true, true), {
            NLs::MinPartitionsCountEqual(2),
            NLs::MaxPartitionsCountEqual(2),
            NLs::CheckBoundaries
        });

        // Check data in restored table
        const auto restoredFirstTablet = TTestTxConfig::FakeHiveTablets + 5;
        const auto restoredSecondTablet = TTestTxConfig::FakeHiveTablets + 6;
        {
            auto expectedJson = TStringBuilder() << "[[[["
                << "["
                    << R"(["1"];)" // key
                    << R"(["valueA"])" // value
                << "];"
            << "];\%false]]]";
            auto content = ReadTable(runtime, restoredFirstTablet, "Restored", {"key"}, {"key", "value"});
            NKqp::CompareYson(expectedJson, content);
        }
        {
            auto expectedJson = TStringBuilder() << "[[[["
                << "["
                    << R"(["2"];)" // key
                    << R"(["valueB"])" // value
                << "];"
            << "];\%false]]]";
            auto content = ReadTable(runtime, restoredSecondTablet, "Restored", {"key"}, {"key", "value"});
            NKqp::CompareYson(expectedJson, content);
        }
    }

    Y_UNIT_TEST(ShouldRestoreTableWithVolatilePartitioningSplit) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::SEQUENCEPROXY, NActors::NLog::PRI_TRACE);

        // Create table with 2 tablets
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
            PartitionConfig {
                PartitioningPolicy {
                    MinPartitionsCount: 2
                    MaxPartitionsCount: 2
                }
            }
            SplitBoundary {
              KeyPrefix {
                Tuple { Optional { Uint32: 3 } }
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // Upload data
        const auto firstTablet = TTestTxConfig::FakeHiveTablets;
        UpdateRow(runtime, "Original", 1, "valueA", firstTablet);
        UpdateRow(runtime, "Original", 2, "valueB", firstTablet);

        // Add delay after copying tables
        ui64 copyTablesTxId;
        auto prevObserver = runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto* msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
                if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables) {
                    copyTablesTxId = msg->Record.GetTxId();
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TBlockEvents<TEvSchemeShard::TEvNotifyTxCompletionResult> delay(runtime, [&](auto& ev) {
            return ev->Get()->Record.GetTxId() == copyTablesTxId;
        });

        // Start exporting table
        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        const ui64 exportId = txId;

        // Wait for delay after copying tables
        runtime.WaitFor("delay after copying tables", [&]{ return delay.size() >= 1; });

        // Split 2 tablets in 3 during the delay
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            PartitionConfig {
                PartitioningPolicy {
                    MinPartitionsCount: 3
                    MaxPartitionsCount: 3
                }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestSplitTable(runtime, ++txId, "/MyRoot/Original", Sprintf(R"(
            SourceTabletId: %lu
            SplitBoundary {
                KeyPrefix {
                    Tuple { Optional { Uint32: 2 } }
                }
            }
        )", firstTablet));
        env.TestWaitNotification(runtime, txId);

        // Finish the delay and continue exporting
        delay.Unblock();
        env.TestWaitNotification(runtime, exportId);

        // Check export
        TestGetExport(runtime, exportId, "/MyRoot");

        // Restore table
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        const ui64 importId = txId;
        env.TestWaitNotification(runtime, importId);

        // Check import
        TestGetImport(runtime, importId, "/MyRoot");

        // Check partitioning in restored table
        TestDescribeResult(DescribePath(runtime, "/MyRoot/Restored", true, true), {
            NLs::MinPartitionsCountEqual(2),
            NLs::MaxPartitionsCountEqual(2),
            NLs::CheckBoundaries
        });

        // Check data in restored table
        const auto restoredFirstTablet = TTestTxConfig::FakeHiveTablets + 6;
        const auto restoredSecondTablet = TTestTxConfig::FakeHiveTablets + 7;
        {
            auto expectedJson = TStringBuilder() << "[[[["
                << "["
                    << R"(["1"];)" // key
                    << R"(["valueA"])" // value
                << "];"
                << "["
                    << R"(["2"];)" // key
                    << R"(["valueB"])" // value
                << "];"
            << "];\%false]]]";
            auto content = ReadTable(runtime, restoredFirstTablet, "Restored", {"key"}, {"key", "value"});
            NKqp::CompareYson(expectedJson, content);
        }
        {
            auto expectedJson = "[[[[];\%false]]]";
            auto content = ReadTable(runtime, restoredSecondTablet, "Restored", {"key"}, {"key", "value"});
            NKqp::CompareYson(expectedJson, content);
        }
    }

    void ExportImportOnSupportedDatatypesImpl(bool encrypted, bool commonPrefix, bool emptyTable = false) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableParameterizedDecimal(true));
        runtime.GetAppData().FeatureFlags.SetEnableEncryptedExport(true);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"_(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "int32_value" Type: "Int32" }
            Columns { Name: "uint32_value" Type: "Uint32" }
            Columns { Name: "int64_value" Type: "Int64" }
            Columns { Name: "uint64_value" Type: "Uint64" }
            Columns { Name: "uint8_value" Type: "Uint8" }
            Columns { Name: "bool_value" Type: "Bool" }
            Columns { Name: "double_value" Type: "Double" }
            Columns { Name: "float_value" Type: "Float" }
            Columns { Name: "date_value" Type: "Date" }
            Columns { Name: "datetime_value" Type: "Datetime" }
            Columns { Name: "timestamp_value" Type: "Timestamp" }
            Columns { Name: "interval_value" Type: "Interval" }
            Columns { Name: "date32_value" Type: "Date32" }
            Columns { Name: "datetime64_value" Type: "Datetime64" }
            Columns { Name: "timestamp64_value" Type: "Timestamp64" }
            Columns { Name: "interval64_value" Type: "Interval64" }
            Columns { Name: "decimal_value" Type: "Decimal" }
            Columns { Name: "decimal35_value" Type: "Decimal(35,10)" }
            Columns { Name: "dynumber_value" Type: "DyNumber" }
            Columns { Name: "string_value" Type: "String" }
            Columns { Name: "utf8_value" Type: "Utf8" }
            Columns { Name: "json_value" Type: "Json" }
            Columns { Name: "jsondoc_value" Type: "JsonDocument" }
            Columns { Name: "uuid_value" Type: "Uuid" }
            KeyColumnNames: ["key"]
        )_");
        env.TestWaitNotification(runtime, txId);

        if (!emptyTable) {
            const int partitionIdx = 0;

            const TVector<TCell> keys = {TCell::Make(1ull)};

            const TString string = "test string";
            const TString json = R"({"key": "value"})";
            auto binaryJson = NBinaryJson::SerializeToBinaryJson(json);
            Y_ABORT_UNLESS(std::holds_alternative<NBinaryJson::TBinaryJson>(binaryJson));
            const auto& binaryJsonValue = std::get<NBinaryJson::TBinaryJson>(binaryJson);

            const std::pair<ui64, ui64> decimal = NYql::NDecimal::MakePair(NYql::NDecimal::FromString("16.17", NScheme::DECIMAL_PRECISION, NScheme::DECIMAL_SCALE));
            const std::pair<ui64, ui64> decimal35 = NYql::NDecimal::MakePair(NYql::NDecimal::FromString("555555555555555.123456789", 35, 10));
            const TString dynumber = *NDyNumber::ParseDyNumberString("18");

            char uuid[16];
            NUuid::ParseUuidToArray(TString("65df1ec1-a97d-47b2-ae56-3c023da6ee8c"), reinterpret_cast<ui16*>(uuid), false);

            const TVector<TCell> values = {
                TCell::Make<i32>(-1), // Int32
                TCell::Make<ui32>(2), // Uint32
                TCell::Make<i64>(-3), // Int64
                TCell::Make<ui64>(4), // Uint64
                TCell::Make<ui8>(5), // Uint8
                TCell::Make<bool>(true), // Bool
                TCell::Make<double>(6.66), // Double
                TCell::Make<float>(7.77), // Float
                TCell::Make<ui16>(8), // Date
                TCell::Make<ui32>(9), // Datetime
                TCell::Make<ui64>(10), // Timestamp
                TCell::Make<i64>(-11), // Interval
                TCell::Make<i32>(-12), // Date32
                TCell::Make<i64>(-13), // Datetime64
                TCell::Make<i64>(-14), // Timestamp64
                TCell::Make<i64>(-15), // Interval64
                TCell::Make<std::pair<ui64, ui64>>(decimal), // Decimal
                TCell::Make<std::pair<ui64, ui64>>(decimal35), // Decimal
                TCell(dynumber.data(), dynumber.size()), // Dynumber
                TCell(string.data(), string.size()), // String
                TCell(string.data(), string.size()), // Utf8
                TCell(json.data(), json.size()), // Json
                TCell(binaryJsonValue.Data(), binaryJsonValue.Size()), // JsonDocument
                TCell(uuid, sizeof(uuid)), // Uuid
            };

            const TVector<ui32> keyTags = {1};
            TVector<ui32> valueTags(values.size());
            std::iota(valueTags.begin(), valueTags.end(), 2);

            UploadRow(runtime, "/MyRoot/Table", partitionIdx, keyTags, valueTags, keys, values);
        }

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TString encryptionSettings;
        if (encrypted) {
            encryptionSettings = R"(encryption_settings {
                encryption_algorithm: "ChaCha20-Poly1305"
                symmetric_key {
                    key: "Very very secret export key!!!!!"
                }
            })";
        }
        TString exportItems, importItems;
        if (commonPrefix) {
            exportItems = R"(
                source_path: "/MyRoot"
                destination_prefix: "BackupPrefix"
                items {
                    source_path: "Table"
                }
            )";
            importItems = R"(
                source_prefix: "BackupPrefix"
                destination_path: "/MyRoot/Restored"
            )";
        } else {
            exportItems = R"(
                items {
                    source_path: "/MyRoot/Table"
                    destination_prefix: "Backup1"
                }
            )";
            importItems = R"(
                items {
                    source_prefix: "Backup1"
                    destination_path: "/MyRoot/Restored"
                }
            )";
        }

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              %s
              %s
            }
        )", port, exportItems.c_str(), encryptionSettings.c_str()));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              %s
              %s
            }
        )", port, importItems.c_str(), encryptionSettings.c_str()));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");

        if (!emptyTable) {
            TString expectedJson = TStringBuilder() << "[[[[["
                << "[%true];" // bool
                << "[\"" << -12 << "\"];" // date32
                << "[\"" << 8 << "\"];" // date
                << "[\"" << -13 << "\"];" // datetime64
                << "[\"" << 9 << "\"];" // datetime
                << "[\"" << "555555555555555.123456789" << "\"];" // decimal35
                << "[\"" << "16.17" << "\"];" // decimal
                << "[\"" << 6.66 << "\"];" // double
                << "[\"" << ".18e2" << "\"];" // dynumber
                << "[\"" << 7.77f << "\"];" // float
                << "[\"" << -1 << "\"];" // int32
                << "[\"" << -3 << "\"];" // int64
                << "[\"" << -15 << "\"];" // interval64
                << "[\"" << -11 << "\"];" // interval
                << "[\"" << "{\\\"key\\\": \\\"value\\\"}" << "\"];" // json
                << "[\"" << "{\\\"key\\\":\\\"value\\\"}" << "\"];" // jsondoc
                << "[\"" << 1 << "\"];" // key
                << "[\"" << "test string" << "\"];" // string
                << "[\"" << -14 << "\"];" // timestamp64
                << "[\"" << 10 << "\"];" // timestamp
                << "[\"" << 2 << "\"];" // uint32
                << "[\"" << 4 << "\"];" // uint64
                << "[\"" << 5 << "\"];" // uint8
                << "[\"" << "test string" << "\"];" // utf8
                << "[[\"" << "wR7fZX2pskeuVjwCPabujA==" << "\"]]" // uuid
            << "]];\%false]]]";

            const TVector<TString> readColumns = {
                "key",
                "int32_value",
                "uint32_value",
                "int64_value",
                "uint64_value",
                "uint8_value",
                "bool_value",
                "double_value",
                "float_value",
                "date_value",
                "datetime_value",
                "timestamp_value",
                "interval_value",
                "date32_value",
                "datetime64_value",
                "timestamp64_value",
                "interval64_value",
                "decimal_value",
                "decimal35_value",
                "dynumber_value",
                "string_value",
                "utf8_value",
                "json_value",
                "jsondoc_value",
                "uuid_value",
            };

            auto contentOriginalTable = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, readColumns);
            NKqp::CompareYson(expectedJson, contentOriginalTable);

            auto contentRestoredTable = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 2, commonPrefix ? "Table" : "Restored", {"key"}, readColumns);
            NKqp::CompareYson(expectedJson, contentRestoredTable);
        }
    }

    Y_UNIT_TEST(ExportImportOnSupportedDatatypes) {
        ExportImportOnSupportedDatatypesImpl(false, false);
    }

    Y_UNIT_TEST(ExportImportOnSupportedDatatypesWithCommonDestPrefix) {
        ExportImportOnSupportedDatatypesImpl(false, true);
    }

    Y_UNIT_TEST(ExportImportOnSupportedDatatypesEncrypted) {
        ExportImportOnSupportedDatatypesImpl(true, true);
    }

    Y_UNIT_TEST(ExportImportOnSupportedDatatypesEncryptedNoData) {
        ExportImportOnSupportedDatatypesImpl(true, true, true);
    }

    Y_UNIT_TEST(ZeroLengthEncryptedFileTreatedAsCorrupted) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableParameterizedDecimal(true));
        runtime.GetAppData().FeatureFlags.SetEnableEncryptedExport(true);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"_(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "String" }
            KeyColumnNames: ["key"]
        )_");
        env.TestWaitNotification(runtime, txId);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              source_path: "/MyRoot"
                destination_prefix: "BackupPrefix"
                items {
                    source_path: "Table"
                }
              encryption_settings {
                encryption_algorithm: "ChaCha20-Poly1305"
                symmetric_key {
                    key: "Very very secret export key!!!!!"
                }
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        // Successfully imports
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              source_prefix: "BackupPrefix"
              destination_path: "/MyRoot/Restored"
              encryption_settings {
                symmetric_key {
                    key: "Very very secret export key!!!!!"
                }
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");

        // Delete data from different files
        auto checkFailsIfFileIsEmpty = [&](const TString& fileName) {
            TString& data = s3Mock.GetData()[fileName];
            UNIT_ASSERT(!data.empty());
            TString srcData = data;
            data.clear();

            TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
              ImportFromS3Settings {
                endpoint: "localhost:%d"
                scheme: HTTP
                source_prefix: "BackupPrefix"
                destination_path: "/MyRoot/Restored2"
                encryption_settings {
                  symmetric_key {
                    key: "Very very secret export key!!!!!"
                  }
                }
              }
            )", port));
            env.TestWaitNotification(runtime, txId);
            TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::CANCELLED);

            data = srcData;
        };

        checkFailsIfFileIsEmpty("/BackupPrefix/SchemaMapping/metadata.json.enc");
        checkFailsIfFileIsEmpty("/BackupPrefix/SchemaMapping/mapping.json.enc");
        checkFailsIfFileIsEmpty("/BackupPrefix/001/data_00.csv.enc");
        checkFailsIfFileIsEmpty("/BackupPrefix/001/metadata.json.enc");
    }

    Y_UNIT_TEST(ExportImportPg) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableTablePgTypes(true));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "pgint4" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        UploadRow(runtime, "/MyRoot/Table", 0, {1}, {2}, {TCell::Make(55555u)}, {TCell::Make(55555u)});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: "Backup1"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "Backup1"
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");
    }

    Y_UNIT_TEST(ExportImportDecimalKey) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableParameterizedDecimal(true));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"_(
            Name: "Table"
            Columns { Name: "key" Type: "Decimal(2,1)" }
            Columns { Name: "value" Type: "Decimal(35,10)" }
            KeyColumnNames: ["key"]
        )_");
        env.TestWaitNotification(runtime, txId);

        const std::pair<ui64, ui64> decimal2 = NYql::NDecimal::MakePair(NYql::NDecimal::FromString("32.1", 2, 1));
        const std::pair<ui64, ui64> decimal35 = NYql::NDecimal::MakePair(NYql::NDecimal::FromString("555555555555555.123456789", 35, 10));
        UploadRow(runtime, "/MyRoot/Table", 0, {1}, {2},
            {TCell::Make<std::pair<ui64, ui64>>(decimal2)}, {TCell::Make<std::pair<ui64, ui64>>(decimal35)});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: "Backup1"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "Backup1"
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");
    }

    Y_UNIT_TEST(ExportImportUuid) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableTablePgTypes(true));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Uuid" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        {
            TString tablePath = "/MyRoot/Table";
            int partitionIdx = 0;

            auto tableDesc = DescribePath(runtime, tablePath, true, true);
            const auto& tablePartitions = tableDesc.GetPathDescription().GetTablePartitions();
            UNIT_ASSERT(partitionIdx < tablePartitions.size());
            const ui64 datashardTabletId = tablePartitions[partitionIdx].GetDatashardId();

            NKikimrMiniKQL::TResult result;
            TString error;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, datashardTabletId, Sprintf(R"(
                (
                    (let key '( '('key (Uint32 '%d) ) ) )
                    (let row '( '('value (Uuid '"%s") ) ) )
                    (return (AsList (UpdateRow '__user__%s key row) ))
                )
            )", 1, "0123456789012345", "Table"), result, error);

            UNIT_ASSERT_VALUES_EQUAL_C(status, NKikimrProto::EReplyStatus::OK, error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
        }

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Table"
                destination_prefix: "Backup1"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "Backup1"
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot");
    }

     Y_UNIT_TEST_WITH_COMPRESSION(ExportImportWithChecksums) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableChecksumsExport(true));

        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        // Create table
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Upload data
        UpdateRow(runtime, "Original", 1, "valueA", TTestTxConfig::FakeHiveTablets);

        // Export table
        const char* compression = Codec == ECompressionCodec::Zstd ? "zstd" : "";
        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
              compression: "%s"
            }
        )", port, compression));
        const ui64 exportId = txId;
        env.TestWaitNotification(runtime, exportId);

        // Check export
        TestGetExport(runtime, exportId, "/MyRoot");

        UNIT_ASSERT_VALUES_EQUAL(s3Mock.GetData().size(), 6);

        // Restore table
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        const ui64 importId = txId;
        env.TestWaitNotification(runtime, importId);

        // Check import
        TestGetImport(runtime, importId, "/MyRoot");

        // Check data in restored table
        {
            auto expectedJson = TStringBuilder() << "[[[["
                << "["
                    << R"(["1"];)" // key
                    << R"(["valueA"])" // value
                << "];"
            << "];\%false]]]";
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 2, "Restored", {"key"}, {"key", "value"});
            NKqp::CompareYson(expectedJson, content);
        }
    }

    template<ECompressionCodec Codec = ECompressionCodec::None, typename T>
    void ExportImportWithCorruption(T corruption) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableChecksumsExport(true).EnablePermissionsExport(true));

        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        // Create table
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // Upload data
        UpdateRow(runtime, "Original", 1, "valueA", TTestTxConfig::FakeHiveTablets);

        // Export table
        const char* compression = Codec == ECompressionCodec::Zstd ? "zstd" : "";
        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
              compression: "%s"
            }
        )", port, compression));
        const ui64 exportId = txId;
        env.TestWaitNotification(runtime, exportId);

        // Check export
        TestGetExport(runtime, exportId, "/MyRoot");

        UNIT_ASSERT_VALUES_EQUAL(s3Mock.GetData().size(), 8);

        // Make corruption
        corruption(s3Mock.GetData());

        // Restore corrupted table
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored1"
              }
            }
        )", port));
        ui64 importId = txId;
        env.TestWaitNotification(runtime, importId);

        // Check corrupted import
        TestGetImport(runtime, importId, "/MyRoot", Ydb::StatusIds::CANCELLED);

        // Restore corrupted table with skip checksum validation
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored2"
              }
              skip_checksum_validation: true
            }
        )", port));
        importId = txId;
        env.TestWaitNotification(runtime, importId);

        // Check corrupted import with skip checksum validation
        TestGetImport(runtime, importId, "/MyRoot");
    }

    Y_UNIT_TEST(ExportImportWithMetadataCorruption) {
        ExportImportWithCorruption([](auto& s3){
            s3["/metadata.json"] = "corrupted";
        });
    }

    Y_UNIT_TEST(ExportImportWithSchemeCorruption) {
        ExportImportWithCorruption([](auto& s3){
            s3["/scheme.pb"] = std::regex_replace(std::string(s3["/scheme.pb"]), std::regex("value"), "val");
        });
    }

    Y_UNIT_TEST(ExportImportWithPermissionsCorruption) {
        ExportImportWithCorruption([](auto& s3){
            s3["/permissions.pb"] = std::regex_replace(std::string(s3["/permissions.pb"]), std::regex("root"), "alice");
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ExportImportWithDataCorruption) {
        ExportImportWithCorruption<Codec>([](auto& s3){
            s3["/data_00.csv"] = std::regex_replace(std::string(s3["/data_00.csv"]), std::regex("valueA"), "valueB");
        });
    }

    Y_UNIT_TEST(ExportImportWithMetadataChecksumCorruption) {
        ExportImportWithCorruption([](auto& s3){
            s3["/metadata.json.sha256"] = "corrupted";
        });
    }

    Y_UNIT_TEST(ExportImportWithSchemeChecksumCorruption) {
        ExportImportWithCorruption([](auto& s3){
            s3["/scheme.pb.sha256"] = "corrupted";
        });
    }

    Y_UNIT_TEST(ExportImportWithPermissionsChecksumCorruption) {
        ExportImportWithCorruption([](auto& s3){
            s3["/permissions.pb.sha256"] = "corrupted";
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ExportImportWithDataChecksumCorruption) {
        ExportImportWithCorruption<Codec>([](auto& s3){
            s3["/data_00.csv.sha256"] = "corrupted";
        });
    }

    Y_UNIT_TEST(ExportImportWithMetadataChecksumAbsence) {
        ExportImportWithCorruption([](auto& s3){
            s3.erase("/metadata.json.sha256");
        });
    }

    Y_UNIT_TEST(ExportImportWithSchemeChecksumAbsence) {
        ExportImportWithCorruption([](auto& s3){
            s3.erase("/scheme.pb.sha256");
        });
    }

    Y_UNIT_TEST(ExportImportWithPermissionsChecksumAbsence) {
        ExportImportWithCorruption([](auto& s3){
            s3.erase("/permissions.pb.sha256");
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ExportImportWithDataChecksumAbsence) {
        ExportImportWithCorruption<Codec>([](auto& s3){
            s3.erase("/data_00.csv.sha256");
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldCountWrittenBytesAndRows) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        const auto data = GenerateTestData(Codec, "a", 2);

        TMaybe<NKikimrTxDataShard::TShardOpResult> result;
        runtime.SetObserverFunc([&result](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvDataShard::EvSchemaChanged) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            const auto& record = ev->Get<TEvDataShard::TEvSchemaChanged>()->Record;
            if (!record.HasOpResult()) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            result = record.GetOpResult();
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        Restore(runtime, env, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data});

        if (!result) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&result](IEventHandle&) -> bool {
                return result.Defined();
            });
            runtime.DispatchEvents(opts);
        }

        UNIT_ASSERT_VALUES_EQUAL(result->GetBytesProcessed(), 16);
        UNIT_ASSERT_VALUES_EQUAL(result->GetRowsProcessed(), 2);
    }

    Y_UNIT_TEST(ShouldHandleOverloadedShard) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // prepare table schema with special policy
        TTableDescription desc;
        desc.SetName("Table");
        desc.AddKeyColumnNames("key");
        {
            auto& column = *desc.AddColumns();
            column.SetName("key");
            column.SetType("Uint32");
        }
        {
            auto& column = *desc.AddColumns();
            column.SetName("value");
            column.SetType("Utf8");
        }

        auto policy = NLocalDb::CreateDefaultUserTablePolicy();
        policy->InMemForceSizeToSnapshot = 1;
        policy->Serialize(*desc.MutablePartitionConfig()->MutableCompactionPolicy());

        // serialize schema
        TString scheme;
        UNIT_ASSERT(google::protobuf::TextFormat::PrintToString(desc, &scheme));
        TestCreateTable(runtime, ++txId, "/MyRoot", scheme);
        env.TestWaitNotification(runtime, txId);

        ui32 requests = 0;
        ui32 responses = 0;
        runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            requests += ui32(ev->GetTypeRewrite() == TEvDataShard::EvS3UploadRowsRequest);
            responses += ui32(ev->GetTypeRewrite() == TEvDataShard::EvS3UploadRowsResponse);
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TPortManager portManager;
        THolder<TS3Mock> s3Mock;

        const auto data = GenerateTestData("", 1000);
        const ui32 batchSize = 32;
        RestoreNoWait(runtime, ++txId, portManager.GetPort(), s3Mock, {data}, batchSize);
        env.TestWaitNotification(runtime, txId);

        const ui32 expected = data.Data.size() / batchSize + ui32(bool(data.Data.size() % batchSize));
        UNIT_ASSERT(requests > expected);
        UNIT_ASSERT_VALUES_EQUAL(responses, expected);

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    template <ECompressionCodec Codec>
    void ShouldFailOnFileWithoutNewLines(ui32 batchSize) {
        TTestBasicRuntime runtime;

        const TString v = "\"a1\",\"value1\"";
        const auto d = Codec == ECompressionCodec::Zstd ? ZstdCompress(v) : v;
        const auto data = TTestData(d, EmptyYsonStr, Codec);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data}, batchSize);

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnFileWithoutNewLinesStandardBatch) {
        ShouldFailOnFileWithoutNewLines<Codec>(128);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnFileWithoutNewLinesSmallBatch) {
        ShouldFailOnFileWithoutNewLines<Codec>(1);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnEmptyToken) {
        TTestBasicRuntime runtime;

        const TString v = "\"a1\",\n";
        const auto d = Codec == ECompressionCodec::Zstd ? ZstdCompress(v) : v;
        const auto data = TTestData(d, EmptyYsonStr, Codec);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnInvalidValue) {
        TTestBasicRuntime runtime;

        const TString v = "\"a1\",\"value1\"\n";
        const auto d = Codec == ECompressionCodec::Zstd ? ZstdCompress(v) : v;
        const auto data = TTestData(d, EmptyYsonStr, Codec);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnOutboundKey) {
        TTestBasicRuntime runtime;

        const auto a = GenerateTestData(Codec, "a", 1);
        const auto b = TTestData(a.Data, EmptyYsonStr);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
            SplitBoundary {
              KeyPrefix {
                Tuple { Optional { Text: "b" } }
              }
            }
        )", {a, b});

        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(a.YsonStr, content);
        }
        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(b.YsonStr, content);
        }
    }

    Y_UNIT_TEST(ShouldFailOnInvalidFrame) {
        TTestBasicRuntime runtime;

        const TString garbage = "\"a1\",\"value1\""; // not valid zstd data
        const auto data = TTestData(garbage, EmptyYsonStr, ECompressionCodec::Zstd);

        Restore(runtime, R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )", {data});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    void TestRestoreNegative(TTestActorRuntime& runtime, ui64 txId, const TString& parentPath, const TString& name,
            const TVector<TExpectedResult>& expectedResults) {

        TestRestore(runtime, ++txId, parentPath, Sprintf(R"(
            TableName: "%s"
            S3Settings {
                Endpoint: "localhost"
                Scheme: HTTP
            }
        )", name.data()), expectedResults);
    }

    Y_UNIT_TEST(ShouldFailOnVariousErrors) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        TestRestoreNegative(runtime, ++txId, "/MyRoot", "Table", {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestMkDir(runtime, ++txId, "/MyRoot", "Dir");
        TestRestoreNegative(runtime, ++txId, "/MyRoot", "Dir", {NKikimrScheme::StatusNameConflict});
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestRestoreNegative(runtime, ++txId, "/MyRoot", "Dir", {NKikimrScheme::StatusNameConflict});
        TestRestoreNegative(runtime, ++txId, "/MyRoot", "NotExist", {NKikimrScheme::StatusPathDoesNotExist});

        TestAlterTable(runtime, ++txId, "/MyRoot",R"(
            Name: "Table"
            Columns { Name: "extra"  Type: "Utf8"}
        )");
        TestRestoreNegative(runtime, ++txId, "/MyRoot", "Table", {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestDropTable(runtime, ++txId, "/MyRoot", "Table");
        TestRestoreNegative(runtime, ++txId, "/MyRoot", "Table", {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
              Name: "IndexedTable"
              Columns { Name: "key" Type: "Utf8" }
              Columns { Name: "value" Type: "Utf8" }
              KeyColumnNames: ["key"]
            }
            IndexDescription {
              Name: "ByValue"
              KeyColumnNames: ["value"]
            }
        )");
        TestRestoreNegative(runtime, ++txId, "/MyRoot", "IndexedTable", {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestRestoreNegative(runtime, ++txId, "/MyRoot", "IndexedTable", {NKikimrScheme::StatusInvalidParameter});
    }

    template <typename TEvToDelay>
    void CancelShouldSucceed(const TTestData& data, bool kill = false) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        THolder<IEventHandle> delayed;
        auto prevObserver = SetDelayObserver(runtime, delayed, [](TAutoPtr<IEventHandle>& ev) {
            return ev->GetTypeRewrite() == TEvToDelay::EventType;
        });

        TPortManager portManager;
        THolder<TS3Mock> s3Mock;

        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_DEBUG);
        RestoreNoWait(runtime, txId, portManager.GetPort(), s3Mock, {data});
        const ui64 restoreTxId = txId;

        if (kill) {
            s3Mock.Destroy();
        }

        WaitForDelayed(runtime, delayed, prevObserver);

        runtime.Send(delayed.Release(), 0, true);
        TestCancelTxTable(runtime, ++txId, restoreTxId);
        env.TestWaitNotification(runtime, {restoreTxId, txId});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(CancelUponProposeShouldSucceed) {
        auto data = GenerateTestData(Codec, "a", 1);
        data.YsonStr = EmptyYsonStr;
        CancelShouldSucceed<TEvDataShard::TEvProposeTransaction>(data);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(CancelUponProposeResultShouldSucceed) {
        auto data = GenerateTestData(Codec, "a", 1);
        data.YsonStr = EmptyYsonStr;
        CancelShouldSucceed<TEvDataShard::TEvProposeTransactionResult>(data);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(CancelUponUploadResponseShouldSucceed) {
        const auto data = GenerateTestData(Codec, "a", 1);
        CancelShouldSucceed<TEvDataShard::TEvS3UploadRowsResponse>(data);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(CancelHungOperationShouldSucceed) {
        auto data = GenerateTestData(Codec, "a", 1);
        data.YsonStr = EmptyYsonStr;
        CancelShouldSucceed<TEvDataShard::TEvProposeTransactionResult>(data, true);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(CancelAlmostCompleteOperationShouldNotHaveEffect) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        THolder<IEventHandle> schemaChanged;
        auto prevObserver = SetDelayObserver(runtime, schemaChanged, [](TAutoPtr<IEventHandle>& ev) {
            return ev->GetTypeRewrite() == TEvDataShard::TEvSchemaChanged::EventType;
        });

        TPortManager portManager;
        THolder<TS3Mock> s3Mock;
        const auto data = GenerateTestData(Codec, "a", 1);

        RestoreNoWait(runtime, txId, portManager.GetPort(), s3Mock, {data});
        const ui64 restoreTxId = txId;

        WaitForDelayed(runtime, schemaChanged, prevObserver);

        THolder<IEventHandle> progress;
        prevObserver = SetDelayObserver(runtime, progress, [](TAutoPtr<IEventHandle>& ev) {
            return ev->GetTypeRewrite() == TEvPrivate::TEvProgressOperation::EventType;
        });

        TestCancelTxTable(runtime, ++txId, restoreTxId);
        WaitForDelayed(runtime, progress, prevObserver);

        runtime.Send(schemaChanged.Release(), 0, true);
        runtime.Send(progress.Release(), 0, true);
        env.TestWaitNotification(runtime, {restoreTxId, txId});

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.YsonStr, content);
    }
}

Y_UNIT_TEST_SUITE(TRestoreWithRebootsTests) {
    void Restore(TTestWithReboots& t, TTestActorRuntime& runtime, bool& activeZone,
            ui16 port, const TString& creationScheme, TVector<TTestData>&& data, ui32 readBatchSize = 128) {

        THolder<TS3Mock> s3Mock;
        TString schemeStr;

        {
            TInactiveZone inactive(activeZone);

            TestCreateTable(runtime, ++t.TxId, "/MyRoot", creationScheme);
            t.TestEnv->TestWaitNotification(runtime, t.TxId);

            const auto desc = DescribePath(runtime, "/MyRoot/Table", true, true);
            UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

            s3Mock.Reset(new TS3Mock(ConvertTestData({GenerateScheme(desc), std::move(data)}), TS3Mock::TSettings(port)));
            UNIT_ASSERT(s3Mock->Start());

            runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
            schemeStr = GenerateTableDescription(desc);
        }

        TestRestore(runtime, ++t.TxId, "/MyRoot", Sprintf(R"(
            TableName: "Table"
            TableDescription {
                %s
            }
            S3Settings {
                Endpoint: "localhost:%d"
                Scheme: HTTP
                Limits {
                    ReadBatchSize: %d
                }
            }
        )", schemeStr.data(), port, readBatchSize));
        t.TestEnv->TestWaitNotification(runtime, t.TxId);
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnSingleShardTable) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const auto data = GenerateTestData(Codec, "a", 1);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )", {data});

            {
                TInactiveZone inactive(activeZone);

                auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
                NKqp::CompareYson(data.YsonStr, content);
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnMultiShardTable) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const auto a = GenerateTestData(Codec, "a", 1);
            const auto b = GenerateTestData(Codec, "b", 1);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                SplitBoundary {
                  KeyPrefix {
                    Tuple { Optional { Text: "b" } }
                  }
                }
            )", {a, b});

            {
                TInactiveZone inactive(activeZone);
                {
                    auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
                    NKqp::CompareYson(a.YsonStr, content);
                }
                {
                    auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "Table", {"key"}, {"key", "value"});
                    NKqp::CompareYson(b.YsonStr, content);
                }
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnMultiShardTableAndLimitedResources) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);

                using namespace NResourceBroker;

                auto config = MakeDefaultConfig();
                for (auto& queue : *config.MutableQueues()) {
                    if (queue.GetName() == "queue_restore") {
                        queue.MutableLimit()->SetCpu(1);
                        break;
                    }
                }

                runtime.RegisterService(MakeResourceBrokerID(),
                    runtime.Register(CreateResourceBrokerActor(config, runtime.GetDynamicCounters(0))));
            }

            const auto a = GenerateTestData(Codec, "a", 1);
            const auto b = GenerateTestData(Codec, "b", 1);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                SplitBoundary {
                  KeyPrefix {
                    Tuple { Optional { Text: "b" } }
                  }
                }
            )", {a, b});

            {
                TInactiveZone inactive(activeZone);
                {
                    auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
                    NKqp::CompareYson(a.YsonStr, content);
                }
                {
                    auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "Table", {"key"}, {"key", "value"});
                    NKqp::CompareYson(b.YsonStr, content);
                }
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldSucceedOnLargeData) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const auto data = GenerateTestData(Codec, "", 100);
            UNIT_ASSERT(data.Data.size() > 128);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Uint32" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )", {data});

            {
                TInactiveZone inactive(activeZone);

                auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
                NKqp::CompareYson(data.YsonStr, content);
            }
        });
    }

    Y_UNIT_TEST(ShouldSucceedOnMultipleFrames) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const auto data = GenerateZstdTestData("a", 3, 2);
            const ui32 batchSize = 7; // less than any frame

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )", {data}, batchSize);

            {
                TInactiveZone inactive(activeZone);

                auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
                NKqp::CompareYson(data.YsonStr, content);
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnFileWithoutNewLines) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const TString v = "\"a1\",\"value1\"";
            const auto d = Codec == ECompressionCodec::Zstd ? ZstdCompress(v) : v;
            const auto data = TTestData(d, EmptyYsonStr, Codec);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )", {data});

            {
                TInactiveZone inactive(activeZone);

                auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
                NKqp::CompareYson(data.YsonStr, content);
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnEmptyToken) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const TString v = "\"a1\",\n";
            const auto d = Codec == ECompressionCodec::Zstd ? ZstdCompress(v) : v;
            const auto data = TTestData(d, EmptyYsonStr, Codec);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )", {data});

            {
                TInactiveZone inactive(activeZone);

                auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
                NKqp::CompareYson(data.YsonStr, content);
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnInvalidValue) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const TString v = "\"a1\",\"value1\"\n";
            const auto d = Codec == ECompressionCodec::Zstd ? ZstdCompress(v) : v;
            const auto data = TTestData(d, EmptyYsonStr, Codec);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            )", {data});

            {
                TInactiveZone inactive(activeZone);

                auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
                NKqp::CompareYson(data.YsonStr, content);
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(ShouldFailOnOutboundKey) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            const auto a = GenerateTestData(Codec, "a", 1);
            const auto b = TTestData(a.Data, EmptyYsonStr);

            Restore(t, runtime, activeZone, port, R"(
                Name: "Table"
                Columns { Name: "key" Type: "Utf8" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
                SplitBoundary {
                  KeyPrefix {
                    Tuple { Optional { Text: "b" } }
                  }
                }
            )", {a, b});

            {
                TInactiveZone inactive(activeZone);
                {
                    auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
                    NKqp::CompareYson(a.YsonStr, content);
                }
                {
                    auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "Table", {"key"}, {"key", "value"});
                    NKqp::CompareYson(b.YsonStr, content);
                }
            }
        });
    }

    Y_UNIT_TEST_WITH_COMPRESSION(CancelShouldSucceed) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();
        const auto data = GenerateTestData(Codec, "a", 1);

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            THolder<TS3Mock> s3Mock;
            TString schemeStr;

            {
                TInactiveZone inactive(activeZone);

                TestCreateTable(runtime, ++t.TxId, "/MyRoot", R"(
                    Name: "Table"
                    Columns { Name: "key" Type: "Utf8" }
                    Columns { Name: "value" Type: "Utf8" }
                    KeyColumnNames: ["key"]
                )");
                t.TestEnv->TestWaitNotification(runtime, t.TxId);

                const auto desc = DescribePath(runtime, "/MyRoot/Table", true, true);
                UNIT_ASSERT_VALUES_EQUAL(desc.GetStatus(), NKikimrScheme::StatusSuccess);

                s3Mock.Reset(new TS3Mock(ConvertTestData({GenerateScheme(desc), {data}}), TS3Mock::TSettings(port)));
                UNIT_ASSERT(s3Mock->Start());

                runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
                schemeStr = GenerateTableDescription(desc);
            }

            AsyncRestore(runtime, ++t.TxId, "/MyRoot", Sprintf(R"(
                TableName: "Table"
                TableDescription {
                    %s
                }
                S3Settings {
                    Endpoint: "localhost:%d"
                    Scheme: HTTP
                    Limits {
                        ReadBatchSize: 128
                    }
                }
            )", schemeStr.data(), port));
            const ui64 restoreTxId = t.TxId;

            t.TestEnv->ReliablePropose(runtime, CancelTxRequest(++t.TxId, restoreTxId), {
                NKikimrScheme::StatusAccepted,
                NKikimrScheme::StatusTxIdNotExists
            });
            t.TestEnv->TestWaitNotification(runtime, {restoreTxId, t.TxId});
        });
    }
}

Y_UNIT_TEST_SUITE(TImportTests) {
    void Run(TTestBasicRuntime& runtime, TTestEnv& env,
            THashMap<TString, TString>&& data, const TString& request,
            Ydb::StatusIds::StatusCode expectedStatus = Ydb::StatusIds::SUCCESS,
            const TString& dbName = "/MyRoot", bool serverless = false, const TString& userSID = "", const TString& peerName = "")
    {
        ui64 id = 100;

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(data, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        ui64 schemeshardId = TTestTxConfig::SchemeShard;
        if (dbName != "/MyRoot") {
            TestCreateExtSubDomain(runtime, ++id, "/MyRoot", Sprintf(R"(
                Name: "%s"
            )", TStringBuf(serverless ? "/MyRoot/Shared" : dbName).RNextTok('/').data()));
            env.TestWaitNotification(runtime, id);

            const auto describeResult = DescribePath(runtime, serverless ? "/MyRoot/Shared" : dbName);
            const auto subDomainPathId = describeResult.GetPathId();

            TestAlterExtSubDomain(runtime, ++id, "/MyRoot", Sprintf(R"(
                PlanResolution: 50
                Coordinators: 1
                Mediators: 1
                TimeCastBucketsPerMediator: 2
                ExternalSchemeShard: true
                Name: "%s"
                StoragePools {
                  Name: "name_User_kind_hdd-1"
                  Kind: "common"
                }
                StoragePools {
                  Name: "name_User_kind_hdd-2"
                  Kind: "external"
                }
            )", TStringBuf(serverless ? "/MyRoot/Shared" : dbName).RNextTok('/').data()));
            env.TestWaitNotification(runtime, id);

            if (serverless) {
                const auto attrs = AlterUserAttrs({
                    {"cloud_id", "CLOUD_ID_VAL"},
                    {"folder_id", "FOLDER_ID_VAL"},
                    {"database_id", "DATABASE_ID_VAL"}
                });

                TestCreateExtSubDomain(runtime, ++id, "/MyRoot", Sprintf(R"(
                    Name: "%s"
                    ResourcesDomainKey {
                        SchemeShard: %lu
                        PathId: %lu
                    }
                )", TStringBuf(dbName).RNextTok('/').data(), TTestTxConfig::SchemeShard, subDomainPathId), attrs);
                env.TestWaitNotification(runtime, id);

                TestAlterExtSubDomain(runtime, ++id, "/MyRoot", Sprintf(R"(
                    PlanResolution: 50
                    Coordinators: 1
                    Mediators: 1
                    TimeCastBucketsPerMediator: 2
                    ExternalSchemeShard: true
                    ExternalHive: false
                    Name: "%s"
                    StoragePools {
                      Name: "name_User_kind_hdd-1"
                      Kind: "common"
                    }
                    StoragePools {
                      Name: "name_User_kind_hdd-2"
                      Kind: "external"
                    }
                )", TStringBuf(dbName).RNextTok('/').data()));
                env.TestWaitNotification(runtime, id);
            }

            TestDescribeResult(DescribePath(runtime, dbName), {
                NLs::PathExist,
                NLs::ExtractTenantSchemeshard(&schemeshardId)
            });
        }

        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        auto initialStatus = Ydb::StatusIds::SUCCESS;
        switch (expectedStatus) {
        case Ydb::StatusIds::BAD_REQUEST:
        case Ydb::StatusIds::PRECONDITION_FAILED:
            initialStatus = expectedStatus;
            break;
        default:
            break;
        }

        TestImport(runtime, schemeshardId, ++id, dbName, Sprintf(request.data(), port), userSID, peerName, initialStatus);
        env.TestWaitNotification(runtime, id, schemeshardId);

        if (initialStatus != Ydb::StatusIds::SUCCESS) {
            return;
        }

        TestGetImport(runtime, schemeshardId, id, dbName, expectedStatus);
    }

    void Run(TTestBasicRuntime& runtime, THashMap<TString, TString>&& data, const TString& request,
            Ydb::StatusIds::StatusCode expectedStatus = Ydb::StatusIds::SUCCESS,
            const TString& dbName = "/MyRoot", bool serverless = false, const TString& userSID = "") {

        TTestEnv env(runtime, TTestEnvOptions());
        Run(runtime, env, std::move(data), request, expectedStatus, dbName, serverless, userSID);
    }

    Y_UNIT_TEST(ShouldSucceedOnSingleShardTable) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )");

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.Data[0].YsonStr, content);
    }

    Y_UNIT_TEST(ShouldSucceedOnMultiShardTable) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            partition_at_keys {
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "b" } }
              }
            }
        )", {{"a", 1}, {"b", 1}});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )");

        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(data.Data[0].YsonStr, content);
        }
        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(data.Data[1].YsonStr, content);
        }
    }

    void ShouldSucceedOnIndexedTable(ui32 indexes, const TString& indexType = "global_index {}") {
        TTestBasicRuntime runtime;

        auto scheme = TStringBuilder() << R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )";

        for (ui32 i = 0; i < indexes; ++i) {
            scheme << Sprintf(R"(
                indexes {
                  name: "by_value_%i"
                  index_columns: "value"
                  %s
                }
            )", i + 1, indexType.data());
        }

        const auto data = GenerateTestData(scheme, {{"a", 1}});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )");

        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "Table", {"key"}, {"key", "value"});
            NKqp::CompareYson(data.Data[0].YsonStr, content);
        }

        for (ui32 i = 0; i < indexes; ++i) {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1 + i, "indexImplTable", {"value"}, {"value", "key"}, "'ExcFrom ");
            NKqp::CompareYson(data.Data[0].YsonStr, content);
        }
    }

    Y_UNIT_TEST(ShouldSucceedOnIndexedTable1) {
        ShouldSucceedOnIndexedTable(1);
    }

    Y_UNIT_TEST(ShouldSucceedOnIndexedTable2) {
        ShouldSucceedOnIndexedTable(2);
    }

    Y_UNIT_TEST(ShouldSucceedOnIndexedTable3) {
        ShouldSucceedOnIndexedTable(1, "");
    }

    Y_UNIT_TEST(ShouldSucceedOnManyTables) {
        TTestBasicRuntime runtime;

        const auto a = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        const auto b = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"b", 1}});

        Run(runtime, ConvertTestData({{"/a", a}, {"/b", b}}), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "a"
                destination_path: "/MyRoot/TableA"
              }
              items {
                source_prefix: "b"
                destination_path: "/MyRoot/TableB"
              }
            }
        )");

        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 0, "TableA", {"key"}, {"key", "value"});
            NKqp::CompareYson(a.Data[0].YsonStr, content);
        }
        {
            auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets + 1, "TableB", {"key"}, {"key", "value"});
            NKqp::CompareYson(b.Data[0].YsonStr, content);
        }
    }

    Y_UNIT_TEST(ShouldSucceedWithoutTableProfiles) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions()
            .RunFakeConfigDispatcher(true));

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        Run(runtime, env, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )");

        auto content = ReadTable(runtime, TTestTxConfig::FakeHiveTablets, "Table", {"key"}, {"key", "value"});
        NKqp::CompareYson(data.Data[0].YsonStr, content);
    }

    Y_UNIT_TEST(ShouldWriteBillRecordOnServerlessDb) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 100}});

        TVector<TString> billRecords;
        runtime.SetObserverFunc([&billRecords](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != NMetering::TEvMetering::EvWriteMeteringJson) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            billRecords.push_back(ev->Get<NMetering::TEvMetering::TEvWriteMeteringJson>()->MeteringJson);
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        Run(runtime, env, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/User/Table"
              }
            }
        )", Ydb::StatusIds::SUCCESS, "/MyRoot/User", true);

        if (billRecords.empty()) {
            TDispatchOptions opts;
            opts.FinalEvents.emplace_back([&billRecords](IEventHandle&) -> bool {
                return !billRecords.empty();
            });
            runtime.DispatchEvents(opts);
        }

        const TString expectedBillRecord = R"({"usage":{"start":0,"quantity":50,"finish":0,"unit":"request_unit","type":"delta"},"tags":{},"id":"281474976725758-72075186233409549-2-72075186233409549-4","cloud_id":"CLOUD_ID_VAL","source_wt":0,"source_id":"sless-docapi-ydb-ss","resource_id":"DATABASE_ID_VAL","schema":"ydb.serverless.requests.v1","folder_id":"FOLDER_ID_VAL","version":"1.0.0"})";

        UNIT_ASSERT_VALUES_EQUAL(billRecords.size(), 1);
        MeteringDataEqual(billRecords[0], expectedBillRecord);
    }

    Y_UNIT_TEST(ShouldNotWriteBillRecordOnCommonDb) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 100}});

        TVector<TString> billRecords;
        runtime.SetObserverFunc([&billRecords](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != NMetering::TEvMetering::EvWriteMeteringJson) {
                return TTestActorRuntime::EEventAction::PROCESS;
            }

            billRecords.push_back(ev->Get<NMetering::TEvMetering::TEvWriteMeteringJson>()->MeteringJson);
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        Run(runtime, env, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/User/Table"
              }
            }
        )", Ydb::StatusIds::SUCCESS, "/MyRoot/User");

        UNIT_ASSERT(billRecords.empty());
    }

    void ShouldRestoreSettings(const TString& settings, const TVector<NLs::TCheckFunc>& checks) {
        TTestBasicRuntime runtime;

        const auto empty = TTestData("", EmptyYsonStr);
        const auto data = TTestDataWithScheme(TStringBuilder() << R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "created_at"
              type { optional_type { item { type_id: TIMESTAMP } } }
            }
            columns {
              name: "modified_at"
              type { optional_type { item { type_id: UINT32 } } }
            }
            primary_key: "key"
        )" << settings, {empty, empty});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/User/Table"
              }
            }
        )", Ydb::StatusIds::SUCCESS, "/MyRoot/User");

        ui64 schemeshardId = 0;
        TestDescribeResult(DescribePath(runtime, "/MyRoot/User"), {
            NLs::PathExist,
            NLs::ExtractTenantSchemeshard(&schemeshardId)
        });

        TestDescribeResult(DescribePath(runtime, schemeshardId, "/MyRoot/User/Table", true, true), checks);
    }

    Y_UNIT_TEST(ShouldRestoreTtlSettingsInDateTypeColumnMode) {
        ShouldRestoreSettings(R"(
            ttl_settings {
              date_type_column {
                column_name: "created_at"
                expire_after_seconds: 3600
              }
            }
        )", {
            NLs::HasTtlEnabled("created_at", TDuration::Hours(1)),
        });
    }

    Y_UNIT_TEST(ShouldRestoreTtlSettingsInValueSinceUnixEpochMode) {
        ShouldRestoreSettings(R"(
            ttl_settings {
              value_since_unix_epoch {
                column_name: "modified_at"
                column_unit: UNIT_SECONDS
                expire_after_seconds: 7200
              }
            }
        )", {
            NLs::HasTtlEnabled("modified_at", TDuration::Hours(2), TTTLSettings::UNIT_SECONDS),
        });
    }

    Y_UNIT_TEST(ShouldRestoreStorageSettings) {
        auto check = [](const NKikimrScheme::TEvDescribeSchemeResult& desc) {
            const auto& config = desc.GetPathDescription().GetTable().GetPartitionConfig().GetColumnFamilies(0).GetStorageConfig();

            UNIT_ASSERT_VALUES_EQUAL(config.GetSysLog().GetPreferredPoolKind(), "common");
            UNIT_ASSERT_VALUES_EQUAL(config.GetLog().GetPreferredPoolKind(), "common");
            UNIT_ASSERT_VALUES_EQUAL(config.GetExternal().GetPreferredPoolKind(), "external");
            UNIT_ASSERT(config.HasExternalThreshold());
        };

        ShouldRestoreSettings(R"(
            storage_settings {
              tablet_commit_log0 { media: "common" }
              tablet_commit_log1 { media: "common" }
              external { media: "external" }
              store_external_blobs: ENABLED
            }
        )", {
            check,
        });
    }

    Y_UNIT_TEST(ShouldRestoreColumnFamilies) {
        ShouldRestoreSettings(R"(
            storage_settings {
              tablet_commit_log0 { media: "common" }
              tablet_commit_log1 { media: "common" }
            }
            column_families {
              name: "compressed"
              data { media: "common" }
              compression: COMPRESSION_LZ4
            }
        )", {
            NLs::ColumnFamiliesHas(1, "compressed"),
        });
    }

    Y_UNIT_TEST(ShouldRestoreAttributes) {
        ShouldRestoreSettings(R"(
            attributes {
              key: "key"
              value: "value"
            }
        )", {
            NLs::UserAttrsEqual({{"key", "value"}}),
        });
    }

    Y_UNIT_TEST(ShouldRestoreIncrementalBackupFlag) {
        ShouldRestoreSettings(R"(
            attributes {
              key: "__incremental_backup"
              value: "{}"
            }
        )", {
            NLs::IncrementalBackup(true),
        });
    }

    Y_UNIT_TEST(ShouldRestoreIncrementalBackupFlagNullAsFalse) {
        ShouldRestoreSettings(R"(
            attributes {
              key: "__incremental_backup"
              value: "null"
            }
        )", {
            NLs::IncrementalBackup(false),
        });
    }

    // Skip compaction_policy (not supported)
    // Skip uniform_partitions (has no effect)

    Y_UNIT_TEST(ShouldRestoreSplitPoints) {
        ShouldRestoreSettings(R"(
            partition_at_keys {
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "b" } }
              }
            }
        )", {
            NLs::CheckBoundaries,
        });
    }

    Y_UNIT_TEST(ShouldRestorePartitioningBySize) {
        ShouldRestoreSettings(R"(
            partitioning_settings {
              partitioning_by_size: ENABLED
              partition_size_mb: 1024
            }
        )", {
            NLs::SizeToSplitEqual(1 << 30),
        });
    }

    Y_UNIT_TEST(ShouldRestorePartitioningByLoad) {
        ShouldRestoreSettings(R"(
            partitioning_settings {
              partitioning_by_load: ENABLED
            }
        )", {
            NLs::PartitioningByLoadStatus(true),
        });
    }

    Y_UNIT_TEST(ShouldRestoreMinMaxPartitionsCount) {
        ShouldRestoreSettings(R"(
            partitioning_settings {
              min_partitions_count: 2
              max_partitions_count: 3
            }
        )", {
            NLs::MinPartitionsCountEqual(2),
            NLs::MaxPartitionsCountEqual(3),
        });
    }

    Y_UNIT_TEST(ShouldRestoreKeyBloomFilter) {
        ShouldRestoreSettings(R"(
            key_bloom_filter: ENABLED
        )", {
            NLs::KeyBloomFilterStatus(true),
        });
    }

    Y_UNIT_TEST(ShouldRestorePerAzReadReplicas) {
        NKikimrHive::TFollowerGroup group;
        group.SetFollowerCount(1);
        group.SetRequireAllDataCenters(true);
        group.SetFollowerCountPerDataCenter(true);

        ShouldRestoreSettings(R"(
            read_replicas_settings {
              per_az_read_replicas_count: 1
            }
        )", {
            NLs::FollowerGroups({group}),
        });
    }

    Y_UNIT_TEST(ShouldRestoreAnyAzReadReplicas) {
        NKikimrHive::TFollowerGroup group;
        group.SetFollowerCount(1);
        group.SetRequireAllDataCenters(false);

        ShouldRestoreSettings(R"(
            read_replicas_settings {
              any_az_read_replicas_count: 1
            }
        )", {
            NLs::FollowerGroups({group}),
        });
    }

    void ShouldRestoreIndexTableSettings(const TString& schemeAdditions, auto&& tableDescriptionChecker) {
        TTestBasicRuntime runtime;

        const auto empty = TTestData("", EmptyYsonStr);
        const auto data = TTestDataWithScheme(TStringBuilder() << R"(
                columns {
                    name: "key"
                    type { optional_type { item { type_id: UTF8 } } }
                }
                columns {
                    name: "value"
                    type { optional_type { item { type_id: UINT32 } } }
                }
                primary_key: "key"
            )" << schemeAdditions,
            {empty}
        );

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
                endpoint: "localhost:%d"
                scheme: HTTP
                items {
                    source_prefix: ""
                    destination_path: "/MyRoot/User/Table"
                }
            }
        )", Ydb::StatusIds::SUCCESS, "/MyRoot/User");

        ui64 schemeshardId = 0;
        TestDescribeResult(DescribePath(runtime, "/MyRoot/User"), {
            NLs::PathExist,
            NLs::ExtractTenantSchemeshard(&schemeshardId)
        });

        tableDescriptionChecker(
            DescribePath(runtime, schemeshardId, "/MyRoot/User/Table/ByValue/indexImplTable", true, true, true)
        );
    }

    Y_UNIT_TEST(ShouldRestoreIndexTableSplitPoints) {
        ShouldRestoreIndexTableSettings(R"(
                indexes {
                    name: "ByValue"
                    index_columns: "value"
                    global_index {
                        settings {
                            partition_at_keys {
                                split_points {
                                    type { tuple_type { elements { optional_type { item { type_id: UINT32 } } } } }
                                    value { items { uint32_value: 1 } }
                                }
                            }
                        }
                    }
                }
            )",
            [](const NKikimrScheme::TEvDescribeSchemeResult& tableDescription) {
                TestDescribeResult(
                    tableDescription,
                    {NLs::CheckBoundaries}
                );
            }
        );
    }

    Y_UNIT_TEST(ShouldRestoreIndexTableUniformPartitionsCount) {
        ShouldRestoreIndexTableSettings(R"(
                indexes {
                    name: "ByValue"
                    index_columns: "value"
                    global_index {
                        settings {
                            uniform_partitions: 10
                        }
                    }
                }
            )",
            [](const NKikimrScheme::TEvDescribeSchemeResult& tableDescription) {
                const auto& pathDescription = tableDescription.GetPathDescription();
                UNIT_ASSERT_VALUES_EQUAL_C(
                    pathDescription.TablePartitionsSize(), 10,
                    pathDescription.ShortDebugString()
                );
            }
        );
    }

    Y_UNIT_TEST(ShouldRestoreIndexTablePartitioningSettings) {
        ShouldRestoreIndexTableSettings(R"(
                indexes {
                    name: "ByValue"
                    index_columns: "value"
                    global_index {
                        settings {
                            partitioning_settings {
                                partitioning_by_size: ENABLED
                                partition_size_mb: 1024
                                partitioning_by_load: ENABLED
                                min_partitions_count: 2
                                max_partitions_count: 3
                            }
                        }
                    }
                }
            )",
            [](const NKikimrScheme::TEvDescribeSchemeResult& tableDescription) {
                TestDescribeResult(
                    tableDescription,
                    {
                        NLs::SizeToSplitEqual(1 << 30),
                        NLs::PartitioningByLoadStatus(true),
                        NLs::MinPartitionsCountEqual(2),
                        NLs::MaxPartitionsCountEqual(3)
                    }
                );
            }
        );
    }

    Y_UNIT_TEST(ShouldFailOnInvalidSchema) {
        TTestBasicRuntime runtime;

        Run(runtime, ConvertTestData(GenerateTestData("", {{"a", 1}})), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", Ydb::StatusIds::CANCELLED);
    }

    void ShouldFailOnInvalidCsv(const TString& csv) {
        TTestBasicRuntime runtime;

        const auto data = TTestDataWithScheme(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {TTestData(csv, EmptyYsonStr)});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(ShouldFailOnFileWithoutNewLines) {
        ShouldFailOnInvalidCsv("\"a1\",\"value1\"");
    }

    Y_UNIT_TEST(ShouldFailOnEmptyToken) {
        ShouldFailOnInvalidCsv("\"a1\",\n");
    }

    Y_UNIT_TEST(ShouldFailOnInvalidValue) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UINT64 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(ShouldFailOnOutboundKey) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            partition_at_keys {
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "b" } }
              }
            }
        )", {{"a", 1}, {"a", 1}});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(ShouldFailOnAbsentData) {
        TTestBasicRuntime runtime;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            partition_at_keys {
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "b" } }
              }
            }
        )", {{"a", 1}});

        Run(runtime, ConvertTestData(data), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(ShouldFailOnNonUniqDestinationPaths) {
        TTestBasicRuntime runtime;

        auto unusedTestData = THashMap<TString, TString>();
        Run(runtime, std::move(unusedTestData), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "a"
                destination_path: "/MyRoot/Table"
              }
              items {
                source_prefix: "b"
                destination_path: "/MyRoot/Table"
              }
            }
        )", Ydb::StatusIds::BAD_REQUEST);
    }

    Y_UNIT_TEST(ShouldFailOnInvalidPath) {
        TTestBasicRuntime runtime;

        auto unusedTestData = THashMap<TString, TString>();
        Run(runtime, std::move(unusedTestData), R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "a"
                destination_path: "/InvalidRoot/Table"
              }
            }
        )", Ydb::StatusIds::BAD_REQUEST);
    }

    void CancelShouldSucceed(TDelayFunc delayFunc) {
        TTestBasicRuntime runtime;
        std::vector<std::string> auditLines;
        runtime.AuditLogBackends = std::move(CreateTestAuditLogBackends(auditLines));

        TTestEnv env(runtime, TTestEnvOptions());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            indexes {
              name: "by_value"
              index_columns: "value"
              global_index {}
            }
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        THolder<IEventHandle> delayed;
        auto prevObserver = SetDelayObserver(runtime, delayed, delayFunc);

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        const ui64 importId = txId;

        // Check audit record for import start
        {
            auto line = FindAuditLine(auditLines, "operation=IMPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=IMPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", importId));
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address={none}");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject={none}");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT(!line.contains("start_time"));
            UNIT_ASSERT(!line.contains("end_time"));
        }

        WaitForDelayed(runtime, delayed, prevObserver);

        TestCancelImport(runtime, ++txId, "/MyRoot", importId);
        runtime.Send(delayed.Release(), 0, true);
        env.TestWaitNotification(runtime, importId);

        // Check audit record for import end
        //
        {
            auto line = FindAuditLine(auditLines, "operation=IMPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=IMPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", importId));
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address={none}");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject={none}");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=ERROR");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=CANCELLED");
            UNIT_ASSERT_STRING_CONTAINS(line, "reason=Cancelled");
            UNIT_ASSERT_STRING_CONTAINS(line, "start_time=");
            UNIT_ASSERT_STRING_CONTAINS(line, "end_time=");
        }

        TestGetImport(runtime, importId, "/MyRoot", Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(CancelUponGettingSchemeShouldSucceed) {
        CancelShouldSucceed([](TAutoPtr<IEventHandle>& ev) {
            return ev->GetTypeRewrite() == TEvPrivate::EvImportSchemeReady;
        });
    }

    Y_UNIT_TEST(CancelUponCreatingTableShouldSucceed) {
        CancelShouldSucceed([](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == ESchemeOpCreateIndexedTable;
        });
    }

    Y_UNIT_TEST(CancelUponTransferringShouldSucceed) {
        CancelShouldSucceed([](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == ESchemeOpRestore;
        });
    }

    Y_UNIT_TEST(CancelUponBuildingIndicesShouldSucceed) {
        CancelShouldSucceed([](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == ESchemeOpApplyIndexBuild;
        });
    }

    Y_UNIT_TEST(ShouldCheckQuotas) {
        const TString userSID = "user@builtin";
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().SystemBackupSIDs({userSID}));

        TSchemeLimits lowLimits;
        lowLimits.MaxImports = 0;
        SetSchemeshardSchemaLimits(runtime, lowLimits);

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        const TString request = R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )";

        Run(runtime, env, ConvertTestData(data), request, Ydb::StatusIds::PRECONDITION_FAILED);
        Run(runtime, env, ConvertTestData(data), request, Ydb::StatusIds::SUCCESS, "/MyRoot", false, userSID);
    }

    Y_UNIT_TEST(UidAsIdempotencyKey) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        const auto request = Sprintf(R"(
            OperationParams {
              labels {
                key: "uid"
                value: "foo"
              }
            }
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port);

        // create operation
        TestImport(runtime, ++txId, "/MyRoot", request);
        const ui64 importId = txId;
        // create operation again with same uid
        TestImport(runtime, ++txId, "/MyRoot", request);
        // new operation was not created
        TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
        // check previous operation
        TestGetImport(runtime, importId, "/MyRoot");
        env.TestWaitNotification(runtime, importId);
    }

    Y_UNIT_TEST(ImportStartTime) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        runtime.UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));

        const auto desc = TestGetImport(runtime, txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_PREPARING);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(!entry.HasEndTime());
    }

    Y_UNIT_TEST(CompletedImportEndTime) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        runtime.UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));

        runtime.AdvanceCurrentTime(TDuration::Seconds(30)); // doing import

        env.TestWaitNotification(runtime, txId);

        const auto desc = TestGetImport(runtime, txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());
    }

    Y_UNIT_TEST(CancelledImportEndTime) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        runtime.UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        auto delayFunc = [](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == ESchemeOpRestore;
        };

        THolder<IEventHandle> delayed;
        auto prevObserver = SetDelayObserver(runtime, delayed, delayFunc);

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        const ui64 importId = txId;

        runtime.AdvanceCurrentTime(TDuration::Seconds(30)); // doing import

        WaitForDelayed(runtime, delayed, prevObserver);

        TestCancelImport(runtime, ++txId, "/MyRoot", importId);

        auto desc = TestGetImport(runtime, importId, "/MyRoot");
        auto entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLATION);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(!entry.HasEndTime());

        runtime.Send(delayed.Release(), 0, true);
        env.TestWaitNotification(runtime, importId);

        desc = TestGetImport(runtime, importId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLED);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());
    }

    // Based on CompletedImportEndTime
    Y_UNIT_TEST(AuditCompletedImport) {
        TTestBasicRuntime runtime;
        std::vector<std::string> auditLines;
        runtime.AuditLogBackends = std::move(CreateTestAuditLogBackends(auditLines));

        TTestEnv env(runtime);

        runtime.UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        const auto request = Sprintf(R"(
            OperationParams {
              labels {
                key: "uid"
                value: "foo"
              }
            }
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port);
        TestImport(runtime, ++txId, "/MyRoot", request, /*userSID*/ "user@builtin", /*peerName*/ "127.0.0.1:9876");

        // Check audit record for import start
        {
            auto line = FindAuditLine(auditLines, "operation=IMPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=IMPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", txId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT(!line.contains("start_time"));
            UNIT_ASSERT(!line.contains("end_time"));
        }

        runtime.AdvanceCurrentTime(TDuration::Seconds(30)); // doing import

        env.TestWaitNotification(runtime, txId);

        // Check audit record for import end
        //
        {
            auto line = FindAuditLine(auditLines, "operation=IMPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=IMPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", txId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT_STRING_CONTAINS(line, "start_time=");
            UNIT_ASSERT_STRING_CONTAINS(line, "end_time=");
        }

        const auto desc = TestGetImport(runtime, txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_DONE);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());
    }

    // Based on CancelledImportEndTime
    Y_UNIT_TEST(AuditCancelledImport) {
        TTestBasicRuntime runtime;
        std::vector<std::string> auditLines;
        runtime.AuditLogBackends = std::move(CreateTestAuditLogBackends(auditLines));

        TTestEnv env(runtime);

        runtime.UpdateCurrentTime(TInstant::Now());
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        auto delayFunc = [](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() != TEvSchemeShard::EvModifySchemeTransaction) {
                return false;
            }

            return ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>()->Record
                .GetTransaction(0).GetOperationType() == ESchemeOpRestore;
        };

        THolder<IEventHandle> delayed;
        auto prevObserver = SetDelayObserver(runtime, delayed, delayFunc);

        const auto request = Sprintf(R"(
            OperationParams {
              labels {
                key: "uid"
                value: "foo"
              }
            }
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port);
        TestImport(runtime, ++txId, "/MyRoot", request, /*userSID*/ "user@builtin", /*peerName*/ "127.0.0.1:9876");
        const ui64 importId = txId;

        // Check audit record for import start
        {
            auto line = FindAuditLine(auditLines, "operation=IMPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=IMPORT START");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", importId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=SUCCESS");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=SUCCESS");
            UNIT_ASSERT(!line.contains("reason"));
            UNIT_ASSERT(!line.contains("start_time"));
            UNIT_ASSERT(!line.contains("end_time"));
        }

        runtime.AdvanceCurrentTime(TDuration::Seconds(30)); // doing import

        WaitForDelayed(runtime, delayed, prevObserver);

        TestCancelImport(runtime, ++txId, "/MyRoot", importId);

        auto desc = TestGetImport(runtime, importId, "/MyRoot");
        auto entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLATION);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(!entry.HasEndTime());

        runtime.Send(delayed.Release(), 0, true);
        env.TestWaitNotification(runtime, importId);

        desc = TestGetImport(runtime, importId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLED);
        UNIT_ASSERT(entry.HasStartTime());
        UNIT_ASSERT(entry.HasEndTime());
        UNIT_ASSERT_LT(entry.GetStartTime().seconds(), entry.GetEndTime().seconds());

        // Check audit record for import end
        //
        {
            auto line = FindAuditLine(auditLines, "operation=IMPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, "component=schemeshard");
            UNIT_ASSERT_STRING_CONTAINS(line, "operation=IMPORT END");
            UNIT_ASSERT_STRING_CONTAINS(line, Sprintf("id=%lu", importId));
            UNIT_ASSERT_STRING_CONTAINS(line, "uid=foo");
            UNIT_ASSERT_STRING_CONTAINS(line, "remote_address=127.0.0.1");
            UNIT_ASSERT_STRING_CONTAINS(line, "subject=user@builtin");
            UNIT_ASSERT_STRING_CONTAINS(line, "database=/MyRoot");
            UNIT_ASSERT_STRING_CONTAINS(line, "status=ERROR");
            UNIT_ASSERT_STRING_CONTAINS(line, "detailed_status=CANCELLED");
            UNIT_ASSERT_STRING_CONTAINS(line, "reason=Cancelled");
            UNIT_ASSERT_STRING_CONTAINS(line, "start_time=");
            UNIT_ASSERT_STRING_CONTAINS(line, "end_time=");
        }
    }

    Y_UNIT_TEST(UserSID) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        const TString request = Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port);
        const TString userSID = "user@builtin";
        TestImport(runtime, ++txId, "/MyRoot", request, userSID);

        const auto desc = TestGetImport(runtime, txId, "/MyRoot");
        const auto& entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_PREPARING);
        UNIT_ASSERT_VALUES_EQUAL(entry.GetUserSID(), userSID);
    }

    Y_UNIT_TEST(TablePermissions) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto permissions = R"(
            actions {
              change_owner: "eve"
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.generic.read"
              }
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.generic.write"
              }
            }
            actions {
              grant {
                subject: "bob"
                permission_names: "ydb.generic.read"
              }
            }
        )";

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}}, permissions);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"), {
            NLs::PathExist,
            NLs::HasOwner("eve"),
            NLs::HasRight("+R:alice"),
            NLs::HasRight("+W:alice"),
            NLs::HasRight("+R:bob")
        });
    }

    Y_UNIT_TEST(UnexpectedPermission) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto permissions = R"(
            actions {
              change_owner: "eve"
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.unexpected.permission"
              }
            }
        )";

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}}, permissions);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);

        auto desc = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        auto entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLED);
    }

    Y_UNIT_TEST(CorruptedPermissions) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto permissions = R"(
            corrupted
        )";

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}}, permissions);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);

        auto desc = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::CANCELLED);
        auto entry = desc.GetResponse().GetEntry();
        UNIT_ASSERT_VALUES_EQUAL(entry.GetProgress(), Ydb::Import::ImportProgress::PROGRESS_CANCELLED);
    }

    Y_UNIT_TEST(NoACLOption) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto permissions = R"(
            actions {
              change_owner: "eve"
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.generic.read"
              }
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.generic.write"
              }
            }
            actions {
              grant {
                subject: "bob"
                permission_names: "ydb.generic.read"
              }
            }
        )";

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}}, permissions);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        const TString userSID = "user@builtin";
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
              no_acl: true
            }
        )", port), userSID);
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"), {
            NLs::PathExist,
            NLs::HasOwner(userSID),
            NLs::HasNoRight("+R:alice"),
            NLs::HasNoRight("+W:alice"),
            NLs::HasNoRight("+R:bob")
        });
    }

    Y_UNIT_TEST(ShouldBlockMerge) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            partitioning_settings {
              min_partitions_count: 1
            }
            partition_at_keys {
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "b" } }
              }
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "c" } }
              }
            }
        )", {{"a", 1}, {"b", 1}, {"c", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        // Add delay after creating table
        ui64 createTableTxId;
        auto prevObserver = runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto* msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
                if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateIndexedTable) {
                    createTableTxId = msg->Record.GetTxId();
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TBlockEvents<TEvSchemeShard::TEvNotifyTxCompletionResult> delay(runtime, [&](auto& ev) {
            return ev->Get()->Record.GetTxId() == createTableTxId;
        });

        // Start importing table
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        const ui64 importId = txId;

        // Wait for delay after creating table
        runtime.WaitFor("delay after creating table", [&]{ return delay.size() >= 1; });

        // Merge tablets during the delay should be blocked
        const TVector<TExpectedResult> expectedError = {{ NKikimrScheme::StatusInvalidParameter }};
        TestSplitTable(runtime, ++txId, "/MyRoot/Table", Sprintf(R"(
            SourceTabletId: %lu
            SourceTabletId: %lu
        )", TTestTxConfig::FakeHiveTablets, TTestTxConfig::FakeHiveTablets + 1), expectedError);

        // Finish the delay and continue importing
        delay.Unblock();
        env.TestWaitNotification(runtime, importId);

        // Check import
        TestGetImport(runtime, importId, "/MyRoot");

        // Merge tablets after import
        TestSplitTable(runtime, ++txId, "/MyRoot/Table", Sprintf(R"(
            SourceTabletId: %lu
            SourceTabletId: %lu
        )", TTestTxConfig::FakeHiveTablets, TTestTxConfig::FakeHiveTablets + 1));
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(ShouldBlockSplit) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            partitioning_settings {
              min_partitions_count: 1
            }
            partition_at_keys {
              split_points {
                type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                value { items { text_value: "c" } }
              }
            }
        )", {{"a", 1}, {"c", 1}});

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        // Add delay after creating table
        ui64 createTableTxId;
        auto prevObserver = runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == TEvSchemeShard::EvModifySchemeTransaction) {
                const auto* msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
                if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateIndexedTable) {
                    createTableTxId = msg->Record.GetTxId();
                }
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TBlockEvents<TEvSchemeShard::TEvNotifyTxCompletionResult> delay(runtime, [&](auto& ev) {
            return ev->Get()->Record.GetTxId() == createTableTxId;
        });

        // Start importing table
        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        const ui64 importId = txId;

        // Wait for delay after creating table
        runtime.WaitFor("delay after creating table", [&]{ return delay.size() >= 1; });

        // Split tablet during the delay should be blocked
        const TVector<TExpectedResult> expectedError = {{ NKikimrScheme::StatusInvalidParameter }};
        TestSplitTable(runtime, ++txId, "/MyRoot/Table", Sprintf(R"(
            SourceTabletId: %lu
            SplitBoundary {
                KeyPrefix {
                    Tuple { Optional { Text: "b" } }
                }
            }
        )", TTestTxConfig::FakeHiveTablets), expectedError);

        // Finish the delay and continue importing
        delay.Unblock();
        env.TestWaitNotification(runtime, importId);

        // Check import
        TestGetImport(runtime, importId, "/MyRoot");

        // Split table after import
        TestSplitTable(runtime, ++txId, "/MyRoot/Table", Sprintf(R"(
            SourceTabletId: %lu
            SplitBoundary {
                KeyPrefix {
                    Tuple { Optional { Text: "b" } }
                }
            }
        )", TTestTxConfig::FakeHiveTablets));
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(ViewCreationRetry) {
        TTestBasicRuntime runtime;
        auto options = TTestEnvOptions()
            .RunFakeConfigDispatcher(true)
            .SetupKqpProxy(true);
        TTestEnv env(runtime, options);
        runtime.GetAppData().FeatureFlags.SetEnableViews(true);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        ui64 txId = 100;

        THashMap<TString, TTestDataWithScheme> bucketContent(2);
        bucketContent.emplace("/table", GenerateTestData(R"(
            columns {
                name: "key"
                type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
                name: "value"
                type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )"));
        bucketContent.emplace("/view", GenerateTestData(
            {
                EPathTypeView,
                R"(
                    -- backup root: "/MyRoot"
                    CREATE VIEW IF NOT EXISTS `view` WITH security_invoker = TRUE AS SELECT * FROM `table`;
                )"
            }
        ));

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(bucketContent), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        ui64 tableCreationTxId = 0;
        TActorId schemeshardActorId;
        TBlockEvents<TEvSchemeShard::TEvModifySchemeTransaction> tableCreationBlocker(runtime,
            [&](const TEvSchemeShard::TEvModifySchemeTransaction::TPtr& event) {
                const auto& record = event->Get()->Record;
                if (record.GetTransaction(0).GetOperationType() == ESchemeOpCreateIndexedTable) {
                    tableCreationTxId = record.GetTxId();
                    schemeshardActorId = event->Recipient;
                    return true;
                }
                return false;
            }
        );

        TBlockEvents<TEvPrivate::TEvImportSchemeQueryResult> queryResultBlocker(runtime,
            [&](const TEvPrivate::TEvImportSchemeQueryResult::TPtr& event) {
                // The test expects the SchemeShard actor ID to be already initialized when we receive the first query result message.
                // This expectation is valid because we import items in order of their appearance on the import items list.
                if (!schemeshardActorId || event->Recipient != schemeshardActorId) {
                    return false;
                }
                UNIT_ASSERT_VALUES_EQUAL(event->Get()->Status, Ydb::StatusIds::SCHEME_ERROR);
                const auto* error = std::get_if<TString>(&event->Get()->Result);
                UNIT_ASSERT(error);
                UNIT_ASSERT_STRING_CONTAINS(*error, "Cannot find table");
                return true;
            }
        );

        const ui64 importId = ++txId;
        TestImport(runtime, importId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "table"
                destination_path: "/MyRoot/table"
              }
              items {
                source_prefix: "view"
                destination_path: "/MyRoot/view"
              }
            }
        )", port));

        runtime.WaitFor("table creation attempt", [&]{ return !tableCreationBlocker.empty(); });
        runtime.WaitFor("query result", [&]{ return !queryResultBlocker.empty(); });
        tableCreationBlocker.Unblock().Stop();
        queryResultBlocker.Unblock().Stop();
        env.TestWaitNotification(runtime, tableCreationTxId);

        env.TestWaitNotification(runtime, importId);
        TestGetImport(runtime, importId, "/MyRoot");

        TestDescribeResult(DescribePath(runtime, "/MyRoot/view"), {
            NLs::Finished,
            NLs::IsView
        });
    }

    Y_UNIT_TEST(MultipleViewCreationRetries) {
        TTestBasicRuntime runtime;
        auto options = TTestEnvOptions()
            .RunFakeConfigDispatcher(true)
            .SetupKqpProxy(true);
        TTestEnv env(runtime, options);
        runtime.GetAppData().FeatureFlags.SetEnableViews(true);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        ui64 txId = 100;

        constexpr int ViewLayers = 10;
        THashMap<TString, TTestDataWithScheme> bucketContent(ViewLayers);
        for (int i = 0; i < ViewLayers; ++i) {
            bucketContent.emplace(std::format("/view{}", i), GenerateTestData(
                {
                    EPathTypeView,
                    std::format(R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `view` WITH security_invoker = TRUE AS {};
                        )", i == 0
                            ? "SELECT 1"
                            : std::format("SELECT * FROM `view{}`", i - 1)
                    )
                }
            ));
        }

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(bucketContent), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TActorId schemeshardActorId;
        TBlockEvents<TEvSchemeShard::TEvModifySchemeTransaction> viewCreationBlocker(runtime,
            [&](const TEvSchemeShard::TEvModifySchemeTransaction::TPtr& event) {
                const auto& record = event->Get()->Record;
                if (record.GetTransaction(0).GetOperationType() == ESchemeOpCreateView) {
                    schemeshardActorId = event->Recipient;
                    return true;
                }
                return false;
            }
        );

        int missingDependencyFails = 0;
        auto missingDependencyObserver = runtime.AddObserver<TEvPrivate::TEvImportSchemeQueryResult>(
            [&](const TEvPrivate::TEvImportSchemeQueryResult::TPtr& event) {
                if (!schemeshardActorId
                    || event->Recipient != schemeshardActorId
                    || event->Get()->Status != Ydb::StatusIds::SCHEME_ERROR) {
                    return;
                }
                const auto* error = std::get_if<TString>(&event->Get()->Result);
                if (error && error->Contains("Cannot find table")) {
                    ++missingDependencyFails;
                }
            }
        );

        auto importSettings = TStringBuilder() << std::format(R"(
                ImportFromS3Settings {{
                    endpoint: "localhost:{}"
                    scheme: HTTP
            )", port
        );
        for (int i = 0; i < ViewLayers; ++i) {
            importSettings << std::format(R"(
                    items {{
                        source_prefix: "view{}"
                        destination_path: "/MyRoot/view{}"
                    }}
                )", i, i
            );
        }
        importSettings << '}';

        const ui64 importId = ++txId;
        TestImport(runtime, importId, "/MyRoot", importSettings);

        int expectedFails = 0;
        for (int iteration = 1; iteration <= ViewLayers; ++iteration) {
            runtime.WaitFor("blocked view creation", [&]{ return !viewCreationBlocker.empty(); });

            expectedFails += ViewLayers - iteration;
            if (iteration > 1) {
                runtime.WaitFor("query results", [&]{ return missingDependencyFails >= expectedFails; });
            } else {
                // the first iteration might miss some query results due to the initially unset schemeshardActorId
                missingDependencyFails = expectedFails;
            }

            viewCreationBlocker.Unblock(1);
        }
        UNIT_ASSERT(viewCreationBlocker.empty());
        viewCreationBlocker.Stop();

        env.TestWaitNotification(runtime, importId);
        TestGetImport(runtime, importId, "/MyRoot");

        for (int i = 0; i < ViewLayers; ++i) {
            TestDescribeResult(DescribePath(runtime, std::format("/MyRoot/view{}", i)), {
                NLs::Finished,
                NLs::IsView
            });
        }
    }

    struct TGeneratedChangefeed {
        std::pair<TString, TTestDataWithScheme> Changefeed;
        std::function<void(TTestBasicRuntime&)> Checker;
    };

    TGeneratedChangefeed GenChangefeed(ui64 num = 1) {
        const TString changefeedName = TStringBuilder() << "updates_feed" << num;
        const auto changefeedPath = TStringBuilder() << "/" << changefeedName;

        const auto changefeedDesc = Sprintf(R"(
            name: "%s"
            mode: MODE_UPDATES
            format: FORMAT_JSON
            state: STATE_ENABLED
        )", changefeedName.c_str());

        const auto topicDesc = R"(
            partitioning_settings {
                min_active_partitions: 1
                max_active_partitions: 1
                auto_partitioning_settings {
                    strategy: AUTO_PARTITIONING_STRATEGY_DISABLED
                    partition_write_speed {
                        stabilization_window {
                            seconds: 300
                        }
                        up_utilization_percent: 80
                        down_utilization_percent: 20
                    }
                }
            }
            partitions {
                active: true
            }
            retention_period {
                seconds: 86400
            }
            partition_write_speed_bytes_per_second: 1048576
            partition_write_burst_bytes: 1048576
            attributes {
                key: "__max_partition_message_groups_seqno_stored"
                value: "6000000"
            }
            attributes {
                key: "_allow_unauthenticated_read"
                value: "true"
            }
            attributes {
                key: "_allow_unauthenticated_write"
                value: "true"
            }
            attributes {
                key: "_message_group_seqno_retention_period_ms"
                value: "1382400000"
            }
            consumers {
                name: "my_consumer"
                read_from {
                }
                attributes {
                    key: "_service_type"
                    value: "data-streams"
                }
            }
        )";

        NAttr::TAttributes attr;
        attr.emplace(NAttr::EKeys::TOPIC_DESCRIPTION, topicDesc);
        return {
            {changefeedPath, GenerateTestData({EPathTypeCdcStream, changefeedDesc, std::move(attr)})},
            [changefeedPath = TString(changefeedPath)](TTestBasicRuntime& runtime) {
                TestDescribeResult(DescribePath(runtime, "/MyRoot/Table" + changefeedPath, false, false, true), {
                    NLs::PathExist,
                });
                TestDescribeResult(DescribePath(runtime, "/MyRoot/Table" + changefeedPath + "/streamImpl", false, false, true), {
                    NLs::ConsumerExist("my_consumer")
                });
            }
        };
    }

    TVector<std::function<void(TTestBasicRuntime&)>> GenChangefeeds(THashMap<TString, TTestDataWithScheme>& bucketContent, ui64 count = 1) {
        TVector<std::function<void(TTestBasicRuntime&)>> checkers;
        checkers.reserve(count);
        for (ui64 i = 1; i <= count; ++i) {
            auto genChangefeed = GenChangefeed(i);
            bucketContent.emplace(genChangefeed.Changefeed);
            checkers.push_back(genChangefeed.Checker);
        }
        return checkers;
    }

    std::function<void(TTestBasicRuntime&)> AddedSchemeCommon(THashMap<TString, TTestDataWithScheme>& bucketContent, const TString& permissions) {
        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )", {{"a", 1}}, permissions);

        bucketContent.emplace("", data);
        return [](TTestBasicRuntime& runtime) {
            TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"), {
                NLs::PathExist
            });
        };
    }

    std::function<void(TTestBasicRuntime&)> AddedScheme(THashMap<TString, TTestDataWithScheme>& bucketContent) {
        return AddedSchemeCommon(bucketContent, "");
    }

    std::function<void(TTestBasicRuntime&)> AddedSchemeWithPermissions(THashMap<TString, TTestDataWithScheme>& bucketContent) {
        const auto permissions = R"(
            actions {
              change_owner: "eve"
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.generic.read"
              }
            }
            actions {
              grant {
                subject: "alice"
                permission_names: "ydb.generic.write"
              }
            }
            actions {
              grant {
                subject: "bob"
                permission_names: "ydb.generic.read"
              }
            }
        )";
        return AddedSchemeCommon(bucketContent, permissions);
    }

    using SchemeFunction = std::function<std::function<void(TTestBasicRuntime&)>(THashMap<TString, TTestDataWithScheme>&)>;

    void TestImportChangefeeds(ui64 countChangefeed, SchemeFunction addedScheme) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;
        runtime.GetAppData().FeatureFlags.SetEnableChangefeedsImport(true);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        const auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )");

        THashMap<TString, TTestDataWithScheme> bucketContent(countChangefeed + 1);

        auto checkerTable = addedScheme(bucketContent);
        auto checkersChangefeeds = GenChangefeeds(bucketContent, countChangefeed);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(bucketContent), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);

        checkerTable(runtime);
        for (const auto& checker : checkersChangefeeds) {
            checker(runtime);
        }
    }

    Y_UNIT_TEST(Changefeed) {
        TestImportChangefeeds(1, AddedScheme);
    }

    Y_UNIT_TEST(Changefeeds) {
        TestImportChangefeeds(3, AddedScheme);
    }

    Y_UNIT_TEST(ChangefeedWithTablePermissions) {
        TestImportChangefeeds(1, AddedSchemeWithPermissions);
    }

    Y_UNIT_TEST(ChangefeedsWithTablePermissions) {
        TestImportChangefeeds(3, AddedSchemeWithPermissions);
    }

    void TestCreateCdcStreams(TTestEnv& env, TTestActorRuntime& runtime, ui64& txId, const TString& dbName, ui64 count, bool isShouldSuccess) {
        for (ui64 i = 1; i <= count; ++i) {
            TestCreateCdcStream(runtime, ++txId, dbName, Sprintf(R"(
                TableName: "Original"
                StreamDescription {
                  Name: "Stream_%d"
                  Mode: ECdcStreamModeKeysOnly
                  Format: %s
                }
            )", i, isShouldSuccess ? "ECdcStreamFormatJson" : "ECdcStreamFormatProto"));
            env.TestWaitNotification(runtime, txId);
        }
    }

    void ChangefeedsExportRestore(bool isShouldSuccess) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
        runtime.GetAppData().FeatureFlags.SetEnableChangefeedsExport(true);
        runtime.GetAppData().FeatureFlags.SetEnableChangefeedsImport(true);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Original"
            Columns { Name: "key" Type: "Uint32" }
            Columns { Name: "double_value" Type: "Double" }
            Columns { Name: "float_value" Type: "Float" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateCdcStreams(env, runtime, txId, "/MyRoot", 3, isShouldSuccess);

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Original"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot", isShouldSuccess ? Ydb::StatusIds::SUCCESS : Ydb::StatusIds::CANCELLED);
    }

    Y_UNIT_TEST(ChangefeedsExportRestore) {
        ChangefeedsExportRestore(true);
    }

    Y_UNIT_TEST(ChangefeedsExportRestoreUnhappyPropose) {
        ChangefeedsExportRestore(false);
    }

    Y_UNIT_TEST(IgnoreBasicSchemeLimits) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableRealSystemViewPaths(false));
        ui64 txId = 100;

        TestCreateExtSubDomain(runtime, ++txId,  "/MyRoot", R"(
            Name: "Alice"
        )");
        TestAlterExtSubDomain(runtime, ++txId,  "/MyRoot", R"(
            Name: "Alice"
            ExternalSchemeShard: true
            PlanResolution: 50
            Coordinators: 1
            Mediators: 1
            TimeCastBucketsPerMediator: 2
            StoragePools {
                Name: "Alice:hdd"
                Kind: "hdd"
            }
        )");
        env.TestWaitNotification(runtime, txId);

        ui64 tenantSchemeShard = 0;
        TestDescribeResult(DescribePath(runtime, "/MyRoot/Alice"), {
            NLs::ExtractTenantSchemeshard(&tenantSchemeShard)
        });
        UNIT_ASSERT_UNEQUAL(tenantSchemeShard, 0);

        TSchemeLimits basicLimits;
        basicLimits.MaxShards = 4;
        basicLimits.MaxShardsInPath = 2;
        SetSchemeshardSchemaLimits(runtime, basicLimits, tenantSchemeShard);

        TestDescribeResult(DescribePath(runtime, tenantSchemeShard, "/MyRoot/Alice"), {
            NLs::DomainLimitsIs(basicLimits.MaxPaths, basicLimits.MaxShards),
            NLs::PathsInsideDomain(0),
            NLs::ShardsInsideDomain(3)
        });

        TestCreateTable(runtime, tenantSchemeShard, ++txId, "/MyRoot/Alice", R"(
            Name: "table1"
            Columns { Name: "Key" Type: "Uint64" }
            Columns { Name: "Value" Type: "Utf8" }
            KeyColumnNames: ["Key"]
        )");
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        TestCreateTable(runtime, tenantSchemeShard, ++txId, "/MyRoot/Alice", R"(
                Name: "table2"
                Columns { Name: "Key" Type: "Uint64" }
                Columns { Name: "Value" Type: "Utf8" }
                KeyColumnNames: ["Key"]
            )",
            { NKikimrScheme::StatusResourceExhausted }
        );

        const auto data = GenerateTestData(R"(
                columns {
                    name: "key"
                    type { optional_type { item { type_id: UTF8 } } }
                }
                columns {
                    name: "value"
                    type { optional_type { item { type_id: UTF8 } } }
                }
                primary_key: "key"
                partition_at_keys {
                    split_points {
                        type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                        value { items { text_value: "b" } }
                    }
                }
                indexes {
                    name: "ByValue"
                    index_columns: "value"
                    global_index {}
                }
            )",
            {{"a", 1}, {"b", 1}}
        );

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        const auto importId = ++txId;
        TestImport(runtime, tenantSchemeShard, importId, "/MyRoot/Alice", Sprintf(R"(
                ImportFromS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_prefix: ""
                        destination_path: "/MyRoot/Alice/ImportDir/Table"
                    }
                }
            )",
            port
        ));
        env.TestWaitNotification(runtime, importId, tenantSchemeShard);
        TestGetImport(runtime, tenantSchemeShard, importId, "/MyRoot/Alice");

        TestDescribeResult(DescribePath(runtime, tenantSchemeShard, "/MyRoot/Alice"), {
            NLs::DomainLimitsIs(basicLimits.MaxPaths, basicLimits.MaxShards),
            NLs::PathsInsideDomain(5),
            NLs::ShardsInsideDomain(7)
        });
    }

    Y_UNIT_TEST(TopicImport) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        const auto data = GenerateTestData(
            {
                EPathTypePersQueueGroup,
                R"(partitioning_settings {
  min_active_partitions: 1
  max_active_partitions: 1
  auto_partitioning_settings {
    strategy: AUTO_PARTITIONING_STRATEGY_DISABLED
    partition_write_speed {
      stabilization_window {
        seconds: 300
      }
      up_utilization_percent: 80
      down_utilization_percent: 20
    }
  }
}
retention_period {
  seconds: 64800
}
supported_codecs {
}
partition_write_speed_bytes_per_second: 1048576
partition_write_burst_bytes: 1048576
consumers {
  name: "consumer1"
  read_from {
  }
  attributes {
    key: "_service_type"
    value: "data-streams"
  }
}
consumers {
  name: "consumer2"
  read_from {
  }
  attributes {
    key: "_service_type"
    value: "data-streams"
  }
}
)"});

        THashMap<TString, TTestDataWithScheme> bucketContent;

        bucketContent.emplace("/Topic", data);

        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock(ConvertTestData(bucketContent), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: "/Topic"
                destination_path: "/MyRoot/Restored/Topic"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Restored/Topic"), {
            NLs::PathExist,
        });
    }

    Y_UNIT_TEST(TopicExportImport) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TS3Mock s3Mock({}, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::DATASHARD_BACKUP, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::EXPORT, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);

        TestCreatePQGroup(runtime, ++txId, "/MyRoot", R"(
              Name: "Topic"
              TotalGroupCount: 2
              PartitionPerTablet: 1
              PQTabletConfig {
                  PartitionConfig {
                      LifetimeSeconds: 10
                  }
              }
          )");
        env.TestWaitNotification(runtime, txId);

        TestExport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ExportToS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_path: "/MyRoot/Topic"
                destination_prefix: ""
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetExport(runtime, txId, "/MyRoot");

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Restored"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);
        TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::SUCCESS);
    }

    Y_UNIT_TEST(UnknownSchemeObjectImport) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TS3Mock s3Mock({
            {"/unknown_key", "unknown_scheme_object"},
            {"/metadata.json", R"({"version": 0})"}
        }, TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TestImport(runtime, ++txId, "/MyRoot", Sprintf(R"(
            ImportFromS3Settings {
              endpoint: "localhost:%d"
              scheme: HTTP
              items {
                source_prefix: ""
                destination_path: "/MyRoot/Unknown"
              }
            }
        )", port));
        env.TestWaitNotification(runtime, txId);

        auto issues = TestGetImport(runtime, txId, "/MyRoot", Ydb::StatusIds::CANCELLED)
                        .GetResponse().GetEntry().GetIssues();
        UNIT_ASSERT(!issues.empty());
        UNIT_ASSERT_EQUAL(issues.begin()->message(), "Unsupported scheme object type");
    }
}

Y_UNIT_TEST_SUITE(TImportWithRebootsTests) {

    constexpr TStringBuf DefaultImportSettings = R"(
        ImportFromS3Settings {
            endpoint: "localhost:%d"
            scheme: HTTP
            items {
                source_prefix: ""
                destination_path: "/MyRoot/Table"
            }
        }
    )";

    void ShouldSucceed(const THashMap<TString, TTypedScheme>& schemes, TStringBuf importSettings = DefaultImportSettings) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        THashMap<TString, TTestDataWithScheme> bucketContent(schemes.size());
        for (const auto& [prefix, typedScheme] : schemes) {
            bucketContent.emplace(prefix, GenerateTestData(typedScheme));
        }
        TS3Mock s3Mock(ConvertTestData(bucketContent), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestWithReboots t;
        const bool createsViews = AnyOf(schemes, [](const auto& scheme) {
            return scheme.second.Type == EPathTypeView;
        });
        if (createsViews) {
            t.GetTestEnvOptions().RunFakeConfigDispatcher(true);
            t.GetTestEnvOptions().SetupKqpProxy(true);
        }
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);

                runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
                runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
                runtime.GetAppData().FeatureFlags.SetEnableChangefeedsImport(true);
                if (createsViews) {
                    runtime.GetAppData().FeatureFlags.SetEnableViews(true);
                }
            }

            const ui64 importId = ++t.TxId;
            AsyncImport(runtime, importId, "/MyRoot", Sprintf(importSettings.data(), port));
            t.TestEnv->TestWaitNotification(runtime, importId);

            {
                TInactiveZone inactive(activeZone);
                TestGetImport(runtime, importId, "/MyRoot", {
                    Ydb::StatusIds::SUCCESS,
                    Ydb::StatusIds::NOT_FOUND
                });
            }
        });
    }

    void ShouldSucceed(const TTypedScheme& scheme) {
        ShouldSucceed({{"", scheme}});
    }

    Y_UNIT_TEST(ShouldSucceedOnSimpleTable) {
        ShouldSucceed(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )");
    }

    Y_UNIT_TEST(ShouldSucceedOnTableWithChecksum) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        auto data = GenerateTestData(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )" , {{"a", 1}}, "", R"({"version": 1})");

        TS3Mock s3Mock(ConvertTestData(data), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestWithReboots t;
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);

                runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
                runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
            }

            const ui64 importId = ++t.TxId;
            AsyncImport(runtime, importId, "/MyRoot", Sprintf(DefaultImportSettings.data(), port));
            t.TestEnv->TestWaitNotification(runtime, importId);

            {
                TInactiveZone inactive(activeZone);
                TestGetImport(runtime, importId, "/MyRoot", {
                    Ydb::StatusIds::SUCCESS,
                    Ydb::StatusIds::NOT_FOUND
                });
            }
        });
    }

    Y_UNIT_TEST(ShouldSucceedOnIndexedTable) {
        ShouldSucceed(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            indexes {
              name: "by_value"
              index_columns: "value"
              global_index {}
            }
        )");
    }

    Y_UNIT_TEST(ShouldSucceedOnSingleView) {
        ShouldSucceed(
            {
                EPathTypeView,
                R"(
                    -- backup root: "/MyRoot"
                    CREATE VIEW IF NOT EXISTS `view` WITH security_invoker = TRUE AS SELECT 1;
                )"
            }
        );
    }

    Y_UNIT_TEST(ShouldSucceedOnViewsAndTables) {
        ShouldSucceed(
            {
                {
                    "/view",
                    {
                        EPathTypeView,
                        R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `view` WITH security_invoker = TRUE AS SELECT 1;
                        )"
                    }
                }, {
                    "/table",
                    {
                        EPathTypeTable,
                        R"(
                            columns {
                                name: "key"
                                type { optional_type { item { type_id: UTF8 } } }
                            }
                            columns {
                                name: "value"
                                type { optional_type { item { type_id: UTF8 } } }
                            }
                            primary_key: "key"
                        )"
                    }
                }
            }, R"(
                ImportFromS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_prefix: "view"
                        destination_path: "/MyRoot/View"
                    }
                    items {
                        source_prefix: "table"
                        destination_path: "/MyRoot/Table"
                    }
                }
            )"
        );
    }

    Y_UNIT_TEST(ShouldSucceedOnDependentView) {
        ShouldSucceed(
            {
                {
                    "/DependentView",
                    {
                        EPathTypeView,
                        R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `DependentView` WITH security_invoker = TRUE AS SELECT * FROM `BaseView`;
                        )"
                    }
                }, {
                    "/BaseView",
                    {
                        EPathTypeView,
                        R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `BaseView` WITH security_invoker = TRUE AS SELECT 1;
                        )"
                    }
                }
            }, R"(
                ImportFromS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_prefix: "DependentView"
                        destination_path: "/MyRoot/DependentView"
                    }
                    items {
                        source_prefix: "BaseView"
                        destination_path: "/MyRoot/BaseView"
                    }
                }
            )"
        );
    }

    void CancelShouldSucceed(const THashMap<TString, TTypedScheme>& schemes, TStringBuf importSettings = DefaultImportSettings) {
        TPortManager portManager;
        const ui16 port = portManager.GetPort();

        THashMap<TString, TTestDataWithScheme> bucketContent(schemes.size());
        for (const auto& [prefix, typedScheme] : schemes) {
            bucketContent.emplace(prefix, GenerateTestData(typedScheme));
        }
        TS3Mock s3Mock(ConvertTestData(bucketContent), TS3Mock::TSettings(port));
        UNIT_ASSERT(s3Mock.Start());

        TTestWithReboots t;
        const bool createsViews = AnyOf(schemes, [](const auto& scheme) {
            return scheme.second.Type == EPathTypeView;
        });
        if (createsViews) {
            t.GetTestEnvOptions().RunFakeConfigDispatcher(true);
            t.GetTestEnvOptions().SetupKqpProxy(true);
        }
        t.Run([&](TTestActorRuntime& runtime, bool& activeZone) {
            {
                TInactiveZone inactive(activeZone);

                runtime.SetLogPriority(NKikimrServices::DATASHARD_RESTORE, NActors::NLog::PRI_TRACE);
                runtime.SetLogPriority(NKikimrServices::IMPORT, NActors::NLog::PRI_TRACE);
                if (createsViews) {
                    runtime.GetAppData().FeatureFlags.SetEnableViews(true);
                }
            }

            const ui64 importId = ++t.TxId;
            AsyncImport(runtime, importId, "/MyRoot", Sprintf(importSettings.data(), port));

            t.TestEnv->ReliablePropose(runtime, CancelImportRequest(++t.TxId, "/MyRoot", importId), {
                Ydb::StatusIds::SUCCESS,
                Ydb::StatusIds::NOT_FOUND
            });
            t.TestEnv->TestWaitNotification(runtime, importId);

            {
                TInactiveZone inactive(activeZone);
                const auto response = TestGetImport(runtime, importId, "/MyRoot", {
                    Ydb::StatusIds::SUCCESS,
                    Ydb::StatusIds::CANCELLED,
                    Ydb::StatusIds::NOT_FOUND
                });
                const auto& entry = response.GetResponse().GetEntry();
                if (entry.GetStatus() == Ydb::StatusIds::CANCELLED) {
                    UNIT_ASSERT_STRING_CONTAINS(NYql::IssuesFromMessageAsString(entry.GetIssues()), "Cancelled manually");
                }
            }
        });
    }

    void CancelShouldSucceed(const TTypedScheme& scheme) {
        CancelShouldSucceed({{"", scheme}});
    }

    Y_UNIT_TEST(CancelShouldSucceedOnSimpleTable) {
        CancelShouldSucceed(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )");
    }

    Y_UNIT_TEST(CancelShouldSucceedOnIndexedTable) {
        CancelShouldSucceed(R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
            indexes {
              name: "by_value"
              index_columns: "value"
              global_index {}
            }
        )");
    }

    Y_UNIT_TEST(CancelShouldSucceedOnSingleView) {
        CancelShouldSucceed(
            {
                EPathTypeView,
                R"(
                    -- backup root: "/MyRoot"
                    CREATE VIEW IF NOT EXISTS `view` WITH security_invoker = TRUE AS SELECT 1;
                )"
            }
        );
    }

    Y_UNIT_TEST(CancelShouldSucceedOnViewsAndTables) {
        CancelShouldSucceed(
            {
                {
                    "/view",
                    {
                        EPathTypeView,
                        R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `view` WITH security_invoker = TRUE AS SELECT 1;
                        )"
                    }
                }, {
                    "/table",
                    {
                        EPathTypeTable,
                        R"(
                            columns {
                                name: "key"
                                type { optional_type { item { type_id: UTF8 } } }
                            }
                            columns {
                                name: "value"
                                type { optional_type { item { type_id: UTF8 } } }
                            }
                            primary_key: "key"
                        )"
                    }
                }
            }, R"(
                ImportFromS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_prefix: "view"
                        destination_path: "/MyRoot/View"
                    }
                    items {
                        source_prefix: "table"
                        destination_path: "/MyRoot/Table"
                    }
                }
            )"
        );
    }

    THashMap<TString, TTypedScheme> GetSchemeWithChangefeed() {
        THashMap<TString, TTypedScheme> schemes;

        const auto changefeedName = "update_changefeed";

        schemes.emplace("", R"(
            columns {
              name: "key"
              type { optional_type { item { type_id: UTF8 } } }
            }
            columns {
              name: "value"
              type { optional_type { item { type_id: UTF8 } } }
            }
            primary_key: "key"
        )");

        const auto changefeedDesc = Sprintf(R"(
            name: "%s"
            mode: MODE_UPDATES
            format: FORMAT_JSON
            state: STATE_ENABLED
        )", changefeedName);

        const auto topicDesc = R"(
            partitioning_settings {
                min_active_partitions: 1
                max_active_partitions: 1
                auto_partitioning_settings {
                    strategy: AUTO_PARTITIONING_STRATEGY_DISABLED
                    partition_write_speed {
                        stabilization_window {
                            seconds: 300
                        }
                        up_utilization_percent: 80
                        down_utilization_percent: 20
                    }
                }
            }
            partitions {
                active: true
            }
            retention_period {
                seconds: 86400
            }
            partition_write_speed_bytes_per_second: 1048576
            partition_write_burst_bytes: 1048576
            attributes {
                key: "__max_partition_message_groups_seqno_stored"
                value: "6000000"
            }
            attributes {
                key: "_allow_unauthenticated_read"
                value: "true"
            }
            attributes {
                key: "_allow_unauthenticated_write"
                value: "true"
            }
            attributes {
                key: "_message_group_seqno_retention_period_ms"
                value: "1382400000"
            }
            consumers {
                name: "my_consumer"
                read_from {
                }
                attributes {
                    key: "_service_type"
                    value: "data-streams"
                }
            }
        )";

        NAttr::TAttributes attr;
        attr.emplace(NAttr::EKeys::TOPIC_DESCRIPTION, topicDesc);

        schemes.emplace("/update_feed",
            TTypedScheme {
                EPathTypeCdcStream,
                changefeedDesc,
                std::move(attr)
            }
        );
        return schemes;
    }

    Y_UNIT_TEST(ShouldSucceedOnSingleChangefeed) {
        ShouldSucceed(GetSchemeWithChangefeed());
    }

    Y_UNIT_TEST(CancelShouldSucceedOnSingleChangefeed) {
        CancelShouldSucceed(GetSchemeWithChangefeed());
    }

    Y_UNIT_TEST(CancelShouldSucceedOnDependentView) {
        CancelShouldSucceed(
            {
                {
                    "/DependentView",
                    {
                        EPathTypeView,
                        R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `DependentView` WITH security_invoker = TRUE AS SELECT * FROM `BaseView`;
                        )"
                    }
                }, {
                    "/BaseView",
                    {
                        EPathTypeView,
                        R"(
                            -- backup root: "/MyRoot"
                            CREATE VIEW IF NOT EXISTS `BaseView` WITH security_invoker = TRUE AS SELECT 1;
                        )"
                    }
                }
            }, R"(
                ImportFromS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_prefix: "DependentView"
                        destination_path: "/MyRoot/DependentView"
                    }
                    items {
                        source_prefix: "BaseView"
                        destination_path: "/MyRoot/BaseView"
                    }
                }
            )"
        );
    }

    Y_UNIT_TEST(ShouldSucceedOnSingleTopic) {
        ShouldSucceed({{"",
            {
                EPathTypePersQueueGroup,
                R"(partitioning_settings {
  min_active_partitions: 1
  max_active_partitions: 1
  auto_partitioning_settings {
    strategy: AUTO_PARTITIONING_STRATEGY_DISABLED
    partition_write_speed {
      stabilization_window {
        seconds: 300
      }
      up_utilization_percent: 80
      down_utilization_percent: 20
    }
  }
}
retention_period {
  seconds: 64800
}
supported_codecs {
}
partition_write_speed_bytes_per_second: 1048576
partition_write_burst_bytes: 1048576
consumers {
  name: "consumer1"
  read_from {
  }
  attributes {
    key: "_service_type"
    value: "data-streams"
  }
}
consumers {
  name: "consumer2"
  read_from {
  }
  attributes {
    key: "_service_type"
    value: "data-streams"
  }
}
)"}
        }}, R"(
                ImportFromS3Settings {
                    endpoint: "localhost:%d"
                    scheme: HTTP
                    items {
                        source_prefix: ""
                        destination_path: "/MyRoot/Topic"
                    }
                }
            )");
    }
}
