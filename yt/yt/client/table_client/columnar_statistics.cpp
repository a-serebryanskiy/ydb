#include "columnar_statistics.h"

#include "name_table.h"
#include "unversioned_row.h"
#include "versioned_row.h"

#include <library/cpp/iterator/functools.h>

#include <numeric>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

TNamedColumnarStatistics& TNamedColumnarStatistics::operator+=(const TNamedColumnarStatistics& other)
{
    for (const auto& [columnName, dataWeight] : other.ColumnDataWeights) {
        ColumnDataWeights[columnName] += dataWeight;
    }
    if (other.TimestampTotalWeight) {
        TimestampTotalWeight = TimestampTotalWeight.value_or(0) + *other.TimestampTotalWeight;
    }
    LegacyChunkDataWeight += other.LegacyChunkDataWeight;
    return *this;
}

////////////////////////////////////////////////////////////////////////////////

namespace {

constexpr size_t MaxStringValueLength = 100;
constexpr auto NullUnversionedValue = MakeNullValue<TUnversionedValue>();

//! Approximates long string values with shorter but lexicographically less ones. Other values are intact.
TUnversionedOwningValue ApproximateMinValue(TUnversionedValue value)
{
    if (value.Type != EValueType::String || value.Length <= MaxStringValueLength) {
        value.Flags = EValueFlags::None;
        value.Id = 0;
        return value;
    }
    return MakeUnversionedStringValue(value.AsStringBuf().SubString(0, MaxStringValueLength));
}

//! Approximates long string values with shorter but lexicographically greater ones. Other values are intact.
TUnversionedOwningValue ApproximateMaxValue(TUnversionedValue value)
{
    if (value.Type != EValueType::String || value.Length <= MaxStringValueLength) {
        value.Flags = EValueFlags::None;
        value.Id = 0;
        return value;
    }

    const char MaxChar = std::numeric_limits<unsigned char>::max();
    auto truncatedStringBuf = value.AsStringBuf().SubString(0, MaxStringValueLength);

    while (!truncatedStringBuf.empty() && truncatedStringBuf.back() == MaxChar) {
        truncatedStringBuf.remove_suffix(1);
    }

    if (truncatedStringBuf.empty()) {
        // String was larger than any possible approximation.
        return MakeSentinelValue<TUnversionedValue>(EValueType::Max);
    } else {
        TUnversionedOwningValue result = MakeUnversionedStringValue(truncatedStringBuf);
        char* mutableString = result.GetMutableString();
        int lastIndex = truncatedStringBuf.size() - 1;
        mutableString[lastIndex] = static_cast<unsigned char>(mutableString[lastIndex]) + 1;
        return result;
    }
}

void UpdateLargeColumnarStatistics(TLargeColumnarStatistics& statistics, TUnversionedValue value)
{
    if (value.Type != EValueType::Null) {
        auto valueNoFlags = value;
        valueNoFlags.Flags = EValueFlags::None;
        auto fingerprint = TBitwiseUnversionedValueHash()(valueNoFlags);
        statistics.ColumnHyperLogLogDigests[value.Id].Add(fingerprint);
    }
}

void UpdateColumnarStatistics(
    TUnversionedValue value,
    i64 dataWeight,
    TColumnarStatistics* statistics,
    std::vector<TUnversionedValue>* minValues,
    std::vector<TUnversionedValue>* maxValues,
    bool needsValueStatistics,
    bool needsLargeStatistics)
{
    if (Y_UNLIKELY(static_cast<int>(value.Id) >= statistics->GetColumnCount())) {
        statistics->Resize(value.Id + 1);

        if (needsValueStatistics) {
            minValues->resize(value.Id + 1, NullUnversionedValue);
            maxValues->resize(value.Id + 1, NullUnversionedValue);
        }
    }

    statistics->ColumnDataWeights[value.Id] += dataWeight;

    if (needsValueStatistics) {
        auto& minValue = (*minValues)[value.Id];
        auto& maxValue = (*maxValues)[value.Id];
        if (IsAnyOrComposite(value.Type)) {
            // Composite and YSON values are not comparable.
            minValue = MakeSentinelValue<TUnversionedValue>(EValueType::Min);
            maxValue = MakeSentinelValue<TUnversionedValue>(EValueType::Max);
        } else if (value.Type != EValueType::Null) {
            if (minValue == NullUnversionedValue || value < minValue) {
                minValue = value;
            }
            if (maxValue == NullUnversionedValue || value > maxValue) {
                maxValue = value;
            }
        }

        statistics->ColumnNonNullValueCounts[value.Id] += (value.Type != EValueType::Null);

        if (needsLargeStatistics) {
            UpdateLargeColumnarStatistics(statistics->LargeStatistics, value);
        }
    }
}

void UpdateMinAndMax(
    TColumnarStatistics* statistics,
    TRange<TUnversionedValue> minValues,
    TRange<TUnversionedValue> maxValues)
{
    YT_VERIFY(minValues.size() == maxValues.size());
    YT_VERIFY(std::ssize(minValues) == statistics->GetColumnCount());

    for (int index = 0; index < std::ssize(minValues); ++index) {
        if (minValues[index] != NullUnversionedValue &&
            (statistics->ColumnMinValues[index] == NullUnversionedValue || statistics->ColumnMinValues[index] > minValues[index]))
        {
            statistics->ColumnMinValues[index] = ApproximateMinValue(minValues[index]);
        }

        if (maxValues[index] != NullUnversionedValue &&
            (statistics->ColumnMaxValues[index] == NullUnversionedValue || statistics->ColumnMaxValues[index] < maxValues[index]))
        {
            statistics->ColumnMaxValues[index] = ApproximateMaxValue(maxValues[index]);
        }
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

bool TLargeColumnarStatistics::IsEmpty() const
{
    return ColumnHyperLogLogDigests.empty();
}

void TLargeColumnarStatistics::Clear()
{
    ColumnHyperLogLogDigests.clear();
}

void TLargeColumnarStatistics::Resize(int columnCount)
{
    ColumnHyperLogLogDigests.resize(columnCount);
}

TLargeColumnarStatistics& TLargeColumnarStatistics::operator+=(const TLargeColumnarStatistics& other)
{
    for (int index = 0; index < std::ssize(ColumnHyperLogLogDigests); ++index) {
        ColumnHyperLogLogDigests[index].Merge(other.ColumnHyperLogLogDigests[index]);
    }
    return *this;
}

////////////////////////////////////////////////////////////////////////////////

TColumnarStatistics& TColumnarStatistics::operator+=(const TColumnarStatistics& other)
{
    ReadDataSizeEstimate.reset();

    if (GetColumnCount() == 0) {
        Resize(other.GetColumnCount(), other.HasValueStatistics(), other.HasLargeStatistics());
    }

    YT_VERIFY(GetColumnCount() == other.GetColumnCount());

    for (int index = 0; index < GetColumnCount(); ++index) {
        ColumnDataWeights[index] += other.ColumnDataWeights[index];
    }
    if (other.TimestampTotalWeight) {
        TimestampTotalWeight = TimestampTotalWeight.value_or(0) + *other.TimestampTotalWeight;
    }
    LegacyChunkDataWeight += other.LegacyChunkDataWeight;

    if (ChunkRowCount.has_value() && other.ChunkRowCount.has_value()) {
        ChunkRowCount = *ChunkRowCount + *other.ChunkRowCount;
    } else {
        ChunkRowCount.reset();
    }
    if (LegacyChunkRowCount.has_value() && other.LegacyChunkRowCount.has_value()) {
        LegacyChunkRowCount = *LegacyChunkRowCount + *other.LegacyChunkRowCount;
    } else {
        LegacyChunkRowCount.reset();
    }

    if (!other.HasValueStatistics()) {
        ClearValueStatistics();
    } else if (HasValueStatistics()) {
        bool mergeLargeStatistics = HasLargeStatistics() && other.HasLargeStatistics();
        for (int index = 0; index < GetColumnCount(); ++index) {

            if (other.ColumnMinValues[index] != NullUnversionedValue &&
                (ColumnMinValues[index] == NullUnversionedValue || ColumnMinValues[index] > other.ColumnMinValues[index]))
            {
                ColumnMinValues[index] = other.ColumnMinValues[index];
            }

            if (other.ColumnMaxValues[index] != NullUnversionedValue &&
                (ColumnMaxValues[index] == NullUnversionedValue || ColumnMaxValues[index] < other.ColumnMaxValues[index]))
            {
                ColumnMaxValues[index] = other.ColumnMaxValues[index];
            }

            ColumnNonNullValueCounts[index] += other.ColumnNonNullValueCounts[index];
        }
        if (mergeLargeStatistics) {
            LargeStatistics += other.LargeStatistics;
        } else {
            LargeStatistics.Clear();
        }
    }

    return *this;
}

TColumnarStatistics TColumnarStatistics::MakeEmpty(int columnCount, bool hasValueStatistics, bool hasLargeStatistics)
{
    TColumnarStatistics result;
    result.Resize(columnCount, hasValueStatistics, hasLargeStatistics);
    return result;
}

TColumnarStatistics TColumnarStatistics::MakeLegacy(int columnCount, i64 legacyChunkDataWeight, i64 legacyChunkRowCount)
{
    TColumnarStatistics result = MakeEmpty(columnCount, /*hasValueStatistics*/ false);
    result.LegacyChunkDataWeight = legacyChunkDataWeight;
    result.LegacyChunkRowCount = legacyChunkRowCount;
    return result;
}

TLightweightColumnarStatistics TColumnarStatistics::MakeLightweightStatistics() const
{
    return TLightweightColumnarStatistics{
        .ColumnDataWeightsSum = std::accumulate(ColumnDataWeights.begin(), ColumnDataWeights.end(), (i64)0),
        .TimestampTotalWeight = TimestampTotalWeight,
        .LegacyChunkDataWeight = LegacyChunkDataWeight,
        .ReadDataSizeEstimate = ReadDataSizeEstimate,
    };
}

TNamedColumnarStatistics TColumnarStatistics::MakeNamedStatistics(const std::vector<std::string>& names) const
{
    TNamedColumnarStatistics result;
    result.TimestampTotalWeight = TimestampTotalWeight;
    result.LegacyChunkDataWeight = LegacyChunkDataWeight;

    for (const auto& [name, columnDataWeight] : Zip(names, ColumnDataWeights)) {
        result.ColumnDataWeights[name] = columnDataWeight;
    }

    return result;
}

bool TColumnarStatistics::HasValueStatistics() const
{
    YT_VERIFY(ColumnMinValues.size() == ColumnMaxValues.size());
    YT_VERIFY(ColumnMinValues.size() == ColumnNonNullValueCounts.size());

    return GetColumnCount() == 0 || !ColumnMinValues.empty();
}

bool TColumnarStatistics::HasLargeStatistics() const
{
    return GetColumnCount() == 0 || !LargeStatistics.IsEmpty();
}

void TColumnarStatistics::ClearValueStatistics()
{
    ColumnMinValues.clear();
    ColumnMaxValues.clear();
    ColumnNonNullValueCounts.clear();
    LargeStatistics.Clear();
}

int TColumnarStatistics::GetColumnCount() const
{
    return ColumnDataWeights.size();
}

void TColumnarStatistics::Resize(int columnCount, bool keepValueStatistics, bool keepLargeStatistics)
{
    ReadDataSizeEstimate.reset();

    if (columnCount < GetColumnCount()) {
        // Downsizes are not allowed. If reducing column count, must clear the stats completely.
        YT_VERIFY(columnCount == 0);
    }
    keepValueStatistics &= HasValueStatistics();
    keepLargeStatistics &= (keepValueStatistics && HasLargeStatistics());

    ColumnDataWeights.resize(columnCount, 0);

    if (keepValueStatistics) {
        ColumnMinValues.resize(columnCount, NullUnversionedValue);
        ColumnMaxValues.resize(columnCount, NullUnversionedValue);
        ColumnNonNullValueCounts.resize(columnCount, 0);

        if (keepLargeStatistics) {
            LargeStatistics.Resize(columnCount);
        } else {
            LargeStatistics.Clear();
        }
    } else {
        ClearValueStatistics();
    }
}

void TColumnarStatistics::Update(TRange<TUnversionedRow> rows)
{
    ReadDataSizeEstimate.reset();

    std::vector<TUnversionedValue> minValues(GetColumnCount(), NullUnversionedValue);
    std::vector<TUnversionedValue> maxValues(GetColumnCount(), NullUnversionedValue);

    for (auto row : rows) {
        for (auto value : row) {
            UpdateColumnarStatistics(value, GetDataWeight(value), this, &minValues, &maxValues, HasValueStatistics(), HasLargeStatistics());
        }
    }

    if (HasValueStatistics()) {
        UpdateMinAndMax(this, minValues, maxValues);
    }

    if (ChunkRowCount) {
        ChunkRowCount = *ChunkRowCount + rows.Size();
    }
}

void TColumnarStatistics::Update(TRange<TVersionedRow> rows)
{
    ReadDataSizeEstimate.reset();

    std::vector<TUnversionedValue> minValues(GetColumnCount(), NullUnversionedValue);
    std::vector<TUnversionedValue> maxValues(GetColumnCount(), NullUnversionedValue);

    for (const auto& row : rows) {
        for (const auto& value: row.Keys()) {
            UpdateColumnarStatistics(value, GetDataWeight(value), this, &minValues, &maxValues, HasValueStatistics(), HasLargeStatistics());
        }

        for (const auto& value: row.Values()) {
            UpdateColumnarStatistics(value, GetDataWeight(value), this, &minValues, &maxValues, HasValueStatistics(), HasLargeStatistics());
        }

        TimestampTotalWeight = TimestampTotalWeight.value_or(0) +
            (row.GetWriteTimestampCount() + row.GetDeleteTimestampCount()) * sizeof(TTimestamp);
    }

    if (HasValueStatistics()) {
        UpdateMinAndMax(this, minValues, maxValues);
    }


    if (ChunkRowCount) {
        ChunkRowCount = *ChunkRowCount + rows.Size();
    }
}

TColumnarStatistics TColumnarStatistics::SelectByColumnNames(const TNameTablePtr& nameTable, const std::vector<TColumnStableName>& columnStableNames) const
{
    auto result = MakeEmpty(columnStableNames.size(), HasValueStatistics(), HasLargeStatistics());

    for (const auto& [columnIndex, columnName] : Enumerate(columnStableNames)) {
        if (auto id = nameTable->FindId(columnName.Underlying()); id && *id < GetColumnCount()) {
            result.ColumnDataWeights[columnIndex] = ColumnDataWeights[*id];

            if (HasValueStatistics()) {
                result.ColumnMinValues[columnIndex] = ColumnMinValues[*id];
                result.ColumnMaxValues[columnIndex] = ColumnMaxValues[*id];
                result.ColumnNonNullValueCounts[columnIndex] = ColumnNonNullValueCounts[*id];

                if (HasLargeStatistics()) {
                    result.LargeStatistics.ColumnHyperLogLogDigests[columnIndex] = LargeStatistics.ColumnHyperLogLogDigests[*id];
                }
            }
        }
    }
    result.TimestampTotalWeight = TimestampTotalWeight;
    result.LegacyChunkDataWeight = LegacyChunkDataWeight;

    result.ChunkRowCount = ChunkRowCount;
    result.LegacyChunkRowCount = LegacyChunkRowCount;

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
