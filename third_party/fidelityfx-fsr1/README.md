# AMD FidelityFX Super Resolution 1

`ffx_a.h` and `ffx_fsr1.h`, verbatim from
[GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
(`ffx-fsr/`), version 1.20210629. MIT licensed -- see `LICENSE`.

Kept as reference, not compiled. The headers are a large HLSL/GLSL
portability layer built around packed 16-bit maths and preprocessor
configuration, which is more than render-host needs for one compute pass;
`render-host/shaders/upscale.comp` ports EASU's real arithmetic out of
them into plain GLSL, with the reference's own variable names so the two
can be read side by side.

They are here because the first attempt wrote EASU's weights from memory
instead, and got them wrong -- visibly, as scratched edges, blocky patches
and distorted text. Anything touching that shader should be checked
against these files rather than against a recollection of them.
