#!/usr/bin/env python3

"""
This program generates C include files containing NIST CAVP test vectors.
These are used to verify implementations of cryptographic hash functions.

For more information, consult ./src/README_cavp.md in the Firedancer project.
"""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
import textwrap
from typing import Iterator


def bin2cstr(data: bytes) -> str:
    """Returns a C string with the given data hex-escaped"""
    if len(data) == 0:
        return "NULL"
    out = '"'
    for i, b in enumerate(data):
        if i == 0:
            pass
        elif i % 32 == 0:
            out += '\n"'
        elif i % 8 == 0:
            out += ' "'
        out += "\\x%02x" % (b)
        if i % 8 == 7:
            out += '"'
    if len(data) % 8 != 0:
        out += '"'
    return out


def bin2carr(data: bytes) -> str:
    """Returns a C array initializer containing the given data"""
    assert len(data) > 0
    out = ""
    for i, b in enumerate(data):
        if i == 0:
            pass
        elif i % 16 == 0:
            out += ",\n"
        elif i % 8 == 0:
            out += ", "
        else:
            out += ","
        out += "_(%02x)" % (b)
    return out


@dataclass
class Msg:
    msg: bytes
    digest: bytes


def _find_line_match(lines: Iterator[str], pat: re.Pattern) -> re.Match:
    for line in lines:
        match = pat.match(line)
        if match:
            return match
    raise AssertionError("failed to parse file")


def parse_msg_rsp(file) -> "list[Msg]":
    """Parses a CAVP response file containing message tests."""

    lines = iter(file)
    _match_msg_count = re.compile(r"\[L = (\d+)\]")
    _match_msg_sz = re.compile(r"^Len = (\d+)\s*$")
    _match_msg = re.compile(r"^Msg = ([0-9a-fA-F]+)\s*$")
    _match_md = re.compile(r"^MD = ([0-9a-fA-F]+)\s*$")

    msgs = []
    msg_count = int(_find_line_match(lines, _match_msg_count).group(1))
    while msg_count > 0:
        msg_sz = int(_find_line_match(lines, _match_msg_sz).group(1))
        msg = bytes.fromhex(_find_line_match(lines, _match_msg).group(1))[:msg_sz]
        md = bytes.fromhex(_find_line_match(lines, _match_md).group(1))
        msgs.append(Msg(msg, md))
        msg_count -= 1

    return msgs


class HashMsgGenerator:
    name: str
    test_vector_type: str
    hashes: "list[bytes]"

    def __init__(self, name: str, test_vector_type: str):
        self.name = name
        self.test_vector_type = test_vector_type
        self.hashes = []

    def _test_vector_name(self, i) -> str:
        return f"{self.name}_test_{i}"

    def write_test(self, msg: Msg):
        if len(msg.msg) == 0:
            return
        i = len(self.hashes)
        self.hashes.append(msg.digest)
        print(f"static uchar const {self._test_vector_name(i)}[] = {{")
        print(textwrap.indent(bin2carr(msg.msg), prefix="  "))
        print("};")

    def finish(self):
        print(f"static {self.test_vector_type} const {self.name}[] = {{")
        for i, dgst in enumerate(self.hashes):
            decl_name = self._test_vector_name(i)
            print(f"  {{ (char const *){decl_name}, sizeof({decl_name}),")
            print(textwrap.indent(bin2cstr(dgst), "    "), end="")
            print(" },")
        print(r"  { NULL, 0UL, { 0 } }")
        print("};\n")


def _main():
    parser = argparse.ArgumentParser(
        description="""Generates a C include containing 'static const' SHA-2 test vectors given a CAVS response file.
Result is written to stdout.""",
    )
    parser.add_argument("--rsp", type=Path, help="Path to response file")
    parser.add_argument(
        "--alg", type=str, choices=["sha256", "sha384", "sha512"], required=True, help="Algorithm"
    )
    parser.add_argument("--name", type=str, required=True, help="Test name")
    parser.add_argument("--out", type=Path, help="Output to file (default stdout)")
    args = parser.parse_args()

    if args.out:
        sys.stdout = open(args.out, "w")

    print(
        f"""/* This file was auto-generated by ./contrib/test/cavp_generate.py as part of the Firedancer project.

   File name: {args.rsp.name} */

#define _(v) ((uchar)0x##v)
"""
    )

    with open(args.rsp) as rsp_file:
        msgs = parse_msg_rsp(rsp_file)

    test_vector_type = f"fd_{args.alg}_test_vector_t"
    gen = HashMsgGenerator(args.name, test_vector_type)
    for msg in msgs:
        gen.write_test(msg)
    print()
    gen.finish()

    print("#undef _")


if __name__ == "__main__":
    _main()