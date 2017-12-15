// MIT License
//
// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <algorithm>
#include <functional>
#include <iostream>
#include <type_traits>
#include <vector>
#include <utility>

// Google Test
#include <gtest/gtest.h>
// HC API
#include <hcc/hc.hpp>
// rocPRIM
#include <block/block_radix_sort.hpp>

#include "test_utils.hpp"

namespace rp = rocprim;

template<
    class Key,
    class Value,
    unsigned int BlockSize,
    unsigned int ItemsPerThread,
    bool Descending = false,
    unsigned int StartBit = 0,
    unsigned int EndBit = sizeof(Key) * 8
>
struct params
{
    using key_type = Key;
    using value_type = Value;
    static constexpr unsigned int block_size = BlockSize;
    static constexpr unsigned int items_per_thread = ItemsPerThread;
    static constexpr bool descending = Descending;
    static constexpr unsigned int start_bit = StartBit;
    static constexpr unsigned int end_bit = EndBit;
};

template<class Params>
class RocprimBlockRadixSort : public ::testing::Test {
public:
    using params = Params;
};

typedef ::testing::Types<
    // Power of 2 Blocksize
    params<unsigned int, int, 64U, 1>,
    params<int, int, 128U, 1>,
    params<unsigned int, int, 256U, 1>,
    params<unsigned short, char, 1024U, 1, true>,

    // Non-power of 2 Blocksize
    params<double, unsigned int, 65U, 1>,
    params<float, int, 37U, 1>,
    params<long long, char, 510U, 1, true>,
    params<unsigned int, long long, 162U, 1>,
    params<unsigned char, float, 255U, 1>,

    // Power of 2 Blocksize and ItemsPerThread > 1
    params<float, char, 64U, 2, true>,
    params<int, short, 128U, 4>,
    params<unsigned short, char, 256U, 7>,

    // Non-power of 2 Blocksize and ItemsPerThread > 1
    params<double, int, 33U, 5>,
    params<char, double, 464U, 2>,
    params<unsigned short, int, 100U, 3>,
    params<short, int, 234U, 9>,

    // StartBits and EndBits
    params<unsigned long long, char, 64U, 1, false, 8, 20>,
    params<unsigned short, int, 102U, 3, true, 4, 10>,
    params<unsigned int, short, 162U, 2, true, 3, 12>
> Params;

TYPED_TEST_CASE(RocprimBlockRadixSort, Params);

template<class Key, bool Descending, unsigned int StartBit, unsigned int EndBit>
struct key_comparator
{
    static_assert(std::is_unsigned<Key>::value, "Test supports start and bits only for unsigned integers");

    bool operator()(const Key& lhs, const Key& rhs)
    {
        auto mask = (1ull << (EndBit - StartBit)) - 1;
        auto l = (static_cast<unsigned long long>(lhs) >> StartBit) & mask;
        auto r = (static_cast<unsigned long long>(rhs) >> StartBit) & mask;
        return Descending ? (r < l) : (l < r);
    }
};

template<class Key, bool Descending>
struct key_comparator<Key, Descending, 0, sizeof(Key) * 8>
{
    bool operator()(const Key& lhs, const Key& rhs)
    {
        return Descending ? (rhs < lhs) : (lhs < rhs);
    }
};

template<class Key, class Value, bool Descending, unsigned int StartBit, unsigned int EndBit>
struct key_value_comparator
{
    bool operator()(const std::pair<Key, Value>& lhs, const std::pair<Key, Value>& rhs)
    {
        return key_comparator<Key, Descending, StartBit, EndBit>()(lhs.first, rhs.first);
    }
};

TYPED_TEST(RocprimBlockRadixSort, SortKeys)
{
    hc::accelerator acc;

    using Key = typename TestFixture::params::key_type;
    constexpr size_t block_size = TestFixture::params::block_size;
    constexpr size_t items_per_thread = TestFixture::params::items_per_thread;
    constexpr bool descending = TestFixture::params::descending;
    constexpr unsigned int start_bit = TestFixture::params::start_bit;
    constexpr unsigned int end_bit = TestFixture::params::end_bit;
    constexpr size_t items_per_block = block_size * items_per_thread;
    // Given block size not supported
    if(block_size > get_max_tile_size(acc))
    {
        return;
    }

    const size_t size = items_per_block * 1134;
    // Generate data
    std::vector<Key> key_output;
    if(std::is_floating_point<Key>::value)
        key_output = get_random_data<Key>(size, (Key)-1000, (Key)+1000);
    else
        key_output = get_random_data<Key>(size, std::numeric_limits<Key>::min(), std::numeric_limits<Key>::max());

    // Calulcate expected results on host
    std::vector<Key> expected(key_output);
    for(size_t i = 0; i < size / items_per_block; i++)
    {
        std::stable_sort(
            expected.begin() + (i * items_per_block),
            expected.begin() + ((i + 1) * items_per_block),
            key_comparator<Key, descending, start_bit, end_bit>()
        );
    }

    hc::array_view<Key, 1> d_key_output(size, key_output.data());
    hc::parallel_for_each(
        acc.get_default_view(),
        hc::extent<1>(size / items_per_thread).tile(block_size),
        [=](hc::tiled_index<1> idx) [[hc]]
        {
            const unsigned int thread_id = idx.global[0];

            Key keys[items_per_thread];
            for(unsigned int i = 0; i < items_per_thread; i++)
            {
                keys[i] = d_key_output[thread_id * items_per_thread + i];
            }

            rp::block_radix_sort<Key, block_size, items_per_thread> bsort;
            if(descending)
                bsort.sort_desc(keys, start_bit, end_bit);
            else
                bsort.sort(keys, start_bit, end_bit);

            for(unsigned int i = 0; i < items_per_thread; i++)
            {
                d_key_output[thread_id * items_per_thread + i] = keys[i];
            }
        }
    );

    d_key_output.synchronize();
    for(size_t i = 0; i < size; i++)
    {
        ASSERT_EQ(key_output[i], expected[i]);
    }
}

TYPED_TEST(RocprimBlockRadixSort, SortKeysValues)
{
    hc::accelerator acc;

    using Key = typename TestFixture::params::key_type;
    using Value = typename TestFixture::params::value_type;
    constexpr size_t block_size = TestFixture::params::block_size;
    constexpr size_t items_per_thread = TestFixture::params::items_per_thread;
    constexpr bool descending = TestFixture::params::descending;
    constexpr unsigned int start_bit = TestFixture::params::start_bit;
    constexpr unsigned int end_bit = TestFixture::params::end_bit;
    constexpr size_t items_per_block = block_size * items_per_thread;
    // Given block size not supported
    if(block_size > get_max_tile_size(acc))
    {
        return;
    }

    const size_t size = items_per_block * 1134;
    // Generate data
    std::vector<Key> key_output;
    if(std::is_floating_point<Key>::value)
        key_output = get_random_data<Key>(size, (Key)-1000, (Key)+1000);
    else
        key_output = get_random_data<Key>(size, std::numeric_limits<Key>::min(), std::numeric_limits<Key>::max());

    std::vector<Value> value_output;
    if(std::is_floating_point<Value>::value)
        value_output = get_random_data<Value>(size, (Value)-1000, (Value)+1000);
    else
        value_output = get_random_data<Value>(size, std::numeric_limits<Value>::min(), std::numeric_limits<Value>::max());

    using key_value = std::pair<Key, Value>;

    // Calulcate expected results on host
    std::vector<key_value> expected(size);
    for(size_t i = 0; i < size; i++)
    {
        expected[i] = key_value(key_output[i], value_output[i]);
    }

    for(size_t i = 0; i < size / items_per_block; i++)
    {
        std::stable_sort(
            expected.begin() + (i * items_per_block),
            expected.begin() + ((i + 1) * items_per_block),
            key_value_comparator<Key, Value, descending, start_bit, end_bit>()
        );
    }

    hc::array_view<Key, 1> d_key_output(size, key_output.data());
    hc::array_view<Value, 1> d_value_output(size, value_output.data());
    hc::parallel_for_each(
        acc.get_default_view(),
        hc::extent<1>(size / items_per_thread).tile(block_size),
        [=](hc::tiled_index<1> idx) [[hc]]
        {
            const unsigned int thread_id = idx.global[0];

            Key keys[items_per_thread];
            Value values[items_per_thread];
            for(unsigned int i = 0; i < items_per_thread; i++)
            {
                keys[i] = d_key_output[thread_id * items_per_thread + i];
                values[i] = d_value_output[thread_id * items_per_thread + i];
            }

            rp::block_radix_sort<Key, block_size, items_per_thread, Value> bsort;
            if(descending)
                bsort.sort_desc(keys, values, start_bit, end_bit);
            else
                bsort.sort(keys, values, start_bit, end_bit);

            for(unsigned int i = 0; i < items_per_thread; i++)
            {
                d_key_output[thread_id * items_per_thread + i] = keys[i];
                d_value_output[thread_id * items_per_thread + i] = values[i];
            }
        }
    );

    d_key_output.synchronize();
    d_value_output.synchronize();
    for(size_t i = 0; i < size; i++)
    {
        ASSERT_EQ(key_output[i], expected[i].first);
        ASSERT_EQ(value_output[i], expected[i].second);
    }
}
