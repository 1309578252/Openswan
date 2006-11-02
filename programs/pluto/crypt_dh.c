/* 
 * Cryptographic helper function - calculate DH
 * Copyright (C) 2004 Michael C. Richardson <mcr@xelerance.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * This code was developed with the support of IXIA communications.
 *
 * RCSID $Id: crypt_dh.c,v 1.11 2005/08/14 21:47:29 mcr Exp $
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <signal.h>
#include <crypto/cryptodev.h>

#include <openswan.h>
#include <openswan/ipsec_policy.h>

#include "sysdep.h"
#include "constants.h"
#include "defs.h"
#include "packet.h"
#include "demux.h"
#include "crypto.h"
#include "rnd.h"
#include "state.h"
#include "pluto_crypt.h"
#include "oswlog.h"
#include "log.h"
#include "timer.h"
#include "ike_alg.h"
#include "id.h"
#include "secrets.h"
#include "keys.h"
#include "ocf_cryptodev.h"

#ifdef VULCAN_PK
#include "dev/hifn/vulcanpk_funcs.h"
#endif

#ifdef VULCAN_PK
#include "dev/hifn/vulcanpk_funcs.h"
#endif

/** Compute DH shared secret from our local secret and the peer's public value.
 * We make the leap that the length should be that of the group
 * (see quoted passage at start of ACCEPT_KE).
 */
void
calc_dh_shared_gmp(chunk_t *shared, const chunk_t g
		   , const chunk_t *secchunk
		   , const struct oakley_group_desc *group)
{
    MP_INT mp_g, mp_shared, sec;
    struct timeval tv0, tv1;
    unsigned long tv_diff;

    gettimeofday(&tv0, NULL);

    /* recover the long term secret */
    n_to_mpz(&sec, secchunk->ptr, secchunk->len);

    /* do the exponentiation */
    n_to_mpz(&mp_g, g.ptr, g.len);
    mpz_init(&mp_shared);

#ifdef PK_DH_REGRESS
    if(pkdh_verbose) {
	printf("mp_g: ");
	mpz_out_str(stdout, 16, &mp_g);
	printf("\nsecret: ");
	mpz_out_str(stdout, 16, &sec);
	printf("\nmodulus: ");
	mpz_out_str(stdout, 16, group->modulus);
    }
#endif

    mpz_powm(&mp_shared, &mp_g, &sec, group->modulus);

#ifdef PK_DH_REGRESS
    if(pkdh_verbose) {
	printf("\nshared: ");
	mpz_out_str(stdout, 16, &mp_shared);
	printf("\n");
    }
#endif
    mpz_clear(&mp_g);

    *shared = mpz_to_n(&mp_shared, group->bytes);
    mpz_clear(&mp_shared);

    gettimeofday(&tv1, NULL);
    tv_diff=(tv1.tv_sec  - tv0.tv_sec) * 1000000 + (tv1.tv_usec - tv0.tv_usec);
    DBG(DBG_CRYPT, 
    	DBG_log("calc_dh_shared(): time elapsed (%s): %ld usec"
		, enum_show(&oakley_group_names, group->group)
		, tv_diff);
       );
    /* if took more than 200 msec ... */
    if (tv_diff > 200000) {
	loglog(RC_LOG_SERIOUS, "WARNING: calc_dh_shared(): for %s took "
			"%ld usec"
		, enum_show(&oakley_group_names, group->group)
		, tv_diff);
    }

    DBG_cond_dump_chunk(DBG_CRYPT, "DH shared-secret:\n", *shared);
}

#ifdef VULCAN_PK
#include "vulcan/crypto_dh_vulcanpk.c"
#else

/*
 * Set up the modular exponentiation operation via OCF interface
 */
void
calc_dh_shared_ocf(chunk_t *shared, const chunk_t g
		   , const chunk_t *secchunk
		   , const struct oakley_group_desc *group)
{
	struct crypt_kop kop;
	struct timeval tv0, tv1;
	chunk_t gle, secle;
	unsigned char *result;
	unsigned long tv_diff;
	int ret = 1;

#if 0
	/* Currently, we know we can do mod exp iff we can do any
	 * asymmetric operations at all.
	 */
	DBG(DBG_CRYPT, DBG_log("dh_share_ocf: %d\n", cryptodev_asymfeat));

	if (cryptodev_asymfeat == 0) {
	    DBG(DBG_CRYPT, DBG_log("ocf not available, using gmp\n"));
	    
	    calc_dh_shared_gmp(shared, g, secchunk, group);
	    return;
	}
#endif

	gettimeofday(&tv0, NULL);

	/* mod exp calculates A^B mod M, (g^x)^y mod M */
	memset(&kop, 0, sizeof kop);
	kop.crk_op = CRK_MOD_EXP;

	/* point to peer's calculated (g^x) */

	clonetochunk(gle, g.ptr, g.len, "gle");
	chunk2le(gle);
	kop.crk_param[CRK_MOD_PARAM_BASE].crp_p = (caddr_t)gle.ptr;
	kop.crk_param[CRK_MOD_PARAM_BASE].crp_nbits = gle.len*8;

	/* point to our secret value, ^y */
	clonetochunk(secle, secchunk->ptr, secchunk->len, "secle");
	chunk2le(secle);
	kop.crk_param[CRK_MOD_PARAM_EXP].crp_p = (caddr_t)secle.ptr;
	kop.crk_param[CRK_MOD_PARAM_EXP].crp_nbits = secle.len*8;

	/* point to our secret value, ^y */
	/* M = modulus */
	kop.crk_param[CRK_MOD_PARAM_MOD].crp_p = (caddr_t)group->raw_modulus_le.ptr;
	kop.crk_param[CRK_MOD_PARAM_MOD].crp_nbits = group->raw_modulus_le.len*8;
	
	kop.crk_param[CRK_MOD_PARAM_RECIP].crp_p = (caddr_t)group->rec_modulus_le.ptr;
	kop.crk_param[CRK_MOD_PARAM_RECIP].crp_nbits=group->rec_modulus_le.len*8;

	kop.crk_iparams = 4;
	
	result = alloc_bytes(group->bytes, "DH result");
	kop.crk_param[kop.crk_iparams].crp_p = (caddr_t)result;
	kop.crk_param[kop.crk_iparams].crp_nbits = group->bytes * 8;
	setchunk(*shared, result, group->bytes);

	kop.crk_oparams = 1;

	ret = cryptodev_asym(&kop);

	{ int e = errno;

	    chunk2be(*shared);

	    gettimeofday(&tv1, NULL);
	    tv_diff=(tv1.tv_sec  - tv0.tv_sec) * 1000000 + (tv1.tv_usec - tv0.tv_usec);
	    if(ret != 0) {
		loglog(RC_LOG_SERIOUS, "failed to do DH calculation: %s",
		       strerror(e));
	    }
	}
	
	DBG(DBG_CRYPT, 
	    DBG_log("calc_dh_shared_ocf(): time elapsed (%s): %ld usec"
		    , enum_show(&oakley_group_names, group->group)
		    , tv_diff);
	    );
	/* if took more than 200 msec ... */
	if (tv_diff > 200000) {
	    loglog(RC_LOG_SERIOUS, "WARNING: calc_dh_shared_ocf(): for %s took "
		   "%ld usec"
		   , enum_show(&oakley_group_names, group->group)
		   , tv_diff);
	}
	
	DBG_cond_dump_chunk(DBG_CRYPT, "DH shared-secret:\n", *shared);
	
//err:
	freeanychunk(gle);
	freeanychunk(secle);
	return;
}
#endif

/* SKEYID for preshared keys.
 * See draft-ietf-ipsec-ike-01.txt 4.1
 */
static void
skeyid_preshared(const chunk_t pss
		 , const chunk_t ni
		 , const chunk_t nr
		 , const struct hash_desc *hasher
		 , chunk_t *skeyid)
{
    struct hmac_ctx ctx;

    passert(hasher != NULL);

    DBG(DBG_CRYPT,
	DBG_log("Skey inputs (PSK+NI+NR)");
	DBG_dump_chunk("ni: ", ni);
	DBG_dump_chunk("nr: ", nr));
    
    hmac_init_chunk(&ctx, hasher, pss);
    hmac_update_chunk(&ctx, ni);
    hmac_update_chunk(&ctx, nr);
    hmac_final_chunk(*skeyid, "st_skeyid in skeyid_preshared()", &ctx);
    DBG(DBG_CRYPT,
	DBG_dump_chunk("keyid: ", *skeyid));
}

static void
skeyid_digisig(const chunk_t ni
	       , const chunk_t nr
	       , const chunk_t shared
	       , const struct hash_desc *hasher
	       , chunk_t *skeyid)
{
    struct hmac_ctx ctx;
    chunk_t nir;

    DBG(DBG_CRYPT,
	DBG_log("skeyid inputs (digi+NI+NR+shared) hasher: %s", hasher->common.name);
	DBG_dump_chunk("shared-secret: ", shared);
	DBG_dump_chunk("ni: ", ni);
	DBG_dump_chunk("nr: ", nr));
    
    /* We need to hmac_init with the concatenation of Ni_b and Nr_b,
     * so we have to build a temporary concatentation.
     */
    nir.len = ni.len + nr.len;
    nir.ptr = alloc_bytes(nir.len, "Ni + Nr in skeyid_digisig");
    memcpy(nir.ptr, ni.ptr, ni.len);
    memcpy(nir.ptr+ ni.len, nr.ptr, nr.len);
    hmac_init_chunk(&ctx, hasher, nir);
    pfree(nir.ptr);

    hmac_update_chunk(&ctx, shared);
    hmac_final_chunk(*skeyid, "st_skeyid in skeyid_digisig()", &ctx);
    DBG(DBG_CRYPT,
	DBG_dump_chunk("keyid: ", *skeyid));
}

/* Generate the SKEYID_* and new IV
 * See draft-ietf-ipsec-ike-01.txt 4.1
 */
static void
calc_skeyids_iv(struct pcr_skeyid_q *skq
		, chunk_t shared
		, const size_t keysize     /* = st->st_oakley.enckeylen/BITS_PER_BYTE; */
		, chunk_t *skeyid          /* output */
		, chunk_t *skeyid_d        /* output */
		, chunk_t *skeyid_a        /* output */
		, chunk_t *skeyid_e        /* output */
		, chunk_t *new_iv
		, chunk_t *enc_key
    )
{
    oakley_auth_t auth = skq->auth;
    oakley_hash_t hash = skq->hash;
    const struct hash_desc *hasher = crypto_get_hasher(hash);
    chunk_t pss;  
    chunk_t ni;
    chunk_t nr;
    chunk_t gi;
    chunk_t gr;
    chunk_t icookie;
    chunk_t rcookie;

    /* this doesn't take any memory */
    setchunk_fromwire(gi, &skq->gi, skq);
    setchunk_fromwire(gr, &skq->gr, skq);
    setchunk_fromwire(ni, &skq->ni, skq);
    setchunk_fromwire(nr, &skq->nr, skq);
    setchunk_fromwire(icookie, &skq->icookie, skq);
    setchunk_fromwire(rcookie, &skq->rcookie, skq);

    /* Generate the SKEYID */
    switch (auth)
    {
	case OAKLEY_PRESHARED_KEY:
	    setchunk_fromwire(pss,    &skq->pss, skq);
	    skeyid_preshared(pss, ni, nr, hasher, skeyid);
	    break;

	case OAKLEY_RSA_SIG:
	    skeyid_digisig(ni, nr, shared, hasher, skeyid);
	    break;

	case OAKLEY_DSS_SIG:
	    /* XXX */

	case OAKLEY_RSA_ENC:
	case OAKLEY_RSA_ENC_REV:
	case OAKLEY_ELGAMAL_ENC:
	case OAKLEY_ELGAMAL_ENC_REV:
	    /* XXX */

	default:
	    bad_case(auth);
    }

    /* generate SKEYID_* from SKEYID */
    {
	struct hmac_ctx ctx;

	/* SKEYID_D */
	hmac_init_chunk(&ctx, hasher, *skeyid);
	hmac_update_chunk(&ctx, shared);
	hmac_update_chunk(&ctx, icookie);
	hmac_update_chunk(&ctx, rcookie);
	hmac_update(&ctx, (const u_char *)"\0", 1);
	hmac_final_chunk(*skeyid_d, "st_skeyid_d in generate_skeyids_iv()", &ctx);

	/* SKEYID_A */
	hmac_reinit(&ctx);
	hmac_update_chunk(&ctx, *skeyid_d);
	hmac_update_chunk(&ctx, shared);
	hmac_update_chunk(&ctx, icookie);
	hmac_update_chunk(&ctx, rcookie);
	hmac_update(&ctx, (const u_char *)"\1", 1);
	hmac_final_chunk(*skeyid_a, "st_skeyid_a in generate_skeyids_iv()", &ctx);

	/* SKEYID_E */
	hmac_reinit(&ctx);
	hmac_update_chunk(&ctx, *skeyid_a);
	hmac_update_chunk(&ctx, shared);
	hmac_update_chunk(&ctx, icookie);
	hmac_update_chunk(&ctx, rcookie);
	hmac_update(&ctx, (const u_char *)"\2", 1);
	hmac_final_chunk(*skeyid_e, "st_skeyid_e in generate_skeyids_iv()", &ctx);
    }

    /* generate IV */
    {
	union hash_ctx hash_ctx;

	new_iv->len = hasher->hash_digest_len;
	new_iv->ptr = alloc_bytes(new_iv->len, "calculated new iv");

        DBG(DBG_CRYPT,
            DBG_dump_chunk("DH_i:", gi);
            DBG_dump_chunk("DH_r:", gr);
        );
	hasher->hash_init(&hash_ctx);
	hasher->hash_update(&hash_ctx, gi.ptr, gi.len);
	hasher->hash_update(&hash_ctx, gr.ptr, gr.len);
	hasher->hash_final(new_iv->ptr, &hash_ctx);
    }

    /* Oakley Keying Material
     * Derived from Skeyid_e: if it is not big enough, generate more
     * using the PRF.
     * See RFC 2409 "IKE" Appendix B
     */
    {
	u_char keytemp[MAX_OAKLEY_KEY_LEN + MAX_DIGEST_LEN];
	u_char *k = skeyid_e->ptr;

	if (keysize > skeyid_e->len)
	{
	    struct hmac_ctx ctx;
	    size_t i = 0;

	    hmac_init_chunk(&ctx, hasher, *skeyid_e);
	    hmac_update(&ctx, (const u_char *)"\0", 1);
	    for (;;)
	    {
		hmac_final(&keytemp[i], &ctx);
		i += ctx.hmac_digest_len;
		if (i >= keysize)
		    break;
		hmac_reinit(&ctx);
		hmac_update(&ctx, &keytemp[i - ctx.hmac_digest_len], ctx.hmac_digest_len);
	    }
	    k = keytemp;
	}
	clonereplacechunk(*enc_key, k, keysize, "st_enc_key");
    }

    DBG(DBG_CRYPT,
	DBG_dump_chunk("Skeyid:  ", *skeyid);
	DBG_dump_chunk("Skeyid_d:", *skeyid_d);
	DBG_dump_chunk("Skeyid_a:", *skeyid_a);
	DBG_dump_chunk("Skeyid_e:", *skeyid_e);
	DBG_dump_chunk("enc key:",  *enc_key);
	DBG_dump_chunk("IV:",       *new_iv));
}


void calc_dh_iv(struct pluto_crypto_req *r)
{
    struct pcr_skeyid_q *skq = &r->pcr_d.dhq;
    struct pcr_skeyid_r *skr = &r->pcr_d.dhr;
    struct pcr_skeyid_q dhq;
    const struct oakley_group_desc *group;
    chunk_t  sec;
    chunk_t  shared, g;
    chunk_t  skeyid, skeyid_d, skeyid_a, skeyid_e; 
    chunk_t  new_iv, enc_key;

    /* copy the request, since we will use the same memory for the reply */
    memcpy(&dhq, skq, sizeof(struct pcr_skeyid_q));

    /* clear out the reply */
    memset(skr, 0, sizeof(*skr));
    skr->thespace.start = 0;
    skr->thespace.len   = sizeof(skr->space);

    group = lookup_group(dhq.oakley_group);
    passert(group != NULL);

    pluto_crypto_allocchunk(&skr->thespace
			   , &skr->shared
			   , group->bytes);
    shared.ptr = wire_chunk_ptr(skr, &skr->shared);
    shared.len = group->bytes;

    sec.ptr = wire_chunk_ptr(&dhq, &dhq.secret);
    sec.len = dhq.secret.len;

    DBG(DBG_CRYPT,
	DBG_dump_chunk("long term secret: ", sec));

    /* now calculate the (g^x)(g^y) --- need gi on responder, gr on initiator */

    if(dhq.init == RESPONDER) {
      setchunk_fromwire(g, &dhq.gi, &dhq);
    } else {
      setchunk_fromwire(g, &dhq.gr, &dhq);
    }

    (*cryptodev.calc_dh_shared)(&shared, g, &sec, group);
    
    memset(&skeyid, 0, sizeof(skeyid));
    memset(&skeyid_d, 0, sizeof(skeyid_d));
    memset(&skeyid_a, 0, sizeof(skeyid_a));
    memset(&skeyid_e, 0, sizeof(skeyid_e));
    memset(&new_iv,   0, sizeof(new_iv));
    memset(&enc_key,  0, sizeof(enc_key));
    /* okay, so now calculate IV */
    calc_skeyids_iv(&dhq
		    , shared
		    , dhq.keysize
		    , &skeyid
		    , &skeyid_d
		    , &skeyid_a
		    , &skeyid_e
		    , &new_iv
		    , &enc_key);

    /* now translate it back to wire chunks, freeing the chunks */
    setwirechunk_fromchunk(skr->shared,   shared,   skr);
    setwirechunk_fromchunk(skr->skeyid,   skeyid,   skr);
    setwirechunk_fromchunk(skr->skeyid_d, skeyid_d, skr);
    setwirechunk_fromchunk(skr->skeyid_a, skeyid_a, skr);
    setwirechunk_fromchunk(skr->skeyid_e, skeyid_e, skr);
    setwirechunk_fromchunk(skr->new_iv,   new_iv,   skr);
    setwirechunk_fromchunk(skr->enc_key,  enc_key,  skr);

    freeanychunk(shared);
    freeanychunk(skeyid);
    freeanychunk(skeyid_d);
    freeanychunk(skeyid_a);
    freeanychunk(skeyid_e);
    freeanychunk(new_iv);
    freeanychunk(enc_key);

    return;
}

void calc_dh(struct pluto_crypto_req *r)
{
    struct pcr_skeyid_q *skq = &r->pcr_d.dhq;
    struct pcr_skeyid_r *skr = &r->pcr_d.dhr;
    struct pcr_skeyid_q dhq;
    const struct oakley_group_desc *group;
    chunk_t  sec;
    chunk_t  shared, g;

    /* copy the request, since we will use the same memory for the reply */
    memcpy(&dhq, skq, sizeof(struct pcr_skeyid_q));

    /* clear out the reply */
    memset(skr, 0, sizeof(*skr));
    skr->thespace.start = 0;
    skr->thespace.len   = sizeof(skr->space);

    group = lookup_group(dhq.oakley_group);

    pluto_crypto_allocchunk(&skr->thespace
			   , &skr->shared
			   , group->bytes);
    shared.ptr = wire_chunk_ptr(skr, &skr->shared);
    shared.len = group->bytes;

    /* recover the long term secret */
    sec.ptr = wire_chunk_ptr(&dhq, &dhq.secret);
    sec.len = dhq.secret.len;

    /* now calculate the (g^x)(g^y) */

    if(dhq.init == RESPONDER) {
      setchunk_fromwire(g, &dhq.gi, &dhq);
    } else {
      setchunk_fromwire(g, &dhq.gr, &dhq);
    }
    (*cryptodev.calc_dh_shared)(&shared, g, &sec, group);

    /* now translate it back to wire chunks, freeing the chunks */
    setwirechunk_fromchunk(skr->shared,   shared,   skr);
    freeanychunk(shared);

    return;
}


/*
 * Local Variables:
 * c-basic-offset:4
 * c-style: pluto
 * End:
 */

