#pragma once
#include "defs.h"
#include "keyvalue_item_type.h"
#include "keyvalue_key_header.h"
#include "keyvalue_simple_db.h"
#include <ydb/core/base/logoblob.h>

namespace NKikimr {
namespace NKeyValue {

struct TKeyValueStoredStateData;
struct TKeyValueData;
struct THelpers {
    static ui8 Checksum(ui8 prev, size_t dataSize, const ui8* data);
    static bool CheckChecksum(const TString &key);

    static bool ExtractKeyParts(const TString &key, TString &arbitraryPart, TKeyHeader &header);
    static TString GenerateKeyFor(EItemType itemType, const ui8* data, size_t size);
    static TString GenerateKeyFor(EItemType itemType, const TString &arbitraryPart);

    static void DbUpdateState(TKeyValueStoredStateData &state, ISimpleDb &db);
    static void DbEraseUserKey(const TString &userKey, ISimpleDb &db);
    static void DbUpdateUserKeyValue(const TString &userKey, const TString& value, ISimpleDb &db);
    static void DbEraseTrash(const TLogoBlobID &id, ISimpleDb &db);
    static void DbUpdateTrash(const TLogoBlobID &id, ISimpleDb &db);
    static void DbEraseCollect(ISimpleDb &db);
    static void DbUpdateVacuumGeneration(ui64 generation, ISimpleDb &db);

    using TGenerationStep = std::tuple<ui32, ui32>;
    static TGenerationStep GenerationStep(const TLogoBlobID &id);
};

} // NKeyValue
} // NKikimr
