//
// Copyright (c) 2017 The Khronos Group Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "common.h"
#include "function_list.h"
#include "test_functions.h"
#include "utility.h"

#include <cinttypes>
#include <cstring>

namespace {

int BuildKernel(const char *name, int vectorSize, cl_kernel *k, cl_program *p,
                bool relaxedMode)
{
    auto kernel_name = GetKernelName(vectorSize);
    auto source = GetUnaryKernel(kernel_name, name, ParameterType::Float,
                                 ParameterType::UInt, vectorSize);
    std::array<const char *, 1> sources{ source.c_str() };
    return MakeKernel(sources.data(), sources.size(), kernel_name.c_str(), k, p,
                      relaxedMode);
}

struct BuildKernelInfo2
{
    cl_kernel *kernels;
    Programs &programs;
    const char *nameInCode;
    bool relaxedMode; // Whether to build with -cl-fast-relaxed-math.
};

cl_int BuildKernelFn(cl_uint job_id, cl_uint thread_id UNUSED, void *p)
{
    BuildKernelInfo2 *info = (BuildKernelInfo2 *)p;
    cl_uint vectorSize = gMinVectorSizeIndex + job_id;
    return BuildKernel(info->nameInCode, vectorSize, info->kernels + vectorSize,
                       &(info->programs[vectorSize]), info->relaxedMode);
}

} // anonymous namespace

int TestFunc_Float_UInt(const Func *f, MTdata d, bool relaxedMode)
{
    int error;
    Programs programs;
    cl_kernel kernels[VECTOR_SIZE_COUNT];
    float maxError = 0.0f;
    int ftz = f->ftz || gForceFTZ || 0 == (CL_FP_DENORM & gFloatCapabilities);
    float maxErrorVal = 0.0f;
    uint64_t step = getTestStep(sizeof(float), BUFFER_SIZE);
    int scale = (int)((1ULL << 32) / (16 * BUFFER_SIZE / sizeof(double)) + 1);

    logFunctionInfo(f->name, sizeof(cl_float), relaxedMode);

    float float_ulps;
    if (gIsEmbedded)
        float_ulps = f->float_embedded_ulps;
    else
        float_ulps = f->float_ulps;

    // Init the kernels
    {
        BuildKernelInfo2 build_info{ kernels, programs, f->nameInCode,
                                     relaxedMode };
        if ((error = ThreadPool_Do(BuildKernelFn,
                                   gMaxVectorSizeIndex - gMinVectorSizeIndex,
                                   &build_info)))
            return error;
    }

    for (uint64_t i = 0; i < (1ULL << 32); i += step)
    {
        // Init input array
        uint32_t *p = (uint32_t *)gIn;
        if (gWimpyMode)
        {
            for (size_t j = 0; j < BUFFER_SIZE / sizeof(float); j++)
                p[j] = (uint32_t)i + j * scale;
        }
        else
        {
            for (size_t j = 0; j < BUFFER_SIZE / sizeof(float); j++)
                p[j] = (uint32_t)i + j;
        }
        if ((error = clEnqueueWriteBuffer(gQueue, gInBuffer, CL_FALSE, 0,
                                          BUFFER_SIZE, gIn, 0, NULL, NULL)))
        {
            vlog_error("\n*** Error %d in clEnqueueWriteBuffer ***\n", error);
            return error;
        }

        // write garbage into output arrays
        for (auto j = gMinVectorSizeIndex; j < gMaxVectorSizeIndex; j++)
        {
            uint32_t pattern = 0xffffdead;
            memset_pattern4(gOut[j], &pattern, BUFFER_SIZE);
            if ((error =
                     clEnqueueWriteBuffer(gQueue, gOutBuffer[j], CL_FALSE, 0,
                                          BUFFER_SIZE, gOut[j], 0, NULL, NULL)))
            {
                vlog_error("\n*** Error %d in clEnqueueWriteBuffer2(%d) ***\n",
                           error, j);
                goto exit;
            }
        }

        // Run the kernels
        for (auto j = gMinVectorSizeIndex; j < gMaxVectorSizeIndex; j++)
        {
            size_t vectorSize = sizeValues[j] * sizeof(cl_float);
            size_t localCount = (BUFFER_SIZE + vectorSize - 1) / vectorSize;
            if ((error = clSetKernelArg(kernels[j], 0, sizeof(gOutBuffer[j]),
                                        &gOutBuffer[j])))
            {
                LogBuildError(programs[j]);
                goto exit;
            }
            if ((error = clSetKernelArg(kernels[j], 1, sizeof(gInBuffer),
                                        &gInBuffer)))
            {
                LogBuildError(programs[j]);
                goto exit;
            }

            if ((error =
                     clEnqueueNDRangeKernel(gQueue, kernels[j], 1, NULL,
                                            &localCount, NULL, 0, NULL, NULL)))
            {
                vlog_error("FAILED -- could not execute kernel\n");
                goto exit;
            }
        }

        // Get that moving
        if ((error = clFlush(gQueue))) vlog("clFlush failed\n");

        // Calculate the correctly rounded reference result
        float *r = (float *)gOut_Ref;
        cl_uint *s = (cl_uint *)gIn;
        for (size_t j = 0; j < BUFFER_SIZE / sizeof(float); j++)
            r[j] = (float)f->func.f_u(s[j]);

        // Read the data back
        for (auto j = gMinVectorSizeIndex; j < gMaxVectorSizeIndex; j++)
        {
            if ((error =
                     clEnqueueReadBuffer(gQueue, gOutBuffer[j], CL_TRUE, 0,
                                         BUFFER_SIZE, gOut[j], 0, NULL, NULL)))
            {
                vlog_error("ReadArray failed %d\n", error);
                goto exit;
            }
        }

        if (gSkipCorrectnessTesting) break;

        // Verify data
        uint32_t *t = (uint32_t *)gOut_Ref;
        for (size_t j = 0; j < BUFFER_SIZE / sizeof(float); j++)
        {
            for (auto k = gMinVectorSizeIndex; k < gMaxVectorSizeIndex; k++)
            {
                uint32_t *q = (uint32_t *)(gOut[k]);

                // If we aren't getting the correctly rounded result
                if (t[j] != q[j])
                {
                    float test = ((float *)q)[j];
                    double correct = f->func.f_u(s[j]);
                    float err = Ulp_Error(test, correct);
                    int fail = !(fabsf(err) <= float_ulps);

                    if (fail)
                    {
                        if (ftz || relaxedMode)
                        {
                            // retry per section 6.5.3.2
                            if (IsFloatResultSubnormal(correct, float_ulps))
                            {
                                fail = fail && (test != 0.0f);
                                if (!fail) err = 0.0f;
                            }
                        }
                    }
                    if (fabsf(err) > maxError)
                    {
                        maxError = fabsf(err);
                        maxErrorVal = s[j];
                    }
                    if (fail)
                    {
                        vlog_error(
                            "\n%s%s: %f ulp error at 0x%8.8x: *%a vs. %a\n",
                            f->name, sizeNames[k], err, ((uint32_t *)gIn)[j],
                            ((float *)gOut_Ref)[j], test);
                        error = -1;
                        goto exit;
                    }
                }
            }
        }

        if (0 == (i & 0x0fffffff))
        {
            if (gVerboseBruteForce)
            {
                vlog("base:%14" PRIu64 " step:%10" PRIu64
                     "  bufferSize:%10d \n",
                     i, step, BUFFER_SIZE);
            }
            else
            {
                vlog(".");
            }
            fflush(stdout);
        }
    }

    if (!gSkipCorrectnessTesting)
    {
        if (gWimpyMode)
            vlog("Wimp pass");
        else
            vlog("passed");

        vlog("\t%8.2f @ %a", maxError, maxErrorVal);
    }

    vlog("\n");

exit:
    // Release
    for (auto k = gMinVectorSizeIndex; k < gMaxVectorSizeIndex; k++)
    {
        clReleaseKernel(kernels[k]);
    }

    return error;
}
