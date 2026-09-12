# Third-party licences

Pastiche itself is MIT-licensed (see `LICENSE`). The release ZIP bundles or
links against the components below. Full licence texts are reproduced in the
sections that follow or shipped alongside the binaries as noted.

## Libraries linked into the executables

| Component        | Version  | Licence            | Source                                      |
|------------------|----------|--------------------|---------------------------------------------|
| libpng           | 1.6.x    | libpng / zlib-like | http://www.libpng.org/pub/png/libpng.html   |
| zlib             | 1.3.x    | zlib               | https://zlib.net                            |
| libjpeg-turbo    | 3.1.x    | IJG + BSD-3 + zlib | https://libjpeg-turbo.org                   |
| libwebp          | 1.6.x    | BSD-3              | https://chromium.googlesource.com/webm/libwebp |
| libjxl (optional)| 0.11.x   | BSD-3              | https://github.com/libjxl/libjxl            |
| highway, brotli (libjxl deps) | - | Apache-2.0 / MIT | via libjxl                                |
| SDL2 (GUI)       | 2.32.x   | zlib               | https://libsdl.org                          |
| Dear ImGui (GUI) | 1.91.x   | MIT                | https://github.com/ocornut/imgui            |
| nativefiledialog-extended (GUI) | - | zlib          | https://github.com/btzy/nativefiledialog-extended |
| doctest (tests only, not shipped) | 2.4.11 | MIT   | https://github.com/doctest/doctest          |
| MinGW-w64 runtime DLLs (libstdc++, libgcc, libwinpthread) | GCC 15 | GPL-3.0 with GCC Runtime Library Exception | https://gcc.gnu.org |

## Runtime components loaded at run time

| Component     | Version | Licence | Source |
|---------------|---------|---------|--------|
| ONNX Runtime (DirectML build) | 1.22.1 | MIT | https://github.com/microsoft/onnxruntime (release asset `Microsoft.ML.OnnxRuntime.DirectML.1.22.1.nupkg`); `ThirdPartyNotices.txt` from the package is shipped as `onnxruntime-ThirdPartyNotices.txt` |
| DirectML      | 1.15.4  | Microsoft Software License Terms (redistributable) | NuGet `Microsoft.AI.DirectML`; licence text shipped as `LICENSE-DirectML.txt` |
| stable-diffusion.cpp / ggml (stage 5) | pinned tag | MIT | https://github.com/leejet/stable-diffusion.cpp |

## Model weights

| Model | Licence | Source | Shipped in release |
|-------|---------|--------|--------------------|
| Johnson fast-neural-style: candy, mosaic, rain_princess, udnie (+ pointilism) | BSD-3-Clause (pytorch/examples); ONNX export from the ONNX Model Zoo (Apache-2.0 repository) | https://github.com/pytorch/examples/tree/main/fast_neural_style, https://github.com/onnx/models | yes |
| AdaIN (VGG encoder + decoder) | MIT (naoto0804/pytorch-AdaIN) | https://github.com/naoto0804/pytorch-AdaIN | yes (planned) |
| Stable Diffusion 1.5 | CreativeML OpenRAIL-M | Hugging Face | no - user download with licence prompt |
| IP-Adapter, ControlNet 1.1 | Apache-2.0 | Hugging Face | no - user download |

## Licence texts

### MIT (Dear ImGui, doctest, ONNX Runtime, stable-diffusion.cpp, ggml, pytorch-AdaIN)

```
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
```

### BSD-3-Clause (libwebp, libjxl, pytorch/examples models)

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib (SDL2, zlib, nativefiledialog-extended)

```
This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### libpng

```
PNG Reference Library License version 2

Copyright (c) 1995-2024 The PNG Reference Library Authors.
Copyright (c) 2018-2024 Cosmin Truta
Copyright (c) 1998-2018 Glenn Randers-Pehrson
Copyright (c) 1996-1997 Andreas Dilger
Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

The software is supplied "as is", without warranty of any kind, express or
implied, including, without limitation, the warranties of merchantability,
fitness for a particular purpose, title, and non-infringement. In no event
shall the Copyright owners, or anyone distributing the software, be liable
for any damages or other liability, whether in contract, tort or otherwise,
arising from, out of, or in connection with the software, or the use or other
dealings in the software, even if advised of the possibility of such damage.

Permission is hereby granted to use, copy, modify, and distribute this
software, or portions hereof, for any purpose, without fee, subject to the
following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated, but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This Copyright notice may not be removed or altered from any source or
   altered source distribution.
```

### libjpeg-turbo

libjpeg-turbo is covered by three compatible BSD-style open source licences:
the IJG (Independent JPEG Group) License, the Modified (3-clause) BSD License
(see above) and the zlib License (see above). The IJG notice:

```
This software is based in part on the work of the Independent JPEG Group.
```

### GCC Runtime Library Exception (MinGW-w64 libstdc++ / libgcc DLLs)

The MinGW runtime DLLs are GPL-3.0 with the GCC Runtime Library Exception,
which permits their redistribution together with programs compiled by GCC
regardless of the program's licence. See https://www.gnu.org/licenses/gcc-exception-3.1.html.

### Apache-2.0 (highway, ONNX Model Zoo repository, IP-Adapter, ControlNet)

Full text: https://www.apache.org/licenses/LICENSE-2.0

### DirectML

Microsoft DirectML is distributed under the Microsoft Software License Terms
for the "Microsoft.AI.DirectML" package, which allow redistribution of
`DirectML.dll` with applications. The full text ships as `LICENSE-DirectML.txt`.
