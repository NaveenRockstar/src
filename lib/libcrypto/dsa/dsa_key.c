/* $OpenBSD: dsa_key.c,v 1.32 2022/11/26 16:08:52 tb Exp $ */
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

#include <stdio.h>
#include <time.h>

#include <openssl/opensslconf.h>

#ifndef OPENSSL_NO_SHA

#include <openssl/bn.h>
#include <openssl/dsa.h>

#include "bn_local.h"
#include "dsa_local.h"

static int dsa_builtin_keygen(DSA *dsa);

int
DSA_generate_key(DSA *dsa)
{
	if (dsa->meth->dsa_keygen)
		return dsa->meth->dsa_keygen(dsa);
	return dsa_builtin_keygen(dsa);
}

static int
dsa_builtin_keygen(DSA *dsa)
{
	int ok = 0;
	BN_CTX *ctx = NULL;
	BIGNUM *pub_key = NULL, *priv_key = NULL;

	if ((ctx = BN_CTX_new()) == NULL)
		goto err;

	if ((priv_key = dsa->priv_key) == NULL) {
		if ((priv_key = BN_new()) == NULL)
			goto err;
	}

	if (!bn_rand_interval(priv_key, BN_value_one(), dsa->q))
		goto err;

	if ((pub_key = dsa->pub_key) == NULL) {
		if ((pub_key = BN_new()) == NULL)
			goto err;
	}

	if (!BN_mod_exp_ct(pub_key, dsa->g, priv_key, dsa->p, ctx))
		goto err;

	dsa->priv_key = priv_key;
	dsa->pub_key = pub_key;
	ok = 1;

 err:
	if (dsa->pub_key == NULL)
		BN_free(pub_key);
	if (dsa->priv_key == NULL)
		BN_free(priv_key);
	BN_CTX_free(ctx);
	return ok;
}
#endif
