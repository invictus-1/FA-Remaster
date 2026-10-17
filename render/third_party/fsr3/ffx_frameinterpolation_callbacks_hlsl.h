// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "ffx_frameinterpolation_resources.h"

#if defined(FFX_GPU)
#ifdef __hlsl_dx_compiler
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#endif //__hlsl_dx_compiler
#include "../../../../../api/internal/gpu/ffx_core.h"
#ifdef __hlsl_dx_compiler
#pragma dxc diagnostic pop
#endif //__hlsl_dx_compiler
#endif // #if defined(FFX_GPU)

#if defined(FFX_GPU)

#define COUNTER_SPD                          0
#define COUNTER_FRAME_INDEX_SINCE_LAST_RESET 1

// FA Remastered: what a pass reads outside a picture is nought and what it writes there is dropped, as Direct3D 12
// guarantees and these passes count on (at the borders, and past the size drawn); without robust image access
// Vulkan leaves both undefined. So with FFX_FA_ROBUST_ACCESS (render/frame_generation.cpp sets it for every
// backend but Direct3D 12, whose own rule it would only repeat) each read and write checks the picture's size
// first. `outside` is the statement for that case (a return).
#if FFX_FA_ROBUST_ACCESS
#define FFX_FA_OUTSIDE(resource, pos, outside) { FfxUInt32 faWidth, faHeight; resource.GetDimensions(faWidth, faHeight); if (any(FfxUInt32x2(FfxInt32x2(pos)) >= FfxUInt32x2(faWidth, faHeight))) outside; }
#else
#define FFX_FA_OUTSIDE(resource, pos, outside)
#endif

// FA Remastered: gfx.h has no R16G16_SINT: the optical flow's vectors are kept as their two 16-bit halves in an
// R32UI picture (x low, y high; ffx_opticalflow_callbacks_hlsl.h writes them so)
FfxInt32x2 ffxFaUnpackFlow(FfxUInt32 packed)
{
    return FfxInt32x2(FfxInt32(packed << 16) >> 16, FfxInt32(packed) >> 16);
}

  ///////////////////////////////////////////////
 // declare CBs and CB accessors
///////////////////////////////////////////////
#if defined(FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION)
    cbuffer cbFI FFX_DECLARE_CB(FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION)
    {
        FfxInt32x2      renderSize;
        FfxInt32x2      displaySize;

        FfxFloat32x2    displaySizeRcp;
        FfxFloat32      cameraNear;
        FfxFloat32      cameraFar;

        FfxInt32x2      upscalerTargetSize;
        FfxInt32        Mode;
        FfxInt32        reset;

        FfxFloat32x4    fDeviceToViewDepth;

        FfxFloat32      deltaTime;
        FfxInt32        HUDLessAttachedFactor;
        FfxInt32x2      distortionFieldSize;

        FfxFloat32x2    opticalFlowScale;
        FfxInt32        opticalFlowBlockSize;
        FfxUInt32       dispatchFlags;

        FfxInt32x2      maxRenderSize;
        FfxInt32        opticalFlowHalfResMode;
        FfxInt32        NumInstances;

        FfxInt32x2      interpolationRectBase;
        FfxInt32x2      interpolationRectSize;

        FfxFloat32x3    debugBarColor;
        FfxUInt32       backBufferTransferFunction;

        FfxFloat32x2    minMaxLuminance;
        FfxFloat32      fTanHalfFOV;
        FfxInt32        _pad1;

        FfxFloat32x2    fJitter;
        FfxFloat32x2    fMotionVectorScale;
    }

    const FfxFloat32x2 Jitter()
    {
        return fJitter;
    }

    const FfxFloat32x2 MotionVectorScale()
    {
        return fMotionVectorScale;
    }

    const FfxInt32x2 InterpolationRectBase()
    {
        return interpolationRectBase;
    }

    const FfxInt32x2 InterpolationRectSize()
    {
        return interpolationRectSize;
    }

    const FfxInt32x2 RenderSize()
    {
        return renderSize;
    }

    const FfxInt32x2 DisplaySize()
    {
        return displaySize;
    }

    const FfxBoolean Reset()
    {
        return reset == 1;
    }

    FfxFloat32x4 DeviceToViewSpaceTransformFactors()
    {
        return fDeviceToViewDepth;
    }

    FfxInt32x2 GetOpticalFlowSize()
    {
        FfxInt32x2 iOpticalFlowSize = (1.0f / opticalFlowScale) / FfxFloat32x2(opticalFlowBlockSize.xx);

        return iOpticalFlowSize;
    }

    FfxInt32x2 GetOpticalFlowSize2()
    {
        return GetOpticalFlowSize() * 1;
    }

    FfxFloat32x2 GetOpticalFlowScale()
    {
        return opticalFlowScale;
    }

    FfxInt32 GetOpticalFlowBlockSize()
    {
        return opticalFlowBlockSize;
    }
    
    FfxInt32 GetHUDLessAttachedFactor()
    {
        return HUDLessAttachedFactor;
    }

    FfxInt32x2 GetDistortionFieldSize()
    {
        return distortionFieldSize;
    }

    FfxUInt32 GetDispatchFlags()
    {
        return dispatchFlags;
    }

    FfxInt32x2 GetMaxRenderSize()
    {
        return maxRenderSize;
    }

    FfxInt32 GetOpticalFlowHalfResMode()
    {
        return opticalFlowHalfResMode;
    }

    FfxFloat32x3 GetDebugBarColor()
    {
        return debugBarColor;
    }

    FfxFloat32 TanHalfFoV()
    {
        return fTanHalfFOV;
    }

    FfxUInt32 BackBufferTransferFunction()
    {
        return backBufferTransferFunction;
    }

    FfxFloat32 MinLuminance()
    {
        return minMaxLuminance[0];
    }

    FfxFloat32 MaxLuminance()
    {
        return minMaxLuminance[1];
    }

#endif // #if defined(FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION)

#if defined(FFX_FRAMEINTERPOLATION_BIND_CB_INPAINTING_PYRAMID)
    cbuffer cbInpaintingPyramid FFX_DECLARE_CB(FFX_FRAMEINTERPOLATION_BIND_CB_INPAINTING_PYRAMID)
    {
        FfxUInt32 mips;
        FfxUInt32 numWorkGroups;
        FfxUInt32x2 workGroupOffset;
    }

    FfxUInt32 NumMips()
    {
        return mips;
    }
    FfxUInt32 NumWorkGroups()
    {
        return numWorkGroups;
    }
    FfxUInt32x2 WorkGroupOffset()
    {
        return workGroupOffset;
    }
#endif // #if defined(FFX_FRAMEINTERPOLATION_BIND_CB_INPAINTING_PYRAMID)

#define FFX_FRAMEINTERPOLATION_ROOTSIG_STRINGIFY(p) FFX_FRAMEINTERPOLATION_ROOTSIG_STR(p)
#define FFX_FRAMEINTERPOLATION_ROOTSIG_STR(p) #p
#define FFX_FRAMEINTERPOLATION_ROOTSIG [RootSignature( "DescriptorTable(UAV(u0, numDescriptors = " FFX_FRAMEINTERPOLATION_ROOTSIG_STRINGIFY(FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "DescriptorTable(SRV(t0, numDescriptors = " FFX_FRAMEINTERPOLATION_ROOTSIG_STRINGIFY(FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "CBV(b0), " \
                                    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, " \
                                                      "addressU = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressV = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressW = TEXTURE_ADDRESS_CLAMP, " \
                                                      "comparisonFunc = COMPARISON_NEVER, " \
                                                      "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK)" )]

#define FFX_FRAMEINTERPOLATION_INPAINTING_ROOTSIG [RootSignature( "DescriptorTable(UAV(u0, numDescriptors = " FFX_FRAMEINTERPOLATION_ROOTSIG_STRINGIFY(FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "DescriptorTable(SRV(t0, numDescriptors = " FFX_FRAMEINTERPOLATION_ROOTSIG_STRINGIFY(FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "CBV(b0), " \
                                    "CBV(b1), " \
                                    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, " \
                                                      "addressU = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressV = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressW = TEXTURE_ADDRESS_CLAMP, " \
                                                      "comparisonFunc = COMPARISON_NEVER, " \
                                                      "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK)" )]

#if defined(FFX_FRAMEINTERPOLATION_EMBED_ROOTSIG) && FFX_FRAMEINTERPOLATION_EMBED_ROOTSIG==1
#define FFX_FRAMEINTERPOLATION_EMBED_ROOTSIG_CONTENT FFX_FRAMEINTERPOLATION_ROOTSIG
#define FFX_FRAMEINTERPOLATION_EMBED_INPAINTING_ROOTSIG_CONTENT FFX_FRAMEINTERPOLATION_INPAINTING_ROOTSIG
#else
#define FFX_FRAMEINTERPOLATION_EMBED_ROOTSIG_CONTENT
#define FFX_FRAMEINTERPOLATION_EMBED_INPAINTING_ROOTSIG_CONTENT
#endif // #if FFX_FRAMEINTERPOLATION_EMBED_ROOTSIG

///////////////////////////////////////////////
// declare samplers
///////////////////////////////////////////////

SamplerState s_LinearClamp FFX_DECLARE_SAMPLER(1);  // FA Remastered: s1, the renderer's linear clamp (AMD's s0)

///////////////////////////////////////////////
// declare SRVs and SRV accessors
///////////////////////////////////////////////

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE
    Texture2D<FfxFloat32x4> r_previous_interpolation_source FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE);

    FfxFloat32x3 LoadPreviousBackbuffer(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_previous_interpolation_source, iPxPos, return 0)  // FA Remastered
        return r_previous_interpolation_source[iPxPos].rgb;
    }
    FfxFloat32x3 SamplePreviousBackbuffer(FFX_PARAMETER_IN FfxFloat32x2 fUv)
    {
        return r_previous_interpolation_source.SampleLevel(s_LinearClamp, fUv, 0).xyz;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE
    Texture2D<FfxFloat32x4> r_current_interpolation_source FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE);

    FfxFloat32x3 LoadCurrentBackbuffer(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_current_interpolation_source, iPxPos, return 0)  // FA Remastered
        return r_current_interpolation_source[iPxPos].rgb;
    }
    FfxFloat32x3 SampleCurrentBackbuffer(FFX_PARAMETER_IN FfxFloat32x2 fUv)
    {
        return r_current_interpolation_source.SampleLevel(s_LinearClamp, fUv, 0).xyz;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS
    Texture2D<FfxFloat32x2> r_dilated_motion_vectors FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS);

    FfxFloat32x2 LoadDilatedMotionVector(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_dilated_motion_vectors, iPxPos, return 0)  // FA Remastered
        return r_dilated_motion_vectors[iPxPos].xy;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH
    Texture2D<FfxFloat32> r_dilated_depth FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH);

    FfxFloat32 LoadDilatedDepth(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_dilated_depth, iPxPos, return 0)  // FA Remastered
        return r_dilated_depth[iPxPos].x;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME
    Texture2D<FfxUInt32>   r_reconstructed_depth_previous_frame FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME);

    FfxFloat32 LoadReconstructedDepthPreviousFrame(FFX_PARAMETER_IN FfxInt32x2 iPxInput)
    {
        FFX_FA_OUTSIDE(r_reconstructed_depth_previous_frame, iPxInput, return 0)  // FA Remastered
        return asfloat(r_reconstructed_depth_previous_frame[iPxInput]);
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME
    Texture2D<FfxUInt32>   r_reconstructed_depth_interpolated_frame FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME);

    FfxFloat32 LoadEstimatedInterpolationFrameDepth(FFX_PARAMETER_IN FfxInt32x2 iPxInput)
    {
        FFX_FA_OUTSIDE(r_reconstructed_depth_interpolated_frame, iPxInput, return 0)  // FA Remastered
        return asfloat(r_reconstructed_depth_interpolated_frame[iPxInput]);
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_DISOCCLUSION_MASK
    Texture2D<FfxFloat32x4> r_disocclusion_mask FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_DISOCCLUSION_MASK);

    FfxFloat32x4 LoadDisocclusionMask(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_disocclusion_mask, iPxPos, return 0)  // FA Remastered
        return r_disocclusion_mask[iPxPos];
    }
    FfxFloat32x4 SampleDisocclusionMask(FFX_PARAMETER_IN FfxFloat32x2 fUv)
    {
        return r_disocclusion_mask.SampleLevel(s_LinearClamp, fUv, 0);
    }
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_X) && \
    defined(FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_Y)
    Texture2D<FfxUInt32> r_game_motion_vector_field_x FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_X);
    Texture2D<FfxUInt32> r_game_motion_vector_field_y FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_GAME_MOTION_VECTOR_FIELD_Y);

    FfxUInt32x2 LoadGameFieldMv(FFX_PARAMETER_IN FfxInt32x2 iPxSample)
    {
        FFX_FA_OUTSIDE(r_game_motion_vector_field_x, iPxSample, return 0)  // FA Remastered
        FfxUInt32 packedX = r_game_motion_vector_field_x[iPxSample];
        FfxUInt32 packedY = r_game_motion_vector_field_y[iPxSample];

        return FfxUInt32x2(packedX, packedY);
    }
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X) && \
    defined(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y)
    Texture2D<FfxUInt32> r_optical_flow_motion_vector_field_x FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X);
    Texture2D<FfxUInt32> r_optical_flow_motion_vector_field_y FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y);

    FfxUInt32x2 LoadOpticalFlowFieldMv(FFX_PARAMETER_IN FfxInt32x2 iPxSample)
    {
        FFX_FA_OUTSIDE(r_optical_flow_motion_vector_field_x, iPxSample, return 0)  // FA Remastered
        FfxUInt32 packedX = r_optical_flow_motion_vector_field_x[iPxSample];
        FfxUInt32 packedY = r_optical_flow_motion_vector_field_y[iPxSample];

        return FfxUInt32x2(packedX, packedY);
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW
    Texture2D<FfxUInt32> r_optical_flow FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW);  // FA Remastered: packed (AMD's R16G16_SINT)
    
    #if defined(FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION)
        FfxFloat32x2 LoadOpticalFlow(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
        {
            FFX_FA_OUTSIDE(r_optical_flow, iPxPos, return 0)  // FA Remastered
            return ffxFaUnpackFlow(r_optical_flow[iPxPos]) * GetOpticalFlowScale();  // FA Remastered: unpacked
        }
    #endif
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_UPSAMPLED
    Texture2D<FfxFloat32x2> r_optical_flow_upsampled FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_UPSAMPLED);

    FfxFloat32x2 LoadOpticalFlowUpsampled(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        return r_optical_flow_upsampled[iPxPos];
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_CONFIDENCE
    Texture2D<FfxUInt32x2> r_optical_flow_confidence FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_CONFIDENCE);
    
    FfxFloat32 LoadOpticalFlowConfidence(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_optical_flow_confidence, iPxPos, return 0)  // FA Remastered
        return r_optical_flow_confidence[iPxPos].y;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_GLOBAL_MOTION
    Texture2D<FfxUInt32> r_optical_flow_global_motion FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_GLOBAL_MOTION);

    FfxUInt32 LoadOpticalFlowGlobalMotion(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_optical_flow_global_motion, iPxPos, return 0)  // FA Remastered
        return r_optical_flow_global_motion[iPxPos];
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_SCENE_CHANGE_DETECTION
    Texture2D<FfxUInt32> r_optical_flow_scd FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_SCENE_CHANGE_DETECTION);

    FfxUInt32 LoadOpticalFlowSceneChangeDetection(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(r_optical_flow_scd, iPxPos, return 0)  // FA Remastered
        return r_optical_flow_scd[iPxPos];
    }

    FfxBoolean HasSceneChanged()
    {
        #define SCD_OUTPUT_HISTORY_BITS_SLOT 1
        //if (FrameIndex() <= 5) // threshold according to original OpenCL code
        //{
        //    return 1.0;
        //}
        //else
        {
            // Report that the scene is changed if the change was detected in any of the
            // 4 previous frames (0xfu - covers 4 history bits).
            return (r_optical_flow_scd[FfxInt32x2(SCD_OUTPUT_HISTORY_BITS_SLOT, 0)] & 0xfu) != 0;
        }
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_DEBUG
    Texture2D<FfxFloat32x4> r_optical_flow_debug FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_DEBUG);

    FfxFloat32x4 LoadOpticalFlowDebug(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        return r_optical_flow_debug[iPxPos];
    }
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_MASK) && defined(FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT)
    Texture2D<FfxFloat32x3> r_output          FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT);
    Texture2D<FfxFloat32>   r_inpainting_mask FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_MASK);

    FfxFloat32x4 LoadFrameInterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxInput)
    {
        FFX_FA_OUTSIDE(r_output, iPxInput, return 0)  // FA Remastered
        return FfxFloat32x4(r_output[iPxInput], r_inpainting_mask[iPxInput]);
    }
#elif defined(FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT)
    Texture2D<FfxFloat32x4> r_output          FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT);
    FfxFloat32x4 LoadFrameInterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxInput)
    {
        FFX_FA_OUTSIDE(r_output, iPxInput, return 0)  // FA Remastered
        return r_output[iPxInput];
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID
    Texture2D<FfxFloat32x4> r_inpainting_pyramid FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID);

    FfxFloat32x4 LoadInpaintingPyramid(FFX_PARAMETER_IN FfxInt32 mipLevel, FFX_PARAMETER_IN FfxUInt32x2 iPxInput)
    {
#if FFX_FA_ROBUST_ACCESS
        // FA Remastered: nought outside the level, or for a level the picture does not have (FFX_FA_OUTSIDE)
        FfxUInt32 faWidth, faHeight, faLevels;
        r_inpainting_pyramid.GetDimensions(0, faWidth, faHeight, faLevels);
        if (FfxUInt32(mipLevel) >= faLevels || any(iPxInput >= FfxUInt32x2(max(faWidth >> mipLevel, 1u), max(faHeight >> mipLevel, 1u))))
            return 0;
#endif
        return r_inpainting_pyramid.mips[mipLevel][iPxInput];
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_PRESENT_BACKBUFFER
    Texture2D<FfxFloat32x4> r_present_backbuffer FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_PRESENT_BACKBUFFER);

    FfxFloat32x4 LoadPresentBackbuffer(FFX_PARAMETER_IN FfxInt32x2 iPxInput)
    {
        FFX_FA_OUTSIDE(r_present_backbuffer, iPxInput, return 0)  // FA Remastered
        return r_present_backbuffer[iPxInput];
    }
    FfxFloat32x4 SamplePresentBackbuffer(FFX_PARAMETER_IN FfxFloat32x2 fUv)
    {
        return r_present_backbuffer.SampleLevel(s_LinearClamp, fUv, 0);
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_SRV_COUNTERS
    // FA Remastered: the counters are a 2 x 1 R32UI picture, not a buffer (gfx.h writes no buffers)
    Texture2D<FfxUInt32> r_counters FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_COUNTERS);

    FfxUInt32 LoadCounter(FFX_PARAMETER_IN FfxInt32 iPxPos)
    {
        return r_counters[FfxInt32x2(iPxPos, 0)];
    }

    const FfxUInt32 FrameIndexSinceLastReset()
    {
        return LoadCounter(COUNTER_FRAME_INDEX_SINCE_LAST_RESET);
    }
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_DEPTH)
Texture2D<FfxFloat32> r_input_depth FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_DEPTH);
FfxFloat32 LoadInputDepth(FfxInt32x2 iPxPos)
{
    FFX_FA_OUTSIDE(r_input_depth, iPxPos, return 0)  // FA Remastered
    return r_input_depth[iPxPos];
}
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_MOTION_VECTORS)
Texture2D<FfxFloat32x4> r_input_motion_vectors FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_INPUT_MOTION_VECTORS);
FfxFloat32x2 LoadInputMotionVector(FfxInt32x2 iPxDilatedMotionVectorPos)
{
    FFX_FA_OUTSIDE(r_input_motion_vectors, iPxDilatedMotionVectorPos, return 0)  // FA Remastered
    FfxFloat32x2 fSrcMotionVector = r_input_motion_vectors[iPxDilatedMotionVectorPos].xy;

    FfxFloat32x2 fUvMotionVector = fSrcMotionVector * MotionVectorScale();

#if FFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS
    fUvMotionVector -= MotionVectorJitterCancellation();
#endif

    return fUvMotionVector;
}
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD)
    Texture2D<FfxFloat32x2> r_input_distortion_field FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD);
    FfxFloat32x2 SampleDistortionField(FFX_PARAMETER_IN FfxFloat32x2 fUv)
    {
        return r_input_distortion_field.SampleLevel(s_LinearClamp, fUv, 0);
    }
#endif

///////////////////////////////////////////////
// declare UAVs and UAV accessors
///////////////////////////////////////////////
#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_MASK) && defined(FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT)
    // FA Remastered: the output declared with four channels (AMD's three): Vulkan's writes must give every channel
    // of the picture's format (RGBA8); the alpha written is 1
    RWTexture2D<FfxFloat32x4> rw_output          FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT);
    RWTexture2D<FfxFloat32>   rw_inpainting_mask FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_MASK);

    FfxFloat32x4 RWLoadFrameinterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_output, iPxPos, return 0)  // FA Remastered
        return FfxFloat32x4(rw_output[iPxPos].rgb, rw_inpainting_mask[iPxPos]);
    }

    void StoreFrameinterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x4 val)
    {
        FFX_FA_OUTSIDE(rw_output, iPxPos, return)  // FA Remastered
        rw_output[iPxPos] = FfxFloat32x4(val.rgb, 1.0f);
        rw_inpainting_mask[iPxPos] = val.a;
    }

#elif defined(FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT)
    RWTexture2D<FfxFloat32x4> rw_output FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT);

    FfxFloat32x4 RWLoadFrameinterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_output, iPxPos, return 0)  // FA Remastered
        return rw_output[iPxPos];
    }

    void StoreFrameinterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x4 val)
    {
        FFX_FA_OUTSIDE(rw_output, iPxPos, return)  // FA Remastered
        rw_output[iPxPos] = val;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_UAV_DILATED_MOTION_VECTORS
    RWTexture2D<FfxFloat32x2> rw_dilated_motion_vectors FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_DILATED_MOTION_VECTORS);

    FfxFloat32x2 RWLoadDilatedMotionVectors(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_dilated_motion_vectors, iPxPos, return 0)  // FA Remastered
        return rw_dilated_motion_vectors[iPxPos];
    }

    void StoreDilatedMotionVectors(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 val)
    {
        FFX_FA_OUTSIDE(rw_dilated_motion_vectors, iPxPos, return)  // FA Remastered
        rw_dilated_motion_vectors[iPxPos] = val;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_UAV_DILATED_DEPTH
    RWTexture2D<FfxFloat32> rw_dilated_depth FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_DILATED_DEPTH);

    FfxFloat32 RWLoadDilatedDepth(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_dilated_depth, iPxPos, return 0)  // FA Remastered
        return rw_dilated_depth[iPxPos];
    }

    void StoreDilatedDepth(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32 val)
    {
        FFX_FA_OUTSIDE(rw_dilated_depth, iPxPos, return)  // FA Remastered
        rw_dilated_depth[iPxPos] = val;
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME
    RWTexture2D<FfxUInt32> rw_reconstructed_depth_previous_frame FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_PREVIOUS_FRAME);

    FfxFloat32 RWLoadReconstructedDepthPreviousFrame(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_reconstructed_depth_previous_frame, iPxPos, return 0)  // FA Remastered
        return ffxAsFloat(rw_reconstructed_depth_previous_frame[iPxPos]);
    }

    void UpdateReconstructedDepthPreviousFrame(FfxInt32x2 iPxSample, FfxFloat32 fDepth)
    {
        FFX_FA_OUTSIDE(rw_reconstructed_depth_previous_frame, iPxSample, return)  // FA Remastered
        FfxUInt32 uDepth = ffxAsUInt32(fDepth);

#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
        InterlockedMax(rw_reconstructed_depth_previous_frame[iPxSample], uDepth);
#else
        InterlockedMin(rw_reconstructed_depth_previous_frame[iPxSample], uDepth);  // min for standard, max for inverted depth
#endif
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME
    RWTexture2D<FfxUInt32>   rw_reconstructed_depth_interpolated_frame FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME);
    
    FfxFloat32 RWLoadReconstructedDepthInterpolatedFrame(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_reconstructed_depth_interpolated_frame, iPxPos, return 0)  // FA Remastered
        return ffxAsFloat(rw_reconstructed_depth_interpolated_frame[iPxPos]);
    }

    void StoreReconstructedDepthInterpolatedFrame(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32 value)
    {
        FFX_FA_OUTSIDE(rw_reconstructed_depth_interpolated_frame, iPxPos, return)  // FA Remastered
        FfxUInt32 uDepth                                  = ffxAsUInt32(value);
        rw_reconstructed_depth_interpolated_frame[iPxPos] = uDepth;
    }

    void UpdateReconstructedDepthInterpolatedFrame(FfxInt32x2 iPxSample, FfxFloat32 fDepth)
    {
        FFX_FA_OUTSIDE(rw_reconstructed_depth_interpolated_frame, iPxSample, return)  // FA Remastered
        FfxUInt32 uDepth = ffxAsUInt32(fDepth);

#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
        InterlockedMax(rw_reconstructed_depth_interpolated_frame[iPxSample], uDepth);
#else
        InterlockedMin(rw_reconstructed_depth_interpolated_frame[iPxSample], uDepth);  // min for standard, max for inverted depth
#endif
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_UAV_DISOCCLUSION_MASK
    RWTexture2D<FfxFloat32x2> rw_disocclusion_mask FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_DISOCCLUSION_MASK);

    FfxFloat32x2 RWLoadDisocclusionMask(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_disocclusion_mask, iPxPos, return 0)  // FA Remastered
        return rw_disocclusion_mask[iPxPos];
    }

    void StoreDisocclusionMask(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 val)
    {
        FFX_FA_OUTSIDE(rw_disocclusion_mask, iPxPos, return)  // FA Remastered
        rw_disocclusion_mask[iPxPos] = val;
    }
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_X) && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_Y)

    RWTexture2D<FfxUInt32> rw_game_motion_vector_field_x FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_X);
    RWTexture2D<FfxUInt32> rw_game_motion_vector_field_y FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_Y);

    FfxUInt32 RWLoadGameMotionVectorFieldX(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_game_motion_vector_field_x, iPxPos, return 0)  // FA Remastered
        return rw_game_motion_vector_field_x[iPxPos];
    }

    void StoreGameMotionVectorFieldX(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32 val)
    {
        FFX_FA_OUTSIDE(rw_game_motion_vector_field_x, iPxPos, return)  // FA Remastered
        rw_game_motion_vector_field_x[iPxPos] = val;
    }

    FfxUInt32 RWLoadGameMotionVectorFieldY(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_game_motion_vector_field_y, iPxPos, return 0)  // FA Remastered
        return rw_game_motion_vector_field_y[iPxPos];
    }

    void StoreGameMotionVectorFieldY(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32 val)
    {
        FFX_FA_OUTSIDE(rw_game_motion_vector_field_y, iPxPos, return)  // FA Remastered
        rw_game_motion_vector_field_y[iPxPos] = val;
    }

    void UpdateGameMotionVectorField(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32x2 packedVector)
    {
        FFX_FA_OUTSIDE(rw_game_motion_vector_field_x, iPxPos, return)  // FA Remastered
        InterlockedMax(rw_game_motion_vector_field_x[iPxPos], packedVector.x);
        InterlockedMax(rw_game_motion_vector_field_y[iPxPos], packedVector.y);
    }

    FfxUInt32 UpdateGameMotionVectorFieldEx(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32x2 packedVector)
    {
        FFX_FA_OUTSIDE(rw_game_motion_vector_field_x, iPxPos, return 0)  // FA Remastered
        FfxUInt32 uPreviousValueX = 0;
        FfxUInt32 uPreviousValueY = 0;
        InterlockedMax(rw_game_motion_vector_field_x[iPxPos], packedVector.x, uPreviousValueX);
        InterlockedMax(rw_game_motion_vector_field_y[iPxPos], packedVector.y, uPreviousValueY);

        const FfxUInt32 uExistingVectorFieldEntry = ffxMax(uPreviousValueX, uPreviousValueY);

        return uExistingVectorFieldEntry;
    }
#endif

#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X) && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y)

    RWTexture2D<FfxUInt32> rw_optical_flow_motion_vector_field_x FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X);
    RWTexture2D<FfxUInt32> rw_optical_flow_motion_vector_field_y FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y);

    FfxUInt32 RWLoadOpticalflowMotionVectorFieldX(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_optical_flow_motion_vector_field_x, iPxPos, return 0)  // FA Remastered
        return rw_optical_flow_motion_vector_field_x[iPxPos];
    }
    void StoreOpticalflowMotionVectorFieldX(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32 val)
    {
        FFX_FA_OUTSIDE(rw_optical_flow_motion_vector_field_x, iPxPos, return)  // FA Remastered
        rw_optical_flow_motion_vector_field_x[iPxPos] = val;
    }
    FfxUInt32 RWLoadOpticalflowMotionVectorFieldY(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        FFX_FA_OUTSIDE(rw_optical_flow_motion_vector_field_y, iPxPos, return 0)  // FA Remastered
        return rw_optical_flow_motion_vector_field_y[iPxPos];
    }
    void StoreOpticalflowMotionVectorFieldY(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32 val)
    {
        FFX_FA_OUTSIDE(rw_optical_flow_motion_vector_field_y, iPxPos, return)  // FA Remastered
        rw_optical_flow_motion_vector_field_y[iPxPos] = val;
    }
    void UpdateOpticalflowMotionVectorField(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32x2 packedVector)
    {
        FFX_FA_OUTSIDE(rw_optical_flow_motion_vector_field_x, iPxPos, return)  // FA Remastered
        InterlockedMax(rw_optical_flow_motion_vector_field_x[iPxPos], packedVector.x);
        InterlockedMax(rw_optical_flow_motion_vector_field_y[iPxPos], packedVector.y);
    }
#endif

#ifdef FFX_FRAMEINTERPOLATION_BIND_UAV_COUNTERS
    // FA Remastered: a 2 x 1 R32UI picture, not a buffer (as r_counters)
    globallycoherent RWTexture2D<FfxUInt32> rw_counters FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_COUNTERS);

    FfxUInt32 RWLoadCounter(FFX_PARAMETER_IN FfxInt32 iPxPos)
    {
        return rw_counters[FfxInt32x2(iPxPos, 0)];
    }

    void StoreCounter(FFX_PARAMETER_IN FfxInt32 iPxPos, FFX_PARAMETER_IN FfxUInt32 counter)
    {
        rw_counters[FfxInt32x2(iPxPos, 0)] = counter;
    }
    void AtomicIncreaseCounter(FFX_PARAMETER_IN FfxInt32 iPxPos, FFX_PARAMETER_OUT FfxUInt32 oldVal)
    {
        InterlockedAdd(rw_counters[FfxInt32x2(iPxPos, 0)], 1, oldVal);
    }
#endif


#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_0)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_1)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_2)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_3)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_4)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_5)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_6)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_7)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_8)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_9)    && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_10)   && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_11)   && \
    defined(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_12)

    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid0   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_0);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid1   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_1);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid2   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_2);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid3   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_3);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid4   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_4);
    globallycoherent RWTexture2D<FfxFloat32x4>  rw_inpainting_pyramid5   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_5);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid6   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_6);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid7   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_7);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid8   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_8);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid9   FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_9);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid10  FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_10);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid11  FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_11);
    RWTexture2D<FfxFloat32x4>                   rw_inpainting_pyramid12  FFX_DECLARE_UAV(FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_PYRAMID_MIPMAP_12);


    FfxFloat32x4 RWLoadInpaintingPyramid(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxUInt32 index)
    {
        #define LOAD(idx)                                 \
            if (index == idx)                             \
            {                                             \
                FFX_FA_OUTSIDE(rw_inpainting_pyramid##idx, iPxPos, return 0) /* FA Remastered */ \
                return rw_inpainting_pyramid##idx[iPxPos]; \
            }
        LOAD(0);
        LOAD(1);
        LOAD(2);
        LOAD(3);
        LOAD(4);
        LOAD(5);
        LOAD(6);
        LOAD(7);
        LOAD(8);
        LOAD(9);
        LOAD(10);
        LOAD(11);
        LOAD(12);
        return 0;

        #undef LOAD
    }

    void StoreInpaintingPyramid(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x4 outValue, FFX_PARAMETER_IN FfxUInt32 index)
    {
        #define STORE(idx)                   \
            if (index == idx)                \
            {                                \
                FFX_FA_OUTSIDE(rw_inpainting_pyramid##idx, iPxPos, return) /* FA Remastered */ \
                rw_inpainting_pyramid##idx[iPxPos] = outValue; \
            }

        STORE(0);
        STORE(1);
        STORE(2);
        STORE(3);
        STORE(4);
        STORE(5);
        STORE(6);
        STORE(7);
        STORE(8);
        STORE(9);
        STORE(10);
        STORE(11);
        STORE(12);

        #undef STORE
    }
#endif

#endif // #if defined(FFX_GPU)
