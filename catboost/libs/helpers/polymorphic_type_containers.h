#pragma once

#include "array_subset.h"
#include "dynamic_iterator.h"
#include "maybe_owning_array_holder.h"

#include <catboost/private/libs/index_range/index_range.h>

#include <library/threading/local_executor/local_executor.h>

#include <util/generic/cast.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/ymath.h>
#include <util/system/compiler.h>
#include <util/system/types.h>
#include <util/system/yassert.h>

#include <type_traits>


namespace NCB {

    template <class T>
    class ITypedArraySubset : public TThrRefBase {
    public:
        virtual ui32 GetSize() const = 0;

        virtual IDynamicBlockIteratorPtr<T> GetBlockIterator(ui32 offset = 0) const = 0;

        virtual TIntrusivePtr<ITypedArraySubset<T>> CloneWithNewSubsetIndexing(
            const TArraySubsetIndexing<ui32>* newSubsetIndexing
        ) const = 0;

        // f is a visitor function that will be repeatedly called with (index, srcIndex) arguments
        template <class F>
        void ForEach(F&& f) const {
            IDynamicBlockIteratorPtr<T> blockIterator = GetBlockIterator();

            ui32 idx = 0;
            while (auto block = blockIterator->Next()) {
                for (auto element : block) {
                    f(idx++, element);
                }
            }
        }

        /* f is a visitor function that will be repeatedly called with (index, srcIndex) arguments
         * for TRangesSubset block sizes might not be equal to approximateBlockSize because
         * block sizes might not be divisible by approximateBlockSize, that's why 'approximate'
         * is in the name of this block size parameter
         * if approximateBlockSize is undefined divide data approximately evenly between localExecutor
         * threads
         */
        template <class F>
        void ParallelForEach(
            F&& f,
            NPar::TLocalExecutor* localExecutor,
            TMaybe<ui32> approximateBlockSize = Nothing()
        ) const {
            TVector<IDynamicBlockIteratorPtr<T>> subRangeIterators;
            TVector<ui32> subRangeStarts;

            CreateSubRangesIterators(
                *localExecutor,
                approximateBlockSize,
                &subRangeIterators,
                &subRangeStarts
            );

            localExecutor->ExecRangeWithThrow(
                [&] (int subRangeIdx) {
                    IDynamicBlockIteratorPtr<T> subRangeIterator = std::move(subRangeIterators[subRangeIdx]);
                    ui32 idx = subRangeStarts[subRangeIdx];

                    while (auto block = subRangeIterator->Next()) {
                        for (auto element : block) {
                            f(idx++, element);
                        }
                    }
                },
                0,
                subRangeIterators.size(),
                NPar::TLocalExecutor::WAIT_COMPLETE
            );
        }

        /* predicate is a visitor function that returns bool
         * it will be repeatedly called with (index, srcIndex) arguments
         * until it returns true or all elements are iterated over
         *
         *  @returns true if a pair of (index, subIndex) for which predicate returned true was found
         *  and false otherwise
         */
        template <class TPredicate>
        bool Find(const TPredicate& predicate) const {
            IDynamicBlockIteratorPtr<T> blockIterator = GetBlockIterator();

            ui32 idx = 0;
            while (auto block = blockIterator->Next()) {
                for (auto element : block) {
                    if (predicate(idx++, element)) {
                        return true;
                    }
                }
            }
            return false;
        }

    protected:
        virtual void CreateSubRangesIterators(
            const NPar::TLocalExecutor& localExecutor,
            TMaybe<ui32> approximateBlockSize,
            TVector<IDynamicBlockIteratorPtr<T>>* subRangeIterators,
            TVector<ui32>* subRangeStarts
        ) const = 0;
    };

    template <class T>
    using ITypedArraySubsetPtr = TIntrusivePtr<ITypedArraySubset<T>>;


    template <class TInterfaceValue, class TStoredValue>
    class TTypeCastArraySubset final : public ITypedArraySubset<TInterfaceValue> {
    public:
        using TData = TMaybeOwningConstArrayHolder<TStoredValue>;

    public:
        TTypeCastArraySubset(
            TMaybeOwningConstArrayHolder<TStoredValue> data,
            const TArraySubsetIndexing<ui32>* subsetIndexing
        )
            : Data(std::move(data))
            , SubsetIndexing(subsetIndexing)
        {
            Y_ASSERT(SubsetIndexing);
        }

        ui32 GetSize() const override {
            return SubsetIndexing->Size();
        }

        IDynamicBlockIteratorPtr<TInterfaceValue> GetBlockIterator(ui32 offset = 0) const override {
            const ui32 size = GetSize();
            const ui32 remainingSize = size - offset;

            switch (SubsetIndexing->index()) {
                case TVariantIndexV<TFullSubset<ui32>, TArraySubsetIndexing<ui32>::TBase>:
                    return MakeHolder<
                            TArraySubsetBlockIterator<TInterfaceValue, TData, TRangeIterator<ui32>>
                        >(
                            Data,
                            remainingSize,
                            TRangeIterator<ui32>(TIndexRange<ui32>(offset, GetSize()))
                        );
                case TVariantIndexV<TRangesSubset<ui32>, TArraySubsetIndexing<ui32>::TBase>:
                    return MakeHolder<
                            TArraySubsetBlockIterator<TInterfaceValue, TData, TRangesSubsetIterator<ui32>>
                        >(
                            Data,
                            remainingSize,
                            TRangesSubsetIterator<ui32>(SubsetIndexing->Get<TRangesSubset<ui32>>(), offset)
                        );
                case TVariantIndexV<TIndexedSubset<ui32>, TArraySubsetIndexing<ui32>::TBase>:
                    {
                        using TIterator = TStaticIteratorRangeAsDynamic<const ui32*>;

                        const auto& indexedSubset = SubsetIndexing->Get<TIndexedSubset<ui32>>();

                        return MakeHolder<TArraySubsetBlockIterator<TInterfaceValue, TData, TIterator>>(
                            Data,
                            remainingSize,
                            TIterator(indexedSubset.begin() + offset, indexedSubset.end())
                        );
                    }
                default:
                    Y_UNREACHABLE();
            }
            Y_UNREACHABLE();
        }

        TIntrusivePtr<ITypedArraySubset<TInterfaceValue>> CloneWithNewSubsetIndexing(
            const TArraySubsetIndexing<ui32>* newSubsetIndexing
        ) const override {
            return MakeIntrusive<TTypeCastArraySubset<TInterfaceValue, TStoredValue>>(
                Data,
                newSubsetIndexing
            );
        }

    protected:
        void CreateSubRangesIterators(
            const TFullSubset<ui32>& fullSubset,
            ui32 approximateBlockSize,
            TVector<IDynamicBlockIteratorPtr<TInterfaceValue>>* subRangeIterators,
            TVector<ui32>* subRangeStarts
        ) const {
            TSimpleIndexRangesGenerator<ui32> indexRanges(
                TIndexRange<ui32>(fullSubset.Size),
                approximateBlockSize
            );
            subRangeIterators->reserve(indexRanges.RangesCount());
            subRangeStarts->reserve(indexRanges.RangesCount());

            for (auto subRangeIdx : xrange(indexRanges.RangesCount())) {
                auto subRange = indexRanges.GetRange(subRangeIdx);
                subRangeIterators->push_back(
                    MakeHolder<
                        TArraySubsetBlockIterator<TInterfaceValue, TData, TRangeIterator<ui32>>
                    >(
                        Data,
                        subRange.GetSize(),
                        TRangeIterator<ui32>(subRange)
                    )
                );
                subRangeStarts->push_back(subRange.Begin);
            }
        }

        void CreateSubRangesIterators(
            const TRangesSubset<ui32>& rangesSubset,
            ui32 approximateBlockSize,
            TVector<IDynamicBlockIteratorPtr<TInterfaceValue>>* subRangeIterators,
            TVector<ui32>* subRangeStarts
        ) const {
            // TODO(akhropov): don't join small blocks (rare case in practice) for simplicity
            for (const auto& block : rangesSubset.Blocks) {
                TSimpleIndexRangesGenerator<ui32> indexRanges(
                    TIndexRange<ui32>(block.GetSize()),
                    approximateBlockSize
                );

                for (auto subRangeIdx : xrange(indexRanges.RangesCount())) {
                    auto subRange = indexRanges.GetRange(subRangeIdx);
                    subRangeIterators->push_back(
                        MakeHolder<
                            TArraySubsetBlockIterator<TInterfaceValue, TData, TRangesSubsetIterator<ui32>>
                        >(
                            Data,
                            subRange.GetSize(),
                            TRangesSubsetIterator<ui32>(
                                &block,
                                subRange.Begin,
                                &block + 1,
                                subRange.End
                            )
                        )
                    );
                    subRangeStarts->push_back(block.DstBegin + subRange.Begin);
                }
            }
        }

        void CreateSubRangesIterators(
            const TIndexedSubset<ui32>& indexedSubset,
            ui32 approximateBlockSize,
            TVector<IDynamicBlockIteratorPtr<TInterfaceValue>>* subRangeIterators,
            TVector<ui32>* subRangeStarts
        ) const {
            TSimpleIndexRangesGenerator<ui32> indexRanges(
                TIndexRange<ui32>(GetSize()),
                approximateBlockSize
            );
            subRangeIterators->reserve(indexRanges.RangesCount());
            subRangeStarts->reserve(indexRanges.RangesCount());

            using TIterator = TStaticIteratorRangeAsDynamic<const ui32*>;
            const ui32* indexedSubsetBegin = indexedSubset.data();

            for (auto subRangeIdx : xrange(indexRanges.RangesCount())) {
                auto subRange = indexRanges.GetRange(subRangeIdx);
                subRangeIterators->push_back(
                    MakeHolder<
                        TArraySubsetBlockIterator<TInterfaceValue, TData, TIterator>
                    >(
                        Data,
                        subRange.GetSize(),
                        TIterator(indexedSubsetBegin + subRange.Begin, indexedSubsetBegin + subRange.End)
                    )
                );
                subRangeStarts->push_back(subRange.Begin);
            }
        }

        void CreateSubRangesIterators(
            const NPar::TLocalExecutor& localExecutor,
            TMaybe<ui32> approximateBlockSize,
            TVector<IDynamicBlockIteratorPtr<TInterfaceValue>>* subRangeIterators,
            TVector<ui32>* subRangeStarts
        ) const override {
            const ui32 size = GetSize();
            if (!size) {
                subRangeIterators->clear();
                subRangeStarts->clear();
                return;
            }

            ui32 definedApproximateBlockSize
                = approximateBlockSize ?
                    *approximateBlockSize
                    : CeilDiv(size, SafeIntegerCast<ui32>(localExecutor.GetThreadCount()) + 1);

            Visit(
                [&] (const auto& subsetIndexingAlternative) {
                    CreateSubRangesIterators(
                        subsetIndexingAlternative,
                        definedApproximateBlockSize,
                        subRangeIterators,
                        subRangeStarts
                    );
                },
                *SubsetIndexing
            );
        }

        const TData& GetData() const {
            return Data;
        }

        const TArraySubsetIndexing<ui32>* GetSubsetIndexing() const {
            return SubsetIndexing;
        }

    private:
        TData Data;
        const TArraySubsetIndexing<ui32>* SubsetIndexing;
    };


    template <class T>
    class ITypedSequence : public TThrRefBase {
    public:
        virtual int operator&(IBinSaver& binSaver) = 0;

        // comparison is strict by default, useful for unit tests
        bool operator==(const ITypedSequence<T>& rhs) const {
            return EqualTo(rhs, /*strict*/ true);
        }

        // if strict is true compare bit-by-bit, else compare values
        virtual bool EqualTo(const ITypedSequence<T>& rhs, bool strict = true) const = 0;

        virtual ui32 GetSize() const = 0;

        virtual IDynamicBlockWithExactIteratorPtr<T> GetBlockIterator(TIndexRange<ui32> indexRange) const = 0;

        IDynamicBlockWithExactIteratorPtr<T> GetBlockIterator() const {
            return GetBlockIterator(TIndexRange<ui32>(GetSize()));
        }

        virtual TIntrusivePtr<ITypedArraySubset<T>> GetSubset(
            const TArraySubsetIndexing<ui32>* subsetIndexing
        ) const = 0;

        // f is a visitor function that will be repeatedly called with (element) argument
        template <class F>
        void ForEach(F&& f) const {
            IDynamicBlockIteratorPtr<T> blockIterator = GetBlockIterator();
            while (auto block = blockIterator->Next()) {
                for (auto element : block) {
                    f(element);
                }
            }
        }
    };

    template <class T>
    using ITypedSequencePtr = TIntrusivePtr<ITypedSequence<T>>;


    template <class TInterfaceValue, class TStoredValue>
    class TTypeCastArrayHolder final : public ITypedSequence<TInterfaceValue> {
    public:
        explicit TTypeCastArrayHolder(TMaybeOwningConstArrayHolder<TStoredValue> values)
            : Values(std::move(values))
        {}

        explicit TTypeCastArrayHolder(TVector<TStoredValue>&& values)
            : Values(TMaybeOwningConstArrayHolder<TStoredValue>::CreateOwning(std::move(values)))
        {}

        int operator&(IBinSaver& binSaver) override {
            binSaver.Add(0, &Values);
            return 0;
        }

        bool EqualTo(const ITypedSequence<TInterfaceValue>& rhs, bool strict = true) const override {
            if (strict) {
                if (const auto* rhsAsThisType
                        = dynamic_cast<const TTypeCastArrayHolder<TInterfaceValue, TStoredValue>*>(&rhs))
                {
                    return Values == rhsAsThisType->Values;
                } else {
                    return false;
                }
            } else {
                return AreBlockedSequencesEqual<TInterfaceValue, TInterfaceValue>(
                    ITypedSequence<TInterfaceValue>::GetBlockIterator(),
                    rhs.ITypedSequence<TInterfaceValue>::GetBlockIterator()
                );
            }
        }

        ui32 GetSize() const override {
            return SafeIntegerCast<ui32>(Values.GetSize());
        }

        IDynamicBlockWithExactIteratorPtr<TInterfaceValue> GetBlockIterator(
            TIndexRange<ui32> indexRange
        ) const override {
            TConstArrayRef<TStoredValue> subRangeArrayRef(
                Values.begin() + indexRange.Begin,
                Values.begin() + indexRange.End
            );
            if constexpr (std::is_same_v<TInterfaceValue, TStoredValue>) {
                return MakeHolder<TArrayBlockIterator<TInterfaceValue>>(subRangeArrayRef);
            } else {
                return MakeHolder<TTypeCastingArrayBlockIterator<TInterfaceValue, TStoredValue>>(
                    subRangeArrayRef
                );
            }
        }

        TIntrusivePtr<ITypedArraySubset<TInterfaceValue>> GetSubset(
            const TArraySubsetIndexing<ui32>* subsetIndexing
        ) const override {
            return MakeIntrusive<TTypeCastArraySubset<TInterfaceValue, TStoredValue>>(
                Values,
                subsetIndexing
            );
        }

    private:
        TMaybeOwningConstArrayHolder<TStoredValue> Values;
    };


    // for Cython where MakeIntrusive cannot be used
    template <class TInterfaceValue, class TStoredValue>
    ITypedSequencePtr<TInterfaceValue> MakeTypeCastArrayHolder(
        TMaybeOwningConstArrayHolder<TStoredValue> values
    ) {
        return MakeIntrusive<TTypeCastArrayHolder<TInterfaceValue, TStoredValue>>(std::move(values));
    }


    template <class TInterfaceValue, class TStoredValue>
    ITypedSequencePtr<TInterfaceValue> MakeNonOwningTypeCastArrayHolder(
        const TStoredValue* begin,
        const TStoredValue* end
    ) {
        return MakeIntrusive<TTypeCastArrayHolder<TInterfaceValue, TStoredValue>>(
            TMaybeOwningConstArrayHolder<TStoredValue>::CreateNonOwning(
                TConstArrayRef<TStoredValue>(begin, end)
            )
        );
    }

    // for Cython where MakeHolder cannot be used
    template <class TInterfaceValue, class TStoredValue>
    ITypedSequencePtr<TInterfaceValue> MakeTypeCastArrayHolderFromVector(TVector<TStoredValue>& values) {
        return MakeIntrusive<TTypeCastArrayHolder<TInterfaceValue, TStoredValue>>(
            TMaybeOwningConstArrayHolder<TStoredValue>::CreateOwning(std::move(values))
        );
    }
}
