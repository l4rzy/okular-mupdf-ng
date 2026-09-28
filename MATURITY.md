# Feature Maturity

This page summarizes the maturity of the features users can access in
okular-mupdf-ng and the kind of automated testing currently present for them.
The project is in beta; even features marked **Stable** are not a guarantee of
production readiness. For important PDF editing, use Okular's official PDF
backend as recommended in the project README.

## Ratings

- 🟢 **Stable** — intended for regular use within the listed capability; well tested
and observed stable on different Linux platforms.
- 🟡 **Partial** — usable, but not thoroughly tested with a large PDF corpus.
- 🟣 **Experimental** — available for evaluation; tested locally with a very small
corpus; might missing some features.

The test column describes the kind of automated coverage present, not its
thoroughness. Worker tests exercise document-engine behavior; integration tests
exercise communication across the worker boundary. A test label is not a
promise that every input or workflow has been tested.

| Feature | Maturity | Automated test coverage |
|---|---|---|
| Open PDF and ePUB documents | 🟢 Stable | Worker and IPC integration |
| Render PDF pages | 🟢 Stable | Worker and IPC integration |
| Text extraction and search | 🟢 Stable | Worker tests; text-page conversion tests |
| Metadata, fonts, and embedded files | 🟢 Stable | Worker and generator tests |
| Linux sandboxed document processing | 🟢 Stable | Security and IPC integration tests |
| Render ePUB pages | 🟡 Partial | Worker and IPC integration |
| ePUB custom CSS and page sizes | 🟡 Partial | Worker tests; settings tests |
| Export ePUB to PDF | 🟡 Partial | Worker and IPC integration |
| OCR for scanned PDF pages | 🟡 Partial | Worker and plugin integration tests |
| Outlines, links, and navigation | 🟡 Partial | Worker tests; EPUB link and outline tests 
| PDF annotations | 🟡 Partial | Generator proxy and worker tests |
| Printing and document export | 🟡 Partial | Export integration; printing has no direct automated test |
| NSS certificate manager | 🟣 Experimental | NSS runtime and certificate tests |
| PDF signature verification and signing | 🟣 Experimental | Signature validation and worker tests |
| PDF form filling | 🟣 Experimental | Generator proxy and worker tests |
| Command-line utility (mupdfng-cli) | 🟣 Experimental | CLI argument-parsing tests; XFDF end-to-end tests |
| XFDF import and export | 🟣 Experimental | Parser and generator tests; CLI integration |

## Known limitations

- MuPDF's ePUB support is limited compared with its PDF support, so some ePUB
  content or behavior may not be handled fully.
- XFDF coordinates are serialized in the page's rotated display frame, which
  can displace annotations on rotated pages. Import also requires an explicit
  rectangle and skips unsupported or malformed annotations with a warning.
- The sandbox is best-effort: available protections depend on Linux kernel and
  system support, and the worker reports when hardening is degraded.
- Form and annotation changes that have not been saved can be lost if the
  worker stops and the document must be reopened.

## Tracked Okular issues

| Issue | Status |
|---|---|
| [Bug 525809](https://bugs.kde.org/show_bug.cgi?id=525809) | Merged |
| [Bug 525842](https://bugs.kde.org/show_bug.cgi?id=525842) | Not merged yet |
