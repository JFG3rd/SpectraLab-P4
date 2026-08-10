/* Shared front-end for every SpectraLab-P4 page.
 *
 * Before this file the theme toggle and the browser-clock push were pasted
 * verbatim into all four pages, and navigation was hub-and-spoke: each page
 * had one link home and none to its siblings. Both live here now, so a page
 * only has to include this script and mark up its own content.
 *
 * Served from flash as a C array (see tools/gen_web_assets.py), so keep it
 * small and dependency-free — there is no bundler and no CDN reachable from
 * the device's own access point.
 */

/* ── dark / light toggle ─────────────────────────────────────────── */

function applyTheme(dark) {
    document.body.classList.toggle("dark-mode", dark);
    document.documentElement.style.colorScheme = dark ? "dark" : "light";
    var b = document.getElementById("themeToggle");
    if (b) b.textContent = dark ? "☀️ Light Mode" : "🌙 Dark Mode";
}

function toggleTheme() {
    var dark = !document.body.classList.contains("dark-mode");
    try { localStorage.setItem("theme", dark ? "dark" : "light"); } catch (e) {}
    applyTheme(dark);
}

(function () {
    var saved = null;
    try { saved = localStorage.getItem("theme"); } catch (e) {}
    var dark = saved ? (saved === "dark")
        : (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches);
    document.addEventListener("DOMContentLoaded", function () { applyTheme(dark); });
})();

/* ── navigation ──────────────────────────────────────────────────── */

var SL_NAV = [
    { href: "/",                 label: "Dashboard" },
    { href: "/settings.html",    label: "Settings" },
    { href: "/wifi-setup.html",  label: "Network" },
    { href: "/files.html",       label: "Files" },
    { href: "/cal-upload.html",  label: "Calibration" }
];

/* Rendered into <nav id="slNav"> on every page. The active entry is matched on
 * the path alone, so a query string or a trailing index.html still highlights. */
function renderNav() {
    var host = document.getElementById("slNav");
    if (!host) return;
    var here = window.location.pathname;
    if (here === "/index.html") here = "/";
    host.innerHTML = "";
    SL_NAV.forEach(function (item) {
        var a = document.createElement("a");
        a.href = item.href;
        a.textContent = item.label;
        a.className = "sl-nav-link" + (item.href === here ? " active" : "");
        host.appendChild(a);
    });
}
document.addEventListener("DOMContentLoaded", renderNav);

/* ── clock ───────────────────────────────────────────────────────── */

/* Tell the device what time it is. The board has no RTC and may have no route
 * to an NTP server — in access-point mode it certainly does not. The firmware
 * ignores this once SNTP has supplied a clock, so a skewed browser cannot
 * degrade a good sync. Runs on every page precisely because you cannot predict
 * which one someone opens first. */
function postBrowserClock() {
    fetch("/api/time", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ epoch: Math.floor(Date.now() / 1000) })
    }).catch(function () { /* offline or busy: not worth reporting */ });
}
document.addEventListener("DOMContentLoaded", postBrowserClock);

/* ── helpers shared by the pages ─────────────────────────────────── */

/* Status line writer. Every page used to re-implement this against its own
 * element id; pass the id instead. */
function slStatus(id, msg, kind) {
    var el = document.getElementById(id);
    if (!el) return;
    el.textContent = msg;
    el.className = "flash-stats" + (kind ? " " + kind : "");
}

/* Fetch /api/status once and hand it to a callback. */
function slFetchStatus(cb) {
    fetch("/api/status").then(function (r) { return r.json(); })
        .then(cb).catch(function () {});
}

/* Put the detected board name in the page title and any [data-board] element,
 * so a P4X does not present itself as a P4. */
document.addEventListener("DOMContentLoaded", function () {
    slFetchStatus(function (s) {
        if (!s.board) return;
        document.querySelectorAll("[data-board]").forEach(function (el) {
            el.textContent = s.board;
        });
        document.title = document.title.replace(/SpectraLab-P4X?/, s.board);
    });
});
