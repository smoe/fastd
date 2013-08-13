/*
  Copyright (c) 2012-2013, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "lex.h"

#include <stdlib.h>


struct fastd_lex {
	FILE *file;

	bool needspace;

	size_t start;
	size_t end;
	size_t tok_len;
	char buffer[1024];
};


typedef struct keyword {
	const char *keyword;
	int token;
} keyword_t;

/* the keyword list must be sorted */
static const keyword_t keywords[] = {
	{ "addresses", TOK_ADDRESSES },
	{ "any", TOK_ANY },
	{ "as", TOK_AS },
	{ "auto", TOK_AUTO },
	{ "bind", TOK_BIND },
	{ "capabilities", TOK_CAPABILITIES },
	{ "crypto", TOK_CRYPTO },
	{ "debug", TOK_DEBUG },
	{ "default", TOK_DEFAULT },
	{ "disestablish", TOK_DISESTABLISH },
	{ "down", TOK_DOWN },
	{ "drop", TOK_DROP },
	{ "early", TOK_EARLY },
	{ "error", TOK_ERROR },
	{ "establish", TOK_ESTABLISH },
	{ "fatal", TOK_FATAL },
	{ "float", TOK_FLOAT },
	{ "forward", TOK_FORWARD },
	{ "from", TOK_FROM },
	{ "group", TOK_GROUP },
	{ "hide", TOK_HIDE },
	{ "include", TOK_INCLUDE },
	{ "info", TOK_INFO },
	{ "interface", TOK_INTERFACE },
	{ "ip", TOK_IP },
	{ "ipv4", TOK_IPV4 },
	{ "ipv6", TOK_IPV6 },
	{ "key", TOK_KEY },
	{ "level", TOK_LEVEL },
	{ "limit", TOK_LIMIT },
	{ "log", TOK_LOG },
	{ "mac", TOK_MAC },
	{ "method", TOK_METHOD },
	{ "mode", TOK_MODE },
	{ "mtu", TOK_MTU },
	{ "no", TOK_NO },
	{ "on", TOK_ON },
	{ "peer", TOK_PEER },
	{ "peers", TOK_PEERS },
	{ "pmtu", TOK_PMTU },
	{ "port", TOK_PORT },
	{ "post-down", TOK_POST_DOWN },
	{ "pre-up", TOK_PRE_UP },
	{ "protocol", TOK_PROTOCOL },
	{ "remote", TOK_REMOTE },
	{ "secret", TOK_SECRET },
	{ "stderr", TOK_STDERR },
	{ "syslog", TOK_SYSLOG },
	{ "tap", TOK_TAP },
	{ "to", TOK_TO },
	{ "tun", TOK_TUN },
	{ "up", TOK_UP },
	{ "use", TOK_USE },
	{ "user", TOK_USER },
	{ "verbose", TOK_VERBOSE },
	{ "verify", TOK_VERIFY },
	{ "warn", TOK_WARN },
	{ "yes", TOK_YES },
};

static int compare_keywords(const void *v1, const void *v2) {
	const keyword_t *k1 = v1, *k2 = v2;
	return strcmp(k1->keyword, k2->keyword);
}


static bool advance(fastd_lex_t *lex) {
	if (lex->start > 0) {
		memmove(lex->buffer, lex->buffer+lex->start, lex->end - lex->start);
		lex->end -= lex->start;
		lex->start = 0;
	}

	if (lex->end == sizeof(lex->buffer))
		return false;

	size_t l = fread(lex->buffer+lex->end, 1, sizeof(lex->buffer) - lex->end, lex->file);

	lex->end += l;
	return l;
}

static inline char current(fastd_lex_t *lex) {
	return lex->buffer[lex->start + lex->tok_len];
}

static char* get_token(fastd_lex_t *lex) {
	return strndup(lex->buffer+lex->start, lex->tok_len);
}

static bool next(YYLTYPE *yylloc, fastd_lex_t *lex, bool move) {
	if (lex->start + lex->tok_len >= lex->end)
		return false;

	if (current(lex) == '\n') {
		yylloc->last_column = 0;
		yylloc->last_line++;
	}
	else {
		yylloc->last_column++;
	}

	if (move)
		lex->start++;
	else
		lex->tok_len++;


	if (lex->start + lex->tok_len >= lex->end)
		return advance(lex);

	return true;
}

static void consume(fastd_lex_t *lex, bool needspace) {
	lex->start += lex->tok_len;
	lex->tok_len = 0;

	lex->needspace = needspace;
}

static int syntax_error(YYSTYPE *yylval, fastd_lex_t *lex) {
	yylval->error = "syntax error";
	return -1;
}

static int io_error(YYSTYPE *yylval, fastd_lex_t *lex) {
	yylval->error = "I/O error";
	return -1;
}

static inline int end(YYSTYPE *yylval, fastd_lex_t *lex) {
	if (ferror(lex->file))
		return io_error(yylval, lex);

	return 0;
}

static int consume_comment(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	char prev = 0;

	while (next(yylloc, lex, true)) {
		if (prev == '*' && current(lex) == '/') {
			next(yylloc, lex, true);
			consume(lex, false);
			return 0;
		}

		prev = current(lex);
	}

	if (ferror(lex->file))
		return io_error(yylval, lex);

	yylval->error = "unterminated block comment";
	return -1;
}

static int unterminated_string(YYSTYPE *yylval, fastd_lex_t *lex) {
	if (ferror(lex->file))
		return io_error(yylval, lex);

	yylval->error = "unterminated string";
	return -1;
}

static int parse_string(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	char *buf = NULL;
	size_t len = 1024;
	size_t pos = 0;

	if (lex->needspace)
		return syntax_error(yylval, lex);

	buf = malloc(len);

	while (true) {
		if (!next(yylloc, lex, true)) {
			free(buf);
			return unterminated_string(yylval, lex);
		}

		char cur = current(lex);

		if (cur == '"')
			break;

		if (cur == '\\') {
			if (!next(yylloc, lex, true)) {
				free(buf);
				return unterminated_string(yylval, lex);
			}

			cur = current(lex);

			if (cur == '\n')
				continue;
		}

		if (pos >= len) {
			len *= 2;
			buf = realloc(buf, len);
		}

		buf[pos++] = cur;
	}

	yylval->str = fastd_string_stack_dupn(buf, pos);
	free(buf);

	next(yylloc, lex, true);
	consume(lex, true);

	return TOK_STRING;
}

static int parse_ipv6_address(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	if (lex->needspace)
		return syntax_error(yylval, lex);

	while (next(yylloc, lex, false)) {
		char cur = current(lex);

		if (!((cur >= '0' && cur <= '9') || cur == ':'))
			break;
	}

	bool ok = (current(lex) == ']');

	if (ok) {
		lex->buffer[lex->start + lex->tok_len] = 0;
		ok = inet_pton(AF_INET6, lex->buffer+lex->start+1, &yylval->addr6);
	}

	if (!ok) {
		yylval->error = "invalid address";
		return -1;
	}

	next(yylloc, lex, true);
	consume(lex, true);

	return TOK_ADDR6;
}

static int parse_ipv4_address(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	if (lex->needspace)
		return syntax_error(yylval, lex);

	while (next(yylloc, lex, false)) {
		char cur = current(lex);

		if (!((cur >= '0' && cur <= '9') || cur == '.'))
			break;
	}

	char *token = get_token(lex);
	bool ok = inet_pton(AF_INET, token, &yylval->addr4);

	free(token);

	if (!ok) {
		yylval->error = "invalid address";
		return -1;
	}

	return TOK_ADDR4;
}

static int parse_number(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	if (lex->needspace)
		return syntax_error(yylval, lex);

	while (next(yylloc, lex, false)) {
		char cur = current(lex);

		if (cur == '.')
			return parse_ipv4_address(yylval, yylloc, lex);

		if (!(cur >= '0' && cur <= '9'))
			break;
	}

	char *endptr, *token = get_token(lex);
	yylval->uint64 = strtoull(token, &endptr, 10);

	bool ok = !*endptr;
	free(token);

	if (!ok) {
		yylval->error = "invalid integer constant";
		return -1;
	}

	return TOK_UINT;
}

static int parse_keyword(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	if (lex->needspace)
		return syntax_error(yylval, lex);

	while (next(yylloc, lex, false)) {
		char cur = current(lex);

		if (!((cur >= 'a' && cur <= 'z') || (cur >= '0' && cur <= '9') || cur == '-'))
			break;
	}

	char *token = get_token(lex);
	const keyword_t key = {token};
	const keyword_t *ret = bsearch(&key, keywords, sizeof(keywords)/sizeof(keyword_t), sizeof(keyword_t), compare_keywords);
	free(token);

	if (!ret)
		return syntax_error(yylval, lex);

	consume(lex, true);

	return ret->token;
}

fastd_lex_t* fastd_lex_init(FILE *file) {
	fastd_lex_t *lex = calloc(1, sizeof(fastd_lex_t));
	lex->file = file;

	advance(lex);

	return lex;
}

void fastd_lex_destroy(fastd_lex_t *lex) {
	if (!lex)
		return;

	free(lex);
}

int fastd_lex(YYSTYPE *yylval, YYLTYPE *yylloc, fastd_lex_t *lex) {
	int token;

	while (lex->end > lex->start) {
		yylloc->first_line = yylloc->last_line;
		yylloc->first_column = yylloc->last_column+1;

		switch (current(lex)) {
		case ' ':
		case '\n':
		case '\t':
		case '\r':
			next(yylloc, lex, true);
			consume(lex, false);
			continue;

		case ';':
		case ':':
		case '{':
		case '}':
			token = current(lex);
			next(yylloc, lex, true);
			consume(lex, false);
			return token;

		case '/':
			if (!next(yylloc, lex, true))
				return syntax_error(yylval, lex);

			if (current(lex) == '*') {
				token = consume_comment(yylval, yylloc, lex);
				if (token)
					return token;

				continue;
			}

			if (current(lex) != '/')
				return syntax_error(yylval, lex);

			/* fall-through */
		case '#':
			while (next(yylloc, lex, true)) {
				if (current(lex) == '\n')
					break;
			}

			next(yylloc, lex, true);
			consume(lex, false);
			continue;

		case '"':
			return parse_string(yylval, yylloc, lex);

		case '[':
			return parse_ipv6_address(yylval, yylloc, lex);

		case '0' ... '9':
			return parse_number(yylval, yylloc, lex);

		case 'a' ... 'z':
			return parse_keyword(yylval, yylloc, lex);

		default:
			return syntax_error(yylval, lex);
		}
	}

	return end(yylval, lex);
}