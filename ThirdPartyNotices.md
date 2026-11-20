# Third-party notices

## sfxr / jsfxr reference and derived Retro algorithms

DreamSFX's Retro/Noise note adapters reuse the project's EightBitMetaSound C++ DSP. That DSP was implemented with reference to sfxr/jsfxr behavior (including oscillator, oversampling, envelope/filter/flanger and quantization conventions). The development oracle is `Tools/Verify/reference_sfxr.js`; its canonical UTF-8/LF bytes exactly match the pinned upstream `sfxr.js` (including upstream ESM compatibility). The separate RIFFWAVE stub is local. It is not a DreamSFX product name or an endorsement by the upstream authors.

Upstream project: https://github.com/chr15m/jsfxr/
Application: https://sfxr.me/
Upstream README credits sfxr to Dr. Petter, the HTML5 port to Eric Fredricksen, and maintenance to Chris McCormick.

The upstream repository's UNLICENSE was retrieved on 2026-11-16 from:
https://raw.githubusercontent.com/chr15m/jsfxr/master/UNLICENSE

Its text is reproduced below. On 2026-11-17 the local oracle was matched exactly to upstream commit `8080b1aa56f1dbb4cecbad9b163f3ef62c530ef8`, file `sfxr.js`, Git blob `9641b790c2195b1da8fd61834d05034206f73a5c`, SHA256 `6095a6358e654bc3ffd55d9ccecf694f187953d78b1391ee2823a6fcb92551c5`. This pins reproducible source content; the original download event was not recorded. Source and license hashes are in `Tools/Verify/reference_sfxr.provenance.json`. Pinned source: https://github.com/chr15m/jsfxr/blob/8080b1aa56f1dbb4cecbad9b163f3ef62c530ef8/sfxr.js ; pinned license: https://github.com/chr15m/jsfxr/blob/8080b1aa56f1dbb4cecbad9b163f3ef62c530ef8/UNLICENSE . No commercial Pro application source, account service or paid assets were imported.

### Unlicense text

This is free and unencumbered software released into the public
domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>

## Development-only helper

`Tools/Verify/riffwave.js` is a minimal local stub for headless tests, not the upstream browser WAV encoder. Browser UI libraries and web assets are not runtime dependencies.

## Unreal Engine

Unreal Engine, MetaSound and bundled engine dependencies remain governed by Epic's applicable Unreal Engine license and their respective notices. Nothing here relicenses Unreal Engine source or grants distribution rights to engine components. The engine checkout is not included in this repository.

## Samples and musical examples

Piano/MusicBox are procedural synthesis prototypes, not sampled instruments. Checked-in DreamSFX example phrases were authored for this project; no external audio sample pack or protected-game melody is included. This statement is not a subjective timbre or music-rights guarantee.

## Project license

No new general license for the project's original code is granted by this notice. The project owner must choose any public release license separately.

## Small Magic procedural graph

The opening of the Small Magic blast uses the generated node connections, scheduling and seeded buffer recipes from the MIT-licensed synthesis code by Patch. The reference checkout is pinned to `8a096a0`. The source-controlled preset contains its numeric synthesis instructions.

MIT License

Copyright (c) 2026 Patch

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Graph dynamics and resampling

The graph compressor uses Sean Connelly’s sndfilter at commit `1e45029cc5eae2ad12dc7ca1e06c59693482ff90`, including its Web Audio compressor ancestry. The local adaptation supports MSVC C++ and keeps the requested lookahead length at supported sample rates. [sndfilter](https://github.com/velipso/sndfilter/tree/1e45029cc5eae2ad12dc7ca1e06c59693482ff90)

BSD Zero Clause License

Copyright (c) 2024 by Sean Connelly (https://sean.fun)

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.


### Blink resampling

[Upstream source](https://raw.githubusercontent.com/chromium/chromium/main/third_party/blink/renderer/platform/audio/up_sampler.cc)

```text
Copyright (C) 2013 Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
    * Neither the name of Google Inc. nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Web Audio dynamics compression

[Upstream source](https://raw.githubusercontent.com/WebKit/WebKit/main/Source/WebCore/platform/audio/DynamicsCompressorKernel.cpp)

```text
Copyright (C) 2011 Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1.  Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
2.  Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
3.  Neither the name of Apple Inc. ("Apple") nor the names of
    its contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
