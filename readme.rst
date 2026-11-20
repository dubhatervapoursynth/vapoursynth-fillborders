Description
===========

This is a simple filter that fills the borders of a clip, without changing the clip's dimensions.


Installation
============

The plugin is published on PyPI as `vapoursynth-fillborders <https://pypi.org/project/vapoursynth-fillborders/>`_::

    python -m pip install vapoursynth-fillborders

Wheels are provided for Windows on x86-64, and for macOS and Linux on both x86-64 and arm64.

The wheel places the plugin in the ``vapoursynth/plugins`` directory of the target Python environment, which VapourSynth autoloads, so no further setup is needed.


Usage
=====
::

   fb.FillBorders(clip clip[, int left=0, int right=0, int top=0, int bottom=0, string mode="repeat", int interlaced=0])

left, right, top, bottom
   Number of pixels to fill on each side. These can be any non-negative numbers, within reason. If they are all 0, the input clip is simply passed through.

mode
   "repeat"
      Fills the borders using the outermost line or column.

   "mirror"
      Fills the borders by mirroring.

   "fillmargins"
      Fills the borders exactly like the Avisynth filter `FillMargins <http://forum.doom9.org/showthread.php?t=50132>`_, version 1.0.2.0. This mode is similar to "repeat", except that each pixel at the top and bottom borders is filled with a weighted average of its three neighbours from the previous line.

   "fixborders"
      A direction "aware" modification of FillMargins. It also works on all four sides.

interlaced
   Fills the top and bottom borders only with pixels taken from the same field. Possible values are 1 (always on), 0 (always off), and -1 (uses the _FieldBased frame property to decide if interlaced processing should be used or not).

Supported formats are 8 to 16 bit integer, half precision float and single precision float.

"repeat" and "mirror" only copy samples, so their output is always bit exact. "fillmargins" and "fixborders" average neighbouring samples; for both float formats that averaging is done in single precision.

On subsampled formats the borders are converted to chroma resolution by rounding up, so that a chroma sample overlapping any filled luma sample is filled too.


Compilation
===========

Requires VapourSynth to be installed and importable by the Python that Meson finds,
because the include path is queried with ``vapoursynth.get_include()``.

::

    meson setup build
    ninja -C build

To build a wheel instead::

    python -m build


License
=======

The license is WTFPL; see the LICENSE file.
