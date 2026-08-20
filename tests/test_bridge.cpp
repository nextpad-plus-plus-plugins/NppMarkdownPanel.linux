/*
 * Unit tests for the v1.0.8 preview→editor bridge (reverse scroll sync,
 * double-click word locate) and the script-message parsing — single-TU
 * include of the plugin source (pork2sausage pattern) with a mock Scintilla
 * document behind scintilla_view_send_message.
 *
 * Build:  see tests/build_and_run.sh
 */

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ── Mock Scintilla ──────────────────────────────────────────────────────────

struct MockDoc {
    std::string text;
    long firstVisible = 0;        // in VISIBLE-line units
    long maxFirstVisible = 1L << 40; // SETFIRSTVISIBLELINE clamp (simulates EOF clamp)
    long wrapFactor = 1;          // visible = doc * wrapFactor (crude wrap/fold model)
    long currentPos = 0;
    long targetStart = 0, targetEnd = 0, searchFlags = 0;
    long selAnchor = -1, selCaret = -1;
    int  setSelCalls = 0, scrollCaretCalls = 0, setFirstVisibleCalls = 0;

    long lineCount() const {
        long n = 1;
        for (char c : text) if (c == '\n') n++;
        return n;
    }
    long positionFromLine(long l) const {
        if (l <= 0) return 0;
        long line = 0;
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\n' && ++line == l) return (long)i + 1;
        }
        return (long)text.size();
    }
    long lineEndPosition(long l) const {
        long start = positionFromLine(l);
        size_t nl = text.find('\n', (size_t)start);
        return nl == std::string::npos ? (long)text.size() : (long)nl;
    }
    long lineFromPosition(long pos) const {
        if (pos <= 0) return 0;
        if (pos > (long)text.size()) pos = (long)text.size();
        long line = 0;
        for (long i = 0; i < pos; i++) if (text[(size_t)i] == '\n') line++;
        return line;
    }
};

static MockDoc gDoc;

extern "C" __attribute__((visibility("default")))
intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                     uintptr_t w, intptr_t l);

// The plugin TU (included below) declares this extern "C" — our definition
// wins at link time inside the test executable.
#include "../src/NppMarkdownPanel.cpp"

extern "C" intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                                uintptr_t w, intptr_t l) {
    MockDoc *d = (MockDoc *)view;
    switch (msg) {
        case SCI_GETLINECOUNT:        return d->lineCount();
        case SCI_POSITIONFROMLINE:    return d->positionFromLine((long)w);
        case SCI_GETLINEENDPOSITION:  return d->lineEndPosition((long)w);
        case SCI_LINEFROMPOSITION:    return d->lineFromPosition((long)w);
        case SCI_GETCURRENTPOS:       return d->currentPos;
        case SCI_SETSEARCHFLAGS:      d->searchFlags = (long)w; return 0;
        case SCI_SETTARGETSTART:      d->targetStart = (long)w; return 0;
        case SCI_SETTARGETEND:        d->targetEnd   = (long)w; return 0;
        case SCI_GETTARGETEND:        return d->targetEnd;
        case SCI_SEARCHINTARGET: {
            // match-case plain search inside [targetStart, targetEnd)
            long len = (long)w;
            const char *needle = (const char *)l;
            if (len <= 0 || d->targetStart >= d->targetEnd) return -1;
            long limit = d->targetEnd - len;
            for (long i = d->targetStart; i <= limit; i++) {
                if (memcmp(d->text.data() + i, needle, (size_t)len) == 0) {
                    d->targetStart = i;
                    d->targetEnd   = i + len;
                    return i;
                }
            }
            return -1;
        }
        case SCI_SETSEL:
            d->selAnchor = (long)w; d->selCaret = (long)l;
            d->currentPos = (long)l; d->setSelCalls++;
            return 0;
        case SCI_SCROLLCARET:         d->scrollCaretCalls++; return 0;
        case SCI_GETFIRSTVISIBLELINE: return d->firstVisible;
        case SCI_SETFIRSTVISIBLELINE: {
            long v = (long)w;
            if (v < 0) v = 0;
            if (v > d->maxFirstVisible) v = d->maxFirstVisible;
            d->firstVisible = v;
            d->setFirstVisibleCalls++;
            return 0;
        }
        case SCI_DOCLINEFROMVISIBLE: {
            long doc = (long)w / d->wrapFactor;
            long maxDoc = d->lineCount() - 1;
            return doc > maxDoc ? maxDoc : doc;
        }
        case SCI_VISIBLEFROMDOCLINE:  return (long)w * d->wrapFactor;
        default:                      return 0;
    }
}

// Host stub: hand out the mock doc as "current scintilla".
static long stubHostMsg(unsigned int msg, unsigned long, long) {
    if (msg == NPPM_GETCURRENTSCINTILLA) return (long)(intptr_t)&gDoc;
    return 0;
}

// ── Harness ────────────────────────────────────────────────────────────────

static int gPass = 0, gFail = 0;
static void check(bool ok, const char *what) {
    if (ok) { gPass++; }
    else    { gFail++; fprintf(stderr, "FAIL: %s\n", what); }
}

static void resetDoc(const std::string &text) {
    gDoc = MockDoc();
    gDoc.text = text;
    sLastCaretLine = -1;
    sLastFirstVisibleLine = -1;
}

static JSCContext *gJsc = NULL;
static void postJson(const char *json) {
    JSCValue *v = jsc_value_new_from_json(gJsc, json);
    on_script_message(NULL, v, NULL);
    if (v) g_object_unref(v);
}

int main() {
    nppData.hostMsg = stubHostMsg;
    sPanelVisible = true;
    sSettings.syncPreviewToEditor = true;
    gJsc = jsc_context_new();

    const char *fiveLines = "alpha beta gamma\nbeta done\nline two\nline three\nlast beta line\n";
    // lines: 0:"alpha beta gamma" 1:"beta done" 2:"line two" 3:"line three" 4:"last beta line" 5:""

    // ── applyPreviewScrollToEditor ─────────────────────────────────────────
    {
        // setting off → no movement
        resetDoc(fiveLines);
        sSettings.syncPreviewToEditor = false;
        applyPreviewScrollToEditor(4);
        check(gDoc.setFirstVisibleCalls == 0, "scroll: disabled setting is a no-op");
        sSettings.syncPreviewToEditor = true;

        // panel hidden → no movement
        resetDoc(fiveLines);
        sPanelVisible = false;
        applyPreviewScrollToEditor(4);
        check(gDoc.setFirstVisibleCalls == 0, "scroll: hidden panel is a no-op");
        sPanelVisible = true;

        // dead-band ±1
        resetDoc(fiveLines);
        gDoc.firstVisible = 2;
        applyPreviewScrollToEditor(1);
        applyPreviewScrollToEditor(2);
        applyPreviewScrollToEditor(3);
        check(gDoc.setFirstVisibleCalls == 0, "scroll: +-1 line dead-band holds");
        applyPreviewScrollToEditor(4);
        check(gDoc.setFirstVisibleCalls == 1 && gDoc.firstVisible == 4,
              "scroll: 2-line move applies");

        // clamping: negative and beyond-EOF lines
        resetDoc(fiveLines);
        gDoc.firstVisible = 3;
        applyPreviewScrollToEditor(-50);
        check(gDoc.firstVisible == 0, "scroll: negative line clamps to 0");
        applyPreviewScrollToEditor(1000000);
        check(gDoc.firstVisible == 5, "scroll: huge line clamps to lineCount-1");

        // wrap/fold awareness: doc→visible via VISIBLEFROMDOCLINE
        resetDoc(fiveLines);
        gDoc.wrapFactor = 3;
        applyPreviewScrollToEditor(4);
        check(gDoc.firstVisible == 12, "scroll: wrap-aware (doc 4 -> vis 12)");
        check(sLastFirstVisibleLine == 4, "scroll: tracker in doc lines after wrap");

        // EOF clamp read-back: tracker must hold what Scintilla APPLIED
        resetDoc(fiveLines);
        gDoc.maxFirstVisible = 3;
        applyPreviewScrollToEditor(5);
        check(gDoc.firstVisible == 3 && sLastFirstVisibleLine == 3,
              "scroll: tracker reads back the clamped value");

        // caret tracker pre-update
        resetDoc(fiveLines);
        gDoc.currentPos = (long)gDoc.text.find("done");
        applyPreviewScrollToEditor(4);
        check(sLastCaretLine == 1, "scroll: caret tracker pre-updated");

        // empty document
        resetDoc("");
        applyPreviewScrollToEditor(3);
        check(gDoc.setFirstVisibleCalls == 0, "scroll: empty doc (1 line) inside dead-band");
    }

    // ── locateWordFromPreview ──────────────────────────────────────────────
    {
        // exact occurrence selection: 3 "beta"s at pos 6, 17, 47
        long b0 = 6, b1 = 17, b2 = (long)std::string(fiveLines).find("beta", 30);

        resetDoc(fiveLines);
        locateWordFromPreview("beta", 0, 4, 0);
        check(gDoc.selAnchor == b0 && gDoc.selCaret == b0 + 4, "locate: occ 0 = first beta");

        resetDoc(fiveLines);
        locateWordFromPreview("beta", 0, 4, 1);
        check(gDoc.selAnchor == b1 && gDoc.selCaret == b1 + 4, "locate: occ 1 = second beta");

        resetDoc(fiveLines);
        locateWordFromPreview("beta", 0, 4, 2);
        check(gDoc.selAnchor == b2 && gDoc.selCaret == b2 + 4, "locate: occ 2 = third beta");

        // occ beyond matches → falls back to LAST found in range
        resetDoc(fiveLines);
        locateWordFromPreview("beta", 0, 4, 9);
        check(gDoc.selAnchor == b2, "locate: occ overflow falls back to last in range");

        // block range restricts the search
        resetDoc(fiveLines);
        locateWordFromPreview("beta", 2, 3, 0);
        check(gDoc.setSelCalls == 0, "locate: word outside block range is a silent no-op");

        resetDoc(fiveLines);
        locateWordFromPreview("beta", 1, 1, 0);
        check(gDoc.selAnchor == b1, "locate: single-line block finds its own beta");

        // absent word / case sensitivity
        resetDoc(fiveLines);
        locateWordFromPreview("zebra", 0, 4, 0);
        check(gDoc.setSelCalls == 0, "locate: absent word is a silent no-op");
        locateWordFromPreview("Beta", 0, 4, 0);
        check(gDoc.setSelCalls == 0, "locate: match is case-sensitive");

        // line clamping: bogus ranges still work
        resetDoc(fiveLines);
        locateWordFromPreview("beta", -7, 1000000, 0);
        check(gDoc.selAnchor == b0, "locate: start/end clamp to document");

        resetDoc(fiveLines);
        locateWordFromPreview("gamma", 4, 0, 0);   // endLine < startLine → = startLine
        check(gDoc.setSelCalls == 0, "locate: inverted range collapses to startLine only");
        locateWordFromPreview("last", 4, 0, 0);
        check(gDoc.setSelCalls == 1, "locate: collapsed range still searches startLine");

        // empty/null word
        resetDoc(fiveLines);
        locateWordFromPreview("", 0, 4, 0);
        locateWordFromPreview(NULL, 0, 4, 0);
        check(gDoc.setSelCalls == 0, "locate: empty/null word is a no-op");

        // UTF-8 needle (byte-exact match)
        resetDoc("plain na\xC3\xAFve text\n");
        locateWordFromPreview("na\xC3\xAFve", 0, 0, 0);
        check(gDoc.selAnchor == 6 && gDoc.selCaret == 12, "locate: UTF-8 word matches bytes");

        // empty range line (rangeEnd <= rangeStart)
        resetDoc("\n\n\n");
        locateWordFromPreview("x", 1, 1, 0);
        check(gDoc.setSelCalls == 0, "locate: empty line range is a no-op");

        // trackers + scroll after selection
        resetDoc(fiveLines);
        locateWordFromPreview("beta", 0, 4, 2);
        check(gDoc.scrollCaretCalls == 1, "locate: scrolls caret into view");
        check(sLastCaretLine == 4, "locate: caret tracker points at selected line");
        check(sLastFirstVisibleLine == gDoc.firstVisible,
              "locate: first-visible tracker synced");

        // panel hidden → no-op
        resetDoc(fiveLines);
        sPanelVisible = false;
        locateWordFromPreview("beta", 0, 4, 0);
        check(gDoc.setSelCalls == 0, "locate: hidden panel is a no-op");
        sPanelVisible = true;
    }

    // ── on_script_message parsing ──────────────────────────────────────────
    {
        // valid scroll message
        resetDoc(fiveLines);
        gDoc.firstVisible = 0;
        postJson("{\"type\":\"scroll\",\"line\":4}");
        check(gDoc.firstVisible == 4, "msg: valid scroll message moves editor");

        // valid wordTap, occ present + absent (defaults 0)
        resetDoc(fiveLines);
        postJson("{\"type\":\"wordTap\",\"word\":\"beta\",\"startLine\":0,\"endLine\":4,\"occ\":1}");
        check(gDoc.selAnchor == 17, "msg: valid wordTap selects occ 1");
        resetDoc(fiveLines);
        postJson("{\"type\":\"wordTap\",\"word\":\"beta\",\"startLine\":0,\"endLine\":4}");
        check(gDoc.selAnchor == 6, "msg: wordTap without occ defaults to 0");

        // fractional line (JS interpolation can emit non-integers pre-round;
        // native truncates via to_double cast)
        resetDoc(fiveLines);
        gDoc.firstVisible = 0;
        postJson("{\"type\":\"scroll\",\"line\":4.7}");
        check(gDoc.firstVisible == 4, "msg: fractional line truncates safely");

        // malformed payloads — must all be silent no-ops, no crash
        resetDoc(fiveLines);
        postJson("42");
        postJson("\"scroll\"");
        postJson("null");
        postJson("[1,2,3]");
        postJson("{}");
        postJson("{\"type\":5}");
        postJson("{\"type\":\"scroll\"}");
        postJson("{\"type\":\"scroll\",\"line\":\"abc\"}");
        postJson("{\"type\":\"scroll\",\"line\":null}");
        postJson("{\"type\":\"wordTap\"}");
        postJson("{\"type\":\"wordTap\",\"word\":123,\"startLine\":0,\"endLine\":1}");
        postJson("{\"type\":\"wordTap\",\"word\":\"beta\",\"startLine\":\"x\",\"endLine\":1}");
        postJson("{\"type\":\"wordTap\",\"word\":\"beta\",\"startLine\":0}");
        postJson("{\"type\":\"nonsense\",\"line\":3}");
        on_script_message(NULL, NULL, NULL);
        check(gDoc.setFirstVisibleCalls == 0 && gDoc.setSelCalls == 0,
              "msg: malformed payloads are silent no-ops");
    }

    printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
