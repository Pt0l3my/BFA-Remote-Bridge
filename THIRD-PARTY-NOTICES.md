# Third-party notices

BFA Remote Bridge is MIT-licensed (LICENSE). It includes or is built with the following, each under its own licence.

## ONNX Runtime's C API headers (vendored)

`third_party/onnxruntime/onnxruntime_c_api.h` and `third_party/onnxruntime/onnxruntime_ep_c_api.h` are ONNX Runtime
1.24.4's, unchanged, from <https://github.com/microsoft/onnxruntime> at tag `v1.24.4`
(`include/onnxruntime/core/session/`). The bridge uses only their declarations: it ships no ONNX Runtime code and
loads no runtime of its own; it calls the runtime Better Facial Animations loads. Their licence, MIT, is in
`third_party/onnxruntime/LICENSE`:

> MIT License
>
> Copyright (c) Microsoft Corporation
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
> permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of
> the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
> TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

The layout of `OrtDmlApi` that `src/core/Bridge.cpp` declares (eight entries) follows ONNX Runtime 1.24.4's
`dml_provider_factory.h`, also MIT.

## Linked into BFARemoteBridge.dll (fetched by vcpkg at build time, never committed)

- **CommonLibSSE-NG**, its MIT line (<https://github.com/marthofdoom/CommonLibSSE-NG>), MIT.
- **spdlog** (<https://github.com/gabime/spdlog>), MIT; and **fmt** (<https://github.com/fmtlib/fmt>), MIT.

## Not included

No file of Better Facial Animations (GPL-3.0), NVIDIA's Audio2Face or Audio2Emotion models, ONNX Runtime's DLLs or
DirectML is in this repository or its release. The tests' tiny model is made by `tests/make_test_model.py` when the
tests run.
