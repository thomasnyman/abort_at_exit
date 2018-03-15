/*
 * Copyright (c) 2011, The Chromium OS Authors. All rights reserved.
 * Copyright (c) 2018, Thomas Nyman. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* LD_PRELOAD override that causes normal process termination to instead result
 * in abnormal process termination through a raised SIGABRT signal via abort(3)
 * (even if SIGABRT is ignored, or is caught by a handler that returns).
 * 
 * Loosely based on libminijailpreload.c by Chromium OS authors: 
 * https://android.googlesource.com/platform/external/minijail/+/master/libminijailpreload.c
 */

/* The address of the real main is stored here for fake_main to access */
static int (*real_main) (int, char **, char **);

/* Fake main(), spliced in before the real call to main() in __libc_start_main */
static int fake_main(int argc, char **argv, char **envp)
{	
	/* Register abort(3) as an atexit(3) handler to be called at normal
	 * process termination */
	atexit(abort);

	/* Finally call the real main function */
	return real_main(argc, argv, envp);
}

/* LD_PRELOAD override of __libc_start_main.
 *
 * The objective is to splice fake_main above to be executed instead of the
 * program main function. We cannot use LD_PRELOAD to override main directly as
 * LD_PRELOAD can only be used to override functions in dynamically linked
 * shared libraries whose addresses are determined via the Procedure
 * Linkage Table (PLT). However, main's location is not determined via the PLT,
 * but is statically linked to the executable entry routine at __start which
 * pushes main's address onto the stack, then invokes libc's startup routine,
 * which obtains main's address from the stack. 
 * 
 * Instead, we use LD_PRELOAD to override libc's startup routine,
 * __libc_start_main, which is normally responsible for calling main. We can't
 * just run our setup code *here* because the real __libc_start_main is
 * responsible for setting up the C runtime environment, so we can't rely on
 * standard library functions such as malloc(3) or atexit(3) being available
 * yet. 
 */
int __libc_start_main(int (*main) (int, char **, char **),
		      int argc, char **ubp_av, void (*init) (void),
		      void (*fini) (void), void (*rtld_fini) (void),
		      void (*stack_end))
{
	void *libc_handle, *sym;
	/* This type punning is unfortunately necessary in C99 as casting
	 * directly from void* to function pointers is left undefined in C99.
	 * Strictly speaking, the conversion via union is still undefined
	 * behaviour in C99 (C99 Section 6.2.6.1):
	 * 
	 *  "When a value is stored in a member of an object of union type, the
	 *  bytes of the object representation that do not correspond to that
	 *  member but do correspond to other members take unspecified values,
	 *  but the value of the union object shall not thereby become a trap
	 *  representation."
	 * 
	 * However, this conversion is valid in GCC, and dlsym() also in effect
	 * mandates these conversions to be valid in POSIX system C compilers.
	 * 
	 * C11 explicitly allows this conversion (C11 Section 6.5.2.3): 
	 *  
	 *  "If the member used to read the contents of a union object is not
	 *  the same as the member last used to store a value in the object, the
	 *  appropriate part of the object representation of the value is
	 *  reinterpreted as an object representation in the new type as
	 *  described in 6.2.6 (a process sometimes called ‘‘type punning’’).
	 *  This might be a trap representation.
	 * 
	 * Some compilers allow direct conversion between pointers to an object
	 * or void to a pointer to a function and vice versa. C11's annex “J.5.7
	 * Function pointer casts lists this as a common extension:
	 * 
	 *   "1 A pointer to an object or to void may be cast to a pointer to a
	 *   function, allowing data to be invoked as a function (6.5.4).
         * 
	 *   2 A pointer to a function may be cast to a pointer to an object or
	 *   to void, allowing a function to be inspected or modified (for
	 *   example, by a debugger) (6.5.4)."
	 */
	union {
		int (*fn) (int (*main) (int, char **, char **), int argc,
			   char **ubp_av, void (*init) (void),
			   void (*fini) (void), void (*rtld_fini) (void),
			   void (*stack_end));
		void *sym;
	} real_libc_start_main;


	/* Obtain handle to libc shared library. The object should already be
	 * resident in the programs memory space, hence we can attempt to open
	 * it without loading the shared object. If this fails, we are most
	 * likely dealing with another version of libc.so */
	libc_handle = dlopen("libc.so.6", RTLD_NOLOAD | RTLD_NOW);

	if (!libc_handle) {
		fprintf(stderr, "can't open handle to libc.so.6: %s\n",
			dlerror());
		/* We dare not use abort() here because it would run atexit(3)
		 * handlers and try to flush stdio. */
		_exit(EXIT_FAILURE);
	}
	
	/* Our LD_PRELOAD will overwrite the real __libc_start_main, so we have
	 * to look up the real one from libc and invoke it with a pointer to the
	 * fake main we'd like to run before the real main function. */
	sym = dlsym(libc_handle, "__libc_start_main");
	if (!sym) {
		fprintf(stderr, "can't find __libc_start_main():%s\n",
			dlerror());
		_exit(EXIT_FAILURE);
	}

	real_libc_start_main.sym = sym;
	real_main = main;
	
	/* Close our handle to dynamically loaded libc. Since the libc object
	 * was already loaded previously, this only decrements the reference
	 * count to the shared object. Hence, we can be confident that the
	 * symbol to the read __libc_start_main remains valid even after we
	 * close our handle. In order to strictly adhere to the API, we could
	 * defer closing the handle to our spliced-in fake main before it call
	 * the real main function. */
	if(dlclose(libc_handle)) {
		fprintf(stderr, "can't close handle to libc.so.6: %s\n",
			dlerror());
		_exit(EXIT_FAILURE);
	}

	/* Note that we swap fake_main in for main - fake_main should call
	 * real_main after its setup is done. */
	return real_libc_start_main.fn(fake_main, argc, ubp_av, init, fini,
				       rtld_fini, stack_end);
}

