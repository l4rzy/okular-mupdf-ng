// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/pdf/document.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "engine/constants.hpp"
#include "shared/model/types.hpp"
#include "shared/model/validation.hpp"
#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

namespace {

// Resolve the logical field dictionary for a widget. Widget dictionaries and
// unnamed intermediate nodes inherit the nearest named field's value; do not
// climb past that boundary into an unrelated container field.
pdf_obj* resolveFieldHead(fz_context* context, pdf_obj* field)
{
    // Validate the entire parent chain: field-name loading also visits parents
    // above the nearest named field. Fixed storage is safe across fz_throw.
    std::array<pdf_obj*, 128> visited { };
    std::size_t count = 0;
    pdf_obj* head = nullptr;
    while (field) {
        pdf_obj* resolved = pdf_resolve_indirect(context, field);
        if (!pdf_is_dict(context, resolved))
            fz_throw(context, FZ_ERROR_FORMAT, "form field parent is not a dictionary");
        if (std::find(visited.begin(), visited.begin() + count, resolved) != visited.begin() + count)
            fz_throw(context, FZ_ERROR_FORMAT, "cyclic form field parents");
        if (count == visited.size())
            fz_throw(context, FZ_ERROR_LIMIT, "resource limit: form field parent depth exceeded");
        visited[count++] = resolved;
        if (!head && pdf_dict_get(context, resolved, PDF_NAME(T)))
            head = field;
        pdf_obj* parent = pdf_dict_get(context, resolved, PDF_NAME(Parent));
        if (!parent)
            return head ? head : field;
        field = parent;
    }
    return head;
}

FormValue formValue(const FormField& field)
{
    // Convert the worker's normalized field representation back to the variant
    // expected by mutation responses. Push buttons never carry editable values.
    switch (field.type) {
    case FormFieldType::Text:
        return FormTextValue { field.text };
    case FormFieldType::CheckBox:
    case FormFieldType::RadioButton:
        return FormCheckValue { field.checked };
    case FormFieldType::ComboBox:
    case FormFieldType::ListBox:
        if (!field.currentChoices.empty())
            return FormChoiceSelection { field.currentChoices };
        if (field.type == FormFieldType::ListBox || !field.editableCombo)
            return FormChoiceSelection { };
        return FormChoiceCustomText { field.text };
    case FormFieldType::PushButton:
        break;
    }
    return FormTextValue { };
}

pdf_obj* resetAction(fz_context* context, pdf_obj* field)
{
    // PDF permits the ResetForm action directly in /A or under the user-action
    // entry /AA/U. Prefer /A, matching the normal action lookup precedence.
    pdf_obj* action = pdf_dict_get(context, field, PDF_NAME(A));
    if (!action) {
        pdf_obj* additionalActions = pdf_dict_get(context, field, PDF_NAME(AA));
        action = additionalActions ? pdf_dict_get(context, additionalActions, PDF_NAME(U)) : nullptr;
    }
    if (!action || !pdf_is_dict(context, action)
        || !pdf_name_eq(context, pdf_dict_get(context, action, PDF_NAME(S)), PDF_NAME(ResetForm))) {
        return nullptr;
    }
    return action;
}

bool actionContainsJavaScript(
    fz_context* context, pdf_obj* action, std::array<pdf_obj*, 256>& visited, std::size_t& count, std::size_t depth)
{
    constexpr std::size_t MaxActionDepth = 32;
    if (!action || depth >= MaxActionDepth || count >= visited.size())
        return false;

    action = pdf_resolve_indirect(context, action);
    const auto end = visited.begin() + count;
    if (!action || std::find(visited.begin(), end, action) != end)
        return false;
    visited[count++] = action;

    if (pdf_is_array(context, action)) {
        const int length = pdf_array_len(context, action);
        for (int i = 0; i < length && count < visited.size(); ++i) {
            if (actionContainsJavaScript(context, pdf_array_get(context, action, i), visited, count, depth + 1))
                return true;
        }
        return false;
    }

    if (!pdf_is_dict(context, action))
        return false;

    if (pdf_name_eq(context, pdf_dict_get(context, action, PDF_NAME(S)), PDF_NAME(JavaScript))
        && pdf_dict_get(context, action, PDF_NAME(JS))) {
        return true;
    }
    return actionContainsJavaScript(context, pdf_dict_get(context, action, PDF_NAME(Next)), visited, count, depth + 1);
}

bool hasJavaScriptClickAction(fz_context* context, pdf_obj* field)
{
    pdf_obj* additionalActions = pdf_dict_get(context, field, PDF_NAME(AA));
    // The caller catches MuPDF errors. Fixed POD storage needs no destructor
    // when a malformed action jumps out of this helper.
    std::array<pdf_obj*, 256> visited { };
    std::size_t count = 0;
    if (additionalActions
        && actionContainsJavaScript(
            context, pdf_dict_get(context, additionalActions, PDF_NAME(D)), visited, count, 0)) {
        return true;
    }

    pdf_obj* action = pdf_dict_get(context, field, PDF_NAME(A));
    if (!action && additionalActions)
        action = pdf_dict_get(context, additionalActions, PDF_NAME(U));
    return actionContainsJavaScript(context, action, visited, count, 0);
}

// Charge each owning copy, even when several widgets reference the same PDF string.
std::string copyFormText(fz_context* context, const char* text, std::size_t maxLength, ByteBudget& budget)
{
    if (!text)
        return { };
    const std::size_t length = std::strlen(text);
    if (length > maxLength)
        fz_throw(context, FZ_ERROR_LIMIT, "resource limit: form string exceeds limit");
    if (!budget.charge(length))
        fz_throw(context, FZ_ERROR_LIMIT, "resource limit: document form text size exceeded");
    return std::string(text, length);
}

// Empty choices still allocate string objects. Charge their storage before
// reserving exactly the bounded option count, avoiding vector growth overhead.
void reserveFormChoices(fz_context* context, std::vector<std::string>& choices, int count, ByteBudget& budget)
{
    const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(std::string);
    if (!budget.charge(bytes))
        fz_throw(context, FZ_ERROR_LIMIT, "resource limit: document form text size exceeded");
    choices.reserve(static_cast<std::size_t>(count));
}

std::string copyFieldName(fz_context* context, pdf_obj* field, ByteBudget& budget)
{
    char* name = pdf_load_field_name(context, field);
    if (!name)
        return { };
    const std::size_t length = std::strlen(name);
    if (length > Limit::MaxFormNameBytes || !budget.charge(length)) {
        fz_free(context, name);
        fz_throw(context, FZ_ERROR_LIMIT, "resource limit: form name or document form text size exceeds limit");
    }
    // All MuPDF calls that can throw are complete before establishing ownership.
    const auto freeName = [context](char* value) {
        fz_free(context, value);
    };
    const std::unique_ptr<char, decltype(freeName)> ownedName(name, freeName);
    return std::string(name, length);
}

void appendChoiceValue(fz_context* context, pdf_obj* value, std::vector<const char*>& selected)
{
    if (!value)
        return;
    // /V is a string for single-select fields and an array for multi-select
    // fields. Nested arrays are invalid; never recursively follow PDF values.
    const bool array = pdf_is_array(context, value);
    const int count = array ? pdf_array_len(context, value) : 1;
    if (count > static_cast<int>(Limit::MaxFormSelectedIndices))
        fz_throw(context, FZ_ERROR_LIMIT, "resource limit: form selected choices count exceeded");
    for (int index = 0; index < count; ++index) {
        pdf_obj* item = array ? pdf_array_get(context, value, index) : value;
        if (!pdf_is_string(context, item))
            fz_throw(context, FZ_ERROR_FORMAT, "form choice value is not a string");
        const char* text = pdf_to_text_string(context, item);
        const std::size_t length = std::strlen(text);
        if (length > Limit::MaxFormFieldStringBytes)
            fz_throw(context, FZ_ERROR_LIMIT, "resource limit: form choice value exceeds limit");
        selected.push_back(text);
    }
}

const char* choiceOption(fz_context* context, pdf_obj* field, int index, bool useExportValues)
{
    return pdf_choice_field_option(context, field, useExportValues ? 1 : 0, index);
}

void updateAllPages(fz_context* context, fz_document* document, int pageCount)
{
    // Field values can be shared by widgets on different pages. Rebuild every
    // page's appearance stream before extracting mutation results.
    for (int pageNumber = 0; pageNumber < pageCount; ++pageNumber) {
        fz_page* volatile page = nullptr;
        fz_try(context)
        {
            page = fz_load_page(context, document, pageNumber);
            pdf_update_page(context, pdf_page_from_fz_page(context, page));
        }
        fz_always(context)
        {
            fz_drop_page(context, page);
        }
        fz_catch(context)
        {
            fz_warn(
                context, "could not update form appearances on page %d: %s", pageNumber, fz_caught_message(context));
        }
    }
}

} // namespace

bool PdfDocument::collectFormMutations(std::vector<FieldMutation>* mutations, std::string* error)
{
    if (!mutations)
        return true;

    // Shared fields and JavaScript can affect widgets on any page. Collect
    // only form fields; annotations, signatures, and links are unrelated.
    mutations->clear();
    ExtractionBudgets budgets;
    for (int currentPage = 0; currentPage < pageCount(); ++currentPage) {
        std::string fieldsError;
        std::vector<FormField> fields;
        ByteBudget pageBudget = budgets.forms;
        fz_page* nativePage = loadPage(currentPage, &fieldsError);
        if (nativePage) {
            fz_try(m_context)
            {
                const fz_rect bounds = fz_bound_page(m_context, nativePage);
                fields = extractPageFormFields(nativePage, bounds, currentPage, pageBudget, &fieldsError);
            }
            fz_always(m_context)
            {
                fz_drop_page(m_context, nativePage);
            }
            fz_catch(m_context)
            {
                fieldsError = fz_convert_error(m_context, nullptr);
            }
        }
        if (!fieldsError.empty()) {
            if (fieldsError.starts_with("resource limit:")) {
                mutations->clear();
                return fail(error, fieldsError.c_str());
            }
            fz_warn(m_context, "could not collect form fields on page %d: %s", currentPage, fieldsError.c_str());
            continue;
        }
        budgets.forms = pageBudget;
        for (const auto& field : fields) {
            if (field.type != FormFieldType::PushButton)
                mutations->push_back({ currentPage, field.pdfObjectNumber, formValue(field) });
        }
    }
    return true;
}

std::vector<FormField> PdfDocument::extractPageFormFields(
    fz_page* nativePage, const fz_rect& bounds, int page, ByteBudget& budget, std::string* error) const
{
    if (!m_hasAcroForm)
        return { };

    pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
    if (!pdfPage)
        return { };

    pdf_annot* firstWidget = pdf_first_widget(m_context, pdfPage);
    if (!firstWidget)
        return { };

    pdf_document* pdfDocument = pdf_specifics(m_context, m_document);
    if (!pdfDocument)
        return { };

    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    if (width <= 0 || height <= 0)
        return { };

    std::vector<FormField> result;
    FormField formField;
    std::vector<const char*> selectedValues;

    const auto safeText = [this, &budget](pdf_obj* object, std::size_t maxLen) -> std::string {
        return copyFormText(m_context, pdf_to_text_string(m_context, object), maxLen, budget);
    };

    // Keep the widget object number and inherited logical field number separate:
    // updates target the widget, while shared values and reset operations use the
    // logical field at the head of the PDF field hierarchy.
    fz_try(m_context)
    {
        for (pdf_annot* widget = firstWidget; widget; widget = pdf_next_widget(m_context, widget)) {
            const auto widgetType = pdf_widget_type(m_context, widget);
            // Skip signature widgets (handled separately by extractPageSignatures)
            // and unknown widget types.
            if (widgetType != PDF_WIDGET_TYPE_TEXT && widgetType != PDF_WIDGET_TYPE_CHECKBOX
                && widgetType != PDF_WIDGET_TYPE_RADIOBUTTON && widgetType != PDF_WIDGET_TYPE_COMBOBOX
                && widgetType != PDF_WIDGET_TYPE_LISTBOX && widgetType != PDF_WIDGET_TYPE_BUTTON)
                continue;
            if (result.size() >= Limit::MaxPageFormFields)
                fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: page form field limit exceeded");

            pdf_obj* field = pdf_annot_obj(m_context, widget);
            if (!field)
                continue;

            const int fieldFlags = pdf_field_flags(m_context, field);
            const bool pushButton = (fieldFlags & PDF_BTN_FIELD_IS_PUSHBUTTON) != 0;
            if (widgetType == PDF_WIDGET_TYPE_BUTTON && !pushButton)
                continue;

            formField = { };
            formField.pdfObjectNumber = pdf_to_num(m_context, field);
            formField.page = page;

            pdf_obj* fieldHead = resolveFieldHead(m_context, field);
            formField.fieldObjectNumber = pdf_to_num(m_context, fieldHead);
            formField.groupName = copyFieldName(m_context, fieldHead, budget);

            formField.partialName =
                safeText(pdf_dict_get_inheritable(m_context, field, PDF_NAME(T)), Limit::MaxFormNameBytes);
            formField.uiName =
                safeText(pdf_dict_get_inheritable(m_context, field, PDF_NAME(TU)), Limit::MaxFormFieldStringBytes);

            formField.fullyQualifiedName = copyFieldName(m_context, field, budget);
            if (formField.groupName.empty())
                formField.groupName =
                    copyFormText(m_context, formField.fullyQualifiedName.c_str(), Limit::MaxFormNameBytes, budget);

            const int annotationFlags = pdf_dict_get_int(m_context, field, PDF_NAME(F));

            formField.readOnly =
                (fieldFlags & PDF_FIELD_IS_READ_ONLY) != 0 || (annotationFlags & PDF_ANNOT_IS_READ_ONLY) != 0;
            formField.visible =
                (annotationFlags & (PDF_ANNOT_IS_INVISIBLE | PDF_ANNOT_IS_HIDDEN | PDF_ANNOT_IS_NO_VIEW)) == 0;
            formField.printable = (annotationFlags & PDF_ANNOT_IS_PRINT) != 0;

            const fz_rect rect = pdf_bound_widget(m_context, widget);
            const double x0 = std::clamp(static_cast<double>((rect.x0 - bounds.x0) / width), 0.0, 1.0);
            const double y0 = std::clamp(static_cast<double>((rect.y0 - bounds.y0) / height), 0.0, 1.0);
            const double x1 = std::clamp(static_cast<double>((rect.x1 - bounds.x0) / width), 0.0, 1.0);
            const double y1 = std::clamp(static_cast<double>((rect.y1 - bounds.y0) / height), 0.0, 1.0);
            // The worker protocol exposes normalized top-left coordinates. Clamp
            // malformed widget rectangles before normalizing their orientation.
            formField.rectangle.left = std::min(x0, x1);
            formField.rectangle.top = std::min(y0, y1);
            formField.rectangle.right = std::max(x0, x1);
            formField.rectangle.bottom = std::max(y0, y1);

            if (pushButton) {
                formField.type = FormFieldType::PushButton;
                pdf_obj* appearance = pdf_dict_get(m_context, field, PDF_NAME(MK));
                formField.buttonCaption = safeText(
                    appearance ? pdf_dict_get(m_context, appearance, PDF_NAME(CA)) : nullptr, Limit::MaxFormNameBytes);
                if (resetAction(m_context, field))
                    formField.pushButtonAction = FormPushButtonAction::Reset;
                else if (pdf_js_supported(m_context, pdfDocument) && hasJavaScriptClickAction(m_context, field))
                    formField.pushButtonAction = FormPushButtonAction::JavaScript;
            } else if (widgetType == PDF_WIDGET_TYPE_TEXT) {
                formField.type = FormFieldType::Text;
                if (const char* val = pdf_field_value(m_context, field)) {
                    formField.text = copyFormText(m_context, val, Limit::MaxFormFieldStringBytes, budget);
                }
                formField.maximumLength = pdf_text_widget_max_len(m_context, widget);
                formField.multiline = (fieldFlags & PDF_TX_FIELD_IS_MULTILINE) != 0;
                formField.password = (fieldFlags & PDF_TX_FIELD_IS_PASSWORD) != 0;
            } else if (widgetType == PDF_WIDGET_TYPE_CHECKBOX) {
                formField.type = FormFieldType::CheckBox;
                // MuPDF reports a non-Off appearance state as checked; preserve
                // the on-state name so the plugin can identify the active value.
                if (pdf_obj* onStateObj = pdf_button_field_on_state(m_context, field)) {
                    if (const char* onState = pdf_to_name(m_context, onStateObj))
                        formField.onState = copyFormText(m_context, onState, Limit::MaxFormFieldStringBytes, budget);
                }
                pdf_obj* valObj = pdf_dict_get(m_context, field, PDF_NAME(AS));
                if (valObj) {
                    const char* stateName = pdf_to_name(m_context, valObj);
                    if (stateName && std::strcmp(stateName, "Off") != 0) {
                        formField.checked = true;
                    }
                }
            } else if (widgetType == PDF_WIDGET_TYPE_RADIOBUTTON) {
                formField.type = FormFieldType::RadioButton;
                // Radio widgets are checked only when their appearance matches
                // the widget's declared on-state; Off means unselected.
                if (pdf_obj* onStateObj = pdf_button_field_on_state(m_context, field)) {
                    if (const char* onState = pdf_to_name(m_context, onStateObj))
                        formField.onState = copyFormText(m_context, onState, Limit::MaxFormFieldStringBytes, budget);
                }
                pdf_obj* asObj = pdf_dict_get(m_context, field, PDF_NAME(AS));
                const char* asState = asObj ? pdf_to_name(m_context, asObj) : nullptr;
                if (asState && std::strcmp(asState, "Off") != 0
                    && (formField.onState.empty() || formField.onState == asState)) {
                    formField.checked = true;
                }
                formField.noToggleToOff =
                    (pdf_field_flags(m_context, fieldHead) & PDF_BTN_FIELD_IS_NO_TOGGLE_TO_OFF) != 0;
            } else if (widgetType == PDF_WIDGET_TYPE_COMBOBOX || widgetType == PDF_WIDGET_TYPE_LISTBOX) {
                formField.type =
                    (widgetType == PDF_WIDGET_TYPE_COMBOBOX) ? FormFieldType::ComboBox : FormFieldType::ListBox;
                // Keep display choices and export values separately. The PDF may
                // store either representation in /V, so selection recovery below
                // accepts both and prefers explicit /I indices when present.
                formField.editableCombo = (fieldFlags & PDF_CH_FIELD_IS_EDIT) != 0;
                formField.multiSelect = pdf_choice_widget_is_multiselect(m_context, widget) != 0;

                const int numOpts = pdf_choice_widget_options(m_context, widget, 0, nullptr);
                if (numOpts > 0) {
                    if (numOpts > static_cast<int>(Limit::MaxFormChoices))
                        fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: form choices count exceeded");
                    // Read and charge one option at a time; the bulk API would
                    // materialize every PDF string before the budget can reject it.
                    reserveFormChoices(m_context, formField.choices, numOpts, budget);
                    for (int index = 0; index < numOpts; ++index) {
                        const char* option = choiceOption(m_context, field, index, false);
                        formField.choices.push_back(
                            copyFormText(m_context, option, Limit::MaxFormFieldStringBytes, budget));
                    }
                }

                const int numExports = pdf_choice_widget_options(m_context, widget, 1, nullptr);
                if (numExports > 0) {
                    if (numExports != numOpts)
                        fz_throw(m_context, FZ_ERROR_FORMAT, "form export values count does not match choices count");
                    bool hasDistinctExports = false;
                    for (int index = 0; index < numExports; ++index) {
                        const char* value = choiceOption(m_context, field, index, true);
                        if (formField.choices[static_cast<std::size_t>(index)] != value) {
                            hasDistinctExports = true;
                            break;
                        }
                    }
                    // Identical exports are omitted from the model. Avoid copying
                    // them into a second array only to discard it afterwards.
                    if (hasDistinctExports) {
                        reserveFormChoices(m_context, formField.exportValues, numExports, budget);
                        for (int index = 0; index < numExports; ++index) {
                            const char* value = choiceOption(m_context, field, index, true);
                            formField.exportValues.push_back(
                                copyFormText(m_context, value, Limit::MaxFormFieldStringBytes, budget));
                        }
                    }
                }

                pdf_obj* indices = pdf_dict_get_inheritable(m_context, fieldHead, PDF_NAME(I));
                if (indices) {
                    if (!pdf_is_array(m_context, indices))
                        fz_throw(m_context, FZ_ERROR_FORMAT, "form choice indices are not an array");
                    const int numIndices = pdf_array_len(m_context, indices);
                    if (numIndices > static_cast<int>(Limit::MaxFormSelectedIndices))
                        fz_throw(m_context, FZ_ERROR_LIMIT, "resource limit: form selected choices count exceeded");
                    for (int i = 0; i < numIndices; ++i) {
                        const int index = pdf_array_get_int(m_context, indices, i);
                        if (index < 0 || index >= static_cast<int>(formField.choices.size()))
                            fz_throw(m_context, FZ_ERROR_FORMAT, "form choice index is out of range");
                        formField.currentChoices.push_back(index);
                    }
                } else {
                    selectedValues.clear();
                    appendChoiceValue(
                        m_context, pdf_dict_get_inheritable(m_context, fieldHead, PDF_NAME(V)), selectedValues);
                    for (const auto& selected : selectedValues) {
                        for (std::size_t i = 0; i < formField.choices.size(); ++i) {
                            if (formField.choices[i] == selected
                                || (i < formField.exportValues.size() && formField.exportValues[i] == selected)) {
                                formField.currentChoices.push_back(static_cast<int>(i));
                                break;
                            }
                        }
                    }
                }

                if (const char* val = pdf_field_value(m_context, field)) {
                    formField.text = copyFormText(m_context, val, Limit::MaxFormFieldStringBytes, budget);
                }
            }

            result.push_back(std::move(formField));
        }
    }
    fz_catch(m_context)
    {
        // Convert the caught error before pageDetails wraps it in another
        // MuPDF exception, so the original is marked handled.
        fail(error, fz_convert_error(m_context, nullptr));
        return { };
    }

    return result;
}

bool PdfDocument::updateFormField(int page,
                                  std::int32_t objectNumber,
                                  const Model::FormValue& value,
                                  std::vector<FieldMutation>* mutations,
                                  std::string* error)
{
    if (!m_document || m_locked)
        return fail(error, "document is not open");

    pdf_document* pdfDocument = pdf_specifics(m_context, m_document);
    if (!pdfDocument)
        return fail(error, "document is not a PDF");

    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return false;

    pdf_obj* volatile newValue = nullptr;
    volatile int effectsPossible = 0;
    std::string operationError;

    // Locate the widget on the requested page first, then resolve its logical
    // field head. This prevents a valid object number on another page from being
    // edited through the wrong widget.
    fz_try(m_context)
    {
        pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
        if (!pdfPage)
            fz_throw(m_context, FZ_ERROR_GENERIC, "not a PDF page");

        pdf_annot* targetWidget = nullptr;
        for (pdf_annot* widget = pdf_first_widget(m_context, pdfPage); widget;
             widget = pdf_next_widget(m_context, widget)) {
            pdf_obj* field = pdf_annot_obj(m_context, widget);
            if (field && pdf_to_num(m_context, field) == objectNumber) {
                targetWidget = widget;
                break;
            }
        }

        if (!targetWidget)
            fz_throw(m_context, FZ_ERROR_GENERIC, "widget object not found on page");

        pdf_obj* field = pdf_annot_obj(m_context, targetWidget);
        const int annotationFlags = pdf_dict_get_int(m_context, field, PDF_NAME(F));
        const int fieldFlags = pdf_field_flags(m_context, field);
        const bool readOnly =
            (fieldFlags & PDF_FIELD_IS_READ_ONLY) != 0 || (annotationFlags & PDF_ANNOT_IS_READ_ONLY) != 0;
        if (readOnly)
            fz_throw(m_context, FZ_ERROR_GENERIC, "form field is read-only");

        const auto widgetType = pdf_widget_type(m_context, targetWidget);

        pdf_obj* logicalField = resolveFieldHead(m_context, field);

        if (std::holds_alternative<FormTextValue>(value)) {
            // Text and editable combo values are written through MuPDF so it can
            // apply field inheritance and regenerate the widget appearance.
            if (widgetType != PDF_WIDGET_TYPE_TEXT && widgetType != PDF_WIDGET_TYPE_COMBOBOX)
                fz_throw(m_context, FZ_ERROR_GENERIC, "field is not a text or combo widget");

            const auto& textVal = std::get<FormTextValue>(value);
            const int maxLen = pdf_text_widget_max_len(m_context, targetWidget);
            if (maxLen > 0 && utf8CodepointCount(textVal.text) > static_cast<std::size_t>(maxLen))
                fz_throw(m_context, FZ_ERROR_GENERIC, "text value exceeds field maximum length");

            effectsPossible = 1;
            if (!pdf_set_field_value(m_context, pdfDocument, logicalField, textVal.text.c_str(), 0))
                fz_throw(m_context, FZ_ERROR_GENERIC, "form value rejected");
        } else if (std::holds_alternative<FormCheckValue>(value)) {
            // Checkbox and radio values are represented by appearance-state
            // toggles rather than by a boolean stored directly in the model.
            const auto& checkVal = std::get<FormCheckValue>(value);
            if (widgetType != PDF_WIDGET_TYPE_CHECKBOX && widgetType != PDF_WIDGET_TYPE_RADIOBUTTON) {
                fz_throw(m_context, FZ_ERROR_GENERIC, "field is not a checkbox or radio widget");
            }
            pdf_obj* appearance = pdf_dict_get(m_context, field, PDF_NAME(AS));
            const char* appearanceName = appearance ? pdf_to_name(m_context, appearance) : nullptr;
            const bool checked = appearanceName && std::strcmp(appearanceName, "Off") != 0;
            if (checked != checkVal.checked) {
                if (!checkVal.checked && widgetType == PDF_WIDGET_TYPE_RADIOBUTTON
                    && (pdf_field_flags(m_context, logicalField) & PDF_BTN_FIELD_IS_NO_TOGGLE_TO_OFF) != 0)
                    fz_throw(m_context, FZ_ERROR_GENERIC, "radio button cannot be unchecked directly");

                const bool javaScriptEnabled = pdf_js_supported(m_context, pdfDocument) != 0;
                effectsPossible = 1;
                if (javaScriptEnabled) {
                    fz_try(m_context)
                    {
                        pdf_annot_event_down(m_context, targetWidget);
                    }
                    fz_catch(m_context)
                    {
                        fz_warn(m_context, "checkbox or radio down action failed: %s", fz_caught_message(m_context));
                    }
                }

                // MuPDF's toggle helper writes checkbox /V as a PDF name and
                // synchronizes radio siblings. pdf_set_field_value currently
                // stores button values as strings, which breaks radio state.
                if (javaScriptEnabled) {
                    fz_try(m_context)
                    {
                        if (!pdf_toggle_widget(m_context, targetWidget))
                            fz_throw(m_context, FZ_ERROR_GENERIC, "could not update button widget");
                    }
                    fz_catch(m_context)
                    {
                        fz_warn(m_context, "checkbox or radio toggle failed: %s", fz_caught_message(m_context));
                    }
                } else if (!pdf_toggle_widget(m_context, targetWidget)) {
                    fz_throw(m_context, FZ_ERROR_GENERIC, "could not update button widget");
                }

                if (javaScriptEnabled) {
                    fz_try(m_context)
                    {
                        pdf_annot_event_up(m_context, targetWidget);
                    }
                    fz_catch(m_context)
                    {
                        fz_warn(m_context, "checkbox or radio up action failed: %s", fz_caught_message(m_context));
                    }
                }
            }
        } else if (std::holds_alternative<FormChoiceSelection>(value)) {
            // Store selected display/export values in /V and clear /I. MuPDF can
            // then rebuild dependent widget appearances consistently.
            if (widgetType != PDF_WIDGET_TYPE_COMBOBOX && widgetType != PDF_WIDGET_TYPE_LISTBOX)
                fz_throw(m_context, FZ_ERROR_GENERIC, "field is not a choice widget");

            const auto& selectionVal = std::get<FormChoiceSelection>(value);
            const int numOpts = pdf_choice_widget_options(m_context, targetWidget, 0, nullptr);
            const bool isMulti = pdf_choice_widget_is_multiselect(m_context, targetWidget) != 0;

            if (!isMulti && selectionVal.selectedIndices.size() > 1)
                fz_throw(m_context, FZ_ERROR_GENERIC, "multiple selection is not allowed on this field");

            for (int idx : selectionVal.selectedIndices) {
                if (idx < 0 || idx >= numOpts)
                    fz_throw(m_context, FZ_ERROR_GENERIC, "choice index out of range");
            }

            const bool useExportValues = pdf_choice_widget_options(m_context, targetWidget, 1, nullptr) == numOpts;
            effectsPossible = 1;
            if (selectionVal.selectedIndices.empty()) {
                pdf_dict_del(m_context, logicalField, PDF_NAME(V));
                pdf_dict_del(m_context, logicalField, PDF_NAME(I));
            } else {
                newValue = pdf_new_array(m_context, pdfDocument, static_cast<int>(selectionVal.selectedIndices.size()));
                for (int idx : selectionVal.selectedIndices)
                    pdf_array_push_text_string(
                        m_context, newValue, choiceOption(m_context, logicalField, idx, useExportValues));
                if (selectionVal.selectedIndices.size() == 1) {
                    pdf_obj* item = pdf_array_get(m_context, newValue, 0);
                    pdf_dict_put(m_context, logicalField, PDF_NAME(V), item);
                    pdf_drop_obj(m_context, newValue);
                    newValue = nullptr;
                } else {
                    pdf_dict_put_drop(m_context, logicalField, PDF_NAME(V), newValue);
                    newValue = nullptr;
                }
            }
            pdf_dict_del(m_context, logicalField, PDF_NAME(I));
            pdf_dirty_obj(m_context, logicalField);
            pdf_dirty_annot(m_context, targetWidget);
        } else if (std::holds_alternative<FormChoiceCustomText>(value)) {
            // Custom text is valid only for an editable combo box; list boxes and
            // fixed-choice combos must use indexed selections.
            if (widgetType != PDF_WIDGET_TYPE_COMBOBOX)
                fz_throw(m_context, FZ_ERROR_GENERIC, "field is not a combobox widget");
            if ((fieldFlags & PDF_CH_FIELD_IS_EDIT) == 0)
                fz_throw(m_context, FZ_ERROR_GENERIC, "combobox is not editable");

            const auto& customTextVal = std::get<FormChoiceCustomText>(value);
            effectsPossible = 1;
            if (!pdf_set_field_value(m_context, pdfDocument, logicalField, customTextVal.text.c_str(), 0))
                fz_throw(m_context, FZ_ERROR_GENERIC, "form value rejected");
        }
    }
    fz_always(m_context)
    {
        pdf_drop_obj(m_context, newValue);
        fz_drop_page(m_context, nativePage);
    }
    fz_catch(m_context)
    {
        operationError = fz_caught_message(m_context);
    }

    if (!operationError.empty() && !effectsPossible) {
        clearPageCache();
        return fail(error, operationError.c_str());
    }
    if (!operationError.empty())
        fz_warn(m_context, "form update failed after effects may have been applied: %s", operationError.c_str());

    // Refresh and collect even after a late MuPDF error so callers learn about
    // any field changes that the action committed before it failed.
    updateAllPages(m_context, m_document, pageCount());
    clearPageCache();
    return collectFormMutations(mutations, error);
}

bool PdfDocument::resetForm(int page,
                            std::int32_t objectNumber,
                            std::vector<FieldMutation>* mutations,
                            std::string* error)
{
    if (!m_document || m_locked)
        return fail(error, "document is not open");

    pdf_document* pdfDocument = pdf_specifics(m_context, m_document);
    if (!pdfDocument)
        return fail(error, "document is not a PDF");

    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return false;

    // The request identifies a reset push-button widget. Its action determines
    // the field subset and flags that MuPDF resets, rather than the button's own
    // visible value.
    fz_try(m_context)
    {
        pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
        if (!pdfPage)
            fz_throw(m_context, FZ_ERROR_GENERIC, "not a PDF page");

        pdf_annot* targetWidget = nullptr;
        for (pdf_annot* widget = pdf_first_widget(m_context, pdfPage); widget;
             widget = pdf_next_widget(m_context, widget)) {
            pdf_obj* field = pdf_annot_obj(m_context, widget);
            if (field && pdf_to_num(m_context, field) == objectNumber) {
                targetWidget = widget;
                break;
            }
        }
        if (!targetWidget)
            fz_throw(m_context, FZ_ERROR_GENERIC, "widget object not found on page");

        pdf_obj* field = pdf_annot_obj(m_context, targetWidget);
        const int annotationFlags = pdf_dict_get_int(m_context, field, PDF_NAME(F));
        if ((pdf_field_flags(m_context, field) & PDF_FIELD_IS_READ_ONLY) != 0
            || (annotationFlags & PDF_ANNOT_IS_READ_ONLY) != 0) {
            fz_throw(m_context, FZ_ERROR_GENERIC, "form field is read-only");
        }
        if ((pdf_field_flags(m_context, field) & PDF_BTN_FIELD_IS_PUSHBUTTON) == 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "field is not a push button");

        pdf_obj* action = resetAction(m_context, field);
        if (!action)
            fz_throw(m_context, FZ_ERROR_GENERIC, "push button does not reset the form");

        // Bit 0 of ResetForm /Flags requests exclusion: reset every field except
        // the listed /Fields entries. MuPDF applies the result to all widgets.
        pdf_reset_form(m_context,
                       pdfDocument,
                       pdf_dict_get(m_context, action, PDF_NAME(Fields)),
                       pdf_dict_get_int(m_context, action, PDF_NAME(Flags)) & 1);
        updateAllPages(m_context, m_document, pageCount());
    }
    fz_always(m_context)
    {
        fz_drop_page(m_context, nativePage);
        // updateAllPages refreshed widgets through separate page handles, so the
        // page cache can no longer be trusted.
        clearPageCache();
    }
    fz_catch(m_context)
    {
        fail(error, fz_caught_message(m_context));
        return false;
    }

    return collectFormMutations(mutations, error);
}

bool PdfDocument::clickFormButton(int page,
                                  std::int32_t objectNumber,
                                  std::vector<FieldMutation>* mutations,
                                  std::string* error)
{
    if (!m_document || m_locked)
        return fail(error, "document is not open");

    pdf_document* pdfDocument = pdf_specifics(m_context, m_document);
    if (!pdfDocument)
        return fail(error, "document is not a PDF");
    if (!pdf_js_supported(m_context, pdfDocument))
        return fail(error, "PDF form JavaScript is unavailable");

    fz_page* nativePage = loadPage(page, error);
    if (!nativePage)
        return false;

    volatile int effectsPossible = 0;
    std::string operationError;
    fz_try(m_context)
    {
        pdf_page* pdfPage = pdf_page_from_fz_page(m_context, nativePage);
        if (!pdfPage)
            fz_throw(m_context, FZ_ERROR_GENERIC, "not a PDF page");

        pdf_annot* targetWidget = nullptr;
        for (pdf_annot* widget = pdf_first_widget(m_context, pdfPage); widget;
             widget = pdf_next_widget(m_context, widget)) {
            pdf_obj* field = pdf_annot_obj(m_context, widget);
            if (field && pdf_to_num(m_context, field) == objectNumber) {
                targetWidget = widget;
                break;
            }
        }
        if (!targetWidget)
            fz_throw(m_context, FZ_ERROR_GENERIC, "widget object not found on page");

        pdf_obj* field = pdf_annot_obj(m_context, targetWidget);
        const int fieldFlags = pdf_field_flags(m_context, field);
        const int annotationFlags = pdf_dict_get_int(m_context, field, PDF_NAME(F));
        if ((fieldFlags & PDF_BTN_FIELD_IS_PUSHBUTTON) == 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "field is not a push button");
        if ((fieldFlags & PDF_FIELD_IS_READ_ONLY) != 0 || (annotationFlags & PDF_ANNOT_IS_READ_ONLY) != 0)
            fz_throw(m_context, FZ_ERROR_GENERIC, "form field is read-only");
        if (resetAction(m_context, field))
            fz_throw(m_context, FZ_ERROR_GENERIC, "reset buttons use the form reset operation");
        if (!hasJavaScriptClickAction(m_context, field))
            fz_throw(m_context, FZ_ERROR_GENERIC, "push button has no JavaScript click action");

        effectsPossible = 1;
        fz_try(m_context)
        {
            pdf_annot_event_down(m_context, targetWidget);
        }
        fz_catch(m_context)
        {
            fz_warn(m_context, "push button down action failed: %s", fz_caught_message(m_context));
        }
        fz_try(m_context)
        {
            pdf_annot_event_up(m_context, targetWidget);
        }
        fz_catch(m_context)
        {
            fz_warn(m_context, "push button up action failed: %s", fz_caught_message(m_context));
        }
    }
    fz_always(m_context)
    {
        fz_drop_page(m_context, nativePage);
    }
    fz_catch(m_context)
    {
        operationError = fz_caught_message(m_context);
    }

    if (!operationError.empty() && !effectsPossible) {
        clearPageCache();
        return fail(error, operationError.c_str());
    }
    if (!operationError.empty())
        fz_warn(m_context, "push button action failed after effects may have been applied: %s", operationError.c_str());

    // Button scripts can change fields and appearances anywhere in the document.
    updateAllPages(m_context, m_document, pageCount());
    clearPageCache();
    return collectFormMutations(mutations, error);
}

} // namespace Mu::Worker::Engine
