// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QTest>
#include <QTimeZone>

#include <limits>

extern "C" {
#include <mupdf/pdf.h>
}

#include "generator/conversion/annotation.hpp"
#include "generator/conversion/document.hpp"
#include "generator/conversion/text.hpp"
#include "generator/proxy/embedded_file.hpp"
#include "generator/proxy/form/checkbox.hpp"
#include "generator/proxy/form/choice.hpp"
#include "generator/proxy/form/coordinator.hpp"
#include "generator/proxy/form/push_button.hpp"
#include "generator/proxy/form/radio_button.hpp"
#include "generator/proxy/form/radio_grouping.hpp"
#include "generator/proxy/form/signature.hpp"
#include "generator/proxy/form/text.hpp"
#include "plugin/worker_client.hpp"
#include "shared/model/types.hpp"

namespace Mu::Plugin {

static std::function<std::optional<Model::FormUpdateResponse>(const Model::FormUpdateRequest&)> s_mockUpdateForm;
static std::function<std::optional<Model::FormUpdateResponse>(const Model::FormResetRequest&)> s_mockResetForm;

static std::optional<Model::FormUpdateResponse> echoFormUpdate(const Model::FormUpdateRequest& request)
{
    Model::FormUpdateResponse response;
    response.affectedFields.push_back({ request.handle, request.value });
    return response;
}

std::optional<Model::FormUpdateResponse> WorkerClient::updateForm(const Model::FormUpdateRequest& req) const
{
    if (s_mockUpdateForm)
        return s_mockUpdateForm(req);
    return std::nullopt;
}

std::optional<Model::FormUpdateResponse> WorkerClient::resetForm(const Model::FormResetRequest& req) const
{
    if (s_mockResetForm)
        return s_mockResetForm(req);
    return std::nullopt;
}

} // namespace Mu::Plugin

class TestGeneratorProxies : public QObject {
    Q_OBJECT

private slots:

    void signatureFieldReconstructsBoundaryData()
    {
        ::Mu::Model::SignatureField source;
        source.objectNumber = 41;
        source.partialName = "Approval";
        source.fullyQualifiedName = "Form.Approval";
        source.readOnly = true;
        source.visible = false;
        source.left = .1;
        source.top = .2;
        source.right = .8;
        source.bottom = .9;
        source.signedField = true;
        source.subFilter = "adbe.pkcs7.detached";
        source.signerName = "Ada";
        source.signerSubjectDn = "CN=Ada";
        source.signingTime = { true, 123'000 };
        source.cmsSignature = { 0x30, 0x03, 0x02, 0x01, 0x01 };
        source.byteRange = { 0, 128, 512, 256 };
        source.certificate.subjectDistinguishedName = "CN=Ada";
        source.certificate.nickname = "Ada signing";
        source.signsTotalDocument = true;
        ::Mu::Generator::Proxy::Form::Signature proxy(
            source.objectNumber, source, static_cast<::Mu::Plugin::WorkerClient*>(nullptr));

        QCOMPARE(proxy.id(), source.objectNumber);
        QCOMPARE(proxy.name(), QStringLiteral("Approval"));
        QCOMPARE(proxy.fullyQualifiedName(), QStringLiteral("Form.Approval"));
        QVERIFY(proxy.isReadOnly());
        QVERIFY(!proxy.isVisible());
        QCOMPARE(proxy.rect().left, .1);
        QCOMPARE(proxy.rect().bottom, .9);
        QCOMPARE(proxy.signatureType(), Okular::FormFieldSignature::AdbePkcs7detached);
        QCOMPARE(proxy.signatureInfo().signerName(), QStringLiteral("Ada"));
        QVERIFY(proxy.signatureInfo().signsTotalDocument());
        QCOMPARE(proxy.signatureInfo().signerSubjectDN(), QStringLiteral("CN=Ada"));
        QCOMPARE(proxy.signatureInfo().signature(), QByteArray::fromHex("3003020101"));
        QCOMPARE(proxy.signatureInfo().signedRangeBounds(), QList<qint64>({ 0, 128, 512, 768 }));
        QCOMPARE(proxy.signatureInfo().signingTime(), QDateTime::fromSecsSinceEpoch(123, QTimeZone::UTC));
        QCOMPARE(proxy.signatureInfo().certificateInfo().subjectInfo(Okular::CertificateInfo::DistinguishedName,
                                                                     Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("CN=Ada"));
        QCOMPARE(proxy.signatureInfo().certificateInfo().nickName(), QStringLiteral("Ada signing"));
    }

    void signatureInfoRejectsOverflowingByteRanges()
    {
        ::Mu::Model::SignatureField source;
        source.byteRange = { std::numeric_limits<std::int64_t>::max(), 1, 0, 0 };
        ::Mu::Generator::Proxy::Form::Signature proxy(5, source);

        QVERIFY(proxy.signatureInfo().signedRangeBounds().isEmpty());
    }

    void embeddedFileWrapsMetadataAndPayload()
    {
        const QDateTime created = QDateTime::fromSecsSinceEpoch(1, QTimeZone::UTC);
        const QDateTime modified = QDateTime::fromSecsSinceEpoch(2, QTimeZone::UTC);
        ::Mu::Generator::Proxy::EmbeddedFile file(
            QStringLiteral("note.txt"), QStringLiteral("note"), 3, created, modified, QByteArray("abc"));
        QCOMPARE(file.name(), QStringLiteral("note.txt"));
        QCOMPARE(file.description(), QStringLiteral("note"));
        QCOMPARE(file.size(), 3);
        QCOMPARE(file.data(), QByteArray("abc"));
        QCOMPARE(file.creationDate(), created);
        QCOMPARE(file.modificationDate(), modified);
    }

    void documentSynopsisConstructsHierarchyAndAttributes()
    {
        // 1. Empty nodes should return nullptr
        QVERIFY(!::Mu::Generator::Conversion::documentSynopsis({ }));

        // 2. Structured hierarchy
        std::vector<::Mu::Model::OutlineNode> nodes;

        ::Mu::Model::OutlineNode chapter1;
        chapter1.title = "Chapter 1";
        chapter1.open = true;
        chapter1.link.valid = true;
        chapter1.link.external = false;
        chapter1.link.viewport = {
            0, 0.0, 0.0, ::Mu::Model::Viewport::CoordinateX | ::Mu::Model::Viewport::CoordinateY
        };

        ::Mu::Model::OutlineNode section1;
        section1.title = ""; // Test fallback title to "Item"
        section1.open = false;
        section1.link.valid = true;
        section1.link.external = true;
        section1.link.uri = "https://example.com/sec1";

        chapter1.children.push_back(std::move(section1));
        nodes.push_back(std::move(chapter1));

        const auto synopsis = ::Mu::Generator::Conversion::documentSynopsis(nodes);
        QVERIFY(synopsis != nullptr);

        const QDomElement rootElement = synopsis->documentElement();
        QCOMPARE(rootElement.tagName(), QStringLiteral("Chapter 1"));
        QCOMPARE(rootElement.attribute(QStringLiteral("Open")), QStringLiteral("true"));
        QVERIFY(rootElement.hasAttribute(QStringLiteral("Viewport")));

        const QDomNode childNode = rootElement.firstChild();
        QVERIFY(!childNode.isNull());
        const QDomElement childElement = childNode.toElement();
        QCOMPARE(childElement.tagName(), QStringLiteral("Item"));
        QVERIFY(!childElement.hasAttribute(QStringLiteral("Open")));
        QCOMPARE(childElement.attribute(QStringLiteral("DestinationURI")), QStringLiteral("https://example.com/sec1"));
    }

    void textPageConversionConstructsNormalizedBoxes()
    {
        // 1. Invalid dimensions or empty vector
        std::unique_ptr<Okular::TextPage> emptyPage(::Mu::Generator::Conversion::textPage({ }, 100, 200));
        QVERIFY(emptyPage != nullptr);
        QVERIFY(emptyPage->text().isEmpty());

        // 2. Valid boxes
        std::vector<::Mu::Model::TextBox> boxes;
        boxes.push_back({ "Hello", 10.0, 20.0, 50.0, 40.0 });
        boxes.push_back({ "World", 60.0, 20.0, 100.0, 40.0 });

        std::unique_ptr<Okular::TextPage> page(::Mu::Generator::Conversion::textPage(boxes, 100.0, 200.0));
        QVERIFY(page != nullptr);
        QCOMPARE(page->text(), QStringLiteral("HelloWorld"));
    }

    void annotationConversionPreservesOpacityAndCaretSymbol()
    {
        ::Mu::Model::Annotation source;
        source.subtype = PDF_ANNOT_CARET;
        source.color = 0x80402010;
        source.extras.caretSymbolP = true;

        const auto annotation = ::Mu::Generator::Conversion::fromModel(source);
        QVERIFY(annotation != nullptr);
        QCOMPARE(annotation->style().color(), QColor(0x40, 0x20, 0x10));
        QCOMPARE(annotation->style().opacity(), 128.0 / 255.0);

        const auto* caret = dynamic_cast<const Okular::CaretAnnotation*>(annotation.get());
        QVERIFY(caret != nullptr);
        QCOMPARE(caret->caretSymbol(), Okular::CaretAnnotation::CaretSymbol::P);

        const auto roundTripped = ::Mu::Generator::Conversion::toModel(annotation.get());
        QVERIFY(roundTripped.has_value());
        QCOMPARE(roundTripped->color, source.color);
        QVERIFY(roundTripped->extras.caretSymbolP);
    }

    void annotationConversionRejectsFileAttachmentsWithoutPayload()
    {
        ::Mu::Model::Annotation source;
        source.subtype = PDF_ANNOT_FILE_ATTACHMENT;
        QVERIFY(!::Mu::Generator::Conversion::fromModel(source));

        Okular::FileAttachmentAnnotation annotation;
        QVERIFY(!::Mu::Generator::Conversion::toModel(&annotation));
    }

    void fontAndEmbeddedFileConversions()
    {
        // 1. Font conversion
        ::Mu::Model::Font modelFont;
        modelFont.name = "TestFont";
        modelFont.file = "/path/to/font.ttf";
        modelFont.type = ::Mu::Model::FontType::TrueType;
        modelFont.embedType = ::Mu::Model::FontEmbedType::EmbeddedSubset;

        const Okular::FontInfo fontInfo = ::Mu::Generator::Conversion::fromModel(modelFont);
        QCOMPARE(fontInfo.name(), QStringLiteral("TestFont"));
        QCOMPARE(fontInfo.file(), QStringLiteral("/path/to/font.ttf"));
        QCOMPARE(fontInfo.type(), Okular::FontInfo::TrueType);
        QCOMPARE(fontInfo.embedType(), Okular::FontInfo::EmbeddedSubset);

        // 2. Embedded file conversion
        ::Mu::Model::EmbeddedFile modelEf;
        modelEf.name = "attachment.pdf";
        modelEf.description = "Embedded doc";
        modelEf.size = 4;
        modelEf.creationDate = { true, 1000 };
        modelEf.modificationDate = { true, 2000 };
        modelEf.data = { 0x25, 0x50, 0x44, 0x46 };

        const auto embedded = ::Mu::Generator::Conversion::embeddedFile(modelEf);
        QVERIFY(embedded != nullptr);
        QCOMPARE(embedded->name(), QStringLiteral("attachment.pdf"));
        QCOMPARE(embedded->description(), QStringLiteral("Embedded doc"));
        QCOMPARE(embedded->size(), 4);
        QCOMPARE(embedded->data(), QByteArray("%PDF"));
    }

    void formFieldTextProxyExposesProperties()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);
        ::Mu::Plugin::s_mockUpdateForm = ::Mu::Plugin::echoFormUpdate;
        ::Mu::Model::FormField field;
        field.handle = "g1-f0-o42";
        field.page = 0;
        field.pdfObjectNumber = 42;
        field.type = ::Mu::Model::FormFieldType::Text;
        field.partialName = "FirstName";
        field.uiName = "First Name";
        field.fullyQualifiedName = "User.FirstName";
        field.rectangle = { 0.1, 0.2, 0.5, 0.3 };
        field.readOnly = false;
        field.visible = true;
        field.printable = true;
        field.text = "Alice";
        field.maximumLength = 50;
        field.multiline = false;
        field.password = false;

        ::Mu::Generator::Proxy::Form::Text proxy(field.pdfObjectNumber, field, &coordinator);
        coordinator.registerField(field.handle, &proxy);
        QCOMPARE(proxy.id(), field.pdfObjectNumber);
        QCOMPARE(proxy.name(), QStringLiteral("FirstName"));
        QCOMPARE(proxy.uiName(), QStringLiteral("First Name"));
        QCOMPARE(proxy.fullyQualifiedName(), QStringLiteral("User.FirstName"));
        QCOMPARE(proxy.rect().left, 0.1);
        QCOMPARE(proxy.rect().bottom, 0.3);
        QVERIFY(!proxy.isReadOnly());
        QVERIFY(proxy.isVisible());
        QVERIFY(proxy.isPrintable());
        QCOMPARE(proxy.textType(), Okular::FormFieldText::Normal);
        QCOMPARE(proxy.text(), QStringLiteral("Alice"));
        QCOMPARE(proxy.maximumLength(), 50);
        QVERIFY(!proxy.isPassword());
        QVERIFY(!proxy.isRichText());

        proxy.setText(QStringLiteral("Bob"));
        QCOMPARE(proxy.text(), QStringLiteral("Bob"));
        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void pushButtonActions()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);
        ::Mu::Model::FormField field;
        field.handle = "g1-f0-o44";
        field.pdfObjectNumber = 44;
        field.type = ::Mu::Model::FormFieldType::PushButton;
        field.partialName = "Reset";
        field.buttonCaption = "Clear form";
        field.rectangle = { 0.1, 0.2, 0.3, 0.4 };

        int resetCount = 0;
        ::Mu::Plugin::s_mockResetForm = [&](const ::Mu::Model::FormResetRequest& request) {
            if (request.handle == field.handle)
                ++resetCount;
            return ::Mu::Model::FormUpdateResponse { };
        };

        ::Mu::Generator::Proxy::Form::PushButton proxy(field.pdfObjectNumber, field, &coordinator);
        coordinator.registerField(field.handle, &proxy);
        QCOMPARE(proxy.buttonType(), Okular::FormFieldButton::Push);
        QCOMPARE(proxy.caption(), QStringLiteral("Clear form"));
        QVERIFY(!proxy.state());
        proxy.setState(false);
        QCOMPARE(resetCount, 0);
        proxy.setState(true);
        QCOMPARE(resetCount, 0);

        field.pushButtonAction = ::Mu::Model::FormPushButtonAction::Reset;
        ::Mu::Generator::Proxy::Form::PushButton resetProxy(field.pdfObjectNumber, field, &coordinator);
        coordinator.registerField(field.handle, &resetProxy);
        resetProxy.setState(true);
        QCOMPARE(resetCount, 1);

        ::Mu::Plugin::s_mockResetForm = nullptr;
    }

    void formFieldCheckBoxProxyExposesProperties()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);
        ::Mu::Plugin::s_mockUpdateForm = ::Mu::Plugin::echoFormUpdate;
        ::Mu::Model::FormField field;
        field.handle = "g1-f0-o43";
        field.page = 0;
        field.pdfObjectNumber = 43;
        field.type = ::Mu::Model::FormFieldType::CheckBox;
        field.partialName = "Subscribe";
        field.uiName = "Subscribe to Newsletter";
        field.fullyQualifiedName = "User.Subscribe";
        field.rectangle = { 0.6, 0.7, 0.8, 0.9 };
        field.readOnly = true;
        field.visible = true;
        field.printable = true;
        field.checked = true;
        field.onState = "Yes";

        ::Mu::Generator::Proxy::Form::CheckBox proxy(field.pdfObjectNumber, field, &coordinator);
        coordinator.registerField(field.handle, &proxy);
        QCOMPARE(proxy.id(), field.pdfObjectNumber);
        QCOMPARE(proxy.name(), QStringLiteral("Subscribe"));
        QCOMPARE(proxy.uiName(), QStringLiteral("Subscribe to Newsletter"));
        QCOMPARE(proxy.fullyQualifiedName(), QStringLiteral("User.Subscribe"));
        QCOMPARE(proxy.buttonType(), Okular::FormFieldButton::CheckBox);
        QVERIFY(proxy.state());
        QVERIFY(proxy.isReadOnly());
        QVERIFY(proxy.isVisible());
        QVERIFY(proxy.isPrintable());
        // Read-only field ignores modifications
        proxy.setState(false);
        QVERIFY(proxy.state());

        // Editable field allows state updates
        field.readOnly = false;
        ::Mu::Generator::Proxy::Form::CheckBox editableProxy(field.pdfObjectNumber, field, &coordinator);
        coordinator.registerField(field.handle, &editableProxy);
        editableProxy.setState(false);
        QVERIFY(!editableProxy.state());
        QVERIFY(editableProxy.siblings().isEmpty());
        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void formFieldRadioButtonProxyExposesProperties()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);
        ::Mu::Plugin::s_mockUpdateForm = ::Mu::Plugin::echoFormUpdate;
        ::Mu::Model::FormField field;
        field.handle = "g1-f0-o44";
        field.page = 0;
        field.pdfObjectNumber = 44;
        field.type = ::Mu::Model::FormFieldType::RadioButton;
        field.partialName = "ChoiceA";
        field.uiName = "Option A";
        field.fullyQualifiedName = "Group1";
        field.rectangle = { 0.2, 0.3, 0.4, 0.5 };
        field.readOnly = false;
        field.visible = true;
        field.printable = true;
        field.checked = true;
        field.onState = "ChoiceA";

        ::Mu::Generator::Proxy::Form::RadioButton proxy(field.pdfObjectNumber, field, &coordinator);
        coordinator.registerField(field.handle, &proxy);
        QCOMPARE(proxy.id(), field.pdfObjectNumber);
        QCOMPARE(proxy.buttonType(), Okular::FormFieldButton::Radio);
        QCOMPARE(proxy.name(), QStringLiteral("ChoiceA"));
        QCOMPARE(proxy.uiName(), QStringLiteral("Option A"));
        QCOMPARE(proxy.fullyQualifiedName(), QStringLiteral("Group1"));
        QVERIFY(proxy.state());

        proxy.setSiblings({ 21, 22 });
        QCOMPARE(proxy.siblings(), QList<int>({ 21, 22 }));

        proxy.setState(false);
        QVERIFY(!proxy.state());
        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void formFieldChoiceProxyExposesProperties()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);
        ::Mu::Plugin::s_mockUpdateForm = ::Mu::Plugin::echoFormUpdate;
        ::Mu::Model::FormField comboField;
        comboField.handle = "g1-f0-o45";
        comboField.page = 0;
        comboField.pdfObjectNumber = 45;
        comboField.type = ::Mu::Model::FormFieldType::ComboBox;
        comboField.partialName = "Country";
        comboField.uiName = "Select Country";
        comboField.fullyQualifiedName = "User.Country";
        comboField.rectangle = { 0.1, 0.1, 0.4, 0.2 };
        comboField.readOnly = false;
        comboField.visible = true;
        comboField.printable = true;
        comboField.choices = { "Germany", "France", "United States" };
        comboField.exportValues = { "DE", "FR", "US" };
        comboField.currentChoices = { 1 };
        comboField.editableCombo = true;
        comboField.text = "France";

        ::Mu::Generator::Proxy::Form::Choice comboProxy(comboField.pdfObjectNumber, comboField, &coordinator);
        coordinator.registerField(comboField.handle, &comboProxy);
        QCOMPARE(comboProxy.id(), comboField.pdfObjectNumber);
        QCOMPARE(comboProxy.choiceType(), Okular::FormFieldChoice::ComboBox);
        QCOMPARE(comboProxy.choices(),
                 QStringList({ QStringLiteral("Germany"), QStringLiteral("France"), QStringLiteral("United States") }));
        QVERIFY(comboProxy.isEditable());
        QVERIFY(!comboProxy.multiSelect());
        QCOMPARE(comboProxy.currentChoices(), QList<int>({ 1 }));
        QCOMPARE(comboProxy.editChoice(), QStringLiteral("France"));
        Okular::FormFieldChoice* choice = &comboProxy;
        QCOMPARE(choice->exportValueForChoice(QStringLiteral("Germany")), QStringLiteral("DE"));

        comboProxy.setCurrentChoices({ 2 });
        QCOMPARE(comboProxy.currentChoices(), QList<int>({ 2 }));

        comboProxy.setEditChoice(QStringLiteral("Canada"));
        QCOMPARE(comboProxy.editChoice(), QStringLiteral("Canada"));
        QVERIFY(comboProxy.currentChoices().isEmpty());

        ::Mu::Model::FormField listField;
        listField.handle = "g1-f0-o46";
        listField.type = ::Mu::Model::FormFieldType::ListBox;
        listField.choices = { "A", "B", "C" };
        listField.currentChoices = { 0, 2 };
        listField.multiSelect = true;

        ::Mu::Generator::Proxy::Form::Choice listProxy(31, listField, &coordinator);
        QCOMPARE(listProxy.choiceType(), Okular::FormFieldChoice::ListBox);
        QVERIFY(listProxy.multiSelect());
        QCOMPARE(listProxy.currentChoices(), QList<int>({ 0, 2 }));
        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void coordinatorAppliesCanonicalValuesToAffectedProxies()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        std::vector<Okular::FormField*> refreshedFields;
        std::vector<int> refreshedPages;
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(
            dummyClient, [&](const std::vector<Okular::FormField*>& fields, const std::vector<int>& pages) {
                refreshedFields = fields;
                refreshedPages = pages;
            });

        // Radio 1 (page 0, initially checked)
        ::Mu::Model::FormField f1;
        f1.handle = "h-radio-1";
        f1.page = 0;
        f1.type = ::Mu::Model::FormFieldType::RadioButton;
        f1.checked = true;
        ::Mu::Generator::Proxy::Form::RadioButton r1(101, f1, &coordinator);
        coordinator.registerField(f1.handle, &r1);

        // Radio 2 (page 1, initially unchecked)
        ::Mu::Model::FormField f2;
        f2.handle = "h-radio-2";
        f2.page = 1;
        f2.type = ::Mu::Model::FormFieldType::RadioButton;
        f2.checked = false;
        ::Mu::Generator::Proxy::Form::RadioButton r2(102, f2, &coordinator);
        coordinator.registerField(f2.handle, &r2);

        std::string requestedHandle;
        ::Mu::Plugin::s_mockUpdateForm =
            [&](const ::Mu::Model::FormUpdateRequest& req) -> std::optional<::Mu::Model::FormUpdateResponse> {
            requestedHandle = req.handle;
            ::Mu::Model::FormUpdateResponse resp;
            resp.affectedFields = { { "h-radio-1", ::Mu::Model::FormCheckValue { false } },
                                    { "h-radio-2", ::Mu::Model::FormCheckValue { true } } };
            resp.affectedPages = { 0, 1 };
            return resp;
        };

        // User checks Radio 2
        r2.setState(true);

        QCOMPARE(requestedHandle, std::string("h-radio-2"));

        QVERIFY(r2.state());
        QVERIFY(!r1.state());
        QCOMPARE(refreshedFields, std::vector<Okular::FormField*>({ &r1, &r2 }));
        QCOMPARE(refreshedPages, (std::vector<int> { 0, 1 }));

        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void coordinatorIgnoresIncompatibleCanonicalValues()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        std::vector<Okular::FormField*> refreshedFields;
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(
            dummyClient,
            [&](const std::vector<Okular::FormField*>& fields, const std::vector<int>&) { refreshedFields = fields; });

        ::Mu::Model::FormField field;
        field.handle = "h-text-incompatible";
        field.type = ::Mu::Model::FormFieldType::Text;
        field.text = "OriginalText";
        ::Mu::Generator::Proxy::Form::Text textProxy(202, field, &coordinator);
        coordinator.registerField(field.handle, &textProxy);

        ::Mu::Plugin::s_mockUpdateForm =
            [](const ::Mu::Model::FormUpdateRequest&) -> std::optional<::Mu::Model::FormUpdateResponse> {
            ::Mu::Model::FormUpdateResponse response;
            response.affectedFields = { { "h-text-incompatible", ::Mu::Model::FormCheckValue { true } } };
            return response;
        };

        textProxy.setText(QStringLiteral("RequestedText"));

        QCOMPARE(textProxy.text(), QStringLiteral("OriginalText"));
        QVERIFY(refreshedFields.empty());
        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void coordinatorRejectionPreservesPriorState()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);

        ::Mu::Model::FormField field;
        field.handle = "h-text-1";
        field.type = ::Mu::Model::FormFieldType::Text;
        field.text = "OriginalText";
        ::Mu::Generator::Proxy::Form::Text textProxy(201, field, &coordinator);
        coordinator.registerField(field.handle, &textProxy);

        // Mock failure / rejection
        ::Mu::Plugin::s_mockUpdateForm =
            [](const ::Mu::Model::FormUpdateRequest&) -> std::optional<::Mu::Model::FormUpdateResponse> {
            return std::nullopt;
        };

        textProxy.setText(QStringLiteral("RejectedEdit"));

        // Value must remain unchanged
        QCOMPARE(textProxy.text(), QStringLiteral("OriginalText"));

        // A fail-closed coordinator rejects further edits without mutating the proxy.
        coordinator.setAvailable(false);
        textProxy.setText(QStringLiteral("UnavailableEdit"));
        QCOMPARE(textProxy.text(), QStringLiteral("OriginalText"));

        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }

    void documentWideRadioGroupingLinksAcrossPages()
    {
        const auto siblings = ::Mu::Generator::Proxy::Form::radioSiblingIds(
            { { 1, "GroupA", -1 }, { 2, "GroupA", -1 }, { 3, "GroupA", -1 }, { 4, "GroupB", -1 } });
        QCOMPARE(siblings.at(1), std::vector<int>({ 2, 3 }));
        QCOMPARE(siblings.at(2), std::vector<int>({ 1, 3 }));
        QCOMPARE(siblings.at(3), std::vector<int>({ 1, 2 }));
        QVERIFY(!siblings.contains(4));

        const auto fallbackSiblings = ::Mu::Generator::Proxy::Form::radioSiblingIds(
            { { 5, "", 42 }, { 6, "", 42 }, { 7, "Named", 42 }, { 8, "Named", 99 } });
        QCOMPARE(fallbackSiblings.at(5), std::vector<int>({ 6 }));
        QCOMPARE(fallbackSiblings.at(7), std::vector<int>({ 8 }));
    }

    void anonymousRadiosAreIsolated()
    {
        const auto siblings = ::Mu::Generator::Proxy::Form::radioSiblingIds({ { 10, "", -1 }, { 11, "", -1 } });
        QVERIFY(siblings.empty());
    }

    void radioNoToggleToOffSuppressesDirectUncheckIpc()
    {
        auto* dummyClient = reinterpret_cast<::Mu::Plugin::WorkerClient*>(0x1000);
        ::Mu::Generator::Proxy::Form::Coordinator coordinator(dummyClient);

        // Radio A (initially checked, has noToggleToOff = true)
        ::Mu::Model::FormField f1;
        f1.handle = "h-radio-a";
        f1.type = ::Mu::Model::FormFieldType::RadioButton;
        f1.checked = true;
        f1.noToggleToOff = true;
        ::Mu::Generator::Proxy::Form::RadioButton r1(301, f1, &coordinator);
        coordinator.registerField(f1.handle, &r1);

        // Radio B (initially unchecked, has noToggleToOff = true)
        ::Mu::Model::FormField f2;
        f2.handle = "h-radio-b";
        f2.type = ::Mu::Model::FormFieldType::RadioButton;
        f2.checked = false;
        f2.noToggleToOff = true;
        ::Mu::Generator::Proxy::Form::RadioButton r2(302, f2, &coordinator);
        coordinator.registerField(f2.handle, &r2);

        int requestCount = 0;
        std::string lastRequestedHandle;
        ::Mu::Plugin::s_mockUpdateForm =
            [&](const ::Mu::Model::FormUpdateRequest& req) -> std::optional<::Mu::Model::FormUpdateResponse> {
            ++requestCount;
            lastRequestedHandle = req.handle;
            ::Mu::Model::FormUpdateResponse resp;
            resp.affectedFields = { { "h-radio-a", ::Mu::Model::FormCheckValue { false } },
                                    { "h-radio-b", ::Mu::Model::FormCheckValue { true } } };
            resp.affectedPages = { 0 };
            return resp;
        };

        // 1. Deselecting active sibling directly must NOT trigger IPC
        r1.setState(false);
        QCOMPARE(requestCount, 0);
        QVERIFY(r1.state());

        // 2. Selecting new radio button triggers 1 atomic IPC request
        r2.setState(true);
        QCOMPARE(requestCount, 1);
        QCOMPARE(lastRequestedHandle, std::string("h-radio-b"));

        // 3. Both proxies reflect canonical state
        QVERIFY(r2.state());
        QVERIFY(!r1.state());

        ::Mu::Plugin::s_mockUpdateForm = nullptr;
    }
};

QTEST_MAIN(TestGeneratorProxies)

#include "test_proxies.moc"
