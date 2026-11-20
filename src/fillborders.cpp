#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <new>

#include <VapourSynth4.h>
#include <VSHelper4.h>


enum FillMode {
   ModeFillMargins,
   ModeRepeat,
   ModeMirror,
   ModeFixBorders,
};


enum InterlacedValues {
    InterlacedAuto = -1,
    NotInterlaced = 0,
    Interlaced = 1,
};


typedef void (*FillBordersFunction)(uint8_t *dstp, int width, int height, intptr_t stride, int left, int right, int top, int bottom, int mode, int interlaced);


struct FillBordersData {
   VSNode *node;
   const VSVideoInfo *vi;

   int left;
   int right;
   int top;
   int bottom;
   int mode;
   int interlaced;

   FillBordersFunction fill;
};

// Rounded up, because a chroma sample covering any part of the filled luma area
// has to be filled as well: rounding down leaves the outermost chroma line
// untouched whenever the border is odd.
static inline int64_t subsampledBorder(int64_t border, int subsampling) {
    return (border + (INT64_C(1) << subsampling) - 1) >> subsampling;
}

// IEEE 754 binary16. No portable native type exists for it, so samples are kept
// as raw bits and converted to and from float around every computation. Fills
// that only copy samples never look at the values and stay bit exact.
struct Half {
    uint16_t bits;
};

static_assert(sizeof(Half) == sizeof(uint16_t), "Half must occupy exactly one 16 bit sample.");

static inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    int exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;

    if (exponent == 0x1F) {
        // Infinity or NaN. The payload is carried over so a NaN stays a NaN.
        bits = sign | 0x7F800000 | (mantissa << 13);
    } else if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal half, normal float: shift the mantissa up until the
            // implicit one appears and lower the exponent to match.
            exponent = 1;
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                exponent--;
            }
            bits = sign | ((uint32_t)(exponent + (127 - 15)) << 23) | ((mantissa & 0x3FF) << 13);
        }
    } else {
        bits = sign | ((uint32_t)(exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline uint16_t floatToHalf(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign = (bits >> 16) & 0x8000;
    const int rawExponent = (bits >> 23) & 0xFF;
    uint32_t mantissa = bits & 0x7FFFFF;

    if (rawExponent == 0xFF) {
        // Infinity or NaN. The payload is forced non-zero for a NaN so that
        // dropping the low mantissa bits cannot turn it into an infinity.
        return (uint16_t)(sign | 0x7C00 | (mantissa ? ((mantissa >> 13) | 0x200) : 0));
    }

    int exponent = rawExponent - 127 + 15;

    // Too large for half, including every infinity produced by rounding below.
    if (exponent >= 0x1F)
        return (uint16_t)(sign | 0x7C00);

    if (exponent <= 0) {
        // Anything below half's smallest subnormal rounds to zero. This also
        // covers zero itself and every float subnormal.
        if (exponent < -10)
            return (uint16_t)sign;

        // Subnormal half: shift the mantissa, implicit one included, down into
        // place and round to nearest, ties to even. Rounding up to 0x400 lands
        // exactly on the smallest normal half, which is the intended result.
        mantissa |= 0x800000;

        const int shift = 14 - exponent;
        const uint32_t result = mantissa >> shift;
        const uint32_t dropped = mantissa & ((UINT32_C(1) << shift) - 1);
        const uint32_t halfway = UINT32_C(1) << (shift - 1);

        return (uint16_t)(sign | (result + (dropped > halfway || (dropped == halfway && (result & 1)))));
    }

    // Round to nearest, ties to even. A carry out of the mantissa bumps the
    // exponent, which may in turn overflow to infinity.
    uint32_t result = mantissa >> 13;
    const uint32_t dropped = mantissa & 0x1FFF;

    if (dropped > 0x1000 || (dropped == 0x1000 && (result & 1))) {
        result++;

        if (result == 0x400) {
            result = 0;
            exponent++;

            if (exponent >= 0x1F)
                return (uint16_t)(sign | 0x7C00);
        }
    }

    return (uint16_t)(sign | ((uint32_t)exponent << 10) | result);
}

// How samples are read, written and averaged. Integer formats compute in int and
// keep the original rounded division; float and half compute in float, where the
// rounding offset would be wrong, and half additionally converts on access.
template <typename PixelType>
struct PixelTraits {
    typedef int ArithType;

    static inline ArithType load(PixelType p) { return (ArithType)p; }
    static inline PixelType store(ArithType v) { return (PixelType)v; }
    static inline ArithType avg(ArithType sum, int offset, int divisor) { return (sum + offset) / divisor; }
};

template <>
struct PixelTraits<float> {
    typedef float ArithType;

    static inline float load(float p) { return p; }
    static inline float store(float v) { return v; }
    static inline float avg(float sum, [[maybe_unused]] int offset, int divisor) { return sum / divisor; }
};

template <>
struct PixelTraits<Half> {
    typedef float ArithType;

    static inline float load(Half p) { return halfToFloat(p.bits); }
    static inline Half store(float v) { Half h = { floatToHalf(v) }; return h; }
    static inline float avg(float sum, [[maybe_unused]] int offset, int divisor) { return sum / divisor; }
};

template <typename T>
static inline T absDiff(T a, T b) {
    return a > b ? a - b : b - a;
}

template <typename PixelType>
static inline void fillPixels(PixelType *ptr, PixelType value, size_t num) {
    while (num-- > 0)
        *ptr++ = value;
}

template <>
inline void fillPixels<uint8_t>(uint8_t *ptr, uint8_t value, size_t num) {
    memset(ptr, value, num);
}

template <typename PixelType>
static void fillBorders(uint8_t *dstp8, int width, int height, intptr_t stride, int left, int right, int top, int bottom, int mode, int interlaced) {
   typedef PixelTraits<PixelType> Pixel;
   typedef typename Pixel::ArithType ArithType;

   int x, y;
   PixelType *dstp = (PixelType *)dstp8;
   stride /= sizeof(PixelType);

   // fillmargins/fixborders copy the first pixel and the last 8 columns verbatim;
   // clamp that count so planes narrower than 8 don't index before the row.
   const int copyLen = width < 8 ? width : 8;

   if (mode == ModeFillMargins) {
      for (y = top; y < height - bottom; y++) {
         fillPixels(dstp + stride*y, (dstp + stride*y)[left], left);
         fillPixels(dstp + stride*y + width - right, (dstp + stride*y + width - right)[-1], right);
      }

      for (y = top - 1; y >= 0; y--) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y+1 + interlaced)];
         memcpy(dstp + stride*y + width - copyLen, dstp + stride*(y+1 + interlaced) + width - copyLen, copyLen * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - copyLen; x++) {
            ArithType prev = Pixel::load(dstp[stride*(y+1 + interlaced) + x - 1]);
            ArithType cur  = Pixel::load(dstp[stride*(y+1 + interlaced) + x]);
            ArithType next = Pixel::load(dstp[stride*(y+1 + interlaced) + x + 1]);
            dstp[stride*y + x] = Pixel::store(Pixel::avg(3*prev + 2*cur + 3*next, 4, 8));
         }
      }

      for (y = height - bottom; y < height; y++) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y-1 - interlaced)];
         memcpy(dstp + stride*y + width - copyLen, dstp + stride*(y-1 - interlaced) + width - copyLen, copyLen * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - copyLen; x++) {
            ArithType prev = Pixel::load(dstp[stride*(y-1 - interlaced) + x - 1]);
            ArithType cur  = Pixel::load(dstp[stride*(y-1 - interlaced) + x]);
            ArithType next = Pixel::load(dstp[stride*(y-1 - interlaced) + x + 1]);
            dstp[stride*y + x] = Pixel::store(Pixel::avg(3*prev + 2*cur + 3*next, 4, 8));
         }
      }
   } else if (mode == ModeRepeat) {
      for (y = top; y < height - bottom; y++) {
         fillPixels(dstp + stride*y, (dstp + stride*y)[left], left);
         fillPixels(dstp + stride*y + width - right, (dstp + stride*y + width - right)[-1], right);
      }

      for (y = top - 1; y >= 0; y--) {
         memcpy(dstp + stride*y, dstp + stride*(y+1 + interlaced), stride * sizeof(PixelType));
      }

      for (y = height - bottom; y < height; y++) {
         memcpy(dstp + stride*y, dstp + stride*(y-1 - interlaced), stride * sizeof(PixelType));
      }
   } else if (mode == ModeMirror) {
      for (y = top; y < height - bottom; y++) {
         for (x = 0; x < left; x++) {
            dstp[stride*y + x] = dstp[stride*y + left*2 - 1 - x];
         }

         for (x = 0; x < right; x++) {
            dstp[stride*y + width - right + x] = dstp[stride*y + width - right - 1 - x];
         }
      }

      if (interlaced) {
          int field0_top = top / 2 + top % 2;
          int field1_top = top / 2;

          for (y = 0; y < field0_top; y++)
              memcpy(dstp + stride * y * 2,
                     dstp + stride * (field0_top * 2 - 1 - y) * 2,
                     stride * sizeof(PixelType));

          for (y = 0; y < field1_top; y++)
              memcpy(dstp + stride * y * 2 + stride,
                     dstp + stride * (field1_top * 2 - 1 - y) * 2 + stride,
                     stride * sizeof(PixelType));

          int field0_bottom = bottom / 2;
          int field1_bottom = bottom / 2 + bottom % 2;

          for (y = 0; y < field0_bottom; y++)
              memcpy(dstp + stride * (height - field0_bottom * 2 + y * 2),
                     dstp + stride * (height - field0_bottom * 2 - 2 - y * 2),
                     stride * sizeof(PixelType));

          for (y = 0; y < field1_bottom; y++)
              memcpy(dstp + stride * (height - field1_bottom * 2 + y * 2) + stride,
                     dstp + stride * (height - field1_bottom * 2 - 2 - y * 2) + stride,
                     stride * sizeof(PixelType));
      } else {
          for (y = 0; y < top; y++) {
             memcpy(dstp + stride*y, dstp + stride*(top*2 - 1 - y), stride * sizeof(PixelType));
          }

          for (y = 0; y < bottom; y++) {
             memcpy(dstp + stride*(height - bottom + y), dstp + stride*(height - bottom - 1 - y), stride * sizeof(PixelType));
          }
      }
   } else if (mode == ModeFixBorders) {

      for (x = left - 1; x >= 0; x--) {
	 // copy pixels until top + 3/bottom + 3
	 // this way we avoid darkened corners when all sides need filling
	 for (y = 0; y < top + 3; y++) {
	    dstp[stride*y + x] = dstp[stride*y + x + 1];
	 }
	 for (y = bottom + 3; y > 0; y--) {
            dstp[stride*(height - y) + x] = dstp[stride*(height - y) + x + 1];
	 }

         // weighted average for the rest
         for (y = top + 3; y < height - (bottom + 3); y++) {
            ArithType prev = Pixel::load(dstp[stride*(y - 1 - interlaced) +x+1]);
            ArithType cur  = Pixel::load(dstp[stride*(y) +x+1]);
            ArithType next = Pixel::load(dstp[stride*(y + 1 + interlaced) +x+1]);

            ArithType ref_prev = Pixel::load(dstp[stride*(y - 1 - interlaced) +x+2]);
            ArithType ref_cur  = Pixel::load(dstp[stride*(y) +x+2]);
            ArithType ref_next = Pixel::load(dstp[stride*(y + 1 + interlaced) +x+2]);

	    ArithType fill_prev = Pixel::avg(5*prev + 3*cur + 1*next, 4, 9);
	    ArithType fill_cur = Pixel::avg(1*prev + 3*cur + 1*next, 2, 5);
	    ArithType fill_next = Pixel::avg(1*prev + 3*cur + 5*next, 4, 9);

	    ArithType blur_prev = Pixel::avg(2 * ref_prev + ref_cur + Pixel::load(dstp[stride*(y - 2 - interlaced) + x+2]), 0, 4);
	    ArithType blur_next = Pixel::avg(2 * ref_next + ref_cur + Pixel::load(dstp[stride*(y + 2 + interlaced) + x+2]), 0, 4);

	    ArithType diff_next = absDiff(ref_next, fill_cur);
	    ArithType diff_prev = absDiff(ref_prev, fill_cur);
	    ArithType thr_next = absDiff(ref_next, blur_next);
	    ArithType thr_prev = absDiff(ref_prev, blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = Pixel::store(fill_prev);
		}
		else {
		    dstp[stride*y + x] = Pixel::store(fill_next);
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = Pixel::store(fill_next);
	    } else {
		dstp[stride*y + x] = Pixel::store(fill_cur);
	    }
         }
      }

      for (x = width - right; x < width; x++) {
	 // copy pixels until top + 3/bottom + 3
	 // this way we avoid darkened corners when all sides need filling
	 for (y = 0; y < top + 3; y++) {
            dstp[stride*y + x] = dstp[stride*y + x - 1];
	 }
	 for (y = bottom + 3; y > 0; y--) {
	    dstp[stride*(height - y) + x] = dstp[stride*(height - y) + x - 1];
	 }

         // weighted average for the rest
         for (y = top + 3; y < height - (bottom + 3); y++) {
            ArithType prev = Pixel::load(dstp[stride*(y - 1 - interlaced) + x-1]);
            ArithType cur  = Pixel::load(dstp[stride*(y) + x-1]);
            ArithType next = Pixel::load(dstp[stride*(y + 1 + interlaced) + x-1]);

            ArithType ref_prev = Pixel::load(dstp[stride*(y - 1 - interlaced) + x-2]);
            ArithType ref_cur  = Pixel::load(dstp[stride*(y) + x-2]);
            ArithType ref_next = Pixel::load(dstp[stride*(y + 1 + interlaced) + x-2]);
	    
	    ArithType fill_prev = Pixel::avg(5*prev + 3*cur + 1*next, 4, 9);
	    ArithType fill_cur = Pixel::avg(1*prev + 3*cur + 1*next, 2, 5);
	    ArithType fill_next = Pixel::avg(1*prev + 3*cur + 5*next, 4, 9);

	    ArithType blur_prev = Pixel::avg(2 * ref_prev + ref_cur + Pixel::load(dstp[stride*(y - 2 - interlaced) + x-2]), 0, 4);
	    ArithType blur_next = Pixel::avg(2 * ref_next + ref_cur + Pixel::load(dstp[stride*(y + 2 + interlaced) + x-2]), 0, 4);

	    ArithType diff_next = absDiff(ref_next, fill_cur);
	    ArithType diff_prev = absDiff(ref_prev, fill_cur);
	    ArithType thr_next = absDiff(ref_next, blur_next);
	    ArithType thr_prev = absDiff(ref_prev, blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = Pixel::store(fill_prev);
		}
		else {
		    dstp[stride*y + x] = Pixel::store(fill_next);
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = Pixel::store(fill_next);
	    } else {
		dstp[stride*y + x] = Pixel::store(fill_cur);
	    }
         }
      }

      for (y = top - 1; y >= 0; y--) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y+1 + interlaced)];
         memcpy(dstp + stride*y + width - copyLen, dstp + stride*(y+1 + interlaced) + width - copyLen, copyLen * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - copyLen; x++) {
            ArithType prev = Pixel::load(dstp[stride*(y+1 + interlaced) + x - 1]);
            ArithType cur  = Pixel::load(dstp[stride*(y+1 + interlaced) + x]);
            ArithType next = Pixel::load(dstp[stride*(y+1 + interlaced) + x + 1]);

            ArithType ref_prev = Pixel::load(dstp[stride*(y+2 + interlaced) + x - 1]);
            ArithType ref_cur  = Pixel::load(dstp[stride*(y+2 + interlaced) + x]);
            ArithType ref_next = Pixel::load(dstp[stride*(y+2 + interlaced) + x + 1]);
            ArithType ref_prev2 = Pixel::load(dstp[stride*(y+2 + interlaced) + (x < 2 ? 0 : x - 2)]);

	    ArithType fill_prev = Pixel::avg(5*prev + 3*cur + 1*next, 4, 9);
	    ArithType fill_cur = Pixel::avg(1*prev + 3*cur + 1*next, 2, 5);
	    ArithType fill_next = Pixel::avg(1*prev + 3*cur + 5*next, 4, 9);

	    ArithType blur_prev = Pixel::avg(2 * ref_prev + ref_cur + ref_prev2, 0, 4);
	    ArithType blur_next = Pixel::avg(2 * ref_next + ref_cur + Pixel::load(dstp[stride*(y+2 + interlaced) + x + 2]), 0, 4);

	    ArithType diff_next = absDiff(ref_next, fill_cur);
	    ArithType diff_prev = absDiff(ref_prev, fill_cur);
	    ArithType thr_next = absDiff(ref_next, blur_next);
	    ArithType thr_prev = absDiff(ref_prev, blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = Pixel::store(fill_prev);
		}
		else {
		    dstp[stride*y + x] = Pixel::store(fill_next);
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = Pixel::store(fill_next);
	    } else {
		dstp[stride*y + x] = Pixel::store(fill_cur);
	    }
         }
      }

      for (y = height - bottom; y < height; y++) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y-1 - interlaced)];
         memcpy(dstp + stride*y + width - copyLen, dstp + stride*(y-1 - interlaced) + width - copyLen, copyLen * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - copyLen; x++) {
            ArithType prev = Pixel::load(dstp[stride*(y-1 - interlaced) + x - 1]);
            ArithType cur  = Pixel::load(dstp[stride*(y-1 - interlaced) + x]);
            ArithType next = Pixel::load(dstp[stride*(y-1 - interlaced) + x + 1]);

            ArithType ref_prev = Pixel::load(dstp[stride*(y-2 - interlaced) + x - 1]);
            ArithType ref_cur  = Pixel::load(dstp[stride*(y-2 - interlaced) + x]);
            ArithType ref_next = Pixel::load(dstp[stride*(y-2 - interlaced) + x + 1]);
            ArithType ref_prev2 = Pixel::load(dstp[stride*(y-2 - interlaced) + (x < 2 ? 0 : x - 2)]);
	    
	    ArithType fill_prev = Pixel::avg(5*prev + 3*cur + 1*next, 4, 9);
	    ArithType fill_cur = Pixel::avg(1*prev + 3*cur + 1*next, 2, 5);
	    ArithType fill_next = Pixel::avg(1*prev + 3*cur + 5*next, 4, 9);

	    ArithType blur_prev = Pixel::avg(2 * ref_prev + ref_cur + ref_prev2, 0, 4);
	    ArithType blur_next = Pixel::avg(2 * ref_next + ref_cur + Pixel::load(dstp[stride*(y-2 - interlaced) + x + 2]), 0, 4);

	    ArithType diff_next = absDiff(ref_next, fill_cur);
	    ArithType diff_prev = absDiff(ref_prev, fill_cur);
	    ArithType thr_next = absDiff(ref_next, blur_next);
	    ArithType thr_prev = absDiff(ref_prev, blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = Pixel::store(fill_prev);
		}
		else {
		    dstp[stride*y + x] = Pixel::store(fill_next);
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = Pixel::store(fill_next);
	    } else {
		dstp[stride*y + x] = Pixel::store(fill_cur);
	    }
         }
      }
   } 
}


static const VSFrame *VS_CC fillBordersGetFrame(int n, int activationReason, void *instanceData, [[maybe_unused]] void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
   FillBordersData *d = (FillBordersData *) instanceData;

   if (activationReason == arInitial) {
      vsapi->requestFrameFilter(n, d->node, frameCtx);
   } else if (activationReason == arAllFramesReady) {
      const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
      VSFrame *dst = vsapi->copyFrame(src, core);
      int plane;
      vsapi->freeFrame(src);

      int interlaced_processing = 0;

      if (d->interlaced == Interlaced)
          interlaced_processing = 1;
      else if (d->interlaced == NotInterlaced)
          interlaced_processing = 0;
      else if (d->interlaced == InterlacedAuto) {
          enum FieldBased {
              Progressive = 0,
              BottomFieldFirst = 1,
              TopFieldFirst = 2
          };

          const VSMap *props = vsapi->getFramePropertiesRO(dst);

          int err;
          int64_t field_based = vsapi->mapGetInt(props, "_FieldBased", 0, &err);
          if (err || field_based == Progressive)
              interlaced_processing = 0;
          else
              interlaced_processing = 1;
      }

      int left[2] = { d->left, (int)subsampledBorder(d->left, d->vi->format.subSamplingW) };
      int top[2] = { d->top, (int)subsampledBorder(d->top, d->vi->format.subSamplingH) };
      int right[2] = { d->right, (int)subsampledBorder(d->right, d->vi->format.subSamplingW) };
      int bottom[2] = { d->bottom, (int)subsampledBorder(d->bottom, d->vi->format.subSamplingH) };

      for (plane = 0; plane < d->vi->format.numPlanes; plane++) {
         uint8_t *dstp = vsapi->getWritePtr(dst, plane);
         int width = vsapi->getFrameWidth(dst, plane);
         int height = vsapi->getFrameHeight(dst, plane);
         intptr_t stride = vsapi->getStride(dst, plane);

         d->fill(dstp, width, height, stride, left[!!plane], right[!!plane], top[!!plane], bottom[!!plane], d->mode, interlaced_processing);
      }

      return dst;
   }

   return nullptr;
}


static void VS_CC fillBordersFree(void *instanceData, [[maybe_unused]] VSCore *core, const VSAPI *vsapi) {
   FillBordersData *d = (FillBordersData *)instanceData;

   vsapi->freeNode(d->node);
   delete d;
}


static bool planeTooSmall(int64_t width, int64_t height, int64_t left, int64_t right, int64_t top, int64_t bottom, int mode, int il) {
   if (mode == ModeFillMargins || mode == ModeRepeat) {
      // horizontal: fill reads the first/last kept column; vertical: reads one row
      // (top + il / bottom + il) into the interior.
      return width  < left + right || (left > 0 && width  <= left)     || (right  > 0 && width  <= right) ||
             height < top + bottom || (top  > 0 && height <= top + il) || (bottom > 0 && height <= bottom + il);
   } else if (mode == ModeFixBorders) {
      // side columns read 2 pixels inward and, when present, copy 3 rows at top and
      // bottom; the top/bottom fill reads 2 (+il) rows into the interior.
      bool cols = left > 0 || right > 0;
      return width  < left + right ||
             (left  > 0 && width  < left  + 2) ||
             (right > 0 && width  < right + 2) ||
             height < top + bottom ||
             (cols && (height < top + 3 || height < bottom + 3)) ||
             (top    > 0 && height < top    + 2 + il) ||
             (bottom > 0 && height < bottom + 2 + il);
   } else if (mode == ModeMirror) {
      // reflects up to 2*border-1 pixels; interlaced reflection of an odd border
      // reaches one row further (row 2*border), needing height >= 2*border + 1.
      return width  < 2*left || width  < 2*right ||
             height < 2*top  || height < 2*bottom ||
             (il && (top    % 2) && height < 2*top    + 1) ||
             (il && (bottom % 2) && height < 2*bottom + 1);
   }

   return false;
}


static void VS_CC fillBordersCreate(const VSMap *in, VSMap *out, [[maybe_unused]] void *userData, VSCore *core, const VSAPI *vsapi) {
   // Nothrow, because an exception must not escape into the C API. Ownership is
   // handed to the core by release() once the filter has been created; until then
   // every early return frees it.
   std::unique_ptr<FillBordersData> d(new (std::nothrow) FillBordersData());
   if (!d) {
      vsapi->mapSetError(out, "FillBorders: Out of memory.");
      return;
   }

   int err;

   d->left = vsapi->mapGetIntSaturated(in, "left", 0, &err);
   d->right = vsapi->mapGetIntSaturated(in, "right", 0, &err);
   d->top = vsapi->mapGetIntSaturated(in, "top", 0, &err);
   d->bottom = vsapi->mapGetIntSaturated(in, "bottom", 0, &err);

   const char *mode = vsapi->mapGetData(in, "mode", 0, &err);
   if (err) {
      d->mode = ModeRepeat;
   } else {
      if (strcmp(mode, "fillmargins") == 0) {
         d->mode = ModeFillMargins;
      } else if (strcmp(mode, "repeat") == 0) {
         d->mode = ModeRepeat;
      } else if (strcmp(mode, "mirror") == 0) {
         d->mode = ModeMirror;
      } else if (strcmp(mode, "fixborders") == 0) {
         d->mode = ModeFixBorders;
      } else {
         vsapi->mapSetError(out, "FillBorders: Invalid mode. Valid values are 'fillmargins', 'mirror', 'repeat', and 'fixborders'.");
         return;
      }
   }

   d->interlaced = vsapi->mapGetIntSaturated(in, "interlaced", 0, &err);
   if (err)
       d->interlaced = NotInterlaced;

   if (d->interlaced < InterlacedAuto || d->interlaced > Interlaced) {
      vsapi->mapSetError(out, "FillBorders: interlaced must be -1 (auto), 0 (off), or 1 (on).");
      return;
   }


   if (d->left < 0 || d->right < 0 || d->top < 0 || d->bottom < 0) {
      vsapi->mapSetError(out, "FillBorders: Can't fill a negative number of pixels.");
      return;
   }

   d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
   d->vi = vsapi->getVideoInfo(d->node);

   if (!vsh::isConstantVideoFormat(d->vi)) {
      vsapi->mapSetError(out, "FillBorders: Only constant format input supported.");
      vsapi->freeNode(d->node);
      return;
   }

   // Picking the kernel here doubles as the format check. The only sample format
   // the core knows that is left out is 32 bit integer.
   const VSVideoFormat &format = d->vi->format;

   if (format.sampleType == stInteger && format.bytesPerSample == 1)
      d->fill = fillBorders<uint8_t>;
   else if (format.sampleType == stInteger && format.bytesPerSample == 2)
      d->fill = fillBorders<uint16_t>;
   else if (format.sampleType == stFloat && format.bytesPerSample == 2)
      d->fill = fillBorders<Half>;
   else if (format.sampleType == stFloat && format.bytesPerSample == 4)
      d->fill = fillBorders<float>;
   else {
      vsapi->mapSetError(out, "FillBorders: Only 8..16 bit integer, half precision float and single precision float input supported.");
      vsapi->freeNode(d->node);
      return;
   }

   if (!d->left && !d->right && !d->top && !d->bottom) {
      // Just pass the input node through.
      vsapi->mapConsumeNode(out, "clip", d->node, maReplace);
      return;
   }

   // Interlaced processing reads one row further than progressive. For interlaced=-1
   // (auto) the decision is per-frame, so validate for the worst case (il = 1).
   const int il = (d->interlaced != NotInterlaced) ? 1 : 0;

   // Every plane has to satisfy the requirements on its own. A subsampled chroma
   // plane is smaller, but the margins fillBorders() reads past the border are
   // absolute row and column counts that do not scale down with it.
   bool too_small = planeTooSmall(d->vi->width, d->vi->height,
                                  d->left, d->right, d->top, d->bottom, d->mode, il);

   if (!too_small && d->vi->format.numPlanes > 1) {
      const int ssW = d->vi->format.subSamplingW;
      const int ssH = d->vi->format.subSamplingH;

      too_small = planeTooSmall(d->vi->width >> ssW, d->vi->height >> ssH,
                                subsampledBorder(d->left, ssW), subsampledBorder(d->right, ssW),
                                subsampledBorder(d->top, ssH), subsampledBorder(d->bottom, ssH),
                                d->mode, il);
   }

   if (too_small) {
      vsapi->mapSetError(out, "FillBorders: The input clip is too small or the borders are too big.");
      vsapi->freeNode(d->node);
      return;
   }

   VSFilterDependency deps[] = { {d->node, rpStrictSpatial} };
   vsapi->createVideoFilter(out, "FillBorders", d->vi, fillBordersGetFrame, fillBordersFree, fmParallel, deps, 1, d.get(), core);
   d.release();
}


VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
   vspapi->configPlugin("com.nodame.fillborders", "fb", "FillBorders plugin for VapourSynth", VS_MAKE_VERSION(5, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
   vspapi->registerFunction("FillBorders",
                "clip:vnode;"
                "left:int:opt;"
                "right:int:opt;"
                "top:int:opt;"
                "bottom:int:opt;"
                "mode:data:opt;"
                "interlaced:int:opt;",
                "clip:vnode;",
                fillBordersCreate, nullptr, plugin);
}
