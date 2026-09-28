// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools/cli/cli_args.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>

#include <algorithm>
#include <cctype>

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
    parser.setApplicationDescription("OCR one document page and print the recognized text.");
    parser.addHelpOption();
    parser.addOption({ "lang", "Tesseract language code.", "code", QStringLiteral("eng") });
    parser.addOption({ "dpi", "Render resolution for recognition.", "dpi", QStringLiteral("300") });
    parser.addOption({ "tessdata", "Tesseract tessdata directory.", "dir" });
    parser.addOption(
        { { "o", "output" }, "Write the recognized text to FILE instead of stdout. Use - for stdout.", "file" });
    addSharedOptions(parser);
    parser.addPositionalArgument("file", "Input document.", "<file>");
    parser.addPositionalArgument("page", "Zero-based page index. Default: 0.", "[page]");
    return parser;
}

QCommandLineParser& setupExportParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription("Export a document through the MuPDF worker.\n"
                                     "The format is inferred from the output suffix: .pdf exports an EPUB document "
                                     "to PDF, .xfdf/.xml exports a PDF document's annotations as XFDF.");
    parser.addHelpOption();
    parser.addOption({ { "o", "output" }, "Output file; alternative to the OUTPUT positional.", "file" });
    parser.addOption({ "pages", "Zero-based pages to export, e.g. 0,2,5 or 1-3. Default: all pages.", "list" });
    parser.addOption({ "use-layout",
                       "Apply the configured EPUB layout settings (font size, page size, font family, custom CSS) "
                       "before exporting." });
    addSharedOptions(parser);
    parser.addPositionalArgument("file", "Input document.", "<file>");
    parser.addPositionalArgument("output", "Output file; alternative to -o.", "[output]");
    return parser;
}

QCommandLineParser& setupImportParser(QCommandLineParser& parser)
{
    parser.setApplicationDescription("Apply annotations from an XFDF file to a PDF document, writing a new PDF.\n"
                                     "Coordinates on pages with a non-zero /Rotate may be displaced.");
    parser.addHelpOption();
    parser.addOption({ { "o", "output" }, "Output PDF file; alternative to the OUTPUT positional.", "file" });
    addSharedOptions(parser);
    parser.addPositionalArgument("file", "Input PDF document.", "<file>");
    parser.addPositionalArgument("xfdf", "Input XFDF file.", "<xfdf>");
    parser.addPositionalArgument("output", "Output PDF file; alternative to -o.", "[output]");
    return parser;
}

/// Resolves the output from either the positional at outputIndex or -o.
/// Exactly one of the two must be present.
bool resolveOutput(const QStringList& files,
                   int outputIndex,
                   const QCommandLineParser& parser,
                   std::string& output,
                   std::string& error)
{
    const bool hasPositional = files.size() > outputIndex;
    const bool hasFlag = parser.isSet("output");
    if (hasPositional && hasFlag) {
        error = "output given twice (positional OUTPUT and -o)";
        return false;
    }
    if (!hasPositional && !hasFlag) {
        error = "missing output (positional OUTPUT or -o)";
        return false;
    }
    output = hasFlag ? parser.value("output").toStdString() : files.at(outputIndex).toStdString();
    return true;
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
    if (parser.isSet("help")) {
        command.helpRequested = true;
        return command;
    }
    const QStringList files = parser.positionalArguments();
    if (files.isEmpty() || files.size() > 2) {
        command.error = "expected a FILE and an optional PAGE";
        return command;
    }
    options.file = files.front().toStdString();
    if (files.size() == 2 && (!parseInt(files.at(1).toStdString(), options.page) || options.page < 0)) {
        command.error = "invalid page: " + files.at(1).toStdString();
        return command;
    }
    options.output = parser.value("output").toStdString();
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

Command parseExport(const QStringList& args)
{
    Command command;
    command.kind = Command::Kind::Export;
    auto& options = command.exportOptions;
    QCommandLineParser parser;
    setupExportParser(parser);
    if (!parser.parse(args)) {
        command.error = parser.errorText().toStdString();
        return command;
    }
    if (parser.isSet("help")) {
        command.helpRequested = true;
        return command;
    }
    const QStringList files = parser.positionalArguments();
    if (files.isEmpty() || files.size() > 2) {
        command.error = "expected a FILE and an optional OUTPUT";
        return command;
    }
    options.file = files.front().toStdString();
    if (!resolveOutput(files, 1, parser, options.output, command.error))
        return command;
    options.format = exportFormatForSuffix(options.output);
    if (options.format == ExportFormat::Unknown) {
        command.error = "cannot infer export format from output suffix: " + options.output;
        return command;
    }
    if (parser.isSet("pages") && !parsePageList(parser.value("pages").toStdString(), options.pages, command.error))
        return command;
    options.useLayout = parser.isSet("use-layout");
    if (!readSharedOptions(parser, options.shared, command.error))
        return command;
    return command;
}

Command parseImport(const QStringList& args)
{
    Command command;
    command.kind = Command::Kind::Import;
    auto& options = command.importOptions;
    QCommandLineParser parser;
    setupImportParser(parser);
    if (!parser.parse(args)) {
        command.error = parser.errorText().toStdString();
        return command;
    }
    if (parser.isSet("help")) {
        command.helpRequested = true;
        return command;
    }
    const QStringList files = parser.positionalArguments();
    if (files.size() < 2 || files.size() > 3) {
        command.error = "expected FILE XFDF and an optional OUTPUT";
        return command;
    }
    options.file = files.at(0).toStdString();
    options.xfdf = files.at(1).toStdString();
    if (!resolveOutput(files, 2, parser, options.output, command.error))
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

ExportFormat exportFormatForSuffix(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return ExportFormat::Unknown;
    std::string suffix = path.substr(dot + 1);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (suffix == "pdf")
        return ExportFormat::Pdf;
    if (suffix == "xfdf" || suffix == "xml")
        return ExportFormat::Xfdf;
    return ExportFormat::Unknown;
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
    if (verb == "export")
        return parseExport(sub);
    if (verb == "import")
        return parseImport(sub);
    command.error = "unknown command: " + verb.toStdString();
    return command;
}

QString helpTextFor(Command::Kind kind)
{
    QCommandLineParser parser;
    if (kind == Command::Kind::Ocr)
        return setupOcrParser(parser).helpText();
    if (kind == Command::Kind::Export)
        return setupExportParser(parser).helpText();
    if (kind == Command::Kind::Import)
        return setupImportParser(parser).helpText();
    return QStringLiteral("usage: mupdfng-cli <command> [options]\n\n"
                          "commands:\n"
                          "  ocr FILE [PAGE]              OCR one document page, print the recognized text\n"
                          "  export FILE [OUTPUT]         export a document (format from output suffix)\n"
                          "  import FILE XFDF [OUTPUT]    apply XFDF annotations to a PDF\n\n"
                          "options:\n"
                          "  -V, --version                print version and exit\n\n"
                          "run 'mupdfng-cli <command> --help' for command options.\n");
}

QString versionText(const QString& mupdfVersion, const QString& source)
{
    const QString engine = source.isEmpty() ? QStringLiteral("MuPDF %1").arg(mupdfVersion)
                                            : QStringLiteral("%1: MuPDF %2").arg(source, mupdfVersion);
    return QStringLiteral("mupdfng-cli %1\n%2\n")
        .arg(QString::fromUtf8(::Mu::IPC::COMPAT.data(), static_cast<qsizetype>(::Mu::IPC::COMPAT.size())), engine);
}

} // namespace Mu::Tools::Cli
