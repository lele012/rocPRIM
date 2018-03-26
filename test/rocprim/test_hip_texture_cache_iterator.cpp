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

#include <iostream>
#include <vector>
#include <algorithm>
#include <type_traits>

// Google Test
#include <gtest/gtest.h>
// HIP API
#include <hip/hip_runtime.h>
#include <hip/hip_hcc.h>
// rocPRIM API
#include <rocprim/rocprim.hpp>

#include "test_utils.hpp"

#define HIP_CHECK(error)         \
    ASSERT_EQ(static_cast<hipError_t>(error),hipSuccess)

// Params for tests
template<class InputType>
struct RocprimTextureCacheIteratorParams
{
    using input_type = InputType;
};

template<class Params>
class RocprimTextureCacheIteratorTests : public ::testing::Test
{
public:
    using input_type = typename Params::input_type;
    const bool debug_synchronous = false;
};

typedef ::testing::Types<
    // -----------------------------------------------------------------------
    //
    // -----------------------------------------------------------------------
    RocprimTextureCacheIteratorParams<int>,
    RocprimTextureCacheIteratorParams<unsigned int>,
    RocprimTextureCacheIteratorParams<unsigned char>,
    RocprimTextureCacheIteratorParams<float>
> RocprimTextureCacheIteratorTestsParams;

TYPED_TEST_CASE(RocprimTextureCacheIteratorTests, RocprimTextureCacheIteratorTestsParams);

template<class T>
struct transform
{
    __device__ __host__
    constexpr T operator()(const T& a) const
    {
        return 5 + a;
    }
};

TYPED_TEST(RocprimTextureCacheIteratorTests, Transform)
{
    using T = typename TestFixture::input_type;
    using Iterator = typename rocprim::texture_cache_iterator<T>;
    const bool debug_synchronous = TestFixture::debug_synchronous;

    const size_t size = 1024;

    hipStream_t stream = 0; // default

    std::vector<T> input = test_utils::get_random_data<T>(size, 1, 200);

    std::vector<T> output(size);
    T * d_input;
    T * d_output;
    HIP_CHECK(hipMalloc(&d_input, input.size() * sizeof(T)));
    HIP_CHECK(hipMalloc(&d_output, output.size() * sizeof(T)));
    HIP_CHECK(
        hipMemcpy(
            d_input, input.data(),
            input.size() * sizeof(T),
            hipMemcpyHostToDevice
        )
    );
    HIP_CHECK(hipDeviceSynchronize());
    
    Iterator x;
    x.bind_texture(d_input, sizeof(T) * input.size());

    // Calculate expected results on host
    std::vector<T> expected(size);
    std::transform(
        input.begin(),
        input.end(),
        expected.begin(),
        transform<T>()
    );

    // Run
    HIP_CHECK(
        rocprim::transform(
            x, d_output, size,
            transform<T>(), stream, debug_synchronous
        )
    );
    HIP_CHECK(hipPeekAtLastError());
    HIP_CHECK(hipDeviceSynchronize());

    // Copy output to host
    HIP_CHECK(
        hipMemcpy(
            output.data(), d_output,
            output.size() * sizeof(T),
            hipMemcpyDeviceToHost
        )
    );
    HIP_CHECK(hipDeviceSynchronize());

    // Validating results
    for(size_t i = 0; i < output.size(); i++)
    {
        SCOPED_TRACE(testing::Message() << "where index = " << i);
        if(std::is_integral<T>::value)
        {
            ASSERT_EQ(output[i], expected[i]);
        }
        else if(std::is_floating_point<T>::value)
        {
            auto tolerance = std::max<T>(std::abs(0.1f * expected[i]), T(0.01f));
            ASSERT_NEAR(output[i], expected[i], tolerance);
        }
    }

    x.unbind_texture();
    hipFree(d_input);
    hipFree(d_output);
}
