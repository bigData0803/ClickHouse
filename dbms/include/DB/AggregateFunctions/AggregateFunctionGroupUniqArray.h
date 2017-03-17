#pragma once

#include <DB/IO/WriteHelpers.h>
#include <DB/IO/ReadHelpers.h>

#include <DB/DataTypes/DataTypeArray.h>
#include <DB/DataTypes/DataTypesNumberFixed.h>
#include <DB/DataTypes/DataTypeString.h>

#include <DB/Columns/ColumnArray.h>

#include <DB/Common/HashTable/HashSet.h>

#include <DB/AggregateFunctions/AggregateFunctionGroupArray.h>

#define AGGREGATE_FUNCTION_GROUP_ARRAY_UNIQ_MAX_SIZE 0xFFFFFF


namespace DB
{


template <typename T>
struct AggregateFunctionGroupUniqArrayData
{
	/// When creating, the hash table must be small.
	using Set = HashSet<
		T,
		DefaultHash<T>,
		HashTableGrower<4>,
		HashTableAllocatorWithStackMemory<sizeof(T) * (1 << 4)>
	>;

	Set value;
};


/// Puts all values to the hash set. Returns an array of unique values. Implemented for numeric types.
template <typename T>
class AggregateFunctionGroupUniqArray
	: public IUnaryAggregateFunction<AggregateFunctionGroupUniqArrayData<T>, AggregateFunctionGroupUniqArray<T>>
{
private:
	using State = AggregateFunctionGroupUniqArrayData<T>;

public:
	String getName() const override { return "groupUniqArray"; }

	DataTypePtr getReturnType() const override
	{
		return std::make_shared<DataTypeArray>(std::make_shared<typename DataTypeFromFieldType<T>::Type>());
	}

	void setArgument(const DataTypePtr & argument)
	{
	}


	void addImpl(AggregateDataPtr place, const IColumn & column, size_t row_num, Arena *) const
	{
		this->data(place).value.insert(static_cast<const ColumnVector<T> &>(column).getData()[row_num]);
	}

	void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena * arena) const override
	{
		this->data(place).value.merge(this->data(rhs).value);
	}

	void serialize(ConstAggregateDataPtr place, WriteBuffer & buf) const override
	{
		auto & set = this->data(place).value;
		size_t size = set.size();
		writeVarUInt(size, buf);
		for (auto & elem : set)
			writeIntBinary(elem, buf);
	}

	void deserialize(AggregateDataPtr place, ReadBuffer & buf, Arena *) const override
	{
		this->data(place).value.read(buf);
	}

	void insertResultInto(ConstAggregateDataPtr place, IColumn & to) const override
	{
		ColumnArray & arr_to = static_cast<ColumnArray &>(to);
		ColumnArray::Offsets_t & offsets_to = arr_to.getOffsets();

		const typename State::Set & set = this->data(place).value;
		size_t size = set.size();

		offsets_to.push_back((offsets_to.size() == 0 ? 0 : offsets_to.back()) + size);

		typename ColumnVector<T>::Container_t & data_to = static_cast<ColumnVector<T> &>(arr_to.getData()).getData();
		size_t old_size = data_to.size();
		data_to.resize(old_size + size);

		size_t i = 0;
		for (auto it = set.begin(); it != set.end(); ++it, ++i)
			data_to[old_size + i] = *it;
	}
};


/// Generic implementation, it uses serialized representation as object descriptor.
struct AggreagteFunctionGroupUniqArrayGenericData
{
	static constexpr size_t INIT_ELEMS = 2; /// adjustable
	static constexpr size_t ELEM_SIZE = sizeof(HashSetCellWithSavedHash<StringRef, StringRefHash>);
	using Set = HashSetWithSavedHash<StringRef, StringRefHash, HashTableGrower<INIT_ELEMS>, HashTableAllocatorWithStackMemory<INIT_ELEMS * ELEM_SIZE>>;

	Set value;
};

/** Template parameter with true value should be used for columns that store their elements in memory continuously.
 *  For such columns groupUniqArray() can be implemented more efficently (especially for small numeric arrays).
 */
template <bool is_plain_column = false>
class AggreagteFunctionGroupUniqArrayGeneric : public IUnaryAggregateFunction<AggreagteFunctionGroupUniqArrayGenericData, AggreagteFunctionGroupUniqArrayGeneric<is_plain_column>>
{
	DataTypePtr input_data_type;

	using State = AggreagteFunctionGroupUniqArrayGenericData;

	static StringRef getSerialization(const IColumn & column, size_t row_num, Arena & arena);

	static void deserializeAndInsert(StringRef str, IColumn & data_to);

public:

	String getName() const override { return "groupUniqArray"; }

	void setArgument(const DataTypePtr & argument)
	{
		input_data_type = argument;
	}

	DataTypePtr getReturnType() const override
	{
		return std::make_shared<DataTypeArray>(input_data_type->clone());
	}

	bool allocatesMemoryInArena() const override
	{
		return true;
	}

	void serialize(ConstAggregateDataPtr place, WriteBuffer & buf) const override
	{
		auto & set = this->data(place).value;
		writeVarUInt(set.size(), buf);

		for (const auto & elem : set)
		{
			writeStringBinary(elem, buf);
		}
	}

	void deserialize(AggregateDataPtr place, ReadBuffer & buf, Arena * arena) const override
	{
		auto & set = this->data(place).value;
		size_t size;
		readVarUInt(size, buf);
		//TODO: set.reserve(size);

		for (size_t i = 0; i < size; i++)
		{
			set.insert(readStringBinaryInto(*arena, buf));
		}
	}

	void addImpl(AggregateDataPtr place, const IColumn & column, size_t row_num, Arena * arena) const
	{
		auto & set = this->data(place).value;

		bool inserted;
		State::Set::iterator it;

		StringRef str_serialized = getSerialization(column, row_num, *arena);
		set.emplace(str_serialized, it, inserted);

		if (!is_plain_column)
		{
			if (!inserted)
				arena->rollback(str_serialized.size);
		}
		else
		{
			if (inserted)
				it->data = arena->insert(str_serialized.data, str_serialized.size);
		}
	}

	void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena * arena) const override
	{
		auto & cur_set = this->data(place).value;
		auto & rhs_set = this->data(rhs).value;

		bool inserted;
		State::Set::iterator it;
		for (auto & rhs_elem : rhs_set)
		{
			cur_set.emplace(rhs_elem, it, inserted);
			if (inserted)
				it->data = arena->insert(it->data, it->size);
		}
	}

	void insertResultInto(ConstAggregateDataPtr place, IColumn & to) const override
	{
		ColumnArray & arr_to = static_cast<ColumnArray &>(to);
		ColumnArray::Offsets_t & offsets_to = arr_to.getOffsets();
		IColumn & data_to = arr_to.getData();

		auto & set = this->data(place).value;
		offsets_to.push_back((offsets_to.size() == 0 ? 0 : offsets_to.back()) + set.size());

		for (auto & elem : set)
		{
			deserializeAndInsert(elem, data_to);
		}
	}
};


template <>
inline StringRef AggreagteFunctionGroupUniqArrayGeneric<false>::getSerialization(const IColumn & column, size_t row_num, Arena & arena)
{
	const char * begin = nullptr;
	return column.serializeValueIntoArena(row_num, arena, begin);
}

template <>
inline StringRef AggreagteFunctionGroupUniqArrayGeneric<true>::getSerialization(const IColumn & column, size_t row_num, Arena &)
{
	return column.getDataAt(row_num);
}

template <>
inline void AggreagteFunctionGroupUniqArrayGeneric<false>::deserializeAndInsert(StringRef str, IColumn & data_to)
{
	data_to.deserializeAndInsertFromArena(str.data);
}

template <>
inline void AggreagteFunctionGroupUniqArrayGeneric<true>::deserializeAndInsert(StringRef str, IColumn & data_to)
{
	data_to.insertData(str.data, str.size);
}


#undef AGGREGATE_FUNCTION_GROUP_ARRAY_UNIQ_MAX_SIZE

}