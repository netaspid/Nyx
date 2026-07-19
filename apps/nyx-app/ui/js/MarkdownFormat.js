.pragma library

/** Экранирование HTML. */
function escapeHtml(s) {
    return String(s || "")
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
}

function trimCopy(s) {
    return String(s || "").replace(/^\s+|\s+$/g, "")
}

function isActionMessage(text) {
    return String(text || "").indexOf("nyx-me:") === 0
}

function actionBody(text) {
    const t = String(text || "")
    return isActionMessage(t) ? t.substring(7) : t
}

function normalizeMe(text) {
    const t = trimCopy(text)
    if (t.indexOf("/me ") === 0 || t.indexOf("/me\t") === 0)
        return "nyx-me:" + trimCopy(t.substring(4))
    return String(text || "")
}

function formulaToHtml(latex) {
    const greeks = {
        "\\alpha": "α", "\\beta": "β", "\\gamma": "γ", "\\delta": "δ",
        "\\epsilon": "ε", "\\theta": "θ", "\\lambda": "λ", "\\mu": "μ",
        "\\pi": "π", "\\sigma": "σ", "\\phi": "φ", "\\omega": "ω",
        "\\Gamma": "Γ", "\\Delta": "Δ", "\\Omega": "Ω", "\\infty": "∞",
        "\\pm": "±", "\\times": "×", "\\cdot": "·", "\\leq": "≤",
        "\\geq": "≥", "\\neq": "≠", "\\approx": "≈"
    }
    let plain = trimCopy(latex)
    for (const k in greeks)
        plain = plain.split(k).join(greeks[k])
    plain = plain.replace(/\\frac\{([^{}]+)\}\{([^{}]+)\}/g, "($1/$2)")
    plain = plain.replace(/\\sqrt\{([^{}]+)\}/g, "√($1)")
    plain = escapeHtml(plain)
    plain = plain.replace(/\^\{([^}]+)\}/g, "<sup>$1</sup>")
    plain = plain.replace(/_\{([^}]+)\}/g, "<sub>$1</sub>")
    plain = plain.replace(/\^([A-Za-z0-9])/g, "<sup>$1</sup>")
    plain = plain.replace(/_([A-Za-z0-9])/g, "<sub>$1</sub>")
    plain = plain.replace(/\\/g, "")
    return "<span style=\"font-family:Consolas,'Segoe UI',monospace;font-style:italic;\">"
           + plain + "</span>"
}

function tableToHtml(tableSrc) {
    const lines = String(tableSrc || "").split("\n")
    function splitRow(line) {
        let t = trimCopy(line)
        if (t.charAt(0) === "|") t = t.substring(1)
        if (t.charAt(t.length - 1) === "|") t = t.substring(0, t.length - 1)
        return t.split("|").map(function(c) { return trimCopy(c) })
    }
    function isSep(line) {
        const t = trimCopy(line)
        if (t.indexOf("|") < 0 || t.indexOf("-") < 0) return false
        return /^[\|\-\:\s]+$/.test(t)
    }
    let html = "<table style=\"border-collapse:collapse;margin:4px 0;font-size:13px;\">"
    let header = true
    for (let i = 0; i < lines.length; ++i) {
        if (isSep(lines[i])) { header = false; continue }
        if (lines[i].indexOf("|") < 0) continue
        const cells = splitRow(lines[i])
        html += "<tr>"
        for (let j = 0; j < cells.length; ++j) {
            html += header
                ? "<th style=\"border:1px solid #888;padding:3px 6px;\">"
                : "<td style=\"border:1px solid #888;padding:3px 6px;\">"
            html += escapeHtml(cells[j])
            html += header ? "</th>" : "</td>"
        }
        html += "</tr>"
        if (header) header = false
    }
    return html + "</table>"
}

/**
 * Блоки: { type, text, hash, caption, displayMath }
 * type: paragraph | table | formula | media | action
 */
function parseBlocks(src) {
    const text = String(src || "")
    if (isActionMessage(text))
        return [{ type: "action", text: actionBody(text), hash: "", caption: "", displayMath: false }]

    const lines = text.split("\n")
    const blocks = []
    let para = ""

    function flushPara() {
        while (para.length && para.charAt(para.length - 1) === "\n")
            para = para.substring(0, para.length - 1)
        if (!para.length) return
        blocks.push({ type: "paragraph", text: para, hash: "", caption: "", displayMath: false })
        para = ""
    }

    function isMediaLine(line) {
        const m = trimCopy(line).match(/^!\[([^\]]*)\]\(nyx-media:([a-fA-F0-9]{64})\)\s*$/)
        return m ? { caption: m[1], hash: m[2] } : null
    }
    function isSep(line) {
        const t = trimCopy(line)
        return t.indexOf("|") >= 0 && t.indexOf("-") >= 0 && /^[\|\-\:\s]+$/.test(t)
    }
    function looksTable(line) {
        return trimCopy(line).indexOf("|") >= 0
    }

    for (let i = 0; i < lines.length; ++i) {
        const line = lines[i]
        const trimmed = line.replace(/^\s+/, "")

        // Блок кода ```lang … ``` — отдельный блок (перенос/скролл в QML)
        if (trimmed.indexOf("```") === 0) {
            flushPara()
            const lang = trimmed.substring(3).trim()
            let code = ""
            ++i
            for (; i < lines.length; ++i) {
                if (trimCopy(lines[i]).indexOf("```") === 0)
                    break
                if (code.length) code += "\n"
                code += lines[i]
            }
            blocks.push({
                type: "code", text: code, hash: "",
                caption: lang, displayMath: false
            })
            continue
        }

        if (trimmed.indexOf("$$") === 0) {
            flushPara()
            let body = trimmed.substring(2)
            const end = body.lastIndexOf("$$")
            if (end > 0) {
                blocks.push({
                    type: "formula", text: trimCopy(body.substring(0, end)),
                    hash: "", caption: "", displayMath: true
                })
            } else {
                let math = body
                ++i
                for (; i < lines.length; ++i) {
                    const t2 = trimCopy(lines[i])
                    if (t2 === "$$" || t2.substring(t2.length - 2) === "$$") {
                        if (t2 !== "$$") {
                            if (math.length) math += "\n"
                            math += t2.substring(0, t2.length - 2)
                        }
                        break
                    }
                    if (math.length) math += "\n"
                    math += lines[i]
                }
                blocks.push({
                    type: "formula", text: trimCopy(math),
                    hash: "", caption: "", displayMath: true
                })
            }
            continue
        }

        const media = isMediaLine(line)
        if (media) {
            flushPara()
            blocks.push({
                type: "media", text: "", hash: media.hash,
                caption: media.caption, displayMath: false
            })
            continue
        }

        if (looksTable(line) && i + 1 < lines.length && isSep(lines[i + 1])) {
            flushPara()
            let table = line + "\n" + lines[i + 1]
            i += 2
            for (; i < lines.length && looksTable(lines[i]) && !isSep(lines[i]); ++i)
                table += "\n" + lines[i]
            --i
            blocks.push({ type: "table", text: table, hash: "", caption: "", displayMath: false })
            continue
        }

        if (para.length) para += "\n"
        para += line
    }
    flushPara()
    if (!blocks.length && text.length)
        blocks.push({ type: "paragraph", text: text, hash: "", caption: "", displayMath: false })
    return blocks
}

/** Разбить абзац на md / spoiler сегменты (для Telegram-спойлеров в QML). */
function splitSpoilers(src) {
    const s = String(src || "")
    const parts = []
    const re = /\|\|([\s\S]+?)\|\|/g
    let last = 0
    let m
    let i = 0
    while ((m = re.exec(s)) !== null) {
        if (m.index > last)
            parts.push({ type: "md", text: s.substring(last, m.index) })
        parts.push({ type: "spoiler", text: m[1], index: i++ })
        last = m.index + m[0].length
    }
    if (last < s.length)
        parts.push({ type: "md", text: s.substring(last) })
    if (!parts.length)
        parts.push({ type: "md", text: s })
    return parts
}

/** Ключевые слова по языку (для lite-подсветки в пузыре). */
function _keywordsFor(lang) {
    const l = String(lang || "").toLowerCase()
    const common = "if else for while return break continue true false null class struct " +
                   "const static void int bool new delete this public private protected"
    const map = {
        cpp: common + " include namespace using typedef enum template typename auto " +
             "std string vector map optional nullptr constexpr inline virtual override " +
             "char short long float double unsigned signed wchar_t size_t uint8_t uint16_t " +
             "uint32_t uint64_t int8_t int16_t int32_t int64_t try catch throw noexcept",
        c: common + " include typedef enum sizeof extern register volatile",
        h: common + " include typedef enum sizeof extern",
        hpp: common + " include namespace using typedef template typename auto nullptr",
        python: "def class return if elif else for while import from as try except " +
                "finally with lambda yield True False None and or not in is pass break " +
                "continue raise assert global nonlocal async await",
        py: "def class return if elif else for while import from as try except " +
            "finally with lambda yield True False None and or not in is pass",
        js: "function return if else for while const let var class new this typeof " +
            "async await import export from default true false null undefined try catch " +
            "throw break continue switch case",
        javascript: "function return if else for while const let var class new this typeof " +
                    "async await import export from default true false null undefined",
        ts: "function return if else for while const let var class new this typeof " +
            "async await import export from default true false null undefined interface type",
        qml: "import property signal readonly required id as on true false null undefined " +
             "function return if else for while var let const",
        json: "true false null",
        bash: "if then else elif fi for do done while case esac function return " +
              "echo export local readonly",
        sh: "if then else elif fi for do done while case esac function return echo export",
        html: "html head body div span script style title meta link",
        css: "important important color background border margin padding display flex " +
             "position absolute relative"
    }
    const raw = map[l] || (l.indexOf("c++") >= 0 ? map.cpp : common)
    const set = {}
    raw.split(/\s+/).forEach(function(w) { if (w) set[w] = true })
    return set
}

/**
 * Lite syntax highlight → HTML (цвета как в тёмной IDE).
 * Комментарии, строки, числа, препроцессор, ключевые слова.
 */
function highlightCode(lang, code) {
    const kw = _keywordsFor(lang)
    const C = {
        def: "#d4d4d4",
        kw: "#569cd6",
        str: "#ce9178",
        cmt: "#6a9955",
        num: "#b5cea8",
        pp: "#c586c0",
        type: "#4ec9b0"
    }
    function span(color, s) {
        return "<span style=\"color:" + color + ";\">" + s + "</span>"
    }

    const src = String(code || "")
    let out = ""
    let i = 0
    const n = src.length

    while (i < n) {
        // line comment //
        if (src[i] === "/" && src[i + 1] === "/") {
            let j = i
            while (j < n && src[j] !== "\n") j++
            out += span(C.cmt, escapeHtml(src.substring(i, j)))
            i = j
            continue
        }
        // block comment
        if (src[i] === "/" && src[i + 1] === "*") {
            let j = i + 2
            while (j < n && !(src[j] === "*" && src[j + 1] === "/")) j++
            j = Math.min(n, j + 2)
            out += span(C.cmt, escapeHtml(src.substring(i, j)))
            i = j
            continue
        }
        // python #
        if (src[i] === "#" && (lang || "").toLowerCase().indexOf("py") === 0) {
            let j = i
            while (j < n && src[j] !== "\n") j++
            out += span(C.cmt, escapeHtml(src.substring(i, j)))
            i = j
            continue
        }
        // preprocessor # (начало строки, не python-коммент)
        if (src[i] === "#" && (i === 0 || src[i - 1] === "\n")) {
            let j = i
            while (j < n && src[j] !== "\n") j++
            out += span(C.pp, escapeHtml(src.substring(i, j)))
            i = j
            continue
        }
        // strings
        if (src[i] === "\"" || src[i] === "'") {
            const q = src[i]
            let j = i + 1
            while (j < n) {
                if (src[j] === "\\") { j += 2; continue }
                if (src[j] === q) { j++; break }
                if (src[j] === "\n") break
                j++
            }
            out += span(C.str, escapeHtml(src.substring(i, j)))
            i = j
            continue
        }
        // numbers
        if (/[0-9]/.test(src[i]) && (i === 0 || /[^\w]/.test(src[i - 1]))) {
            let j = i
            while (j < n && /[0-9a-fxA-FX._]/.test(src[j])) j++
            out += span(C.num, escapeHtml(src.substring(i, j)))
            i = j
            continue
        }
        // identifiers / keywords
        if (/[A-Za-z_]/.test(src[i])) {
            let j = i
            while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++
            const word = src.substring(i, j)
            const esc = escapeHtml(word)
            if (kw[word])
                out += span(C.kw, esc)
            else if (/^[A-Z]/.test(word) && word.length > 1)
                out += span(C.type, esc)
            else
                out += span(C.def, esc)
            i = j
            continue
        }
        out += escapeHtml(src[i])
        i++
    }
    return out
}

/** Подсветка с мягким переносом длинных строк (Qt RichText плохо умеет <pre>). */
function highlightCodeWrapped(lang, code, maxCols) {
    const lim = maxCols || 72
    const body = String(code || "").replace(/^\n/, "").replace(/\n$/, "")
    const lines = body.split("\n")
    let html = ""
    for (let li = 0; li < lines.length; ++li) {
        if (li > 0) html += "<br/>"
        const line = lines[li]
        const m = line.match(/^(\s*)([\s\S]*)$/)
        const indentRaw = m ? m[1] : ""
        const indent = indentRaw.replace(/ /g, "&nbsp;").replace(/\t/g, "&nbsp;&nbsp;&nbsp;&nbsp;")
        const rest = m ? m[2] : line
        if (!rest.length) {
            html += indent.length ? indent : "&nbsp;"
            continue
        }
        let pos = 0
        let chunk = 0
        while (pos < rest.length) {
            if (chunk > 0) html += "<br/>" + indent
            let end = Math.min(pos + lim, rest.length)
            if (end < rest.length) {
                const sp = rest.lastIndexOf(" ", end)
                const slash = rest.lastIndexOf("/", end)
                const brk = Math.max(sp, slash)
                if (brk > pos + Math.floor(lim / 3))
                    end = brk + 1
            }
            if (chunk === 0) html += indent
            html += highlightCode(lang, rest.substring(pos, end))
            pos = end
            chunk++
        }
    }
    return html
}

function codeBlockHtml(lang, code) {
    const label = String(lang || "").trim()
    let html = "<div style=\"background-color:#1a2332;border-radius:8px;padding:8px 10px;"
             + "margin:4px 0;border:1px solid rgba(255,255,255,0.08);\">"
    if (label.length)
        html += "<div style=\"font-size:11px;color:#8b9bab;margin-bottom:6px;"
              + "font-family:'Segoe UI';\">" + escapeHtml(label) + "</div>"
    // без <pre> — Qt RichText иначе не переносит длинные строки
    html += "<div style=\"font-family:Consolas,'Cascadia Mono',monospace;font-size:12.5px;"
          + "line-height:1.45;\">"
          + highlightCodeWrapped(label, code, 68) + "</div></div>"
    return html
}

/**
 * Paragraph → HTML.
 * @param skipSpoilers — спойлеры уже вынесены в SpoilerSpan
 */
function toHtml(src, revealedSpoilers, skipSpoilers) {
    const revealed = revealedSpoilers || {}
    let text = String(src || "")
    const fences = []
    text = text.replace(/```([A-Za-z0-9_+#.-]*)[ \t]*\n?([\s\S]*?)```/g, function(_, lang, code) {
        const i = fences.length
        fences.push(codeBlockHtml(lang, code))
        return "\uE000F" + i + "\uE000"
    })

    const maths = []
    text = text.replace(/\$([^\$\n]+)\$/g, function(_, body) {
        const i = maths.length
        maths.push(formulaToHtml(body))
        return "\uE002M" + i + "\uE002"
    })

    const inlines = []
    text = text.replace(/`([^`\n]+)`/g, function(_, code) {
        const i = inlines.length
        inlines.push("<code style=\"font-family:Consolas,monospace;background-color:rgba(127,127,127,0.25);\">"
                     + escapeHtml(code) + "</code>")
        return "\uE001I" + i + "\uE001"
    })

    text = escapeHtml(text)

    if (!skipSpoilers) {
        let spoilerI = 0
        text = text.replace(/\|\|([\s\S]+?)\|\|/g, function(_, body) {
            const i = spoilerI++
            if (revealed[i])
                return "<span>" + body + "</span>"
            return "<a href=\"nyx-spoiler:" + i + "\">" + body + "</a>"
        })
    }

    text = text.replace(/\[([^\]]+)\]\((nyx-user:[a-fA-F0-9]{64}|https?:\/\/[^)\s]+)\)/g,
                        "<a href=\"$2\">$1</a>")

    text = text.replace(/\*\*([^*\n]+)\*\*/g, "<b>$1</b>")
    text = text.replace(/__([^_\n]+)__/g, "<u>$1</u>")
    text = text.replace(/~~([^~\n]+)~~/g, "<s>$1</s>")
    text = text.replace(/(^|[^\w*])\*([^*\n]+)\*(?!\*)/g, "$1<i>$2</i>")
    text = text.replace(/(^|[^\w_])_([^_\n]+)_(?!_)/g, "$1<i>$2</i>")

    const lines = text.split("\n")
    let html = ""
    let inQuote = false
    let inUl = false
    let inOl = false
    function closeLists() {
        if (inUl) { html += "</ul>"; inUl = false }
        if (inOl) { html += "</ol>"; inOl = false }
    }

    for (let li = 0; li < lines.length; ++li) {
        let line = lines[li]
        const qm = line.match(/^&gt;\s?(.*)$/)
        const trimmed = trimCopy(line)

        if (trimmed === "---" || trimmed === "***") {
            if (inQuote) { html += "</blockquote>"; inQuote = false }
            closeLists()
            html += "<hr/>"
            continue
        }
        if (qm) {
            closeLists()
            if (!inQuote) {
                html += "<blockquote style=\"margin:4px 0;padding-left:8px;border-left:3px solid #5288c1;\">"
                inQuote = true
            } else {
                html += "<br/>"
            }
            html += qm[1]
            continue
        }
        if (inQuote) { html += "</blockquote>"; inQuote = false }

        const hm = line.match(/^(#{1,3})\s+(.+)$/)
        if (hm) {
            closeLists()
            const level = hm[1].length
            const px = level === 1 ? 22 : (level === 2 ? 18 : 16)
            html += "<div style=\"font-weight:700;font-size:" + px + "px;margin:4px 0;\">"
                    + hm[2] + "</div>"
            continue
        }
        const ul = line.match(/^[-*]\s+(.+)$/)
        if (ul) {
            if (inOl) { html += "</ol>"; inOl = false }
            if (!inUl) { html += "<ul style=\"margin:4px 0;padding-left:18px;\">"; inUl = true }
            html += "<li>" + ul[1] + "</li>"
            continue
        }
        const ol = line.match(/^(\d+)\.\s+(.+)$/)
        if (ol) {
            if (inUl) { html += "</ul>"; inUl = false }
            if (!inOl) { html += "<ol style=\"margin:4px 0;padding-left:18px;\">"; inOl = true }
            html += "<li>" + ol[2] + "</li>"
            continue
        }
        closeLists()
        if (li > 0) html += "<br/>"
        html += line
    }
    if (inQuote) html += "</blockquote>"
    closeLists()

    for (let i = 0; i < inlines.length; ++i)
        html = html.replace("\uE001I" + i + "\uE001", inlines[i])
    for (let j = 0; j < maths.length; ++j)
        html = html.replace("\uE002M" + j + "\uE002", maths[j])
    for (let k = 0; k < fences.length; ++k)
        html = html.replace("\uE000F" + k + "\uE000", fences[k])

    return html
}

/** HTML всего сообщения (для превью композера). */
function documentToHtml(src, revealedSpoilers) {
    const blocks = parseBlocks(src)
    let html = ""
    for (let i = 0; i < blocks.length; ++i) {
        const b = blocks[i]
        if (b.type === "action") {
            html += "<i>" + escapeHtml(b.text) + "</i>"
        } else if (b.type === "table") {
            html += tableToHtml(b.text)
        } else if (b.type === "formula") {
            html += "<div style=\"text-align:center;margin:6px 0;\">"
                    + formulaToHtml(b.text) + "</div>"
        } else if (b.type === "code") {
            html += codeBlockHtml(b.caption, b.text)
        } else if (b.type === "media") {
            html += "<div>[" + escapeHtml(b.caption || "media") + "]</div>"
        } else {
            html += toHtml(b.text, revealedSpoilers)
        }
        if (i + 1 < blocks.length) html += "<br/>"
    }
    return html
}
