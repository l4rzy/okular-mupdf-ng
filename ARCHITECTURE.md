# Architecture

okular-mupdf-ng keeps document parsing, rendering, and mutation in a separate
MuPDF worker. The host-side generator owns Okular integration; it never shares
MuPDF objects or C++ pointers with the worker.

```mermaid
sequenceDiagram
    actor Okular
    participant Generator as Generator (Okular/KDE)
    participant Plugin as Plugin (Qt host bridge)
    box Sandboxed worker
        participant Runtime as Command service
        participant MuPDF as MuPDF engine
    end

    Okular->>Generator: document action
    Generator->>Plugin: model request
    Plugin->>Runtime: serialized control request
    Runtime->>MuPDF: document operation
    MuPDF-->>Runtime: model result
    Runtime-->>Plugin: serialized response
    Plugin-->>Generator: shared model value
    Generator-->>Okular: updated document state

    opt Rendering
        Runtime->>Runtime: render into memfd
        Runtime-->>Plugin: frame metadata + FD (SCM_RIGHTS)
        Plugin->>Plugin: validate and wrap mapped pixels as QImage (no copy)
        Plugin-->>Generator: pass QImage
        Generator-->>Okular: pass QImage
    end
```

## Layer boundaries

### Generator

`src/generator/` is the only Okular/KDE-facing layer. It implements the
generator interface, exposes configuration UI, and converts shared models into
Okular pages, annotations, form proxies, signatures, and document metadata.
Configuration-to-model translation also lives here.

### Plugin

`src/plugin/` is a pure-Qt host bridge: it has no Okular or generator type
dependency. It starts and supervises the worker, owns the IPC client and frame
mapping, manages OCR scheduling and persistent caches, and hosts NSS-backed
certificate and signing support. The dependency also runs generator-to-plugin:
`crypto`/`util`/`caching` are in-process libraries with two consumers — the
bridge and the generator (in-process NSS work, cert dialogs, pure helpers) —
while out-of-process work always goes through the `WorkerClient` facade (e.g.
`Signature` holds a `WorkerClient*` backend). Do not "fix" the
generator-to-plugin link edges; the boundary that must stay clean is
plugin-to-generator/Okular (enforced by `test_plugin_boundary`).

### Shared layer

`src/shared/` defines protocol messages, serializable data models, validation,
logging, transport helpers, and shared-memory frame rules. It is the only data
contract between host and worker and has no Okular/KDE GUI dependency.

### Worker

`src/worker/` is a native C++23 process with no Qt or Okular dependency. Its
runtime dispatches requests to PDF, EPUB, and MOBI document engines. The engines own
MuPDF document lifetime, page operations, rendering, forms, annotations,
signatures, OCR, metadata, and save operations.

`test_worker_link_boundary` rejects all Qt, Okular, and KDE shared-library
dependencies and checks worker and shared source includes for Qt, KDE, Okular,
and host-layer headers. The plugin-boundary test enforces the plugin's lack of
Okular and generator dependencies.

## IPC and rendering

The plugin creates private Unix-domain control and descriptor sockets for each
worker session. Control requests are serialized shared-model messages. Input
documents and output files are passed only as descriptors; the worker does not
open arbitrary document paths.

Rendered frames use a separate descriptor path. Each frame contains a fixed
header followed by RGBA pixels, and the plugin validates its header and
geometry before exposing the mapped pixels directly through a `QImage`; no
pixel copy is made.

The worker keeps up to eight reusable `memfd` slots with a combined 128 MiB
budget. A new slot's descriptor is passed with `SCM_RIGHTS` once, then later
renders reuse the plugin's existing read-only mapping. When the final `QImage`
reference is destroyed, the plugin releases the slot lease and the worker may
reuse it. If no compatible slot is available, the worker uses a transient
frame, which the plugin unmaps when its final `QImage` reference is destroyed.

One rendered frame is limited to 128 MiB including its header. Valid requests
that exceed this budget are scaled down before allocation; Okular can scale the
returned image to the requested display size. Invalid request geometry is rejected.

## Persistent caches

Persistent cache code lives in `src/plugin/caching/` and is owned by the host,
not the sandboxed worker.

- Generated PDF TOCs are cached by source SHA-256 and heading algorithm revision;
  embedded outlines take precedence, and empty generated TOCs are cached too.
- OCR results are cached per document identity, language, DPI, and page. A
  valid empty OCR page is retained as a cache hit.
- EPUB uses one cache file per document and layout fingerprint. The file has
  independently stored accelerator and compressed-outline sections, so either
  result can be added without discarding the other.
- The EPUB layout fingerprint includes font size, family, page size, and custom
  CSS. Changing any of them intentionally selects a different accelerator and
  outline cache entry.

## Worker security boundary

The worker is a capability process. The parent is the capability broker: it
passes only the descriptors and configured read-only resource directories the
worker needs.

On Linux, hardening is applied after IPC endpoints are established:

- Landlock restricts filesystem access to the build-time tessdata directory,
  the bundled signature-font directory, and optional configured tessdata directories.
- User, network, and IPC namespaces isolate identity and network access when
  unprivileged namespaces are available.
- Seccomp restricts system calls when libseccomp and the host kernel support
  it.
- Resource limits cap address space at 4 GiB. Per-operation CPU and elapsed-time
  budgets terminate the worker on exhaustion; release builds also disable
  worker core dumps and close nonessential inherited descriptors.

Worker sandbox activation is best-effort for portability, and the worker reports
its actual status through the initial ping. The Okular generator defaults to
Strict enforcement: it withholds documents unless the worker reports a fully
hardened sandbox. Relaxed enforcement permits processing with degraded protection.

The worker supports PDF, EPUB, and MOBI. Its build-time tessdata default is
`TESSDATA_DIR` (`/usr/share/tessdata` by default); the plugin may pass
additional absolute directories from its hidden advanced configuration. The
worker also provides `--help`, `--version`, and `--sandbox-check` for diagnostics,
although document processing is exclusively through private host IPC.

## Build and test layout

The default build downloads and statically links the pinned MuPDF found
in `cmake/mupdf.version`. MuPDF is built with PDF, EPUB, and MOBI support while
unused document formats and curl support are disabled. PDF form JavaScript is
enabled by default (`ENABLE_FORM_JAVASCRIPT=ON`); bundled builds include MuJS
when this option is enabled. Linker garbage collection
removes unused static sections. `-DUSE_SYSTEM_MUPDF=ON` opts into a compatible
system package.

MuPDF-linked worker and integration suites share the `test_mupdf` executable
to avoid repeatedly linking the static engine. Cache, generator, security,
frame-pool, and boundary tests use focused executables or CMake scripts.

## Source tree

```text
src/
├── generator/          # Okular integration, conversion, proxies, configuration
├── plugin/             # Qt worker bridge, caching, OCR, NSS, XFDF, utilities
├── shared/             # IPC protocol, models, transport, validation
├── tools/cli/          # command-line interface for worker-backed document tools
└── worker/             # isolated native worker
    ├── engine/         # PDF/EPUB/MOBI engines, OCR, signing
    ├── runtime/        # request dispatch and worker server
    └── sys/            # sandboxing, limits, system integration
```
