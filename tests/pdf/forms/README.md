# AcroForm regression fixtures

- `application.pdf` is an original synthetic two-page form. Regenerate it with
  `python3 tests/pdf/forms/generate.py`. It includes printable text, Unicode
  input, a length-limited field, a read-only field, multiline text on a rotated
  page, a checkbox, cross-page radio buttons, a multiselect list with distinct
  export values, an editable combo, and a non-JavaScript reset button.
- `encrypted.pdf` is the same form encrypted with AES-256, user password
  `forms-test` and owner password `forms-owner`. Regenerate after `application.pdf`:
  `mutool clean -E aes-256 -U forms-test -O forms-owner tests/pdf/forms/application.pdf tests/pdf/forms/encrypted.pdf`.
- `libreoffice.pdf` is KDE Okular's `autotests/data/formSamples.pdf`, produced by
  LibreOffice 3.6. Jon Mease added it with the form-editing tests in upstream
  commit `126b9fdf8c581610f3af54b6906f00b3d814e9c9`. Copied unchanged from Okular
  revision `eecd4051634baeaabde9738e6224eff4c29f6d2f`; its accompanying tests are
  GPL-2.0-or-later. Upstream: https://invent.kde.org/graphics/okular.

The generator tests load the actual plugin and worker. They exercise edits,
undo/redo, saved values, source isolation, reset, and unsaved values printed to
PDF. These fixtures provide regression coverage, not a large real-world corpus
or validation of physical printer submission. JavaScript is disabled throughout.
