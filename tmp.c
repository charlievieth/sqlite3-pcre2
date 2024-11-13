//go:build never
// +build never

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>

#include <sqlite3.h>

// static int count_double_quotes(const char *str) {
// 	int n = 0;
// 	while ((str = strchr(str, '\'')) != NULL) {
// 		n++;
// 	}
// 	return n;
// }

int is_space(unsigned char ch) {
	return !!(ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' ||
	          ch == '\r' || ch == ' ');
	// switch (ch) {
	// case '\t':; case '\n':; case '\v':; case '\f':; case '\r':; case ' ':
	// 	return true;
	// }
	// return false;
}

const char *trim_leading_spaces(const char *str) {
	if (str) {
		char ch = *str;
		if (ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r' || ch == ' ') {
			/* code */
		}
		while (isspace(*str)) {
			str++;
		}
	}
	return str;
}

long long strdelta(const char *start, const char *end) {
	if (start && end && start <= end) {
		return end - start;
	}
	return -1;
}

static const bool invalid_chars[256] = {
    [0 ... 8] = true,
    [12] = true,
    [14 ... 31] = true,
    [39] = true, // '\''
};

static int has_invalid_chars(const char *str, const int len) {
	const unsigned char *p = (const unsigned char *)str;
	const unsigned char *end = p + len;
	while (p < end) {
		switch (*p++) {
		case 0 ... 8:
		case 12:
		case 14 ... 31:
		case 39:
			return true;
		}
	}
	return false;

	// int i;
	// for (i = 0; i < len - 4; i += 4) {
	// 	if (invalid_chars[p[i]] || invalid_chars[p[i+1]] ||
	// invalid_chars[p[i+2]] || invalid_chars[p[i+3]]) { 		return true;
	// 	}
	// }
	// for ( ; i < len; i++) {
	// 	if (invalid_chars[p[i]]) {
	// 		return true;
	// 	}
	// }
	// return false;
}

static const char *ascci_replacement_chars[] = {
    "\\x00", // nul
    "\\x01", // soh
    "\\x02", // stx
    "\\x03", // etx
    "\\x04", // eot
    "\\x05", // enq
    "\\x06", // ack
    "\\x07", // bel
    "\\x08", // bs
    NULL,    // ht '\x09'
    NULL,    // nl '\x0a'
    NULL,    // vt '\x0b'
    "\\x0c", // np
    NULL,    // cr '\x0d'
    "\\x0e", // so
    "\\x0f", // si
    "\\x10", // dle
    "\\x11", // dc1
    "\\x12", // dc2
    "\\x13", // dc3
    "\\x14", // dc4
    "\\x15", // nak
    "\\x16", // syn
    "\\x17", // etb
    "\\x18", // can
    "\\x19", // em
    "\\x1a", // sub
    "\\x1b", // esc
    "\\x1c", // fs
    "\\x1d", // gs
    "\\x1e", // rs
    "\\x1f", // us
};
static const int ascci_replacement_chars_len =
    sizeof(ascci_replacement_chars) / sizeof(ascci_replacement_chars[0]);

static int count_double_quotes(const char *str, int len) {
	int n = 0;
	const char *end = str + len;
	while (str < end) {
		if (*str++ == '\'') {
			n++;
		}
	}
	return n;
}

static char *quote_string(const char *str, int len) {
	int n = count_double_quotes(str, len);
	if (n == 0) {
		return NULL;
	}

	char *buf = malloc(len + n + 1);
	char *p = buf;
	const char *end = str + len;
	while (str < end) {
		*p++ = *str;
		if (*str++ == '\'') {
			*p++ = '\'';
		}
	}
	return buf;
}

/*
static void base64_encode_fast(const char *restrict in, size_t inlen, char
*restrict out) { while (inlen) { *out++ = b64c[(to_uchar(in[0]) >> 2) & 0x3f];
        *out++ = b64c[((to_uchar(in[0]) << 4) + (to_uchar(in[1]) >> 4)) & 0x3f];
        *out++ = b64c[((to_uchar(in[1]) << 2) + (to_uchar(in[2]) >> 6)) & 0x3f];
        *out++ = b64c[to_uchar(in[2]) & 0x3f];

        inlen -= 3;
        in += 3;
    }
}
*/

// #define BASE64_LENGTH(inlen) ((((inlen) + 2) / 3) * 4)

static size_t base64_encoded_len(size_t n) {
	return (n + 2) / 3 * 4;
}

void base64_encode(const char *restrict in, size_t inlen,
                   char *restrict out, size_t outlen) {

	static const char b64[64] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	ssize_t si = 0;
	ssize_t n = (inlen / 3) * 3;
	const unsigned char *p = (const unsigned char *)in;

	while (si < n) {
		*out++ = b64[(p[0] >> 2) & 0x3F];
		*out++ = b64[((p[0] << 4) + (p[1] >> 4)) & 0x3f];
		*out++ = b64[((p[1] << 2) + (p[2] >> 6)) & 0x3f];
		*out++ = b64[p[2] & 0x3F];
		si += 3;
		p += 3;
	}

	ssize_t remain = inlen - si;
	if (remain == 0) {
		*out = '\0';
		return;
	}

	// add the remaining small block
	switch (remain) {
	case 2:
		*out++ = b64[(p[0] >> 2) & 0x3F];
		*out++ = b64[((p[0] << 4) + (p[1] >> 4)) & 0x3f];
		*out++ = b64[(p[1] << 2) & 0x3f];
		*out++ = '=';
		break;
	case 1:
		*out++ = b64[(p[0] >> 2) & 0x3F];
		*out++ = b64[(p[0] << 4) & 0x3f];
		*out++ = '=';
		*out++ = '=';
		break;
	}
	*out = '\0';
}

static char *base64(const char *restrict in) {
	size_t inlen = strlen(in);
	size_t outlen = base64_encoded_len(inlen);
	char *p = malloc(outlen + strlen("base64:") + 1);
	char *out = stpcpy(p, "base64:");
	base64_encode(in, inlen, out, outlen);
	return p;
}

static bool fixup_pattern(const char *str, int str_len, char **dst, int *dst_len, int erroffset) {
	// TODO: use a macro and make this configurable
	const int max_size = 64;

	// "\n... omitting "
	// " bytes ...\n"
	//
	// "\n... omitting %d bytes ...\n"

	if (str_len <= max_size) {
		*dst = NULL;
		*dst_len = 0;
		return false;
	}

	// const char *format = "%.*s\n... omitting %d bytes ...\n%.*s";
	const char *format = "%.*s... omitting %d bytes ...%.*s";

	int omitted = str_len - max_size;
	int n = snprintf(NULL,
	                 0,
	                 format,
	                 max_size / 2,
	                 str,
	                 omitted,
	                 max_size / 2,
	                 &str[str_len - (max_size / 2)]);
	assert(n >= 0);

	char *buf = malloc(n + 1);
	n = snprintf(buf,
	             n + 1,
	             format,
	             max_size / 2,
	             str,
	             omitted,
	             max_size / 2,
	             &str[str_len - (max_size / 2)]);
	assert(n >= 0);

	*dst = buf;
	*dst_len = n;

	return true;
}

// static void fnv128(const char *in, size_t inlen) {
// 	const __int128 offset128Lower  = 0x62b821756295c58d;
// 	const __int128 offset128Higher = 0x6c62272e07bb0142;
// 	const __int128 offset = (offset128Higher << 64) | offset128Lower;
//
// 	__int128 x = 0;
// }

int main(int argc, char const *argv[]) {
	{
		const char *str = " \n \t  abc;";
		// const char *str = "";
		printf("'%s' -> '%s'\n", str, trim_leading_spaces(str));
		return 0;
	}

	// char *str = strdup("AABBCCDDEEFFGG\n");
	// char *res = base64(str);
	// printf("%s\n", res);
	// free(str);
	// return 0;

	// // const char *test_strings[] = {
	// // 	"ab",
	// // 	"a'b",
	// // };
	// // const int test_strings_len = sizeof(test_strings) /
	// // sizeof(test_strings[0]); for (int i = 0; i < test_strings_len; i++) {
	// // 	const char *str = test_strings[i];
	// // 	char *buf = quote_string(str, strlen(str));
	// // 	printf("%s: %s\n", str, buf);
	// // 	free(buf);
	// // }

	// const char *str = "Loads the \x1b\x1b\x1b data from the given locations, "
	//                   "converts them to character string equivalents and "
	//                   "writes the results to a variety of sinks/streams";
	const char *str = "Loads the data";
	int str_len = strlen(str);
	char *dst;
	int dst_len;
	fixup_pattern(str, str_len, &dst, &dst_len, 40);
	printf("%s\n", dst);

	return 0;
}
