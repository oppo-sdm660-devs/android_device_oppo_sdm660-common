/* SPDX-License-Identifier: BSD-3-Clause */
#include "QCamera3StillAlgo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <memory>

namespace qcamera {
namespace {
using Algo = QCamera3StillAlgo;
using Image = Algo::Image;
struct Shift { float x = 0, y = 0, error = 0; bool usable = false; };
// Alignment returns half-pixel offsets, hence the x2 integer coordinates.
inline int sampleHalf(const Image &im, int x2, int y2) {
    const unsigned x=unsigned(x2)/2,y=unsigned(y2)/2;
    const uint8_t *p=im.data+im.yOffset+size_t(y)*im.yStride+x;
    if ((x2&1)==0 && (y2&1)==0) return p[0];
    if ((y2&1)==0) return (p[0]+p[1]+1)/2;
    if ((x2&1)==0) return (p[0]+p[im.yStride]+1)/2;
    return (p[0]+p[1]+p[im.yStride]+p[im.yStride+1]+2)/4;
}
bool stopped(const std::atomic<bool> *flag) { return flag && flag->load(); }
uint8_t byte(float value) { return uint8_t(std::max(0.f, std::min(255.f, value)) + .5f); }
float pixel(const Image &im, int x, int y) {
    x = std::max(0, std::min(int(im.width) - 1, x));
    y = std::max(0, std::min(int(im.height) - 1, y));
    return im.data[im.yOffset + size_t(y) * im.yStride + x];
}
float sample(const Image &im, float x, float y) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    return pixel(im, ix, iy) * (1 - fx) * (1 - fy) +
        pixel(im, ix + 1, iy) * fx * (1 - fy) +
        pixel(im, ix, iy + 1) * (1 - fx) * fy +
        pixel(im, ix + 1, iy + 1) * fx * fy;
}
float patch(const Image &im, float x, float y) {
    return (sample(im, x, y) * 4 + sample(im, x-1, y) + sample(im, x+1, y) +
            sample(im, x, y-1) + sample(im, x, y+1)) / 8;
}
float alignmentError(const Image &ref, const Image &im, float dx, float dy) {
    double sum = 0;
    unsigned n = 0;
    const unsigned step = std::max(2U, std::max(ref.width,ref.height) / 80);
    for (unsigned y = 18; y + 18 < ref.height; y += step)
        for (unsigned x = 18; x + 18 < ref.width; x += step) {
            // Truncation limits foreground influence on the camera-motion fit.
            sum += std::min(24.f, std::fabs(patch(ref,x,y)-patch(im,x+dx,y+dy)));
            ++n;
        }
    return n ? float(sum/n) : 100.f;
}
Shift align(const Image &ref, const Image &im, const std::atomic<bool> *cancel) {
    Shift shift;
    shift.error = alignmentError(ref, im, 0, 0);
    if (shift.error > 1.f) {
        for (int y = -16; y <= 16 && !stopped(cancel); y += 4)
            for (int x = -16; x <= 16; x += 4) {
                const float error = alignmentError(ref, im, x, y);
                if (error < shift.error - .05f) { shift.x=x; shift.y=y; shift.error=error; }
            }
        for (float step : {1.f, .5f}) {
            const float cx=shift.x, cy=shift.y;
            for (int y=-3; y<=3; ++y) for (int x=-3; x<=3; ++x) {
                const float nx=cx+x*step, ny=cy+y*step;
                if (std::fabs(nx)>20 || std::fabs(ny)>20) continue;
                const float error=alignmentError(ref,im,nx,ny);
                if (error < shift.error - .025f) { shift.x=nx; shift.y=ny; shift.error=error; }
            }
        }
    }
    shift.usable = shift.error < 14.f;
    return shift;
}
} // namespace

int32_t QCamera3StillAlgo::process(const Image *images, uint32_t count,
        const Image &dst, const Options &options, Result *result,
        const std::atomic<bool> *cancel) {
    if (!images || !count || count>MAX_FRAMES || options.referenceIndex>=count ||
            !QCamera3HdrAlgo::validImage(dst)) return -EINVAL;
    const Image &ref=images[options.referenceIndex];
    if (ref.width!=dst.width || ref.height!=dst.height ||
            ref.width<40 || ref.height<40) return -EINVAL;
    for (unsigned i=0;i<count;++i) {
        if (!QCamera3HdrAlgo::validImage(images[i]) || images[i].width!=ref.width ||
                images[i].height!=ref.height) return -EINVAL;
    }
    if (stopped(cancel)) return -ECANCELED;
    const uint64_t ySize=uint64_t(dst.width)*dst.height;
    const uint64_t bytes=ySize+ySize/2;
    // Scratch is bounded independently of caller buffer lengths.
    if (bytes>128*1024*1024 || bytes>SIZE_MAX) return -E2BIG;
    std::unique_ptr<uint8_t,decltype(&free)> memory(static_cast<uint8_t *>(malloc(bytes)),free);
    if (!memory) return -ENOMEM;
    Image output={memory.get(),size_t(bytes),dst.width,dst.height,0,size_t(ySize),
        dst.width,dst.width,dst.height,dst.height/2};
    Result report;
    report.referenceIndex=options.referenceIndex;
    Shift shifts[MAX_FRAMES];
    for (unsigned i=0;i<count;++i) {
        shifts[i]=i==options.referenceIndex?Shift{}:align(ref,images[i],cancel);
        if (i==options.referenceIndex) shifts[i].usable=true;
        if (stopped(cancel)) return -ECANCELED;
        if (shifts[i].usable) ++report.acceptedFrames;
    }
    struct Offset {
        unsigned frame;
        int x2, y2, chromaX, chromaY, lumaX, lumaY;
    } offsets[MAX_FRAMES];
    unsigned offsetCount=0;
    for (unsigned i=0;i<count;++i) {
        if (i==options.referenceIndex || !shifts[i].usable) continue;
        offsets[offsetCount++]={i,int(shifts[i].x*2),int(shifts[i].y*2),
            int(std::round(shifts[i].x/2))*2,int(std::round(shifts[i].y/2)),
            int(std::round(shifts[i].x)),int(std::round(shifts[i].y))};
    }
    for (unsigned y=0;y<dst.height;++y) {
        if (stopped(cancel)) return -ECANCELED;
        for (unsigned x=0;x<dst.width;++x) {
            const int anchor=ref.data[ref.yOffset+size_t(y)*ref.yStride+x];
            int sum=anchor*32,weights=32;
            for (unsigned i=0;i<offsetCount;++i) {
                const auto &offset=offsets[i];
                const int sx2=int(x*2)+offset.x2;
                const int sy2=int(y*2)+offset.y2;
                if (sx2<0 || sy2<0 || sx2>=int(ref.width*2-2) || sy2>=int(ref.height*2-2)) continue;
                const int value=sampleHalf(images[offset.frame],sx2,sy2);
                const int difference=std::abs(value-anchor);
                if (difference>=32) continue;
                const int weight=16-difference/2;
                sum+=value*weight; weights+=weight;
            }
            output.data[size_t(y)*dst.width+x]=uint8_t((sum+weights/2)/weights);
        }
    }
    // V/U retain separate accumulators; their luma rejection and source address are shared.
    for (unsigned y=0;y<dst.height/2;++y) {
        if (stopped(cancel)) return -ECANCELED;
        const uint8_t *refChroma=ref.data+ref.uvOffset+size_t(y)*ref.uvStride;
        const uint8_t *refLuma=ref.data+ref.yOffset+size_t(y*2)*ref.yStride;
        uint8_t *outputChroma=output.data+output.uvOffset+size_t(y)*dst.width;
        for (unsigned x=0;x<dst.width;x+=2) {
            const float anchorV=refChroma[x],anchorU=refChroma[x+1];
            float sumV=anchorV*2,sumU=anchorU*2,weightsV=2,weightsU=2;
            for (unsigned i=0;i<offsetCount;++i) {
                const auto &offset=offsets[i];
                const Image &source=images[offset.frame];
                const int sx=int(x)+offset.chromaX;
                const int sy=int(y)+offset.chromaY;
                if (sx<0 || sx+1>=int(ref.width) || sy<0 || sy>=int(ref.height/2)) continue;
                const int lx=int(x)+offset.lumaX;
                const int ly=int(y*2)+offset.lumaY;
                if (lx<0 || ly<0 || lx>=int(ref.width) || ly>=int(ref.height)) continue;
                const int lumaError=std::abs(int(refLuma[x])-
                    int(source.data[source.yOffset+size_t(ly)*source.yStride+lx]));
                if (lumaError>24) continue;
                const uint8_t *chroma=source.data+source.uvOffset+size_t(sy)*source.uvStride+sx;
                const float valueV=chroma[0],valueU=chroma[1];
                if (std::fabs(valueV-anchorV)<=24) { sumV+=valueV; ++weightsV; }
                if (std::fabs(valueU-anchorU)<=24) { sumU+=valueU; ++weightsU; }
            }
            outputChroma[x]=byte(sumV/weightsV);
            outputChroma[x+1]=byte(sumU/weightsU);
        }
    }
    if (stopped(cancel)) return -ECANCELED;
    for (unsigned y = 0; y < dst.height; ++y)
        report.pixelsChanged |= memcmp(ref.data + ref.yOffset + size_t(y) * ref.yStride,
                output.data + size_t(y) * dst.width, dst.width) != 0;
    for (unsigned y = 0; y < dst.height / 2; ++y)
        report.pixelsChanged |= memcmp(ref.data + ref.uvOffset + size_t(y) * ref.uvStride,
                output.data + output.uvOffset + size_t(y) * dst.width, dst.width) != 0;
    for (unsigned y=0;y<dst.height;++y)
        memcpy(dst.data+dst.yOffset+size_t(y)*dst.yStride,output.data+size_t(y)*dst.width,dst.width);
    for (unsigned y=0;y<dst.height/2;++y)
        memcpy(dst.data+dst.uvOffset+size_t(y)*dst.uvStride,output.data+output.uvOffset+size_t(y)*dst.width,dst.width);
    if (result) *result=report;
    return 0;
}
} // namespace qcamera
