/***************************************************************************
 *   Copyright (C) 2026 by l4rzy <me@23ro.org>                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "genpdf.hpp"

#include <QDebug>
#include <QFile>
#include <cstring>

void createMultiPagePDF(fz_context* ctx, const QString& path, int numPages)
{
    fz_try(ctx)
    {
        pdf_document* doc = pdf_create_document(ctx);

        for (int i = 0; i < numPages; ++i) {
            const char* emptyContent = "q Q\n";
            fz_buffer* contents =
                fz_new_buffer_from_copied_data(ctx, (const unsigned char*)emptyContent, std::strlen(emptyContent));
            pdf_obj* resources = pdf_new_dict(ctx, doc, 0);
            pdf_obj* page = pdf_add_page(ctx, doc, fz_unit_rect, 0, resources, contents);
            pdf_insert_page(ctx, doc, -1, page);
            pdf_drop_obj(ctx, page);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);
        }

        pdf_save_document(ctx, doc, QFile::encodeName(path).constData(), &pdf_default_write_options);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createMultiPagePDF failed:" << fz_caught_message(ctx);
    }
}

void createTextPDF(fz_context* ctx, const QString& path)
{
    fz_try(ctx)
    {
        pdf_document* doc = pdf_create_document(ctx);

        const char* streamData = "BT\n/F1 12 Tf\n72 700 Td(Hello World)Tj\nET\n";
        fz_buffer* contents =
            fz_new_buffer_from_copied_data(ctx, (const unsigned char*)streamData, std::strlen(streamData));

        pdf_obj* fontDict = pdf_new_dict(ctx, doc, 3);
        {
            pdf_obj* val = pdf_new_name(ctx, "Font");
            pdf_dict_puts(ctx, fontDict, "Type", val);
            pdf_drop_obj(ctx, val);
        }
        {
            pdf_obj* val = pdf_new_name(ctx, "Type1");
            pdf_dict_puts(ctx, fontDict, "Subtype", val);
            pdf_drop_obj(ctx, val);
        }
        {
            pdf_obj* val = pdf_new_name(ctx, "Helvetica");
            pdf_dict_puts(ctx, fontDict, "BaseFont", val);
            pdf_drop_obj(ctx, val);
        }

        pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, fonts, "F1", fontDict);
        pdf_drop_obj(ctx, fontDict);

        pdf_obj* resources = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, resources, "Font", fonts);
        pdf_drop_obj(ctx, fonts);

        fz_rect mediabox = { 0, 0, 612, 792 };
        pdf_obj* page = pdf_add_page(ctx, doc, mediabox, 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, page);

        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);

        pdf_save_document(ctx, doc, QFile::encodeName(path).constData(), &pdf_default_write_options);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createTextPDF failed:" << fz_caught_message(ctx);
    }
}

void createSignaturePDF(fz_context* ctx, const QString& path)
{
    fz_try(ctx)
    {
        pdf_document* doc = pdf_create_document(ctx);
        const char* emptyContent = "q Q\n";
        fz_buffer* contents = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(emptyContent), std::strlen(emptyContent));
        pdf_obj* resources = pdf_new_dict(ctx, doc, 0);
        pdf_obj* page = pdf_add_page(ctx, doc, { 0, 0, 612, 792 }, 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, page);
        pdf_page* pdfPage = pdf_load_page(ctx, doc, 0);
        pdf_annot* widget = pdf_create_signature_widget(ctx, pdfPage, const_cast<char*>("Approval"));
        pdf_set_annot_rect(ctx, widget, { 72, 650, 240, 700 });
        pdf_drop_annot(ctx, widget);
        pdf_drop_page(ctx, pdfPage);
        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);
        pdf_save_document(ctx, doc, QFile::encodeName(path).constData(), &pdf_default_write_options);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createSignaturePDF failed:" << fz_caught_message(ctx);
    }
}

void createEditableTextFieldPDF(fz_context* ctx, const QString& path)
{
    fz_try(ctx)
    {
        pdf_document* doc = pdf_create_document(ctx);
        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_obj* acroForm = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, acroForm, PDF_NAME(Fields), pdf_new_array(ctx, doc, 0));
        pdf_dict_put_drop(ctx, catalog, PDF_NAME(AcroForm), acroForm);
        const char* emptyContent = "q Q\n";
        fz_buffer* contents = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(emptyContent), std::strlen(emptyContent));
        pdf_obj* resources = pdf_new_dict(ctx, doc, 0);
        pdf_obj* pageObject = pdf_add_page(ctx, doc, { 0, 0, 612, 792 }, 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, pageObject);
        pdf_page* page = pdf_load_page(ctx, doc, 0);
        pdf_annot* widget = pdf_create_annot(ctx, page, PDF_ANNOT_WIDGET);
        pdf_obj* field = pdf_annot_obj(ctx, widget);
        pdf_dict_put(ctx, field, PDF_NAME(FT), PDF_NAME(Tx));
        pdf_dict_put_text_string(ctx, field, PDF_NAME(T), "EditableField");
        pdf_set_annot_rect(ctx, widget, { 72, 650, 240, 700 });
        pdf_update_page(ctx, page);
        pdf_drop_annot(ctx, widget);
        pdf_drop_page(ctx, page);
        pdf_drop_obj(ctx, pageObject);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);
        pdf_save_document(ctx, doc, QFile::encodeName(path).constData(), &pdf_default_write_options);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createEditableTextFieldPDF failed:" << fz_caught_message(ctx);
    }
}

void createShiftedCropTextPDF(fz_context* ctx, const QString& path)
{
    fz_try(ctx)
    {
        pdf_document* doc = pdf_create_document(ctx);
        const char* streamData = "q 0 0 0 rg 120 620 30 30 re f Q\nBT\n/F1 12 Tf\n120 600 Td(Shifted)Tj\nET\n";
        fz_buffer* contents = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(streamData), std::strlen(streamData));

        pdf_obj* fontDict = pdf_new_dict(ctx, doc, 3);
        pdf_dict_puts(ctx, fontDict, "Type", pdf_new_name(ctx, "Font"));
        pdf_dict_puts(ctx, fontDict, "Subtype", pdf_new_name(ctx, "Type1"));
        pdf_dict_puts(ctx, fontDict, "BaseFont", pdf_new_name(ctx, "Helvetica"));
        pdf_obj* fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, fonts, "F1", fontDict);
        pdf_drop_obj(ctx, fontDict);
        pdf_obj* resources = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, resources, "Font", fonts);
        pdf_drop_obj(ctx, fonts);

        const fz_rect mediaBox { 0, 0, 612, 792 };
        pdf_obj* page = pdf_add_page(ctx, doc, mediaBox, 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, page);
        pdf_page* pdfPage = pdf_load_page(ctx, doc, 0);
        pdf_set_page_box(ctx, pdfPage, FZ_CROP_BOX, { 100, 100, 500, 700 });
        pdf_drop_page(ctx, pdfPage);
        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);

        pdf_save_document(ctx, doc, QFile::encodeName(path).constData(), &pdf_default_write_options);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createShiftedCropTextPDF failed:" << fz_caught_message(ctx);
    }
}

void createScannedPDF(fz_context* ctx, const QString& srcTextPdfPath, const QString& scannedPath)
{
    fz_try(ctx)
    {
        fz_document* srcDoc = fz_open_document(ctx, QFile::encodeName(srcTextPdfPath).constData());
        fz_page* srcPage = fz_load_page(ctx, srcDoc, 0);

        fz_matrix ctm = fz_scale(2.f, 2.f);
        fz_pixmap* pix = fz_new_pixmap_from_page(ctx, srcPage, ctm, fz_device_rgb(ctx), 0);
        fz_image* img = fz_new_image_from_pixmap(ctx, pix, nullptr);

        pdf_document* doc = pdf_create_document(ctx);
        pdf_obj* imgRef = pdf_add_image(ctx, doc, img);

        const float w = static_cast<float>(fz_pixmap_width(ctx, pix));
        const float h = static_cast<float>(fz_pixmap_height(ctx, pix));
        const QByteArray stream =
            "q " + QByteArray::number(w) + " 0 0 " + QByteArray::number(h) + " 0 0 cm /Im0 Do Q\n";
        fz_buffer* contents = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(stream.constData()), static_cast<std::size_t>(stream.size()));

        pdf_obj* resources = pdf_new_dict(ctx, doc, 1);
        pdf_obj* xobj = pdf_new_dict(ctx, doc, 1);
        pdf_dict_puts(ctx, xobj, "Im0", imgRef);
        pdf_dict_puts(ctx, resources, "XObject", xobj);
        pdf_drop_obj(ctx, xobj);

        fz_rect mediabox = { 0, 0, w, h };
        pdf_obj* page = pdf_add_page(ctx, doc, mediabox, 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, page);

        pdf_save_document(ctx, doc, QFile::encodeName(scannedPath).constData(), &pdf_default_write_options);

        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, imgRef);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);
        fz_drop_image(ctx, img);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, srcPage);
        fz_drop_document(ctx, srcDoc);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createScannedPDF failed:" << fz_caught_message(ctx);
    }
}

void createEncryptedPDF(fz_context* ctx, const QString& path, const QString& password)
{
    fz_try(ctx)
    {
        pdf_document* doc = pdf_create_document(ctx);

        const char* emptyContent = "q Q\n";
        fz_buffer* contents =
            fz_new_buffer_from_copied_data(ctx, (const unsigned char*)emptyContent, std::strlen(emptyContent));
        pdf_obj* resources = pdf_new_dict(ctx, doc, 0);
        pdf_obj* page = pdf_add_page(ctx, doc, fz_unit_rect, 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, page);
        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, resources);
        fz_drop_buffer(ctx, contents);

        pdf_write_options opts = pdf_default_write_options;
        opts.do_encrypt = PDF_ENCRYPT_AES_128;
        std::strncpy(opts.upwd_utf8, password.toLocal8Bit().constData(), sizeof(opts.upwd_utf8) - 1);
        std::strncpy(opts.opwd_utf8, password.toLocal8Bit().constData(), sizeof(opts.opwd_utf8) - 1);

        pdf_save_document(ctx, doc, QFile::encodeName(path).constData(), &opts);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx)
    {
        qWarning() << "createEncryptedPDF failed:" << fz_caught_message(ctx);
    }
}
