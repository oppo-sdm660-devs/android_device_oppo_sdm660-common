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
// Alignment returns half-pixel offsets. The full-resolution MFNR loop uses
// integer bilinear interpolation, avoiding per-sample floor/clamp/floating point.
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
float quality(const Image &im, int left, int top, int width, int height) {
    double sum = 0;
    unsigned n = 0;
    const int step = std::max(2, std::min(width, height) / 96);
    for (int y = top + 2; y < top + height - 2; y += step)
        for (int x = left + 2; x < left + width - 2; x += step) {
            sum += std::min(64.f, std::fabs(patch(im,x-1,y)-patch(im,x+1,y)) +
                    std::fabs(patch(im,x,y-1)-patch(im,x,y+1)));
            ++n;
        }
    return n ? float(sum / n) : 0;
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
bool validFace(const Algo::Face &face, const Image &im) {
    return face.confidence >= 50 && face.left >= 0 && face.top >= 0 &&
        face.width >= 8 && face.height >= 8 &&
        int64_t(face.left)+face.width <= im.width && int64_t(face.top)+face.height <= im.height;
}
float mask(const Algo::Face &face, float x, float y) {
    const float dx=(x-(face.left+face.width*.5f))/(face.width*.5f);
    const float dy=(y-(face.top+face.height*.5f))/(face.height*.5f);
    return std::max(0.f,std::min(1.f,(1-dx*dx-dy*dy)*3));
}
// Match geometry rather than relying on SIMPLE FD's unstable IDs. Strong
// overlap and a small local residual are required before borrowing face pixels.
bool matchingFace(const Algo::Face &a, const Algo::FaceSet &set,
        const Image &im, const Shift &shift) {
    for (unsigned i=0;i<set.count;++i) {
        const auto &b=set.faces[i];
        if (!validFace(b,im)) continue;
        const float l=std::max(float(a.left),b.left-shift.x);
        const float t=std::max(float(a.top),b.top-shift.y);
        const float r=std::min(float(a.left+a.width),b.left+b.width-shift.x);
        const float d=std::min(float(a.top+a.height),b.top+b.height-shift.y);
        const float overlap=std::max(0.f,r-l)*std::max(0.f,d-t);
        if (overlap > .8f*std::max(float(a.width)*a.height,float(b.width)*b.height)) return true;
    }
    return false;
}
} // namespace

int32_t QCamera3StillAlgo::process(const Image *images, uint32_t count,
        const Image &dst, const Options &options, Result *result,
        const std::atomic<bool> *cancel) {
    if (!images || !count || count>MAX_FRAMES || options.referenceIndex>=count ||
            options.beautyStrength>100 || options.portraitLightStrength>100 ||
            !QCamera3HdrAlgo::validImage(dst)) return -EINVAL;
    const Image &ref=images[options.referenceIndex];
    const unsigned scale=options.superResolution?2:1;
    if (uint64_t(ref.width)*scale!=dst.width || uint64_t(ref.height)*scale!=dst.height ||
            ref.width<40 || ref.height<40) return -EINVAL;
    for (unsigned i=0;i<count;++i) {
        if (!QCamera3HdrAlgo::validImage(images[i]) || images[i].width!=ref.width ||
                images[i].height!=ref.height) return -EINVAL;
        if (options.faces && options.faces[i].count>MAX_FACES) return -EINVAL;
    }
    if (stopped(cancel)) return -ECANCELED;
    const uint64_t ySize=uint64_t(dst.width)*dst.height;
    const uint64_t bytes=ySize+ySize/2;
    // Scratch is bounded independently of buffer lengths supplied by callers.
    if (bytes>128*1024*1024 || bytes>SIZE_MAX) return -E2BIG;
    std::unique_ptr<uint8_t,decltype(&free)> memory(static_cast<uint8_t *>(malloc(bytes)),free);
    if (!memory) return -ENOMEM;
    Image output={memory.get(),size_t(bytes),dst.width,dst.height,0,size_t(ySize),
        dst.width,dst.width,dst.height,dst.height/2};
    Result report;
    report.referenceIndex=options.referenceIndex;
    report.bestFrameIndex=options.referenceIndex;
    float best=quality(ref,0,0,ref.width,ref.height);
    Shift shifts[MAX_FRAMES];
    for (unsigned i=0;i<count;++i) {
        shifts[i]=i==options.referenceIndex?Shift{}:align(ref,images[i],cancel);
        if (i==options.referenceIndex) shifts[i].usable=true;
        if (stopped(cancel)) return -ECANCELED;
        if (shifts[i].usable) {
            ++report.acceptedFrames;
            const float score=quality(images[i],0,0,ref.width,ref.height);
            if (score>best) { best=score; report.bestFrameIndex=i; }
        }
    }
    for (unsigned y=0;y<dst.height;++y) {
        if (stopped(cancel)) return -ECANCELED;
        for (unsigned x=0;x<dst.width;++x) {
            if (scale==1) {
                const int anchor=ref.data[ref.yOffset+size_t(y)*ref.yStride+x];
                int sum=anchor*32,weights=32;
                for (unsigned i=0;i<count;++i) {
                    if (i==options.referenceIndex || !shifts[i].usable) continue;
                    const int sx2=int(x*2)+int(shifts[i].x*2);
                    const int sy2=int(y*2)+int(shifts[i].y*2);
                    if (sx2<0 || sy2<0 || sx2>=int(ref.width*2-2) || sy2>=int(ref.height*2-2)) continue;
                    const int value=sampleHalf(images[i],sx2,sy2);
                    const int difference=std::abs(value-anchor);
                    if (difference>=32) continue;
                    const int weight=16-difference/2;
                    sum+=value*weight; weights+=weight;
                }
                output.data[size_t(y)*dst.width+x]=uint8_t((sum+weights/2)/weights);
                continue;
            }
            const float rx=float(x)/scale,ry=float(y)/scale;
            const float anchor=sample(ref,rx,ry), local=patch(ref,rx,ry);
            float sum=anchor*2, weights=2;
            for (unsigned i=0;i<count;++i) {
                if (i==options.referenceIndex || !shifts[i].usable) continue;
                const float sx=rx+shifts[i].x,sy=ry+shifts[i].y;
                if (sx<1 || sy<1 || sx>=ref.width-2 || sy>=ref.height-2) continue;
                const float difference=std::fabs(local-patch(images[i],sx,sy));
                const float value=sample(images[i],sx,sy);
                // Pixel and low-pass tests suppress moving objects and bad matches.
                if (difference>14 || std::fabs(value-anchor)>40) continue;
                const float weight=(14-difference)/14;
                sum+=value*weight; weights+=weight;
            }
            output.data[size_t(y)*dst.width+x]=byte(sum/weights);
        }
    }
    // NV21 chroma is aligned at half the luma resolution; mixing V and U would
    // create hue shifts, so each channel is addressed independently.
    for (unsigned y=0;y<dst.height/2;++y) {
        if (stopped(cancel)) return -ECANCELED;
        for (unsigned x=0;x<dst.width;x+=2) for (unsigned c=0;c<2;++c) {
            const unsigned rx=x/scale/2*2,ry=y/scale;
            const float anchor=ref.data[ref.uvOffset+size_t(ry)*ref.uvStride+rx+c];
            float sum=anchor*2,weights=2;
            for (unsigned i=0;i<count;++i) {
                if (i==options.referenceIndex || !shifts[i].usable) continue;
                const int sx=int(rx)+int(std::round(shifts[i].x/2))*2;
                const int sy=int(ry)+int(std::round(shifts[i].y/2));
                if (sx<0 || sx+1>=int(ref.width) || sy<0 || sy>=int(ref.height/2)) continue;
                const int lx=int(rx)+int(std::round(shifts[i].x));
                const int ly=int(ry*2)+int(std::round(shifts[i].y));
                if (lx<0 || ly<0 || lx>=int(ref.width) || ly>=int(ref.height)) continue;
                const int lumaError=std::abs(int(ref.data[ref.yOffset+size_t(ry*2)*ref.yStride+rx])-
                    int(images[i].data[images[i].yOffset+size_t(ly)*images[i].yStride+lx]));
                if (lumaError>24) continue;
                const float value=images[i].data[images[i].uvOffset+size_t(sy)*images[i].uvStride+sx+c];
                if (std::fabs(value-anchor)>24) continue;
                sum+=value; ++weights;
            }
            output.data[output.uvOffset+size_t(y)*dst.width+x+c]=byte(sum/weights);
        }
    }
    // Iterative back projection uses the measured fractional sample locations.
    // If there is no subpixel diversity, keep an interpolated output and report
    // reconstructed=false instead of claiming new recoverable detail.
    bool diversity=false;
    for (unsigned i=0;i<count;++i) if (shifts[i].usable &&
        (std::fabs(shifts[i].x-std::round(shifts[i].x))>.1f ||
         std::fabs(shifts[i].y-std::round(shifts[i].y))>.1f)) diversity=true;
    if (options.superResolution && diversity) {
        for (unsigned pass=0;pass<2;++pass) for (unsigned i=0;i<count;++i) {
            if (!shifts[i].usable) continue;
            for (unsigned y=1;y+2<ref.height;++y) {
                if (stopped(cancel)) return -ECANCELED;
                for (unsigned x=1;x+2<ref.width;++x) {
                    const float ox=(x-shifts[i].x)*2,oy=(y-shifts[i].y)*2;
                    const int ix=int(std::floor(ox)),iy=int(std::floor(oy));
                    if (ix<1 || iy<1 || ix+2>=int(dst.width) || iy+2>=int(dst.height)) continue;
                    const float residual=pixel(images[i],x,y)-sample(output,ox,oy);
                    if (std::fabs(residual)>20) continue;
                    const float fx=ox-ix,fy=oy-iy;
                    for (unsigned dy=0;dy<2;++dy) for (unsigned dx=0;dx<2;++dx) {
                        auto &v=output.data[size_t(iy+dy)*dst.width+ix+dx];
                        v=byte(v+residual*.35f*(dx?fx:1-fx)*(dy?fy:1-fy));
                    }
                }
            }
        }
        report.reconstructed=true;
    }
    if (options.enhanceDetails && !options.superResolution) {
        // Reference-derived high-frequency detail avoids in-place filter feedback.
        for (unsigned y=1;y+1<dst.height;++y) {
            if (stopped(cancel)) return -ECANCELED;
            for (unsigned x=1;x+1<dst.width;++x) {
                const float detail=pixel(ref,x,y)-patch(ref,x,y);
                if (std::fabs(detail)>3 && std::fabs(detail)<28) {
                    auto &v=output.data[size_t(y)*dst.width+x];
                    v=byte(v+std::max(-5.f,std::min(5.f,detail*.3f)));
                }
            }
        }
    }
    if (options.faces) for (unsigned f=0;f<options.faces[options.referenceIndex].count;++f) {
        const auto &face=options.faces[options.referenceIndex].faces[f];
        if (!validFace(face,ref)) continue;
        unsigned donor=options.referenceIndex;
        float score=quality(ref,face.left,face.top,face.width,face.height);
        if (options.fuseFaces) for (unsigned i=0;i<count;++i) {
            if (i==options.referenceIndex || !shifts[i].usable ||
                !matchingFace(face,options.faces[i],images[i],shifts[i])) continue;
            const float q=quality(images[i],int(face.left+shifts[i].x),int(face.top+shifts[i].y),face.width,face.height);
            if (q>score*1.15f) { score=q; donor=i; }
        }
        bool usedDonor=false;
        for (int y=face.top*scale;y<(face.top+face.height)*int(scale);++y) {
            if (stopped(cancel)) return -ECANCELED;
            for (int x=face.left*scale;x<(face.left+face.width)*int(scale);++x) {
                const float rx=float(x)/scale,ry=float(y)/scale,alpha=mask(face,rx,ry);
                if (alpha<=0) continue;
                auto &v=output.data[size_t(y)*dst.width+x];
                float value=v;
                if (donor!=options.referenceIndex) {
                    const float candidate=sample(images[donor],rx+shifts[donor].x,ry+shifts[donor].y);
                    if (std::fabs(patch(ref,rx,ry)-patch(images[donor],rx+shifts[donor].x,ry+shifts[donor].y))<12) {
                        value=value*(1-alpha)+candidate*alpha;
                        usedDonor=true;
                    }
                }
                if (options.beautyStrength) {
                    float smooth=0,weights=0;
                    for (int dy=-2;dy<=2;++dy) for (int dx=-2;dx<=2;++dx) {
                        const float p=sample(ref,rx+dx,ry+dy);
                        const float w=std::max(0.f,1-std::fabs(p-pixel(ref,rx,ry))/18);
                        smooth+=p*w; weights+=w;
                    }
                    const float amount=options.beautyStrength*.006f*alpha;
                    value=value*(1-amount)+(weights?smooth/weights:value)*amount;
                }
                if (options.portraitLightStrength) {
                    const float light=options.portraitLightStrength*.12f*alpha*
                        (1-(rx-face.left)/face.width*.5f);
                    value+=light*std::max(0.f,1-value/255);
                }
                v=byte(value);
            }
        }
        if (usedDonor) ++report.replacedFaces;
    }
    if (stopped(cancel)) return -ECANCELED;
    for (unsigned y=0;y<dst.height;++y)
        memcpy(dst.data+dst.yOffset+size_t(y)*dst.yStride,output.data+size_t(y)*dst.width,dst.width);
    for (unsigned y=0;y<dst.height/2;++y)
        memcpy(dst.data+dst.uvOffset+size_t(y)*dst.uvStride,output.data+output.uvOffset+size_t(y)*dst.width,dst.width);
    if (result) *result=report;
    return 0;
}
} // namespace qcamera
