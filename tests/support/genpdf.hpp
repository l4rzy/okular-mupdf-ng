/***************************************************************************
 *   Copyright (C) 2026 by l4rzy <me@23ro.org>                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

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
