/* The SDK's paired-single matrix functions are MWCC assembly: here they are
 * the SDK's C versions (libs/dolphin/src/dolphin/mtx, built without their
 * assembly) */

#include <dolphin/mtx.h>

void PSMTXIdentity(Mtx m)
{
    C_MTXIdentity(m);
}

void PSMTXCopy(Mtx src, Mtx dst)
{
    C_MTXCopy(src, dst);
}

void PSMTXConcat(Mtx a, Mtx b, Mtx ab)
{
    C_MTXConcat(a, b, ab);
}

void PSMTXTranspose(Mtx src, Mtx xPose)
{
    C_MTXTranspose(src, xPose);
}

u32 PSMTXInverse(Mtx src, Mtx inv)
{
    return C_MTXInverse(src, inv);
}

u32 PSMTXInvXpose(Mtx src, Mtx invX)
{
    return C_MTXInvXpose(src, invX);
}

void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    C_MTXRotTrig(m, axis, sinA, cosA);
}

void PSMTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
    C_MTXRotAxisRad(m, axis, rad);
}

/* The SDK has no C version of this one */
void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    m[0][0] = 1.0f;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = xT;
    m[1][0] = 0.0f;
    m[1][1] = 1.0f;
    m[1][2] = 0.0f;
    m[1][3] = yT;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 1.0f;
    m[2][3] = zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    C_MTXScale(m, xS, yS, zS);
}

void PSMTXQuat(Mtx m, QuaternionPtr q)
{
    C_MTXQuat(m, q);
}

void PSMTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    C_MTXMultVec(m, src, dst);
}

void PSMTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    C_MTXMultVecSR(m, src, dst);
}

void PSMTXMultVecArray(Mtx m, Vec* srcBase, Vec* dstBase, u32 count)
{
    C_MTXMultVecArray(m, srcBase, dstBase, count);
}

void PSVECAdd(Vec* a, Vec* b, Vec* ab)
{
    C_VECAdd(a, b, ab);
}

void PSVECSubtract(Vec* a, Vec* b, Vec* a_b)
{
    C_VECSubtract(a, b, a_b);
}

void PSVECScale(Vec* src, Vec* dst, f32 scale)
{
    C_VECScale(src, dst, scale);
}

void PSVECNormalize(Vec* src, Vec* unit)
{
    C_VECNormalize(src, unit);
}

f32 PSVECSquareMag(Vec* v)
{
    return C_VECSquareMag(v);
}

f32 PSVECMag(Vec* v)
{
    return C_VECMag(v);
}

f32 PSVECDotProduct(Vec* a, Vec* b)
{
    return C_VECDotProduct(a, b);
}

void PSVECCrossProduct(Vec* a, Vec* b, Vec* axb)
{
    C_VECCrossProduct(a, b, axb);
}

f32 PSVECSquareDistance(Vec* a, Vec* b)
{
    return C_VECSquareDistance(a, b);
}
