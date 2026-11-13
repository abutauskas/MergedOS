/* SPDX-License-Identifier: BSD-2-Clause */
/* Mergedfence rule parser and matcher. Input lists are untrusted text. */
#include "mergedfence/fence.h"

void fence_init(struct fence *f)
{
	memset(f, 0, sizeof(*f));
	f->enabled = true;
}

void fence_enable(struct fence *f, bool on)
{
	f->enabled = on;
}

const struct fence_stats *fence_stats(const struct fence *f)
{
	return &f->st;
}

const char *fence_verdict_name(enum fence_verdict v)
{
	switch (v) {
	case FENCE_BLOCKED:  return "blocked";
	case FENCE_EXCEPTED: return "allowed (exception)";
	default:             return "allowed";
	}
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

/*
 * Normalise a host name into `out` (lower case). Labels are 1..63 bytes of
 * [a-z0-9-_], no empty labels, no leading/trailing dot.
 */
static bool domain_norm(const char *s, size_t n, char out[FENCE_DOMAIN_MAX])
{
	size_t label = 0;

	if (n && s[n - 1] == '.')
		n--;                            /* fully qualified form */
	if (!n || n >= FENCE_DOMAIN_MAX)
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = (char)mos_tolower((u8)s[i]);

		if (c == '.') {
			if (!label)
				return false;
			label = 0;
		} else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
			   c == '_') {
			label++;
		} else {
			return false;
		}
		out[i] = c;
	}
	if (!label)
		return false;
	out[n] = '\0';
	return true;
}

/* 1 added, 0 duplicate, -MERR_RANGE table full. */
static int add_rule(struct fence *f, const char *dom, bool exception)
{
	for (u32 i = 0; i < f->nrules; i++)
		if (f->rules[i].exception == exception && !strcmp(f->rules[i].domain, dom))
			return 0;
	if (f->nrules >= FENCE_MAX_RULES) {
		f->st.dropped++;
		return -MERR_RANGE;
	}
	mos_strlcpy(f->rules[f->nrules].domain, dom, FENCE_DOMAIN_MAX);
	f->rules[f->nrules].exception = exception;
	f->nrules++;
	f->st.rules++;
	if (exception)
		f->st.exceptions++;
	return 1;
}

/* Names hosts files map to themselves; they are not block rules. */
static bool hosts_builtin(const char *d)
{
	static const char *const names[] = {
		"localhost", "localhost.localdomain", "local", "broadcasthost", "ip6-localhost",
		"ip6-loopback",
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(d, names[i]))
			return true;
	return false;
}

/* "0.0.0.0 a.example b.example # note" -> one rule per name. */
static int parse_hosts(struct fence *f, const char *l, size_t n, size_t ip_len, int *added)
{
	char dom[FENCE_DOMAIN_MAX];
	size_t i = ip_len;
	bool any = false;
	int rc;

	while (i < n) {
		size_t start;

		while (i < n && is_space(l[i]))
			i++;
		if (i == n || l[i] == '#')
			break;
		start = i;
		while (i < n && !is_space(l[i]) && l[i] != '#')
			i++;
		if (!domain_norm(l + start, i - start, dom) || hosts_builtin(dom))
			continue;
		rc = add_rule(f, dom, false);
		if (rc < 0)
			return rc;
		*added += rc;
		any = true;
	}
	if (!any)
		f->st.ignored++;
	return 0;
}

static int parse_line(struct fence *f, const char *l, size_t n, int *added)
{
	char dom[FENCE_DOMAIN_MAX];
	bool exception = false;
	size_t end;
	int rc;

	while (n && is_space(*l))
		l++, n--;
	while (n && is_space(l[n - 1]))
		n--;
	if (!n || l[0] == '#' || l[0] == '!')
		return 0;

	if (n > 8 && !memcmp(l, "0.0.0.0", 7) && is_space(l[7]))
		return parse_hosts(f, l, n, 7, added);
	if (n > 10 && !memcmp(l, "127.0.0.1", 9) && is_space(l[9]))
		return parse_hosts(f, l, n, 9, added);

	if (n > 2 && l[0] == '@' && l[1] == '@') {
		exception = true;
		l += 2, n -= 2;
	}
	/* Only the plain "||domain^" form: anything with options or paths is skipped. */
	if (n < 4 || l[0] != '|' || l[1] != '|' || l[n - 1] != '^') {
		f->st.ignored++;
		return 0;
	}
	end = n - 1;
	if (!domain_norm(l + 2, end - 2, dom)) {
		f->st.ignored++;
		return 0;
	}
	rc = add_rule(f, dom, exception);
	if (rc < 0)
		return rc;
	*added += rc;
	return 0;
}

int fence_load(struct fence *f, const char *buf, size_t len)
{
	bool full = false;
	int added = 0;
	size_t i = 0;

	if (!buf)
		return -MERR_INVAL;
	while (i < len) {
		size_t start = i;

		while (i < len && buf[i] != '\n' && buf[i] != '\0')
			i++;
		if (parse_line(f, buf + start, i - start, &added) < 0)
			full = true;
		i++;
	}
	return full ? -MERR_RANGE : added;
}

/* Does `rule` equal `q` or a parent domain of it (label boundary)? */
static bool covers(const char *rule, const char *q, size_t qlen)
{
	size_t rlen = strlen(rule);
	const char *tail;

	if (rlen > qlen)
		return false;
	tail = q + (qlen - rlen);
	if (rlen < qlen && tail[-1] != '.')
		return false;
	for (size_t i = 0; i < rlen; i++)
		if (mos_tolower((u8)tail[i]) != rule[i])
			return false;
	return true;
}

enum fence_verdict fence_check(struct fence *f, const char *domain)
{
	bool blocked = false, excepted = false;
	size_t qlen;

	f->st.checks++;
	if (!f->enabled || !domain)
		return FENCE_ALLOWED;
	qlen = strlen(domain);
	if (qlen && domain[qlen - 1] == '.')
		qlen--;
	if (!qlen || qlen > 253)
		return FENCE_ALLOWED;

	for (u32 i = 0; i < f->nrules; i++) {
		if (!covers(f->rules[i].domain, domain, qlen))
			continue;
		if (f->rules[i].exception)
			excepted = true;
		else
			blocked = true;
	}
	if (blocked && excepted) {
		f->st.excepted++;
		return FENCE_EXCEPTED;
	}
	if (blocked) {
		f->st.blocked++;
		return FENCE_BLOCKED;
	}
	return FENCE_ALLOWED;
}
