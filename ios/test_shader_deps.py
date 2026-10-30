#!/usr/bin/env python3
"""Compile the actual production deps_match and check its cache-match contract.

Run with /usr/bin/python3 ios/test_shader_deps.py. --source accepts an older
totk_shader_rt.m for a regression comparison. Optional --requests DIR --cache DIR
adds real request/cache pairs; only dependency dictionaries enter the harness.
Shader code, request contents and private paths are never printed or persisted.
Requires macOS Foundation and xcrun clang; does not build or launch the game.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m"
KINDS = ("tex", "img", "attr", "fout")


def extract_function(source):
    """Balance C braces, ignoring quoted strings and both comment forms."""
    text = source.read_text()
    match = re.search(r"\bstatic\s+int\s+deps_match\s*\(", text)
    if not match:
        raise ValueError("static int deps_match was not found")
    opening = text.find("{", match.end())
    if opening < 0:
        raise ValueError("deps_match has no body")
    depth, quote, comment, escape = 0, None, None, False
    i = opening
    while i < len(text):
        c, pair = text[i], text[i:i + 2]
        if comment == "//":
            if c == "\n":
                comment = None
        elif comment == "/*":
            if pair == "*/":
                comment = None
                i += 1
        elif quote:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == quote:
                quote = None
        elif pair in ("//", "/*"):
            comment = pair
            i += 1
        elif c in ('"', "'"):
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():i + 1]
        i += 1
    raise ValueError("deps_match has an unbalanced body")


def basic_cases():
    cases = []

    def add(name, kind, want, have, expected):
        cases.append({"name": name, "deps": {kind: {"0": want}},
                      "answers": {} if have is None else {kind: {"0": have}},
                      "expected": expected})

    add("wildcard accepts 2d", "tex", "*", "2d", True)
    add("wildcard accepts cube", "tex", "*", "cube", True)
    add("wildcard rejects depth", "tex", "*", "2d_depth", False)
    add("wildcard rejects object", "tex", "*", {"type": "2d"}, False)
    add("depth wildcard accepts 2d_depth", "tex", "*_depth", "2d_depth", True)
    add("depth wildcard accepts cube_depth", "tex", "*_depth", "cube_depth", True)
    add("depth wildcard rejects 2d", "tex", "*_depth", "2d", False)
    add("depth wildcard rejects object", "tex", "*_depth", {"type": "2d_depth"}, False)
    add("missing texture defaults to 2d", "tex", "2d", None, True)
    add("wildcard accepts default texture", "tex", "*", None, True)
    add("depth wildcard rejects default texture", "tex", "*_depth", None, False)
    for declared in ("float", "int", "uint"):
        add("unbound attribute defaults to float (declared " + declared + ")",
            "attr", declared, None, declared == "float")
    add("bound attribute rejects wrong type", "attr", "sint", "float", False)
    add("fragment output exact match", "fout", "uint", "uint", True)
    add("fragment output rejects wrong type", "fout", "uint", "float", False)
    add("image exact dictionary match", "img", {"type": "2d", "format": "r32ui"},
        {"format": "r32ui", "type": "2d"}, True)
    add("image rejects wrong dictionary", "img", {"type": "2d", "format": "r32ui"},
        {"type": "2d", "format": "rgba8"}, False)
    add("missing image defaults to empty dictionary", "img", {}, None, True)
    add("fragment output wildcard is not relaxed", "fout", "*", "float", False)
    add("attribute wildcard is not relaxed", "attr", "*", "float", False)
    add("image wildcard is not relaxed", "img", "*", "2d", False)
    return cases


def real_cases(requests, cache):
    paths = sorted(requests.glob("*.json")) if requests.is_dir() else [requests]
    if not paths:
        raise ValueError("no request JSON files found")
    cases = []
    for number, path in enumerate(paths, 1):
        request = json.loads(path.read_text())
        variant = request.get("variant", "")
        if not re.fullmatch(r"[0-9a-f]{40,64}-[0-9a-f]{12,64}", variant):
            raise ValueError("request %d has no valid variant identifier" % number)
        index = cache / (variant.split("-", 1)[0] + ".variants")
        records = json.loads(index.read_text()) if index.is_file() else []
        record = next((r for r in records if r.get("variant") == variant), None)
        if record is None:
            reflection = cache / (variant + "_b.json")
            if not reflection.is_file():
                raise ValueError("request %d has no cached variant" % number)
            record = json.loads(reflection.read_text())
        deps = record.get("deps")
        if not isinstance(deps, dict):
            raise ValueError("request %d has no dependency dictionary" % number)
        answers = {kind: request[kind] for kind in KINDS if kind in request}
        if any(not isinstance(v, dict) for v in answers.values()):
            raise ValueError("request %d has a malformed answer dictionary" % number)
        # Never pass code_hex, c1, source paths or other request fields onward.
        cases.append({"name": "cached request %02d" % number, "deps": deps,
                      "answers": answers, "expected": True})
    return cases


HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdio.h>

__FUNCTION__

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSData *input = [NSData dataWithContentsOfFile:@(argv[1])];
        NSError *error = nil;
        NSArray *cases = [NSJSONSerialization JSONObjectWithData:input options:0 error:&error];
        if (error || ![cases isKindOfClass:NSArray.class]) return 2;
        NSMutableArray *failures = [NSMutableArray array];
        NSUInteger passed = 0;
        for (NSDictionary *test in cases) {
            @try {
                BOOL actual = !!deps_match(test[@"deps"], test[@"answers"]);
                BOOL expected = [test[@"expected"] boolValue];
                if (actual == expected) passed++;
                else [failures addObject:@{@"name": test[@"name"],
                    @"expected": @(expected), @"actual": @(actual)}];
            } @catch (NSException *exception) {
                [failures addObject:@{@"name": test[@"name"], @"exception": exception.name}];
            }
        }
        NSDictionary *result = @{@"passed": @(passed), @"total": @(cases.count),
                                  @"failures": failures};
        NSData *output = [NSJSONSerialization dataWithJSONObject:result options:0 error:&error];
        if (!output || error) return 2;
        fwrite(output.bytes, 1, output.length, stdout);
        return failures.count ? 1 : 0;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--requests", type=Path)
    parser.add_argument("--cache", type=Path)
    args = parser.parse_args()
    if bool(args.requests) != bool(args.cache):
        parser.error("--requests and --cache must be supplied together")
    try:
        function = extract_function(args.source)
        cases = basic_cases()
        extra = real_cases(args.requests, args.cache) if args.requests else []
        cases.extend(extra)
        with tempfile.TemporaryDirectory(prefix="gta-deps-test-") as temp:
            temp = Path(temp)
            source, fixtures, binary = temp / "check.m", temp / "fixtures.json", temp / "check"
            source.write_text(HARNESS.replace("__FUNCTION__", function))
            fixtures.write_text(json.dumps(cases))
            compiled = subprocess.run([
                "xcrun", "clang", "-fobjc-arc", "-framework", "Foundation",
                "-fmodules-cache-path=" + str(temp / "modules"),
                str(source), "-o", str(binary),
            ], capture_output=True, text=True)
            if compiled.returncode:
                print("ERROR: Foundation harness did not compile", file=sys.stderr)
                print(compiled.stderr, file=sys.stderr)
                return 2
            checked = subprocess.run([str(binary), str(fixtures)], capture_output=True, text=True)
            if checked.returncode not in (0, 1) or not checked.stdout:
                print("ERROR: harness produced no result (exit %d)" % checked.returncode, file=sys.stderr)
                return 2
            result = json.loads(checked.stdout)
        print("deps_match %s: %d/%d passed (%d basic, %d cached requests); function sha256 %s" % (
            "PASS" if not result["failures"] else "FAIL", result["passed"], result["total"],
            len(cases) - len(extra), len(extra), hashlib.sha256(function.encode()).hexdigest()[:16]))
        for failure in result["failures"]:
            if "exception" in failure:
                print("FAIL: %s raised %s" % (failure["name"], failure["exception"]))
            else:
                print("FAIL: %s expected=%s actual=%s" % (
                    failure["name"], failure["expected"], failure["actual"]))
        return 1 if result["failures"] else 0
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        # Exceptions can carry private request paths or content; do not dump them.
        print("ERROR: cannot prepare or read regression inputs (%s)" % type(error).__name__, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
