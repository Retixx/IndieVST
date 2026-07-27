# Third-party components

| Component | Version | License | How it is obtained |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 8.0.x | GPLv3 **or** commercial JUCE licence | CMake `FetchContent` at configure time |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) | bundled with JUCE | GPLv3 **or** Steinberg proprietary licence | ships inside JUCE |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | vendored at `third_party/nlohmann/json.hpp` |
| [Catch2](https://github.com/catchorg/Catch2) | 3.x | BSL-1.0 | CMake `FetchContent`, tests only (optional) |

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
