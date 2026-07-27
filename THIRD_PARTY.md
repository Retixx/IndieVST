# Third-party components

| Component | Version | License | How it is obtained |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 8.0.x | GPLv3 **or** commercial JUCE licence | CMake `FetchContent` at configure time |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) | bundled with JUCE | GPLv3 **or** Steinberg proprietary licence | ships inside JUCE |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | vendored at `third_party/nlohmann/json.hpp` |
| [Catch2](https://github.com/catchorg/Catch2) | 3.x | BSL-1.0 | CMake `FetchContent`, tests only (optional) |
| [Inter](https://github.com/rsms/inter) | 4.x | SIL Open Font License 1.1 | vendored at `resources/fonts/`, embedded in the binary |

## Fonts

Inter is embedded in three static weights (Regular 400, Medium 500, SemiBold 600),
instanced from the upstream variable font and subset to Latin so each weight is ~41 KB.
Embedding rather than depending on installed system fonts is what makes the plugin look
identical on every machine.

The SIL OFL permits bundling and redistribution, including in commercial and closed-source
products, provided the licence text travels with the fonts — it does, at
`resources/fonts/Inter-OFL.txt`. The only real restriction is that a modified font may not
be redistributed under the reserved name "Inter"; subsetting and instancing for embedding
do not trigger that.

## Licensing position

Forge is currently developed under the **GPLv3** arms of both the JUCE and VST3 SDK
dual licences. That is fine for the MVP, internal testing, and the pitch demo.

Shipping Forge as a **closed-source commercial product** requires, before release:

1. A paid **JUCE commercial licence** (per-seat / revenue tiered), and
2. Signing the **Steinberg VST3 proprietary licence agreement** (free of charge) and
   registering the product with Steinberg.

Neither is a blocker; both are known, budgeted steps. See `SPEC.md` §3.1.

nlohmann/json is MIT and imposes only an attribution requirement, satisfied by this
file and the header's own embedded licence comment.
