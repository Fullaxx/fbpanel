# Third-Party / Vendored Code Notices

fbpanel's own original code is distributed under the MIT License (see
[LICENSE](../LICENSE)). A handful of files were vendored or adapted from
other GPL/LGPL-licensed projects, or predate the project's current
licensing, and remain under their original terms — a downstream
distributor cannot unilaterally relicense someone else's copyrighted code
by adding a different `LICENSE` file elsewhere in the repository. This
page is an index of those files, verified directly against each file's
own header comment.

---

## MIT (documentation tooling, not part of fbpanel itself)

| File | Upstream | Author | Year |
|---|---|---|---|
| `docs/doxygen-awesome.css` | [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) v2.4.2 | jothepro | 2021–2025 |

A drop-in Doxygen HTML theme, vendored verbatim (pinned to the v2.4.2 tag)
for the `make doc` / `cmake --build build --target doc` output. Compatible
with fbpanel's own MIT license; included here only for completeness, not
because of any licensing concern.

## LGPL-2.0-or-later

| File(s) | Upstream | Author | Year |
|---|---|---|---|
| `panel/bg.c`, `panel/bg.h` | fb-background-monitor | Ian McKellar | 2001–2002 |
| `panel/ev.c`, `panel/ev.h` | fb-background-monitor (see note) | Ian McKellar | 2001–2002 |
| `panel/gtkbar.c`, `panel/gtkbar.h` | GTK+ | Peter Mattis, Spencer Kimball, Josh MacDonald | 1995–1997 |
| `panel/gtkbgbox.c`, `panel/gtkbgbox.h` | GTK+ | Peter Mattis, Spencer Kimball, Josh MacDonald | 1995–1997 |
| `plugins/tray/eggtraymanager.c`, `.h` | GNOME libegg | Anders Carlsson | 2002 |

> **Note**: `panel/ev.c`/`ev.h` still carry the literal header text
> `fb-background-monitor.c:` / `.h:` — a copy-paste artifact from
> `panel/bg.c`/`bg.h`, which they were split from (see
> [ARCHITECTURE.md](ARCHITECTURE.md)). Both pairs share the same upstream
> provenance and license.

## GPL-2.0-or-later

| File(s) | Upstream | Author | Year |
|---|---|---|---|
| `plugins/tray/fixedtip.c`, `.h` | Metacity (fixed-tip tooltip implementation) | Havoc Pennington; Red Hat Inc. | 2001–2002 |

## GPL-2.0-only

| File | Author | Year |
|---|---|---|
| `plugins/batterytext/batterytext.c` | Fred Stober | 2017 |
| `plugins/genmon/genmon.c` | Davide Truffa | 2007 |

Both headers explicitly grant "version 2 dated June, 1991" with no
"or (at your option) any later version" clause — a stricter GPL-2.0-only
grant, distinct from the "-or-later" files above.

## Pre-dates the current license (not a vendoring case)

| File | Author | Year |
|---|---|---|
| `plugins/pager/pager.c` | Anatoly Asviyan, Joe MacDonald | 2002–2003 |

This is fbpanel's own original authorship — Anatoly Asviyan is the
project's founding author (see the
[History](../README.md#history) section) — not code borrowed from
another project. The header carries a bare copyright line with no
separate license grant; it predates the repository's current MIT
[LICENSE](../LICENSE), which already lists this authorship among its
stacked copyright lines.

---

## What this means in practice

None of this is a license *violation* — GPL/LGPL-licensed code can be
included in a project that is otherwise MIT-licensed. It means the
top-level `LICENSE` file's MIT terms apply to fbpanel's own original
code, while the files listed above remain under their original upstream
license regardless of what the top-level file says. Anyone redistributing
fbpanel, or a derivative, should comply with each file's actual license,
not just the top-level one.
