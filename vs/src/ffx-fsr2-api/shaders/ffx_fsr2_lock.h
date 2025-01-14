// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
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

FfxFloat32 GetLuma(FFX_MIN16_I2 pos)
{
    //add some bias to avoid locking dark areas
    return FfxFloat32(LoadPreparedInputColorLuma(pos));
}

FfxFloat32 ComputeThinFeatureConfidence(FFX_MIN16_I2 pos)
{
    const FfxInt32 RADIUS = 1;

    FfxFloat32 fNucleus = GetLuma(pos);

    FfxFloat32 similar_threshold = 1.05f;
    FfxFloat32 dissimilarLumaMin = FSR2_FLT_MAX;
    FfxFloat32 dissimilarLumaMax = 0;

    /*
     0 1 2
     3 4 5
     6 7 8
    */

    #define SETBIT(x) (1U << x)

    FfxUInt32 mask = SETBIT(4); //flag fNucleus as similar

    const FfxUInt32 rejectionMasks[4] = {
        SETBIT(0) | SETBIT(1) | SETBIT(3) | SETBIT(4), //Upper left
        /*
         0 1
         3 4
        */
        SETBIT(1) | SETBIT(2) | SETBIT(4) | SETBIT(5), //Upper right
        SETBIT(3) | SETBIT(4) | SETBIT(6) | SETBIT(7), //Lower left
        SETBIT(4) | SETBIT(5) | SETBIT(7) | SETBIT(8), //Lower right
    };

    FfxInt32 idx = 0;
    FFX_UNROLL
    for (FfxInt32 y = -RADIUS; y <= RADIUS; y++) {
        FFX_UNROLL
        for (FfxInt32 x = -RADIUS; x <= RADIUS; x++, idx++) {
            if (x == 0 && y == 0) continue;

            FFX_MIN16_I2 samplePos = ClampLoad(pos, FFX_MIN16_I2(x, y), FFX_MIN16_I2(RenderSize()));

            FfxFloat32 sampleLuma = GetLuma(samplePos);
            FfxFloat32 difference = ffxMax(sampleLuma, fNucleus) / ffxMin(sampleLuma, fNucleus);

            if (difference > 0 && (difference < similar_threshold)) {   // 차이가 없다
                mask |= SETBIT(idx);
            } else {    // 차이가 있으면
                dissimilarLumaMin = ffxMin(dissimilarLumaMin, sampleLuma);  // 차이가 있는 주변 픽셀의 최저값
                dissimilarLumaMax = ffxMax(dissimilarLumaMax, sampleLuma);  // 차이가 있는 주변 픽셀의 최대값
            }
        }
    }

    // 주변 비유사 픽셀보다 확실히 밝거나,확실히 어두우면
    FfxBoolean isRidge = fNucleus > dissimilarLumaMax || fNucleus < dissimilarLumaMin;

    if (FFX_FALSE == isRidge) {

        return 0;   // ThinFeature 없음
    }

    FFX_UNROLL
    for (FfxInt32 i = 0; i < 4; i++) {

        if ((mask & rejectionMasks[i]) == rejectionMasks[i]) {
            return 0;
        }
    }
    
    return 1;       // ThinFeature 있음
}

FFX_STATIC FfxBoolean s_bLockUpdated = FFX_FALSE;

LOCK_STATUS_T ComputeLockStatus(FFX_MIN16_I2 iPxLrPos, LOCK_STATUS_T fLockStatus)
{
    //#GG_5_Lock : 1.1. 계산 : fConfidenceOfThinFeature : 1 - Thin Feature임 , 0 - Thin Feature아님
    FfxFloat32 fConfidenceOfThinFeature = ComputeThinFeatureConfidence(iPxLrPos);

    s_bLockUpdated = FFX_FALSE;
    if (fConfidenceOfThinFeature > 0.0f)
    {
        //#GG_5_Lock : 1.2.저장 : fLockStatus[LOCK_LIFETIME_REMAINING]
        //put to negative on new lock
        fLockStatus[LOCK_LIFETIME_REMAINING] = (fLockStatus[LOCK_LIFETIME_REMAINING] == LOCK_STATUS_F1(0.0f)) ? LOCK_STATUS_F1(-LockInitialLifetime()) : LOCK_STATUS_F1(-(LockInitialLifetime() * 2));

        s_bLockUpdated = FFX_TRUE;
    }

    return fLockStatus;
}

void ComputeLock(FFX_MIN16_I2 iPxLrPos)
{
    FfxFloat32x2 fSrcJitteredPos = FfxFloat32x2(iPxLrPos) + 0.5f - Jitter();
    FfxFloat32x2 fLrPosInHr = (fSrcJitteredPos / RenderSize()) * DisplaySize();
    FfxFloat32x2 fHrPos = floor(fLrPosInHr) + 0.5;
    FFX_MIN16_I2 iPxHrPos = FFX_MIN16_I2(fHrPos);

    LOCK_STATUS_T fLockStatus = ComputeLockStatus(iPxLrPos, LoadLockStatus(iPxHrPos));

    if ((s_bLockUpdated)) {
        StoreLockStatus(iPxHrPos, fLockStatus);
    }
}

FFX_GROUPSHARED FfxFloat32 gs_ReactiveMask[(8 + 4) * (8 + 4)];

void StoreReactiveMaskToLDS(FfxUInt32x2 coord, FfxFloat32x2 value)
{
    FfxUInt32 baseIdx = coord.y * 12 + coord.x;
    gs_ReactiveMask[baseIdx] = value.x;
    gs_ReactiveMask[baseIdx + 1] = value.y;
}

FfxFloat32 LoadReactiveMaskFromLDS(FfxUInt32x2 coord)
{
    return gs_ReactiveMask[coord.y * 12 + coord.x];
}

void PreProcessReactiveMask(FFX_MIN16_I2 iPxLrPos, FfxUInt32x2 groupId, FfxUInt32x2 groupThreadId)
{
#if OPT_PRECOMPUTE_REACTIVE_MAX && !OPT_USE_EVAL_ACCUMULATION_REACTIVENESS

    if (all(FFX_LESS_THAN(groupThreadId, FFX_BROADCAST_UINT32X2(6)))) {

        FfxInt32x2 iPos = FfxInt32x2(groupId << 3) + FfxInt32x2(groupThreadId << 1) - 1;
        FfxFloat32x4 fReactiveMask2x2 = GatherReactiveMask(iPos).wzxy;

        StoreReactiveMaskToLDS(groupThreadId << 1, fReactiveMask2x2.xy);
        StoreReactiveMaskToLDS((groupThreadId << 1) + FfxInt32x2(0, 1), fReactiveMask2x2.zw);
    }

    FFX_GROUP_MEMORY_BARRIER();

    FfxFloat32 fReactiveMax = 0.0f;

    for (FfxUInt32 row = 0; row < 4; row++) {
        for (FfxUInt32 col = 0; col < 4; col++) {
            const FfxUInt32x2 localOffset = groupThreadId + FfxUInt32x2(col, row);
            const FfxBoolean bOutOfRenderBounds = any(FFX_GREATER_THAN_EQUAL((FfxInt32x2(groupId << 3) + FfxInt32x2(localOffset)), RenderSize()));
            fReactiveMax = bOutOfRenderBounds ? fReactiveMax : ffxMax(fReactiveMax, LoadReactiveMaskFromLDS(localOffset));
        }
    }

    // Threshold reactive value
    fReactiveMax = fReactiveMax > 0.8f ? fReactiveMax : 0.0f;

    StoreReactiveMax(iPxLrPos, FFX_MIN16_F(fReactiveMax));

#endif //OPT_PRECOMPUTE_REACTIVE_MAX && !OPT_USE_EVAL_ACCUMULATION_REACTIVENESS
}
