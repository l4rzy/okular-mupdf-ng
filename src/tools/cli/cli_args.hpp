// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_TOOLS_CLI_ARGS_HPP
#define MU_TOOLS_CLI_ARGS_HPP

#include <QStringList>

#include <string>
#include <vector>

namespace Mu::Tools::Cli {

// Exit codes shared by the tool and its tests.
inline constexpr int ExitOk = 0;
inline constexpr int ExitUsage = 1;
inline constexpr int ExitJobFailed = 2;
inline constexpr int ExitTimeout = 3;

struct SharedOptions {
    std::string worker;
    std::string password;
    int timeoutSeconds = 120;
};

struct OcrOptions {
    std::string file;
    /// Zero-based page index; defaults to the first page.
    int page = 0;
    /// Empty (or "-") means stdout.
    std::string output;
    std::string language = "eng";
    int dpi = 300;
    std::string tessData;
    SharedOptions shared;
};

/// Output format for the export command, inferred from the output suffix.
enum class ExportFormat {
    Pdf,
    Xfdf,
    Unknown,
};

struct ExportOptions {
    std::string file;
    std::string output;
    ExportFormat format = ExportFormat::Unknown;
    /// Empty means all pages.
    std::vector<int> pages;
    /// Apply the persisted EPUB layout settings before exporting.
    bool useLayout = false;
    /// Bake PDF annotations and form widgets into static page content.
    bool flatten = false;
    SharedOptions shared;
};

struct ImportOptions {
    std::string file;
    std::string xfdf;
    std::string output;
    SharedOptions shared;
};

struct Command {
    enum class Kind { Help, Version, Ocr, Export, Import };

    Kind kind = Kind::Help;
    OcrOptions ocr;
    ExportOptions exportOptions;
    ImportOptions importOptions;
    /// True when -h/--help was recognized for the selected command.
    bool helpRequested = false;
    /// Non-empty when parsing failed; main prints it with the help text.
    std::string error;
};

/// Parses argv (argv[0] is the program name) into a Command.
/// Decision logic only: no I/O, no process state, no QCoreApplication needed,
/// so unit tests feed it a QStringList directly.
[[nodiscard]] Command parseArgs(const QStringList& argv);

/// Help text for one command, or the full overview for Kind::Help.
[[nodiscard]] QString helpTextFor(Command::Kind kind);

/// Multi-line version string ("mupdfng-cli <compat>\nWorker: MuPDF <mupdfVersion>"),
/// mirroring the worker --version format. The caller supplies the runtime engine
/// version and its source label; an empty label omits the "Source: " prefix.
/// Pure for unit testing.
[[nodiscard]] QString versionText(const QString& mupdfVersion, const QString& source);

/// Parses "0,2,5" and "1-3,5" page selections. Empty means all pages.
[[nodiscard]] bool parsePageList(const std::string& text, std::vector<int>& pages, std::string& error);

/// Infers the export format from the output path suffix (.pdf, .xfdf/.xml;
/// case-insensitive). Anything else yields ExportFormat::Unknown.
[[nodiscard]] ExportFormat exportFormatForSuffix(const std::string& path);

} // namespace Mu::Tools::Cli

#endif // MU_TOOLS_CLI_ARGS_HPP
