#include "doctest.h"
#include "Fixture.h"
#include "Platform/RobloxPlatform.hpp"

TEST_SUITE_BEGIN("Diagnostics");

/// Collects all workspace diagnostic reports streamed as progress notifications, keyed by uri. Later reports override earlier ones
static std::unordered_map<Uri, lsp::WorkspaceDocumentDiagnosticReport, UriHash> collectStreamedWorkspaceDiagnostics(
    const TestClient& client, const lsp::ProgressToken& token)
{
    std::unordered_map<Uri, lsp::WorkspaceDocumentDiagnosticReport, UriHash> reports;
    for (const auto& [method, params] : client.notificationQueue)
    {
        if (method != "$/progress" || !params)
            continue;

        lsp::ProgressParams progressData = params.value();
        if (progressData.token != token)
            continue;

        lsp::WorkspaceDiagnosticReportPartialResult partialResult = progressData.value;
        for (const auto& report : partialResult.items)
            reports.insert_or_assign(report.uri, report);
    }
    return reports;
}

TEST_CASE_FIXTURE(Fixture, "document_diagnostics_sends_information_for_required_modules")
{
    client->capabilities.textDocument = lsp::TextDocumentClientCapabilities{};
    client->capabilities.textDocument->diagnostic = lsp::DiagnosticClientCapabilities{};
    client->capabilities.textDocument->diagnostic->relatedDocumentSupport = true;

    // Don't show diagnostic for game indexing
    loadDefinition("@extra", "declare game: any");

    registerDocumentForVirtualPath(newDocument("required.luau", R"(
        --!strict
        local x: string = 1
        return {}
    )"),
        "game/Testing/Required");
    auto document = newDocument("main.luau", R"(
        --!strict
        require(game.Testing.Required)
    )");

    auto diagnostics = workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{document}}, nullptr);
    CHECK_EQ(diagnostics.items.size(), 0);
    CHECK_EQ(diagnostics.relatedDocuments.size(), 1);
}

TEST_CASE_FIXTURE(Fixture, "document_diagnostics_does_not_send_information_for_required_modules_if_related_document_support_is_disabled")
{
    client->capabilities.textDocument = lsp::TextDocumentClientCapabilities{};
    client->capabilities.textDocument->diagnostic = lsp::DiagnosticClientCapabilities{};
    client->capabilities.textDocument->diagnostic->relatedDocumentSupport = false;

    // Don't show diagnostic for game indexing
    loadDefinition("@extra", "declare game: any");

    registerDocumentForVirtualPath(newDocument("required.luau", R"(
        --!strict
        local x: string = 1
        return {}
    )"),
        "game/Testing/Required");
    auto document = newDocument("main.luau", R"(
        --!strict
        require(game.Testing.Required)
    )");

    auto diagnostics = workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{document}}, nullptr);
    CHECK_EQ(diagnostics.items.size(), 0);
    CHECK_EQ(diagnostics.relatedDocuments.size(), 0);
}

TEST_CASE_FIXTURE(Fixture, "text_document_update_marks_dependent_files_as_dirty")
{
    auto firstDocument = newDocument("a.luau", R"(
        --!strict
        return { hello = true }
    )");
    auto secondDocument = newDocument("b.luau", R"(
        --!strict
        local a = require("./a.luau")
        print(a.hello)
    )");

    auto diagnosticsA = workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    CHECK_EQ(diagnosticsA.items.size(), 0);

    auto diagnosticsB = workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);
    CHECK_EQ(diagnosticsB.items.size(), 0);

    // We should see diagnostics in the dependent file after the update request
    updateDocument(firstDocument, R"(
        --!strict
        return { hello2 = true }
    )");

    diagnosticsA = workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    CHECK_EQ(diagnosticsA.items.size(), 0);

    diagnosticsB = workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);
    CHECK_EQ(diagnosticsB.items.size(), 1);
    CHECK_EQ(diagnosticsB.items[0].message, "TypeError: Key 'hello' not found in table '{ hello2: boolean }'");
}

TEST_CASE_FIXTURE(Fixture, "text_document_update_triggers_dependent_diagnostics_in_push_based_diagnostics")
{
    client->globalConfig.diagnostics.includeDependents = true;

    auto firstDocument = newDocument("a.luau", R"(
        --!strict
        return { hello = true }
    )");
    auto secondDocument = newDocument("b.luau", R"(
        --!strict
        local a = require("./a.luau")
        print(a.hello)
    )");

    // Assumption: documents were already checked
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);

    updateDocument(firstDocument, R"(
        --!strict
        return { hello2 = true }
    )");

    REQUIRE(client->notificationQueue.size() > 2);
    auto secondNotification = *client->notificationQueue.rbegin();
    auto firstNotification = *(++client->notificationQueue.rbegin());

    REQUIRE_EQ(firstNotification.first, "textDocument/publishDiagnostics");
    REQUIRE(firstNotification.second);
    lsp::PublishDiagnosticsParams pushedDiagnostics = firstNotification.second.value();
    CHECK_EQ(pushedDiagnostics.uri, firstDocument);
    CHECK_EQ(pushedDiagnostics.diagnostics.size(), 0);

    REQUIRE_EQ(secondNotification.first, "textDocument/publishDiagnostics");
    REQUIRE(secondNotification.second);
    pushedDiagnostics = secondNotification.second.value();
    CHECK_EQ(pushedDiagnostics.uri, secondDocument);
    CHECK_EQ(pushedDiagnostics.diagnostics.size(), 1);
    CHECK_EQ(pushedDiagnostics.diagnostics[0].message, "TypeError: Key 'hello' not found in table '{ hello2: boolean }'");
}

TEST_CASE_FIXTURE(Fixture, "text_document_update_does_not_update_workspace_diagnostics")
{
    client->globalConfig.diagnostics.workspace = true;

    auto firstDocument = newDocument("a.luau", R"(
        --!strict
        return { hello = true }
    )");
    auto secondDocument = newDocument("b.luau", R"(
        --!strict
        local a = require("./a.luau")
        print(a.hello)
    )");

    // Assumption: initial workspace diagnostics was triggered
    // We are using documentDiagnostics to replicate workspace diagnostics checking the file (and making it non-dirty)
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);
    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";

    updateDocument(firstDocument, R"(
        --!strict
        return { hello2 = true }
    )");

    // Check no workspace diagnostics progress on queue
    for (const auto& notification : client->notificationQueue)
        CHECK_NE(notification.first, "$/progress");
}

TEST_CASE_FIXTURE(Fixture, "text_document_save_auto_updates_workspace_diagnostics_of_dependent_files")
{
    client->globalConfig.diagnostics.workspace = true;

    auto firstDocument = newDocument("a.luau", R"(
        --!strict
        return { hello = true }
    )");
    auto secondDocument = newDocument("b.luau", R"(
        --!strict
        local a = require("./a.luau")
        print(a.hello)
    )");

    // Assumption: initial workspace diagnostics was triggered
    // We are using documentDiagnostics to replicate workspace diagnostics checking the file (and making it non-dirty)
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);
    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";

    updateDocument(firstDocument, R"(
        --!strict
        return { hello2 = true }
    )");
    workspace.onDidSaveTextDocument(firstDocument, lsp::DidSaveTextDocumentParams{{firstDocument}});
    workspace.processAllWorkspaceDiagnostics();

    auto diagnostics = collectStreamedWorkspaceDiagnostics(*client, client->workspaceDiagnosticsToken.value());
    REQUIRE_EQ(diagnostics.size(), 2);

    auto mainDiagnostics = diagnostics.at(firstDocument);
    CHECK_EQ(mainDiagnostics.items.size(), 0);

    auto dependentDiagnostics = diagnostics.at(secondDocument);
    REQUIRE_EQ(dependentDiagnostics.items.size(), 1);
    CHECK_EQ(dependentDiagnostics.items[0].message, "TypeError: Key 'hello' not found in table '{ hello2: boolean }'");
}

TEST_CASE_FIXTURE(Fixture, "text_document_update_queues_workspace_diagnostics_of_dependent_files")
{
    client->globalConfig.diagnostics.workspace = true;

    auto firstDocument = newDocument("a.luau", R"(
        --!strict
        return { hello = true }
    )");
    auto secondDocument = newDocument("b.luau", R"(
        --!strict
        local a = require("./a.luau")
        print(a.hello)
    )");

    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);
    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";

    updateDocument(firstDocument, R"(
        --!strict
        return { hello2 = true }
    )");

    // Nothing is computed until the queue is processed
    CHECK(workspace.hasPendingWorkspaceDiagnostics());
    CHECK(collectStreamedWorkspaceDiagnostics(*client, client->workspaceDiagnosticsToken.value()).empty());

    workspace.processAllWorkspaceDiagnostics();
    CHECK_FALSE(workspace.hasPendingWorkspaceDiagnostics());

    auto diagnostics = collectStreamedWorkspaceDiagnostics(*client, client->workspaceDiagnosticsToken.value());

    // The edited file is handled by document diagnostics
    CHECK_EQ(diagnostics.find(firstDocument), diagnostics.end());

    REQUIRE_NE(diagnostics.find(secondDocument), diagnostics.end());
    auto dependentDiagnostics = diagnostics.at(secondDocument);
    REQUIRE_EQ(dependentDiagnostics.items.size(), 1);
    CHECK_EQ(dependentDiagnostics.items[0].message, "TypeError: Key 'hello' not found in table '{ hello2: boolean }'");
}

TEST_CASE_FIXTURE(Fixture, "cancelled_background_workspace_diagnostics_are_requeued")
{
    client->globalConfig.diagnostics.workspace = true;
    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";

    auto document = newDocument("a.luau", R"(
        --!strict
        local x: string = 1
        return x
    )");

    workspace.queueWorkspaceDiagnostics({document});

    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();
    workspace.processNextWorkspaceDiagnostic(cancellationToken);

    // Cancelled work is not reported, and is kept in the queue
    CHECK(collectStreamedWorkspaceDiagnostics(*client, client->workspaceDiagnosticsToken.value()).empty());
    CHECK(workspace.hasPendingWorkspaceDiagnostics());

    workspace.processNextWorkspaceDiagnostic(nullptr);
    CHECK_FALSE(workspace.hasPendingWorkspaceDiagnostics());

    auto diagnostics = collectStreamedWorkspaceDiagnostics(*client, client->workspaceDiagnosticsToken.value());
    REQUIRE_NE(diagnostics.find(document), diagnostics.end());
    CHECK_EQ(diagnostics.at(document).items.size(), 1);
}

TEST_CASE_FIXTURE(Fixture, "queued_workspace_diagnostics_wait_for_a_pull_request")
{
    client->capabilities.textDocument = lsp::TextDocumentClientCapabilities{};
    client->capabilities.textDocument->diagnostic = lsp::DiagnosticClientCapabilities{};
    client->globalConfig.diagnostics.workspace = true;

    auto document = newDocument("a.luau", "local x = 1");
    workspace.queueWorkspaceDiagnostics({document});

    // In pull mode, there is nowhere to send results until the client requests workspace diagnostics
    CHECK_FALSE(workspace.hasPendingWorkspaceDiagnostics());

    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";
    CHECK(workspace.hasPendingWorkspaceDiagnostics());
}

TEST_CASE_FIXTURE(Fixture, "queue_all_workspace_diagnostics_streams_results_for_all_files")
{
    client->globalConfig.diagnostics.workspace = true;
    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";

    auto first = Uri::file(tempDir.write_child("first.luau", "--!strict\nlocal x: string = 1\nreturn x\n"));
    auto second = Uri::file(tempDir.write_child("second.luau", "--!strict\nlocal y: number = 1\nreturn y\n"));

    workspace.queueAllWorkspaceDiagnostics();
    CHECK(workspace.hasPendingWorkspaceDiagnostics());
    workspace.processAllWorkspaceDiagnostics();

    auto diagnostics = collectStreamedWorkspaceDiagnostics(*client, client->workspaceDiagnosticsToken.value());
    REQUIRE_NE(diagnostics.find(first), diagnostics.end());
    REQUIRE_NE(diagnostics.find(second), diagnostics.end());
    CHECK_EQ(diagnostics.at(first).items.size(), 1);
    CHECK_EQ(diagnostics.at(second).items.size(), 0);
}

TEST_CASE_FIXTURE(Fixture, "text_document_save_does_not_update_workspace_diagnostics_if_setting_is_disabled")
{
    client->globalConfig.diagnostics.workspace = false;

    auto firstDocument = newDocument("a.luau", R"(
        --!strict
        return { hello = true }
    )");
    auto secondDocument = newDocument("b.luau", R"(
        --!strict
        local a = require("./a.luau")
        print(a.hello)
    )");

    // Assumption: initial workspace diagnostics was triggered
    // We are using documentDiagnostics to replicate workspace diagnostics checking the file (and making it non-dirty)
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{firstDocument}}, nullptr);
    workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{secondDocument}}, nullptr);
    client->workspaceDiagnosticsToken = "WORKSPACE-DIAGNOSTICS-PROGRESS-TOKEN";

    updateDocument(firstDocument, R"(
        --!strict
        return { hello2 = true }
    )");
    workspace.onDidSaveTextDocument(firstDocument, lsp::DidSaveTextDocumentParams{{firstDocument}});

    // Check no workspace diagnostics progress on queue
    for (const auto& notification : client->notificationQueue)
        CHECK_NE(notification.first, "$/progress");
}

TEST_CASE_FIXTURE(Fixture, "document_diagnostics_respects_cancellation")
{
    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();

    auto document = newDocument("a.luau", "local x = 1");
    CHECK_THROWS_AS(workspace.documentDiagnostics(lsp::DocumentDiagnosticParams{{document}}, cancellationToken), RequestCancelledException);
}

TEST_SUITE_END();
