# Third-party notices

This repository ships source code only. Its own code is licensed under the GNU General
Public License, version 2 or (at your option) any later version; see [LICENSE](./LICENSE).

A `.3dsx` built from it links the libraries below. **Anyone who distributes a build must
include this file with it.** A build also contains the user's own copy of the game's code,
translated from `main.dol`; that code belongs to its owners, is not covered by this
project's license, and must not be redistributed.

## The game

*Custom Robo* (GameCube, 2004) was developed by Noise and published by Nintendo. No part of
the game (code, data, disc image or extracted files) is included in this repository. The
user supplies it from their own disc, and the files translated or extracted from it
(`gen/`, `data/`) stay local. This project is not affiliated with or endorsed by Nintendo,
Noise or HAL Laboratory; all trademarks belong to their respective owners.

## libjpeg-turbo

Linked into the build (devkitPro portlibs `libturbojpeg`); decodes the attract movie in
`runtime/thp.c`. Licensed under the IJG License and the Modified (3-clause) BSD License,
<https://libjpeg-turbo.org/>.

This software is based in part on the work of the Independent JPEG Group.

```
Copyright (C)2009-2025 D. R. Commander.  All Rights Reserved.
Copyright (C)2015 Viktor Szathmáry.  All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
- Neither the name of the libjpeg-turbo Project nor the names of its
  contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS",
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## libctru and citro3d

Linked into the build: the 3DS system library and the PICA200 graphics library from
[devkitPro](https://devkitpro.org/). Both are under the zlib License; their copyright
notices are in their repositories, <https://github.com/devkitPro/libctru> and
<https://github.com/devkitPro/citro3d>.

## devkitARM runtime

The build links devkitARM's newlib C library and GCC runtime (libgcc), which carry their
own permissive licenses and the GCC Runtime Library Exception.

## fdlibm

`runtime/libm.c` reproduces the game's copy of fdlibm's `sin`/`cos` (through Metrowerks
MSL), including its polynomial constants.

```
Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.

Developed at SunPro, a Sun Microsystems, Inc. business.
Permission to use, copy, modify, and distribute this
software is freely granted, provided that this notice
is preserved.
```

## Other references

Dolphin (GPL-2.0-or-later), YAGCD, 3dbrew, the doldecomp/melee decompilation, FFmpeg,
mw3ds and the other projects listed under Credits in
[README.detailed.md](./README.detailed.md#credits) were used as references for behaviour,
formats and techniques. Where the runtime follows Dolphin closely, this project's
GPL-2.0-or-later license keeps it compatible.
