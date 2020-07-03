#pragma once

#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnsCommon.h>
#include <Common/FieldVisitors.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeInterval.h>
#include <DataTypes/DataTypeMyDate.h>
#include <DataTypes/DataTypeMyDateTime.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/FunctionsConversion.h>
#include <Functions/FunctionsDateTime.h>
#include <Functions/FunctionsMiscellaneous.h>
#include <Functions/IFunction.h>
#include <IO/Operators.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromVector.h>
#include <IO/parseDateTimeBestEffort.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <Storages/Transaction/Collator.h>

#include <ext/collection_cast.h>
#include <ext/enumerate.h>
#include <ext/range.h>
#include <type_traits>


namespace DB
{

/// cast int/real/decimal/time as string
template <typename FromDataType, bool return_nullable>
struct TiDBConvertToString
{
    using FromFieldType = typename FromDataType::FieldType;

    static size_t charLengthToByteLengthFromUTF8(const char * data, size_t length, size_t char_length)
    {
        size_t ret = 0;
        for (size_t char_index = 0; char_index < char_length && ret < length; char_index++)
        {
            uint8_t c = data[ret];
            if (c < 0x80)
                ret += 1;
            else if (c < 0xE0)
                ret += 2;
            else if (c < 0xF0)
                ret += 3;
            else
                ret += 4;
        }
        return ret;
    }

    static void execute(Block & block, const ColumnNumbers & arguments, size_t result, bool, const tipb::FieldType & tp, const Context &)
    {
        size_t size = block.getByPosition(arguments[0]).column->size();
        ColumnUInt8::MutablePtr col_null_map_to;
        ColumnUInt8::Container * vec_null_map_to [[maybe_unused]] = nullptr;
        if constexpr (return_nullable)
        {
            col_null_map_to = ColumnUInt8::create(size);
            vec_null_map_to = &col_null_map_to->getData();
        }

        const auto & col_with_type_and_name = block.getByPosition(arguments[0]);
        const auto & type = static_cast<const FromDataType &>(*col_with_type_and_name.type);

        auto col_to = ColumnString::create();
        ColumnString::Chars_t & data_to = col_to->getChars();
        ColumnString::Offsets & offsets_to = col_to->getOffsets();

        if constexpr (std::is_same_v<FromDataType, DataTypeString>)
        {
            /// cast string as string
            const IColumn * col_from = block.getByPosition(arguments[0]).column.get();
            const ColumnString * col_from_string = checkAndGetColumn<ColumnString>(col_from);
            const ColumnString::Chars_t * data_from = &col_from_string->getChars();
            const IColumn::Offsets * offsets_from = &col_from_string->getOffsets();

            WriteBufferFromVector<ColumnString::Chars_t> write_buffer(data_to);
            size_t current_offset = 0;
            for (size_t i = 0; i < size; i++)
            {
                size_t next_offset = (*offsets_from)[i];
                size_t org_length = next_offset - current_offset - 1;
                size_t byte_length = org_length;
                if (tp.flen() > 0)
                {
                    byte_length = tp.flen();
                    if (tp.charset() == "utf8" || tp.charset() == "utf8mb4")
                        byte_length = charLengthToByteLengthFromUTF8(
                            reinterpret_cast<const char *>(&(*data_from)[current_offset]), org_length, byte_length);
                }
                // todo handle overflow check
                write_buffer.write(reinterpret_cast<const char *>(&(*data_from)[current_offset]), std::min(byte_length, org_length));
                writeChar(0, write_buffer);
                offsets_to[i] = write_buffer.count();
            }
        }
        else if constexpr (IsDecimal<FromFieldType>)
        {
            /// cast decimal as string
            const auto * col_from = checkAndGetColumn<ColumnDecimal<FromFieldType>>(block.getByPosition(arguments[0]).column.get());
            const typename ColumnDecimal<FromFieldType>::Container & vec_from = col_from->getData();
            ColumnString::Chars_t container_per_element;

            data_to.resize(size * decimal_max_prec + size);
            container_per_element.resize(decimal_max_prec);
            offsets_to.resize(size);

            WriteBufferFromVector<ColumnString::Chars_t> write_buffer(data_to);
            for (size_t i = 0; i < size; ++i)
            {
                WriteBufferFromVector<ColumnString::Chars_t> element_write_buffer(container_per_element);
                FormatImpl<FromDataType>::execute(vec_from[i], element_write_buffer, &type, nullptr);
                write_buffer.write(reinterpret_cast<char *>(container_per_element.data()), element_write_buffer.count());
                writeChar(0, write_buffer);
                offsets_to[i] = write_buffer.count();
            }
        }
        else if (const auto col_from = checkAndGetColumn<ColumnVector<FromFieldType>>(col_with_type_and_name.column.get()))
        {
            /// cast int/real/time as string
            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            ColumnString::Chars_t container_per_element;

            if constexpr (std::is_same_v<FromDataType, DataTypeMyDate>)
            {
                auto length = strlen("YYYY-MM-DD") + 1;
                data_to.resize(size * length);
                container_per_element.resize(length);
            }
            if constexpr (std::is_same_v<FromDataType, DataTypeMyDateTime>)
            {
                auto length = strlen("YYYY-MM-DD hh:mm:ss") + 1 + (type.getFraction() ? 0 : 1 + type.getFraction());
                data_to.resize(size * length);
                container_per_element.resize(length);
            }
            else
            {
                data_to.resize(size * 3);
                container_per_element.resize(3);
            }
            offsets_to.resize(size);

            WriteBufferFromVector<ColumnString::Chars_t> write_buffer(data_to);
            for (size_t i = 0; i < size; ++i)
            {
                WriteBufferFromVector<ColumnString::Chars_t> element_write_buffer(container_per_element);
                FormatImpl<FromDataType>::execute(vec_from[i], element_write_buffer, &type, nullptr);
                write_buffer.write(reinterpret_cast<char *>(container_per_element.data()), element_write_buffer.count());
                writeChar(0, write_buffer);
                offsets_to[i] = write_buffer.count();
            }
        }
        else
            throw Exception(
                "Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function TiDB_cast",
                ErrorCodes::ILLEGAL_COLUMN);

        if constexpr (return_nullable)
            block.getByPosition(result).column = ColumnNullable::create(std::move(col_to), std::move(col_null_map_to));
        else
            block.getByPosition(result).column = std::move(col_to);
    }
};

/// cast int/real/decimal/time/string as int
template <typename FromDataType, typename ToDataType, bool return_nullable, bool to_unsigned>
struct TiDBConvertToInteger
{
    using FromFieldType = typename FromDataType::FieldType;
    using ToFieldType = typename ToDataType::FieldType;

    template <typename T, typename ToFieldType>
    static std::enable_if_t<std::is_floating_point_v<T>, ToFieldType> toUInt(const T & value, const Context &)
    {
        T rounded_value = std::round(value);
        if (rounded_value < 0)
        {
            // todo handle overflow error, check if need clip to zero
            return static_cast<ToFieldType>(rounded_value);
        }
        if (rounded_value > std::numeric_limits<ToFieldType>::max())
            // todo handle overflow error
            return std::numeric_limits<ToFieldType>::max();
        else if (rounded_value == std::numeric_limits<ToFieldType>::max())
            return std::numeric_limits<ToFieldType>::max();
        else
            return static_cast<ToFieldType>(rounded_value);
    }

    template <typename T, typename ToFieldType>
    static std::enable_if_t<std::is_floating_point_v<T>, ToFieldType> toInt(const T & value, const Context &)
    {
        T rounded_value = std::round(value);
        if (rounded_value < std::numeric_limits<ToFieldType>::min())
            // todo handle overflow check
            return std::numeric_limits<ToFieldType>::min();
        if (rounded_value >= std::numeric_limits<ToFieldType>::max())
        {
            // todo handle overflow check when round_value > max()
            return std::numeric_limits<ToFieldType>::max();
        }
        return static_cast<ToFieldType>(rounded_value);
    }

    template <typename T, typename ToFieldType>
    static ToFieldType decToUInt(const DecimalField<T> & value, const Context &)
    {
        auto v = value.getValue().value;
        if (v < 0)
            // todo check overflow
            return static_cast<ToFieldType>(0);
        ScaleType scale = value.getScale();
        for (ScaleType i = 0; i < scale; i++)
        {
            v = v / 10 + (i + 1 == scale && v % 10 >= 5);
        }

        Int128 max_value = std::numeric_limits<ToFieldType>::max();
        if (v > max_value)
        {
            // todo check overflow
            return max_value;
        }
        return static_cast<ToFieldType>(v);
    }

    template <typename T, typename ToFieldType>
    static ToFieldType decToInt(const DecimalField<T> & value, const Context &)
    {
        auto v = value.getValue().value;
        ScaleType scale = value.getScale();
        for (ScaleType i = 0; i < scale; i++)
        {
            v = v / 10 + (i + 1 == scale && v % 10 >= 5);
        }
        if (v > std::numeric_limits<ToFieldType>::max() || v < std::numeric_limits<ToFieldType>::min())
        {
            // todo overflow check
            if (v > 0)
                return std::numeric_limits<ToFieldType>::max();
            return std::numeric_limits<ToFieldType>::min();
        }
        return static_cast<ToFieldType>(v);
    }

    static StringRef trim(const StringRef & value)
    {
        StringRef ret;
        ret.size = 0;
        size_t start = 0;
        static std::unordered_set<char> spaces{'\t', '\n', '\v', '\f', '\r', ' '};
        for (; start < value.size; start++)
        {
            if (!spaces.count(value.data[start]))
                break;
        }
        size_t end = ret.size;
        for (; start < end; end--)
        {
            if (!spaces.count(value.data[end - 1]))
                break;
        }
        if (start >= end)
            return ret;
        ret.data = value.data + start;
        ret.size = end - start;
        return ret;
    }
    //template <typename ToFieldType>
    //static ToFieldType toInt(const StringRef & value, const Context &)
    //{
    //    // trim space
    //    StringRef trim_string = trim(value);
    //    if (trim_string.size == 0)
    //        // todo handle truncated error
    //        return static_cast<ToFieldType>(0);
    //}

    //template <typename ToFieldType>
    //static ToFieldType toUInt(const StringRef & value, const Context &)
    //{
    //
    //}

    static void execute(
        Block & block, const ColumnNumbers & arguments, size_t result, bool, const tipb::FieldType &, const Context & context)
    {
        size_t size = block.getByPosition(arguments[0]).column->size();

        auto col_to = ColumnVector<ToFieldType>::create();
        typename ColumnVector<ToFieldType>::Container & vec_to = col_to->getData();
        vec_to.resize(size);

        ColumnUInt8::MutablePtr col_null_map_to;
        ColumnUInt8::Container * vec_null_map_to [[maybe_unused]] = nullptr;
        if constexpr (return_nullable)
        {
            col_null_map_to = ColumnUInt8::create(size);
            vec_null_map_to = &col_null_map_to->getData();
        }

        if constexpr (IsDecimal<FromFieldType>)
        {
            /// cast decimal as int
            const auto * col_from = checkAndGetColumn<ColumnDecimal<FromFieldType>>(block.getByPosition(arguments[0]).column.get());

            for (size_t i = 0; i < size; ++i)
            {
                auto field = (*col_from)[i].template safeGet<DecimalField<FromFieldType>>();
                if constexpr (to_unsigned)
                {
                    vec_to[i] = decToUInt<FromFieldType, ToFieldType>(field, context);
                }
                else
                {
                    vec_to[i] = decToInt<FromFieldType, ToFieldType>(field, context);
                }
            }
        }
        else if constexpr (std::is_same_v<FromDataType, DataTypeMyDateTime> || std::is_same_v<FromDataType, DataTypeMyDate>)
        {
            /// cast time as int
            const auto & col_with_type_and_name = block.getByPosition(arguments[0]);

            const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(col_with_type_and_name.column.get());
            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            for (size_t i = 0; i < size; i++)
            {
                if constexpr (std::is_same_v<DataTypeMyDate, FromDataType>)
                {
                    MyDate date(vec_from[i]);
                    vec_to[i] = date.year * 10000 + date.month * 100 + date.day;
                }
                else
                {
                    MyDateTime date_time(vec_from[i]);
                    vec_to[i] = date_time.year * 10000000000ULL + date_time.month * 100000000ULL + date_time.day * 100000
                        + date_time.hour * 1000 + date_time.minute * 100 + date_time.second;
                }
            }
        }
        else if constexpr (std::is_same_v<FromDataType, DataTypeString>)
        {
            // todo support cast string as int
            /// cast string as int
            //const IColumn * col_from = block.getByPosition(arguments[0]).column.get();
            //const ColumnString * col_from_string = checkAndGetColumn<ColumnString>(col_from);
            //const ColumnString::Chars_t * chars = &col_from_string->getChars();
            //const IColumn::Offsets * offsets = &col_from_string->getOffsets();
            //size_t current_offset = 0;
            //for (size_t i = 0; i < size; i++)
            //{
            //    size_t next_offset = (*offsets)[i];
            //    size_t string_size = next_offset - current_offset - 1;
            //    ReadBufferFromMemory read_buffer(&(*chars)[current_offset], string_size);
            //}
        }
        else if constexpr (std::is_integral_v<FromFieldType>)
        {
            /// cast int as int
            const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(block.getByPosition(arguments[0]).column.get());
            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            for (size_t i = 0; i < size; i++)
                vec_to[i] = static_cast<ToFieldType>(vec_from[i]);
        }
        else if constexpr (std::is_floating_point_v<FromFieldType>)
        {
            /// cast real as int
            const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(block.getByPosition(arguments[0]).column.get());
            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            if constexpr (to_unsigned)
            {
                for (size_t i = 0; i < size; i++)
                    vec_to[i] = toUInt<FromFieldType, ToFieldType>(vec_from[i], context);
            }
            else
            {
                for (size_t i = 0; i < size; i++)
                    vec_to[i] = toInt<FromFieldType, ToFieldType>(vec_from[i], context);
            }
        }
        else
        {
            throw Exception(
                "Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function tidb_cast",
                ErrorCodes::ILLEGAL_COLUMN);
        }

        if constexpr (return_nullable)
            block.getByPosition(result).column = ColumnNullable::create(std::move(col_to), std::move(col_null_map_to));
        else
            block.getByPosition(result).column = std::move(col_to);
    }
};

/// cast int/real/decimal/time/string as real
template <typename FromDataType, bool return_nullable, bool to_unsigned>
struct TiDBConvertToFloat
{
    using FromFieldType = typename FromDataType::FieldType;

    static Float64 produceTargetFloat64(Float64 value, bool need_truncate, Float64 shift, Float64 max_f, const Context &)
    {
        if (need_truncate)
        {
            value *= shift;
            value = std::round(value) / shift;
            if (value > max_f)
                // todo overflow check
                value = max_f;
            if (value < -max_f)
                value = -max_f;
        }
        if constexpr (to_unsigned)
        {
            if (value < 0)
                // todo overflow check
                value = 0;
        }
        return value;
    }

    template <typename T>
    static std::enable_if_t<std::is_integral_v<T>, Float64> toFloat(
        const T & value, bool need_truncate, Float64 shift, Float64 max_f, const Context & context)
    {
        Float64 float_value = static_cast<Float64>(value);
        if constexpr (to_unsigned)
        {
            float_value = static_cast<Float64>(static_cast<UInt64>(value));
        }
        return produceTargetFloat64(float_value, need_truncate, shift, max_f, context);
    }

    template <typename T>
    static std::enable_if_t<std::is_floating_point_v<T>, Float64> toFloat(
        const T & value, bool need_truncate, Float64 shift, Float64 max_f, const Context & context)
    {
        Float64 float_value = static_cast<Float64>(value);
        return produceTargetFloat64(float_value, need_truncate, shift, max_f, context);
    }

    template <typename T>
    static Float64 toFloat(const DecimalField<T> & value, bool need_truncate, Float64 shift, Float64 max_f, const Context & context)
    {
        Float64 float_value = static_cast<Float64>(value);
        return produceTargetFloat64(float_value, need_truncate, shift, max_f, context);
    }

    static StringRef trim(const StringRef & value)
    {
        StringRef ret;
        ret.size = 0;
        size_t start = 0;
        static std::unordered_set<char> spaces{' ', '\t', '\n', '\v', '\f', '\r', ' '};
        for (; start < value.size; start++)
        {
            if (!spaces.count(value.data[start]))
                break;
        }
        size_t end = ret.size;
        for (; start < end; end--)
        {
            if (!spaces.count(value.data[end - 1]))
                break;
        }
        if (start >= end)
            return ret;
        ret.data = value.data + start;
        ret.size = end - start;
        return ret;
    }
    //template <typename ToFieldType>
    //static ToFieldType toInt(const StringRef & value, const Context &)
    //{
    //    // trim space
    //    StringRef trim_string = trim(value);
    //    if (trim_string.size == 0)
    //        // todo handle truncated error
    //        return static_cast<ToFieldType>(0);
    //}

    //template <typename ToFieldType>
    //static ToFieldType toUInt(const StringRef & value, const Context &)
    //{
    //
    //}

    static void execute(
        Block & block, const ColumnNumbers & arguments, size_t result, bool, const tipb::FieldType & tp, const Context & context)
    {
        size_t size = block.getByPosition(arguments[0]).column->size();

        auto col_to = ColumnVector<Float64>::create();
        typename ColumnVector<Float64>::Container & vec_to = col_to->getData();
        vec_to.resize(size);

        ColumnUInt8::MutablePtr col_null_map_to;
        ColumnUInt8::Container * vec_null_map_to [[maybe_unused]] = nullptr;
        if constexpr (return_nullable)
        {
            col_null_map_to = ColumnUInt8::create(size);
            vec_null_map_to = &col_null_map_to->getData();
        }

        bool need_truncate = tp.flen() != -1 && tp.decimal() != -1 && tp.flen() >= tp.decimal();
        Float64 shift = 0;
        Float64 max_f = 0;
        if (need_truncate)
        {
            shift = std::pow((Float64)10, tp.flen());
            max_f = std::pow((Float64)10, tp.flen() - tp.decimal()) - 1.0 / shift;
        }

        if constexpr (IsDecimal<FromFieldType>)
        {
            /// cast decimal as real
            const auto * col_from = checkAndGetColumn<ColumnDecimal<FromFieldType>>(block.getByPosition(arguments[0]).column.get());

            for (size_t i = 0; i < size; ++i)
            {
                auto & field = (*col_from)[i].template safeGet<DecimalField<FromFieldType>>();
                vec_to[i] = toFloat(field, need_truncate, shift, max_f, context);
            }
        }
        else if constexpr (std::is_same_v<FromDataType, DataTypeMyDateTime> || std::is_same_v<FromDataType, DataTypeMyDate>)
        {
            /// cast time as real
            const auto & col_with_type_and_name = block.getByPosition(arguments[0]);
            const auto & type = static_cast<const FromDataType &>(*col_with_type_and_name.type);

            const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(col_with_type_and_name.column.get());
            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            for (size_t i = 0; i < size; i++)
            {
                if constexpr (std::is_same_v<DataTypeMyDate, FromDataType>)
                {
                    MyDate date(vec_from[i]);
                    vec_to[i] = toFloat(date.year * 10000 + date.month * 100 + date.day, need_truncate, shift, max_f, context);
                }
                else
                {
                    MyDateTime date_time(vec_from[i]);
                    if (type.getFraction() > 0)
                        vec_to[i] = toFloat(date_time.year * 10000000000ULL + date_time.month * 100000000ULL + date_time.day * 100000
                                + date_time.hour * 1000 + date_time.minute * 100 + date_time.second + date_time.micro_second / 1000000.0,
                            need_truncate, shift, max_f, context);
                    else
                        vec_to[i] = toFloat(date_time.year * 10000000000ULL + date_time.month * 100000000ULL + date_time.day * 100000
                                + date_time.hour * 1000 + date_time.minute * 100 + date_time.second,
                            need_truncate, shift, max_f, context);
                }
            }
        }
        else if constexpr (std::is_same_v<FromDataType, DataTypeString>)
        {
            // todo support cast string as real
            /// cast string as real
            //const IColumn * col_from = block.getByPosition(arguments[0]).column.get();
            //const ColumnString * col_from_string = checkAndGetColumn<ColumnString>(col_from);
            //const ColumnString::Chars_t * chars = &col_from_string->getChars();
            //const IColumn::Offsets * offsets = &col_from_string->getOffsets();
            //size_t current_offset = 0;
            //for (size_t i = 0; i < size; i++)
            //{
            //    size_t next_offset = (*offsets)[i];
            //    size_t string_size = next_offset - current_offset - 1;
            //    ReadBufferFromMemory read_buffer(&(*chars)[current_offset], string_size);
            //}
        }
        else if constexpr (std::is_integral_v<FromFieldType> || std::is_floating_point_v<FromFieldType>)
        {
            /// cast int/real as real
            const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(block.getByPosition(arguments[0]).column.get());
            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            for (size_t i = 0; i < size; i++)
                vec_to[i] = toFloat(vec_from[i], need_truncate, shift, max_f, context);
        }
        else
        {
            throw Exception(
                "Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function tidb_cast",
                ErrorCodes::ILLEGAL_COLUMN);
        }

        if constexpr (return_nullable)
            block.getByPosition(result).column = ColumnNullable::create(std::move(col_to), std::move(col_null_map_to));
        else
            block.getByPosition(result).column = std::move(col_to);
    }
};

/// cast int/real/decimal/time/string as decimal
template <typename FromDataType, typename ToFieldType, bool return_nullable, bool to_unsigned>
struct TiDBConvertToDecimal
{
    using FromFieldType = typename FromDataType::FieldType;

    template <typename T, typename U>
    static U ToTiDBDecimalInternal(T value, PrecType prec, ScaleType scale)
    {
        using UType = typename U::NativeType;
        auto maxValue = DecimalMaxValue::Get(prec);
        if (value > maxValue || value < -maxValue)
        {
            // todo should throw exception or log warnings based on the flag in dag request
            if (value > 0)
                return static_cast<UType>(maxValue);
            else
                return static_cast<UType>(-maxValue);
        }
        UType scale_mul = getScaleMultiplier<U>(scale);
        U result = static_cast<UType>(value) * scale_mul;
        return result;
    }

    template <typename U>
    static U ToTiDBDecimal(MyDateTime & date_time, PrecType prec, ScaleType scale, bool in_union, const tipb::FieldType & tp, int fsp)
    {
        UInt64 value_without_fsp = date_time.year * 10000000000ULL + date_time.month * 100000000ULL + date_time.day * 100000
            + date_time.hour * 1000 + date_time.minute * 100 + date_time.second;
        if (fsp > 0)
        {
            Int128 value = value_without_fsp * 1000000 + date_time.micro_second;
            Decimal128 decimal(value);
            return ToTiDBDecimal<Decimal128, U>(decimal, 6, prec, scale, in_union, tp);
        }
        else
        {
            return ToTiDBDecimalInternal<UInt64, U>(value_without_fsp, prec, scale);
        }
    }

    template <typename U>
    static U ToTiDBDecimal(MyDate & date, PrecType prec, ScaleType scale, bool, const tipb::FieldType &)
    {
        UInt64 value = date.year * 10000 + date.month * 100 + date.day;
        return ToTiDBDecimalInternal<UInt64, U>(value, prec, scale);
    }

    template <typename T, typename U>
    static std::enable_if_t<std::is_integral_v<T>, U> ToTiDBDecimal(T value, PrecType prec, ScaleType scale, bool, const tipb::FieldType &)
    {
        if constexpr (std::is_signed_v<T>)
            return ToTiDBDecimalInternal<T, U>(value, prec, scale);
        else
            return ToTiDBDecimalInternal<UInt64, U>(static_cast<UInt64>(value), prec, scale);
    }

    template <typename T, typename U>
    static std::enable_if_t<std::is_floating_point_v<T>, U> ToTiDBDecimal(
        T value, PrecType prec, ScaleType scale, bool, const tipb::FieldType &)
    {
        using UType = typename U::NativeType;
        /// copied from TiDB code, might be some bugs here
        bool neg = false;
        if (value < 0)
        {
            neg = true;
            value = -value;
        }
        for (ScaleType i = 0; i < scale; i++)
        {
            value *= 10;
        }
        auto max_value = DecimalMaxValue::Get(prec);
        if (value > static_cast<Float64>(max_value))
        {
            if (!neg)
                return static_cast<UType>(max_value);
            else
                return static_cast<UType>(-max_value);
        }
        // rounding
        T tenTimesValue = value * 10;
        UType v(value);
        if (Int256(tenTimesValue) % 10 >= 5)
        {
            v++;
        }
        if (neg)
        {
            v = -v;
        }
        return v;
    }

    template <typename T, typename U>
    static std::enable_if_t<IsDecimal<T>, U> ToTiDBDecimal(
        const T & v, ScaleType v_scale, PrecType prec, ScaleType scale, bool in_union, const tipb::FieldType & tp)
    {
        using UType = typename U::NativeType;
        auto value = Int256(v.value);
        if (unlikely(in_union && hasUnsignedFlag(tp) && value < 0))
            return static_cast<UType>(0);

        if (v_scale <= scale)
        {
            for (ScaleType i = v_scale; i < scale; i++)
                value *= 10;
        }
        else
        {
            // todo handle truncate
            bool need2Round = false;
            for (ScaleType i = scale; i < v_scale; i++)
            {
                need2Round = (value < 0 ? -value : value) % 10 >= 5;
                value /= 10;
            }
            if (need2Round)
            {
                if (value < 0)
                    value--;
                else
                    value++;
            }
        }

        auto max_value = DecimalMaxValue::Get(prec);
        if (value > max_value || value < -max_value)
        {
            // todo should throw exception or log warnings based on the flag in dag request
            if (value > 0)
                return static_cast<UType>(max_value);
            else
                return static_cast<UType>(-max_value);
        }
        return static_cast<UType>(value);
    }

    /// cast int/real/time/decimal as decimal
    static void execute(Block & block, const ColumnNumbers & arguments, size_t result, PrecType prec [[maybe_unused]], ScaleType scale,
        bool in_union, const tipb::FieldType & tp, const Context &)
    {
        size_t size = block.getByPosition(arguments[0]).column->size();
        ColumnUInt8::MutablePtr col_null_map_to;
        ColumnUInt8::Container * vec_null_map_to [[maybe_unused]] = nullptr;
        if constexpr (return_nullable)
        {
            col_null_map_to = ColumnUInt8::create(size);
            vec_null_map_to = &col_null_map_to->getData();
        }

        if constexpr (IsDecimal<FromFieldType>)
        {
            /// cast decimal as decimal
            const auto * col_from = checkAndGetColumn<ColumnDecimal<FromFieldType>>(block.getByPosition(arguments[0]).column.get());
            auto col_to = ColumnDecimal<ToFieldType>::create(0, scale);

            const typename ColumnDecimal<FromFieldType>::Container & vec_from = col_from->getData();
            typename ColumnDecimal<ToFieldType>::Container & vec_to = col_to->getData();
            vec_to.resize(size);

            for (size_t i = 0; i < size; ++i)
            {
                vec_to[i] = ToTiDBDecimal<FromFieldType, ToFieldType>(vec_from[i], vec_from.getScale(), prec, scale, in_union, tp);
            }

            if constexpr (return_nullable)
                block.getByPosition(result).column = ColumnNullable::create(std::move(col_to), std::move(col_null_map_to));
            else
                block.getByPosition(result).column = std::move(col_to);
        }
        else if constexpr (std::is_same_v<DataTypeMyDateTime, FromDataType> || std::is_same_v<DataTypeMyDate, FromDataType>)
        {
            /// cast time as decimal
            const auto & col_with_type_and_name = block.getByPosition(arguments[0]);
            const auto & type = static_cast<const FromDataType &>(*col_with_type_and_name.type);

            const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(col_with_type_and_name.column.get());
            auto col_to = ColumnDecimal<ToFieldType>::create(0, scale);

            const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
            typename ColumnDecimal<ToFieldType>::Container & vec_to = col_to->getData();
            vec_to.resize(size);

            for (size_t i = 0; i < size; ++i)
            {
                if constexpr (std::is_same_v<DataTypeMyDate, FromDataType>)
                {
                    MyDate date(vec_from[i]);
                    vec_to[i] = ToTiDBDecimal<ToFieldType>(date, prec, scale, in_union, tp);
                }
                else
                {
                    MyDateTime date_time(vec_from[i]);
                    vec_to[i] = ToTiDBDecimal<ToFieldType>(date_time, prec, scale, in_union, tp, type.getFraction());
                }
            }
            if constexpr (return_nullable)
                block.getByPosition(result).column = ColumnNullable::create(std::move(col_to), std::move(col_null_map_to));
            else
                block.getByPosition(result).column = std::move(col_to);
        }
        else if constexpr (std::is_same_v<DataTypeString, FromDataType>)
        {
            /// cast string as decimal
        }
        else
        {
            /// cast int/real as decimal
            if (const ColumnVector<FromFieldType> * col_from
                = checkAndGetColumn<ColumnVector<FromFieldType>>(block.getByPosition(arguments[0]).column.get()))
            {
                auto col_to = ColumnDecimal<ToFieldType>::create(0, scale);

                const typename ColumnVector<FromFieldType>::Container & vec_from = col_from->getData();
                typename ColumnDecimal<ToFieldType>::Container & vec_to = col_to->getData();
                vec_to.resize(size);

                for (size_t i = 0; i < size; ++i)
                {
                    vec_to[i] = ToTiDBDecimal<FromFieldType, ToFieldType>(vec_from[i], prec, scale, in_union, tp);
                }

                if constexpr (return_nullable)
                    block.getByPosition(result).column = ColumnNullable::create(std::move(col_to), std::move(col_null_map_to));
                else
                    block.getByPosition(result).column = std::move(col_to);
            }
            else
                throw Exception(
                    "Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function tidb_cast",
                    ErrorCodes::ILLEGAL_COLUMN);
        }
    }
};

class PreparedFunctionTiDBCast : public PreparedFunctionImpl
{
public:
    using WrapperType = std::function<void(Block &, const ColumnNumbers &, size_t, bool, const tipb::FieldType &, const Context &)>;

    PreparedFunctionTiDBCast(
        WrapperType && wrapper_function, const char * name_, bool in_union_, const tipb::FieldType & tidb_tp_, const Context & context_)
        : wrapper_function(std::move(wrapper_function)), name(name_), in_union(in_union_), tidb_tp(tidb_tp_), context(context_)
    {}

    String getName() const override { return name; }

protected:
    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result) override
    {
        ColumnNumbers new_arguments{arguments.front()};
        wrapper_function(block, new_arguments, result, in_union, tidb_tp, context);
    }

    bool useDefaultImplementationForConstants() const override { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {1}; }

private:
    WrapperType wrapper_function;
    const char * name;
    bool in_union;
    const tipb::FieldType & tidb_tp;
    const Context & context;
};

class FunctionTiDBCast final : public IFunctionBase
{
public:
    using WrapperType = std::function<void(Block &, const ColumnNumbers &, size_t, bool, const tipb::FieldType &, const Context &)>;
    using MonotonicityForRange = std::function<Monotonicity(const IDataType &, const Field &, const Field &)>;

    FunctionTiDBCast(const Context & context, const char * name, MonotonicityForRange && monotonicity_for_range,
        const DataTypes & argument_types, const DataTypePtr & return_type, bool in_union_, const tipb::FieldType & tidb_tp_)
        : context(context),
          name(name),
          monotonicity_for_range(monotonicity_for_range),
          argument_types(argument_types),
          return_type(return_type),
          in_union(in_union_),
          tidb_tp(tidb_tp_)
    {}

    const DataTypes & getArgumentTypes() const override { return argument_types; }
    const DataTypePtr & getReturnType() const override { return return_type; }

    PreparedFunctionPtr prepare(const Block & /*sample_block*/) const override
    {
        return std::make_shared<PreparedFunctionTiDBCast>(
            prepare(getArgumentTypes()[0], getReturnType()), name, in_union, tidb_tp, context);
    }

    String getName() const override { return name; }

    bool hasInformationAboutMonotonicity() const override
    {
        //return static_cast<bool>(monotonicity_for_range);
        return false;
    }

    Monotonicity getMonotonicityForRange(const IDataType & type, const Field & left, const Field & right) const override
    {
        return monotonicity_for_range(type, left, right);
    }

private:
    const Context & context;
    const char * name;
    MonotonicityForRange monotonicity_for_range;

    DataTypes argument_types;
    DataTypePtr return_type;

    bool in_union;
    const tipb::FieldType & tidb_tp;

    template <typename FromDataType, bool return_nullable>
    WrapperType createWrapper(const DataTypePtr & to_type) const
    {
        /// cast as int
        if (checkDataType<DataTypeUInt64>(to_type.get()))
            return [](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_, const tipb::FieldType & tidb_tp_,
                       const Context & context_) {
                TiDBConvertToInteger<FromDataType, DataTypeUInt64, return_nullable, true>::execute(
                    block, arguments, result, in_union_, tidb_tp_, context_);
            };
        if (checkDataType<DataTypeInt64>(to_type.get()))
            return [](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_, const tipb::FieldType & tidb_tp_,
                       const Context & context_) {
                TiDBConvertToInteger<FromDataType, DataTypeInt64, return_nullable, false>::execute(
                    block, arguments, result, in_union_, tidb_tp_, context_);
            };
        /// cast as decimal
        if (const auto decimal_type = checkAndGetDataType<DataTypeDecimal32>(to_type.get()))
            return [decimal_type](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_,
                       const tipb::FieldType & tidb_tp_, const Context & context_) {
                if (hasUnsignedFlag(tidb_tp_))
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal32::FieldType, return_nullable, true>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
                else
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal32::FieldType, return_nullable, false>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
            };
        if (const auto decimal_type = checkAndGetDataType<DataTypeDecimal64>(to_type.get()))
            return [decimal_type](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_,
                       const tipb::FieldType & tidb_tp_, const Context & context_) {
                if (hasUnsignedFlag(tidb_tp_))
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal64::FieldType, return_nullable, true>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
                else
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal64::FieldType, return_nullable, false>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
            };
        if (const auto decimal_type = checkAndGetDataType<DataTypeDecimal128>(to_type.get()))
            return [decimal_type](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_,
                       const tipb::FieldType & tidb_tp_, const Context & context_) {
                if (hasUnsignedFlag(tidb_tp_))
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal128::FieldType, return_nullable, true>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
                else
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal128::FieldType, return_nullable, false>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
            };
        if (const auto decimal_type = checkAndGetDataType<DataTypeDecimal256>(to_type.get()))
            return [decimal_type](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_,
                       const tipb::FieldType & tidb_tp_, const Context & context_) {
                if (hasUnsignedFlag(tidb_tp_))
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal256::FieldType, return_nullable, true>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
                else
                    TiDBConvertToDecimal<FromDataType, DataTypeDecimal256::FieldType, return_nullable, false>::execute(
                        block, arguments, result, decimal_type->getPrec(), decimal_type->getScale(), in_union_, tidb_tp_, context_);
            };
        /// cast as real
        if (checkDataType<DataTypeFloat64>(to_type.get()))
            return [](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_, const tipb::FieldType & tidb_tp_,
                       const Context & context_) {
                if (hasUnsignedFlag(tidb_tp_))
                {
                    TiDBConvertToFloat<FromDataType, return_nullable, true>::execute(
                        block, arguments, result, in_union_, tidb_tp_, context_);
                }
                else
                {
                    TiDBConvertToFloat<FromDataType, return_nullable, false>::execute(
                        block, arguments, result, in_union_, tidb_tp_, context_);
                }
            };
        /// cast as string
        if (checkDataType<DataTypeString>(to_type.get()))
            return [](Block & block, const ColumnNumbers & arguments, const size_t result, bool in_union_, const tipb::FieldType & tidb_tp_,
                       const Context & context_) {
                TiDBConvertToString<FromDataType, return_nullable>::execute(block, arguments, result, in_union_, tidb_tp_, context_);
            };
        /*
        if (checkDataType<DataTypeMyDate>(to_type.get()))
            return [] (Block & block, const ColumnNumbers & arguments, const size_t result, const Context & context1)
            {
                TiDBConvertImpl<FromDataType, DataTypeMyDate, return_nullable>::execute(block, arguments, result, context1);
            };
        if (checkDataType<DataTypeMyDateTime>(to_type.get()))
            return [] (Block & block, const ColumnNumbers & arguments, const size_t result, const Context & context1)
            {
                TiDBConvertImpl<FromDataType, DataTypeMyDateTime, return_nullable>::execute(block, arguments, result, context1);
            };
            */

        // todo support convert to duration/json type
        throw Exception{"Conversion to " + to_type->getName() + " is not supported", ErrorCodes::CANNOT_CONVERT_TYPE};
    }

    WrapperType createIdentityWrapper(const DataTypePtr &) const
    {
        return [](Block & block, const ColumnNumbers & arguments, const size_t result, bool, const tipb::FieldType &, const Context &) {
            block.getByPosition(result).column = block.getByPosition(arguments.front()).column;
        };
    }

    template <bool return_nullable>
    WrapperType createWrapper(const DataTypePtr & from_type, const DataTypePtr & to_type) const
    {
        if (from_type->equals(*to_type) && !from_type->isParametric() && !from_type->isString())
            return createIdentityWrapper(from_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeUInt8>(from_type.get()))
            return createWrapper<DataTypeUInt8, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeUInt16>(from_type.get()))
            return createWrapper<DataTypeUInt16, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeUInt32>(from_type.get()))
            return createWrapper<DataTypeUInt32, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeUInt64>(from_type.get()))
            return createWrapper<DataTypeUInt64, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeInt8>(from_type.get()))
            return createWrapper<DataTypeInt8, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeInt16>(from_type.get()))
            return createWrapper<DataTypeInt16, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeInt32>(from_type.get()))
            return createWrapper<DataTypeInt32, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeInt64>(from_type.get()))
            return createWrapper<DataTypeInt64, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeFloat32>(from_type.get()))
            return createWrapper<DataTypeFloat32, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeFloat64>(from_type.get()))
            return createWrapper<DataTypeFloat64, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeDecimal32>(from_type.get()))
            return createWrapper<DataTypeDecimal32, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeDecimal64>(from_type.get()))
            return createWrapper<DataTypeDecimal64, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeDecimal128>(from_type.get()))
            return createWrapper<DataTypeDecimal128, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeDecimal256>(from_type.get()))
            return createWrapper<DataTypeDecimal256, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeMyDate>(from_type.get()))
            return createWrapper<DataTypeMyDate, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeMyDateTime>(from_type.get()))
            return createWrapper<DataTypeMyDateTime, return_nullable>(to_type);
        if (const auto from_actual_type = checkAndGetDataType<DataTypeString>(from_type.get()))
            return createWrapper<DataTypeString, return_nullable>(to_type);

        // todo support convert to duration/json type
        throw Exception{
            "Conversion from " + from_type->getName() + " to " + to_type->getName() + " is not supported", ErrorCodes::CANNOT_CONVERT_TYPE};
    }

    WrapperType prepare(const DataTypePtr & from_type, const DataTypePtr & to_type) const
    {
        if (from_type->onlyNull())
        {
            return [](Block & block, const ColumnNumbers &, const size_t result, bool, const tipb::FieldType &, const Context &) {
                auto & res = block.getByPosition(result);
                res.column = res.type->createColumnConstWithDefaultValue(block.rows())->convertToFullColumnIfConst();
            };
        }

        DataTypePtr from_inner_type = removeNullable(from_type);
        DataTypePtr to_inner_type = removeNullable(to_type);

        return prepareImpl(from_inner_type, to_inner_type);
    }

    WrapperType prepareImpl(const DataTypePtr & from_type, const DataTypePtr & to_type) const
    {
        if (from_type->equals(*to_type))
            return createIdentityWrapper(from_type);
        bool return_nullable = to_type->isNullable();
        if (return_nullable)
            return createWrapper<true>(from_type, to_type);
        else
            return createWrapper<false>(from_type, to_type);
    }
};

class FunctionBuilderTiDBCast : public FunctionBuilderImpl
{
public:
    using MonotonicityForRange = FunctionCast::MonotonicityForRange;

    static constexpr auto name = "tidb_cast";
    static FunctionBuilderPtr create(const Context & context) { return std::make_shared<FunctionBuilderTiDBCast>(context); }

    FunctionBuilderTiDBCast(const Context & context) : context(context) {}

    String getName() const override { return name; }

    size_t getNumberOfArguments() const override { return 2; }
    void setInUnion(bool in_union_) { in_union = in_union_; }
    void setTiDBFieldType(const tipb::FieldType * tidb_tp_) { tidb_tp = tidb_tp_; }


protected:
    FunctionBasePtr buildImpl(
        const ColumnsWithTypeAndName & arguments, const DataTypePtr & return_type, std::shared_ptr<TiDB::ITiDBCollator>) const override
    {
        DataTypes data_types(arguments.size());

        for (size_t i = 0; i < arguments.size(); ++i)
            data_types[i] = arguments[i].type;

        auto monotonicity = getMonotonicityInformation(arguments.front().type, return_type.get());
        return std::make_shared<FunctionTiDBCast>(context, name, std::move(monotonicity), data_types, return_type, in_union, *tidb_tp);
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        const auto type_col = checkAndGetColumnConst<ColumnString>(arguments.back().column.get());
        if (!type_col)
            throw Exception(
                "Second argument to " + getName() + " must be a constant string describing type", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        return DataTypeFactory::instance().get(type_col->getValue<String>());
    }

private:
    // todo support monotonicity
    //template <typename DataType>
    //static auto monotonicityForType(const DataType * const)
    //{
    //    return FunctionTo<DataType>::Type::Monotonic::get;
    //}

    MonotonicityForRange getMonotonicityInformation(const DataTypePtr &, const IDataType *) const
    {
        /*
        if (const auto type = checkAndGetDataType<DataTypeUInt8>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeUInt16>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeUInt32>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeUInt64>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeInt8>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeInt16>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeInt32>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeInt64>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeFloat32>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeFloat64>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeDate>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeDateTime>(to_type))
            return monotonicityForType(type);
        else if (const auto type = checkAndGetDataType<DataTypeString>(to_type))
            return monotonicityForType(type);
        else if (from_type->isEnum())
        {
            if (const auto type = checkAndGetDataType<DataTypeEnum8>(to_type))
                return monotonicityForType(type);
            else if (const auto type = checkAndGetDataType<DataTypeEnum16>(to_type))
                return monotonicityForType(type);
        }
         */
        return {};
    }

    const Context & context;
    bool in_union;
    const tipb::FieldType * tidb_tp;
};

} // namespace DB