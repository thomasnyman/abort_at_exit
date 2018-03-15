# LD_PRELOAD override to abort program at exit

A `LD_PRELOAD` override that causes normal process termination to instead
result in abnormal process termination through a raised `SIGABRT` signal via
`abort(3)` (even if `SIGABRT` is ignored, or is caught by a handler that returns).

 The override works by splicing in a _"fake main"_ function to be executed instead
 of the program main function. The `fake_main` function registers `abort(3)` as
 an `atexit(3)` handler to be called at normal process termination.

`LD_PRELOAD` cannot be used to override main directly as it can only override
functions in dynamically linked shared libraries whose addresses are determined
via the Procedure Linkage Table (PLT). However, main's location is not
determined via the PLT, but is statically linked to the executable entry routine
at `__start` which pushes main's address onto the stack, then invokes libc's
startup routine, which obtains main's address from the stack. 

Instead, the override uses `LD_PRELOAD` to override libc's startup routine,
`__libc_start_main`, which is normally responsible for calling main and splice
in `fake_main` there. We can't just run our setup code in the overriden
`__libc_start_main` directly because the real `__libc_start_main` is responsible
for setting up the C runtime environment, so we can't rely on standard library
functions such as `malloc(3)` or `atexit(3)` being available yet. 

Building:

    make

Usage:

    LD_PRELOAD=abort_at_exit.o program

## Acknowledgments

Loosely based on [`libminijailpreload.c`][1] by the Chromium OS Authors.

## License

Use of this source code is governed by a BSD-style license that can be found in
the LICENSE file.

[1]: https://android.googlesource.com/platform/external/minijail/+/master/libminijailpreload.c "libminijailpreload.c - preload hack library"

