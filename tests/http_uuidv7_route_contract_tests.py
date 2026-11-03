#!/usr/bin/env python3
"""Check canonical public HTTP route registrations against the API specification.

A source/spec consistency test, not an HTTP integration test. Native/Qt runtime
behavior is covered separately by native_http_tests.cpp on Qt-enabled builds.
"""
import argparse
import re
from pathlib import Path


def document_routes(text):
    result = set()
    parts = re.split(r"(?=^  /[^\n]+:)", text, flags=re.M)
    for part in parts:
        match = re.match(r"  (/[^\n]+):", part)
        if not match:
            continue
        path = match.group(1)
        for method in re.findall(r"^    (get|post|patch):", part, re.M):
            canonical = path.replace('{projectId}', ':id').replace('{branch}', ':branch')
            canonical = canonical.replace('{revision}', ':revision').replace('{revisionId}', ':revisionId').replace('{networkId}', ':network')
            canonical = canonical.replace('{entityId}', ':entity').replace('{draftId}', ':draft')
            canonical = canonical.replace('{branchId}', ':branchId').replace('{operationId}', ':operationId')
            result.add((method.upper(), '/dvmdb/v1' + canonical))
    return result


def source_routes(text):
    constants = dict(re.findall(r'const char\* const (\w+)\s*=\s*"([^"]+)";', text))
    found = set()
    for verb, argument in re.findall(r'server\.(Get|Post|Patch)\(\s*("[^"]+"|\w+)', text):
        url = argument[1:-1] if argument.startswith('"') else constants.get(argument)
        if url is None:
            raise AssertionError(f'unresolved HTTP route constant: {argument}')
        route = (verb.upper(), url)
        if route in found:
            raise AssertionError(f'duplicate route registration: {route}')
        found.add(route)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--openapi', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text()
    registered = source_routes(source)
    declared = document_routes(args.openapi.read_text())
    if registered != declared:
        raise AssertionError(f'HTTP/OpenAPI mismatch: only registered={sorted(registered - declared)}, '
                             f'only documented={sorted(declared - registered)}')
    if any('-v2' in path or '/revisions/:hash' in path for _, path in registered):
        raise AssertionError('obsolete revision URL remains registered')
    for route in [('POST', '/dvmdb/v1/projects'),
                  ('GET', '/dvmdb/v1/projects/:id/revision-graph'),
                  ('POST', '/dvmdb/v1/projects/:id/branches/:branch/model-commits'),
                  ('POST', '/dvmdb/v1/working-copies/:draft/save'),
                  ('POST', '/dvmdb/v1/projects/:id/branches/:branch/bundle'),
                  ('POST', '/dvmdb/v1/projects/:id/branches/:branch/networks'),
                  ('POST', '/dvmdb/v1/projects/:id/hydraulic-merges')]:
        if route not in registered:
            raise AssertionError(f'missing canonical UUIDv7 route: {route}')
    print(f'{len(registered)} UUIDv7 canonical HTTP methods match the OpenAPI contract')


if __name__ == '__main__':
    main()
