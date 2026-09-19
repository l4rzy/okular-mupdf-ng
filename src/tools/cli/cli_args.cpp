// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools/cli/cli_args.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>

#include "shared/compat.hpp"

namespace Mu::Tools::Cli {

namespace {

bool parseInt(const std::string& text, int& value)
{
    if (text.empty())
        return false;
    std::size_t done = 0;
    try {
        value = std::stoi(text, &done);
    } catch (...) {
        return false;
    }
    return done == text.size();
}

void addSharedOptions(QCommandLineParser& parser)
{
    parser.addOption({ "worker", "Path to the okular-mupdf-worker binary (auto-detected otherwise).", "path" });
    parser.addOption({ "password", "Password for encrypted documents.", "password" });
    parser.addOption({ "timeout", "Timeout in seconds for worker jobs.", "seconds", QString::number(120) });
}

bool readSharedOptions(const QCommandLineParser& parser, SharedOptions& shared, std::string& error)
{
    shared.worker = parser.value("worker").toStdString();
    shared.password = parser.value("password").toStdString();
    if (!parseInt(parser.value("timeout").toStdString(), shared.timeoutSeconds) || shared.timeoutSeconds <= 0) {
        error = "invalid number for --timeout: " + parser.value("timeout").toStdString();
        return false;
    }
    return true;
}

QCommandLineParser& setupOcrParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription("OCR one document page through the MuPDF worker.");
    parser.addHelpOption();
    parser.addOption({ "page", "Zero-based page index to recognize (required).", "index" });
    parser.addOption({ "lang", "Tesseract language code.", "code", QStringLiteral("eng") });
    parser.addOption({ "dpi", "Render resolution for recognition.", "dpi", QStringLiteral("300") });
    parser.addOption({ "tessdata", "Tesseract tessdata directory.", "dir" });
    addSharedOptions(parser);
    parser.addPositionalArgument("file", "Input document.", "<file>");
    return parser;
}

QCommandLineParser& setupExportParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription("Export an EPUB document to PDF through the MuPDF worker.");
    parser.addHelpOption();
    parser.addOption({ { "o", "output" }, "Output PDF file (required).", "file" });
    parser.addOption({ "pages", "Zero-based pages to export, e.g. 0,2,5 or 1-3. Default: all pages.", "list" });
    addSharedOptions(parser);
    parser.addPositionalArgument("file", "Input EPUB document.", "<file>");
    return parser;
}

Command parseOcr(const QStringList& args)
{
    Command command;
    command.kind = Command::Kind::Ocr;
    auto& options = command.ocr;
    QCommandLineParser parser;
    setupOcrParser(parser);
    if (!parser.parse(args)) {
        command.error = parser.errorText().toStdString();
        return command;
    }
    if (parser.isSet("help"))
        return Command { };
    const QStringList files = parser.positionalArguments();
    if (files.size() != 1) {
        command.error = "expected exactly one input file";
        return command;
    }
    options.file = files.front().toStdString();
    if (!parser.isSet("page")) {
        command.error = "missing required --page";
        return command;
    }
    if (!parseInt(parser.value("page").toStdString(), options.page) || options.page < 0) {
        command.error = "invalid number for --page: " + parser.value("page").toStdString();
        return command;
    }
    options.language = parser.value("lang").toStdString();
    if (options.language.empty()) {
        command.error = "language must not be empty";
        return command;
    }
    if (!parseInt(parser.value("dpi").toStdString(), options.dpi) || options.dpi <= 0) {
        command.error = "invalid number for --dpi: " + parser.value("dpi").toStdString();
        return command;
    }
    options.tessData = parser.value("tessdata").toStdString();
    if (!readSharedOptions(parser, options.shared, command.error))
        return command;
    return command;
}

Command parseExportPdf(const QStringList& args)
{
    Command command;
    command.kind = Command::Kind::ExportPdf;
    auto& options = command.exportPdf;
    QCommandLineParser parser;
    setupExportParser(parser);
    if (!parser.parse(args)) {
        command.error = parser.errorText().toStdString();
        return command;
    }
    if (parser.isSet("help"))
        return Command { };
    const QStringList files = parser.positionalArguments();
    if (files.size() != 1) {
        command.error = "expected exactly one input file";
        return command;
    }
    options.file = files.front().toStdString();
    if (!parser.isSet("output")) {
        command.error = "missing required -o output";
        return command;
    }
    options.output = parser.value("output").toStdString();
    if (parser.isSet("pages") && !parsePageList(parser.value("pages").toStdString(), options.pages, command.error))
        return command;
    if (!readSharedOptions(parser, options.shared, command.error))
        return command;
    return command;
}

} // namespace

bool parsePageList(const std::string& text, std::vector<int>& pages, std::string& error)
{
    pages.clear();
    if (text.empty())
        return true;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string token = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (token.empty()) {
            error = "empty entry in page list: " + text;
            return false;
        }
        const std::size_t dash = token.find('-');
        if (dash == std::string::npos) {
            int page = -1;
            if (!parseInt(token, page) || page < 0) {
                error = "invalid page number: " + token;
                return false;
            }
            pages.push_back(page);
        } else {
            int first = -1, last = -1;
            if (!parseInt(token.substr(0, dash), first) || !parseInt(token.substr(dash + 1), last) || first < 0
                || last < first) {
                error = "invalid page range: " + token;
                return false;
            }
            for (int page = first; page <= last; ++page)
                pages.push_back(page);
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return true;
}

Command parseArgs(const QStringList& argv)
{
    Command command;
    if (argv.size() < 2) {
        command.error = "missing command";
        return command;
    }
    const QString verb = argv.at(1);
    if (verb == "-h" || verb == "--help" || verb == "help")
        return command;
    if (verb == "-V" || verb == "--version") {
        command.kind = Command::Kind::Version;
        return command;
    }
    // QCommandLineParser::parse skips element 0 as the program name, so keep
    // argv[0] and drop only the verb. The program name below shows up in the
    // generated help text.
    QStringList sub { argv.front() };
    sub.append(argv.mid(2));
    if (verb == "ocr")
        return parseOcr(sub);
    if (verb == "export-pdf")
        return parseExportPdf(sub);
    command.error = "unknown command: " + verb.toStdString();
    return command;
}

QString helpTextFor(Command::Kind kind)
{
    QCommandLineParser parser;
    if (kind == Command::Kind::Ocr)
        return setupOcrParser(parser).helpText();
    if (kind == Command::Kind::ExportPdf)
        return setupExportParser(parser).helpText();
    return QStringLiteral("usage: mupdfng-cli <command> [options]\n\n"
                          "commands:\n"
                          "  ocr <file> --page N          OCR one page, print text boxes\n"
                          "  export-pdf <file> -o OUT.pdf export an EPUB document to PDF\n\n"
                          "options:\n"
                          "  -V, --version                print version and exit\n\n"
                          "run 'mupdfng-cli <command> --help' for command options.\n");
}

QString versionText()
{
    return QStringLiteral("mupdfng-cli %1\nMuPDF %2\n")
        .arg(QString::fromUtf8(::Mu::IPC::COMPAT.data(), static_cast<qsizetype>(::Mu::IPC::COMPAT.size())))
        .arg(QString::fromUtf8(::Mu::MUPDF_VERSION.data(), static_cast<qsizetype>(::Mu::MUPDF_VERSION.size())));
}

} // namespace Mu::Tools::Cli
