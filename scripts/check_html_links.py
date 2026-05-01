#!/usr/bin/env python3
"""Verify that hand-maintained HTML pages link only to existing locations
in the rendered MkDocs site.

The hand-maintained HTML files (docs/index.html, docs/daqiri-api.html) are
not part of the MkDocs link graph, so `mkdocs build --strict` does not
check their hrefs. This script does, by walking each href and confirming
the target resolves under site/.

Usage: python scripts/check_html_links.py [SITE_DIR]
       (default SITE_DIR is ./site, the output of `mkdocs build`)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import urldefrag, urlparse

HTML_PAGES = ("index.html", "daqiri-api.html")
HREF_RE = re.compile(r'href="([^"]+)"')


def is_external(href: str) -> bool:
    parsed = urlparse(href)
    if parsed.scheme:
        return True
    if href.startswith(("//", "#")):
        return True
    return False


def resolve(file_dir: Path, site_root: Path, href: str) -> tuple[bool, str]:
    target, _ = urldefrag(href)
    target = target.split("?", 1)[0]
    if not target:
        return True, ""
    candidate = (file_dir / target).resolve()
    try:
        candidate.relative_to(site_root.resolve())
    except ValueError:
        return False, f"escapes site root: {href!r}"
    if candidate.is_file():
        return True, ""
    if candidate.is_dir() and (candidate / "index.html").exists():
        return True, ""
    return False, f"target not found: {href!r}"


def main() -> int:
    site = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("site")
    if not site.is_dir():
        print(f"site directory not found: {site}", file=sys.stderr)
        return 1
    errors: list[str] = []
    for page in HTML_PAGES:
        path = site / page
        if not path.exists():
            errors.append(f"{page}: not present in {site}")
            continue
        content = path.read_text(encoding="utf-8")
        for match in HREF_RE.finditer(content):
            href = match.group(1)
            if is_external(href):
                continue
            ok, msg = resolve(path.parent, site, href)
            if not ok:
                errors.append(f"{page}: {msg}")
    if errors:
        print("Broken HTML links:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(f"All HTML links in {', '.join(HTML_PAGES)} resolve.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
