import html, re, sys
src = open(sys.argv[1]).read().split("\n")
out_path = sys.argv[2]

PILLS = {"high": "crit", "medium": "warn", "low": "ok", "fixed": "good",
         "open": "crit", "analysis": "warn", "evidence": "ok", "spike rule": "ok",
         "spike only": "warn"}

def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    return t

def cell(t, header):
    t = t.strip()
    if not header and t in PILLS:
        return f'<td><span class="pill {PILLS[t]}">{html.escape(t)}</span></td>'
    tag = "th" if header else "td"
    return f"<{tag}>{inline(t)}</{tag}>"

out, i = [], 0
para, list_stack = [], None
def flush_para():
    global para
    if para:
        out.append("<p>" + inline(" ".join(para)) + "</p>")
        para = []
while i < len(src):
    line = src[i]
    if line.startswith("```"):
        flush_para()
        code = []
        i += 1
        while not src[i].startswith("```"):
            code.append(src[i]); i += 1
        out.append('<pre><code>' + html.escape("\n".join(code)) + "</code></pre>")
        i += 1; continue
    if line.startswith("|"):
        flush_para()
        rows = []
        while i < len(src) and src[i].startswith("|"):
            rows.append([c for c in src[i].strip().strip("|").split(" | ")]); i += 1
        h = "".join(cell(c, True) for c in rows[0])
        body = "".join("<tr>" + "".join(cell(c, False) for c in r) + "</tr>" for r in rows[2:])
        out.append(f'<div class="tw"><table><thead><tr>{h}</tr></thead><tbody>{body}</tbody></table></div>')
        continue
    m = re.match(r"^(#{1,3}) (.*)", line)
    if m:
        flush_para()
        n = len(m.group(1))
        hid = re.sub(r"[^a-z0-9]+", "-", m.group(2).lower()).strip("-")
        out.append(f'<h{n} id="{hid}">{inline(m.group(2))}</h{n}>')
        i += 1; continue
    m = re.match(r"^(\s*)(- |\d+\. )(.*)", line)
    if m:
        flush_para()
        kind = "ol" if m.group(2)[0].isdigit() else "ul"
        items = []
        while i < len(src):
            mm = re.match(r"^(- |\d+\. )(.*)", src[i])
            if mm and (("ol" if mm.group(1)[0].isdigit() else "ul") == kind):
                items.append([mm.group(2)]); i += 1
            elif src[i].startswith("  ") and items and src[i].strip():
                items[-1].append(src[i].strip()); i += 1
            else:
                break
        out.append(f"<{kind}>" + "".join("<li>" + inline(" ".join(it)) + "</li>" for it in items) + f"</{kind}>")
        continue
    if not line.strip():
        flush_para(); i += 1; continue
    para.append(line.strip()); i += 1
flush_para()

body = "\n".join(out)
page = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Composable uenv spike report</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:ital,wght@0,400;0,600;0,700;1,400&family=IBM+Plex+Serif:wght@600&display=swap" rel="stylesheet">
<style>
:root {{
  /* layout: one reading column, tables may run wider and scroll */
  --bg: #f6f7f8; --surface: #ffffff; --ink: #1d2329; --muted: #59636e;
  --line: #d7dde3; --accent: #1f5f8b;
  --good: #1e7a4f; --warn: #9a6400; --crit: #b3261e; --okc: #4b5b6b;
  --font-display: "IBM Plex Serif", Georgia, serif;
  --font-body: "IBM Plex Sans", system-ui, sans-serif;
  --font-mono: "IBM Plex Mono", ui-monospace, monospace;
}}
@media (prefers-color-scheme: dark) {{
  :root:not([data-theme="light"]) {{
    --bg: #12161a; --surface: #1a2026; --ink: #e3e8ec; --muted: #9aa6b1;
    --line: #2d363f; --accent: #6fb3e0;
    --good: #5cc48f; --warn: #e0a640; --crit: #f07a70; --okc: #a3b1bf;
    color-scheme: dark;
  }}
}}
:root[data-theme="dark"] {{
  --bg: #12161a; --surface: #1a2026; --ink: #e3e8ec; --muted: #9aa6b1;
  --line: #2d363f; --accent: #6fb3e0;
  --good: #5cc48f; --warn: #e0a640; --crit: #f07a70; --okc: #a3b1bf;
  color-scheme: dark;
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--ink);
  font: 16px/1.6 var(--font-body); }}
main {{ max-width: 46rem; margin: 0 auto; padding: 2.5rem 1rem 4rem; }}
h1, h2, h3 {{ font-family: var(--font-display); text-wrap: balance;
  line-height: 1.25; }}
h1 {{ font-size: 2rem; margin: 0 0 1rem; }}
h2 {{ font-size: 1.45rem; margin: 2.75rem 0 0.75rem; padding-top: 1rem;
  border-top: 2px solid var(--line); }}
h3 {{ font-size: 1.12rem; margin: 2rem 0 0.5rem; color: var(--accent); }}
p, li {{ max-width: 65ch; }}
strong {{ font-weight: 600; }}
code {{ font: 0.88em var(--font-mono); background: var(--surface);
  border: 1px solid var(--line); border-radius: 3px; padding: 0 0.25em; }}
pre {{ background: var(--surface); border: 1px solid var(--line);
  border-left: 3px solid var(--accent); padding: 0.75rem 1rem;
  overflow-x: auto; border-radius: 4px; }}
pre code {{ border: 0; padding: 0; background: none; }}
.tw {{ overflow-x: auto; margin: 1rem 0; }}
table {{ border-collapse: collapse; font-size: 0.88rem; min-width: 100%;
  background: var(--surface); font-variant-numeric: tabular-nums; }}
th, td {{ text-align: left; vertical-align: top; padding: 0.4rem 0.6rem;
  border-bottom: 1px solid var(--line); }}
th {{ color: var(--muted); font-weight: 600; border-bottom: 2px solid var(--line); }}
.pill {{ display: inline-block; font-size: 0.78rem; font-weight: 600;
  padding: 0 0.5em; border-radius: 999px; border: 1px solid currentColor;
  white-space: nowrap; }}
.pill.crit {{ color: var(--crit); }} .pill.warn {{ color: var(--warn); }}
.pill.good {{ color: var(--good); }} .pill.ok {{ color: var(--okc); }}
ul, ol {{ padding-left: 1.4rem; display: grid; gap: 0.35rem; }}
a {{ color: var(--accent); }}
</style>
</head>
<body>
<main>
{body}
</main>
</body>
</html>
"""
open(out_path, "w").write(page)
print("wrote", out_path, len(page))
