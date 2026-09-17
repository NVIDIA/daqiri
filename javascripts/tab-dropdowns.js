// Inject hover/focus dropdowns under top-nav tabs that map to a
// multi-page section in mkdocs.yml. The primary sidebar is hidden via
// `hide: navigation` on those section's sub-pages (see frontmatter), so
// without this script there's no way to hop directly between sibling
// pages without bouncing through the section's index.
//
// The section/sub-page list is NOT hand-maintained here: the
// `hooks/nav_dropdowns.py` MkDocs hook serializes the real nav from
// mkdocs.yml into `window.NV_NAV` at build time, so the dropdowns stay in
// sync with the nav automatically. Each entry's `url` is already
// site-root-relative (e.g. "api-reference/python/").

(function () {
  "use strict";

  const SECTIONS = window.NV_NAV || {};

  function getSiteBase() {
    // Material's site logo link in the header always points to the site
    // root, which is the most reliable cross-environment anchor (works in
    // `mkdocs serve` locally and on the deployed gh-pages site).
    const logo = document.querySelector("a.md-header__button.md-logo");
    if (logo && logo.href) {
      const u = new URL(logo.href, window.location.href);
      return u.pathname.endsWith("/") ? u.pathname : u.pathname + "/";
    }
    return "/";
  }

  function buildDropdowns() {
    const base = getSiteBase();
    const tabs = document.querySelectorAll(".md-tabs__item");
    if (!tabs.length) return;

    tabs.forEach((tab) => {
      const link = tab.querySelector(".md-tabs__link");
      if (!link) return;

      const label = link.textContent.trim();
      const subpages = SECTIONS[label];
      if (!subpages) return;

      // Skip if we've already attached (Material instant-loading re-runs us).
      if (tab.classList.contains("nv-has-dropdown")) return;

      const dd = document.createElement("ul");
      dd.className = "nv-tab-dropdown";
      subpages.forEach((sub) => {
        const li = document.createElement("li");
        const a  = document.createElement("a");
        a.href = base + sub.url;
        a.textContent = sub.label;
        li.appendChild(a);
        dd.appendChild(li);
      });

      tab.classList.add("nv-has-dropdown");
      tab.appendChild(dd);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", buildDropdowns);
  } else {
    buildDropdowns();
  }

  // Material's instant-loading feature swaps page content without a full
  // reload; re-run on the document$ event when it fires.
  if (typeof document$ !== "undefined" && document$.subscribe) {
    document$.subscribe(buildDropdowns);
  }
})();
