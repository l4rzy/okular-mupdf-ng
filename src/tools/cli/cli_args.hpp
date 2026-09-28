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
    int page = -1;
    std::string language = "eng";
    int dpi = 300;
    std::string tessData;
    SharedOptions shared;
};

struct ExportPdfOptions {
    std::string file;
    std::string output;
    /// Empty means all pages.
    std::vector<int> pages;
    /// Apply the persisted EPUB layout settings before exporting.
    bool useLayout = false;
    SharedOptions shared;
};

struct ExportXfdfOptions {
    std::string file;
    std::string output;
    SharedOptions shared;
};

struct ApplyXfdfOptions {
    std::string file;
    std::string xfdf;
    std::string output;
    SharedOptions shared;
};

struct Command {
    enum class Kind { Help, Version, Ocr, ExportPdf, ExportXfdf, ApplyXfdf };

    Kind kind = Kind::Help;
    OcrOptions ocr;
    ExportPdfOptions exportPdf;
    ExportXfdfOptions exportXfdf;
    ApplyXfdfOptions applyXfdf;
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

} // namespace Mu::Tools::Cli

#endif // MU_TOOLS_CLI_ARGS_HPP
