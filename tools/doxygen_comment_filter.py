#!/usr/bin/env python3
"""Doxygen INPUT_FILTER: promotes eligible plain /* */ comments to /** */.

fbpanel's source is already densely commented in a consistent prose style
("Parameters:", "Returns:", "Note:", "WARNING:", "BUG:") using plain /* */
blocks, which Doxygen ignores as documentation by default (only /**, /*!,
or /// are recognized). Rather than physically rewriting ~2,000+ comment
blocks across 82 files -- a large, one-time, risky mechanical edit that a
future contributor's next plain-prose comment immediately falls outside of
again -- this filter promotes eligible blocks *only in the stream Doxygen
parses*. The committed source files are never modified: Doxygen invokes
this script as `<script> <file>` and uses stdout as the parsed content
(see FILTER_PATTERNS in the Doxyfile). `git diff` on any source file will
never show output from this script.

Promotion is deliberately conservative (fail-safe, not fail-open):
  - A comment already starting with /** or /*! is passed through
    byte-for-byte untouched (handles files with hand-tagged Doxygen
    comments alongside not-yet-tagged ones).
  - A comment containing copyright/license boilerplate is never promoted
    (left as plain prose; several files carry vendored non-MIT license
    text -- see docs/THIRD_PARTY_NOTICES.md).
  - A comment is promoted only if it sits at brace depth 0 (file/decl
    scope -- excludes the numbered mid-function step-comments common in
    this codebase) AND either:
      (a) is immediately followed by what looks like a function or
          struct/enum/union/typedef declaration, or
      (b) is the file's first eligible (non-license) comment, treated as
          the file-brief and tagged with @file explicitly.
  - Within a promoted comment, "Parameters:" list items become @param
    ONLY when the name matches an actual parameter of the following
    declaration (found via the same lookahead) -- this is what prevents
    the filter from ever manufacturing a doc-comment/signature mismatch
    that WARN_IF_DOC_ERROR would flag.
  - Everything else about the comment's text is left untouched.
"""
import re
import sys

LICENSE_SIGNAL_RE = re.compile(
    r"Copyright|Free Software Foundation|WITHOUT ANY WARRANTY|"
    r"GNU (General|Lesser|Library) Public License",
    re.IGNORECASE,
)

DECLARATION_KEYWORD_RE = re.compile(r"^\s*(struct|enum|union|typedef)\b")

# Return-type + qualifiers (one or more words, allowing '*'), captured as
# group 1, then a name (group 2), then '('. Matches both "int foo(" and
# multi-line "static int\nfoo(".
FUNC_HEAD_RE = re.compile(
    r"^\s*((?:[A-Za-z_][A-Za-z0-9_]*\b[\s*]+){1,6})"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\(",
)

STRAY_COMMAND_RE = re.compile(r"([@\\])(?=[A-Za-z])")
STRAY_LINK_RE = re.compile(r"::(?=[A-Za-z_])")
STRAY_HASH_RE = re.compile(r"#(?=[A-Za-z_])")
POINTER_STAR_RE = re.compile(r"\*(?=[A-Za-z_])")


def escape_stray_doxygen_syntax(text):
    """Neutralize text in already-existing prose that would otherwise be
    misread as Doxygen/Markdown syntax once a comment is promoted:
      - '@word' or '\\word' (this codebase's authors sometimes use '@name'
        informally to mean "the parameter called name", e.g. "@src", "@dir",
        even literal '@file' -- none of these are real Doxygen commands in
        the original plain-comment source) -> doubled ('@@word'/'\\\\word'),
        which Doxygen renders as a literal '@'/'\\' character.
      - '::word' (an explicit cross-reference link in Doxygen syntax,
        requiring no '@'/'\\' prefix at all -- e.g. prose describing a
        GObject vfunc informally as "::destroy") -> the first colon is
        replaced with its HTML numeric entity, breaking the "::" token
        Doxygen's link parser looks for while still rendering as "::" in
        the generated HTML (ASCII-only; avoids embedding literal
        zero-width/invisible Unicode characters in this source file).
      - '#word' (another no-prefix-needed autolink trigger -- prose
        mentioning preprocessor directives like "#include"/"#define" reads
        as a link attempt to a member named "include"/"define") -> escaped
        with a leading backslash, Doxygen's documented way to produce a
        literal special character.
      - '*word' (C pointer-declarator syntax in prose, e.g. "gconf_block
        *b" or "FbBg *bg" -- extremely common throughout this codebase) ->
        backslash-escaped so Markdown's emphasis parser doesn't treat a
        lone '*' as an unmatched *emphasis* opener/closer, which produces
        "found <em>/</em> tag without matching" errors that can point at
        an unrelated later location in the same file.
      - '<' / '>' (e.g. "thermal_zone<N>", "<application>" as a config-block
        name) -> HTML entities, so Doxygen's HTML-tag scanner doesn't try to
        parse them as tags.
    Must run on the raw prose BEFORE this script's own @note/@warning/@bug/
    @return/@param/@file substitutions are inserted, so real commands this
    script adds are never touched.
    """
    text = STRAY_COMMAND_RE.sub(lambda m: m.group(1) * 2, text)
    text = STRAY_LINK_RE.sub("&#58;:", text)
    text = STRAY_HASH_RE.sub(r"\\#", text)
    text = POINTER_STAR_RE.sub(r"\\*", text)
    text = text.replace("<", "&lt;").replace(">", "&gt;")
    return text


NOTE_RE = re.compile(r"^(\s*\*?\s*)(?:NOTE|Note)\s*:\s*(.*)$")
WARNING_RE = re.compile(r"^(\s*\*?\s*)(?:WARNING|Warning)\s*:\s*(.*)$")
BUG_RE = re.compile(r"^(\s*\*?\s*)BUG\s*:\s*(.*)$")
RETURNS_RE = re.compile(r"^(\s*\*?\s*)Returns?\s*:\s*(.*)$")
PARAMETERS_HEADER_RE = re.compile(r"^(\s*\*?\s*)Parameters\s*:\s*$")
# Requires whitespace on BOTH sides of the dash(es): without it, a wrapped
# continuation line starting with a C arrow expression like "dc->thing"
# (name "dc", one literal '-', then ">thing" as "description") is
# misread as a second, duplicate parameter-list entry for "dc".
PARAM_ITEM_RE = re.compile(r"^(\s*\*?\s*)([A-Za-z_][A-Za-z0-9_]*)\s+-{1,2}\s+(.*)$")


def find_top_level_comments(text):
    """Scan text outside strings/char-literals/line-comments for /* */
    blocks. Returns a list of dicts: start, end, text, brace_depth (the
    brace nesting level *before* the comment starts)."""
    i = 0
    n = len(text)
    brace_depth = 0
    comments = []
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j == -1 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            end = n if j == -1 else j + 2
            comments.append(
                {"start": i, "end": end, "text": text[i:end], "brace_depth": brace_depth}
            )
            i = end
            continue
        if c == "{":
            brace_depth += 1
        elif c == "}":
            brace_depth -= 1
        i += 1
    return comments


def looks_like_declaration(remaining, max_lookahead=400):
    """Check whether `remaining` (the text right after a comment) looks
    like it starts with a struct/enum/union/typedef or a function
    declaration/definition.

    Returns (is_declaration, param_names, is_static, is_void_return).
    is_static/is_void_return are only meaningful when a function match
    was found (they matter for deciding whether @param/@return are safe
    to add -- see promote_comment)."""
    snippet = remaining[:max_lookahead]
    lines = snippet.split("\n")
    idx = 0
    while idx < len(lines):
        stripped = lines[idx].strip()
        if stripped == "" or stripped.startswith("#"):
            idx += 1
            continue
        break
    reduced = "\n".join(lines[idx:])

    if DECLARATION_KEYWORD_RE.match(reduced):
        return True, [], False, False

    m = FUNC_HEAD_RE.match(reduced)
    if not m:
        return False, [], False, False

    prefix = m.group(1)
    is_static = bool(re.search(r"\bstatic\b", prefix))
    is_void_return = bool(re.search(r"\bvoid\b", prefix)) and "*" not in prefix

    open_paren_idx = m.end() - 1
    depth = 0
    params_text = None
    j = open_paren_idx
    while j < len(reduced):
        if reduced[j] == "(":
            depth += 1
        elif reduced[j] == ")":
            depth -= 1
            if depth == 0:
                params_text = reduced[open_paren_idx + 1 : j]
                break
        j += 1
    if params_text is None:
        return True, [], is_static, is_void_return

    names = []
    for part in params_text.split(","):
        part = part.strip()
        if not part or part in ("void", "..."):
            continue
        idents = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", part)
        if idents:
            names.append(idents[-1])
    return True, names, is_static, is_void_return


def promote_comment(comment_text, param_names, is_file_brief, allow_param, allow_return):
    """Convert a plain /* ... */ block into a tagged /** ... */ block,
    applying the Note:/WARNING:/BUG:/Returns:/Parameters: substitutions.

    allow_param gates Parameters:->@param. It is True only for `static`
    functions (no linkage outside this translation unit, hence no
    separate header declaration that could already carry hand-written
    @param docs). For non-static functions -- almost always declared with
    full @param/@return tags in one of the hand-tagged panel/*.h headers
    -- Doxygen treats the declaration and definition as one entity, so a
    duplicate @param here would produce a "multiple @param documentation
    sections" error.

    allow_return gates Returns:->@return similarly, AND is also False for
    void-returning functions regardless of static/non-static (Doxygen
    errors on "documented return type ... that does not return
    anything").

    Note:/WARNING:/BUG: still convert unconditionally -- those aren't
    structural per-parameter/per-return documentation, so they don't have
    this duplication failure mode.
    """
    assert comment_text.startswith("/*") and comment_text.endswith("*/")
    inner = escape_stray_doxygen_syntax(comment_text[2:-2])
    lines = inner.split("\n")

    if allow_param and param_names:
        # Completeness pre-check: only convert Parameters: -> @param if
        # EVERY actual parameter gets a matching line. A partial list
        # (common for GTK callbacks, where the original prose only
        # described the "interesting" params and skipped boilerplate ones
        # like `widget`/`context`) makes Doxygen flag the rest as
        # undocumented -- worse than just leaving the whole section as
        # plain prose, which is what happens when allow_param is False.
        matched = set()
        scanning = False
        for line in lines:
            if PARAMETERS_HEADER_RE.match(line):
                scanning = True
                continue
            if scanning:
                pm = PARAM_ITEM_RE.match(line)
                if pm and pm.group(2) in param_names:
                    matched.add(pm.group(2))
                    continue
                scanning = False
        if matched != set(param_names):
            allow_param = False

    out_lines = []
    in_params_section = False

    for line in lines:
        if allow_param and PARAMETERS_HEADER_RE.match(line):
            in_params_section = True
            continue
        if in_params_section:
            pm = PARAM_ITEM_RE.match(line)
            if pm and pm.group(2) in param_names:
                prefix, name, desc = pm.groups()
                out_lines.append(f"{prefix}@param {name} {desc}")
                continue
            in_params_section = False
            # fall through: this line is handled by the normal rules below

        m = RETURNS_RE.match(line) if allow_return else None
        if m:
            prefix, rest = m.groups()
            out_lines.append(f"{prefix}@return {rest}")
            continue
        m = WARNING_RE.match(line)
        if m:
            prefix, rest = m.groups()
            out_lines.append(f"{prefix}@warning {rest}")
            continue
        m = BUG_RE.match(line)
        if m:
            prefix, rest = m.groups()
            out_lines.append(f"{prefix}@bug {rest}")
            continue
        m = NOTE_RE.match(line)
        if m:
            prefix, rest = m.groups()
            out_lines.append(f"{prefix}@note {rest}")
            continue
        out_lines.append(line)

    if is_file_brief:
        # Explicit @file marker so Doxygen attaches this as file-level
        # documentation rather than a free-floating, unassociated comment.
        # A blank comment line MUST separate "@file" from the description:
        # without it, Doxygen's \file argument parser consumes the next
        # bare word as an (invalid) filename argument instead of treating
        # it as the start of the description. Prepend as whole lines
        # (rather than string-splicing the joined text) so multi-line
        # comments (out_lines[0] == "", from the "/*\n" opening) get a
        # clean "* @file" / "*" pair with correct "* " decoration, and
        # single-line comments get a minimal equivalent.
        if out_lines and out_lines[0] == "":
            out_lines = ["", " * @file", " *"] + out_lines[1:]
        else:
            out_lines = [" @file", " *"] + out_lines
    new_inner = "\n".join(out_lines)
    return "/**" + new_inner + "*/"


def process(text):
    comments = find_top_level_comments(text)
    result = []
    last_end = 0
    file_brief_assigned = False

    for c in comments:
        start, end, ctext = c["start"], c["end"], c["text"]
        result.append(text[last_end:start])

        if ctext.startswith("/**") or ctext.startswith("/*!"):
            result.append(ctext)
            last_end = end
            continue

        if LICENSE_SIGNAL_RE.search(ctext):
            result.append(ctext)
            last_end = end
            continue

        promote = False
        is_file_brief = False
        param_names = []
        is_static = False
        is_void_return = False
        if c["brace_depth"] == 0:
            is_decl, param_names, is_static, is_void_return = looks_like_declaration(text[end:])
            if is_decl:
                promote = True
            elif not file_brief_assigned:
                promote = True
                is_file_brief = True

        if promote:
            file_brief_assigned = True
            result.append(
                promote_comment(
                    ctext,
                    param_names,
                    is_file_brief,
                    allow_param=is_static,
                    allow_return=is_static and not is_void_return,
                )
            )
        else:
            result.append(ctext)
        last_end = end

    result.append(text[last_end:])
    return "".join(result)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: doxygen_comment_filter.py <file>\n")
        return 1
    with open(sys.argv[1], "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    sys.stdout.write(process(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
