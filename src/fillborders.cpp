#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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


typedef struct {
   VSNode *node;
   const VSVideoInfo *vi;

   int left;
   int right;
   int top;
   int bottom;
   int mode;
   int interlaced;
} FillBordersData;

template <typename PixelType>
static inline void vs_memset16(void *ptr, int value, size_t num) {
    if (sizeof(PixelType) == 1)
        memset(ptr, value, num);
    else {
        PixelType *tptr = (PixelType *)ptr;
        while (num-- > 0)
            *tptr++ = (PixelType)value;
    }
}

template <typename PixelType>
static void fillBorders(uint8_t *dstp8, int width, int height, intptr_t stride, int left, int right, int top, int bottom, int mode, int interlaced) {
   int x, y;
   PixelType *dstp = (PixelType *)dstp8;
   stride /= sizeof(PixelType);

   if (mode == ModeFillMargins) {
      for (y = top; y < height - bottom; y++) {
         vs_memset16<PixelType>(dstp + stride*y, (dstp + stride*y)[left], left);
         vs_memset16<PixelType>(dstp + stride*y + width - right, (dstp + stride*y + width - right)[-1], right);
      }

      for (y = top - 1; y >= 0; y--) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y+1 + interlaced)];
         memcpy(dstp + stride*y + width - 8, dstp + stride*(y+1 + interlaced) + width - 8, 8 * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - 8; x++) {
            PixelType prev = dstp[stride*(y+1 + interlaced) + x - 1];
            PixelType cur  = dstp[stride*(y+1 + interlaced) + x];
            PixelType next = dstp[stride*(y+1 + interlaced) + x + 1];
            dstp[stride*y + x] = (3*prev + 2*cur + 3*next + 4) / 8;
         }
      }

      for (y = height - bottom; y < height; y++) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y-1 - interlaced)];
         memcpy(dstp + stride*y + width - 8, dstp + stride*(y-1 - interlaced) + width - 8, 8 * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - 8; x++) {
            PixelType prev = dstp[stride*(y-1 - interlaced) + x - 1];
            PixelType cur  = dstp[stride*(y-1 - interlaced) + x];
            PixelType next = dstp[stride*(y-1 - interlaced) + x + 1];
            dstp[stride*y + x] = (3*prev + 2*cur + 3*next + 4) / 8;
         }
      }
   } else if (mode == ModeRepeat) {
      for (y = top; y < height - bottom; y++) {
         vs_memset16<PixelType>(dstp + stride*y, (dstp + stride*y)[left], left);
         vs_memset16<PixelType>(dstp + stride*y + width - right, (dstp + stride*y + width - right)[-1], right);
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
            PixelType prev = dstp[stride*(y - 1 - interlaced) +x+1];
            PixelType cur  = dstp[stride*(y) +x+1];
            PixelType next = dstp[stride*(y + 1 + interlaced) +x+1];

            PixelType ref_prev = dstp[stride*(y - 1 - interlaced) +x+2];
            PixelType ref_cur  = dstp[stride*(y) +x+2];
            PixelType ref_next = dstp[stride*(y + 1 + interlaced) +x+2];

	    PixelType fill_prev = (5*prev + 3*cur + 1*next) / 9 + 0.5;
	    PixelType fill_cur = (1*prev + 3*cur + 1*next) / 5 + 0.5;
	    PixelType fill_next = (1*prev + 3*cur + 5*next) / 9 + 0.5;

	    PixelType blur_prev = (2 * ref_prev + ref_cur + dstp[stride*(y - 2 - interlaced) + x+2]) / 4;
	    PixelType blur_next = (2 * ref_next + ref_cur + dstp[stride*(y + 2 + interlaced) + x+2]) / 4;

	    PixelType diff_next = abs(ref_next - fill_cur);
	    PixelType diff_prev = abs(ref_prev - fill_cur);
	    PixelType thr_next = abs(ref_next - blur_next);
	    PixelType thr_prev = abs(ref_prev - blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = fill_prev;
		}
		else {
		    dstp[stride*y + x] = fill_next;
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = fill_next;
	    } else {
		dstp[stride*y + x] = fill_cur;
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
            PixelType prev = dstp[stride*(y - 1 - interlaced) + x-1];
            PixelType cur  = dstp[stride*(y) + x-1];
            PixelType next = dstp[stride*(y + 1 + interlaced) + x-1];

            PixelType ref_prev = dstp[stride*(y - 1 - interlaced) + x-2];
            PixelType ref_cur  = dstp[stride*(y) + x-2];
            PixelType ref_next = dstp[stride*(y + 1 + interlaced) + x-2];
	    
	    PixelType fill_prev = (5*prev + 3*cur + 1*next) / 9 + 0.5;
	    PixelType fill_cur = (1*prev + 3*cur + 1*next) / 5 + 0.5;
	    PixelType fill_next = (1*prev + 3*cur + 5*next) / 9 + 0.5;

	    PixelType blur_prev = (2 * ref_prev + ref_cur + dstp[stride*(y - 2 - interlaced) + x-2]) / 4;
	    PixelType blur_next = (2 * ref_next + ref_cur + dstp[stride*(y + 2 + interlaced) + x-2]) / 4;

	    PixelType diff_next = abs(ref_next - fill_cur);
	    PixelType diff_prev = abs(ref_prev - fill_cur);
	    PixelType thr_next = abs(ref_next - blur_next);
	    PixelType thr_prev = abs(ref_prev - blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = fill_prev;
		}
		else {
		    dstp[stride*y + x] = fill_next;
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = fill_next;
	    } else {
		dstp[stride*y + x] = fill_cur;
	    }
         }
      }

      for (y = top - 1; y >= 0; y--) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y+1 + interlaced)];
         memcpy(dstp + stride*y + width - 8, dstp + stride*(y+1 + interlaced) + width - 8, 8 * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - 8; x++) {
            PixelType prev = dstp[stride*(y+1 + interlaced) + x - 1];
            PixelType cur  = dstp[stride*(y+1 + interlaced) + x];
            PixelType next = dstp[stride*(y+1 + interlaced) + x + 1];

            PixelType ref_prev = dstp[stride*(y+2 + interlaced) + x - 1];
            PixelType ref_cur  = dstp[stride*(y+2 + interlaced) + x];
            PixelType ref_next = dstp[stride*(y+2 + interlaced) + x + 1];

	    PixelType fill_prev = (5*prev + 3*cur + 1*next) / 9 + 0.5;
	    PixelType fill_cur = (1*prev + 3*cur + 1*next) / 5 + 0.5;
	    PixelType fill_next = (1*prev + 3*cur + 5*next) / 9 + 0.5;

	    PixelType blur_prev = (2 * ref_prev + ref_cur + dstp[stride*(y+2 + interlaced) + x - 2]) / 4;
	    PixelType blur_next = (2 * ref_next + ref_cur + dstp[stride*(y+2 + interlaced) + x + 2]) / 4;

	    PixelType diff_next = abs(ref_next - fill_cur);
	    PixelType diff_prev = abs(ref_prev - fill_cur);
	    PixelType thr_next = abs(ref_next - blur_next);
	    PixelType thr_prev = abs(ref_prev - blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = fill_prev;
		}
		else {
		    dstp[stride*y + x] = fill_next;
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = fill_next;
	    } else {
		dstp[stride*y + x] = fill_cur;
	    }
         }
      }

      for (y = height - bottom; y < height; y++) {
         // copy first pixel
         // copy last eight pixels
         dstp[stride*y] = dstp[stride*(y-1 - interlaced)];
         memcpy(dstp + stride*y + width - 8, dstp + stride*(y-1 - interlaced) + width - 8, 8 * sizeof(PixelType));

         // weighted average for the rest
         for (x = 1; x < width - 8; x++) {
            PixelType prev = dstp[stride*(y-1 - interlaced) + x - 1];
            PixelType cur  = dstp[stride*(y-1 - interlaced) + x];
            PixelType next = dstp[stride*(y-1 - interlaced) + x + 1];

            PixelType ref_prev = dstp[stride*(y-2 + interlaced) + x - 1];
            PixelType ref_cur  = dstp[stride*(y-2 + interlaced) + x];
            PixelType ref_next = dstp[stride*(y-2 + interlaced) + x + 1];
	    
	    PixelType fill_prev = (5*prev + 3*cur + 1*next) / 9 + 0.5;
	    PixelType fill_cur = (1*prev + 3*cur + 1*next) / 5 + 0.5;
	    PixelType fill_next = (1*prev + 3*cur + 5*next) / 9 + 0.5;

	    PixelType blur_prev = (2 * ref_prev + ref_cur + dstp[stride*(y-2 + interlaced) + x - 2]) / 4;
	    PixelType blur_next = (2 * ref_next + ref_cur + dstp[stride*(y-2 + interlaced) + x + 2]) / 4;

	    PixelType diff_next = abs(ref_next - fill_cur);
	    PixelType diff_prev = abs(ref_prev - fill_cur);
	    PixelType thr_next = abs(ref_next - blur_next);
	    PixelType thr_prev = abs(ref_prev - blur_prev);

	    if (diff_next > thr_next) {
		if (diff_prev < diff_next) {
                    dstp[stride*y + x] = fill_prev;
		}
		else {
		    dstp[stride*y + x] = fill_next;
		}
	    } else if (diff_prev > thr_prev) {
                dstp[stride*y + x] = fill_next;
	    } else {
		dstp[stride*y + x] = fill_cur;
	    }
         }
      }
   } 
}


static const VSFrame *VS_CC fillBordersGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;

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

      int left[2] = { d->left, d->left >> d->vi->format.subSamplingW };
      int top[2] = { d->top, d->top >> d->vi->format.subSamplingH };
      int right[2] = { d->right, d->right >> d->vi->format.subSamplingW };
      int bottom[2] = { d->bottom, d->bottom >> d->vi->format.subSamplingH };

      for (plane = 0; plane < d->vi->format.numPlanes; plane++) {
         uint8_t *dstp = vsapi->getWritePtr(dst, plane);
         int width = vsapi->getFrameWidth(dst, plane);
         int height = vsapi->getFrameHeight(dst, plane);
         intptr_t stride = vsapi->getStride(dst, plane);

         (d->vi->format.bytesPerSample == 1 ? fillBorders<uint8_t>
                                             : fillBorders<uint16_t>)(dstp, width, height, stride, left[!!plane], right[!!plane], top[!!plane], bottom[!!plane], d->mode, interlaced_processing);
      }

      return dst;
   }

   return 0;
}


static void VS_CC fillBordersFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;

   FillBordersData *d = (FillBordersData *)instanceData;

   vsapi->freeNode(d->node);
   free(d);
}


static void VS_CC fillBordersCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

   FillBordersData d;
   FillBordersData *data;
   int err;

   d.left = vsapi->mapGetIntSaturated(in, "left", 0, &err);
   d.right = vsapi->mapGetIntSaturated(in, "right", 0, &err);
   d.top = vsapi->mapGetIntSaturated(in, "top", 0, &err);
   d.bottom = vsapi->mapGetIntSaturated(in, "bottom", 0, &err);

   const char *mode = vsapi->mapGetData(in, "mode", 0, &err);
   if (err) {
      d.mode = ModeRepeat;
   } else {
      if (strcmp(mode, "fillmargins") == 0) {
         d.mode = ModeFillMargins;
      } else if (strcmp(mode, "repeat") == 0) {
         d.mode = ModeRepeat;
      } else if (strcmp(mode, "mirror") == 0) {
         d.mode = ModeMirror;
      } else if (strcmp(mode, "fixborders") == 0) {
         d.mode = ModeFixBorders;
      } else {
         vsapi->mapSetError(out, "FillBorders: Invalid mode. Valid values are 'fillmargins', 'mirror', 'repeat', and 'fixborders'.");
         return;
      }
   }

   d.interlaced = !!vsapi->mapGetInt(in, "interlaced", 0, &err);
   if (err)
       d.interlaced = NotInterlaced;


   if (d.left < 0 || d.right < 0 || d.top < 0 || d.bottom < 0) {
      vsapi->mapSetError(out, "FillBorders: Can't fill a negative number of pixels.");
      return;
   }

   d.node = vsapi->mapGetNode(in, "clip", 0, nullptr);
   d.vi = vsapi->getVideoInfo(d.node);

   if (!vsh::isConstantVideoFormat(d.vi) || d.vi->format.sampleType != stInteger || d.vi->format.bytesPerSample > 2) {
      vsapi->mapSetError(out, "FillBorders: Only constant format 8..16 bit integer input supported.");
      vsapi->freeNode(d.node);
      return;
   }

   if (!d.left && !d.right && !d.top && !d.bottom) {
      // Just pass the input node through.
      vsapi->mapConsumeNode(out, "clip", d.node, maReplace);
      return;
   }

   if (d.mode == ModeFillMargins || d.mode == ModeRepeat || d.mode == ModeFixBorders) {
      if (d.vi->width < d.left + d.right || d.vi->width <= d.left || d.vi->width <= d.right ||
          d.vi->height < d.top + d.bottom || d.vi->height <= d.top || d.vi->height <= d.bottom) {
         vsapi->mapSetError(out, "FillBorders: The input clip is too small or the borders are too big.");
         vsapi->freeNode(d.node);
         return;
      }
   } else if (d.mode == ModeMirror) {
      if (d.vi->width < 2*d.left || d.vi->width < 2*d.right ||
          d.vi->height < 2*d.top || d.vi->height < 2*d.bottom) {
         vsapi->mapSetError(out, "FillBorders: The input clip is too small or the borders are too big.");
         vsapi->freeNode(d.node);
         return;
      }
   }

   data = (FillBordersData *)malloc(sizeof(d));
   *data = d;

   VSFilterDependency deps[] = { {data->node, rpStrictSpatial} };
   vsapi->createVideoFilter(out, "FillBorders", data->vi, fillBordersGetFrame, fillBordersFree, fmParallel, deps, 1, data, core);
}


VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
   vspapi->configPlugin("com.nodame.fillborders", "fb", "FillBorders plugin for VapourSynth", VS_MAKE_VERSION(3, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
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
