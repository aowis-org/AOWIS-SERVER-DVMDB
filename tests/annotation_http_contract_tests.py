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
