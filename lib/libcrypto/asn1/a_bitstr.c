/* $OpenBSD: a_bitstr.c,v 1.37 2022/11/08 16:48:28 tb Exp $ */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include "bytestring.h"

const ASN1_ITEM ASN1_BIT_STRING_it = {
	.itype = ASN1_ITYPE_PRIMITIVE,
	.utype = V_ASN1_BIT_STRING,
	.sname = "ASN1_BIT_STRING",
};

ASN1_BIT_STRING *
ASN1_BIT_STRING_new(void)
{
	return (ASN1_BIT_STRING *)ASN1_item_new(&ASN1_BIT_STRING_it);
}

void
ASN1_BIT_STRING_free(ASN1_BIT_STRING *a)
{
	ASN1_item_free((ASN1_VALUE *)a, &ASN1_BIT_STRING_it);
}

static void
asn1_abs_clear_unused_bits(ASN1_BIT_STRING *abs)
{
	abs->flags &= ~(ASN1_STRING_FLAG_BITS_LEFT | 0x07);
}

int
asn1_abs_set_unused_bits(ASN1_BIT_STRING *abs, uint8_t unused_bits)
{
	if (unused_bits > 7)
		return 0;

	asn1_abs_clear_unused_bits(abs);

	abs->flags |= ASN1_STRING_FLAG_BITS_LEFT | unused_bits;

	return 1;
}

int
ASN1_BIT_STRING_set(ASN1_BIT_STRING *x, unsigned char *d, int len)
{
	return ASN1_STRING_set(x, d, len);
}

int
ASN1_BIT_STRING_set_bit(ASN1_BIT_STRING *a, int n, int value)
{
	int w, v, iv;
	unsigned char *c;

	w = n/8;
	v = 1 << (7 - (n & 0x07));
	iv = ~v;
	if (!value)
		v = 0;

	if (a == NULL)
		return 0;

	asn1_abs_clear_unused_bits(a);

	if ((a->length < (w + 1)) || (a->data == NULL)) {
		if (!value)
			return(1); /* Don't need to set */
		if ((c = recallocarray(a->data, a->length, w + 1, 1)) == NULL) {
			ASN1error(ERR_R_MALLOC_FAILURE);
			return 0;
		}
		a->data = c;
		a->length = w + 1;
	}
	a->data[w] = ((a->data[w]) & iv) | v;
	while ((a->length > 0) && (a->data[a->length - 1] == 0))
		a->length--;

	return (1);
}

int
ASN1_BIT_STRING_get_bit(const ASN1_BIT_STRING *a, int n)
{
	int w, v;

	w = n / 8;
	v = 1 << (7 - (n & 0x07));
	if ((a == NULL) || (a->length < (w + 1)) || (a->data == NULL))
		return (0);
	return ((a->data[w] & v) != 0);
}

/*
 * Checks if the given bit string contains only bits specified by
 * the flags vector. Returns 0 if there is at least one bit set in 'a'
 * which is not specified in 'flags', 1 otherwise.
 * 'len' is the length of 'flags'.
 */
int
ASN1_BIT_STRING_check(const ASN1_BIT_STRING *a, const unsigned char *flags,
    int flags_len)
{
	int i, ok;

	/* Check if there is one bit set at all. */
	if (!a || !a->data)
		return 1;

	/* Check each byte of the internal representation of the bit string. */
	ok = 1;
	for (i = 0; i < a->length && ok; ++i) {
		unsigned char mask = i < flags_len ? ~flags[i] : 0xff;
		/* We are done if there is an unneeded bit set. */
		ok = (a->data[i] & mask) == 0;
	}
	return ok;
}

int
ASN1_BIT_STRING_name_print(BIO *out, ASN1_BIT_STRING *bs,
    BIT_STRING_BITNAME *tbl, int indent)
{
	BIT_STRING_BITNAME *bnam;
	char first = 1;

	BIO_printf(out, "%*s", indent, "");
	for (bnam = tbl; bnam->lname; bnam++) {
		if (ASN1_BIT_STRING_get_bit(bs, bnam->bitnum)) {
			if (!first)
				BIO_puts(out, ", ");
			BIO_puts(out, bnam->lname);
			first = 0;
		}
	}
	BIO_puts(out, "\n");
	return 1;
}

int
ASN1_BIT_STRING_set_asc(ASN1_BIT_STRING *bs, const char *name, int value,
    BIT_STRING_BITNAME *tbl)
{
	int bitnum;

	bitnum = ASN1_BIT_STRING_num_asc(name, tbl);
	if (bitnum < 0)
		return 0;
	if (bs) {
		if (!ASN1_BIT_STRING_set_bit(bs, bitnum, value))
			return 0;
	}
	return 1;
}

int
ASN1_BIT_STRING_num_asc(const char *name, BIT_STRING_BITNAME *tbl)
{
	BIT_STRING_BITNAME *bnam;

	for (bnam = tbl; bnam->lname; bnam++) {
		if (!strcmp(bnam->sname, name) ||
		    !strcmp(bnam->lname, name))
			return bnam->bitnum;
	}
	return -1;
}

int
i2c_ASN1_BIT_STRING(ASN1_BIT_STRING *a, unsigned char **pp)
{
	int ret, j, bits, len;
	unsigned char *p, *d;

	if (a == NULL)
		return (0);

	if (a->length == INT_MAX)
		return (0);

	ret = a->length + 1;

	if (pp == NULL)
		return (ret);

	len = a->length;

	if (len > 0) {
		if (a->flags & ASN1_STRING_FLAG_BITS_LEFT) {
			bits = (int)a->flags & 0x07;
		} else {
			for (; len > 0; len--) {
				if (a->data[len - 1])
					break;
			}
			j = a->data[len - 1];
			if (j & 0x01)
				bits = 0;
			else if (j & 0x02)
				bits = 1;
			else if (j & 0x04)
				bits = 2;
			else if (j & 0x08)
				bits = 3;
			else if (j & 0x10)
				bits = 4;
			else if (j & 0x20)
				bits = 5;
			else if (j & 0x40)
				bits = 6;
			else if (j & 0x80)
				bits = 7;
			else
				bits = 0; /* should not happen */
		}
	} else
		bits = 0;

	p= *pp;

	*(p++) = (unsigned char)bits;
	d = a->data;
	if (len > 0) {
		memcpy(p, d, len);
		p += len;
		p[-1] &= 0xff << bits;
	}
	*pp = p;
	return (ret);
}

int
c2i_ASN1_BIT_STRING_cbs(ASN1_BIT_STRING **out_abs, CBS *cbs)
{
	ASN1_BIT_STRING *abs = NULL;
	uint8_t *data = NULL;
	size_t data_len = 0;
	uint8_t unused_bits;
	int ret = 0;

	if (out_abs == NULL)
		goto err;

	if (*out_abs != NULL) {
		ASN1_BIT_STRING_free(*out_abs);
		*out_abs = NULL;
	}

	if (!CBS_get_u8(cbs, &unused_bits)) {
		ASN1error(ASN1_R_STRING_TOO_SHORT);
		goto err;
	}

	if (!CBS_stow(cbs, &data, &data_len))
		goto err;
	if (data_len > INT_MAX)
		goto err;

	if ((abs = ASN1_BIT_STRING_new()) == NULL)
		goto err;

	abs->data = data;
	abs->length = (int)data_len;
	data = NULL;

	/*
	 * We do this to preserve the settings. If we modify the settings,
	 * via the _set_bit function, we will recalculate on output.
	 */
	if (!asn1_abs_set_unused_bits(abs, unused_bits)) {
		ASN1error(ASN1_R_INVALID_BIT_STRING_BITS_LEFT);
		goto err;
	}
	if (abs->length > 0)
		abs->data[abs->length - 1] &= 0xff << unused_bits;

	*out_abs = abs;
	abs = NULL;

	ret = 1;

 err:
	ASN1_BIT_STRING_free(abs);
	freezero(data, data_len);

	return ret;
}

ASN1_BIT_STRING *
c2i_ASN1_BIT_STRING(ASN1_BIT_STRING **out_abs, const unsigned char **pp, long len)
{
	ASN1_BIT_STRING *abs = NULL;
	CBS content;

	if (out_abs != NULL) {
		ASN1_BIT_STRING_free(*out_abs);
		*out_abs = NULL;
	}

	if (len < 0) {
		ASN1error(ASN1_R_LENGTH_ERROR);
		return NULL;
	}

	CBS_init(&content, *pp, len);

	if (!c2i_ASN1_BIT_STRING_cbs(&abs, &content))
		return NULL;

	*pp = CBS_data(&content);

	if (out_abs != NULL)
		*out_abs = abs;

	return abs;
}

int
i2d_ASN1_BIT_STRING(ASN1_BIT_STRING *a, unsigned char **out)
{
	return ASN1_item_i2d((ASN1_VALUE *)a, out, &ASN1_BIT_STRING_it);
}

ASN1_BIT_STRING *
d2i_ASN1_BIT_STRING(ASN1_BIT_STRING **a, const unsigned char **in, long len)
{
	return (ASN1_BIT_STRING *)ASN1_item_d2i((ASN1_VALUE **)a, in, len,
	    &ASN1_BIT_STRING_it);
}
