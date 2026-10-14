/*
 * Copyright 2026 Marcos Holgado
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * translate-kit C ABI.
 *
 * This is the platform-neutral surface of the engine. Every platform wrapper
 * (Android JNI, a future iOS/macOS bridge, Windows) sits on top of these
 * functions. It deals only in opaque handles, UTF-8 `char*`, and POD structs so
 * it can be bound from any FFI.
 *
 * Lifecycle mirrors the public Kotlin API (SPEC-1 section 5):
 *   tk_init                -> TranslateKit.init
 *   tk_detect_language     -> TranslateKit.detectLanguage
 *   tk_load_model          -> TranslateKit.loadModel
 *   tk_model_close         -> TranslateKit.unloadModel / TranslationModel.close
 *   tk_translate           -> TranslationModel.translate
 *   tk_translate_batch     -> TranslationModel.translate (batch overload)
 *
 * Thread-safety: a `tk_context` and the `tk_model`s loaded from it may be used
 * from a single background thread at a time. All calls are BLOCKING; the caller
 * is responsible for keeping them off the UI thread.
 *
 * Privacy: nothing here touches the network. Models are loaded from on-disk
 * paths supplied by the caller; the engine mmaps them.
 */

#ifndef TRANSLATE_KIT_TRANSLATE_KIT_H
#define TRANSLATE_KIT_TRANSLATE_KIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
   /* Building translatekit.dll, or linking the core statically: export.
    * Consumers of the DLL define TK_USING_DLL to import instead. */
#  if defined(TK_USING_DLL)
#    define TK_EXPORT __declspec(dllimport)
#  else
#    define TK_EXPORT __declspec(dllexport)
#  endif
#else
#  define TK_EXPORT __attribute__((visibility("default")))
#endif

/* ---- Status codes ------------------------------------------------------- */

typedef enum tk_status {
    TK_OK = 0,
    TK_ERR_INVALID_ARG = 1,      /* a required pointer/string was null or empty */
    TK_ERR_NOT_INITIALIZED = 2,  /* context is null or not initialized */
    TK_ERR_IO = 3,               /* a model/config/vocab file was missing or unreadable */
    TK_ERR_MODEL_LOAD = 4,       /* the engine rejected the model/config */
    TK_ERR_TRANSLATE = 5,        /* translation failed at runtime */
    TK_ERR_INTERNAL = 6          /* unexpected engine/internal error */
} tk_status;

/* ---- Opaque handles ----------------------------------------------------- */

/* Engine context. Owns the bundled language-ID model and shared engine state.
 * Created by tk_init, destroyed by tk_shutdown. */
typedef struct tk_context tk_context;

/* A single loaded (source -> target) translation model. Created by
 * tk_load_model, destroyed by tk_model_close. Holds mmap'd native memory. */
typedef struct tk_model tk_model;

/* ---- Value types -------------------------------------------------------- */

/* Language-detection outcome. `language` is a short BCP-47-ish code
 * (e.g. "en", "es", "zh"); `confidence` is in [0, 1]. */
typedef struct tk_language_result {
    char language[16];
    float confidence;
} tk_language_result;

/* Translation outcome. `text` is a heap-allocated, NUL-terminated UTF-8 string
 * owned by the library; release it with tk_translation_result_free.
 * `quality_scores` is optional (may be null); when present it has
 * `quality_len` entries. */
typedef struct tk_translation_result {
    char* text;
    float* quality_scores;
    size_t quality_len;
} tk_translation_result;

/* Inputs needed to load one translation model. All paths are to DECOMPRESSED
 * files on disk (the caller un-gzips Mozilla's `.gz` artifacts). Mirrors
 * SPEC-1 section 5 `ModelSpec`. `config_path` points at a pre-generated
 * Bergamot config YAML (produced off-device; SPEC-1 section 10a) and may be
 * null to fall back to the engine's per-architecture defaults. */
typedef struct tk_model_spec {
    const char* source_lang;          /* e.g. "es" */
    const char* target_lang;          /* e.g. "en" */
    const char* model_path;           /* model.<pair>.intgemm.alphas.bin */
    const char* const* vocab_paths;   /* 1 (shared) or 2 (src+tgt) .spm files */
    size_t vocab_count;
    const char* shortlist_path;       /* lex.<...>.s2t.bin */
    const char* config_path;          /* nullable: pre-generated bergamot config */
    int num_workers;                  /* >= 1 translation threads for this model */
} tk_model_spec;

/* ---- API ---------------------------------------------------------------- */

/* Library version string, e.g. "0.1.0". Never null. */
TK_EXPORT const char* tk_version(void);

/* Human-readable description of the last error on the calling thread, or "" if
 * none. The returned pointer is valid until the next tk_* call on this thread. */
TK_EXPORT const char* tk_last_error(void);

/* Initialize the engine. The bundled language detector (CLD2) is self-contained
 * — its profiles are compiled in, so no model file is needed. Idempotent at the
 * caller level (the Kotlin facade keeps one context). On success writes a
 * non-null handle to *out_ctx. Blocking. */
TK_EXPORT tk_status tk_init(tk_context** out_ctx);

/* Detect the language of `text` using the bundled model. Blocking. */
TK_EXPORT tk_status tk_detect_language(tk_context* ctx,
                                       const char* text,
                                       tk_language_result* out_result);

/* Load a translation model described by `spec`. On success writes a non-null
 * handle to *out_model. Returns TK_ERR_IO on missing/unreadable files and
 * TK_ERR_MODEL_LOAD if the engine rejects them. Blocking. */
TK_EXPORT tk_status tk_load_model(tk_context* ctx,
                                  const tk_model_spec* spec,
                                  tk_model** out_model);

/* Translate `input` (plain text, or an HTML fragment when is_html != 0). When
 * is_html is set, inline tags are preserved/re-aligned by the engine
 * (detag-and-project). On success fills *out_result; free it with
 * tk_translation_result_free. Blocking. */
TK_EXPORT tk_status tk_translate(tk_model* model,
                                 const char* input,
                                 int is_html,
                                 tk_translation_result* out_result);

/* Translate `count` inputs in one call (same order). Prefer this over calling
 * tk_translate per paragraph: the engine batches sentences across all inputs and
 * spreads the batches over the model's num_workers threads. `out_results` must
 * point to `count` structs; on TK_OK every one is filled (free each with
 * tk_translation_result_free); on error none are. Blocking. */
TK_EXPORT tk_status tk_translate_batch(tk_model* model,
                                       const char* const* inputs,
                                       size_t count,
                                       int is_html,
                                       tk_translation_result* out_results);

/* Release the contents of a translation result (its text + quality buffer).
 * Safe to call with a zero-initialized struct. Does not free `result` itself. */
TK_EXPORT void tk_translation_result_free(tk_translation_result* result);

/* Unload a model and release its native memory. Safe with null. */
TK_EXPORT void tk_model_close(tk_model* model);

/* Tear down the engine context. Safe with null. */
TK_EXPORT void tk_shutdown(tk_context* ctx);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* TRANSLATE_KIT_TRANSLATE_KIT_H */
