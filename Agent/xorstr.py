#!/usr/bin/env python3
"""
xorstr.py — String encryption compliance checker for the Trinity Agent.

Walks Agent/src/ for .cpp and .hpp files and reports any string literal that
is not wrapped in xorstr_(). Strings longer than 6 characters are always
checked. Short strings (<= 6 chars) are exempt UNLESS they appear on the
PARANOID_BLACKLIST (e.g. "cmd", "http", "tcp", "c2") — these are strong
indicators in a C2 binary even at short length.

Exit codes:
    0 — all clear
    1 — unencrypted string literals found
"""

import os
import re
import sys
from pathlib import Path

MIN_LENGTH = 6  # strings of this length or shorter are exempt (unless blacklisted)

# Paranoid blacklist: short strings that are still strong indicators in a C2/malware
# binary and must be encrypted even though they are below MIN_LENGTH.
# A defender grepping .rdata for any of these gets an immediate signal.
PARANOID_BLACKLIST = frozenset({
    # --- Shell / command execution ---
    "cmd", "shell", "bind", "rev",
    # --- Script / dropper extensions ---
    "bat", "ps1", "vbs", "wsf", "hta", "msi",
    # --- PE / executable ---
    "exe", "dll", "bin", "pe", "nt", "w32",
    # --- Protocol / transport ---
    "http", "https", "tcp", "udp", "dns",
    # --- Crypto ---
    "aes", "sha", "md5", "ssl", "tls", "jwt",
    # --- Windows API / subsystem ---
    "reg", "com", "rpc", "ipc", "svc", "sys",
    "win", "net", "tmp", "ini", "log",
    # --- C2 / malware terminology ---
    "c2", "bot", "mal", "exp", "msf", "meter", "havoc",
    # --- Architecture ---
    "x86", "arm", "64", "32",
    # --- Data / config ---
    "api", "url", "uri", "xml", "json", "csv",
    # --- Image (dropper resources) ---
    "png", "jpg", "gif", "bmp", "ico", "cur",
    # --- Misc strong indicators ---
    "key", "val", "cls",
})

# String prefixes that are exempt (byte strings, wide/unicode literals)
EXEMPT_PREFIXES = (b'\'', 'L"', 'u"', 'U"', "u8\"")


def find_string_literals(line: str) -> list[tuple[int, str]]:
    """
    Extract all string literals from a single line of C++ code.
    Returns a list of (start_col, literal_text) tuples.
    Handles: escaped quotes, raw strings, regular strings.
    Does NOT handle multi-line strings (none expected in this codebase).
    """
    literals = []
    i = 0
    n = len(line)

    while i < n:
        ch = line[i]

        # Skip single-quoted char literals
        if ch == "'":
            i += 1
            while i < n and line[i] != "'":
                if line[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue

        # Check for string literal start
        if ch == '"':
            # Check for prefixes: L, u, U, u8, r, R, etc.
            prefix_start = i
            prefix = ""

            # Look back for prefix characters
            j = i - 1
            while j >= 0 and (line[j].isalpha() or line[j] == "8"):
                prefix = line[j] + prefix
                j -= 1

            # Determine the full prefix (r"R", u8"R", etc.)
            full_prefix = prefix

            # Check if it's a raw string: r"delim...delim" or rR"delim...delim"
            if prefix.startswith("r") or prefix.startswith("R"):
                # Find the opening delimiter
                k = i + 1
                delim = ""
                while k < n and line[k] not in ('"', "\n"):
                    delim += line[k]
                    k += 1
                # Now find the closing "delim"
                close_marker = '"' + delim + '"'
                close_pos = line.find(close_marker, k + 1)
                if close_pos == -1:
                    # Raw string continues on next line — skip to end of line
                    i = n
                else:
                    literal_text = line[i : close_pos + len(close_marker)]
                    literals.append((prefix_start, literal_text))
                    i = close_pos + len(close_marker)
                continue

            # Check if it's an exempt prefix (L, u, U, u8)
            if prefix in ("L", "u", "U", "u8", "r", "R", "u8r", "ru8"):
                # Still extract it so we can report it, but mark as exempt
                # Find end of string
                k = i + 1
                while k < n and line[k] != '"':
                    if line[k] == "\\":
                        k += 1
                    k += 1
                if k < n:
                    literal_text = line[i : k + 1]
                    literals.append((prefix_start, literal_text))
                    i = k + 1
                else:
                    i = n
                continue

            # Regular string literal — find the end
            k = i + 1
            while k < n and line[k] != '"':
                if line[k] == "\\":
                    k += 1
                k += 1
            if k < n:
                literal_text = line[i : k + 1]
                literals.append((prefix_start, literal_text))
                i = k + 1
            else:
                i = n
            continue

        i += 1

    return literals


def is_in_xorstr_call(line: str, col: int) -> bool:
    """
    Check if the string literal starting at `col` is an argument to a xorstr_() call.
    Handles nested parentheses (e.g. xorstr_(std::string("...")) is NOT a match,
    but xorstr_("...") is).
    """
    # Look backwards from `col` for "xorstr_(" pattern
    # We need to find "xorstr_" followed by optional whitespace, then "(",
    # then the string literal should be the next non-whitespace content.
    search_region = line[:col]

    # Find the last occurrence of xorstr_ before this position
    idx = search_region.rfind("xorstr_")
    if idx == -1:
        return False

    # After xorstr_, expect optional whitespace then (
    after = search_region[idx + len("xorstr_") :]
    after_stripped = after.lstrip()
    if not after_stripped.startswith("("):
        return False

    # Now verify that the string literal is the first argument
    # (i.e., between the ( and the string, there should only be whitespace
    #  or a string prefix like L, u, U, u8)
    between = after[len("(") :].rstrip()
    # Strip known string prefixes that may appear between ( and "
    stripped = between.strip()
    if stripped and stripped[0] in ("L", "u", "U"):
        stripped = stripped[1:]
        if stripped.startswith("8"):
            stripped = stripped[1:]
    if stripped.strip() != "":
        return False

    return True


def is_in_include_directive(line: str) -> bool:
    """Check if the line is a #include directive."""
    stripped = line.strip()
    return stripped.startswith("#include")


def is_in_angle_brackets(line: str, col: int) -> bool:
    """
    Check if the string literal at `col` is inside angle brackets (template argument).
    e.g. std::map<std::string, int> — the strings here are type names, not values.
    This is a heuristic: look for < before the string without a matching >.
    """
    # Simple heuristic: if there's a < before the string on this line
    # and no > between that < and the string, it's likely a template arg.
    prefix = line[:col]
    # Count unmatched < in the prefix
    depth = 0
    for ch in prefix:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
    return depth > 0


def extract_string_content(literal: str) -> str:
    """Extract the content between the quotes of a string literal."""
    # Find the first " and the last "
    first_quote = literal.find('"')
    last_quote = literal.rfind('"')
    if first_quote == -1 or last_quote == -1 or first_quote >= last_quote:
        return ""
    return literal[first_quote + 1 : last_quote]


def check_file(filepath: Path) -> list[tuple[int, str]]:
    """
    Check a single file for unencrypted string literals.
    Returns a list of (line_number, string_content) violations.
    """
    violations = []

    try:
        content = filepath.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"Warning: could not read {filepath}: {e}", file=sys.stderr)
        return violations

    lines = content.split("\n")

    # Track preprocessor conditional state (we still check inside #ifdef blocks)
    # Track comment state for multi-line comments
    in_block_comment = False

    for line_num, line in enumerate(lines, start=1):
        # Skip empty lines
        if not line.strip():
            continue

        # Handle multi-line block comments
        if in_block_comment:
            end_idx = line.find("*/")
            if end_idx == -1:
                continue
            line = line[end_idx + 2 :]
            in_block_comment = False

        # Check for start of block comment
        start_idx = line.find("/*")
        if start_idx != -1:
            end_idx = line.find("*/", start_idx + 2)
            if end_idx == -1:
                # Block comment continues on next line
                in_block_comment = True
                # Only check the part before the comment
                line = line[:start_idx]
            else:
                # Remove the comment from the line
                line = line[:start_idx] + line[end_idx + 2 :]

        # Remove single-line comments (naive — doesn't handle // inside strings)
        # Find // that's not inside a string
        comment_idx = _find_line_comment(line)
        if comment_idx != -1:
            line = line[:comment_idx]

        # Skip preprocessor directives
        if is_in_include_directive(line):
            continue

        # Find all string literals in this line
        literals = find_string_literals(line)

        for col, literal in literals:
            # Get the string content (between quotes)
            content_str = extract_string_content(literal)

            # Skip short strings (unless they're on the paranoid blacklist)
            if len(content_str) <= MIN_LENGTH:
                if content_str.lower() not in PARANOID_BLACKLIST:
                    continue

            # Skip exempt prefixes (wide, unicode, byte strings)
            prefix_char = literal[0]
            # Check the character before the opening quote
            quote_pos = literal.find('"')
            if quote_pos > 0:
                prefix = literal[:quote_pos]
                if prefix in ("L", "u", "U", "u8", "b"):
                    continue

            # Skip if inside xorstr_()
            if is_in_xorstr_call(line, col):
                continue

            # Skip if inside angle brackets (template args)
            if is_in_angle_brackets(line, col):
                continue

            # This is a violation
            violations.append((line_num, content_str))

    return violations


def _find_line_comment(line: str) -> int:
    """
    Find the position of a // line comment that is not inside a string literal.
    Returns -1 if no such comment exists.
    """
    i = 0
    n = len(line)
    while i < n - 1:
        if line[i] == '"':
            # Skip string literal
            i += 1
            while i < n and line[i] != '"':
                if line[i] == "\\":
                    i += 1
                i += 1
            i += 1
        elif line[i] == "'":
            # Skip char literal
            i += 1
            while i < n and line[i] != "'":
                if line[i] == "\\":
                    i += 1
                i += 1
            i += 1
        elif line[i] == "/" and line[i + 1] == "/":
            return i
        else:
            i += 1
    return -1


def main() -> int:
    """Main entry point. Walks src/ and checks for unencrypted strings."""
    # Determine the src/ directory relative to this script
    script_dir = Path(__file__).resolve().parent
    src_dir = script_dir / "src"

    if not src_dir.is_dir():
        print(f"Error: source directory not found: {src_dir}", file=sys.stderr)
        return 1

    # Collect all .cpp and .hpp files
    extensions = {".cpp", ".hpp", ".h"}
    files = []
    for root, _dirs, filenames in os.walk(src_dir):
        for filename in filenames:
            ext = os.path.splitext(filename)[1]
            if ext in extensions:
                files.append(Path(root) / filename)

    files.sort()

    total_violations = 0
    files_with_violations = 0

    for filepath in files:
        violations = check_file(filepath)
        if violations:
            files_with_violations += 1
            total_violations += len(violations)
            relative_path = filepath.relative_to(script_dir)
            for line_num, content_str in violations:
                # Truncate very long strings for display
                display = content_str if len(content_str) <= 60 else content_str[:57] + "..."
                print(f"{relative_path}:{line_num}: unencrypted string: \"{display}\"")

    if total_violations > 0:
        print(
            f"\n{total_violations} unencrypted string literal(s) found "
            f"in {files_with_violations} file(s).",
            file=sys.stderr,
        )
        print(
            f"Wrap them in xorstr_(\"...\") or shorten to <= {MIN_LENGTH} characters.",
            file=sys.stderr,
        )
        return 1

    print(f"OK — all string literals in {len(files)} file(s) are compliant.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
