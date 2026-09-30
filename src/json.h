/* json.h - SPEC 4.5: strict JSON in, strict JSON out.
 *
 * Why this module exists: the language could already EMIT json (out.json,
 * --fmt json, v_tojson) but not READ it, so an agent had to shell out to
 * `python -c 'import json'` for package.json / tsconfig.json / an API response.
 * One call should do it.
 *
 * Two axioms govern every line here:
 *  1. NEVER GUESS. Malformed bytes are an ERR value, never a repair. No trailing
 *     commas, no single quotes, no comments, no NaN, no bracket completion.
 *     A duplicate key is not an error (RFC 8259 allows it) but it is always
 *     REPORTED: last one wins and the result carries `dupes=N`.
 *  2. A failure must be actionable in ONE line, so `code=`, `offset=`, `line=`,
 *     `col=`, `near=` and `path=` travel as flat fields on the ERR (SPEC 2.1).
 *     offset= is a 0-based BYTE index, col= is 1-based bytes from the line start
 *     (VXA strings are byte-counted, SPEC 9), near= is <= 60 escaped chars and
 *     never contains a raw newline, because vxa output is single-line.
 *
 * Encoding: UTF-8 is passed through as bytes. \uXXXX (and a well-formed
 * surrogate pair) decodes to UTF-8; over-long, surrogate-encoded and truncated
 * sequences are errors, not replacement characters. The only extension to
 * ECMA-404 input is \xNN (<= 0x7f), because that is what this project's own
 * emitter writes for control bytes - see buf_json_str in util.c. Accepting it
 * is what makes parse(stringify(v)) close the loop for real files.
 *
 * Allocation: everything comes from the caller's Arena, nothing is global, and
 * nothing can reach arena_alloc's abort(): the byte/node budgets are checked
 * before allocating, so an exhausted arena is an E_LIMIT value, not a crash.
 */
#ifndef VXA_JSON_H
#define VXA_JSON_H
#include "vxa.h"
#include "lib.h"

/* ---------- limits (SPEC 4.5). Defaults are the shipped ones; a caller with a
 * Ctx gets them overlaid from config keys json.max_depth / json.max_bytes /
 * json.max_nodes, which is how an agent raises a limit on purpose. ---------- */
#define JSON_DEPTH_DEFAULT 128          /* nesting levels of {}/[] */
#define JSON_DEPTH_HARD_MAX 2048        /* also bounded by the C stack */
#define JSON_BYTES_DEFAULT (64LL * 1024 * 1024)
#define JSON_NODES_DEFAULT 4000000LL    /* values built by one parse */
#define JSON_SNIPPET_MAX 60             /* near= length in escaped chars */
#define JSON_PATH_MAX 160               /* path= rendered length cap */
#define JSON_MAX_SEGS 64                /* longest dotted path */

typedef struct { int max_depth; long long max_bytes, max_nodes; } JsonLimits;
typedef struct { long long bytes, nodes; int dupes, depth; } JsonMeta;

JsonLimits json_limits_default(void);
JsonLimits json_limits(Ctx *c);           /* defaults overlaid with cfg json.* */

/* ---------- parse ----------
 * V_NULL | V_BOOL | V_NUM | V_STR | V_LIST | V_REC on success, V_ERR otherwise
 * (E_PARSE for malformed bytes, E_LIMIT for depth/size/node caps, E_RANGE for a
 * number that cannot be a double). On failure *err_snippet, when given, is the
 * same escaped region the ERR carries as near= (empty when there is no region).
 * meta, when given, is zeroed and then filled: bytes = input size, nodes =
 * values materialised, depth = deepest nesting scanned, dupes = record keys
 * collapsed last-wins. */
V json_parse_lim(Arena *a, Str text, Str *err_snippet, const JsonLimits *lim, JsonMeta *meta);
V json_parse(Arena *a, Str text, Str *err_snippet);

/* ---------- render ----------
 * Strict JSON; for depth <= 32 the bytes are identical to v_tojson (tested),
 * and it keeps going to JSON_RENDER_DEPTH so that anything json_parse produced
 * round-trips. Integers never print as "1.0" (fmt_num). Non-finite numbers and
 * unsupported value types become null, exactly like v_tojson. */
#define JSON_RENDER_DEPTH 140
void json_render(Arena *a, V v, Buf *out, bool pretty);
Str json_stringify(Arena *a, V v, bool pretty);

/* ---------- addressing ----------
 * Dotted paths select a node without materialising the rest of the tree:
 *   "dependencies.build"   record key
 *   "items[2].name"        list index, brackets
 *   "items.2.name"         list index, bare number (also a record key "2")
 * A missing path is E_NOENT naming the path, where it stopped and the keys it
 * could have been. Ambiguous or malformed paths are E_SYNTAX, never a guess. */
V json_get_path(Ctx *c, V doc, Str dotted);           /* walk a parsed document */
V json_pick(Arena *a, Str text, Str dotted, Str *err_snippet);  /* parse only the match */
V json_pick_lim(Arena *a, Str text, Str dotted, Str *err_snippet,
                const JsonLimits *lim, JsonMeta *meta);

/* ---------- language surface (registered by the 2-line change in lib.c) ----------
 * json.parse(text, {meta:true})        -> {v=doc,dupes=0,nodes=17,depth=3,bytes=128}
 * json.stringify(v, {pretty:false})    -> str
 * json.query(text, path, {meta:false}) -> the value at path, without building the rest
 * json.get(doc, path)                  -> the value at path in a parsed document */
const Builtin *t_json(int *n);
#endif
