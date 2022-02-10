#pragma once

#include "change_collector_iface.h"

#include <util/generic/cast.h>
#include <ydb/core/tablet/tablet_exception.h>
#include <ydb/library/yql/minikql/mkql_function_registry.h>
#include <ydb/library/yql/minikql/mkql_node_cast.h>
#include <ydb/library/yql/minikql/mkql_program_builder.h>
#include <ydb/core/engine/mkql_engine_flat_host.h>

namespace NKikimr {
namespace NMiniKQL {

struct TEngineHostCounters {
    ui64 NSelectRow = 0;
    ui64 NSelectRange = 0;
    ui64 NUpdateRow  = 0;
    ui64 NEraseRow = 0;

    ui64 SelectRowRows = 0;
    ui64 SelectRowBytes = 0;
    ui64 SelectRangeRows = 0;
    ui64 SelectRangeBytes = 0;
    ui64 SelectRangeDeletedRowSkips = 0;
    ui64 UpdateRowBytes = 0;
    ui64 EraseRowBytes = 0;

    ui64 InvisibleRowSkips = 0;

    TEngineHostCounters& operator+=(const TEngineHostCounters& other) {
        NSelectRow += other.NSelectRow;
        NSelectRange += other.NSelectRange;
        NUpdateRow += other.NUpdateRow;
        NEraseRow += other.NEraseRow;
        SelectRowRows += other.SelectRowRows;
        SelectRowBytes += other.SelectRowBytes;
        SelectRangeRows += other.SelectRangeRows;
        SelectRangeBytes += other.SelectRangeBytes;
        SelectRangeDeletedRowSkips += other.SelectRangeDeletedRowSkips;
        UpdateRowBytes += other.UpdateRowBytes;
        EraseRowBytes += other.EraseRowBytes;
        InvisibleRowSkips += other.InvisibleRowSkips;
        return *this;
    }

    TString ToString() const {
        return TStringBuilder()
            << "{NSelectRow: " << NSelectRow
            << ", NSelectRange: " << NSelectRange
            << ", NUpdateRow: " << NUpdateRow
            << ", NEraseRow: " << NEraseRow
            << ", SelectRowRows: " << SelectRowRows
            << ", SelectRowBytes: " << SelectRowBytes
            << ", SelectRangeRows: " << SelectRangeRows
            << ", SelectRangeBytes: " << SelectRangeBytes
            << ", UpdateRowBytes: " << UpdateRowBytes
            << ", EraseRowBytes: " << EraseRowBytes
            << ", SelectRangeDeletedRowSkips: " << SelectRangeDeletedRowSkips
            << ", InvisibleRowSkips: " << InvisibleRowSkips
            << "}";
    }
};

struct IKeyAccessSampler : public TThrRefBase {
    using TPtr = TIntrusivePtr<IKeyAccessSampler>;
    virtual void AddSample(const TTableId& tableId, const TArrayRef<const TCell>& key) = 0;
};

struct TNoopKeySampler : public IKeyAccessSampler {
    void AddSample(const TTableId& tableId, const TArrayRef<const TCell>& key) override {
        Y_UNUSED(tableId);
        Y_UNUSED(key);
    }
};

struct TEngineHostSettings {
    ui64 ShardId;
    bool IsReadonly;
    bool DisableByKeyFilter;
    IKeyAccessSampler::TPtr KeyAccessSampler;

    explicit TEngineHostSettings(ui64 shardId = 0, bool IsReadonly = false, bool disableByKeyFilter = false,
                                 IKeyAccessSampler::TPtr keyAccessSampler = new TNoopKeySampler())
        : ShardId(shardId)
        , IsReadonly(IsReadonly)
        , DisableByKeyFilter(disableByKeyFilter)
        , KeyAccessSampler(keyAccessSampler)
    {}
};

class TEngineHost : public IEngineFlatHost {
public:
    using TScheme = NTable::TScheme;

    explicit TEngineHost(NTable::TDatabase& db, TEngineHostCounters& counters,
        const TEngineHostSettings& settings = TEngineHostSettings());
    ui64 GetShardId() const override;
    const TScheme::TTableInfo* GetTableInfo(const TTableId& tableId) const override; 
    bool IsReadonly() const override;
    bool IsValidKey(TKeyDesc& key, std::pair<ui64, ui64>& maxSnapshotTime) const override;
    ui64 CalculateReadSize(const TVector<const TKeyDesc*>& keys) const override;
    ui64 CalculateResultSize(const TKeyDesc& key) const override;
    void PinPages(const TVector<THolder<TKeyDesc>>& keys, ui64 pageFaultCount) override;

    NUdf::TUnboxedValue SelectRow(const TTableId& tableId, const TArrayRef<const TCell>& row,
        TStructLiteral* columnIds, TOptionalType* returnType, const TReadTarget& readTarget,
        const THolderFactory& holderFactory) override;

    NUdf::TUnboxedValue SelectRange(const TTableId& tableId, const TTableRange& range,
        TStructLiteral* columnIds, TListLiteral* skipNullKeys, TStructType* returnType,
        const TReadTarget& readTarget, ui64 itemsLimit, ui64 bytesLimit, bool reverse,
        std::pair<const TListLiteral*, const TListLiteral*> forbidNullArgs, const THolderFactory& holderFactory) override;

    void UpdateRow(const TTableId& tableId, const TArrayRef<const TCell>& row,
        const TArrayRef<const TUpdateCommand>& commands) override;
    void EraseRow(const TTableId& tableId, const TArrayRef<const TCell>& row) override;
    bool IsPathErased(const TTableId& tableId) const override;
    bool IsMyKey(const TTableId& tableId, const TArrayRef<const TCell>& row) const override;
    ui64 GetTableSchemaVersion(const TTableId&) const override;

    void SetPeriodicCallback(TPeriodicCallback&& callback) override;
    void ExecPeriodicCallback() { if (PeriodicCallback) { PeriodicCallback();} }

    TEngineHostCounters& GetCounters() const { return Counters; }

    virtual TRowVersion GetWriteVersion(const TTableId& tableId) const = 0;
    virtual TRowVersion GetReadVersion(const TTableId& tableId) const = 0;

    virtual IChangeCollector* GetChangeCollector(const TTableId& tableId) const = 0;

protected:
    virtual ui64 LocalTableId(const TTableId& tableId) const;
    void ConvertKeys(const TScheme::TTableInfo* tableInfo, const TArrayRef<const TCell>& row,
        TSmallVec<TRawTypeValue>& key) const;
    void DoCalculateReadSize(const TKeyDesc& key, NTable::TSizeEnv& env) const;

protected:
    NTable::TDatabase& Db;
    const TScheme& Scheme;
    const TEngineHostSettings Settings;
    TEngineHostCounters& Counters;
    TPeriodicCallback PeriodicCallback;
};

class TUnversionedEngineHost : public TEngineHost {
public:
    using TEngineHost::TEngineHost;

    TRowVersion GetWriteVersion(const TTableId& tableId) const override {
        Y_UNUSED(tableId);
        return TRowVersion::Min();
    }

    TRowVersion GetReadVersion(const TTableId& tableId) const override {
        Y_UNUSED(tableId);
        return TRowVersion::Max();
    }

    IChangeCollector* GetChangeCollector(const TTableId& tableId) const override {
        Y_UNUSED(tableId);
        return nullptr;
    }
};

void AnalyzeRowType(TStructLiteral* columnIds, TSmallVec<NTable::TTag>& tags, TSmallVec<NTable::TTag>& systemColumnTags);
NUdf::TUnboxedValue GetCellValue(const TCell& cell, NScheme::TTypeId type);
NUdf::TUnboxedValue CreateSelectRangeLazyRowsList(NTable::TDatabase& db, const NTable::TScheme& scheme,
    const THolderFactory& holderFactory, const TTableId& tableId, ui64 localTid, const TSmallVec<NTable::TTag>& tags,
    const TSmallVec<bool>& skipNullKeys, const TTableRange& range, ui64 itemsLimit, ui64 bytesLimit,
    bool reverse, TEngineHostCounters& counters, const TSmallVec<NTable::TTag>& systemColumnTags, ui64 shardId);

void ConvertTableKeys(const NTable::TScheme& scheme, const NTable::TScheme::TTableInfo* tableInfo,
    const TArrayRef<const TCell>& row, TSmallVec<TRawTypeValue>& key, ui64* keyDataBytes);

}}
