#!/usr/bin/env python3
"""Guard against silently dropping the DVMDB annotation HTTP integration.

This is a source-contract smoke check, not a substitute for the native HTTP
regression or the full Qt/C++ build. It can run in Qt-free CI.
"""

from pathlib import Path
import re
import sys


def require(source: str, expression: str, message: str) -> None:
    if re.search(expression, source, re.MULTILINE | re.DOTALL) is None:
        raise AssertionError(message)


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: annotation_http_contract_tests.py ROUTES TESTS OPENAPI")
    routes, tests, api = (Path(name).read_text(encoding="utf-8") for name in sys.argv[1:])
    cmake = (Path(__file__).resolve().parents[1] / "CMakeLists.txt").read_text(encoding="utf-8")
    require(cmake, r"target_link_libraries\(aowis-dvmdb-http PUBLIC\s+"
            r"aowis-dvmdb-hydraulic\s+aowis-dvmdb-annotation-integrity\s+"
            r"aowis-dvmdb-annotation-action-replay\s*\)",
            "production HTTP target must export annotation action replay linkage")
    if re.search(r"target_link_libraries\(aowis-dvmdb-http-tests\s+PRIVATE\s+"
                 r"aowis-dvmdb-annotation-action-replay", cmake):
        raise AssertionError("replay linkage must not be test-only")
    # Old direct branch hydraulic writes cannot publish unjournaled
    # Distribution revisions or omit the mandatory annotation layer.
    for route in ("routeNetworks", "routeEdits"):
        require(routes, rf"server\.Post\({route},\s*\[\]\(.*?"
                rf"errorReply\(410,\s*QStringLiteral\(\"legacy_hydraulic_write_retired\"\)",
                f"legacy hydraulic branch POST must remain permanently retired: {route}")
    for route in ("routeAnnotationLayers", "routeAnnotationLayer", "routeDraftAnnotationLayer"):
        require(routes, rf"server\.Get\(\s*{route}\s*,", f"missing GET registration: {route}")
    for symbol in (
        "annotationLayerId",
        "materializeDraftAnnotationEdits",
        "validateDraftAnnotationCandidate",
        "checkNoClientAnnotationChanges",
        "emptyDistributionAnnotationLayer",
        "saveWorkingCopyTransactions",
    ):
        require(routes, re.escape(symbol), f"annotation edit integration missing: {symbol}")
    require(routes, r"std::optional<std::string>\s+validateEngineeringCandidateIfNeeded\("
            r".*?hydraulic::hydraulicDistributionNetworkKind"
            r".*?isAnnotationNativeKind\(record\.kind\)"
            r".*?annotations::validateAnnotationSnapshot\(candidate\)",
            "conditional engineering candidate validation was lost")
    require(routes, r"repository\.importRevisionBundle\(\*project, spool\.fd\(\), branch, limits,"
            r".*?validateEngineeringCandidateIfNeeded\(candidate\)",
            "HTTP replication import lost pre-publication annotation validation")
    native_store = (Path(__file__).resolve().parents[1] / "src/native_store.cpp").read_text(encoding="utf-8")
    require(native_store, r"invalid imported engineering revision",
            "low-level revision import lost semantic history preflight")
    require(cmake, r"target_link_libraries\(aowis-dvmdb-sync PRIVATE "
            r"aowis-dvmdb-annotation-integrity\)",
            "Qt-backed sync CLI lost its annotation validator")
    require(routes, r"annotations::navigateVerifiedAnnotationAction\(service, operationId",
            "annotation archived History lost its fenced HTTP publication adapter")
    require(routes, r"annotations::navigateVerifiedCombinedAction\(service, operationId",
            "combined hydraulic/annotation History lost its fenced navigation adapter")
    require(routes, r"annotations::navigateVerifiedActiveDraftAction\(service, operationId",
            "active annotation/mixed draft History lost its fenced navigation adapter")
    require(tests, r"active mixed new edit failed to invalidate obsolete Redo tail",
            "active combined Undo/Redo and branching regression was removed")
    require(tests, r"active annotation-only Undo or idempotent retry failed",
            "active annotation-only Undo/Redo regression was removed")
    require(tests, r"unborn annotation new edit did not abandon the Redo suffix",
            "unborn annotation redo invalidation regression was removed")
    require(tests, r"unborn annotation boundary zero did not preserve initialization seed",
            "unborn annotation active History regression was removed")
    require(routes, r"canNavigateVerifiedActiveAnnotationDraftAction\(",
            "unborn History cursor must check annotation replay eligibility")
    require(tests, r"active fork Undo did not restore the verified archive boundary",
            "archived-fork active annotation Undo regression was removed")
    require(tests, r"active fork Redo resurrected abandoned archived history",
            "archived-fork active annotation Redo regression was removed")
    require(tests, r"combined action boundary 1 failed to restore committed hydraulic and annotation values",
            "mixed HTTP History publication regression was removed")
    require(tests, r"annotation archived Redo to tip did not recover clean committed HEAD",
            "annotation archived History publication regression was removed")
    require(routes, r"server\.Post\(routeModelCommits.*?service\.commitModel\(.*?"
            r"validateEngineeringCandidateIfNeeded\(candidate\)",
            "direct MODEL HTTP Commit lost semantic candidate validation")
    require(tests, r"void\s+exerciseAnnotationDraftHttp\s*\(",
            "missing annotation HTTP integration test definition")
    require(tests, r"(?<!void\s)exerciseAnnotationDraftHttp\s*\(\s*\)\s*;",
            "annotation HTTP integration test is not invoked")
    for path in (
        "/projects/{projectId}/branches/{branch}/annotation-layers:",
        "/projects/{projectId}/branches/{branch}/annotation-layers/{layer}:",
        "/working-copies/{draftId}/annotation-layers/{layer}:",
    ):
        if path not in api:
            raise AssertionError(f"annotation route missing from OpenAPI: {path}")
    print("Annotation HTTP source contract present; runtime behavior requires CTest")


if __name__ == "__main__":
    main()
