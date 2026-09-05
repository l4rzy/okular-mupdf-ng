// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TEST_GENPDF_HPP
#define TEST_GENPDF_HPP

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QString>

void createMultiPagePDF(fz_context* ctx, const QString& path, int numPages);
void createTextPDF(fz_context* ctx, const QString& path);
void createShiftedCropTextPDF(fz_context* ctx, const QString& path);
void createEncryptedPDF(fz_context* ctx, const QString& path, const QString& password);
void createSignaturePDF(fz_context* ctx, const QString& path);
void createEditableTextFieldPDF(fz_context* ctx, const QString& path);
void createScannedPDF(fz_context* ctx, const QString& srcTextPdfPath, const QString& scannedPath);

#endif
