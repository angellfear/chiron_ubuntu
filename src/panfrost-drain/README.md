# panfrost-drain

A preloaded library that makes a Mesa panfrost context wait for its last GPU
job before it is destroyed. It stands in for a one-line Mesa fix and should be
removed once the archive Mesa carries that fix.

## The defect

Mesa's `panfrost_destroy()` releases a context without waiting for the jobs it
has already submitted. Those jobs point into Mesa's screen-wide shader and
descriptor pools, which are in no job's BO list, so the kernel does not keep
that memory alive. A program that exits without `glFinish()` has the pools
freed under the GPU: the running job takes an MMU translation fault, and each
job still queued takes a fault and a 500 ms scheduler timeout of its own before
the file descriptor finishes closing.

    GST_GL_PLATFORM=egl GST_GL_WINDOW=surfaceless gst-launch-1.0 \
        gltestsrc pattern=mandelbrot num-buffers=300 ! \
        'video/x-raw(memory:GLMemory),width=1920,height=1080' ! fakesink

On this board that pipeline leaves 40 to 60 `Unhandled Page fault` /
`gpu sched timeout` pairs in the kernel log and takes about 24 seconds to exit.
With this library it takes about 1.3 seconds and leaves none. The same happens
with Mesa 25.2, 26.0 and 26.2.

It was pinned down by logging the DRM ioctls of the pipeline: the faulting
buffers were never part of any submission, and were closed in the process
teardown while jobs were still queued. `PAN_MESA_DEBUG=sync`, which waits for
every job, and a `glFinish()` before exit both make it disappear.

## What the library does

The context's syncobj is the out-fence of its last job, and
`panfrost_destroy()` destroys it on the way out. The library remembers which
syncobjs a process handed to `DRM_IOCTL_PANFROST_SUBMIT` as an out-fence -- on
a file descriptor that `DRM_IOCTL_VERSION` confirms is panfrost -- and waits on
such a syncobj, for at most five seconds, before letting
`DRM_IOCTL_SYNCOBJ_DESTROY` through. Every other ioctl passes straight through.

It is listed in `/etc/ld.so.preload` of the graphical image and installed as
`/usr/local/lib/chiron/libpanfrost-drain.so`.
