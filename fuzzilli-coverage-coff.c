/*
 * QuickJS Javascript Engine
 *
 * Copyright (c) 2026 Fuzzilli contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Clang's COFF sanitizer coverage constructor addresses guard bounds through
 * these symbols. The first bound is referenced with an eight-byte offset, so
 * reserve two uint32_t slots ahead of the instrumented .SCOV$GM sections.
 * Compile this file without sanitizer coverage and link it into qjs.exe only.
 */
#include <stdint.h>

#ifdef _WIN32
uint32_t __start___sancov_guards[2] __attribute__((section(".SCOV$GA"), used));
uint32_t __stop___sancov_guards __attribute__((section(".SCOV$GZ"), used));
#endif
