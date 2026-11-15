#include <wordexp.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include "pthread_impl.h"

static void reap(pid_t pid)
{
	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
}

static char *getword(FILE *f)
{
	char *s = 0;
	return getdelim(&s, (size_t [1]){0}, 0, f) < 0 ? 0 : s;
}

static int is_wc(int c)
{
	return c == '_' || (c >= '0' && c <= '9') ||
		(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ANSI-C $'...' unit at s (s[0]=='$', s[1]=='\'').
 * *len set past the closing quote.
 * in_arith/in_dq describe the enclosing scan: inside arithmetic or
 * double quotes $'...' is NOT an ANSI span (the shell treats $ and '
 * literally there), so an embedded $(...) or backquote executes and
 * must report dirty. In a genuine ANSI position the span content is
 * literal and never executes: report clean (unterminated stays -1,
 * empty stays 2). Callers guarantee sq==0 ($ is suppressed there).
 * Returns 1 dirty (executable cmdsubst inside), 2 if empty,
 * 0 if clean, -1 if unterminated. */
static int ansi_unit(const char *s, size_t *len, int in_arith, int in_dq)
{
	size_t i = 2;
	int dirty = 0, empty = 1;
	for (;;) {
		if (s[i] == '\\') {
			if (!s[i+1]) return -1;
			empty = 0;
			i += 2;
			continue;
		}
		if (s[i] == '\'') {
			*len = i+1;
			if (empty) return 2;
			return dirty;
		}
		if (!s[i]) return -1;
		empty = 0;
		if ((in_arith || in_dq) && s[i] == '$' && s[i+1] == '(') dirty = 1;
		if ((in_arith || in_dq) && s[i] == '`') dirty = 1;
		i++;
	}
}

static int brace_unit(const char *s, size_t *len, int in_arith, int depth,
	int in_dq);
static int arith_ok(const char *p, size_t *len);

/* Bound on nested ${...} recursion: the scanner recurses per nesting
 * level, so unbounded input depth would exhaust the stack. Legitimate
 * expansions nest single digits deep; anything beyond this is treated
 * as too complex to verify safe (fail closed, like upstream). */
#define BRACE_MAXDEPTH 64

/* ${...} unit at s (s[0]=='$', s[1]=='{').
 * in_arith: single-quote does not suppress cmdsub detection.
 * in_dq: the double-quote state enclosing this brace word (the
 * top-level scan's dq at '${', or the enclosing word's current
 * dq for nested braces; 0 inside arithmetic, which aborts on any
 * quote). Pairing stays nested (a fresh sq/dq pair per unit, as
 * the shell pairs quotes when finding the closing brace). A
 * single-quoted region opened under outer double quotes does NOT
 * suppress $(...) or backquote detection ("${X:-'$(touch M)'}"
 * executes in the shell despite the quotes, because ' is literal
 * inside "..."; the pre-check must see through it). Everything
 * else behaves exactly as the old scan. *len set past the
 * closing '}'.
 * Returns 0 clean, 1 dirty (command substitution inside),
 * 2 malformed or too deeply nested, -1 to fall back to legacy
 * handling (bare brace or unbalanced: the caller reverts to
 * historical behaviour). */
static int brace_unit(const char *s, size_t *len, int in_arith, int depth,
	int in_dq)
{
	size_t i = 2;
	int brdepth = 1, sq = 0, dq = 0;
	if (depth > BRACE_MAXDEPTH)
		return 2;
	while (s[i]) {
		char c = s[i];
		if (c == '\\') {
			if (!s[i+1]) return -2;
			i += 2;
			continue;
		}
		if (c == '\'' && !dq) {
			sq ^= 1;
			i++;
			continue;
		}
		if (c == '"' && !sq) {
			dq ^= 1;
			i++;
			continue;
		}
		if (c == '$' && s[i+1] == '\'' && !sq) {
			size_t al = 0;
			/* $'...' is genuine ANSI only when neither the
			 * enclosing context (in_dq) nor the quotes opened
			 * so far in this unit (dq) put it inside double
			 * quotes; in either dq case the shell reads $ and '
			 * literally and an embedded $(...)/backquote runs. */
			int a = ansi_unit(s+i, &al, in_arith, (in_dq || dq));
			if (a < 0) return -2;
			if (a == 2) return 2;
			if (a) return 1;
			i += al;
			continue;
		}
		if (c == '$' && s[i+1] == '"' &&
		    (in_arith || !sq || (sq && in_dq))) {
			/* A "$" pair may CLOSE an enclosing double-quote
			 * (the in_dq context) rather than opening a new one.
			 * When it closes inside arithmetic, or anywhere a
			 * double-quote context may be live, a directly
			 * following paren forms a live $( command
			 * substitution that the shell executes (S6q:
			 * "${X-"$"(...)}"; R10c inside $((...))), while a
			 * literal reading misses it entirely. Fail closed
			 * on the ambiguous shape: a paren that cannot
			 * start validated arithmetic is a command
			 * substitution request. With no enclosing
			 * double-quote context (and outside arithmetic),
			 * a "$" can only open a new quoted span -- the
			 * unit's own dq tracking then governs, and the
			 * historical literal treatment below is kept. */
			if ((in_arith || in_dq) && s[i+2] == '(') {
				if (s[i+3] == '(') {
					size_t al = 0;
					int ar = arith_ok(s+i+2, &al);
					if (ar == 0) {
						i += 2 + al;
						continue;
					}
					if (ar == 1) return 1;
					/* Malformed arithmetic (ar==2)
					 * falls through to the literal scan
					 * below: it cannot execute, and the
					 * shell itself rejects what remains. */
				} else {
					return 1;
				}
			}
		}
		/* A bare { opens no executable construct on its own (command
		 * grouping needs ;/newline plus a matching close, all of
		 * which expand as data inside a default word, never as
		 * code the pre-check must see through). Treating it as
		 * literal keeps lazily-skipped defaults expanding exactly
		 * as the shell expands them (${X-{}} -> {}); unbalanced
		 * input still fails closed at the end of the unit. */
		if (c == '{' && !sq) {
			i++;
			continue;
		}
		if (c == '$' && s[i+1] == '{' && !sq) {
			size_t nl = 0;
			int n = brace_unit(s+i, &nl, in_arith, depth+1, dq);
			if (n < 0) return n;
			if (n == 2) return 2;
			if (n) return 1;
			i += nl;
			continue;
		}
		if (c == '$' && s[i+1] == '(' && s[i+2] == '(' && !sq) {
			size_t al = 0;
			int ar = arith_ok(s+i+1, &al);
			if (ar == 0) {
				i += 1 + al;
				continue;
			}
			if (ar == 1) return 1;
			/* Malformed arithmetic (ar==2) falls through to
			 * the literal scan below: it cannot execute.
			 * Every construct the shell could execute as a
			 * command -- $(...), backquotes, quotes, ; and
			 * newlines, adjacent names -- already reports 1
			 * above, so reaching here means no executable
			 * content. The literal scan still trips on an
			 * inner $( or backquote, and the shell itself
			 * rejects what remains (case 34's shape, with
			 * command words and ;, reports 1 and is blocked).
			 * Treating malformed arithmetic as literal keeps
			 * lazily-skipped defaults working: ${X-$((}))}
			 * expands fine when X is set, exactly as the
			 * shell does. */
			i++;
			continue;
		}
		if (c == '$' && s[i+1] == '(') {
			if (s[i+2] != '(') {
				if (in_arith || !sq || (sq && in_dq)) return 1;
				i += 2;
				continue;
			}
		}
		if (c == '`' && (in_arith || !sq || (sq && in_dq))) return 1;
		if (c == '}' && !sq) {
			if (--brdepth == 0) {
				*len = i+1;
				return 0;
			}
			i++;
			continue;
		}
		i++;
	}
	return -2;
}

/* Validate "((...))" arithmetic at p (p[0]=='(', p[1]=='(').
 * *len set through the final ')' on success.
 * Returns 0 valid, 1 blocked (caller: WRDE_CMDSUB),
 * 2 malformed (caller: WRDE_SYNTAX). */
static int arith_ok(const char *p, size_t *len)
{
	size_t i = 0;
	int depth = 0, bdepth = 0, last = 0;
	while (p[i]) {
		char c = p[i];
		if (c == '(') {
			depth++;
			last = 0;
			i++;
			continue;
		}
		if (c == '[') {
			bdepth++;
			last = 0;
			i++;
			continue;
		}
		if (c == ')') {
			if (last != 1) return 2;
			if (--depth == 0) {
				/* The shell only parses $((...)) as
				 * arithmetic when the two closing parens
				 * are adjacent: '$((1 + 2 ) )' and
				 * '$((echo + 1 ) )' both execute as $(...)
				 * command substitution in bash (which is
				 * also /bin/sh here, the shell wordexp
				 * invokes), verified 2026-11-12. A spaced
				 * close is therefore not verified
				 * arithmetic: fail closed (caller blocks).
				 * (A genuine close followed by a literal
				 * " )" still ends here first: the close
				 * itself stays adjacent.) */
				if (p[i-1] != ')')
					return 1;
				*len = i+1;
				return 0;
			}
			last = 1;
			i++;
			continue;
		}
		if (c == ']') {
			if (bdepth == 0 || last != 1) return 1;
			bdepth--;
			last = 1;
			i++;
			continue;
		}
		if (c == ' ' || c == '\t') {
			i++;
			continue;
		}
		if (c == '\'')
			return 1;
		if (c == '"') {
			/* Double-quoted span inside arithmetic: the shell
			 * still expands it (so $(("1")) is valid, value 1),
			 * but $(...) and backquotes inside execute. Scan the
			 * span: command substitution blocks, ${...} words
			 * recurse through brace_unit (in quoted context, so
			 * bogus single-quotes cannot hide execution), anything
			 * else is inert arithmetic content. An unterminated
			 * quote is malformed. Adjacent values stay rejected,
			 * mirroring the word rule below. */
			size_t j = i+1;
			while (p[j] && p[j] != '"') {
				if (p[j] == '\\') {
					if (!p[j+1]) return 2;
					j += 2;
					continue;
				}
				if (p[j] == '$' && p[j+1] == '(') return 1;
				if (p[j] == '`') return 1;
				if (p[j] == '$' && p[j+1] == '{') {
					size_t bl = 0;
					int b = brace_unit(p+j, &bl, 1, 0, 1);
					if (b < 0) return 2;
					if (b) return 1;
					j += bl;
					continue;
				}
				j++;
			}
			if (!p[j]) return 2;
			if (last == 1) return 1;
			last = 1;
			i = j+1;
			continue;
		}
		if (c == '\\' || c == '`' || c == ';' ||
		    c == '\n') return 1;
		if (c == '#') {
			/* base notation 16#ff needs a digit before # */
			if (i > 0 && p[i-1] >= '0' && p[i-1] <= '9') {
				while (is_wc(p[i+1]) || p[i+1] == '@')
					i++;
				i++;
				last = 1;
				continue;
			}
			return 2;
		}
		if (c == '$') {
			if (p[i+1] == '\'') {
				size_t al = 0;
				int a = ansi_unit(p+i, &al, 1, 0);
				if (a) return a == 2 ? 2 : 1;
				if (last == 1) return 1;
				last = 1;
				i += al;
				continue;
			}
			if (p[i+1] == '{') {
				size_t bl = 0;
				int b;
				if (last == 1) return 1;
				/* in_dq=0: arithmetic aborts on any quote,
				 * so no quoted context can be live here. */
				b = brace_unit(p+i, &bl, 1, 0, 0);
				if (b < 0) return 2;
				if (b) return 1;
				last = 1;
				i += bl;
				continue;
			}
			if (p[i+1] == '(') {
				if (p[i+2] != '(') return 1;
				i++;
				last = 0;
				continue;
			}
			{
				size_t j = i+1;
				if (p[j] == '{' || p[j] == '\'')
					return 1;
				if (is_wc(p[j]) || p[j] == '#' ||
				    p[j] == '?' || p[j] == '-' ||
				    p[j] == '$' || p[j] == '!') {
					if (last == 1) return 1;
					last = 1;
					i++;
					while (is_wc(p[i])) i++;
					continue;
				}
				return 1;
			}
		}
		if (is_wc(c) || c == '.') {
			size_t j = i;
			if (last == 1) return 1;
			while (is_wc(p[j]) || p[j] == '.') j++;
			last = 1;
			i = j;
			continue;
		}
		if (c == '}') {
			/* A bare } has no meaning inside arithmetic
			 * (nested ${...} is consumed by the $ case
			 * above, so anything reaching here is
			 * unbalanced): malformed, not blocked. The
			 * shell cannot execute through it either --
			 * a } closes nothing here -- so failing
			 * closed as SYNTAX-equivalent is safe, and
			 * falling through lets lazily-skipped
			 * defaults (${X-$((}))}) expand exactly as
			 * the shell expands them. (A bare { stays
			 * blocked: it can open an executable group.) */
			return 2;
		}
		{
			size_t j = i;
			while (p[j] == '+' || p[j] == '-' || p[j] == '*' ||
			       p[j] == '/' || p[j] == '%' || p[j] == '<' ||
			       p[j] == '>' || p[j] == '&' || p[j] == '|' ||
			       p[j] == '^' || p[j] == '~' || p[j] == '=' ||
			       p[j] == '!' || p[j] == '?' || p[j] == ':' ||
			       p[j] == ',')
				j++;
			if (j == i) return 1;
			if (j - i == 1 && (p[i] == '&' || p[i] == '|'))
				return 1;
			if (last == 1) {
				last = 2;
				i = j;
				continue;
			}
			{
				size_t k = i;
				while (k < j && (p[k] == '+' || p[k] == '-' ||
						 p[k] == '~' || p[k] == '!'))
					k++;
				if (k != j) return 2;
				i = j;
				continue;
			}
		}
	}
	return 2;
}

/* WRDE_UNDEF support: detect attempts to expand unset variables
 * without executing anything (Sinew 0020).
 *
 * undef_ref(s) returns 1 if s unambiguously attempts to expand an
 * unset shell variable, 0 otherwise (clean input, or too ambiguous
 * to judge: pass through to the shell, preserving current behaviour).
 *
 * Model (POSIX shell left-to-right, per SUSv3 sections 2.2/2.5/2.6;
 * glibc H-series behaviour corroborates but does not define it):
 * - Tracks quoting (sq/dq, ANSI $'...' skipped as literal),
 *   backslash escapes, and #-comments (word-start, outside quotes).
 * - $(...) and `...` interiors are skipped, never analysed: the
 *   command's business, not wordexp's. $((...)) contents ARE
 *   analysed (arithmetic expansion attempts count).
 * - ${...} operators with lazy-branch evaluation: only evaluated
 *   branches are descended into. ${v=word}/${v:=word} record v as
 *   shell-locally defined (bounded set below; evaluated assignments
 *   only — environment values come from getenv, never conflated).
 * - Special parameters ($*, $@, $#, $?, $-, $$, $!, $0-9) and
 *   tilde/globs are never attempts (POSIX leaves specials
 *   unspecified; current behaviour preserved).
 * - Depth over BRACE_MAXDEPTH, unbalanced input, bare braces,
 *   indirection (${!...}), quoted arithmetic operands, arithmetic
 *   laziness (? && ||) or anything unparseable: pass through
 *   (return 0). Fail-open is a provisional strategy, not a
 *   conformance claim: false negatives allowed, false positives
 *   forbidden. Known gaps are documented, not silently resolved.
 * - Assigned-set overflow (pathological inputs): pass through.
 * - Runs before fork, only when WRDE_UNDEF is set: a WRDE_BADVAL
 *   return performs zero command side effects by construction.
 *   No fork/exec/pipe, no new shell flags, no rerun. */

/* Bounded shell-local assignment set. Entries point into the scanned
 * input string (no allocation, no truncation). Emptiness of assigned
 * values is not tracked: later null-tests (:- :? :+) on assigned
 * names pass through. */
#define UNDEF_MAXASSIGN 64
struct undef_asn {
	const char *name;
	size_t len;
};

/* Definedness of a variable name (not NUL-terminated): environment
 * first, then shell-local assignments recorded earlier in the same
 * word. Returns 2 if set with known-nonempty value (environment),
 * 1 if set but known-empty (environment), 3 if shell-locally
 * assigned (defined, emptiness unknown), 0 if unset, -1 if
 * unjudgeable (overlong name: pass through). */
static int undef_isdef(const char *name, size_t len,
	struct undef_asn *asn, int nasn)
{
	char nbuf[256];
	char *v;
	int i;
	if (len >= sizeof nbuf) {
		return -1;
	}
	for (i = 0; i < nasn; i++) {
		if (asn[i].len == len && !memcmp(asn[i].name, name, len)) {
			return 3;
		}
	}
	memcpy(nbuf, name, len);
	nbuf[len] = 0;
	v = getenv(nbuf);
	if (!v) {
		return 0;
	}
	return *v ? 2 : 1;
}

/* Record a shell-local assignment from an evaluated =/:= operator.
 * Returns 0 ok, -1 when the bounded set overflows (pass through). */
static int undef_assign(const char *name, size_t len,
	struct undef_asn *asn, int *nasn)
{
	if (*nasn >= UNDEF_MAXASSIGN) {
		return -1;
	}
	asn[*nasn].name = name;
	asn[*nasn].len = len;
	(*nasn)++;
	return 0;
}

/* Skip a `...` span at s. No nesting (POSIX sh). Returns past the
 * closing backquote, NULL if unterminated. Contents are never
 * analysed (D1: the command's business). */
static const char *undef_skip_backquote(const char *s, const char *end)
{
	size_t i = 1;
	while (s + i < end) {
		if (s[i] == '\\') {
			i += 2;
			continue;
		}
		if (s[i] == '`') {
			return s + i + 1;
		}
		i++;
	}
	return NULL;
}

static const char *undef_skip_brace(const char *s, const char *end,
	int depth, int sq, int dq);
static const char *undef_skip_cmdsubst(const char *s, const char *end,
	int depth, int sq, int dq);
static const char *undef_span(const char *s, const char *end, int depth,
	int sq, int dq, int analyze, int in_brace, int ws0,
	struct undef_asn *asn, int *nasn, int *found);
static const char *undef_brace(const char *s, const char *end, int depth,
	int sq, int dq, int analyze, int edq,
	struct undef_asn *asn, int *nasn, int *found);
static const char *undef_arith(const char *s, const char *end, int depth,
	int analyze, struct undef_asn *asn, int *nasn, int *found);

/* Skip a ${...} unit at s (structural only, no analysis).
 * Returns past the matching '}', NULL on bare braces, unbalanced
 * input or over-depth. Brace words re-tokenize with a fresh quote
 * context (0016 brace_unit precedent: "parsed as their own context");
 * balanced interiors are quote-neutral. Mirrors brace_unit's
 * traversal discipline without its NOCMD verdicts. */
static const char *undef_skip_brace(const char *s, const char *end,
	int depth, int sq, int dq)
{
	size_t i = 2;
	if (depth > BRACE_MAXDEPTH) {
		return NULL;
	}
	/* Entry sq is always 0 ($ is suppressed inside sq); the dq
	 * reset is the effective change. */
	sq = 0;
	dq = 0;
	while (s + i < end) {
		char c = s[i];
		if (c == '\\') {
			i += 2;
			continue;
		}
		if (c == '\'' && !dq) {
			sq ^= 1;
			i++;
			continue;
		}
		if (c == '"' && !sq) {
			dq ^= 1;
			i++;
			continue;
		}
		if (c == '$' && s + i + 1 < end && s[i+1] == '\'' && !sq) {
			size_t al = 0;
			if (ansi_unit(s+i, &al, 0, 0) < 0) {
				return NULL;
			}
			i += al;
			continue;
		}
		if (c == '{' && !sq) {
			return NULL;
		}
		if (c == '$' && s + i + 1 < end && s[i+1] == '{' && !sq) {
			const char *e = undef_skip_brace(s+i, end,
							 depth+1, sq, dq);
			if (!e) {
				return NULL;
			}
			i = e - s;
			continue;
		}
		if (c == '$' && s + i + 1 < end && s[i+1] == '(' && !sq) {
			if (s + i + 2 < end && s[i+2] == '(') {
				const char *e = undef_arith(s+i+1, end,
					depth+1, 0, NULL, NULL, NULL);
				if (!e) {
					return NULL;
				}
				i = 1 + (e - (s+i+1));
			} else {
				const char *e = undef_skip_cmdsubst(s+i, end,
					depth+1, sq, dq);
				if (!e) {
					return NULL;
				}
				i = e - s;
			}
			continue;
		}
		if (c == '`' && !sq) {
			const char *e = undef_skip_backquote(s+i, end);
			if (!e) {
				return NULL;
			}
			i = e - s;
			continue;
		}
		if (c == '}' && !sq) {
			return s + i + 1;
		}
		i++;
	}
	return NULL;
}

/* Skip a $(...) unit at s (structural only).
 * Returns past the matching ')', NULL if unbalanced. Command and
 * arithmetic interiors are skipped atomically (never analysed). */
static const char *undef_skip_cmdsubst(const char *s, const char *end,
	int depth, int sq, int dq)
{
	size_t i = 2;
	int np = 1;
	if (depth > BRACE_MAXDEPTH) {
		return NULL;
	}
	while (s + i < end) {
		char c = s[i];
		if (c == '\\') {
			i += 2;
			continue;
		}
		if (c == '\'' && !dq) {
			sq ^= 1;
			i++;
			continue;
		}
		if (c == '"' && !sq) {
			dq ^= 1;
			i++;
			continue;
		}
		if (!sq && !dq) {
			if (c == '$' && s + i + 1 < end && s[i+1] == '\'') {
				size_t al = 0;
				if (ansi_unit(s+i, &al, 0, 0) < 0) {
					return NULL;
				}
				i += al;
				continue;
			}
			if (c == '$' && s + i + 1 < end && s[i+1] == '{') {
				const char *e = undef_skip_brace(s+i, end,
					depth+1, sq, dq);
				if (!e) {
					return NULL;
				}
				i = e - s;
				continue;
			}
			if (c == '$' && s + i + 1 < end && s[i+1] == '(') {
				if (s + i + 2 < end && s[i+2] == '(') {
					const char *e = undef_arith(s+i+1, end,
						depth+1, 0, NULL, NULL, NULL);
					if (!e) {
						return NULL;
					}
					i = 1 + (e - (s+i+1));
				} else {
					np++;
					i += 2;
				}
				continue;
			}
			if (c == '`') {
				const char *e = undef_skip_backquote(s+i, end);
				if (!e) {
					return NULL;
				}
				i = e - s;
				continue;
			}
			if (c == '(') {
				np++;
				i++;
				continue;
			}
			if (c == ')') {
				if (--np == 0) {
					return s + i + 1;
				}
				i++;
				continue;
			}
		} else if (!sq && c == '`') {
			const char *e = undef_skip_backquote(s+i, end);
			if (!e) {
				return NULL;
			}
			i = e - s;
			continue;
		}
		i++;
	}
	return NULL;
}

/* Scan a word span for unambiguous attempts to expand unset
 * variables. Threads sq/dq quote state (shell tokenization).
 * Returns 0 with *found set on clean scan, -1 to pass through. */
/* Parse a variable name at s (s[0] is [A-Za-z_]). *len set past the
 * name. Names are matched exactly (no locale). */
static int undef_name(const char *s, const char *end, size_t *len)
{
	size_t i = 1;
	while (s + i < end && is_wc(s[i])) {
		i++;
	}
	*len = i;
	return 0;
}

/* Evaluate one variable reference as an expansion attempt:
 * unset (0) sets *found. Specials are filtered by callers.
 * Returns 0 ok, -1 to pass through (overlong name). */
static int undef_varref(const char *name, size_t len,
	struct undef_asn *asn, int nasn, int *found)
{
	int d = undef_isdef(name, len, asn, nasn);
	if (d < 0) {
		return -1;
	}
	if (!d) {
		*found = 1;
	}
	return 0;
}

/* Scan a word span. Stops at end, or at an unquoted (sq-wise) '}'
 * when in_brace is set (returns at the brace for the caller).
 * in_brace is 0 at top level, 1 for a brace word entered from an
 * unquoted context, 2 for a brace word entered from a quoted
 * context (Class-B: only quoted-context words can expose a
 * quote-first "$" pair; unquoted "${X-"$"Y}" stays literal, as
 * the shell does). sq/dq thread the shell tokenization; brace
 * words start from a fresh context (see undef_brace). analyze=0:
 * structural scan only (no flagging, no recording). Returns stop
 * position, NULL on malformation (pass through). */
static const char *undef_span(const char *s, const char *end, int depth,
	int sq, int dq, int analyze, int in_brace, int ws0,
	struct undef_asn *asn, int *nasn, int *found)
{
	int wstart = ws0;
	/* Class-B: set iff the immediately preceding consumed char is
	 * an unescaped double-quote that opened dq (toggle 0->1).
	 * Tracked by direction, not lookbehind, so an escaped \"
	 * never counts. Gated with in_brace==2 below. */
	int pqo = 0;
	if (depth > BRACE_MAXDEPTH) {
		return NULL;
	}
	while (s < end) {
		char c = *s;
		if (c == '\\') {
			if (s + 1 >= end) {
				return NULL;
			}
			s += 2;
			wstart = 0;
			pqo = 0;
			continue;
		}
		if (c == '\'' && !dq) {
			sq ^= 1;
			s++;
			wstart = 0;
			pqo = 0;
			continue;
		}
		if (c == '"' && !sq) {
			if (!dq) {
				dq = 1;
				pqo = 1;
			} else {
				dq = 0;
				pqo = 0;
			}
			s++;
			wstart = 0;
			continue;
		}
		if (!sq && !dq && (c == ' ' || c == '\t' || c == '\n' ||
				   c == ';' || c == '&' || c == '|' ||
				   c == '(' || c == ')')) {
			s++;
			wstart = 1;
			pqo = 0;
			continue;
		}
		/* Class-B: a # starts a comment only when unquoted
		 * in the outer sense. Word spans entered from a
		 * quoted context (in_brace==2) are inside the
		 * outer double quotes the whole time, where the
		 * shell never starts a comment, so # stays
		 * literal there. (Unquoted-context words keep the
		 * legacy rule.) */
		if (!sq && !dq && c == '#' && in_brace != 2) {
			if (!wstart) {
				s++;
				pqo = 0;
				continue;
			}
			while (s < end && *s != '\n') {
				s++;
			}
			wstart = 1;
			pqo = 0;
			continue;
		}
		wstart = 0;
		if (c == '}' && !sq && in_brace) {
			return s;
		}
		if (c == '$' && !sq) {
			size_t nl = 0;
			if (s + 1 >= end) {
				s++;
				pqo = 0;
				continue;
			}
			if (s[1] == '\'') {
				if (ansi_unit(s, &nl, 0, 0) < 0) {
					return NULL;
				}
				s += nl;
				pqo = 0;
				continue;
			}
			/* Class-B: quote-first "$" vs dollar-first $".
			 * A quote-first pair (the $ directly follows a
			 * dq-opening quote, inside a quoted-context brace
			 * word) is transparent: the shell expands through
			 * it ("${X-"$"Y}" behaves as "${X-"$Y"}"), so the
			 * following token is an expansion position. A
			 * dollar-first $" opens a locale string instead
			 * (opaque, but its content still expands).
			 * In-dq $" outside braces keeps the legacy
			 * literal-$ reading ("a$"b" is literal). */
			if (s[1] == '"') {
				if (in_brace == 2 && pqo) {
					const char *q = s + 2;
					s += 2;
					pqo = 0;
					if (q >= end) {
						return NULL;
					}
					if (*q == '_' ||
					    (*q >= 'a' && *q <= 'z') ||
					    (*q >= 'A' && *q <= 'Z')) {
						size_t vl = 0;
						undef_name(q, end, &vl);
						if (analyze) {
							int dd = undef_isdef(q, vl, asn, *nasn);
							if (dd < 0) {
								return NULL;
							}
							if (!dd) {
								/* Class-B: flag the exposed
								 * name only if the rest of
								 * this word is quote-clean.
								 * A later quote/backslash
								 * means the shell re-parses
								 * (often rejecting) the word
								 * as a whole -- e.g. the
								 * extra-quote shape the shell
								 * itself reports as a syntax
								 * error -- so pass through and
								 * let the shell decide. Nested
								 * ${...} is skipped atomically;
								 * anything unbalanced passes
								 * through. False negatives are
								 * allowed; false positives are
								 * not. */
								const char *r = q + vl;
								while (r < end) {
									if (*r == '"' ||
									    *r == '\'' ||
									    *r == '\\') {
										return NULL;
									}
									if (*r == '$' &&
									    r + 1 < end &&
									    r[1] == '{') {
										const char *e = undef_skip_brace(r, end,
											depth + 1, 0, 0);
										if (!e) {
											return NULL;
										}
										r = e;
										continue;
									}
									if (*r == '{') {
										return NULL;
									}
									if (*r == '}') {
										break;
									}
									r++;
								}
								if (r >= end) {
									return NULL;
								}
								*found = 1;
							}
						}
						s = q + vl;
						continue;
					}
					if (*q == '{') {
						const char *e = undef_brace(q - 1, end,
							depth + 1, sq, dq, analyze, dq,
							asn, nasn, found);
						if (!e) {
							return NULL;
						}
						s = e;
						continue;
					}
					if (*q == '(') {
						const char *e;
						if (q + 1 < end && q[1] == '(') {
							e = undef_arith(q - 1, end,
								depth + 1, analyze,
								asn, nasn, found);
						} else {
							e = undef_skip_cmdsubst(q - 1, end,
								depth + 1, sq, dq);
						}
						if (!e) {
							return NULL;
						}
						s = e;
						continue;
					}
					/* Anything else: the pair was
					 * transparent; resume after it. */
					continue;
				}
				if (!dq) {
					const char *q = s + 2;
					const char *ce = NULL;
					while (q < end) {
						if (*q == '\\') {
							q += 2;
							continue;
						}
						if (*q == '"') {
							ce = q;
							break;
						}
						q++;
					}
					if (!ce) {
						return NULL;
					}
					if (analyze) {
						const char *e = undef_span(s + 2, ce, depth,
							0, 0, analyze, 0, 0,
							asn, nasn, found);
						if (!e) {
							return NULL;
						}
					}
					s = ce + 1;
					pqo = 0;
					continue;
				}
				/* else: in-dq non-pair $": legacy path below */
			}
			if (s[1] == '{') {
				const char *e;
				if (!analyze) {
					e = undef_skip_brace(s, end, depth+1,
							     sq, dq);
					if (!e) {
						return NULL;
					}
					s = e;
					pqo = 0;
					continue;
				}
				e = undef_brace(s, end, depth+1, sq, dq,
						analyze, dq, asn, nasn, found);
				if (!e) {
					return NULL;
				}
				s = e;
				pqo = 0;
				continue;
			}
			if (s[1] == '(') {
				const char *e;
				if (s + 2 < end && s[2] == '(') {
					e = undef_arith(s, end, depth+1,
							analyze, asn, nasn, found);
				} else {
					e = undef_skip_cmdsubst(s, end, depth+1,
								sq, dq);
				}
				if (!e) {
					return NULL;
				}
				s = e;
				pqo = 0;
				continue;
			}
			if (s[1] == '*' || s[1] == '@' || s[1] == '#' ||
			    s[1] == '?' || s[1] == '-' || s[1] == '$' ||
			    s[1] == '!' || (s[1] >= '0' && s[1] <= '9')) {
				s += 2;
				pqo = 0;
				continue;
			}
			if (s[1] == '_' || (s[1] >= 'a' && s[1] <= 'z') ||
			    (s[1] >= 'A' && s[1] <= 'Z')) {
				size_t vl = 0;
				undef_name(s+1, end, &vl);
				if (analyze && undef_varref(s+1, vl, asn, *nasn, found)) {
					return NULL;
				}
				s += 1 + vl;
				pqo = 0;
				continue;
			}
			s++;
			pqo = 0;
			continue;
		}
		if (c == '`' && !sq) {
			const char *e = undef_skip_backquote(s, end);
			if (!e) {
				return NULL;
			}
			s = e;
			pqo = 0;
			continue;
		}
		if (c == '{' && !sq) {
			return NULL;
		}
		s++;
		pqo = 0;
	}
	if (sq || dq) {
		return NULL;
	}
	return s;
}

/* Parse one ${...} unit at s (s[0]=='$', s[1]=='{').
 * Returns past the closing '}', NULL on malformation (pass through).
 * Operator semantics per shell left-to-right with lazy branches;
 * only evaluated branches are descended into. edq records whether
 * the brace was entered from a quoted context (Class-B); the word
 * itself is always scanned with a fresh quote context. */
static const char *undef_brace(const char *s, const char *end, int depth,
	int sq, int dq, int analyze, int edq,
	struct undef_asn *asn, int *nasn, int *found)
{
	const char *p = s + 2;
	const char *name = NULL;
	size_t nlen = 0;
	int lenop = 0, special = 0, d, col = 0;
	const char *rest;
	/* Quoted-context words scan with in_brace=2 (quote-first "$"
	 * pairs expose the following token); unquoted-context words
	 * scan with in_brace=1 (legacy literal-$ reading). */
	int inb = edq ? 2 : 1;
	if (depth > BRACE_MAXDEPTH) {
		return NULL;
	}
	/* Fresh quote context for the word (see undef_span). Entry
	 * sq is always 0 ($ is suppressed inside sq). */
	sq = 0;
	dq = 0;
	if (p < end && *p == '#') {
		lenop = 1;
		p++;
	}
	if (p < end && (*p == '_' || (*p >= 'a' && *p <= 'z') ||
	    (*p >= 'A' && *p <= 'Z'))) {
		name = p;
		p++;
		while (p < end && is_wc(*p)) {
			p++;
		}
		nlen = p - name;
	} else if (p < end && (*p == '*' || *p == '@' || *p == '#' ||
	    *p == '?' || *p == '-' || *p == '$' || *p == '!' ||
	    (*p >= '0' && *p <= '9'))) {
		special = 1;
		p++;
	} else {
		return NULL;
	}
	if (!analyze) {
		const char *e = undef_skip_brace(s, end, depth, sq, dq);
		return e;
	}
	if (special) {
		if (p >= end) {
			return NULL;
		}
		if (*p == '}') {
			return p + 1;
		}
		return NULL;
	}
	if (lenop) {
		if (p < end && *p == '}') {
			d = undef_isdef(name, nlen, asn, *nasn);
			if (d < 0) {
				return NULL;
			}
			if (!d) {
				*found = 1;
			}
			return p + 1;
		}
		return NULL;
	}
	if (p >= end) {
		return NULL;
	}
	if (*p == '}') {
		d = undef_isdef(name, nlen, asn, *nasn);
		if (d < 0) {
			return NULL;
		}
		if (!d) {
			*found = 1;
		}
		return p + 1;
	}
	if (*p == ':') {
		col = 1;
		p++;
		if (p >= end) {
			return NULL;
		}
	}
	if (*p == '-' || *p == '=' || *p == '?' || *p == '+') {
		char op = *p;
		int take;
		rest = p + 1;
		d = undef_isdef(name, nlen, asn, *nasn);
		if (d < 0) {
			return NULL;
		}
		if (op == '?') {
			const char *e;
			if (!d || (col && d == 1)) {
				*found = 1;
			}
			e = undef_span(rest, end, depth, sq, dq, 0, inb, 0,
				       asn, nasn, found);
			if (!e || e >= end || *e != '}') {
				return NULL;
			}
			return e + 1;
		}
		if (op == '+') {
			take = d && (!col || d == 2);
		} else {
			take = !d || (col && d == 1);
		}
		if (op == '=' && take) {
			if (undef_assign(name, nlen, asn, nasn)) {
				return NULL;
			}
		}
		if (take) {
			const char *e = undef_span(rest, end, depth, sq, dq,
						   1, inb, 0, asn, nasn, found);
			if (!e || e >= end || *e != '}') {
				return NULL;
			}
			return e + 1;
		}
		{
			const char *e = undef_span(rest, end, depth, sq, dq,
						   0, inb, 0, asn, nasn, found);
			if (!e || e >= end || *e != '}') {
				return NULL;
			}
			return e + 1;
		}
	}
	if (*p == '#' || *p == '%') {
		char op = *p;
		const char *e;
		if (p + 1 < end && p[1] == op) {
			p++;
		}
		d = undef_isdef(name, nlen, asn, *nasn);
		if (d < 0) {
			return NULL;
		}
		if (!d) {
			*found = 1;
		}
		e = undef_span(p + 1, end, depth, sq, dq, 1, inb, 0,
			       asn, nasn, found);
		if (!e || e >= end || *e != '}') {
			return NULL;
		}
		return e + 1;
	}
	if (*p == '/') {
		const char *e;
		d = undef_isdef(name, nlen, asn, *nasn);
		if (d < 0) {
			return NULL;
		}
		if (!d) {
			*found = 1;
		}
		e = undef_span(p + 1, end, depth, sq, dq, 1, inb, 0,
			       asn, nasn, found);
		if (!e || e >= end || *e != '}') {
			return NULL;
		}
		return e + 1;
	}
	if (*p == '@') {
		d = undef_isdef(name, nlen, asn, *nasn);
		if (d < 0) {
			return NULL;
		}
		if (!d) {
			*found = 1;
		}
		if (p + 2 >= end || p[2] != '}') {
			return NULL;
		}
		switch (p[1]) {
		case 'Q':
		case 'E':
		case 'P':
		case 'A':
		case 'K':
		case 'a':
			break;
		default:
			return NULL;
		}
		return p + 3;
	}
	return NULL;
}

/* Parse one $((...)) unit at s (s[0]=='$', s[1]=='(', s[2]=='(').
 * Returns past the final ')', NULL on malformation (pass through).
 * Variables are expansion attempts; lazy operators (?: && ||),
 * quoted operands and stray syntax give up instead of guessing. */
static const char *undef_arith(const char *s, const char *end, int depth,
	int analyze,
	struct undef_asn *asn, int *nasn, int *found)
{
	const char *p = s + 3;
	int np = 0, bp = 0;
	if (depth > BRACE_MAXDEPTH) {
		return NULL;
	}
	if (!analyze) {
		int sq = 0, dq = 0;
		while (p < end) {
			char c = *p;
			if (c == '\\') {
				if (p + 1 >= end || p[1] != '\n') {
					return NULL;
				}
				p += 2;
				continue;
			}
			if (c == '\'' || c == '"') {
				return NULL;
			}
			if (c == '`') {
				const char *e = undef_skip_backquote(p, end);
				if (!e) {
					return NULL;
				}
				p = e;
				continue;
			}
			if (c == '$' && p + 1 < end && p[1] == '(') {
				const char *e;
				if (p + 2 < end && p[2] == '(') {
					e = undef_arith(p+1, end, depth+1,
							0, NULL, NULL, NULL);
				} else {
					e = undef_skip_cmdsubst(p, end, depth+1,
								sq, dq);
				}
				if (!e) {
					return NULL;
				}
				p = e;
				continue;
			}
			if (c == '$' && p + 1 < end && p[1] == '{') {
				const char *e = undef_skip_brace(p, end,
					depth+1, sq, dq);
				if (!e) {
					return NULL;
				}
				p = e;
				continue;
			}
			if (c == '(') {
				np++;
				p++;
				continue;
			}
			if (c == '[') {
				bp++;
				p++;
				continue;
			}
			if (c == ')') {
				if (np > 0) {
					np--;
					p++;
					continue;
				}
				if (p + 1 >= end || p[1] != ')') {
					return NULL;
				}
				return p + 2;
			}
			if (c == ']') {
				if (!bp) {
					return NULL;
				}
				bp--;
				p++;
				continue;
			}
			p++;
		}
		return NULL;
	}
	while (p < end) {
		char c = *p;
		if (c == ' ' || c == '\t' || c == '\n') {
			p++;
			continue;
		}
		if (c == '\\') {
			if (p + 1 >= end || p[1] != '\n') {
				return NULL;
			}
			p += 2;
			continue;
		}
		if (c == '\'' || c == '"') {
			return NULL;
		}
		if (c == '`') {
			const char *e = undef_skip_backquote(p, end);
			if (!e) {
				return NULL;
			}
			p = e;
			continue;
		}
		if (c == '?') {
			return NULL;
		}
		if (c == ';') {
			return NULL;
		}
		if ((c == '&' || c == '|') && p + 1 < end && p[1] == c) {
			return NULL;
		}
		if (c == ':') {
			return NULL;
		}
		if (c == '(') {
			np++;
			p++;
			continue;
		}
		if (c == '[') {
			bp++;
			p++;
			continue;
		}
		if (c == ')') {
			if (np > 0) {
				np--;
				p++;
				continue;
			}
			if (p + 1 >= end || p[1] != ')') {
				return NULL;
			}
			return p + 2;
		}
		if (c == ']') {
			if (!bp) {
				return NULL;
			}
			bp--;
			p++;
			continue;
		}
		if (c == '$') {
			size_t nl = 0;
			if (p + 1 >= end) {
				return NULL;
			}
			if (p[1] == '\'') {
				if (ansi_unit(p, &nl, 0, 0) < 0) {
					return NULL;
				}
				p += nl;
				continue;
			}
			if (p[1] == '{') {
				const char *e = undef_brace(p, end, depth+1,
					0, 0, analyze, 0, asn, nasn, found);
				if (!e) {
					return NULL;
				}
				p = e;
				continue;
			}
			if (p[1] == '(') {
				const char *e;
				if (p + 2 < end && p[2] == '(') {
					e = undef_arith(p+1, end, depth+1,
							analyze, asn, nasn, found);
				} else {
					e = undef_skip_cmdsubst(p, end, depth+1,
								0, 0);
				}
				if (!e) {
					return NULL;
				}
				p = e;
				continue;
			}
			if (p[1] == '*' || p[1] == '@' || p[1] == '#' ||
			    p[1] == '?' || p[1] == '-' || p[1] == '$' ||
			    p[1] == '!' || (p[1] >= '0' && p[1] <= '9')) {
				p += 2;
				continue;
			}
			if (p[1] == '_' || (p[1] >= 'a' && p[1] <= 'z') ||
			    (p[1] >= 'A' && p[1] <= 'Z')) {
				size_t vl = 0;
				const char *q;
				undef_name(p+1, end, &vl);
				q = p + 1 + vl;
				while (q < end && (*q == ' ' || *q == '\t' ||
				       *q == '\n')) {
					q++;
				}
				if (q < end && *q == '=' &&
				    (q + 1 >= end || q[1] != '=')) {
					if (undef_assign(p+1, vl, asn, nasn)) {
						return NULL;
					}
					p = q + 1;
					continue;
				}
				if (undef_varref(p+1, vl, asn, *nasn, found)) {
					return NULL;
				}
				p += 1 + vl;
				continue;
			}
			return NULL;
		}
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    c == '_' || (c >= '0' && c <= '9')) {
			size_t vl = 0;
			const char *q;
			/* Class-B: a digit-led token is a numeric
			 * constant, never a variable (plain 1, hex
			 * 0x1F, base notation 8#ff all expand without
			 * consulting the environment, and set -u stays
			 * silent for them). Skip without flagging and
			 * without recording an assignment. */
			if (c >= '0' && c <= '9') {
				undef_name(p, end, &vl);
				p += vl;
				if (p < end && *p == '#') {
					p++;
					while (p < end &&
					       (is_wc(*p) || *p == '@')) {
						p++;
					}
				}
				continue;
			}
			undef_name(p, end, &vl);
			q = p + vl;
			while (q < end && (*q == ' ' || *q == '\t' ||
			       *q == '\n')) {
				q++;
			}
			if (q < end && *q == '=' &&
			    (q + 1 >= end || q[1] != '=')) {
				if (undef_assign(p, vl, asn, nasn)) {
					return NULL;
				}
				p = q + 1;
				continue;
			}
			if (undef_varref(p, vl, asn, *nasn, found)) {
				return NULL;
			}
			p = q;
			continue;
		}
		p++;
	}
	return NULL;
}

/* Top-level check: 1 if s unambiguously attempts to expand an
 * unset variable, 0 otherwise (clean or pass-through). */
static int undef_ref(const char *s)
{
	struct undef_asn asn[UNDEF_MAXASSIGN];
	int nasn = 0, found = 0;
	size_t n = strlen(s);
	const char *e = undef_span(s, s + n, 0, 0, 0, 1, 0, 1,
				   asn, &nasn, &found);
	if (!e) {
		return 0;
	}
	return found ? 1 : 0;
}

static int do_wordexp(const char *s, wordexp_t *we, int flags)
{
	size_t i, l;
	int sq=0, dq=0;
	char *w, **tmp;
	int err = 0;
	int null = -1;
	FILE *f;
	size_t wc = 0;
	char **wv = 0;
	int p[2];
	pid_t pid;
	sigset_t set;

	if (flags & WRDE_REUSE) wordfree(we);

	if (flags & WRDE_NOCMD) for (i=0; s[i]; i++) switch (s[i]) {
	case '\\':
		if (!sq && !s[++i]) return WRDE_SYNTAX;
		break;
	case '\'':
		if (!dq) sq^=1;
		break;
	case '"':
		if (!sq) dq^=1;
		break;
	case '(':
	case ')':
	case '\n':
	case '|':
	case '&':
	case ';':
	case '<':
	case '>':
	case '{':
	case '}':
		if (!(sq|dq)) return WRDE_BADCHAR;
		break;
	case '#':
		/* A # starting a word begins a comment; skip to the
		 * end of the line. A newline in command position
		 * still separates commands. A comment running to end
		 * of input simply ends the scan here; execution
		 * proceeds normally (never return success early --
		 * that would skip the shell and leave stale output). */
		if (!sq && !dq &&
		    (i == 0 || strchr(" \t\n;&|()<>", s[i-1]))) {
			while (s[i+1] && s[i+1] != '\n') i++;
			break;
		}
		break;
	case '$':
		if (sq) break;
		if (s[i+1] == '\'') {
			/* ANSI-C $'...' quote: consume literally (escapes
			 * honoured). An embedded $(...) or backquote blocks
			 * only where the span is not genuine ANSI (inside
			 * double quotes; inside arithmetic it always does):
			 * at top level the content is literal. */
			size_t al = 0;
			int a = ansi_unit(s+i, &al, 0, dq);
			if (a < 0 || a == 2) return WRDE_SYNTAX;
			if (a) return WRDE_CMDSUB;
			i += al-1;
			break;
		}
		if (s[i+1] == '{') {
			/* ${...} is parsed as its own word: braces must
			 * balance and no command substitution may hide
			 * inside (single-quotes suppress outside
			 * arithmetic). The word is scanned in the current
			 * (outer) quote context: sq is always 0 here ($ is
			 * suppressed inside sq), dq is passed through.
			 * Unbalanced or bare-brace input keeps the
			 * historical verdicts below. */
			size_t bl = 0;
			int b = brace_unit(s+i, &bl, 0, 0, dq);
			if (b == 1) return WRDE_CMDSUB;
			if (b == 2 || b == -2) return WRDE_SYNTAX;
			if (b == 0) {
				i += bl-1;
				break;
			}
			break;
		}
		if (s[i+1] == '(' && s[i+2] == '(') {
			/* Arithmetic is allowed, but only when it really
			 * is arithmetic: validate the token stream.
			 * Anything else is treated as the command
			 * substitution the shell would parse it as. */
			size_t al = 0;
			int ar = arith_ok(s+i+1, &al);
			if (ar == 1) return WRDE_CMDSUB;
			if (ar == 2) return WRDE_SYNTAX;
			i += al;
			break;
		} else if (s[i+1] != '(') break;
		/* A single "$(" is command substitution, handled by the
		 * shared backtick path below. */
		/* fall through */
	case '`':
		if (sq) break;
		return WRDE_CMDSUB;
	}

	/* WRDE_UNDEF, checked without executing anything: report an
	 * error on an unambiguous attempt to expand an unset shell
	 * variable. Runs after the NOCMD scan (which keeps precedence)
	 * and before any fork, so a WRDE_BADVAL return performs zero
	 * command side effects by construction. Pass-through (return 0
	 * here) preserves current behaviour exactly. */
	if ((flags & WRDE_UNDEF) && undef_ref(s))
		return WRDE_BADVAL;

	if (flags & WRDE_APPEND) {
		wc = we->we_wordc;
		wv = we->we_wordv;
	}

	i = wc;
	if (flags & WRDE_DOOFFS) {
		if (we->we_offs > SIZE_MAX/sizeof(void *)/4)
			goto nospace;
		i += we->we_offs;
	} else {
		we->we_offs = 0;
	}

	if (pipe2(p, O_CLOEXEC) < 0) goto nospace;
	if (!(flags & WRDE_SHOWERR)) {
		null = open("/dev/null", O_WRONLY|O_CLOEXEC);
		if (null < 0) goto nospace;
	}
	__block_all_sigs(&set);
	pid = fork();
	__restore_sigs(&set);
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		close(null);
		goto nospace;
	}
	if (!pid) {
		if (p[1] == 1) fcntl(1, F_SETFD, 0);
		else dup2(p[1], 1);
		if (null == 2) fcntl(2, F_SETFD, 0);
		else dup2(null, 2);
		execl("/bin/sh", "sh", "-c",
			"eval \"set --;printf %s\\\\\\\\0 x $1\"",
			"sh", s, (char *)0);
		_exit(1);
	}
	close(p[1]);
	close(null);
	
	f = fdopen(p[0], "r");
	if (!f) {
		close(p[0]);
		kill(pid, SIGKILL);
		reap(pid);
		goto nospace;
	}

	l = wv ? i+1 : 0;

	free(getword(f));
	if (feof(f)) {
		fclose(f);
		reap(pid);
		return WRDE_SYNTAX;
	}

	while ((w = getword(f))) {
		if (i+1 >= l) {
			l += l/2+10;
			tmp = realloc(wv, l*sizeof(char *));
			if (!tmp) {
				free(w);
				break;
			}
			wv = tmp;
		}
		wv[i++] = w;
		wv[i] = 0;
	}
	if (!feof(f)) err = WRDE_NOSPACE;

	fclose(f);
	reap(pid);

	if (!wv) wv = calloc(i+1, sizeof *wv);

	we->we_wordv = wv;
	we->we_wordc = i;

	if (flags & WRDE_DOOFFS) {
		if (wv) for (i=we->we_offs; i; i--)
			we->we_wordv[i-1] = 0;
		we->we_wordc -= we->we_offs;
	}
	return err;

nospace:
	if (!(flags & WRDE_APPEND)) {
		we->we_wordc = 0;
		we->we_wordv = 0;
	}
	return WRDE_NOSPACE;
}

int wordexp(const char *restrict s, wordexp_t *restrict we, int flags)
{
	int r, cs;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	r = do_wordexp(s, we, flags);
	pthread_setcancelstate(cs, 0);
	return r;
}

void wordfree(wordexp_t *we)
{
	size_t i;
	if (!we->we_wordv) return;
	for (i=0; i<we->we_wordc; i++) free(we->we_wordv[we->we_offs+i]);
	free(we->we_wordv);
	we->we_wordv = 0;
	we->we_wordc = 0;
}
