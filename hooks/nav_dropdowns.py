"""Serialize the MkDocs nav into each page so the top-nav tab dropdowns can be
built from a single source of truth.

The sub-pages of several sections (Benchmarking, API Reference, Tutorials) hide
the primary sidebar via `hide: navigation` front matter, so without an injected
dropdown there is no way to hop between sibling pages. `docs/javascripts/
tab-dropdowns.js` builds those dropdowns at runtime; this hook hands it the data
derived directly from `mkdocs.yml` nav, so the two never drift.
"""

import json

_NAV_JSON = "{}"


def on_nav(nav, config, files):
    """Capture multi-page sections from the built nav as {title: [{label, url}]}."""
    global _NAV_JSON
    sections = {}
    for item in nav.items:
        if not getattr(item, "is_section", False) or not item.children:
            continue
        subpages = [
            {"label": child.title, "url": child.url}
            for child in item.children
            if getattr(child, "is_page", False) and child.title
        ]
        if len(subpages) > 1:
            sections[item.title] = subpages
    _NAV_JSON = json.dumps(sections)
    return nav


def on_post_page(output, page, config):
    """Inject the captured nav as `window.NV_NAV` for tab-dropdowns.js to read."""
    snippet = "<script>window.NV_NAV = {};</script>".format(_NAV_JSON)
    return output.replace("</head>", snippet + "</head>", 1)
