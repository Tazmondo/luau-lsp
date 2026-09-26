#include "LSP/Diagnostics.hpp"

#include "LSP/Workspace.hpp"
#include "LSP/LanguageServer.hpp"
#include "LSP/Client.hpp"
#include "LSP/LuauExt.hpp"
#include "Luau/TimeTrace.h"
#include "LuauFileUtils.hpp"

LUAU_FASTFLAG(LuauSolverV2)

bool usingPullDiagnostics(const lsp::ClientCapabilities& capabilities)
{
    return capabilities.textDocument && capabilities.textDocument->diagnostic;
}

static bool supportsRelatedDocuments(const lsp::ClientCapabilities& capabilities)
{
    return capabilities.textDocument && capabilities.textDocument->diagnostic && capabilities.textDocument->diagnostic->relatedDocumentSupport;
}

/// Compute a document diagnostics report for a single file (and potentially related files)
/// By default, this is called by the client for an open document. Hence we can expect that files are managed
/// However, we sometimes call this as part of reverse-dependency updates (see updateTextDocument), where the file may be unmanaged
/// In the default cause, we don't want to bother opening the file unnecessarily if it was closed.
lsp::DocumentDiagnosticReport WorkspaceFolder::documentDiagnostics(
    const lsp::DocumentDiagnosticParams& params, const LSPCancellationToken& cancellationToken, bool allowUnmanagedFiles)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::documentDiagnostics", "LSP");
    if (!isReady)
    {
        lsp::DiagnosticServerCancellationData cancellationData{/*retriggerRequest: */ true};
        throw JsonRpcException(lsp::ErrorCode::ServerCancelled, "server not yet received configuration for diagnostics", cancellationData);
    }

    // TODO: should we apply a resultId and return an unchanged report if unchanged?
    lsp::DocumentDiagnosticReport report;
    std::unordered_map<Uri, std::vector<lsp::Diagnostic>, UriHash> relatedDiagnostics{};

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    TextDocumentPtr textDocument = allowUnmanagedFiles ? fileResolver.getOrCreateTextDocumentFromModuleName(moduleName)
                                                       : TextDocumentPtr(fileResolver.getTextDocument(params.textDocument.uri));
    if (!textDocument)
        return report; // Bail early with empty report - file was likely closed

    // Check the module
    // In the new solver, we end up calling `checkStrict` (retain type graphs), because documentation diagnostics is typically
    // on the file a user is working on. So, we will end up having to call checkStrict later for Hover etc. i.e., calling 2 typechecks
    // for no reason.
    // In the old solver, it doesn't really matter, because there is a differnce between module + moduleForAutocomplete. So we prefer
    // using checkSimple as we won't use the type graphs
    Luau::CheckResult cr =
        FFlag::LuauSolverV2 ? checkStrict(moduleName, cancellationToken, /* forAutocomplete= */ false) : checkSimple(moduleName, cancellationToken);

    throwIfCancelled(cancellationToken);

    // If there was an error retrieving the source module
    // Bail early with an empty report - it is likely that the file was closed
    if (!frontend.getSourceModule(moduleName))
        return report;

    auto config = client->getConfiguration(rootUri);

    // If the file is a definitions file, then don't display any diagnostics
    if (isDefinitionFile(params.textDocument.uri, config))
        return report;

    // Report Type Errors
    // Note that type errors can extend to related modules in the require graph - so we report related information here
    for (auto& error : cr.errors)
    {
        if (error.moduleName == moduleName)
        {
            auto diagnostic = createTypeErrorDiagnostic(error, &fileResolver, *textDocument);
            report.items.emplace_back(diagnostic);
        }
        else if (supportsRelatedDocuments(client->capabilities))
        {
            auto uri = platform->resolveToRealPath(error.moduleName);
            if (!uri)
                continue;
            auto relatedTextDocument = fileResolver.getTextDocumentFromModuleName(error.moduleName);
            if (isIgnoredFile(*uri, config))
                continue;
            auto diagnostic = createTypeErrorDiagnostic(error, &fileResolver, relatedTextDocument);
            auto& currentDiagnostics = relatedDiagnostics[*uri];
            currentDiagnostics.emplace_back(diagnostic);
        }
    }

    // Convert the related diagnostics map into an equivalent report
    if (supportsRelatedDocuments(client->capabilities) && !relatedDiagnostics.empty())
    {
        for (auto& [uri, diagnostics] : relatedDiagnostics)
        {
            // TODO: resultId?
            lsp::SingleDocumentDiagnosticReport subReport{lsp::DocumentDiagnosticReportKind::Full, std::nullopt, diagnostics};
            report.relatedDocuments.emplace(uri, subReport);
        }
    }

    // Report Lint Warnings
    // Lints only apply to the current file
    for (auto& error : cr.lintResult.errors)
    {
        auto diagnostic = createLintDiagnostic(error, *textDocument);
        diagnostic.severity = lsp::DiagnosticSeverity::Error; // Report this as an error instead
        report.items.emplace_back(diagnostic);
    }
    for (auto& error : cr.lintResult.warnings)
        report.items.emplace_back(createLintDiagnostic(error, *textDocument));

    return report;
}

std::vector<Uri> WorkspaceFolder::findFilesForWorkspaceDiagnostics(const std::string& rootPath, const ClientConfiguration& config)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::findFilesForWorkspaceDiagnostics", "LSP");

    std::vector<Uri> files{};
    Luau::FileUtils::traverseDirectoryRecursive(rootPath,
        [&](auto& path)
        {
            auto uri = Uri::file(path);
            auto ext = uri.extension();
            if ((ext == ".lua" || ext == ".luau") && !isDefinitionFile(uri, config))
            {
                files.push_back(uri);
            }
        });

    return files;
}

/// Compute the diagnostics report for a single file as part of workspace diagnostics.
/// Returns nothing if the file should be disregarded.
/// Throws RequestCancelledException if the cancellation token was triggered whilst checking.
std::optional<lsp::WorkspaceDocumentDiagnosticReport> WorkspaceFolder::computeWorkspaceDocumentDiagnostics(
    const Uri& uri, const ClientConfiguration& config, const LSPCancellationToken& cancellationToken)
{
    lsp::WorkspaceDocumentDiagnosticReport documentReport;
    documentReport.uri = uri;
    documentReport.kind = lsp::DocumentDiagnosticReportKind::Full;

    // If we don't have workspace diagnostics enabled, or we are are ignoring this file
    // Then provide an empty report to clear the file diagnostics
    if (!config.diagnostics.workspace || isIgnoredFile(uri, config) || isDefinitionFile(uri, config))
        return documentReport;

    auto moduleName = fileResolver.getModuleName(uri);
    auto document = fileResolver.getTextDocument(uri);
    if (document)
        documentReport.version = document->version();

    // Compute new check result
    Luau::CheckResult cr = checkSimple(moduleName, cancellationToken);

    // A cancelled check returns an empty result and leaves the module dirty, so it must not be reported
    throwIfCancelled(cancellationToken);

    // If there was an error retrieving the source module, disregard this file
    // TODO: should we file a diagnostic?
    if (!frontend.getSourceModule(moduleName))
        return std::nullopt;

    documentReport.items.reserve(cr.errors.size() + cr.lintResult.errors.size() + cr.lintResult.warnings.size());

    // Report Type Errors
    // Only report errors for the current file
    for (auto& error : cr.errors)
    {
        if (error.moduleName == moduleName)
        {
            auto diagnostic = createTypeErrorDiagnostic(error, &fileResolver, document);
            documentReport.items.emplace_back(diagnostic);
        }
    }

    // Report Lint Warnings
    for (auto& error : cr.lintResult.errors)
    {
        auto diagnostic = createLintDiagnostic(error, document);
        diagnostic.severity = lsp::DiagnosticSeverity::Error; // Report this as an error instead
        documentReport.items.emplace_back(diagnostic);
    }
    for (auto& error : cr.lintResult.warnings)
        documentReport.items.emplace_back(createLintDiagnostic(error, document));

    return documentReport;
}

lsp::WorkspaceDiagnosticReport WorkspaceFolder::workspaceDiagnostics(const lsp::WorkspaceDiagnosticParams& params)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::workspaceDiagnostics", "LSP");
    if (!isReady)
    {
        lsp::DiagnosticServerCancellationData cancellationData{/*retriggerRequest: */ true};
        throw JsonRpcException(lsp::ErrorCode::ServerCancelled, "server not yet received configuration for diagnostics", cancellationData);
    }

    lsp::WorkspaceDiagnosticReport workspaceReport;

    // Don't compute any workspace diagnostics for null workspace
    if (isNullWorkspace())
        return workspaceReport;

    auto config = client->getConfiguration(rootUri);

    // Find a list of files to compute diagnostics for
    auto files = findFilesForWorkspaceDiagnostics(rootUri.fsPath(), config);
    workspaceReport.items.reserve(files.size());

    for (const auto& uri : files)
    {
        if (auto documentReport = computeWorkspaceDocumentDiagnostics(uri, config, /* cancellationToken= */ nullptr))
            workspaceReport.items.emplace_back(std::move(*documentReport));
    }

    return workspaceReport;
}

void WorkspaceFolder::queueAllWorkspaceDiagnostics()
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::queueAllWorkspaceDiagnostics", "LSP");
    if (!isReady)
    {
        lsp::DiagnosticServerCancellationData cancellationData{/*retriggerRequest: */ true};
        throw JsonRpcException(lsp::ErrorCode::ServerCancelled, "server not yet received configuration for diagnostics", cancellationData);
    }

    // Don't compute any workspace diagnostics for null workspace
    if (isNullWorkspace())
        return;

    auto config = client->getConfiguration(rootUri);
    auto files = findFilesForWorkspaceDiagnostics(rootUri.fsPath(), config);

    // Files which will not be checked just need an empty report to clear them. Send these immediately
    std::vector<Uri> filesToCheck;
    std::vector<lsp::WorkspaceDocumentDiagnosticReport> clearedReports;
    for (auto& uri : files)
    {
        if (!config.diagnostics.workspace || isIgnoredFile(uri, config))
        {
            lsp::WorkspaceDocumentDiagnosticReport report;
            report.uri = uri;
            report.kind = lsp::DocumentDiagnosticReportKind::Full;
            clearedReports.emplace_back(std::move(report));
        }
        else
            filesToCheck.emplace_back(std::move(uri));
    }

    if (!clearedReports.empty())
        reportWorkspaceDocumentDiagnostics(clearedReports);

    queueWorkspaceDiagnostics(filesToCheck);
}

void WorkspaceFolder::queueWorkspaceDiagnostics(const std::vector<Uri>& uris)
{
    size_t newlyQueued = 0;
    for (const auto& uri : uris)
    {
        if (pendingWorkspaceDiagnosticsSet.insert(uri).second)
        {
            pendingWorkspaceDiagnostics.push_back(uri);
            newlyQueued++;
        }
    }

    if (newlyQueued > 0)
        updateWorkspaceDiagnosticsProgress(newlyQueued);
}

/// Whether there is somewhere to send computed workspace diagnostics to.
/// In pull mode, results are streamed as partial results of the open `workspace/diagnostic` request, so we
/// have to wait until the client (re-)requests them. Nothing is lost, as the queue persists until then
bool WorkspaceFolder::canReportWorkspaceDiagnostics() const
{
    return client->getWorkspaceDiagnosticsToken() || !usingPullDiagnostics(client->capabilities);
}

bool WorkspaceFolder::hasPendingWorkspaceDiagnostics() const
{
    return !pendingWorkspaceDiagnostics.empty() && canReportWorkspaceDiagnostics();
}

void WorkspaceFolder::reportWorkspaceDocumentDiagnostics(const std::vector<lsp::WorkspaceDocumentDiagnosticReport>& reports)
{
    if (auto token = client->getWorkspaceDiagnosticsToken())
    {
        client->sendProgress({*token, lsp::WorkspaceDiagnosticReportPartialResult{reports}});
    }
    else if (!usingPullDiagnostics(client->capabilities))
    {
        for (const auto& report : reports)
        {
            if (report.kind == lsp::DocumentDiagnosticReportKind::Full)
                client->publishDiagnostics(lsp::PublishDiagnosticsParams{report.uri, report.version, report.items});
        }
    }
}

void WorkspaceFolder::processNextWorkspaceDiagnostic(const LSPCancellationToken& cancellationToken)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFolder::processNextWorkspaceDiagnostic", "LSP");
    if (pendingWorkspaceDiagnostics.empty())
        return;

    auto uri = pendingWorkspaceDiagnostics.front();
    pendingWorkspaceDiagnostics.pop_front();
    pendingWorkspaceDiagnosticsSet.erase(uri);

    if (workspaceDiagnosticsProgressActive)
    {
        auto done = std::min(workspaceDiagnosticsProgressDone, workspaceDiagnosticsProgressTotal);
        auto percentage = static_cast<uint8_t>(done * 100 / workspaceDiagnosticsProgressTotal);
        client->sendWorkDoneProgressReport(workspaceDiagnosticsProgressToken(),
            "(" + std::to_string(done) + "/" + std::to_string(workspaceDiagnosticsProgressTotal) + ") " + uri.lexicallyRelative(rootUri),
            percentage);
    }

    try
    {
        auto config = client->getConfiguration(rootUri);
        if (auto report = computeWorkspaceDocumentDiagnostics(uri, config, cancellationToken))
            reportWorkspaceDocumentDiagnostics({*report});
    }
    catch (const RequestCancelledException&)
    {
        // Pre-empted by an incoming message. Retry this file first once we are idle again.
        // Any dependencies which finished checking before cancellation are kept, so no work is lost
        if (pendingWorkspaceDiagnosticsSet.insert(uri).second)
            pendingWorkspaceDiagnostics.push_front(uri);
        return;
    }
    catch (const std::exception& e)
    {
        client->sendLogMessage(lsp::MessageType::Error, "failed to compute workspace diagnostics for " + uri.toString() + ": " + e.what());
    }

    workspaceDiagnosticsProgressDone++;
    updateWorkspaceDiagnosticsProgress(0);
}

void WorkspaceFolder::processAllWorkspaceDiagnostics()
{
    while (!pendingWorkspaceDiagnostics.empty())
        processNextWorkspaceDiagnostic(/* cancellationToken= */ nullptr);
}

/// Only show progress for bulk re-checks. Re-checking the handful of dependents of an edited file should not flash a progress bar
static constexpr size_t kMinFilesForWorkspaceDiagnosticsProgress = 20;

std::string WorkspaceFolder::workspaceDiagnosticsProgressToken() const
{
    return "luau/workspaceDiagnostics/" + name;
}

void WorkspaceFolder::updateWorkspaceDiagnosticsProgress(size_t newlyQueued)
{
    const std::string token = workspaceDiagnosticsProgressToken();

    if (newlyQueued > 0)
    {
        if (workspaceDiagnosticsProgressActive)
        {
            workspaceDiagnosticsProgressTotal += newlyQueued;
        }
        else if (pendingWorkspaceDiagnostics.size() >= kMinFilesForWorkspaceDiagnosticsProgress)
        {
            workspaceDiagnosticsProgressActive = true;
            workspaceDiagnosticsProgressTotal = pendingWorkspaceDiagnostics.size();
            workspaceDiagnosticsProgressDone = 0;
            client->createWorkDoneProgress(token);
            client->sendWorkDoneProgressBegin(token, "Luau: Checking workspace", std::nullopt, 0);
        }
        return;
    }

    // Per-file progress is reported as each file starts being checked (see processNextWorkspaceDiagnostic)
    if (workspaceDiagnosticsProgressActive && pendingWorkspaceDiagnostics.empty())
    {
        workspaceDiagnosticsProgressActive = false;
        client->sendWorkDoneProgressEnd(token);
    }
}

void WorkspaceFolder::pushDiagnostics(const lsp::DocumentUri& uri, const size_t version)
{
    // Convert the diagnostics report into a series of diagnostics published for each relevant file
    lsp::DocumentDiagnosticParams params{lsp::TextDocumentIdentifier{uri}};

    try
    {
        auto diagnostics = documentDiagnostics(params, /* cancellationToken= */ nullptr);
        client->publishDiagnostics(lsp::PublishDiagnosticsParams{uri, version, diagnostics.items});
        if (!diagnostics.relatedDocuments.empty())
        {
            for (const auto& [relatedUri, relatedDiagnostics] : diagnostics.relatedDocuments)
            {
                if (relatedDiagnostics.kind == lsp::DocumentDiagnosticReportKind::Full)
                {
                    client->publishDiagnostics(lsp::PublishDiagnosticsParams{relatedUri, std::nullopt, relatedDiagnostics.items});
                }
            }
        }
    }
    catch (const JsonRpcException&)
    {
        // Server is not yet configured to send diagnostic messages
    }
}

/// Recompute all necessary diagnostics when we detect a configuration (or sourcemap) change
void WorkspaceFolder::recomputeDiagnostics(const ClientConfiguration& config)
{
    // Handle diagnostics if in push-mode
    if ((!client->capabilities.textDocument || !client->capabilities.textDocument->diagnostic))
    {
        // Recompute workspace diagnostics in the background if requested
        if (config.diagnostics.workspace)
        {
            queueAllWorkspaceDiagnostics();
        }
        // Recompute diagnostics for all currently opened files
        else
        {
            for (const auto& [_, document] : fileResolver.managedFiles)
                pushDiagnostics(document.uri(), document.version());
        }
    }
    else
    {
        client->terminateWorkspaceDiagnostics();
        client->refreshWorkspaceDiagnostics();
    }
}

lsp::PartialResponse<lsp::WorkspaceDiagnosticReport> LanguageServer::workspaceDiagnostic(const lsp::WorkspaceDiagnosticParams& params)
{
    client->workspaceDiagnosticsToken = params.partialResultToken;
    if (params.partialResultToken)
    {
        // Queue up all files to be checked in the background, streaming each result as a partial result once computed.
        // This keeps the server responsive to other requests whilst the (potentially large) workspace is checked
        for (auto& workspace : workspaceFolders)
            workspace->queueAllWorkspaceDiagnostics();
        return std::nullopt;
    }

    // The client does not support streaming, so we have no choice but to compute everything up front
    lsp::WorkspaceDiagnosticReport fullReport;
    for (auto& workspace : workspaceFolders)
    {
        auto report = workspace->workspaceDiagnostics(params);
        fullReport.items.insert(fullReport.items.end(), std::make_move_iterator(report.items.begin()), std::make_move_iterator(report.items.end()));
    }
    return fullReport;
}

void LSPClient::terminateWorkspaceDiagnostics(bool retriggerRequest)
{
    lsp::DiagnosticServerCancellationData cancellationData{retriggerRequest};

    if (this->workspaceDiagnosticsRequestId)
    {
        this->sendError(this->workspaceDiagnosticsRequestId,
            JsonRpcException(lsp::ErrorCode::ServerCancelled, "workspace diagnostics terminated", cancellationData));
    }

    this->workspaceDiagnosticsRequestId = std::nullopt;
    this->workspaceDiagnosticsToken = std::nullopt;
}
