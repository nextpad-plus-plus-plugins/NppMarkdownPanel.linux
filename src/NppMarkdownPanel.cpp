/*
 * NppMarkdownPanel — Linux (GTK4) port
 *
 * Real-time Markdown preview panel for Nextpad++ Linux.
 * Uses WebKitGTK 6.0 with the SAME JS pipeline as the macOS port:
 * marked.js + highlight.js (+ optional mermaid / "squared" engine),
 * vendored verbatim under resources/.
 *
 * Original Windows plugin by Jens Wollgarten (GPLv2)
 * macOS port: NppMarkdownPanel.mm — this file mirrors it section by section.
 */

#include "plugin.h"        /* host contract: FuncItem, NppData, NPPM_/NPPN_ */
#include "Scintilla.h"

#include <webkit/webkit.h>
#include <jsc/jsc.h>
#include <glib/gstdio.h>
#include <dlfcn.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <memory>

#define NPP_EXPORT __attribute__((visibility("default")))

// ═══════════════════════════════════════════════════════════════════════════
//  Constants
// ═══════════════════════════════════════════════════════════════════════════

static const char *PLUGIN_NAME = "Markdown Panel";
static const int NB_FUNC = 11;
static FuncItem funcItem[NB_FUNC];
static NppData nppData;

static const int RENDER_DEBOUNCE_MS = 400;

// ═══════════════════════════════════════════════════════════════════════════
//  Settings
// ═══════════════════════════════════════════════════════════════════════════

struct MarkdownSettings {
    int zoomLevel = 100;
    bool autoShowPanel = false;
    bool syncWithCaret = true;
    bool syncWithFirstVisibleLine = true;
    bool allowAllExtensions = false;
    std::string supportedExtensions = "md,mkd,mdwn,mdown,mdtxt,markdown,mmd";
    bool enableMermaid = false;
    bool enableSquared = false;              // render mermaid flowcharts in the "squared" style
    std::string squaredTheme = "boardroom";  // boardroom | linen | blueprint
    bool syncPreviewToEditor = false;        // reverse sync: scrolling the preview scrolls the editor
};

static MarkdownSettings sSettings;

// ═══════════════════════════════════════════════════════════════════════════
//  Plugin state
// ═══════════════════════════════════════════════════════════════════════════

// Content widget — the GtkBox we register with the host via
// NPPM_DMM_REGISTERPANEL. Toolbar row (search + buttons) above the WebView.
static GtkWidget      *sContentBox = NULL;
static WebKitWebView  *sWebView    = NULL;
static GtkWidget      *sSearchEntry = NULL;

static long  g_panelHandle  = 0;    // NPPM_DMM_REGISTERPANEL handle
static bool  sPanelVisible  = false;
static bool  sWebViewReady  = false; // template finished loading (renderMarkdown JS exists)
static bool  sExporting     = false; // silent export in flight — pause panel renders

static std::string sLastRenderedText;
static std::string sCurrentFilePath;
static std::string sCurrentTempHtmlPath;
static std::string sResourcesDir;
static std::string sFullTemplate;

static guint sPendingRender = 0;    // g_timeout source ids (0 = none)
static guint sPendingScroll = 0;

// Image cache-buster generation. WebKit's WebContent process serves file://
// <img> subresources from an in-process memory cache keyed by URL, so an
// image edited on disk keeps rendering stale until its URL changes. Every
// render passes this generation to the page (window._imgGen) and the JS
// appends ?v=<gen> to local img URLs. Bumped on template (re)loads — which
// covers the Refresh button — and when the application becomes active again
// (the "edited the image in another app, switched back" case).
static long sImageGeneration = 0;

// Forward declarations
static void togglePanel();
static void syncWithCaretCmd();
static void syncWithFirstVisibleLineCmd();
static void showSettingsCmd();
static void showHelpCmd();
static void showAboutCmd();
static void exportCurrentToHtmlCmd();
static void exportCurrentToPdfCmd();
static void renderMarkdownDirect();
static void renderMarkdownDeferred();
// Preview → editor bridge (script-message callbacks)
static void applyPreviewScrollToEditor(intptr_t docLine);
static void locateWordFromPreview(const char *word, intptr_t startLine,
                                  intptr_t endLine, intptr_t occ);
static void ensureContentView();
static void refreshMarkdownPreview();
static void saveMarkdownAsPDF();
static void printMarkdownPreview();
static GtkPageSetup *letterPageSetup();
static void adjustPreviewZoom(int delta);
static void loadTemplateIntoWebView();
static void saveSettings();
static bool markdownPanelIsShown();

// ═══════════════════════════════════════════════════════════════════════════
//  Host / Scintilla access (see PORTING_NOTES.md — hostMsg carries NPPM only)
// ═══════════════════════════════════════════════════════════════════════════

extern "C" intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                                uintptr_t wParam, intptr_t lParam);

static long npp(unsigned int msg, unsigned long w = 0, long l = 0) {
    return nppData.hostMsg(msg, w, l);
}

static void *getCurScintilla() {
    int which = 0;
    return (void *)(intptr_t)npp(NPPM_GETCURRENTSCINTILLA, 0, (long)(intptr_t)&which);
}

static intptr_t sci(void *h, unsigned int msg, uintptr_t w = 0, intptr_t l = 0) {
    return scintilla_view_send_message(h, msg, w, l);
}

static std::string getCurrentFilePath() {
    char buf[4096] = {0};
    npp(NPPM_GETFULLCURRENTPATH, sizeof(buf) - 1, (long)(intptr_t)buf);
    return std::string(buf);
}

static std::string getCurrentExtension() {
    char buf[256] = {0};
    npp(NPPM_GETEXTPART, sizeof(buf) - 1, (long)(intptr_t)buf);
    return std::string(buf);
}

static bool isSupportedExtension() {
    if (sSettings.allowAllExtensions) return true;
    std::string ext = getCurrentExtension();
    if (ext.empty()) return false;
    if (ext[0] == '.') ext = ext.substr(1);
    for (auto &c : ext) c = tolower(c);
    std::string exts = sSettings.supportedExtensions;
    for (auto &c : exts) c = tolower(c);
    size_t pos = 0;
    while (pos < exts.size()) {
        size_t comma = exts.find(',', pos);
        if (comma == std::string::npos) comma = exts.size();
        std::string candidate = exts.substr(pos, comma - pos);
        while (!candidate.empty() && candidate.front() == ' ') candidate.erase(candidate.begin());
        while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
        if (candidate == ext) return true;
        pos = comma + 1;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Resource loading
// ═══════════════════════════════════════════════════════════════════════════

static std::string findResourcesDir() {
    Dl_info info;
    if (dladdr((void *)&findResourcesDir, &info) && info.dli_fname) {
        std::string soPath(info.dli_fname);
        size_t lastSlash = soPath.rfind('/');
        if (lastSlash != std::string::npos)
            return soPath.substr(0, lastSlash) + "/resources";
    }
    return "";
}

static std::string readFileToString(const std::string &path) {
    gchar *data = NULL; gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &data, &len, NULL)) return "";
    std::string out(data, len);
    g_free(data);
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HTML template composition — ported VERBATIM from the macOS build
//  (the JS/CSS pipeline is identical; only the squared file:// URL and the
//  string plumbing are platform code).
// ═══════════════════════════════════════════════════════════════════════════

static const char *kGitHubMarkdownCSS = R"CSS(
/* GitHub-flavored Markdown CSS — minimal, supports light + dark */
:root {
  --color-fg: #1f2328;
  --color-bg: #ffffff;
  --color-border: #d0d7de;
  --color-code-bg: #f6f8fa;
  --color-blockquote: #59636e;
  --color-link: #0969da;
  --color-heading-border: #d8dee4;
}
@media (prefers-color-scheme: dark) {
  :root {
    --color-fg: #e6edf3;
    --color-bg: #0d1117;
    --color-border: #30363d;
    --color-code-bg: #161b22;
    --color-blockquote: #8b949e;
    --color-link: #58a6ff;
    --color-heading-border: #21262d;
  }
}
* { box-sizing: border-box; }
body {
  font-family: system-ui, -apple-system, 'Segoe UI', 'Ubuntu', 'Cantarell', 'Noto Sans', Helvetica, Arial, sans-serif;
  font-size: 16px;
  line-height: 1.6;
  color: var(--color-fg);
  background: var(--color-bg);
  max-width: 980px;
  margin: 0 auto;
  padding: 20px 32px 48px;
  word-wrap: break-word;
}
h1, h2, h3, h4, h5, h6 { margin-top: 24px; margin-bottom: 16px; font-weight: 600; line-height: 1.25; }
h1 { font-size: 2em; padding-bottom: 0.3em; border-bottom: 1px solid var(--color-heading-border); }
h2 { font-size: 1.5em; padding-bottom: 0.3em; border-bottom: 1px solid var(--color-heading-border); }
h3 { font-size: 1.25em; }
h4 { font-size: 1em; }
h5 { font-size: 0.875em; }
h6 { font-size: 0.85em; color: var(--color-blockquote); }
p { margin-top: 0; margin-bottom: 16px; }
a { color: var(--color-link); text-decoration: none; }
a:hover { text-decoration: underline; }
img { max-width: 100%; height: auto; display: block; margin: 16px 0; border-radius: 6px; }
code {
  font-family: 'DejaVu Sans Mono', 'Ubuntu Mono', 'Liberation Mono', 'Courier New', monospace;
  font-size: 85%;
  padding: 0.2em 0.4em;
  background: var(--color-code-bg);
  border-radius: 6px;
}
pre {
  padding: 16px;
  overflow-x: auto;
  font-size: 85%;
  line-height: 1.45;
  background: var(--color-code-bg);
  border-radius: 6px;
  margin-bottom: 16px;
}
pre code { padding: 0; background: transparent; font-size: 100%; }
blockquote {
  margin: 0 0 16px;
  padding: 0 1em;
  color: var(--color-blockquote);
  border-left: 0.25em solid var(--color-border);
}
table { border-collapse: collapse; width: 100%; margin-bottom: 16px; }
th, td { padding: 6px 13px; border: 1px solid var(--color-border); }
th { font-weight: 600; background: var(--color-code-bg); }
tr:nth-child(2n) { background: var(--color-code-bg); }
hr { height: 0.25em; padding: 0; margin: 24px 0; background: var(--color-border); border: 0; border-radius: 2px; }
ul, ol { padding-left: 2em; margin-bottom: 16px; }
li + li { margin-top: 0.25em; }
input[type="checkbox"] { margin-right: 0.5em; }
/* YAML frontmatter displayed as code */
.frontmatter { background: var(--color-code-bg); padding: 12px 16px; border-radius: 6px; margin-bottom: 24px; font-size: 85%; font-family: monospace; color: var(--color-blockquote); border-left: 4px solid var(--color-border); white-space: pre-wrap; }
/* In-document search highlight — applied by highlightMatches() below */
mark.npp-find { background: #ffeb3b; color: #000; padding: 0 2px; border-radius: 2px; box-shadow: 0 0 0 1px rgba(0,0,0,0.1); }
mark.npp-find.current { background: #ff9800; box-shadow: 0 0 0 2px rgba(255,152,0,0.35); }
@media (prefers-color-scheme: dark) {
  mark.npp-find { background: #ffd54f; color: #000; }
  mark.npp-find.current { background: #ffb300; }
}
)CSS";

static void buildTemplate() {
    std::string markedJS    = readFileToString(sResourcesDir + "/marked.min.js");
    std::string hljsJS      = readFileToString(sResourcesDir + "/highlight.min.js");
    std::string hljsLightCSS = readFileToString(sResourcesDir + "/hljs-github.css");
    std::string hljsDarkCSS  = readFileToString(sResourcesDir + "/hljs-github-dark.css");

    if (markedJS.empty()) {
        g_warning("[MarkdownPanel] ERROR: marked.min.js not found in %s", sResourcesDir.c_str());
        return;
    }

    std::string mermaidJS;
    if (sSettings.enableMermaid)
        mermaidJS = readFileToString(sResourcesDir + "/mermaid.min.js");

    std::string html = "<!DOCTYPE html>\n<html><head>\n<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";

    html += "<style>\n";
    html += kGitHubMarkdownCSS;
    html += "\n</style>\n";
    html += "<style media=\"(prefers-color-scheme: light)\">\n" + hljsLightCSS + "\n</style>\n";
    html += "<style media=\"(prefers-color-scheme: dark)\">\n" + hljsDarkCSS + "\n</style>\n";

    html += "<style>body { zoom: " + std::to_string(sSettings.zoomLevel) + "%; }</style>\n";
    html += "<style>.squared-diagram{margin:16px 0;} .squared-diagram svg{max-width:100%;height:auto;}"
            " pre.squared-pending{background:transparent;border:none;padding:6px 0;}"
            " pre.squared-pending code{display:none;}"
            " pre.squared-pending::after{content:\"Rendering diagram\\2026\";color:#888;font-style:italic;}</style>\n";

    html += "<script>\n" + markedJS + "\n</script>\n";
    if (!hljsJS.empty())
        html += "<script>\n" + hljsJS + "\n</script>\n";
    if (!mermaidJS.empty())
        html += "<script>\n" + mermaidJS + "\n</script>\n";

    // The "squared" diagram engine, loaded as a file:// ES module from the
    // plugin resources (no network). Same mechanics as macOS: squared.js's own
    // imports + its Web Worker resolve relative to this file:// URL. WebKitGTK's
    // allow-file-access/allow-universal-access settings (set on the WebView)
    // stand in for WKWebView's read-access scoping.
    if (sSettings.enableSquared) {
        gchar *jsURL = g_filename_to_uri((sResourcesDir + "/squared/squared.js").c_str(), NULL, NULL);
        if (jsURL) {
            html += "<script>window.__squaredIntended=true;window.__squaredTheme='"
                  + sSettings.squaredTheme + "';</script>\n";
            html += "<script type=\"module\">\n";
            html += "try{\n";
            html += "  const m = await import('" + std::string(jsURL) + "');\n";
            html += "  window.renderSquared = m.renderSquared;\n";
            html += "  if (m.initSquared) m.initSquared();\n";
            html += "  if (window.renderDiagrams) window.renderDiagrams();\n";
            html += "}catch(e){ console.error('[squared] engine load failed', e); }\n";
            html += "</script>\n";
            g_free(jsURL);
        }
    }

    // Application JS — VERBATIM from the macOS template (rendering, scroll
    // sync block map, in-document search). See NppMarkdownPanel.mm for the
    // extensively-commented original; comments trimmed here, logic identical.
    html += R"HTML(
<script>
window.renderDiagrams = function() {
  var blocks = document.querySelectorAll('pre code.language-mermaid');
  if (!blocks.length) return;
  if (window.__squaredIntended) {
    var ready = (typeof window.renderSquared === 'function');
    blocks.forEach(function(el) {
      if (el.getAttribute('data-sq')) return;
      var pre = el.parentNode, src = el.textContent;
      pre.classList.add('squared-pending');
      if (!ready) return;
      el.setAttribute('data-sq', '1');
      window.renderSquared(src, { theme: window.__squaredTheme || 'boardroom' }).then(function(res) {
        if (res && res.ok && res.svg && pre.parentNode) {
          var wrap = document.createElement('div');
          wrap.className = 'squared-diagram';
          if (pre.id) wrap.id = pre.id;
          wrap.innerHTML = res.svg;
          pre.parentNode.replaceChild(wrap, pre);
        } else {
          pre.classList.remove('squared-pending');
        }
      }).catch(function(e) { console.error('[squared] render', e); pre.classList.remove('squared-pending'); });
    });
  } else if (typeof mermaid !== 'undefined') {
    blocks.forEach(function(el) {
      var pre = el.parentNode;
      pre.classList.add('mermaid');
      pre.innerHTML = el.textContent;
    });
    try { mermaid.run({ querySelector: '.mermaid' }); } catch (e) {}
  }
};

function renderMarkdown(md) {
  var content = md;
  var frontmatter = '';
  if (md.startsWith('---\n') || md.startsWith('---\r\n')) {
    var endIdx = md.indexOf('\n---', 3);
    if (endIdx === -1) endIdx = md.indexOf('\r\n---', 3);
    if (endIdx > 0) {
      var fmEnd = md.indexOf('\n', endIdx + 1);
      if (fmEnd === -1) fmEnd = md.length;
      frontmatter = md.substring(4, endIdx).trim();
      content = md.substring(fmEnd + 1);
    }
  }

  var html = '';
  if (frontmatter) {
    html += '<div class="frontmatter">' +
      frontmatter.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;') +
      '</div>\n';
  }

  html += marked.parse(content, {gfm: true, breaks: false});

  var blockIdx = 0;
  html = html.replace(/<(h[1-6]|p|pre|ul|ol|table|blockquote|hr)([\s>])/g,
    function(match, tag, after) {
      return '<' + tag + ' id="block-' + (blockIdx++) + '"' + after;
    });

  document.getElementById('content').innerHTML = html;

  // Cache-bust local images: append ?v=<gen> so an image edited on disk gets
  // a fresh cache key (native bumps window._imgGen; the file loader ignores
  // the query). Remote and data: URLs untouched; gen 0 keeps pristine URLs.
  var _gen = window._imgGen || 0;
  if (_gen > 0) {
    document.querySelectorAll('#content img').forEach(function(img) {
      var src = img.getAttribute('src') || '';
      if (!src || /^(https?|data):/i.test(src)) return;
      img.setAttribute('src', src + (src.indexOf('?') >= 0 ? '&' : '?') + 'v=' + _gen);
    });
  }

  var newBlockMap = _buildBlockMap(md, content);
  var actualBlockCount = document.querySelectorAll('[id^="block-"]').length;
  if (newBlockMap.length === actualBlockCount && actualBlockCount > 0) {
    window._blockMap = newBlockMap;
  } else {
    if (newBlockMap.length !== actualBlockCount) {
      console.warn('[NppMarkdownPanel] blockMap/DOM mismatch (' +
        newBlockMap.length + ' vs ' + actualBlockCount +
        ') — falling back to proportional');
    }
    window._blockMap = [];
  }

  if (typeof hljs !== 'undefined') {
    document.querySelectorAll('pre code').forEach(function(el) {
      hljs.highlightElement(el);
    });
  }

  if (window.renderDiagrams) window.renderDiagrams();

  window._totalLines = md.split('\n').length;

  if (window._searchQuery) {
    highlightMatches(window._searchQuery);
  }
}

window._searchQuery = '';
function _escRegex(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

function _clearHighlights() {
  var marks = document.querySelectorAll('mark.npp-find');
  marks.forEach(function(m) {
    var parent = m.parentNode;
    if (!parent) return;
    while (m.firstChild) parent.insertBefore(m.firstChild, m);
    parent.removeChild(m);
    parent.normalize();
  });
}

function highlightMatches(query) {
  _clearHighlights();
  window._searchQuery = query || '';
  if (!query) return 0;

  var re = new RegExp(_escRegex(query), 'gi');
  var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
    acceptNode: function(node) {
      if (!node.nodeValue || !node.nodeValue.trim()) return NodeFilter.FILTER_REJECT;
      var p = node.parentElement;
      while (p) {
        var tag = p.tagName;
        if (tag === 'SCRIPT' || tag === 'STYLE') return NodeFilter.FILTER_REJECT;
        if (p.classList && p.classList.contains('hljs')) return NodeFilter.FILTER_REJECT;
        p = p.parentElement;
      }
      return NodeFilter.FILTER_ACCEPT;
    }
  });

  var targets = [];
  var n;
  while ((n = walker.nextNode())) {
    if (re.test(n.nodeValue)) targets.push(n);
    re.lastIndex = 0;
  }

  var count = 0;
  targets.forEach(function(node) {
    var text = node.nodeValue;
    var frag = document.createDocumentFragment();
    var lastIdx = 0;
    var m;
    re.lastIndex = 0;
    while ((m = re.exec(text))) {
      if (m.index > lastIdx) {
        frag.appendChild(document.createTextNode(text.slice(lastIdx, m.index)));
      }
      var mark = document.createElement('mark');
      mark.className = 'npp-find';
      mark.textContent = m[0];
      frag.appendChild(mark);
      lastIdx = re.lastIndex;
      count++;
      if (m.index === re.lastIndex) re.lastIndex++;
    }
    if (lastIdx < text.length) {
      frag.appendChild(document.createTextNode(text.slice(lastIdx)));
    }
    if (node.parentNode) node.parentNode.replaceChild(frag, node);
  });

  var first = document.querySelector('mark.npp-find');
  if (first) {
    first.classList.add('current');
    first.scrollIntoView({block: 'center', inline: 'nearest'});
  }
  return count;
}

window._blockMap = [];

function _buildBlockMap(md, content) {
  var prefixLen = md.length - content.length;
  var contentStartLine = 0;
  if (prefixLen > 0) {
    contentStartLine = (md.substring(0, prefixLen).match(/\n/g) || []).length;
  }

  var blockMap = [];
  var blockIdx = 0;
  var tokens;
  try {
    tokens = marked.lexer(content);
  } catch (e) {
    return [];
  }

  var BLOCK_TYPES = {
    heading: 1, paragraph: 1, code: 1, list: 1,
    table: 1, blockquote: 1, hr: 1
  };

  function rawNewlines(tok) {
    return (tok && tok.raw && tok.raw.match(/\n/g))
             ? tok.raw.match(/\n/g).length : 0;
  }

  function walk(toks, lineCursor, insideLooseListItem) {
    if (!toks) return;
    for (var i = 0; i < toks.length; i++) {
      var tok = toks[i];
      var lines = rawNewlines(tok);
      var startLine = lineCursor;

      if (tok.type === 'space' || tok.type === 'def') {
        lineCursor += lines;
        continue;
      }

      if (BLOCK_TYPES[tok.type]) {
        blockMap.push({ line: startLine, id: 'block-' + blockIdx });
        blockIdx++;
      } else if (insideLooseListItem && tok.type === 'text') {
        blockMap.push({ line: startLine, id: 'block-' + blockIdx });
        blockIdx++;
      }

      if (tok.type === 'blockquote' && tok.tokens) {
        walk(tok.tokens, startLine, false);
      }
      if (tok.type === 'list' && tok.loose && tok.items) {
        var itemLineCursor = startLine;
        for (var j = 0; j < tok.items.length; j++) {
          var item = tok.items[j];
          if (item.tokens) walk(item.tokens, itemLineCursor, true);
          itemLineCursor += rawNewlines(item);
        }
      }

      lineCursor += lines;
    }
  }

  walk(tokens, contentStartLine, false);
  return blockMap;
}

function _resolveBlockTargetY(lineNo, bm, maxScroll) {
  var lo = 0, hi = bm.length - 1, k = -1;
  while (lo <= hi) {
    var mid = (lo + hi) >> 1;
    if (bm[mid].line <= lineNo) { k = mid; lo = mid + 1; }
    else                         { hi = mid - 1; }
  }
  if (k < 0) return 0;

  var blockEl = document.getElementById(bm[k].id);
  if (!blockEl) return 0;

  var startY = blockEl.offsetTop;

  if (k + 1 < bm.length) {
    var nextEl = document.getElementById(bm[k + 1].id);
    if (nextEl) {
      var lineSpan = bm[k + 1].line - bm[k].line;
      if (lineSpan > 0) {
        var t = Math.max(0, Math.min(1, (lineNo - bm[k].line) / lineSpan));
        startY = startY + t * (nextEl.offsetTop - startY);
      }
    }
  } else {
    var lineSpan = (window._totalLines - 1) - bm[k].line;
    if (lineSpan > 0) {
      var t = Math.max(0, Math.min(1, (lineNo - bm[k].line) / lineSpan));
      startY = startY + t * (maxScroll - startY);
    }
  }

  return Math.max(0, Math.min(maxScroll, Math.round(startY)));
}

var _scrollTimer = null;
function scrollToLine(lineNo) {
  if (!window._totalLines || window._totalLines <= 1) return;
  if (_scrollTimer) { clearTimeout(_scrollTimer); }
  _scrollTimer = setTimeout(function() {
    var maxScroll = Math.max(0,
      document.documentElement.scrollHeight - window.innerHeight);
    var targetY;
    var bm = window._blockMap || [];
    if (bm.length > 0) {
      targetY = _resolveBlockTargetY(lineNo, bm, maxScroll);
    } else {
      var ratio = Math.max(0, Math.min(1, lineNo / (window._totalLines - 1)));
      targetY = Math.round(ratio * maxScroll);
    }
    // Mask this programmatic scroll from the reverse-sync reporter: smooth
    // scrolling emits a stream of scroll events until it settles, and none
    // of them may be echoed back to the editor (feedback-loop guard #2).
    if (Math.abs(window.scrollY - targetY) > 2) {
      window._progTargetY = targetY;
      window._progTargetTs = Date.now();
    }
    window.scrollTo({ top: targetY, behavior: 'smooth' });
    _scrollTimer = null;
  }, 50);
}

function scrollToTop() {
  window.scrollTo({top: 0, behavior: 'smooth'});
}

// ───────────────── Preview → editor bridge ─────────────────
// Posts { type:'scroll', line } while the USER scrolls the preview, and
// { type:'wordTap', ... } on double-click. Feedback-loop protection is
// layered on both sides of the bridge:
//   JS  #1: positive intent gate — nothing is reported unless real input
//           (wheel / scrollbar mousedown / keydown) happened in the last
//           300 ms; programmatic scrolls produce none of these.
//   JS  #2: scrollToLine() masks its own smooth-scroll event stream via
//           window._progTargetY until the target settles (or 700 ms).
//   Native: applyPreviewScrollToEditor() pre-updates the forward-sync
//           trackers and keeps a ±1-line dead-band (see the .cpp side).
var _bridge = (window.webkit && window.webkit.messageHandlers)
                ? window.webkit.messageHandlers.nppmd : null;
window._progTargetY  = null;
window._progTargetTs = 0;
var _lastUserInputTs = 0;
['wheel', 'mousedown', 'keydown'].forEach(function(evt) {
  window.addEventListener(evt, function() { _lastUserInputTs = Date.now(); },
                          { passive: true, capture: true });
});

// Inverse of _resolveBlockTargetY: current scroll Y → source line, using the
// same blockMap with interpolation between block tops (proportional fallback
// when the map is empty).
function _lineFromScrollY(y) {
  var bm = window._blockMap || [];
  var maxScroll = Math.max(1,
    document.documentElement.scrollHeight - window.innerHeight);
  var total = (window._totalLines || 1) - 1;
  if (!bm.length) {
    return Math.round(Math.max(0, Math.min(1, y / maxScroll)) * total);
  }
  var lo = 0, hi = bm.length - 1, k = 0;
  while (lo <= hi) {
    var mid = (lo + hi) >> 1;
    var el = document.getElementById(bm[mid].id);
    var top = el ? el.offsetTop : 0;
    if (top <= y) { k = mid; lo = mid + 1; } else { hi = mid - 1; }
  }
  var elK = document.getElementById(bm[k].id);
  if (!elK) return bm[k].line;
  var topK = elK.offsetTop;
  var line = bm[k].line;
  if (k + 1 < bm.length) {
    var elN = document.getElementById(bm[k + 1].id);
    if (elN && elN.offsetTop > topK) {
      var t = Math.max(0, Math.min(1, (y - topK) / (elN.offsetTop - topK)));
      line = bm[k].line + t * (bm[k + 1].line - bm[k].line);
    }
  } else if (maxScroll > topK) {
    var t2 = Math.max(0, Math.min(1, (y - topK) / (maxScroll - topK)));
    line = bm[k].line + t2 * (total - bm[k].line);
  }
  return Math.max(0, Math.round(line));
}

var _revScrollTimer = null;
window.addEventListener('scroll', function() {
  if (!_bridge) return;
  var y = window.scrollY;
  if (window._progTargetY !== null) {          // guard #2: our own smooth scroll
    if (Math.abs(y - window._progTargetY) <= 2 ||
        Date.now() - window._progTargetTs > 700) {
      window._progTargetY = null;
    }
    return;
  }
  if (Date.now() - _lastUserInputTs > 300) return;   // guard #1: no user intent
  if (_revScrollTimer) clearTimeout(_revScrollTimer);
  _revScrollTimer = setTimeout(function() {
    _revScrollTimer = null;
    _bridge.postMessage({ type: 'scroll', line: _lineFromScrollY(window.scrollY) });
  }, 100);
}, { passive: true });

// Double-click a word in the preview → select it in the source. Always on
// (explicit gesture, no loop potential). Best-effort by design: preview
// text differs from source text inside link labels/emphasis, so native
// falls back from "same occurrence" to "first occurrence in the block".
document.addEventListener('dblclick', function() {
  if (!_bridge) return;
  var sel = window.getSelection();
  if (!sel || sel.isCollapsed || sel.rangeCount === 0) return;
  var word = sel.toString().trim();
  if (!word || word.length > 200 || /\s/.test(word)) return;
  var node = sel.anchorNode;
  var el = (node && node.nodeType === 3) ? node.parentElement : node;
  if (!el || !el.closest) return;
  if (el.closest('svg, .mermaid')) return;      // diagrams have no text mapping
  var block = el.closest('[id^="block-"]');
  if (!block) return;
  var bm = window._blockMap || [];
  var idx = -1;
  for (var i = 0; i < bm.length; i++) {
    if (bm[i].id === block.id) { idx = i; break; }
  }
  if (idx < 0) return;
  var startLine = bm[idx].line;
  var endLine = (idx + 1 < bm.length)
                  ? Math.max(startLine, bm[idx + 1].line - 1)
                  : ((window._totalLines || startLine + 1) - 1);
  // Count occurrences of the word in this block BEFORE the selection, so
  // native can pick the matching occurrence in the source range.
  var occ = 0;
  try {
    var r = sel.getRangeAt(0);
    var pre = document.createRange();
    pre.selectNodeContents(block);
    pre.setEnd(r.startContainer, r.startOffset);
    var esc = word.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    var m = pre.toString().match(new RegExp(esc, 'g'));
    occ = m ? m.length : 0;
  } catch (e) { occ = 0; }
  _bridge.postMessage({ type: 'wordTap', word: word,
                        startLine: startLine, endLine: endLine, occ: occ });
}, true);

if (typeof mermaid !== 'undefined') {
  mermaid.initialize({startOnLoad: false, theme: 'default'});
}
</script>
</head>
<body>
<div id="content"><p style="color: #888; font-style: italic;">Markdown preview will appear here...</p></div>
</body>
</html>
)HTML";

    sFullTemplate = html;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Settings persistence (GKeyFile INI — see PORTING_NOTES.md on why not JSON;
//  keys mirror the macOS JSON keys 1:1)
// ═══════════════════════════════════════════════════════════════════════════

static std::string getConfigPath() {
    char buf[2048] = {};
    npp(NPPM_GETPLUGINSCONFIGDIR, sizeof(buf), (long)(intptr_t)buf);
    if (!buf[0]) return "";
    return std::string(buf) + "/NppMarkdownPanel.ini";
}

static void loadSettings() {
    std::string path = getConfigPath();
    if (path.empty()) return;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        int z = g_key_file_get_integer(kf, "markdown", "zoomLevel", &e);
        if (!e) sSettings.zoomLevel = z; else g_clear_error(&e);
        #define LOAD_BOOL(field, key) do { \
            gboolean v = g_key_file_get_boolean(kf, "markdown", key, &e); \
            if (!e) sSettings.field = v; else g_clear_error(&e); } while (0)
        LOAD_BOOL(autoShowPanel,            "autoShowPanel");
        LOAD_BOOL(syncWithCaret,            "syncWithCaret");
        LOAD_BOOL(syncWithFirstVisibleLine, "syncWithFirstVisibleLine");
        LOAD_BOOL(allowAllExtensions,       "allowAllExtensions");
        LOAD_BOOL(enableMermaid,            "enableMermaid");
        LOAD_BOOL(enableSquared,            "enableSquared");
        LOAD_BOOL(syncPreviewToEditor,      "syncPreviewToEditor");
        #undef LOAD_BOOL
        gchar *s = g_key_file_get_string(kf, "markdown", "supportedExtensions", NULL);
        if (s) { sSettings.supportedExtensions = s; g_free(s); }
        s = g_key_file_get_string(kf, "markdown", "squaredTheme", NULL);
        if (s) { sSettings.squaredTheme = s; g_free(s); }

        // Migration (macOS parity): ensure .mmd is in the extensions list.
        {
            std::string lower = sSettings.supportedExtensions;
            for (auto &c : lower) c = tolower(c);
            if (lower.find("mmd") == std::string::npos)
                sSettings.supportedExtensions += ",mmd";
        }
        // Migration (macOS parity): the sync modes used to be exclusive.
        if (sSettings.syncWithCaret || sSettings.syncWithFirstVisibleLine) {
            sSettings.syncWithCaret            = true;
            sSettings.syncWithFirstVisibleLine = true;
        }
    }
    g_key_file_free(kf);
}

static void saveSettings() {
    std::string path = getConfigPath();
    if (path.empty()) return;
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "markdown", "zoomLevel",   sSettings.zoomLevel);
    g_key_file_set_boolean(kf, "markdown", "autoShowPanel", sSettings.autoShowPanel);
    g_key_file_set_boolean(kf, "markdown", "syncWithCaret", sSettings.syncWithCaret);
    g_key_file_set_boolean(kf, "markdown", "syncWithFirstVisibleLine", sSettings.syncWithFirstVisibleLine);
    g_key_file_set_boolean(kf, "markdown", "allowAllExtensions", sSettings.allowAllExtensions);
    g_key_file_set_string (kf, "markdown", "supportedExtensions", sSettings.supportedExtensions.c_str());
    g_key_file_set_boolean(kf, "markdown", "enableMermaid", sSettings.enableMermaid);
    g_key_file_set_boolean(kf, "markdown", "enableSquared", sSettings.enableSquared);
    g_key_file_set_string (kf, "markdown", "squaredTheme",  sSettings.squaredTheme.c_str());
    g_key_file_set_boolean(kf, "markdown", "syncPreviewToEditor", sSettings.syncPreviewToEditor);
    g_key_file_save_to_file(kf, path.c_str(), NULL);
    g_key_file_free(kf);
}

// ═══════════════════════════════════════════════════════════════════════════
//  JS plumbing
// ═══════════════════════════════════════════════════════════════════════════

// Fire-and-forget JS in the panel WebView.
static void runJS(WebKitWebView *view, const std::string &js) {
    if (!view) return;
    webkit_web_view_evaluate_javascript(view, js.c_str(), -1, NULL, NULL,
                                        NULL, NULL, NULL);
}

// JSON-escape a UTF-8 string into a double-quoted JS string literal
// (replaces the macOS NSJSONSerialization trick).
static std::string jsonEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 16);
    out += '"';
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    g_snprintf(hex, sizeof hex, "\\u%04x", c);
                    out += hex;
                } else {
                    out += (char)c;
                }
        }
    }
    // U+2028/U+2029 are legal in JSON strings (and in JS string literals
    // since ES2019) — no special-casing needed for a "..." literal.
    out += '"';
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
//  WebView panel
// ═══════════════════════════════════════════════════════════════════════════

// External links open in the default browser; template loads and JS-driven
// navigations pass through (mirrors MarkdownNavigationDelegate).
static gboolean on_decide_policy(WebKitWebView *, WebKitPolicyDecision *dec,
                                 WebKitPolicyDecisionType type, gpointer) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
    WebKitNavigationPolicyDecision *nav = WEBKIT_NAVIGATION_POLICY_DECISION(dec);
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(nav);
    WebKitNavigationType nt = webkit_navigation_action_get_navigation_type(action);
    if (nt == WEBKIT_NAVIGATION_TYPE_OTHER || nt == WEBKIT_NAVIGATION_TYPE_RELOAD) {
        webkit_policy_decision_use(dec);
        return TRUE;
    }
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
    if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
        GtkUriLauncher *l = gtk_uri_launcher_new(uri);
        gtk_uri_launcher_launch(l, NULL, NULL, NULL, NULL);
        g_object_unref(l);
    }
    webkit_policy_decision_ignore(dec);
    return TRUE;
}

// Template finished loading — renderMarkdown() + the JS libs exist now.
// Flush the current document (the macOS cold-WebKit-start race fix, same idea).
static void on_load_changed(WebKitWebView *, WebKitLoadEvent ev, gpointer) {
    if (ev == WEBKIT_LOAD_FINISHED) {
        sWebViewReady = true;
        renderMarkdownDirect();
    }
}

static gboolean on_load_failed(WebKitWebView *, WebKitLoadEvent, char *, GError *, gpointer) {
    sWebViewReady = true;   // never stay permanently "not ready"
    return FALSE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer) {
    // GtkSearchEntry already debounces keystrokes (~150 ms) before emitting
    // search-changed — no manual dispatch dance needed.
    const char *q = gtk_editable_get_text(GTK_EDITABLE(entry));
    runJS(sWebView, "if (typeof highlightMatches === 'function') highlightMatches(" +
                    jsonEscape(q ? q : "") + ");");
}

// Escape in the search field clears the query and the highlights (macOS parity;
// GTK4's GtkSearchEntry emits stop-search on Escape but does NOT auto-clear).
static void on_search_stop(GtkSearchEntry *entry, gpointer) {
    gtk_editable_set_text(GTK_EDITABLE(entry), "");   // re-fires search-changed → clears highlights
}

static void on_btn_settings(GtkButton *, gpointer) { showSettingsCmd(); }
static void on_btn_refresh (GtkButton *, gpointer) { refreshMarkdownPreview(); }
static void on_btn_pdf     (GtkButton *, gpointer) { saveMarkdownAsPDF(); }
static void on_btn_print   (GtkButton *, gpointer) { printMarkdownPreview(); }

// Ctrl +/- (and Ctrl 0) zoom the preview live — only while focus is inside
// the panel (the controller is on sContentBox), so the editor's own zoom
// shortcuts are never hijacked. Mirrors the macOS local key monitor.
static gboolean on_panel_key(GtkEventControllerKey *, guint keyval, guint,
                             GdkModifierType state, gpointer) {
    if (!(state & GDK_CONTROL_MASK)) return FALSE;
    switch (keyval) {
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
            adjustPreviewZoom(+10); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_underscore: case GDK_KEY_KP_Subtract:
            adjustPreviewZoom(-10); return TRUE;
        case GDK_KEY_0: case GDK_KEY_KP_0:
            adjustPreviewZoom(100 - sSettings.zoomLevel); return TRUE;
    }
    return FALSE;
}

static GtkWidget *panelButton(const char *icon, const char *tooltip,
                              GCallback cb) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

// ─────────────────────────────────────────────────────────────────────────────
// JS → native bridge. The template posts to window.webkit.messageHandlers.
// nppmd (the SAME JS surface WebKitGTK exposes for a registered script-message
// handler); the signal delivers a JSCValue on the main thread. Two message
// types today, mirroring the macOS _NMPScriptBridge:
//   { type:'scroll',  line }                          — reverse scroll sync
//   { type:'wordTap', word, startLine, endLine, occ } — double-click locate
// ─────────────────────────────────────────────────────────────────────────────

static bool jscPropNumber(JSCValue *obj, const char *name, intptr_t *out) {
    JSCValue *v = jsc_value_object_get_property(obj, name);
    bool ok = v && jsc_value_is_number(v);
    if (ok) *out = (intptr_t)jsc_value_to_double(v);
    if (v) g_object_unref(v);
    return ok;
}

static void on_script_message(WebKitUserContentManager *, JSCValue *value, gpointer) {
    if (!value || !jsc_value_is_object(value)) return;
    JSCValue *tv = jsc_value_object_get_property(value, "type");
    gchar *type = (tv && jsc_value_is_string(tv)) ? jsc_value_to_string(tv) : NULL;
    if (tv) g_object_unref(tv);
    if (!type) return;

    if (g_strcmp0(type, "scroll") == 0) {
        intptr_t line = 0;
        if (jscPropNumber(value, "line", &line))
            applyPreviewScrollToEditor(line);
    } else if (g_strcmp0(type, "wordTap") == 0) {
        intptr_t s = 0, e = 0, occ = 0;
        JSCValue *wv = jsc_value_object_get_property(value, "word");
        gchar *word = (wv && jsc_value_is_string(wv)) ? jsc_value_to_string(wv) : NULL;
        if (wv) g_object_unref(wv);
        if (word && jscPropNumber(value, "startLine", &s) &&
                    jscPropNumber(value, "endLine", &e)) {
            jscPropNumber(value, "occ", &occ);   // optional, defaults 0
            locateWordFromPreview(word, s, e, occ);
        }
        g_free(word);
    }
    g_free(type);
}

// ─────────────────────────────────────────────────────────────────────────────
// Application re-activation — the Linux stand-in for macOS's
// NSApplicationDidBecomeActiveNotification. "Edited the image in another app
// and switched back" is the moment stale images are actually noticed: bump the
// cache-buster and re-render. GTK has no app-level activation signal, so we
// watch notify::is-active on the panel's toplevel (the host window when
// docked, the float when detached — re-tracked on every map, since dock/float
// reparents the panel). A window losing focus to ANOTHER of our toplevels
// (find window, dialogs…) is NOT an app deactivation — a short probe checks
// that no app toplevel is active before arming, so in-app window switches
// don't trigger full re-renders (mermaid would visibly re-run).
// ─────────────────────────────────────────────────────────────────────────────

static GtkWindow *sActiveWin        = NULL;   // weak — window we listen on
static gulong     sActiveWinHandler = 0;
static bool       sAppWasInactive   = false;
static guint      sAppInactiveProbe = 0;      // g_timeout source id (0 = none)

static gboolean appInactiveProbeCb(gpointer) {
    sAppInactiveProbe = 0;
    GListModel *tops = gtk_window_get_toplevels();
    guint n = g_list_model_get_n_items(tops);
    bool anyActive = false;
    for (guint i = 0; i < n && !anyActive; i++) {
        gpointer item = g_list_model_get_item(tops, i);   // strong ref
        if (item) {
            anyActive = GTK_IS_WINDOW(item) && gtk_window_is_active(GTK_WINDOW(item));
            g_object_unref(item);
        }
    }
    if (!anyActive) sAppWasInactive = true;
    return G_SOURCE_REMOVE;
}

static void on_root_active_changed(GObject *win, GParamSpec *, gpointer) {
    if (gtk_window_is_active(GTK_WINDOW(win))) {
        if (sAppInactiveProbe) { g_source_remove(sAppInactiveProbe); sAppInactiveProbe = 0; }
        if (!sAppWasInactive) return;
        sAppWasInactive = false;
        if (!sPanelVisible || !sWebView || !sWebViewReady) return;
        sImageGeneration++;
        sLastRenderedText.clear();   // defeat the no-change early-out
        renderMarkdownDeferred();
    } else {
        // Deactivated — but focus may just be moving to another app window.
        // Probe shortly after, once the focus transfer has settled.
        if (sAppInactiveProbe) g_source_remove(sAppInactiveProbe);
        sAppInactiveProbe = g_timeout_add(60, appInactiveProbeCb, NULL);
    }
}

// (Re)attach the is-active watcher to the panel's CURRENT toplevel. Called
// from the panel's map handler — map fires again after every dock/float
// reparent, so the watcher follows the panel across toplevels.
static void panelTrackActiveWindow() {
    GtkRoot *root = sContentBox ? gtk_widget_get_root(sContentBox) : NULL;
    GtkWindow *win = (root && GTK_IS_WINDOW(root)) ? GTK_WINDOW(root) : NULL;
    if (win == sActiveWin) return;
    if (sActiveWin) {   // weak ptr — NULLed automatically if the float died
        g_signal_handler_disconnect(sActiveWin, sActiveWinHandler);
        g_object_remove_weak_pointer(G_OBJECT(sActiveWin), (gpointer *)&sActiveWin);
        sActiveWin = NULL;
        sActiveWinHandler = 0;
    }
    if (!win) return;
    sActiveWin = win;
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&sActiveWin);
    sActiveWinHandler = g_signal_connect(win, "notify::is-active",
                                         G_CALLBACK(on_root_active_changed), NULL);
}

static void on_panel_map(GtkWidget *, gpointer) { panelTrackActiveWindow(); }

// Build (once) the panel content: search/buttons toolbar row + WebView.
// Layout mirrors the macOS panel:
//   [search field ▸ expandable] [settings] [refresh] [save-PDF] [print]
//   ─────────────────────────────────────────────────
//   [WebKitWebView — fills the rest]
static void ensureContentView() {
    if (sContentBox) return;

    sContentBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(row, 6);
    gtk_widget_set_margin_end(row, 6);
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);
    gtk_box_append(GTK_BOX(sContentBox), row);

    sSearchEntry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(sSearchEntry),
                                          "Search in document...");
    gtk_widget_set_hexpand(sSearchEntry, TRUE);
    g_signal_connect(sSearchEntry, "search-changed",
                     G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(sSearchEntry, "stop-search",
                     G_CALLBACK(on_search_stop), NULL);
    gtk_box_append(GTK_BOX(row), sSearchEntry);

    gtk_box_append(GTK_BOX(row), panelButton("emblem-system-symbolic",
        "Settings", G_CALLBACK(on_btn_settings)));
    gtk_box_append(GTK_BOX(row), panelButton("view-refresh-symbolic",
        "Refresh preview", G_CALLBACK(on_btn_refresh)));
    gtk_box_append(GTK_BOX(row), panelButton("document-save-symbolic",
        "Save as PDF", G_CALLBACK(on_btn_pdf)));
    gtk_box_append(GTK_BOX(row), panelButton("document-print-symbolic",
        "Print preview", G_CALLBACK(on_btn_print)));

    sWebView = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *ws = webkit_web_view_get_settings(sWebView);
    // file:// preview page must reach other file:// trees (relative images,
    // the squared engine + its Worker) — the WebKitGTK equivalent of the
    // WKWebView read-access/KVC keys on macOS.
    webkit_settings_set_allow_file_access_from_file_urls(ws, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(ws, TRUE);
    webkit_settings_set_enable_developer_extras(ws, TRUE);

    g_signal_connect(sWebView, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(sWebView, "load-changed",  G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(sWebView, "load-failed",   G_CALLBACK(on_load_failed), NULL);

    // JS → native message channel (reverse scroll sync + double-click word
    // locate). Registered on the view's content manager BEFORE the first
    // load, so window.webkit.messageHandlers.nppmd exists in every page.
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(sWebView);
    webkit_user_content_manager_register_script_message_handler(ucm, "nppmd", NULL);
    g_signal_connect(ucm, "script-message-received::nppmd",
                     G_CALLBACK(on_script_message), NULL);

    gtk_widget_set_vexpand(GTK_WIDGET(sWebView), TRUE);
    gtk_box_append(GTK_BOX(sContentBox), GTK_WIDGET(sWebView));

    // App re-activation watcher (image cache-buster) — see panelTrackActiveWindow.
    g_signal_connect(sContentBox, "map", G_CALLBACK(on_panel_map), NULL);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_panel_key), NULL);
    gtk_widget_add_controller(sContentBox, keys);
}

// Runtime check — is the preview actually on screen? The host hides the
// panel_frame both for NPPM_DMM_HIDEPANEL and for its own titlebar ✕ (which
// gives the plugin no callback), and either way our content unmaps. Reading
// the live mapped state self-corrects the menu toggle after a host-side hide.
static bool markdownPanelIsShown() {
    return sContentBox && gtk_widget_get_mapped(sContentBox);
}

static void loadTemplateIntoWebView() {
    if (!sWebView || sFullTemplate.empty()) return;

    // New navigation, same WebContent process → same memory cache. Bump the
    // image generation so this page's renders re-fetch local images (covers
    // the Refresh button, file switches, and settings reloads).
    sImageGeneration++;

    // Write the HTML into the SAME directory as the markdown file so relative
    // image paths resolve naturally (identical reasoning to the macOS build:
    // a <base> tag does not govern file:// image resolution).
    std::string fp = getCurrentFilePath();
    std::string tmpPath;
    if (!fp.empty()) {
        gchar *dir = g_path_get_dirname(fp.c_str());
        tmpPath = std::string(dir) + "/.npp-md-preview.html";
        g_free(dir);
    } else {
        tmpPath = std::string(g_get_tmp_dir()) + "/npp-md-preview.html";
    }

    if (!sCurrentTempHtmlPath.empty() && sCurrentTempHtmlPath != tmpPath)
        g_unlink(sCurrentTempHtmlPath.c_str());
    sCurrentTempHtmlPath = tmpPath;

    g_file_set_contents(tmpPath.c_str(), sFullTemplate.c_str(),
                        (gssize)sFullTemplate.size(), NULL);

    gchar *uri = g_filename_to_uri(tmpPath.c_str(), NULL, NULL);
    sWebViewReady = false;   // load-changed(FINISHED) flips it true
    webkit_web_view_load_uri(sWebView, uri);
    g_free(uri);

    sLastRenderedText.clear();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Markdown rendering
// ═══════════════════════════════════════════════════════════════════════════

static std::string getEditorText() {
    void *h = getCurScintilla();
    if (!h) return "";
    intptr_t len = sci(h, SCI_GETLENGTH);
    if (len <= 0) return "";
    if (len > 5 * 1024 * 1024) return ""; // Skip files > 5MB

    std::string buf(len + 1, '\0');
    sci(h, SCI_GETTEXT, (uintptr_t)(len + 1), (intptr_t)buf.data());
    buf.resize(len);
    return buf;
}

static gboolean deferredRenderCb(gpointer);

static void renderMarkdownDirect() {
    if (sExporting) return;   // paused during a silent export
    if (!sPanelVisible || !sWebView) return;
    if (!sWebViewReady) return;   // load-changed will call us again
    if (!isSupportedExtension()) {
        runJS(sWebView,
            "document.getElementById('content').innerHTML = "
            "'<p style=\"color:#888;font-style:italic;\">Current file is not a Markdown file.</p>';");
        return;
    }

    std::string text = getEditorText();
    if (text == sLastRenderedText) return;
    sLastRenderedText = text;

    // Standalone .mmd files: wrap in a ```mermaid fence (macOS parity).
    std::string ext = getCurrentExtension();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    for (auto &c : ext) c = tolower(c);
    if (ext == "mmd")
        text = "```mermaid\n" + text + "\n```\n";

    // File path changed → the temp HTML must move next to the new file for
    // relative images; full reload, then render once the template is back.
    std::string newPath = getCurrentFilePath();
    if (newPath != sCurrentFilePath) {
        sCurrentFilePath = newPath;
        loadTemplateIntoWebView();
        return;   // on_load_changed(FINISHED) re-enters renderMarkdownDirect
    }

    runJS(sWebView, "window._imgGen=" + std::to_string(sImageGeneration) +
                    "; renderMarkdown(" + jsonEscape(text) + ");");
}

static gboolean deferredRenderCb(gpointer) {
    sPendingRender = 0;
    renderMarkdownDirect();
    return G_SOURCE_REMOVE;
}

static void renderMarkdownDeferred() {
    if (sExporting) return;
    if (!sPanelVisible) return;
    if (sPendingRender) g_source_remove(sPendingRender);
    sPendingRender = g_timeout_add(RENDER_DEBOUNCE_MS, deferredRenderCb, NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Scroll synchronization (logic identical to the macOS build)
// ═══════════════════════════════════════════════════════════════════════════

static intptr_t sLastCaretLine        = -1;
static intptr_t sLastFirstVisibleLine = -1;
static intptr_t sPendingScrollLine    = -1;

static gboolean scrollFlushCb(gpointer) {
    sPendingScroll = 0;
    if (sWebView)
        runJS(sWebView, "scrollToLine(" + std::to_string(sPendingScrollLine) + ");");
    return G_SOURCE_REMOVE;
}

static void syncScroll() {
    if (!sPanelVisible || !sWebView) return;

    void *h = getCurScintilla();
    if (!h) return;

    intptr_t pos = sci(h, SCI_GETCURRENTPOS);
    intptr_t caretLine = sci(h, SCI_LINEFROMPOSITION, (uintptr_t)pos);
    intptr_t firstVisibleVis = sci(h, SCI_GETFIRSTVISIBLELINE);
    intptr_t firstVisibleDoc = sci(h, SCI_DOCLINEFROMVISIBLE, (uintptr_t)firstVisibleVis);

    intptr_t targetLine = -1;
    bool caretMoved        = sSettings.syncWithCaret &&
                             caretLine != sLastCaretLine;
    bool firstVisibleMoved = sSettings.syncWithFirstVisibleLine &&
                             firstVisibleDoc != sLastFirstVisibleLine;

    if (caretMoved)             targetLine = caretLine;
    else if (firstVisibleMoved) targetLine = firstVisibleDoc;
    else                        return;

    sLastCaretLine        = caretLine;
    sLastFirstVisibleLine = firstVisibleDoc;

    // Debounce — SCN_UPDATEUI fires very frequently (per paint on this
    // backend; the handler only READS sci state, no mutation → safe to run
    // from inside Paint, see reference_gtk4_gotchas).
    sPendingScrollLine = targetLine;
    if (sPendingScroll) g_source_remove(sPendingScroll);
    sPendingScroll = g_timeout_add(100, scrollFlushCb, NULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reverse sync: the preview reported a user scroll → move the editor.
// Feedback-loop guards on this side (the JS side has its own two):
//   - dead-band: a move of ≤1 doc line is quantization noise from the
//     pixel→line interpolation, not intent — applying it would let the two
//     panes "correct" each other in a limit cycle;
//   - tracker pre-update: refresh sLastCaretLine/sLastFirstVisibleLine with
//     the values the imminent SCN_UPDATEUI will observe, so syncScroll()
//     sees "no change" and does not echo this scroll back into the preview.
// ─────────────────────────────────────────────────────────────────────────────
static void applyPreviewScrollToEditor(intptr_t docLine) {
    if (!sSettings.syncPreviewToEditor) return;
    if (!sPanelVisible) return;
    void *h = getCurScintilla();
    if (!h) return;

    intptr_t lineCount = sci(h, SCI_GETLINECOUNT);
    if (lineCount <= 0) return;
    if (docLine < 0) docLine = 0;
    if (docLine > lineCount - 1) docLine = lineCount - 1;

    intptr_t curVis = sci(h, SCI_GETFIRSTVISIBLELINE);
    intptr_t curDoc = sci(h, SCI_DOCLINEFROMVISIBLE, (uintptr_t)curVis);
    if (docLine >= curDoc - 1 && docLine <= curDoc + 1) return;   // dead-band

    // Doc line → visible line handles word wrap and folds.
    intptr_t vis = sci(h, SCI_VISIBLEFROMDOCLINE, (uintptr_t)docLine);
    sci(h, SCI_SETFIRSTVISIBLELINE, (uintptr_t)vis);

    // Read BACK what Scintilla actually applied (it may clamp near EOF) so
    // the tracker matches exactly what the next SCN_UPDATEUI will report.
    intptr_t actualVis = sci(h, SCI_GETFIRSTVISIBLELINE);
    sLastFirstVisibleLine = sci(h, SCI_DOCLINEFROMVISIBLE, (uintptr_t)actualVis);
    intptr_t pos = sci(h, SCI_GETCURRENTPOS);
    sLastCaretLine = sci(h, SCI_LINEFROMPOSITION, (uintptr_t)pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Double-click in the preview: select `word` in the source, preferring the
// occ-th occurrence within the block's source-line range [startLine,endLine]
// (occ = occurrences seen before the clicked one in the preview block).
// Preview text ≠ source text inside link labels/emphasis, so this degrades
// deliberately: exact occurrence → last found in range → silent no-op.
// ─────────────────────────────────────────────────────────────────────────────
static void locateWordFromPreview(const char *word, intptr_t startLine,
                                  intptr_t endLine, intptr_t occ) {
    if (!sPanelVisible || !word || !*word) return;
    void *h = getCurScintilla();
    if (!h) return;

    intptr_t lineCount = sci(h, SCI_GETLINECOUNT);
    if (lineCount <= 0) return;
    if (startLine < 0) startLine = 0;
    if (startLine > lineCount - 1) startLine = lineCount - 1;
    if (endLine < startLine) endLine = startLine;
    if (endLine > lineCount - 1) endLine = lineCount - 1;

    intptr_t needleLen = (intptr_t)strlen(word);

    intptr_t rangeStart = sci(h, SCI_POSITIONFROMLINE, (uintptr_t)startLine);
    intptr_t rangeEnd   = sci(h, SCI_GETLINEENDPOSITION, (uintptr_t)endLine);
    if (rangeEnd <= rangeStart) return;

    sci(h, SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
    intptr_t foundStart = -1, foundEnd = -1;
    intptr_t searchPos = rangeStart;
    for (intptr_t i = 0; searchPos < rangeEnd; i++) {
        sci(h, SCI_SETTARGETSTART, (uintptr_t)searchPos);
        sci(h, SCI_SETTARGETEND, (uintptr_t)rangeEnd);
        intptr_t hit = sci(h, SCI_SEARCHINTARGET, (uintptr_t)needleLen, (intptr_t)word);
        if (hit < 0) break;
        foundStart = hit;
        foundEnd   = sci(h, SCI_GETTARGETEND);
        if (i == occ) break;                 // reached the matching occurrence
        searchPos = foundEnd;
    }
    if (foundStart < 0) return;              // nothing in range — stay silent

    sci(h, SCI_SETSEL, (uintptr_t)foundStart, (intptr_t)foundEnd);
    sci(h, SCI_SCROLLCARET);

    // The user is already looking at the right block in the preview — keep
    // the forward sync from scrolling it again (same tracker trick as above).
    intptr_t pos = sci(h, SCI_GETCURRENTPOS);
    sLastCaretLine = sci(h, SCI_LINEFROMPOSITION, (uintptr_t)pos);
    intptr_t curVis = sci(h, SCI_GETFIRSTVISIBLELINE);
    sLastFirstVisibleLine = sci(h, SCI_DOCLINEFROMVISIBLE, (uintptr_t)curVis);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Menu commands
// ═══════════════════════════════════════════════════════════════════════════

static gboolean postToggleRenderCb(gpointer) {
    renderMarkdownDirect();
    return G_SOURCE_REMOVE;
}

static void togglePanel() {
    ensureContentView();
    if (sFullTemplate.empty()) buildTemplate();

    // First toggle: register with the host docking API. The Linux host has
    // supported it since GAP-49, so no floating fallback is needed here.
    // NOTE Linux ABI: lParam = widget, wParam = title (REVERSED vs macOS).
    if (g_panelHandle == 0) {
        g_panelHandle = npp(NPPM_DMM_REGISTERPANEL,
                            (unsigned long)(uintptr_t)"Markdown Panel",
                            (long)(intptr_t)sContentBox);
        if (g_panelHandle == 0) {
            g_warning("[MarkdownPanel] panel registration failed");
            return;
        }
        // Declare the reopen command so the host restores the panel after a
        // restart (GH linux#18): module = getName() ("Markdown Panel"),
        // cmdIndex 0 = "Toggle Markdown Panel". Hosts < 1.1.0 return 0 — ignored.
        NppPanelInfo info;
        info.moduleName = PLUGIN_NAME;
        info.cmdIndex   = 0;
        npp(NPPM_DMM_SETPANELINFO, (unsigned long)(uintptr_t)g_panelHandle,
            (long)(intptr_t)&info);
    }

    // Target the OPPOSITE of the live state — self-corrects when the user
    // closed the panel through the host frame's ✕ (no plugin callback).
    bool targetShown = !markdownPanelIsShown();

    sPanelVisible = targetShown;
    npp(NPPM_SETMENUITEMCHECK, (unsigned long)funcItem[0].cmdID, targetShown ? 1 : 0);

    if (targetShown) {
        npp(NPPM_DMM_SHOWPANEL, (unsigned long)g_panelHandle, 0);
        sCurrentFilePath.clear();   // force baseURL/template refresh
        sLastRenderedText.clear();
        loadTemplateIntoWebView();
        g_timeout_add(300, postToggleRenderCb, NULL);
    } else {
        npp(NPPM_DMM_HIDEPANEL, (unsigned long)g_panelHandle, 0);
    }
}

static void syncWithCaretCmd() {
    sSettings.syncWithCaret = !sSettings.syncWithCaret;
    npp(NPPM_SETMENUITEMCHECK, (unsigned long)funcItem[2].cmdID,
        sSettings.syncWithCaret ? 1 : 0);
    saveSettings();
    sLastCaretLine        = -1;
    sLastFirstVisibleLine = -1;
    if (sSettings.syncWithCaret || sSettings.syncWithFirstVisibleLine) syncScroll();
}

static void syncWithFirstVisibleLineCmd() {
    sSettings.syncWithFirstVisibleLine = !sSettings.syncWithFirstVisibleLine;
    npp(NPPM_SETMENUITEMCHECK, (unsigned long)funcItem[3].cmdID,
        sSettings.syncWithFirstVisibleLine ? 1 : 0);
    saveSettings();
    sLastCaretLine        = -1;
    sLastFirstVisibleLine = -1;
    if (sSettings.syncWithCaret || sSettings.syncWithFirstVisibleLine) syncScroll();
}

// Toolbar "refresh": rebuild the HTML template + reload from scratch.
static void refreshMarkdownPreview() {
    if (!sPanelVisible) return;
    sLastRenderedText.clear();
    sCurrentFilePath.clear();
    buildTemplate();
    loadTemplateIntoWebView();   // load-changed(FINISHED) re-renders
}

// Live zoom via JS (no re-render) + persist. Reset = delta to 100.
static void adjustPreviewZoom(int delta) {
    int z = sSettings.zoomLevel + delta;
    if (z < 50)  z = 50;
    if (z > 300) z = 300;
    if (z == sSettings.zoomLevel) return;
    sSettings.zoomLevel = z;
    if (sWebView && sWebViewReady)
        runJS(sWebView, "document.body.style.zoom='" + std::to_string(z) + "%';");
    saveSettings();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Print / PDF via WebKitPrintOperation
// ═══════════════════════════════════════════════════════════════════════════

static GtkWindow *rootWindow() {
    GtkRoot *root = sContentBox ? gtk_widget_get_root(sContentBox) : NULL;
    if (root && GTK_IS_WINDOW(root)) return GTK_WINDOW(root);
    return GTK_WINDOW(nppData.nppHandle);
}

static void printMarkdownPreview() {
    if (!sPanelVisible || !sWebView) return;
    WebKitPrintOperation *op = webkit_print_operation_new(sWebView);
    // Seed the dialog with the same US-Letter / 0.5" geometry as the silent
    // PDF path (macOS presets 36pt margins + automatic pagination on its sheet).
    GtkPageSetup *setup = letterPageSetup();
    webkit_print_operation_set_page_setup(op, setup);
    g_object_unref(setup);
    webkit_print_operation_run_dialog(op, rootWindow());
    g_object_unref(op);
}

// US-Letter, 0.5" margins — same geometry as the macOS writePaginatedPdf.
static GtkPageSetup *letterPageSetup() {
    GtkPageSetup *setup = gtk_page_setup_new();
    GtkPaperSize *letter = gtk_paper_size_new(GTK_PAPER_NAME_LETTER);
    gtk_page_setup_set_paper_size(setup, letter);
    gtk_paper_size_free(letter);
    gtk_page_setup_set_top_margin   (setup, 36, GTK_UNIT_POINTS);
    gtk_page_setup_set_bottom_margin(setup, 36, GTK_UNIT_POINTS);
    gtk_page_setup_set_left_margin  (setup, 36, GTK_UNIT_POINTS);
    gtk_page_setup_set_right_margin (setup, 36, GTK_UNIT_POINTS);
    return setup;
}

static bool sPrintDone = false;
static bool sPrintOK   = false;
static void on_print_finished(WebKitPrintOperation *, gpointer) {
    sPrintDone = true; sPrintOK = true;
}
static gboolean on_print_failed(WebKitPrintOperation *, GError *e, gpointer) {
    g_warning("[MarkdownPanel] PDF print failed: %s", e ? e->message : "?");
    sPrintDone = true; sPrintOK = false;
    return TRUE;
}

// Pump the default main context until *flag or timeout — the Linux stand-in
// for the macOS pumpUntil (WebKit load/render/print are all async; this keeps
// exports effectively synchronous so batch macro runs sequence correctly).
static bool pumpUntilFlag(double timeoutSec, const bool *flag) {
    gint64 deadline = g_get_monotonic_time() + (gint64)(timeoutSec * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < deadline) {
        if (*flag) return true;
        g_main_context_iteration(NULL, FALSE);
        g_usleep(10000);
    }
    return *flag;
}

// Print webView's rendered content as a paginated PDF to destPath, silently.
static bool writePaginatedPdf(WebKitWebView *view, const std::string &destPath) {
    if (!view) return false;
    WebKitPrintOperation *op = webkit_print_operation_new(view);

    GtkPrintSettings *ps = gtk_print_settings_new();
    gtk_print_settings_set_printer(ps, "Print to File");
    gtk_print_settings_set(ps, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
    gchar *uri = g_filename_to_uri(destPath.c_str(), NULL, NULL);
    gtk_print_settings_set(ps, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
    g_free(uri);
    webkit_print_operation_set_print_settings(op, ps);

    GtkPageSetup *setup = letterPageSetup();
    webkit_print_operation_set_page_setup(op, setup);

    sPrintDone = false; sPrintOK = false;
    g_signal_connect(op, "finished", G_CALLBACK(on_print_finished), NULL);
    g_signal_connect(op, "failed",   G_CALLBACK(on_print_failed), NULL);
    webkit_print_operation_print(op);
    pumpUntilFlag(60.0, &sPrintDone);

    g_object_unref(setup);
    g_object_unref(ps);
    g_object_unref(op);
    return sPrintOK;
}

// Toolbar "save as PDF": async file dialog, then print-to-file.
static void on_pdf_dialog_done(GObject *src, GAsyncResult *res, gpointer) {
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    gchar *path = g_file_get_path(f);
    if (path) {
        writePaginatedPdf(sWebView, path);
        g_free(path);
    }
    g_object_unref(f);
}

static void saveMarkdownAsPDF() {
    if (!sPanelVisible || !sWebView) return;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    std::string base = "preview";
    std::string fp = getCurrentFilePath();
    if (!fp.empty()) {
        gchar *b = g_path_get_basename(fp.c_str());
        char *dot = strrchr(b, '.');
        if (dot && dot != b) *dot = '\0';
        if (*b) base = b;
        g_free(b);
    }
    gtk_file_dialog_set_initial_name(dlg, (base + ".pdf").c_str());
    gtk_file_dialog_save(dlg, rootWindow(), NULL, on_pdf_dialog_done, NULL);
    g_object_unref(dlg);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Silent export (menu + macro/batch): render the CURRENT file off-panel and
//  write <stem>.html / <stem>.pdf next to the source, no dialogs. A dedicated
//  fixed-width WebView inside a never-presented window keeps export geometry
//  deterministic (the macOS off-screen-window trick, minus window positioning
//  which GTK4 can't do — an unpresented window is simply never mapped).
// ═══════════════════════════════════════════════════════════════════════════

static const int kExportWidthPx = 850;
static GtkWidget      *sExportWindow  = NULL;
static WebKitWebView  *sExportWebView = NULL;
static bool            sExportNavDone = false;
static std::string     sExportTempHtmlPath;

static void on_export_load_changed(WebKitWebView *, WebKitLoadEvent ev, gpointer) {
    if (ev == WEBKIT_LOAD_FINISHED) sExportNavDone = true;
}
static gboolean on_export_load_failed(WebKitWebView *, WebKitLoadEvent, char *, GError *, gpointer) {
    sExportNavDone = true;
    return FALSE;
}

static WebKitWebView *ensureExportWebView() {
    if (sExportWebView) return sExportWebView;
    sExportWebView = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *ws = webkit_web_view_get_settings(sExportWebView);
    webkit_settings_set_allow_file_access_from_file_urls(ws, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(ws, TRUE);
    g_signal_connect(sExportWebView, "load-changed",
                     G_CALLBACK(on_export_load_changed), NULL);
    g_signal_connect(sExportWebView, "load-failed",
                     G_CALLBACK(on_export_load_failed), NULL);

    sExportWindow = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(sExportWindow), kExportWidthPx, 700);
    gtk_window_set_child(GTK_WINDOW(sExportWindow), GTK_WIDGET(sExportWebView));
    // Never presented: the web process still loads/executes/prints; the
    // window exists only to own the widget at a deterministic width.
    return sExportWebView;
}

static void loadTemplateIntoExportWebView() {
    if (!sExportWebView || sFullTemplate.empty()) return;
    std::string fp = getCurrentFilePath();
    std::string tmpPath;
    if (!fp.empty()) {
        gchar *dir = g_path_get_dirname(fp.c_str());
        tmpPath = std::string(dir) + "/.npp-md-export.html";
        g_free(dir);
    } else {
        tmpPath = std::string(g_get_tmp_dir()) + "/npp-md-export.html";
    }
    if (!sExportTempHtmlPath.empty() && sExportTempHtmlPath != tmpPath)
        g_unlink(sExportTempHtmlPath.c_str());
    sExportTempHtmlPath = tmpPath;

    g_file_set_contents(tmpPath.c_str(), sFullTemplate.c_str(),
                        (gssize)sFullTemplate.size(), NULL);
    gchar *uri = g_filename_to_uri(tmpPath.c_str(), NULL, NULL);
    sExportNavDone = false;
    webkit_web_view_load_uri(sExportWebView, uri);
    g_free(uri);
}

// "Diagrams have settled" probe — verbatim from the macOS build.
static const char *kExportSettleJS =
    "(function(){if(document.querySelector('pre.squared-pending'))return false;"
    "var m=document.querySelectorAll('.mermaid');"
    "for(var i=0;i<m.length;i++){if(!m[i].querySelector('svg'))return false;}return true;})()";

// Clone the document, strip <script>s, inline every <img> as a data: URI,
// stash the result on window.__nppExportHtml (WebKitGTK's evaluate has no
// top-level await — an async IIFE + a polled global replaces macOS's
// callAsyncJavaScript).
static const char *kExportInlineHtmlJS =
    "window.__nppExportHtml = null;"
    "(async () => { try {"
    "  const clone = document.documentElement.cloneNode(true);"
    "  clone.querySelectorAll('script').forEach(s => s.remove());"
    "  const imgs = Array.from(clone.querySelectorAll('img'));"
    "  for (const img of imgs) { try {"
    "    const s = img.getAttribute('src');"
    "    if (s && !s.startsWith('data:')) {"
    "      const abs = new URL(s, document.baseURI).href;"
    "      const resp = await fetch(abs); const blob = await resp.blob();"
    "      const durl = await new Promise((res, rej) => { const fr = new FileReader();"
    "        fr.onloadend = () => res(fr.result); fr.onerror = rej; fr.readAsDataURL(blob); });"
    "      img.setAttribute('src', durl);"
    "    }"
    "  } catch (e) {} }"
    "  window.__nppExportHtml = '<!DOCTYPE html>\\n' + clone.outerHTML;"
    "} catch (e) { window.__nppExportHtml = ''; } })(); true;";

// Evaluate `js` in the export view and return its result as a string
// (empty on error/null). Pumps until the async completion fires.
struct EvalResult { bool done = false; std::string str; bool boolean = false; };

// The result is shared_ptr-owned so a callback that fires AFTER evalSync has
// already timed out and returned writes into a still-live object instead of a
// dangling stack slot. The callback holds its own ref (freed here); evalSync's
// local ref keeps it alive for the copy-out.
static void on_eval_done(GObject *src, GAsyncResult *res, gpointer user) {
    auto *sp = static_cast<std::shared_ptr<EvalResult> *>(user);
    EvalResult *er = sp->get();
    GError *err = NULL;
    JSCValue *v = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(src), res, &err);
    if (v) {
        if (jsc_value_is_string(v)) {
            gchar *s = jsc_value_to_string(v);
            if (s) { er->str = s; g_free(s); }
        }
        er->boolean = jsc_value_to_boolean(v);
        g_object_unref(v);
    }
    g_clear_error(&err);
    er->done = true;
    delete sp;   // release the callback's ref
}

static EvalResult evalSync(WebKitWebView *view, const char *js, double timeoutSec) {
    auto er = std::make_shared<EvalResult>();
    auto *cbRef = new std::shared_ptr<EvalResult>(er);   // the callback's own ref
    webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL,
                                        on_eval_done, cbRef);
    pumpUntilFlag(timeoutSec, &er->done);
    return *er;   // default-valued if it timed out; object stays alive via cbRef
}

// Render the CURRENT buffer into the export view and wait (bounded) for the
// markdown + async diagram engines to settle.
static bool exportRenderCurrentAndSettle() {
    if (!sExportWebView) return false;
    if (!pumpUntilFlag(15.0, &sExportNavDone)) return false;   // template loaded

    std::string text = getEditorText();
    std::string ext = getCurrentExtension();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    for (auto &c : ext) c = (char)tolower((unsigned char)c);
    if (ext == "mmd") text = "```mermaid\n" + text + "\n```\n";

    std::string js = "renderMarkdown(" + jsonEscape(text) + "); true;";
    EvalResult r = evalSync(sExportWebView, js.c_str(), 15.0);
    if (!r.done) return false;

    // Best-effort wait for the async diagram engines (squared worker / mermaid).
    gint64 deadline = g_get_monotonic_time() + (gint64)(8.0 * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < deadline) {
        EvalResult s = evalSync(sExportWebView, kExportSettleJS, 2.0);
        if (s.done && s.boolean) break;
        g_main_context_iteration(NULL, FALSE);
        g_usleep(50000);
    }
    return true;   // proceed to capture even if diagrams didn't fully settle
}

static bool exportCaptureHtml(const std::string &destPath) {
    EvalResult kick = evalSync(sExportWebView, kExportInlineHtmlJS, 10.0);
    if (!kick.done) return false;
    // Poll the global until the async IIFE stashes the HTML.
    gint64 deadline = g_get_monotonic_time() + (gint64)(20.0 * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < deadline) {
        EvalResult r = evalSync(sExportWebView,
            "window.__nppExportHtml === null ? '' : window.__nppExportHtml", 5.0);
        if (r.done && !r.str.empty()) {
            return g_file_set_contents(destPath.c_str(), r.str.c_str(),
                                       (gssize)r.str.size(), NULL);
        }
        g_main_context_iteration(NULL, FALSE);
        g_usleep(50000);
    }
    g_warning("[MarkdownPanel] HTML capture timed out");
    return false;
}

static void exportCurrentFile(bool wantPDF) {
    if (sExporting) return;   // never re-enter (nested main-context pumps)

    std::string fp = getCurrentFilePath();
    if (fp.empty()) {   // untitled/unsaved → non-blocking warning, skip
        GtkAlertDialog *a = gtk_alert_dialog_new("Export skipped");
        gtk_alert_dialog_set_detail(a,
            "Save the file before exporting it to HTML or PDF.");
        gtk_alert_dialog_show(a, GTK_WINDOW(nppData.nppHandle));
        g_object_unref(a);
        return;
    }

    std::string stem = fp;
    {
        size_t slash = stem.rfind('/');
        size_t dot   = stem.rfind('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            stem = stem.substr(0, dot);
    }
    std::string dest = stem + (wantPDF ? ".pdf" : ".html");

    if (sFullTemplate.empty()) buildTemplate();
    if (sFullTemplate.empty()) {
        g_warning("[MarkdownPanel] Export aborted: render template unavailable.");
        return;
    }

    sExporting = true;
    ensureExportWebView();
    loadTemplateIntoExportWebView();

    bool ok = false;
    if (exportRenderCurrentAndSettle())
        ok = wantPDF ? writePaginatedPdf(sExportWebView, dest)
                     : exportCaptureHtml(dest);

    sExporting = false;

    g_message("[MarkdownPanel] Export %s -> %s : %s", wantPDF ? "PDF" : "HTML",
              dest.c_str(), ok ? "OK" : "FAILED");
}

static void exportCurrentToHtmlCmd() { exportCurrentFile(false); }
static void exportCurrentToPdfCmd()  { exportCurrentFile(true);  }

// ═══════════════════════════════════════════════════════════════════════════
//  Settings dialog (GTK4 async window; apply-on-Save like the macOS modal)
// ═══════════════════════════════════════════════════════════════════════════

static struct {
    GtkWidget *win;
    GtkWidget *zoom, *exts;
    GtkWidget *chk_allext, *chk_autoshow, *chk_caret, *chk_firstline;
    GtkWidget *chk_mermaid, *chk_squared, *chk_syncprev;
    GtkWidget *rad_boardroom, *rad_linen, *rad_blueprint;
} sDlg;

static void on_settings_destroy(GtkWidget *, gpointer) { sDlg.win = NULL; }

static void on_squared_toggled(GtkCheckButton *b, gpointer) {
    gboolean on = gtk_check_button_get_active(b);
    gtk_widget_set_sensitive(sDlg.rad_boardroom, on);
    gtk_widget_set_sensitive(sDlg.rad_linen, on);
    gtk_widget_set_sensitive(sDlg.rad_blueprint, on);
}

static gboolean settingsReloadCb(gpointer) {
    renderMarkdownDirect();
    return G_SOURCE_REMOVE;
}

static void on_settings_save(GtkButton *, gpointer) {
    bool needsReload = false;

    int newZoom = atoi(gtk_editable_get_text(GTK_EDITABLE(sDlg.zoom)));
    if (newZoom < 50) newZoom = 50;
    if (newZoom > 300) newZoom = 300;
    if (newZoom != sSettings.zoomLevel) { sSettings.zoomLevel = newZoom; needsReload = true; }

    const char *exts = gtk_editable_get_text(GTK_EDITABLE(sDlg.exts));
    sSettings.supportedExtensions = exts ? exts : "";
    sSettings.allowAllExtensions =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_allext));
    sSettings.autoShowPanel =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_autoshow));
    sSettings.syncWithCaret =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_caret));
    sSettings.syncWithFirstVisibleLine =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_firstline));

    bool newMermaid = gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_mermaid));
    if (newMermaid != sSettings.enableMermaid) { sSettings.enableMermaid = newMermaid; needsReload = true; }
    bool newSquared = gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_squared));
    if (newSquared != sSettings.enableSquared) { sSettings.enableSquared = newSquared; needsReload = true; }

    std::string newTheme =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.rad_linen))     ? "linen" :
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.rad_blueprint)) ? "blueprint"
                                                                          : "boardroom";
    if (newTheme != sSettings.squaredTheme) { sSettings.squaredTheme = newTheme; needsReload = true; }

    sSettings.syncPreviewToEditor =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(sDlg.chk_syncprev));

    npp(NPPM_SETMENUITEMCHECK, (unsigned long)funcItem[2].cmdID, sSettings.syncWithCaret ? 1 : 0);
    npp(NPPM_SETMENUITEMCHECK, (unsigned long)funcItem[3].cmdID, sSettings.syncWithFirstVisibleLine ? 1 : 0);

    saveSettings();
    gtk_window_destroy(GTK_WINDOW(sDlg.win));

    if (needsReload && sPanelVisible) {
        buildTemplate();
        sCurrentFilePath.clear();
        sLastRenderedText.clear();
        loadTemplateIntoWebView();
        g_timeout_add(300, settingsReloadCb, NULL);
    }
}

static gboolean on_settings_key(GtkEventControllerKey *, guint keyval, guint,
                                GdkModifierType, gpointer) {
    if (keyval == GDK_KEY_Escape) {
        gtk_window_destroy(GTK_WINDOW(sDlg.win));
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *check(const char *label, bool active) {
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), active);
    return c;
}

static void showSettingsCmd() {
    if (sDlg.win) { gtk_window_present(GTK_WINDOW(sDlg.win)); return; }

    sDlg.win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(sDlg.win), "Markdown Panel Settings");
    gtk_window_set_modal(GTK_WINDOW(sDlg.win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(sDlg.win), GTK_WINDOW(nppData.nppHandle));
    gtk_window_set_resizable(GTK_WINDOW(sDlg.win), FALSE);
    g_signal_connect(sDlg.win, "destroy", G_CALLBACK(on_settings_destroy), NULL);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_settings_key), NULL);
    gtk_widget_add_controller(sDlg.win, keys);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(sDlg.win), vbox);

    // Zoom row
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(row), gtk_label_new("Zoom Level:"));
        sDlg.zoom = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(sDlg.zoom),
                              std::to_string(sSettings.zoomLevel).c_str());
        gtk_editable_set_width_chars(GTK_EDITABLE(sDlg.zoom), 5);
        gtk_box_append(GTK_BOX(row), sDlg.zoom);
        gtk_box_append(GTK_BOX(row), gtk_label_new("%"));
        gtk_box_append(GTK_BOX(vbox), row);
    }

    // Extensions row
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(row), gtk_label_new("Supported extensions:"));
        sDlg.exts = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(sDlg.exts),
                              sSettings.supportedExtensions.c_str());
        gtk_widget_set_hexpand(sDlg.exts, TRUE);
        gtk_box_append(GTK_BOX(row), sDlg.exts);
        gtk_box_append(GTK_BOX(vbox), row);
    }

    sDlg.chk_allext    = check("Allow all file extensions", sSettings.allowAllExtensions);
    sDlg.chk_autoshow  = check("Automatically show panel for supported files", sSettings.autoShowPanel);
    sDlg.chk_caret     = check("Synchronize with caret position", sSettings.syncWithCaret);
    sDlg.chk_firstline = check("Synchronize with first visible line", sSettings.syncWithFirstVisibleLine);
    sDlg.chk_mermaid   = check("Enable Mermaid diagram rendering", sSettings.enableMermaid);
    sDlg.chk_squared   = check("Enable Squared flow diagrams", sSettings.enableSquared);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_allext);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_autoshow);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_caret);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_firstline);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_mermaid);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_squared);
    g_signal_connect(sDlg.chk_squared, "toggled", G_CALLBACK(on_squared_toggled), NULL);

    // Squared theme radios — display Plain/Pastel/Blue, engine ids stay
    // boardroom/linen/blueprint (macOS parity).
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(row, 24);
        sDlg.rad_boardroom = gtk_check_button_new_with_label("Plain");
        sDlg.rad_linen     = gtk_check_button_new_with_label("Pastel");
        sDlg.rad_blueprint = gtk_check_button_new_with_label("Blue");
        gtk_check_button_set_group(GTK_CHECK_BUTTON(sDlg.rad_linen),
                                   GTK_CHECK_BUTTON(sDlg.rad_boardroom));
        gtk_check_button_set_group(GTK_CHECK_BUTTON(sDlg.rad_blueprint),
                                   GTK_CHECK_BUTTON(sDlg.rad_boardroom));
        if (sSettings.squaredTheme == "linen")
            gtk_check_button_set_active(GTK_CHECK_BUTTON(sDlg.rad_linen), TRUE);
        else if (sSettings.squaredTheme == "blueprint")
            gtk_check_button_set_active(GTK_CHECK_BUTTON(sDlg.rad_blueprint), TRUE);
        else
            gtk_check_button_set_active(GTK_CHECK_BUTTON(sDlg.rad_boardroom), TRUE);
        gtk_widget_set_sensitive(sDlg.rad_boardroom, sSettings.enableSquared);
        gtk_widget_set_sensitive(sDlg.rad_linen,     sSettings.enableSquared);
        gtk_widget_set_sensitive(sDlg.rad_blueprint, sSettings.enableSquared);
        gtk_box_append(GTK_BOX(row), sDlg.rad_boardroom);
        gtk_box_append(GTK_BOX(row), sDlg.rad_linen);
        gtk_box_append(GTK_BOX(row), sDlg.rad_blueprint);
        gtk_box_append(GTK_BOX(vbox), row);
    }

    // Reverse scroll sync — bottom of the dialog (macOS layout parity)
    sDlg.chk_syncprev = check("Synchronize editor when scrolling preview",
                              sSettings.syncPreviewToEditor);
    gtk_box_append(GTK_BOX(vbox), sDlg.chk_syncprev);

    // Save button
    {
        GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(btns, GTK_ALIGN_END);
        gtk_widget_set_margin_top(btns, 6);
        GtkWidget *save = gtk_button_new_with_label("Save");
        gtk_widget_add_css_class(save, "suggested-action");
        g_signal_connect(save, "clicked", G_CALLBACK(on_settings_save), NULL);
        gtk_box_append(GTK_BOX(btns), save);
        gtk_box_append(GTK_BOX(vbox), btns);
        gtk_window_set_default_widget(GTK_WINDOW(sDlg.win), save);
    }

    gtk_window_present(GTK_WINDOW(sDlg.win));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Help / About
// ═══════════════════════════════════════════════════════════════════════════

static gboolean helpToggleCb(gpointer) {
    if (!sPanelVisible) togglePanel();
    return G_SOURCE_REMOVE;
}

static void showHelpCmd() {
    std::string helpPath = sResourcesDir + "/help.md";
    if (g_file_test(helpPath.c_str(), G_FILE_TEST_IS_REGULAR)) {
        npp(NPPM_DOOPEN, 0, (long)(intptr_t)helpPath.c_str());
        if (!sPanelVisible)
            g_timeout_add(300, helpToggleCb, NULL);
    } else {
        GtkUriLauncher *l = gtk_uri_launcher_new(
            "https://github.com/notepad-plus-plus-mac/NppMarkdownPanel");
        gtk_uri_launcher_launch(l, NULL, NULL, NULL, NULL);
        g_object_unref(l);
    }
}

static void showAboutCmd() {
    GtkAlertDialog *a = gtk_alert_dialog_new("Markdown Panel");
    gtk_alert_dialog_set_detail(a,
        "Markdown Panel for Nextpad++ (Linux port)\n\n"
        "Real-time Markdown preview with GitHub-flavored rendering.\n\n"
        "Features:\n"
        "- Live preview as you type\n"
        "- Syntax highlighting for code blocks\n"
        "- Relative image support\n"
        "- Dark mode support\n"
        "- Scroll synchronization\n"
        "- Export to HTML / PDF\n"
        "- YAML frontmatter display\n"
        "- Mermaid diagram support (optional)\n\n"
        "Rendering: marked.js + highlight.js in WebKitGTK\n\n"
        "Original Windows plugin by Jens Wollgarten (GPLv2)\n"
        "Ported from the macOS port's WebKit implementation.");
    gtk_alert_dialog_show(a, GTK_WINDOW(nppData.nppHandle));
    g_object_unref(a);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Plugin exports
// ═══════════════════════════════════════════════════════════════════════════

// WebKit's web-process sandbox launches bwrap, which needs an unprivileged
// user namespace WITH a writable uid_map. Ubuntu 24.04's AppArmor denies that
// to unpackaged binaries (kernel.apparmor_restrict_unprivileged_userns=1) —
// bwrap then aborts and WebKit g_error()s, taking the WHOLE HOST down at first
// WebView creation. Probe bwrap's exact failing sequence in a throwaway child;
// only when it fails, flip WebKit's official opt-out BEFORE any WebKit object
// exists. Systems where user namespaces work keep the sandbox.
static bool userNamespacesUsable() {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (unshare(CLONE_NEWUSER) != 0) _exit(1);
        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd >= 0) { (void)!write(fd, "deny", 4); close(fd); }
        fd = open("/proc/self/uid_map", O_WRONLY);
        if (fd < 0) _exit(1);
        char map[64];
        int n = snprintf(map, sizeof map, "0 %d 1", (int)getuid());
        bool ok = (n > 0 && write(fd, map, (size_t)n) == n);
        close(fd);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void ensureWebKitCanLaunch() {
    if (g_getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) return;
    if (userNamespacesUsable()) return;
    g_message("[MarkdownPanel] user namespaces unavailable (AppArmor userns "
              "restriction?) — disabling the WebKit sandbox so the preview "
              "can run; bwrap would otherwise abort the application");
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", TRUE);
}

static gboolean autoShowCb(gpointer) {
    if (!sPanelVisible) togglePanel();
    return G_SOURCE_REMOVE;
}

static void registerToolbarIcon() {
    static bool done = false;
    if (done) return;
    done = true;
    static std::string iconPath;
    iconPath = sResourcesDir + "/toolbar.png";
    npp(NPPM_ADDTOOLBARICON_FORDARKMODE,
        (unsigned long)funcItem[0].cmdID, (long)(intptr_t)iconPath.c_str());
}

extern "C" NPP_EXPORT void setInfo(NppData data) {
    nppData = data;
    ensureWebKitCanLaunch();   // must run before the first WebKit object
    sResourcesDir = findResourcesDir();
    loadSettings();

    int idx = 0;
    auto addItem = [&](const char *name, void (*func)(void)) {
        g_strlcpy(funcItem[idx].itemName, name, sizeof funcItem[idx].itemName);
        funcItem[idx].pFunc = func;
        funcItem[idx].init2Check = 0;
        idx++;
    };
    // Separator: "-" on Linux (the host's menu builder starts a new section;
    // macOS used "" which this host would skip entirely). Each separator
    // still consumes a cmdID, keeping indices aligned with the macOS build.
    auto addSep = [&]() { addItem("-", NULL); };

    addItem("Toggle Markdown Panel",                togglePanel);        // 0
    addSep();                                                            // 1
    addItem("Synchronize with caret position",      syncWithCaretCmd);   // 2
    addItem("Synchronize with first visible line",  syncWithFirstVisibleLineCmd); // 3
    addSep();                                                            // 4
    addItem("Export to HTML",                       exportCurrentToHtmlCmd); // 5
    addItem("Export to PDF",                        exportCurrentToPdfCmd);  // 6
    addSep();                                                            // 7
    addItem("Settings",                             showSettingsCmd);    // 8
    addItem("Help",                                 showHelpCmd);        // 9
    addItem("About",                                showAboutCmd);       // 10

    funcItem[2].init2Check = sSettings.syncWithCaret;
    funcItem[3].init2Check = sSettings.syncWithFirstVisibleLine;
}

extern "C" NPP_EXPORT const char *getName(void) {
    return PLUGIN_NAME;
}

extern "C" NPP_EXPORT FuncItem *getFuncsArray(int *nbF) {
    *nbF = NB_FUNC;
    return funcItem;
}

extern "C" NPP_EXPORT void beNotified(SCNotification *n) {
    switch (n->nmhdr.code) {
        case NPPN_TBMODIFICATION:   // not fired by this host today (GAP-88a);
        case NPPN_READY:            // READY covers it — guard makes both safe
            registerToolbarIcon();
            if (n->nmhdr.code == NPPN_READY &&
                sSettings.autoShowPanel && isSupportedExtension()) {
                g_idle_add(autoShowCb, NULL);
            }
            break;

        case NPPN_BUFFERACTIVATED:
            if (sPanelVisible) {
                sLastRenderedText.clear();
                sCurrentFilePath.clear();
                sLastCaretLine        = -1;
                sLastFirstVisibleLine = -1;
                renderMarkdownDeferred();
            }
            break;

        case SCN_MODIFIED:
            // Self-filter by modificationType — this host forwards EVERY
            // SCN_MODIFIED flavour raw (styling, markers, …), unlike the
            // filtered macOS/Windows fan-out. See PORTING_NOTES.md.
            if (sPanelVisible &&
                (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
                renderMarkdownDeferred();
            }
            break;

        case SCN_UPDATEUI:
            if (sPanelVisible) syncScroll();
            break;

        case NPPN_SHUTDOWN:
            saveSettings();
            if (!sCurrentTempHtmlPath.empty()) {
                g_unlink(sCurrentTempHtmlPath.c_str());
                sCurrentTempHtmlPath.clear();
            }
            if (!sExportTempHtmlPath.empty()) {
                g_unlink(sExportTempHtmlPath.c_str());
                sExportTempHtmlPath.clear();
            }
            if (sPendingRender) { g_source_remove(sPendingRender); sPendingRender = 0; }
            if (sPendingScroll) { g_source_remove(sPendingScroll); sPendingScroll = 0; }
            if (sAppInactiveProbe) { g_source_remove(sAppInactiveProbe); sAppInactiveProbe = 0; }
            if (g_panelHandle > 0) {
                npp(NPPM_DMM_UNREGISTERPANEL, (unsigned long)g_panelHandle, 0);
                g_panelHandle = 0;
            }
            break;

        default:
            break;
    }
}

extern "C" NPP_EXPORT long messageProc(unsigned int, unsigned long, long) {
    return 1;
}

extern "C" NPP_EXPORT int isUnicode(void) {
    return 1;
}
