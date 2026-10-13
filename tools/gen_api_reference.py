#!/usr/bin/env python3
"""Generate docs/API_REFERENCE.md from the public LambdaEngine2D headers.

Reads every public header (src/*/include/lam/** plus the app, backend and engine include roots)
and writes one Markdown reference of the public declarations and their doc comments. Standard
library only. Paths are resolved from the repository root, so it runs from anywhere:

    python3 tools/gen_api_reference.py            # write docs/API_REFERENCE.md
    python3 tools/gen_api_reference.py --check    # exit 1 if docs/API_REFERENCE.md is stale
    python3 tools/gen_api_reference.py --stdout   # print the reference instead of writing it

A doc comment is the run of `//` lines directly above a declaration plus a trailing `//` on the
same line; a `// ---- x ----` separator ends it. The first comment after `#pragma once` is the
header's own comment. Skipped: `detail` namespaces, private members, friends, static_assert,
out-of-line definitions, uncommented forward declarations, and macros named LAM_DETAIL_* or
ending in `_`.

Output order is fixed, so reruns are byte-identical. Parse problems go to stderr; --strict makes
them exit code 2.
"""

from __future__ import annotations

import argparse
import dataclasses
import re
import sys
from pathlib import Path
from typing import Optional

# --------------------------------------------------------------------------------------------
# Configuration

MODULE_ORDER = [
    "math", "core", "ecs", "app", "platform", "engine", "assets", "render", "physics",
    "input", "audio", "scene", "tilemap", "animation", "particles", "ui",
]

# Include roots that do not follow src/<module>/include. Paths are relative to the repo root.
SPECIAL_ROOTS = {
    "src/core/app/include": "app",
    "src/core/backend/include": "platform",
    "src/core/engine/include": "engine",
}

# CMake target of each module (cmake/LamHelpers.cmake: lam_add_module(NAME x) -> lam_x unless
# TARGET is given; app, platform and engine pass TARGET).
MODULE_TARGETS = {
    "math": "lam_math", "core": "lam_core", "ecs": "lam_ecs", "app": "lam_app",
    "platform": "lam_platform", "engine": "lam_engine", "assets": "lam_assets",
    "render": "lam_render", "physics": "lam_physics", "input": "lam_input",
    "audio": "lam_audio", "scene": "lam_scene", "tilemap": "lam_tilemap",
    "animation": "lam_animation", "particles": "lam_particles", "ui": "lam_ui",
}

OUTPUT = "docs/API_REFERENCE.md"
MAX_WIDTH = 100          # signature lines longer than this are wrapped at parameter commas
COMMENT_COLUMN = 64      # trailing comments in code blocks start at this column when they fit
MAX_INIT_CHARS = 60      # longer initialisers are shown as /* ... */
MAX_MACRO_BODY = 60      # longer (or multi-line) macro bodies are shown as /* ... */

REFLECT_MACROS = {
    "LAM_REFLECT", "LAM_REFLECT_FLAGS", "LAM_REFLECT_TAG", "LAM_REFLECT_STRUCT",
    "LAM_REFLECT_ENUM", "LAM_REFLECT_CUSTOM",
}

SPECIFIERS = {
    "inline", "static", "constexpr", "consteval", "constinit", "extern", "friend", "virtual",
    "explicit", "mutable", "thread_local",
}
# A '(' after one of these words is not a parameter list.
NON_PARAM_WORDS = {
    "decltype", "alignas", "noexcept", "sizeof", "alignof", "requires", "explicit",
    "__attribute__", "__declspec", "static_assert", "throw", "typeid",
}
TYPE_KEYS = {"class", "struct", "union", "enum"}


# --------------------------------------------------------------------------------------------
# Diagnostics

class Diagnostics:
    def __init__(self) -> None:
        self.messages: list[str] = []

    def warn(self, path: str, line: int, msg: str) -> None:
        self.messages.append(f"{path}:{line}: warning: {msg}")


DIAG = Diagnostics()


# --------------------------------------------------------------------------------------------
# Tokenizer

@dataclasses.dataclass
class Token:
    kind: str        # ident, number, string, char, punct, pp, comment
    text: str
    start: int       # offset of the first character
    end: int         # offset after the last character
    line: int        # 1-based line of the first character
    end_line: int    # 1-based line of the last character
    trailing: bool = False   # comments only: code precedes it on the same line


PUNCT_RE = re.compile(
    r"::|->\*|->|\.\.\.|<=>|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|[-+*/%&|^]=|##|.",
    re.S,
)
TOKEN_RES = [
    ("rawstring", re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\((?:.|\n)*?\)\1"')),
    ("string", re.compile(r'(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"')),
    ("char", re.compile(r"(?:u8|u|U|L)?'(?:\\.|[^'\\\n])*'")),
    ("number", re.compile(r"\.?\d(?:[eEpP][+-]|[\w.']|)*")),
    ("ident", re.compile(r"[A-Za-z_]\w*")),
]


def tokenize(src: str, path: str) -> list[Token]:
    toks: list[Token] = []
    i, n, line = 0, len(src), 1
    line_has_code = False      # a non-comment token already started on this line
    at_line_start = True       # only whitespace since the last newline
    while i < n:
        c = src[i]
        if c == "\n":
            i += 1
            line += 1
            line_has_code = False
            at_line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if c == "\\" and i + 1 < n and src[i + 1] == "\n":   # stray line splice
            i += 2
            line += 1
            continue
        if at_line_start and c == "#":
            j = i
            while True:
                k = src.find("\n", j)
                if k < 0:
                    k = n
                if k > 0 and src[k - 1] == "\\" and k < n:
                    j = k + 1
                    continue
                break
            text = src[i:k]
            nl = text.count("\n")
            toks.append(Token("pp", text, i, k, line, line + nl))
            line += nl
            i = k
            line_has_code = True
            at_line_start = False
            continue
        at_line_start = False
        if src.startswith("//", i):
            k = src.find("\n", i)
            if k < 0:
                k = n
            toks.append(Token("comment", src[i:k], i, k, line, line, trailing=line_has_code))
            i = k
            continue
        if src.startswith("/*", i):
            k = src.find("*/", i + 2)
            if k < 0:
                DIAG.warn(path, line, "unterminated block comment")
                k = n - 2
            text = src[i:k + 2]
            nl = text.count("\n")
            toks.append(Token("comment", text, i, k + 2, line, line + nl, trailing=line_has_code))
            line += nl
            i = k + 2
            continue
        for kind, rx in TOKEN_RES:
            m = rx.match(src, i)
            if m:
                text = m.group(0)
                if kind == "rawstring":
                    kind = "string"
                nl = text.count("\n")
                toks.append(Token(kind, text, i, m.end(), line, line + nl))
                line += nl
                i = m.end()
                break
        else:
            m = PUNCT_RE.match(src, i)
            text = m.group(0)
            toks.append(Token("punct", text, i, m.end(), line, line))
            i = m.end()
        line_has_code = True
    return toks


# --------------------------------------------------------------------------------------------
# Data model

@dataclasses.dataclass
class Decl:
    kind: str                      # class struct union enum function variable alias concept
                                   # forward macro reflect using
    name: str
    namespace: str                 # "lam", "lam::literals", "" (global)
    line: int
    doc: list[str] = dataclasses.field(default_factory=list)       # raw comment text lines
    trailing: list[str] = dataclasses.field(default_factory=list)  # trailing comment texts
    sig: str = ""                  # one-line signature (class head for types; no ';')
    access: str = "public"
    members: list["Decl"] = dataclasses.field(default_factory=list)
    verbatim: list[str] = dataclasses.field(default_factory=list)  # enum source lines
    declarators: str = ""          # text between a class body's '}' and ';'
    end_trailing: list[str] = dataclasses.field(default_factory=list)  # comment after '} x;'
    qualname: str = ""             # namespace::Outer::Name
    is_friend: bool = False
    out_of_line: bool = False      # Foo::bar definition after the class
    has_body: bool = False         # class/enum defined here (not a forward declaration)
    param_span: Optional[tuple[int, int]] = None   # char offsets of '(' and ')' in sig
    # macros
    cond: str = ""                 # enclosing #if condition, "" if unconditional
    alt: list[str] = dataclasses.field(default_factory=list)  # other-branch definitions
    # reflection
    reflect_macro: str = ""
    reflect_args: list[str] = dataclasses.field(default_factory=list)

    @property
    def is_type(self) -> bool:
        return self.kind in TYPE_KEYS and self.has_body


# --------------------------------------------------------------------------------------------
# Comment handling

SEPARATOR_RE = re.compile(r"^\s*(?:[-=*/#~_]{3,}.*|.*[-=]{4,}\s*)$")


def comment_text(tok: Token) -> list[str]:
    """Raw text lines of a comment token without the // or /* */ markers."""
    t = tok.text
    if t.startswith("//"):
        return [t[2:].rstrip()]
    body = t[2:-2]
    lines = []
    for ln in body.split("\n"):
        ln = ln.rstrip()
        s = ln.lstrip()
        if s.startswith("*"):
            ln = " " + s[1:].lstrip() if s[1:2] == " " or len(s) == 1 else s[1:]
        lines.append(ln)
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def is_separator(text: str) -> bool:
    return bool(SEPARATOR_RE.match(text))


class Source:
    """One header: tokens plus per-line comment information."""

    def __init__(self, path: str, src: str):
        self.path = path
        self.src = src
        all_toks = tokenize(src, path)
        self.comments = [t for t in all_toks if t.kind == "comment"]
        self.toks = [t for t in all_toks if t.kind != "comment"]
        self.code_lines: set[int] = set()
        for t in self.toks:
            self.code_lines.update(range(t.line, t.end_line + 1))
        # line -> comment that occupies the whole line (comment-only lines)
        self.line_comment: dict[int, Token] = {}
        for c in self.comments:
            if not c.trailing and c.line == c.end_line and c.line not in self.code_lines:
                self.line_comment[c.line] = c
            elif not c.trailing and c.line not in self.code_lines:
                # multi-line block comment: index by its last line
                self.line_comment[c.end_line] = c
        self.consumed_lines: set[int] = set()   # module comment lines
        self.line_starts = [0]
        for m in re.finditer("\n", src):
            self.line_starts.append(m.end())

    def line_end(self, line: int) -> int:
        """Offset of the newline that ends 1-based `line` (or the end of the file)."""
        if line < len(self.line_starts):
            return self.line_starts[line] - 1
        return len(self.src)

    def column(self, offset: int) -> int:
        lo, hi = 0, len(self.line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.line_starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return offset - self.line_starts[lo]

    def doc_above(self, line: int) -> list[str]:
        """Comment lines directly above `line` (no blank line, stops at a separator)."""
        out: list[list[str]] = []
        ln = line - 1
        while ln > 0 and ln in self.line_comment and ln not in self.consumed_lines:
            c = self.line_comment[ln]
            texts = comment_text(c)
            if any(is_separator(t) for t in texts):
                break
            out.append(texts)
            ln = c.line - 1
        out.reverse()
        return [t for block in out for t in block]

    def trailing_between(self, start: int, end: int) -> list[str]:
        """Trailing comments whose offset lies in [start, end)."""
        res = []
        for c in self.comments:
            if c.trailing and start <= c.start < end:
                res.extend(t.strip() for t in comment_text(c))
        return [t for t in res if t and not is_separator(t)]

    def trailing_after(self, tok_index: int) -> list[str]:
        """Trailing comments on the same line right after token tok_index (no code between)."""
        t = self.toks[tok_index]
        limit = len(self.src)
        if tok_index + 1 < len(self.toks):
            nxt = self.toks[tok_index + 1]
            if nxt.line == t.end_line:
                limit = nxt.start
        res = []
        for c in self.comments:
            if c.trailing and c.line == t.end_line and t.end <= c.start < limit:
                res.extend(x.strip() for x in comment_text(c))
        return [x for x in res if x and not is_separator(x)]

    def module_comment(self) -> list[str]:
        """First comment block after #pragma once (or at the top of the file)."""
        first_code = None
        for t in self.toks:
            if t.kind == "pp" and re.match(r"#\s*pragma\s+once", t.text):
                continue
            first_code = t
            break
        pragma_line = 0
        for t in self.toks:
            if t.kind == "pp" and re.match(r"#\s*pragma\s+once", t.text):
                pragma_line = t.end_line
                break
        lines: list[str] = []
        ln = pragma_line + 1
        limit = first_code.line if first_code else 10 ** 9
        # skip blank lines between #pragma once and the comment
        while ln < limit and ln not in self.line_comment and ln not in self.code_lines:
            if ln > len(self.line_starts):
                break
            ln += 1
        while ln < limit and ln in self.line_comment:
            c = self.line_comment[ln]
            lines.extend(comment_text(c))
            self.consumed_lines.update(range(c.line, c.end_line + 1))
            ln = c.end_line + 1
        return lines


# --------------------------------------------------------------------------------------------
# Parser

OPEN = {"(": ")", "[": "]", "{": "}"}


class Parser:
    def __init__(self, source: Source):
        self.s = source
        self.toks = source.toks
        self.n = len(self.toks)
        self.cond_stack: list[list] = []   # [condition text, branch] per open #if
        self.macros: dict[str, Decl] = {}
        self.namespaces: list[str] = []
        self.file_out: list[Decl] = []

    # ---- helpers ---------------------------------------------------------------------------
    def warn(self, i: int, msg: str) -> None:
        line = self.toks[i].line if i < self.n else (self.toks[-1].end_line if self.toks else 0)
        DIAG.warn(self.s.path, line, msg)

    def text(self, i: int) -> str:
        return self.toks[i].text if 0 <= i < self.n else ""

    def match(self, i: int) -> int:
        """Index of the bracket that closes the bracket at i (round, square or curly)."""
        stack = [OPEN[self.toks[i].text]]
        j = i + 1
        while j < self.n:
            t = self.toks[j].text
            if self.toks[j].kind == "punct":
                if t in OPEN:
                    stack.append(OPEN[t])
                elif t in (")", "]", "}"):
                    if t != stack[-1]:
                        self.warn(j, f"mismatched '{t}' (expected '{stack[-1]}')")
                    stack.pop()
                    if not stack:
                        return j
            j += 1
        self.warn(i, f"unbalanced '{self.toks[i].text}'")
        return self.n - 1

    def skip_angles(self, i: int) -> int:
        """i is at '<'; returns the index after the matching '>'."""
        depth = 0
        j = i
        while j < self.n:
            t = self.toks[j]
            if t.kind == "punct":
                if t.text == "<":
                    depth += 1
                elif t.text == ">":
                    depth -= 1
                elif t.text == ">>":
                    depth -= 2
                elif t.text in ("(", "[", "{"):
                    j = self.match(j)
                elif t.text == ";":
                    self.warn(i, "unterminated template argument list")
                    return j
                if depth <= 0:
                    return j + 1
            j += 1
        return j

    def join(self, a: int, b: int) -> tuple[str, list[int]]:
        """Source text of tokens [a, b) on one line; returns (text, char offset per token)."""
        parts: list[str] = []
        offs: list[int] = []
        pos = 0
        for k in range(a, b):
            t = self.toks[k]
            if k > a and t.start > self.toks[k - 1].end:
                parts.append(" ")
                pos += 1
            offs.append(pos)
            txt = t.text
            if "\n" in txt:   # multi-line string literal or pp: flatten
                txt = re.sub(r"\s*\n\s*", " ", txt)
            parts.append(txt)
            pos += len(txt)
        return "".join(parts), offs

    def doc_for(self, i: int) -> list[str]:
        # Only a declaration that starts its line owns the comment lines above it.
        if i > 0 and self.toks[i - 1].end_line == self.toks[i].line:
            return []
        return self.s.doc_above(self.toks[i].line)

    # ---- preprocessor ----------------------------------------------------------------------
    def handle_pp(self, i: int, ns: str, out: list[Decl]) -> None:
        t = self.toks[i]
        text = t.text
        m = re.match(r"#\s*(\w+)\s*(.*)", text, re.S)
        if not m:
            return
        directive, rest = m.group(1), m.group(2)
        rest_flat = re.sub(r"\s*\\\n\s*", " ", rest).strip()
        rest_flat = re.sub(r"\s*//.*$", "", rest_flat)
        if directive in ("if", "ifdef", "ifndef"):
            cond = rest_flat
            if directive == "ifdef":
                cond = f"defined({rest_flat})"
            elif directive == "ifndef":
                cond = f"!defined({rest_flat})"
            self.cond_stack.append([cond, "if"])
        elif directive == "elif":
            if self.cond_stack:
                self.cond_stack[-1] = [rest_flat, "elif"]
        elif directive == "else":
            if self.cond_stack:
                self.cond_stack[-1][1] = "else"
        elif directive == "endif":
            if self.cond_stack:
                self.cond_stack.pop()
            else:
                self.warn(i, "#endif without #if")
        elif directive == "define":
            self.handle_define(i, rest, ns, self.file_out)

    def handle_define(self, i: int, rest: str, ns: str, out: list[Decl]) -> None:
        t = self.toks[i]
        m = re.match(r"([A-Za-z_]\w*)(\([^)]*\))?(.*)", rest, re.S)
        if not m:
            self.warn(i, "unparsed #define")
            return
        name, params, body = m.group(1), m.group(2) or "", m.group(3)
        if re.search(r"_DETAIL_|^LAM_DETAIL|_$", name) or name.endswith("_H") \
                or name.endswith("_HPP"):
            return
        # Trailing // comment on the last line (outside string literals).
        last = body.split("\n")[-1]
        trailing = []
        cm = re.search(r'//(.*)$', last)
        if cm and last[:cm.start()].count('"') % 2 == 0:
            trailing = [cm.group(1).strip()]
            body = body[: len(body) - len(last) + cm.start()]
        body_flat = re.sub(r"\s*\\\n\s*", " ", body).strip()
        multiline = "\\\n" in body
        params = re.sub(r"\s+", " ", params)
        if not body_flat:
            shown = ""
        elif multiline or len(body_flat) > MAX_MACRO_BODY:
            shown = " /* ... */"
        else:
            shown = " " + body_flat
        sig = f"#define {name}{params}{shown}"
        cond = ""
        branch = "if"
        if self.cond_stack:
            cond = " && ".join(f"({c})" if len(self.cond_stack) > 1 else c
                               for c, _ in self.cond_stack)
            branch = self.cond_stack[-1][1]
        if name in self.macros:
            first = self.macros[name]
            prefix = {"else": "#else", "elif": "#elif"}.get(branch, "#if " + cond)
            first.alt.append(f"{prefix}\n{sig}")
            return
        d = Decl("macro", name, "", t.line, doc=self.s.doc_above(t.line), trailing=trailing,
                 sig=sig, cond=cond if branch == "if" else (f"!({cond})" if cond else ""))
        d.qualname = name
        self.macros[name] = d
        out.append(d)

    # ---- scopes ----------------------------------------------------------------------------
    def parse_file(self) -> list[Decl]:
        out: list[Decl] = []
        self.file_out = out
        i = self.parse_scope(0, "", None, "public", out)
        while i < self.n:
            self.warn(i, f"unexpected '{self.text(i)}' at file scope")
            i = self.parse_scope(i + 1, "", None, "public", out)
        if self.cond_stack:
            DIAG.warn(self.s.path, self.toks[-1].line if self.toks else 0, "unterminated #if")
        return out

    def parse_scope(self, i: int, ns: str, owner: Optional[Decl], access: str,
                    out: list[Decl]) -> int:
        """Parses declarations until the '}' that closes this scope; returns its index."""
        in_class = owner is not None
        while i < self.n:
            t = self.toks[i]
            tx = t.text
            if t.kind == "pp":
                self.handle_pp(i, ns, out)
                i += 1
                continue
            if tx == "}":
                return i
            if tx == ";":
                i += 1
                continue
            if in_class and tx in ("public", "private", "protected") and self.text(i + 1) == ":":
                access = tx
                i += 2
                continue
            if not in_class and (tx == "namespace" or
                                 (tx == "inline" and self.text(i + 1) == "namespace")):
                i = self.parse_namespace(i, ns, out)
                continue
            if not in_class and tx == "extern" and self.toks[i + 1].kind == "string" \
                    and self.text(i + 2) == "{":
                close = self.match(i + 2)
                self.parse_scope(i + 3, ns, None, "public", out)
                i = close + 1
                continue
            if t.kind == "ident" and re.fullmatch(r"LAM_[A-Z0-9_]+", tx) \
                    and self.text(i + 1) == "(":
                i = self.parse_macro_call(i, ns, out)
                continue
            d, i = self.parse_decl(i, ns, owner)
            if d is not None:
                d.access = access
                out.append(d)
        return i

    def parse_namespace(self, i: int, ns: str, out: list[Decl]) -> int:
        j = i + 1 if self.text(i) == "namespace" else i + 2
        name_parts = []
        while self.text(j) not in ("{", "=", ";") and j < self.n:
            name_parts.append(self.text(j))
            j += 1
        name = "".join(p for p in name_parts if p != "inline")
        if self.text(j) == "=":     # namespace alias
            k = j
            while k < self.n and self.text(k) != ";":
                k += 1
            sig, _ = self.join(i, k)
            d = Decl("alias", name, ns, self.toks[i].line, doc=self.doc_for(i),
                     trailing=self.s.trailing_after(k), sig=sig)
            out.append(d)
            return k + 1
        if self.text(j) != "{":
            self.warn(i, "unparsed namespace")
            return j + 1
        inner = ns + "::" + name if ns and name else (name or ns + "::(anonymous)")
        if name and inner not in self.namespaces:
            self.namespaces.append(inner)
        close = self.parse_scope(j + 1, inner, None, "public", out)
        if self.text(close) != "}":
            self.warn(i, f"namespace {name} not closed")
        return close + 1

    def parse_macro_call(self, i: int, ns: str, out: list[Decl]) -> int:
        name = self.text(i)
        close = self.match(i + 1)
        end = close + 1
        if self.text(end) == ";":
            end += 1
        if name not in REFLECT_MACROS:
            self.warn(i, f"macro call {name}(...) at declaration scope (skipped)")
            return end
        args = self.split_args(i + 2, close)
        sig, offs = self.join(i, close + 1)
        d = Decl("reflect", args[0] if args else "", ns, self.toks[i].line, doc=self.doc_for(i),
                 trailing=self.s.trailing_after(end - 1), sig=sig,
                 param_span=(offs[1], offs[close - i]))
        d.reflect_macro = name
        d.reflect_args = args
        d.qualname = args[0].lstrip(":") if args else ""
        out.append(d)
        return end

    def split_args(self, a: int, b: int) -> list[str]:
        """Comma-separated arguments of tokens [a, b) (respects brackets and template args)."""
        args, start, j, angle = [], a, a, 0
        while j < b:
            t = self.toks[j]
            if t.text in OPEN:
                j = self.match(j) + 1
                continue
            if t.text == "<" and j > a and self.toks[j - 1].kind == "ident":
                angle += 1
            elif t.text == ">" and angle:
                angle -= 1
            elif t.text == ">>" and angle:
                angle = max(0, angle - 2)
            elif t.text == "," and angle == 0:
                args.append(self.join(start, j)[0])
                start = j + 1
            j += 1
        if start < b:
            args.append(self.join(start, b)[0])
        return args

    # ---- declarations ----------------------------------------------------------------------
    def parse_decl(self, i: int, ns: str, owner: Optional[Decl]) -> tuple[Optional[Decl], int]:
        start = i
        j = i
        while self.text(j) == "template" and self.text(j + 1) == "<":
            j = self.skip_angles(j + 1)
        k = j
        is_friend = False
        while k < self.n:
            tx = self.text(k)
            if tx == "[" and self.text(k + 1) == "[":
                k = self.match(k) + 1
                continue
            if tx == "alignas" and self.text(k + 1) == "(":
                k = self.match(k + 1) + 1
                continue
            if tx == "requires" and k == j:      # requires-clause after the template header
                k = self.skip_requires(k)
                continue
            if tx in SPECIFIERS:
                is_friend = is_friend or tx == "friend"
                k += 1
                continue
            break
        head = self.text(k)
        if head == "template" and self.text(k + 1) != "<":
            # explicit instantiation: skip
            return None, self.skip_to_semicolon(k)
        if head in TYPE_KEYS:
            res = self.parse_type(start, k, ns, owner, is_friend)
            if res is not None:
                return res
        if head == "static_assert":
            return None, self.skip_to_semicolon(k)
        if head == "using" or head == "typedef":
            return self.parse_using(start, k, ns, owner)
        if head == "concept" or (self.text(k + 1) == "concept"):
            end = self.skip_to_semicolon(k)
            sig, _ = self.join(start, end - 1)
            name = self.text(k + 1) if head == "concept" else self.text(k + 2)
            d = Decl("concept", name, ns, self.toks[start].line, doc=self.doc_for(start),
                     trailing=self.s.trailing_after(end - 1), sig=sig)
            d.qualname = self.qualify(ns, owner, name)
            return d, end
        return self.parse_general(start, k, ns, owner, is_friend)

    def skip_requires(self, k: int) -> int:
        """k at 'requires' after a template header; returns the index after the clause."""
        j = k + 1
        while j < self.n:
            tx = self.text(j)
            if tx == "(":
                j = self.match(j) + 1
            elif self.toks[j].kind == "ident" or tx in ("::", "&&", "||", "!"):
                j += 1
                if tx not in ("&&", "||", "!", "::") and self.text(j) not in (
                        "::", "<", "&&", "||"):
                    # an identifier not followed by more of the constraint ends it
                    if self.text(j) == "(" or self.toks[j].kind == "ident":
                        return j
                if self.text(j) == "<":
                    j = self.skip_angles(j)
            else:
                return j
        return j

    def skip_to_semicolon(self, j: int) -> int:
        """Returns the index after the ';' that ends the declaration starting at j."""
        while j < self.n:
            tx = self.text(j)
            if tx in OPEN:
                j = self.match(j) + 1
                continue
            if tx == ";":
                return j + 1
            if tx == "}":
                self.warn(j, "unexpected '}' while looking for ';'")
                return j
            j += 1
        return j

    def qualify(self, ns: str, owner: Optional[Decl], name: str) -> str:
        base = owner.qualname if owner is not None else ns
        return f"{base}::{name}" if base else name

    def parse_using(self, start: int, k: int, ns: str, owner: Optional[Decl]):
        end = self.skip_to_semicolon(k)
        semi = end - 1
        if self.text(k + 1) == "namespace":
            return None, end
        sig, _ = self.join(start, semi)
        if self.text(k) == "typedef":
            name = ""
            for q in range(semi - 1, k, -1):
                if self.toks[q].kind == "ident":
                    name = self.toks[q].text
                    break
            kind = "alias"
        elif self.text(k + 2) == "=":
            name, kind = self.text(k + 1), "alias"
        else:
            name, kind = self.text(semi - 1), "using"
        d = Decl(kind, name, ns, self.toks[start].line, doc=self.doc_for(start),
                 trailing=self.s.trailing_after(semi), sig=sig)
        d.qualname = self.qualify(ns, owner, name)
        return d, end

    def parse_type(self, start: int, k: int, ns: str, owner: Optional[Decl], is_friend: bool):
        key = self.text(k)
        m = k + 1
        if key == "enum" and self.text(m) in ("class", "struct"):
            m += 1
        while True:
            if self.text(m) == "[" and self.text(m + 1) == "[":
                m = self.match(m) + 1
            elif self.text(m) == "alignas":
                m = self.match(m + 1) + 1
            else:
                break
        name_start = m
        if self.toks[m].kind == "ident" or self.text(m) == "::":
            while m < self.n:
                if self.text(m) == "::" or self.toks[m].kind == "ident":
                    if self.text(m) == "final" and m > name_start:
                        break
                    m += 1
                elif self.text(m) == "<":
                    m = self.skip_angles(m)
                else:
                    break
        name, _ = self.join(name_start, m)
        name = name.replace(" ", "") if "<" not in name else name
        if self.text(m) == "final":
            m += 1
        nxt = self.text(m)
        if nxt == ":" and key != "enum":
            q = m
            while q < self.n and self.text(q) not in ("{", ";"):
                if self.text(q) == "<":
                    q = self.skip_angles(q)
                    continue
                if self.text(q) == "(":
                    q = self.match(q) + 1
                    continue
                q += 1
            m = q
            nxt = self.text(m)
        elif nxt == ":" and key == "enum":
            q = m
            while q < self.n and self.text(q) not in ("{", ";"):
                q += 1
            m = q
            nxt = self.text(m)
        if nxt == ";":
            sig, _ = self.join(start, m)
            d = Decl("forward", name, ns, self.toks[start].line, doc=self.doc_for(start),
                     trailing=self.s.trailing_after(m), sig=sig, is_friend=is_friend)
            d.qualname = self.qualify(ns, owner, name)
            return d, m + 1
        if nxt != "{":
            return None    # elaborated type specifier inside another declaration
        brace = m
        close = self.match(brace)
        # declarators after the body, up to ';'
        q = close + 1
        while q < self.n and self.text(q) != ";":
            if self.text(q) in OPEN:
                q = self.match(q) + 1
                continue
            q += 1
        semi = q
        declarators = self.join(close + 1, semi)[0] if semi > close + 1 else ""
        head, _ = self.join(start, brace)
        first_member = self.toks[brace + 1] if brace + 1 < self.n else None
        head_end = first_member.start if first_member and \
            first_member.line == self.toks[brace].line else self.s.line_end(self.toks[brace].line)
        trailing = self.s.trailing_between(self.toks[start].start, self.toks[brace].start)
        trailing += [c for c in self.s.trailing_between(self.toks[brace].end, head_end)
                     if c not in trailing]
        end_trailing = [c for c in self.s.trailing_after(min(semi, self.n - 1))
                        if c not in trailing]
        if not declarators.strip():
            trailing += end_trailing
            end_trailing = []
        if self.text(semi) != ";":
            self.warn(start, f"{key} {name}: missing ';' after the body")
        d = Decl(key, name, ns, self.toks[start].line, doc=self.doc_for(start),
                 trailing=trailing, sig=head, is_friend=is_friend, has_body=True,
                 declarators=declarators, end_trailing=end_trailing)
        if "::" in name.split("<")[0] and owner is None:
            d.qualname = (ns + "::" if ns and not name.startswith(ns) else "") + name
        else:
            d.qualname = self.qualify(ns, owner, name or "(anonymous)")
        if key == "enum":
            d.verbatim = self.verbatim_lines(start, semi)
            return d, semi + 1
        default_access = "private" if key == "class" else "public"
        end = self.parse_scope(brace + 1, ns, d, default_access, d.members)
        if end != close:
            self.warn(brace, f"{key} {name}: body parse ended at line "
                             f"{self.toks[end].line}, expected line {self.toks[close].line}")
        return d, semi + 1

    def verbatim_lines(self, a: int, semi: int) -> list[str]:
        """Source lines of tokens [a, semi], dedented to the first token's column."""
        start = self.toks[a].start
        end = self.toks[semi].end if semi < self.n else self.toks[-1].end
        # include trailing comments on the last line
        tail = self.s.trailing_after(semi) if semi < self.n else []
        text = self.s.src[start:end]
        col = self.s.column(start)
        lines = text.split("\n")
        out = [lines[0].rstrip()]
        for ln in lines[1:]:
            strip = min(col, len(ln) - len(ln.lstrip(" ")))
            out.append(ln[strip:].rstrip())
        if tail:
            out[-1] = out[-1] + "  // " + " ".join(tail)
        return out

    def parse_general(self, start: int, k: int, ns: str, owner: Optional[Decl],
                      is_friend: bool):
        j = k
        angle = 0
        after_eq = False
        eq_index = None
        brace_init = None
        is_func = False
        funcptr = False
        params = None            # (open, close) token indices
        ctor_init = False
        sig_end = None
        body = None              # (open, close)
        last_ident = None
        funcptr_name = None
        end = None
        while j < self.n:
            t = self.toks[j]
            tx = t.text
            prev = self.toks[j - 1] if j > start else None
            if t.kind == "pp":
                self.warn(j, "preprocessor directive inside a declaration")
                j += 1
                continue
            if tx == ";":
                end = j
                break
            if tx == "}":
                self.warn(j, "unexpected '}' inside a declaration")
                end = j
                break
            if tx in ("(", "["):
                close = self.match(j)
                if (tx == "(" and not after_eq and angle == 0 and not is_func and not funcptr
                        and not (prev is not None and prev.text in NON_PARAM_WORDS)
                        and not (prev is not None and prev.text == "operator")):
                    inner = self.text(j + 1)
                    if inner in ("*", "&", "&&", "^") or self.is_member_pointer(j + 1, close):
                        funcptr = True
                        for x in range(close - 1, j, -1):
                            if self.toks[x].kind == "ident":
                                funcptr_name = self.toks[x].text
                                break
                    else:
                        is_func = True
                        params = (j, close)
                j = close + 1
                continue
            if tx == "{":
                if after_eq or not is_func:
                    if not after_eq and brace_init is None:
                        brace_init = j
                    j = self.match(j) + 1
                    continue
                if ctor_init and prev is not None and prev.text not in (")", "}", "..."):
                    j = self.match(j) + 1        # brace mem-initializer
                    continue
                body = (j, self.match(j))
                if sig_end is None:
                    sig_end = j
                end = body[1]
                while self.text(end + 1) == "catch" and self.text(end + 2) == "(":
                    handler = self.match(end + 2) + 1          # function-try-block handlers
                    end = self.match(handler) if self.text(handler) == "{" else handler
                break
            if tx == "try" and is_func and params and j > params[1] and sig_end is None:
                sig_end = j
            if not after_eq:
                if tx == "=" and angle == 0 and not (prev is not None and prev.text == "operator"):
                    after_eq = True
                    eq_index = j
                elif tx == ":" and is_func and params and j > params[1]:
                    ctor_init = True
                    if sig_end is None:
                        sig_end = j
                elif tx == "<" and prev is not None and prev.text != "operator" and (
                        prev.kind == "ident" or prev.text == ">"):
                    angle += 1
                elif tx == ">" and angle > 0 and not (prev is not None and prev.text == "operator"):
                    angle -= 1
                elif tx == ">>" and angle > 0:
                    angle = max(0, angle - 2)
                elif t.kind == "ident" and angle == 0 and not is_func and tx not in SPECIFIERS:
                    last_ident = tx
            j += 1
        if end is None:
            self.warn(start, "declaration not terminated")
            end = self.n - 1
        if sig_end is None:
            sig_end = end
        # Name
        name = ""
        out_of_line = False
        kind = "function" if is_func else "variable"
        if is_func:
            name, out_of_line = self.function_name(k, params[0])
        elif funcptr:
            name = funcptr_name or ""
        else:
            name = last_ident or ""
        # Signature with long initialisers elided
        init_start = None
        if not is_func and eq_index is not None:
            init_start = eq_index + 1
        elif not is_func and brace_init is not None:
            init_start = brace_init
        sig, offs = self.join(start, sig_end)
        if init_start is not None and init_start < sig_end:
            init_text, _ = self.join(init_start, sig_end)
            if len(init_text) > MAX_INIT_CHARS and not self.has_top_level_comma(init_start,
                                                                                sig_end):
                cut = offs[init_start - start]
                prefix = sig[:cut].rstrip()
                if self.text(init_start) == "{":
                    sig = prefix + ("{ /* ... */ }" if brace_init == init_start
                                    else " { /* ... */ }")
                else:
                    sig = prefix + " /* ... */"
        param_span = None
        if is_func and params and params[1] < sig_end:
            param_span = (offs[params[0] - start], offs[params[1] - start])
        trailing = self.s.trailing_between(self.toks[start].start, self.toks[sig_end].start
                                           if sig_end < self.n else 10 ** 12)
        if body is not None:
            o = body[0]
            nxt = self.toks[o + 1] if o + 1 < self.n else None
            lim = nxt.start if nxt is not None and nxt.line == self.toks[o].line else \
                self.s.line_end(self.toks[o].line)
            trailing += [c for c in self.s.trailing_between(self.toks[o].end, lim)
                         if c not in trailing]
        trailing += [c for c in self.s.trailing_after(end) if c not in trailing]
        d = Decl(kind, name, ns, self.toks[start].line, doc=self.doc_for(start),
                 trailing=trailing, sig=sig, is_friend=is_friend, out_of_line=out_of_line,
                 param_span=param_span)
        d.qualname = self.qualify(ns, owner, name)
        if not name:
            self.warn(start, f"could not find the name of: {sig[:80]}")
        return d, end + 1

    def has_top_level_comma(self, a: int, b: int) -> bool:
        """True if tokens [a, b) hold a ',' outside brackets (several declarators)."""
        j = a
        while j < b:
            if self.text(j) in OPEN:
                j = self.match(j) + 1
                continue
            if self.text(j) == ",":
                return True
            j += 1
        return False

    def is_member_pointer(self, a: int, b: int) -> bool:
        """True for tokens like `Class::*name` or `ns::Class::*name` in [a, b)."""
        q = a
        while q + 1 < b and self.toks[q].kind == "ident" and self.text(q + 1) == "::":
            q += 2
            if self.text(q) == "*":
                return True
        return False

    def function_name(self, k: int, popen: int) -> tuple[str, bool]:
        # operator?
        for q in range(k, popen):
            if self.text(q) == "operator":
                name = self.join(q, popen)[0]
                name = re.sub(r"operator\s+(?=[^\w\s])", "operator", name)
                qualified = q > k and self.text(q - 1) == "::"
                return name, qualified
        q = popen - 1
        if self.text(q) == ">":         # explicit specialisation f<T>(...)
            depth = 0
            while q > k:
                if self.text(q) == ">":
                    depth += 1
                elif self.text(q) == "<":
                    depth -= 1
                    if depth == 0:
                        q -= 1
                        break
                q -= 1
        name = self.text(q)
        if self.text(q - 1) == "~":
            name = "~" + name
            q -= 1
        qualified = q - 1 >= k and self.text(q - 1) == "::"
        return name, qualified


# --------------------------------------------------------------------------------------------
# Header model

@dataclasses.dataclass
class Header:
    path: str             # repo-relative path
    include: str          # lam/core/app.h
    module: str
    module_comment: list[str]
    decls: list[Decl]
    namespaces: list[str]


def discover(root: Path) -> list[tuple[Path, str, str]]:
    """(file, include path, module) for every public header."""
    found: dict[Path, tuple[str, str]] = {}
    for inc_root in sorted((root / "src").glob("*/include")):
        module = inc_root.parent.name
        for f in sorted(inc_root.glob("lam/**/*.h")) + sorted(inc_root.glob("lam/**/*.hpp")):
            found[f] = (f.relative_to(inc_root).as_posix(), module)
    for rel, module in SPECIAL_ROOTS.items():
        inc_root = root / rel
        for f in sorted(inc_root.glob("lam/**/*.h")) + sorted(inc_root.glob("lam/**/*.hpp")):
            found[f] = (f.relative_to(inc_root).as_posix(), module)
    return [(f, inc, mod) for f, (inc, mod) in found.items()]


def is_internal_ns(ns: str) -> bool:
    return any(part == "detail" or part.startswith("(anonymous") for part in ns.split("::"))


def load_header(root: Path, f: Path, include: str, module: str) -> Header:
    rel = f.relative_to(root).as_posix()
    src = f.read_text(encoding="utf-8")
    source = Source(rel, src)
    mc = source.module_comment()
    parser = Parser(source)
    decls = parser.parse_file()
    namespaces = [ns for ns in parser.namespaces if not is_internal_ns(ns)]
    return Header(rel, include, module, mc, decls, namespaces)


# --------------------------------------------------------------------------------------------
# Selection

def public_members(d: Decl) -> list[Decl]:
    """Public and protected members, without friends."""
    return [m for m in d.members if m.access != "private" and not m.is_friend]


def visible(d: Decl) -> bool:
    if d.kind == "macro":
        return True
    if d.kind == "reflect":
        return True
    if is_internal_ns(d.namespace) or d.is_friend or d.out_of_line:
        return False
    if d.kind == "forward":
        return bool(d.doc or d.trailing)
    if d.kind == "using":
        return False
    if d.kind in ("function", "variable") and "::" in d.name.split("<")[0]:
        return False
    if d.kind in TYPE_KEYS and d.name.startswith("detail::"):
        return False
    return True


# --------------------------------------------------------------------------------------------
# Rendering helpers

def md_escape_plain(s: str) -> str:
    s = re.sub(r"<(?=[A-Za-z/!?])", "&lt;", s)
    s = s.replace("*", r"\*")
    s = s.replace("~", r"\~")
    s = re.sub(r"(?<![A-Za-z0-9])_|_(?![A-Za-z0-9])", r"\\_", s)
    s = re.sub(r"\[(?=[^\]]*\]\()", r"\\[", s)
    return s


TEMPLATE_NAME_RE = re.compile(
    r"(?<![\w`])(?:[A-Za-z_][\w]*(?:::|\.))*[A-Za-z_]\w*"
    r"<[^<>`\n]{0,60}?(?:<[^<>`\n]{0,60}?>[^<>`\n]{0,60}?)*>(?:\(\))?")


QUOTED_SPECIAL_RE = re.compile(r'"[^"\s`]*[<>*_~|\[\]][^"\s`]*"')


def md_inline(text: str) -> str:
    # Wrap Grid<T>-style names and quoted strings like "audio.<bus>.volume" in code spans, so
    # Markdown doesn't eat the angle brackets.
    for rx in (QUOTED_SPECIAL_RE, TEMPLATE_NAME_RE):
        pieces = re.split(r"(`[^`\n]+`)", text)
        for idx, p in enumerate(pieces):
            if not (len(p) >= 2 and p.startswith("`") and p.endswith("`")):
                pieces[idx] = rx.sub(lambda m: f"`{m.group(0)}`", p)
        text = "".join(pieces)
    parts = re.split(r"(`[^`\n]+`)", text)
    out = []
    for p in parts:
        if len(p) >= 2 and p.startswith("`") and p.endswith("`"):
            out.append(p)
        else:
            out.append(md_escape_plain(p))
    res = "".join(out)
    # A paragraph must not start like a heading, quote or list item.
    res = re.sub(r"^(\s*)(#+(?=\s|$)|>|[-+](?=\s))", lambda m: m.group(1) + "\\" + m.group(2),
                 res)
    res = re.sub(r"^(\s*\d+)([.)])(?=\s)", lambda m: m.group(1) + "\\" + m.group(2), res)
    return res


def join_lines(lines: list[str]) -> str:
    """Joins wrapped comment lines; a line ending in '/' continues without a space."""
    text = ""
    for ln in (x.strip() for x in lines):
        if text and not (text.endswith("/") and ln[:1].isalpha()):
            text += " "
        text += ln
    return text


def render_doc(lines: list[str]) -> list[str]:
    """Doc comment lines -> Markdown paragraphs and code blocks."""
    out: list[str] = []
    para: list[str] = []
    code: list[str] = []

    def flush_para():
        if para:
            out.append(md_inline(join_lines(para)))
            out.append("")
            para.clear()

    def flush_code():
        if code:
            indent = min(len(c) - len(c.lstrip(" ")) for c in code if c.strip())
            out.append("```cpp")
            out.extend(c[indent:] for c in code)
            out.append("```")
            out.append("")
            code.clear()

    for raw in lines:
        text = raw[1:] if raw.startswith(" ") else raw
        if not text.strip():
            flush_para()
            flush_code()
            continue
        if text.startswith(" ") or text.startswith("\t"):
            flush_para()
            code.append(text.replace("\t", "    "))
        else:
            flush_code()
            para.append(text)
    flush_para()
    flush_code()
    while out and out[-1] == "":
        out.pop()
    return out


def template_header_end(sig: str) -> int:
    """End offset of a leading `template <...>` header (plus a `requires(...)` clause), or 0."""
    m = re.match(r"template\s*<", sig)
    if not m:
        return 0
    depth, paren, i = 1, 0, m.end()
    while i < len(sig) and depth:
        ch = sig[i]
        if ch in "([":
            paren += 1
        elif ch in ")]":
            paren -= 1
        elif paren == 0 and ch == "<":
            depth += 1
        elif paren == 0 and ch == ">":
            depth -= 1
        i += 1
    if depth:
        return 0
    m2 = re.match(r"\s*requires\s*\(", sig[i:])
    if m2:
        j, paren = i + m2.end(), 1
        while j < len(sig) and paren:
            paren += {"(": 1, ")": -1}.get(sig[j], 0)
            j += 1
        i = j
    return i if i < len(sig) else 0


def wrap_signature(sig: str, indent: str, param_span: Optional[tuple[int, int]]) -> list[str]:
    """Wraps a long signature at the top-level commas of its parameter list."""
    line = indent + sig
    if len(line) <= MAX_WIDTH:
        return [line]
    h_end = template_header_end(sig)
    if h_end:
        rest = sig[h_end:].lstrip()
        shift = len(sig) - len(rest)
        span = None
        if param_span is not None and param_span[0] >= shift:
            span = (param_span[0] - shift, param_span[1] - shift)
        return [indent + sig[:h_end].rstrip()] + wrap_signature(rest, indent, span)
    if param_span is None:
        return [line]
    p_open, p_close = param_span
    inner = sig[p_open + 1:p_close]
    params, depth, cur = [], 0, ""
    for idx, ch in enumerate(inner):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "<" and idx > 0 and (inner[idx - 1].isalnum() or inner[idx - 1] == "_"):
            depth += 1
        elif ch == ">" and depth > 0 and inner[idx - 1] != "-":
            depth -= 1
        if ch == "," and depth == 0:
            params.append(cur.strip() + ",")
            cur = ""
        else:
            cur += ch
    if cur.strip():
        params.append(cur.strip())
    if len(params) < 2:
        return [line]
    head = indent + sig[:p_open + 1]
    tail = sig[p_close:]
    cont = " " * len(head)
    if len(head) > 60:
        cont = indent + " " * 8
    lines = []
    cur_line = head
    first = True
    for idx, p in enumerate(params):
        piece = p + (tail if idx == len(params) - 1 else "")
        candidate = cur_line + ("" if first else " ") + piece
        if first or len(candidate) <= MAX_WIDTH:
            cur_line = candidate
        else:
            lines.append(cur_line)
            cur_line = cont + piece
        first = False
    lines.append(cur_line)
    return lines


def with_trailing(lines: list[str], trailing: list[str]) -> list[str]:
    if not trailing:
        return lines
    comment = "// " + " ".join(trailing)
    last = lines[-1]
    if len(last) < COMMENT_COLUMN - 1:
        lines[-1] = last.ljust(COMMENT_COLUMN) + comment
    elif len(last) + 2 + len(comment) <= MAX_WIDTH + 30:
        lines[-1] = last + "  " + comment
    else:
        indent = " " * (len(lines[0]) - len(lines[0].lstrip(" ")))
        lines.insert(0, indent + comment)
    return lines


def doc_comment_lines(doc: list[str], indent: str) -> list[str]:
    return [(indent + "//" + d).rstrip() for d in doc]


def render_member_block(d: Decl, indent: str) -> list[str]:
    """C++ lines for a declaration inside a code block (types recurse)."""
    out = doc_comment_lines(d.doc, indent)
    if d.kind == "enum" and d.has_body:
        out.extend(indent + v if v else v for v in d.verbatim)
        return out
    if d.kind in ("class", "struct", "union") and d.has_body:
        out.extend(render_type_body(d, indent))
        return out
    sig_lines = wrap_signature(d.sig + ";", indent, d.param_span)
    out.extend(with_trailing(sig_lines, d.trailing))
    return out


def render_type_body(d: Decl, indent: str) -> list[str]:
    members = public_members(d)
    decl_tail = (" " + d.declarators.strip() if d.declarators.strip() else "") + ";"
    if not d.members:                       # empty in the source: tag types
        return with_trailing([indent + d.sig.rstrip() + " {}" + decl_tail],
                             d.trailing + d.end_trailing)
    lines = with_trailing([indent + d.sig.rstrip() + " {"], d.trailing)
    if not members:
        lines.append(indent + "    // no public members")
        lines.extend(with_trailing([indent + "}" + decl_tail], d.end_trailing))
        return lines
    access = "private" if d.kind == "class" else "public"
    for m in members:
        if m.access != access:
            lines.append(indent + m.access + ":")
            access = m.access
        lines.extend(render_member_block(m, indent + "    "))
    lines.extend(with_trailing([indent + "}" + decl_tail], d.end_trailing))
    return lines


def ns_relative(name: str, ns: str) -> str:
    if ns == "lam" or not ns:
        return name
    if ns.startswith("lam::"):
        return ns[5:] + "::" + name
    return ns + "::" + name


def anchor(text: str, used: dict[str, int]) -> str:
    """GitHub-style heading anchor (lower case, punctuation removed, spaces to '-')."""
    a = text.strip().lower()
    a = re.sub(r"[^\w\- ]", "", a)
    a = a.replace(" ", "-")
    if a in used:
        used[a] += 1
        a = f"{a}-{used[a]}"
    else:
        used[a] = 0
    return a


def reflect_info(r: Decl) -> tuple[str, str, list[str]]:
    """(description, flags expression or "", fields or enum names) of a LAM_REFLECT* call."""
    macro, args = r.reflect_macro, r.reflect_args
    second = args[1] if len(args) > 1 else ""
    if macro == "LAM_REFLECT":
        return "component", "lam::kDefaultFlags", args[1:]
    if macro == "LAM_REFLECT_FLAGS":
        return "component", second, args[2:]
    if macro == "LAM_REFLECT_TAG":
        return "tag component", "kTag | kSerialize | kSnapshot | kHash", []
    if macro == "LAM_REFLECT_STRUCT":
        return "nested value type, not a component", "", args[1:]
    if macro == "LAM_REFLECT_ENUM":
        return "enum names", "", args[1:]
    return "component with custom lam_read/lam_write/lam_hash", second, []


def reflect_summary(r: Decl) -> str:
    kind, flags, fields = reflect_info(r)
    parts = [f"`{r.reflect_macro}`: {kind}"]
    if flags:
        parts.append(f"flags `{flags}`")
    if fields:
        label = "names" if r.reflect_macro == "LAM_REFLECT_ENUM" else "fields"
        parts.append(f"{label} " + ", ".join(f"`{f}`" for f in fields))
    return "; ".join(parts) + "."


# --------------------------------------------------------------------------------------------
# Document

def render(headers: list[Header]) -> str:
    used: dict[str, int] = {}
    out: list[str] = []
    title = "LambdaEngine2D API Reference"
    out.append(f"# {title}")
    anchor(title, used)
    out.append("")
    out.append("<!-- GENERATED FILE. Do not edit by hand: edit the header comments and "
               "regenerate. -->")
    out.append("")
    out.append("This file is generated by `tools/gen_api_reference.py` from the public headers "
               "(`src/*/include/lam/**`, `src/core/app/include`, `src/core/backend/include`, "
               "`src/core/engine/include`). Do not edit it by hand. To change a description, "
               "edit the `//` comment above the declaration in the header, then regenerate "
               "from the repository root:")
    out.append("")
    out.append("```sh")
    out.append("python3 tools/gen_api_reference.py          # rewrite docs/API_REFERENCE.md")
    out.append("python3 tools/gen_api_reference.py --check  # exit 1 if this file is out of date")
    out.append("```")
    out.append("")
    out.append("How to read it. Every header has its include line, its module comment, and its "
               "public declarations in file order. A type shows its description, then its public "
               "members in one code block (member comments are kept as `//` comments). A free "
               "function, alias or constant shows its signature, then its description. Function "
               "bodies, private members and `detail` namespaces are left out. Everything is in "
               "namespace `lam` unless a block says otherwise. When this file and a header "
               "disagree, the header wins.")
    out.append("")

    by_module: dict[str, list[Header]] = {}
    for h in headers:
        by_module.setdefault(h.module, []).append(h)
    modules = [m for m in MODULE_ORDER if m in by_module] + \
        sorted(m for m in by_module if m not in MODULE_ORDER)

    # Pre-compute anchors in document order so the TOC can link to them.
    toc_title = "Contents"
    module_anchor: dict[str, str] = {}
    header_anchor: dict[str, str] = {}
    anchor(toc_title, used)
    for mod in modules:
        module_anchor[mod] = anchor(f"Module {mod}", used)
        for h in by_module[mod]:
            header_anchor[h.path] = anchor(h.include, used)
            for d in h.decls:
                if visible(d) and d.is_type:
                    anchor(f"{d.kind} {ns_relative(d.name, d.namespace)}", used)
    anchor("Reflected types", used)

    # Reflection map (type qualname -> reflect decls)
    reflections: dict[str, list[tuple[Decl, Header]]] = {}
    for h in headers:
        for d in h.decls:
            if d.kind == "reflect":
                reflections.setdefault(d.qualname, []).append((d, h))

    out.append(f"## {toc_title}")
    out.append("")
    for mod in modules:
        target = MODULE_TARGETS.get(mod, "")
        tgt = f" (`{target}`)" if target else ""
        out.append(f"- [Module {mod}](#{module_anchor[mod]}){tgt}")
        for h in by_module[mod]:
            types = [ns_relative(d.name, d.namespace) for d in h.decls
                     if visible(d) and d.is_type]
            suffix = (": " + ", ".join(f"`{t}`" for t in types)) if types else ""
            out.append(f"  - [{h.include}](#{header_anchor[h.path]}){suffix}")
    out.append("- [Reflected types](#reflected-types)")
    out.append("")

    for mod in modules:
        out.append(f"## Module {mod}")
        out.append("")
        target = MODULE_TARGETS.get(mod)
        hs = by_module[mod]
        if target:
            out.append(f"CMake target `{target}`. {len(hs)} public header"
                       f"{'s' if len(hs) != 1 else ''}.")
            out.append("")
        for h in hs:
            out.extend(render_header(h, reflections))
    out.extend(render_reflection_index(headers, header_anchor))
    text = "\n".join(out).rstrip() + "\n"
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text


def render_header(h: Header, reflections) -> list[str]:
    out = [f"### {h.include}", ""]
    out.append(f"```cpp\n#include <{h.include}>\n```")
    out.append("")
    meta = f"Source: [`{h.path}`](../{h.path})"
    if h.namespaces:
        meta += ". Namespaces: " + ", ".join(f"`{n}`" for n in h.namespaces)
    out.append(meta + ".")
    out.append("")
    if h.module_comment:
        out.extend(render_doc(h.module_comment))
        out.append("")
    decls = [d for d in h.decls if visible(d)]
    if not decls:
        out.append("_No public declarations._")
        out.append("")
        return out
    i = 0
    while i < len(decls):
        d = decls[i]
        if d.is_type:
            out.extend(render_type_entry(d, reflections, h.include))
            i += 1
            continue
        if d.kind == "macro":
            group = [d]
            i += 1
            if not d.doc and not d.alt and not d.cond:
                while i < len(decls) and decls[i].kind == "macro" and not decls[i].doc \
                        and not decls[i].alt and not decls[i].cond:
                    group.append(decls[i])
                    i += 1
            out.extend(render_macro_group(group))
            continue
        if d.kind == "reflect":
            group = [d]
            i += 1
            while i < len(decls) and decls[i].kind == "reflect":
                group.append(decls[i])
                i += 1
            out.append("```cpp")
            for r in group:
                out.extend(doc_comment_lines(r.doc, ""))
                out.extend(with_trailing(wrap_signature(r.sig, "", r.param_span), r.trailing))
            out.append("```")
            out.append("")
            continue
        # free declarations: undocumented runs merge into one block
        group = [d]
        i += 1
        if not d.doc:
            while i < len(decls):
                e = decls[i]
                if e.is_type or e.kind in ("macro", "reflect") or e.doc \
                        or e.namespace != d.namespace:
                    break
                group.append(e)
                i += 1
        out.extend(render_free_group(group))
    return out


def render_free_group(group: list[Decl]) -> list[str]:
    ns = group[0].namespace
    out = ["```cpp"]
    wrap_ns = ns not in ("lam", "")
    if wrap_ns:
        out.append(f"namespace {ns} {{")
    for d in group:
        out.extend(render_member_block(dataclasses.replace(d, doc=[]), ""))
    if wrap_ns:
        out.append("}")
    out.append("```")
    out.append("")
    if group[0].doc:
        out.extend(render_doc(group[0].doc))
        out.append("")
    return out


def render_macro_group(group: list[Decl]) -> list[str]:
    out = ["```cpp"]
    for d in group:
        if d.cond:
            out.append(f"#if {d.cond}")
        out.extend(with_trailing([d.sig], d.trailing))
        for alt in d.alt:
            out.extend(alt.split("\n"))
        if d.cond:
            out.append("#endif")
    out.append("```")
    out.append("")
    d = group[0]
    if d.doc:
        out.extend(render_doc(d.doc))
        out.append("")
    return out


def render_type_entry(d: Decl, reflections, include: str) -> list[str]:
    out = [f"#### {d.kind} `{ns_relative(d.name, d.namespace)}`", ""]
    if d.doc:
        out.extend(render_doc(d.doc))
        out.append("")
    code = ["```cpp"]
    wrap_ns = d.namespace not in ("lam", "")
    if wrap_ns:
        code.append(f"namespace {d.namespace} {{")
    code.extend(render_member_block(dataclasses.replace(d, doc=[]), ""))
    if wrap_ns:
        code.append("}")
    code.append("```")
    out.extend(code)
    out.append("")
    for r, rh in reflections.get(d.qualname, []):
        where = "" if rh.include == include else f" (in `{rh.include}`)"
        out.append("Reflected" + where + " with " + reflect_summary(r))
        out.append("")
    return out


def render_reflection_index(headers: list[Header], header_anchor) -> list[str]:
    out = ["## Reflected types", ""]
    comp = header_anchor.get("src/ecs/include/lam/ecs/component.h")
    see = f" (see [lam/ecs/component.h](#{comp}))" if comp else ""
    out.append("Every `LAM_REFLECT*` registration in the public headers. Reflected component "
               f"types register themselves during static initialisation{see}.")
    out.append("")
    rows = []
    for h in headers:
        for d in h.decls:
            if d.kind != "reflect":
                continue
            kind, flags, fields = reflect_info(d)
            cell = f"`{flags}`".replace("|", "\\|") + f" ({kind})" if flags else kind
            fields_txt = ", ".join(f"`{f}`".replace("|", "\\|") for f in fields)
            macro = d.reflect_macro
            rows.append(f"| `{d.qualname}` | `{macro}` | {cell} | {fields_txt} "
                        f"| [{h.include}](#{header_anchor[h.path]}) |")
    if rows:
        out.append("| Type | Macro | Kind and flags | Fields or names | Header |")
        out.append("|---|---|---|---|---|")
        out.extend(rows)
    else:
        out.append("_No reflected types._")
    out.append("")
    return out


# --------------------------------------------------------------------------------------------
# Main

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="repository root (default: the parent of tools/)")
    ap.add_argument("--output", default=OUTPUT, help=f"output path (default: {OUTPUT})")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the output file is out of date")
    ap.add_argument("--stdout", action="store_true", help="print to stdout instead of writing")
    ap.add_argument("--strict", action="store_true", help="exit 2 if any warning was reported")
    args = ap.parse_args(argv)
    root: Path = args.root.resolve()

    found = discover(root)
    if not found:
        print(f"error: no public headers found under {root}/src", file=sys.stderr)
        return 2
    headers = [load_header(root, f, inc, mod) for f, inc, mod in found]
    order = {m: k for k, m in enumerate(MODULE_ORDER)}
    headers.sort(key=lambda h: (order.get(h.module, len(order)), h.module, h.path))
    for h in headers:
        if h.module not in order:
            DIAG.warn(h.path, 1, f"module '{h.module}' is not in MODULE_ORDER; listed last")
    text = render(headers)

    for msg in DIAG.messages:
        print(msg, file=sys.stderr)

    out_path = root / args.output
    if args.stdout:
        sys.stdout.write(text)
    elif args.check:
        current = out_path.read_text(encoding="utf-8") if out_path.exists() else ""
        if current != text:
            print(f"{args.output} is out of date; run python3 tools/gen_api_reference.py",
                  file=sys.stderr)
            return 1
        print(f"{args.output} is up to date ({len(headers)} headers).", file=sys.stderr)
    else:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text, encoding="utf-8")
        print(f"wrote {args.output}: {len(headers)} headers, "
              f"{sum(1 for h in headers for d in h.decls if visible(d))} declarations, "
              f"{len(DIAG.messages)} warnings.", file=sys.stderr)
    if args.strict and DIAG.messages:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
