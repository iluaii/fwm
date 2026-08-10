# fwm — merge a user's config.toml with the shipped example.
#
#   awk -f config-merge.awk [-v stats=FILE] yours.toml config.toml.example
#
# The example is the skeleton: its comments, its ordering, and every option it
# has grown since your file was written. Your file is where the VALUES come
# from — wherever both name the same key in the same section, your line is the
# one that lands in the output, inline comment and all. Keys you have that the
# example does not (your own binds, a mode of your own) are kept at the end of
# the section they were in, and sections the example has never heard of are kept
# at the end of the file. Nothing you set is dropped.
#
# Only ACTIVE key lines are substituted. A commented line in the example is a
# suggestion rather than a setting, so it is copied through as it stands and the
# file goes on reading like the documentation it is; if you had enabled one of
# those, your version arrives in the section's kept block instead.
#
# Line-oriented, deliberately: it has to preserve comments, and no TOML parser
# does. The one thing it cannot see is a value written across several lines —
# an array broken over a few of them — which is why the merged file is offered
# for review rather than written blind.

function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }

# The key an active "key = value" line sets, or "" for anything else.
function keyof(line,   s, p, k) {
    s = trim(line)
    if (s == "" || substr(s, 1, 1) == "#" || substr(s, 1, 1) == "[") return ""
    p = index(s, "=")
    if (p == 0) return ""
    k = trim(substr(s, 1, p - 1))
    return (k == "") ? "" : k
}

# "[name]" or "#[name]" -> name; "" when the line is not a section header. An
# array-of-tables header ([[rule]]) comes back with its brackets kept, since two
# of them are two different blocks and must never be merged key by key.
function secof(line,   s) {
    s = trim(line)
    if (substr(s, 1, 1) == "#") s = trim(substr(s, 2))
    if (substr(s, 1, 1) != "[") return ""
    if (s !~ /^\[\[?[^]]+\]\]?/) return ""
    sub(/[ \t]*#.*$/, "", s)
    return trim(s)
}

function is_table_array(name) { return substr(name, 1, 2) == "[[" }

# Everything of yours that the example had no place for.
function flush_kept(s,   i, n, parts, k, printed) {
    if (!(s in ukeys)) return
    n = split(ukeys[s], parts, SUBSEP)
    printed = 0
    for (i = 1; i <= n; i++) {
        k = parts[i]
        if (k == "" || ((s, k) in used)) continue
        if (!printed) {
            print ""
            print "# ── kept from your config ─────────────────────────────────────────────"
            printed = 1
        }
        print uval[s, k]
        used[s, k] = 1
        kept_n++
    }
    if (printed) print ""
}

# ── pass 1: your config, read into a table ─────────────────────────────
NR == FNR {
    name = secof($0)
    if (name != "" && trim($0) !~ /^#/) {
        if (is_table_array(name)) {
            # Opaque: several blocks share one name, so they travel whole.
            usec = name "#" (++dup[name])
            opaque[usec] = 1
        } else {
            usec = name
        }
        if (!(usec in useen)) { uorder[++un] = usec; useen[usec] = 1 }
        uraw[usec] = uraw[usec] $0 "\n"
        next
    }
    uraw[usec] = uraw[usec] $0 "\n"
    if (opaque[usec]) next

    k = keyof($0)
    if (k == "" || ((usec, k) in uval)) next
    uval[usec, k] = $0
    ukeys[usec] = (usec in ukeys) ? ukeys[usec] SUBSEP k : k
    yours_n++
    next
}

# ── pass 2: the example, walked line by line ───────────────────────────
{
    name = secof($0)
    if (name != "") {
        if (trim($0) ~ /^#/) {
            # A commented section: the keys under it are its own, not the
            # enclosing section's, or a "#g = ..." inside a suggested mode would
            # be matched against your [gestures].
            csec = name
            print
            next
        }
        flush_kept(sec)
        sec = name
        csec = ""
        seen[sec] = 1
        print
        next
    }

    k = (csec == "") ? keyof($0) : ""
    if (k == "") { print; next }

    if ((sec, k) in used) next          # the example names it twice; once is enough
    if ((sec, k) in uval) {
        print uval[sec, k]
        used[sec, k] = 1
        yours_kept++
    } else {
        print
        new_n++
    }
}

END {
    flush_kept(sec)

    for (i = 1; i <= un; i++) {
        s = uorder[i]
        if (s in seen) continue
        if (s == "") continue
        print ""
        print "# ── kept from your config: the example does not have this section ─────"
        printf "%s", uraw[s]
        sections_n++
    }

    # For the caller to report: settings found in your file, of those the ones
    # placed back into the example's own lines, the ones kept aside because the
    # example has no line for them, whole sections kept, and options that are
    # new to you.
    if (stats != "")
        printf "%d %d %d %d %d\n",
               yours_n, yours_kept, kept_n, sections_n, new_n > stats
}
