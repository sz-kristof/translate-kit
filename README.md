# translate-kit

On-device **neural machine translation** + **offline language detection** as a reusable native library.

`translate-kit` wraps the [Bergamot](https://github.com/mozilla/translations) translation engine
(a fork of Marian NMT with quantized int8 student models) and the bundled
[CLD2](https://github.com/CLD2Owners/cld2) Compact Language Detector, and exposes them through a thin,
platform-neutral C ABI. It ships two platform wrappers today — **Android** (JNI → Kotlin, published as a
Maven AAR) and **Apple** (macOS/iOS, a Swift package over a prebuilt XCFramework) — both layered on the
same core. **Windows** builds the same core as `translatekit.dll`, exposing the C ABI directly (callable
from C/C++ or from .NET via P/Invoke).

CLD2 is self-contained — its language profiles are compiled into the library, so language detection
needs no model file and no network.

> **Privacy:** the library never touches the network. It does no model downloading and consumes only
> on-disk model paths supplied by the caller. Everything stays on-device.

## Requirements

**Android**
- **minSdk 28.** Forced by the Bergamot engine: marian's `pathie-cpp` uses `glob()`/`iconv()`, which
  Android bionic only provides at API 28+. Consumers on a lower app minSdk gate the feature off below 28.
- **NDK 28.2.13676358**, CMake 3.22+. Language detection (CLD2) alone has no such floor.

**Apple**
- **macOS 12+ / iOS 15+** deployment targets.
- **Xcode** (Swift 5.9+) to consume the Swift package. Building the XCFramework from source additionally
  needs **CMake 3.22+** and **Ninja**.

**Windows**
- **x64**, any CPU: the DLL targets the portable x86-64-v2 baseline and intgemm picks its AVX2 int8
  kernels at runtime. The C/C++ runtime is linked statically, so no Visual C++ Redistributable is needed.
- Building needs **Visual Studio 2022+** with the C++ workload (its bundled CMake and Ninja are enough)
  and Git for Windows (bash, for the patch/fetch scripts).

## Repository layout

```
core/        platform-agnostic C++ engine wrapper + the C ABI (include/translate_kit/translate_kit.h)
android/     Android Gradle project — the AAR wrapper (JNI shim + Kotlin facade)
apple/       Apple wrapper — Swift package (TranslateKit) over the prebuilt XCFramework
windows/     Windows build — translatekit.dll (C ABI exports) + the C-ABI smoke test
models/      test-model fixtures (fetched by scripts/, gitignored) — debug/test only, never in the release AAR
scripts/     cross-compile + model-fetch + engine-patch helpers
third_party/ git submodules: cld2 (detector), translations (Bergamot engine); patches/ (engine patches)
```

The translation engine wrapper lives in `core/` and is consumed by every platform wrapper through the
C ABI in `core/include/translate_kit/translate_kit.h`. The Android module builds it via Gradle
`externalNativeBuild` (CMake); other platforms use `scripts/build-*.sh`.

## Building from source

```bash
# 1. Engine submodules (the inference subset is enough for the build):
git submodule update --init --checkout --recursive third_party/cld2
git submodule update --init --checkout third_party/translations
git -C third_party/translations submodule update --init --recursive \
    inference/3rd_party/ssplit-cpp \
    inference/marian-fork/src/3rd_party/sentencepiece \
    inference/marian-fork/src/3rd_party/intgemm \
    inference/marian-fork/src/3rd_party/ruy \
    inference/marian-fork/src/3rd_party/simd_utils

# 2. Apply translate-kit's local engine patches (see third_party/patches/):
scripts/apply-engine-patches.sh
```

Steps 1–2 are shared by all platforms; the build itself is per-platform.

**Android** (JDK 17/21) — builds the AAR:

```bash
cd android && ./gradlew :translate-kit:assembleRelease
```

**Apple** (Xcode + CMake/Ninja) — builds the per-slice merged static libs and assembles
`apple/build/TranslateKit.xcframework`:

```bash
scripts/build-apple.sh                            # all 5 slices (universal macOS + iOS device + sim)
scripts/build-apple.sh macos-arm64 iossim-arm64   # or a subset; the XCFramework is assembled from
                                                  # whatever was built (this is what CI builds)
```

**Windows** (Visual Studio 2022+, from PowerShell) — builds `build/windows/translatekit.dll` plus its import
library and runs the C-ABI smoke test against it (live translation too, after `scripts/fetch-models.sh`):

```powershell
./scripts/build-windows.ps1
```

## Android usage (Kotlin)

```kotlin
TranslateKit.init(context)                 // loads the native library + inits the engine. Idempotent. Blocking.

val lang = TranslateKit.detectLanguage(text)            // LanguageResult(language, confidence)

val model = TranslateKit.loadModel(
    ModelSpec(
        sourceLang = "es",
        targetLang = "en",
        modelPath = "/…/model.esen.intgemm.alphas.bin",
        vocabPaths = listOf("/…/vocab.esen.spm"),        // 1 (shared) or 2 (src+tgt)
        shortlistPath = "/…/lex.50.50.esen.s2t.bin",
        configYaml = "/…/config.esen.yml",               // pre-generated off-device
    ),
)
val result = model.translate(htmlFragment, isHtml = true)  // inline tags preserved (detag-and-project)
model.close()
```

All native calls are **blocking** — call them off the main thread (e.g. a coroutine on an IO dispatcher).
`translate(input, isHtml = true)` preserves inline HTML tags via the engine's alignment-based
detag-and-project. The library is single-hop per loaded model; non-English↔non-English pairs are pivoted
through English by the caller.

## Apple usage (Swift)

```swift
import TranslateKit

let kit = try TranslateKit()                            // inits the engine. Blocking.

let lang = try kit.detectLanguage(text)                 // LanguageResult(language, confidence)

let model = try kit.loadModel(
    ModelSpec(
        sourceLang: "es",
        targetLang: "en",
        modelPath: "/…/model.esen.intgemm.alphas.bin",
        vocabPaths: ["/…/vocab.esen.spm"],              // 1 (shared) or 2 (src+tgt)
        shortlistPath: "/…/lex.50.50.esen.s2t.bin",
        configYaml: "/…/config.esen.yml"                // pre-generated off-device; nil → engine defaults
    )
)
let result = try model.translate(htmlFragment, isHtml: true)  // inline tags preserved
model.close()
```

The Swift API mirrors the Android facade 1:1: `TranslateKit` (`init()` / `detectLanguage(_:)` /
`loadModel(_:)` / `unloadModel(_:)`) and `TranslationModel` (`translate(_:isHtml:)`, a batch overload,
and `close()` — also wired to `deinit`). Language detection (CLD2) needs no model file. All calls are
**blocking** — invoke them off the main thread (e.g. a background `Task`/`DispatchQueue`); a `TranslateKit`
and the `TranslationModel`s loaded from it are **not** thread-safe, so serialize access. Deployment
targets are **macOS 12 / iOS 15**.

The library is distributed as a prebuilt XCFramework via Swift Package Manager — see
[Coordinates](#coordinates) below.

## Windows usage (C ABI / .NET)

C and C++ callers include `translate_kit/translate_kit.h`, define `TK_USING_DLL`, and link
`translatekit.lib`. From .NET, P/Invoke the same functions; pass text as **UTF-8**
(`UnmanagedType.LPUTF8Str`), since .NET's default ANSI string marshalling is not UTF-8:

```csharp
[DllImport("translatekit")] static extern int tk_init(out IntPtr ctx);
[DllImport("translatekit")] static extern int tk_detect_language(IntPtr ctx,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string text, out LanguageResult result);
[DllImport("translatekit")] static extern int tk_translate(IntPtr model,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string input, int isHtml, out TranslationResult result);
// inputs: UTF-8 strings from Marshal.StringToCoTaskMemUTF8; free each result with tk_translation_result_free
[DllImport("translatekit")] static extern int tk_translate_batch(IntPtr model, IntPtr[] inputs,
    UIntPtr count, int isHtml, [Out] TranslationResult[] results);
```

Text that is not well-formed UTF-8 is repaired (ill-formed bytes become U+FFFD) before it reaches CLD2 or
the engine, so a stray byte can neither crash detection nor blank out a translation.

Measured on a Ryzen 9 5900X with the `ende.student.tiny11` test model, one worker: ~2,000 words/s
(a 100-word paragraph in ~51 ms), model load ~44 ms, language detection ~13 µs. Pinning the whole
engine to `/arch:AVX2` (marian's MSVC default) was ~20% *slower* than this portable build.

## Throughput: batches and workers

`num_workers` in `tk_model_spec` is the number of translation threads for that model. Each worker
holds its own model replica, so workers trade memory for speed. The engine batches sentences across
all inputs of a call, so translate a page with **`tk_translate_batch`** rather than one `tk_translate`
per paragraph.

`core/test/bench_cli`, Hungarian → English, 146 paragraphs (10,763 words) of the Hungarian Wikipedia
article *Budapest*, Ryzen 9 5900X (12 cores) on an ordinary, not idle, desktop. Median of 5 runs
(range), after an untimed warm-up, one worker count per process:

| workers | one `tk_translate` per paragraph | one `tk_translate_batch` | peak RAM |
|--------:|---------------------------------:|-------------------------:|---------:|
| 1 | 652 words/s | 875 (833–931) words/s | 185 MB |
| 2 | 633 words/s | 1,187 (1,159–1,720) words/s | 274 MB |
| 4 | 623 words/s | 1,838 (1,796–2,694) words/s | 512 MB |
| 8 | 614 words/s | 2,105 (2,026–3,025) words/s | 982 MB |

Batching alone is ~1.3× faster at no memory cost. Each extra worker costs ~120 MB with these models:
4 workers give ~2.8× the per-paragraph throughput, 8 only ~15% more than 4 for twice the memory.
Per-paragraph calls barely benefit from extra workers, since a paragraph is usually a single batch.
The default stays `num_workers = 1` (also the Android and Apple wrappers' default), so existing callers
get the same threads and memory as before.

## Coordinates

**Android (Maven Central)**

```
com.duckduckgo.translatekit:translate-kit-android:<version>
```

**Apple (Swift Package Manager)**

Add the package by Git URL + version; SwiftPM downloads the prebuilt XCFramework from the matching
GitHub Release, so consumers don't clone this repo or build the C++ engine:

```swift
// In your Package.swift dependencies:
.package(url: "https://github.com/duckduckgo/translate-kit.git", from: "0.1.0"),
// …then add the "TranslateKit" product to your target's dependencies.
```

Both artifacts share one version and are published together by the `Release` workflow
(`.github/workflows/release.yml`).

## Model-file contract

Identical on both wrappers: the library consumes **decompressed** model files (Mozilla ships them
gzipped; decompression is the caller's job) plus a pre-generated Bergamot config YAML (the config path is
**nullable** — omit it to fall back to the engine's per-architecture defaults). Nothing is bundled and
nothing is downloaded — the library only opens the on-disk paths you pass it. See `SPEC-1 §10a`.

## License

Apache-2.0 (this project). Bundled/linked third-party components retain their own licenses — see
[`NOTICE`](NOTICE) and `THIRD_PARTY_LICENSES.md`.
