/*
 * card-flex.c: Support for Schlumberger cards
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sc-internal.h"
#include "sc-log.h"

static const char *flex_atrs[] = {
	"3B:95:94:40:FF:63:01:01:02:01", /* Cryptoflex 16k */
	"3B:19:14:55:90:01:02:02:00:05:04:B0",
	NULL
};

static struct sc_card_operations flex_ops;
static const struct sc_card_driver flex_drv = {
	NULL,
	"Schlumberger Multiflex/Cryptoflex",
	"slb",
	&flex_ops
};

static int flex_finish(struct sc_card *card)
{
	return 0;
}

static int flex_match_card(struct sc_card *card)
{
	int i, match = -1;

	for (i = 0; flex_atrs[i] != NULL; i++) {
		u8 defatr[SC_MAX_ATR_SIZE];
		size_t len = sizeof(defatr);
		const char *atrp = flex_atrs[i];

		if (sc_hex_to_bin(atrp, defatr, &len))
			continue;
		if (len != card->atr_len)
			continue;
		if (memcmp(card->atr, defatr, len) != 0)
			continue;
		match = i;
		break;
	}
	if (match == -1)
		return 0;

	return 1;
}

static int flex_init(struct sc_card *card)
{
	card->ops_data = NULL;
	card->cla = 0xC0;

	return 0;
}

static unsigned int ac_to_acl(u8 nibble)
{
	unsigned int acl_table[16] = {
		/* 0 */ SC_AC_NONE, SC_AC_CHV1, SC_AC_CHV2, SC_AC_PRO,
		/* 4 */ SC_AC_AUT, SC_AC_UNKNOWN, SC_AC_CHV1 | SC_AC_PRO,
		/* 7 */ SC_AC_CHV2 | SC_AC_PRO, SC_AC_CHV1 | SC_AC_AUT,
		/* 9 */ SC_AC_CHV2 | SC_AC_AUT, SC_AC_UNKNOWN, SC_AC_UNKNOWN,
		/* c */	SC_AC_UNKNOWN, SC_AC_UNKNOWN, SC_AC_UNKNOWN,
		/* f */ SC_AC_NEVER };
	return acl_table[nibble & 0x0F];
}

static int parse_flex_sf_reply(struct sc_context *ctx, const u8 *buf, int buflen,
			       struct sc_file *file)
{
	const u8 *p = buf + 2;
	u8 b1, b2;
        int left;
	
	if (buflen < 14)
		return -1;
	b1 = *p++;
	b2 = *p++;
	file->size = (b1 << 8) + b2;
	b1 = *p++;
	b2 = *p++;
	file->id = (b1 << 8) + b2;
	switch (*p) {
	case 0x01:
		file->type = SC_FILE_TYPE_WORKING_EF;
		file->ef_structure = SC_FILE_EF_TRANSPARENT;
		break;
	case 0x02:
		file->type = SC_FILE_TYPE_WORKING_EF;
		file->ef_structure = SC_FILE_EF_LINEAR_FIXED;
		break;
	case 0x04:
		file->type = SC_FILE_TYPE_WORKING_EF;
		file->ef_structure = SC_FILE_EF_LINEAR_VARIABLE;
		break;
	case 0x06:
		file->type = SC_FILE_TYPE_WORKING_EF;
		file->ef_structure = SC_FILE_EF_CYCLIC;
		break;
	case 0x38:
		file->type = SC_FILE_TYPE_DF;
		break;
	default:
		error(ctx, "invalid file type: 0x%02X\n", *p);
                return SC_ERROR_UNKNOWN_REPLY;
	}
        p += 2;
	if (file->type == SC_FILE_TYPE_DF) {
		file->acl[SC_AC_OP_LIST_FILES] = ac_to_acl(p[0] >> 4);
		file->acl[SC_AC_OP_DELETE] = ac_to_acl(p[1] >> 4);
		file->acl[SC_AC_OP_CREATE] = ac_to_acl(p[1] & 0x0F);
	} else { /* EF */
		file->acl[SC_AC_OP_READ] = ac_to_acl(p[0] >> 4);
		switch (file->ef_structure) {
		case SC_FILE_EF_TRANSPARENT:
			file->acl[SC_AC_OP_UPDATE] = ac_to_acl(p[0] & 0x0F);
			break;
		case SC_FILE_EF_LINEAR_FIXED:
		case SC_FILE_EF_LINEAR_VARIABLE:
			file->acl[SC_AC_OP_UPDATE] = ac_to_acl(p[0] & 0x0F);
			break;
		case SC_FILE_EF_CYCLIC:
#if 0
			/* FIXME */
			file->acl[SC_AC_OP_DECREASE] = ac_to_acl(p[0] & 0x0F);
#endif
			break;
		}
	}
	file->acl[SC_AC_OP_REHABILITATE] = ac_to_acl(p[2] >> 4);
	file->acl[SC_AC_OP_INVALIDATE] = ac_to_acl(p[2] & 0x0F);
	p += 3; /* skip ACs */
	if (*p++)
		file->status = SC_FILE_STATUS_ACTIVATED;
	else
                file->status = SC_FILE_STATUS_INVALIDATED;
        left = *p++;
	/* FIXME: CODEME */
	file->magic = SC_FILE_MAGIC;

	return 0;
}

static int flex_select_file(struct sc_card *card, const struct sc_path *path,
			     struct sc_file *file)
{
	int r, i;
	struct sc_apdu apdu;
        u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	const u8 *pathptr = path->value;
	size_t pathlen = path->len;
	int locked = 0;

	SC_FUNC_CALLED(card->ctx, 3);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xA4, 0, 0);
	apdu.resp = rbuf;
        apdu.resplen = sizeof(rbuf);
        apdu.p1 = apdu.p2 = 0;

	switch (path->type) {
	case SC_PATH_TYPE_PATH:
		if ((pathlen & 1) != 0) /* not divisible by 2 */
			return SC_ERROR_INVALID_ARGUMENTS;
		if (pathlen != 2 || memcmp(pathptr, "\x3F\x00", 2) != 0) {
			struct sc_path tmppath;

			locked = 1;
			r = sc_lock(card);
			SC_TEST_RET(card->ctx, r, "sc_lock() failed");
			if (memcmp(pathptr, "\x3F\x00", 2) != 0) {
				sc_format_path("I3F00", &tmppath);
				r = flex_select_file(card, &tmppath, NULL);
				if (r)
					sc_unlock(card);
				SC_TEST_RET(card->ctx, r, "Unable to select Master File (MF)");
			}
			while (pathlen > 2) {
				memcpy(tmppath.value, pathptr, 2);
				tmppath.len = 2;
				r = flex_select_file(card, &tmppath, NULL);
				if (r)
					sc_unlock(card);
				SC_TEST_RET(card->ctx, r, "Unable to select DF");
				pathptr += 2;
				pathlen -= 2;
			}
		}
                break;
	case SC_PATH_TYPE_DF_NAME:
		apdu.p1 = 0x04;
		break;
	case SC_PATH_TYPE_FILE_ID:
		if ((pathlen & 1) != 0)
			return SC_ERROR_INVALID_ARGUMENTS;
                break;
	}
	apdu.datalen = pathlen;
        apdu.data = pathptr;
        apdu.lc = pathlen;

	/* No need to get file information, if file is NULL or already
         * valid. */
#if 0
	if (file == NULL || sc_file_valid(file))
#endif
	if (file == NULL)
                apdu.resplen = 0;
	r = sc_transmit_apdu(card, &apdu);
	if (locked)
		sc_unlock(card);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	r = sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, r, "Card returned error");
#if 0
	if (file == NULL || sc_file_valid(file))
#endif
	if (file == NULL)
                return 0;

	if (apdu.resplen < 14)
		return SC_ERROR_UNKNOWN_REPLY;

	if (apdu.resp[0] == 0x6F) {
		error(card->ctx, "unsupported: card returned FCI\n");
		return SC_ERROR_UNKNOWN_REPLY; /* FIXME */
	}

	memset(file, 0, sizeof(struct sc_file));
	for (i = 0; i < SC_MAX_AC_OPS; i++)
		file->acl[i] = SC_AC_UNKNOWN;

	return parse_flex_sf_reply(card->ctx, apdu.resp, apdu.resplen, file);
}

static int flex_list_files(struct sc_card *card, u8 *buf, size_t buflen)
{
	struct sc_apdu apdu;
	u8 rbuf[4];
	int r;
	size_t count = 0;
	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xA8, 0, 0);
	apdu.cla = 0xF0;
	apdu.le = 4;
	apdu.resplen = 4;
	apdu.resp = rbuf;
	while (buflen > 2) {
		r = sc_transmit_apdu(card, &apdu);
		if (r)
			return r;
		if (apdu.sw1 == 0x6A && apdu.sw2 == 0x82)
			break;
		r = sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2);
		if (r)
			return r;
		if (apdu.resplen != 4) {
			error(card->ctx, "expected 4 bytes, got %d.\n", apdu.resplen);
			return SC_ERROR_ILLEGAL_RESPONSE;
		}
		memcpy(buf, rbuf + 2, 2);
		buf += 2;
		count += 2;
		buflen -= 2;
	}
	return count;
}

static int flex_delete_file(struct sc_card *card, const struct sc_path *path)
{
	int r;
	u8 sbuf[2];
	struct sc_apdu apdu;

	SC_FUNC_CALLED(card->ctx, 1);
	if (path->type != SC_PATH_TYPE_FILE_ID && path->len != 2) {
		error(card->ctx, "File type has to be SC_PATH_TYPE_FILE_ID\n");
		SC_FUNC_RETURN(card->ctx, 1, SC_ERROR_INVALID_ARGUMENTS);
	}
	sbuf[0] = path->value[0];
	sbuf[1] = path->value[1];
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE4, 0x00, 0x00);
	apdu.cla = 0xF0;	/* Override CLA byte */
	apdu.lc = 2;
	apdu.datalen = 2;
	apdu.data = sbuf;
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2);
}

static int acl_to_ac(unsigned int acl)
{
	int i;
	unsigned int acl_table[16] = {
		/* 0 */ SC_AC_NONE, SC_AC_CHV1, SC_AC_CHV2, SC_AC_PRO,
		/* 4 */ SC_AC_AUT, SC_AC_UNKNOWN, SC_AC_CHV1 | SC_AC_PRO,
		/* 7 */ SC_AC_CHV2 | SC_AC_PRO, SC_AC_CHV1 | SC_AC_AUT,
		/* 9 */ SC_AC_CHV2 | SC_AC_AUT, SC_AC_UNKNOWN, SC_AC_UNKNOWN,
		/* c */	SC_AC_UNKNOWN, SC_AC_UNKNOWN, SC_AC_UNKNOWN,
		/* f */ SC_AC_NEVER };
	
	for (i = 0; i < sizeof(acl_table)/sizeof(acl_table[0]); i++)
		if (acl == acl_table[i])
			return i;
	return -1;
}

static int encode_file_structure(struct sc_card *card, const struct sc_file *file,
				 u8 *buf, size_t *buflen)
{
	u8 *p = buf;
	int r, r2;

	p[0] = 0xFF;
	p[1] = 0xFF;
	p[2] = file->size >> 8;
	p[3] = file->size & 0xFF;
	p[4] = file->id >> 8;
	p[5] = file->id & 0xFF;
	if (file->type == SC_FILE_TYPE_DF)
		p[6] = 0x38;
	else
		switch (file->ef_structure) {
		case SC_FILE_EF_TRANSPARENT:
			p[6] = 0x01;
			break;
		case SC_FILE_EF_LINEAR_FIXED:
			p[6] = 0x02;
			break;
		case SC_FILE_EF_LINEAR_VARIABLE:
			p[6] = 0x04;
			break;
		case SC_FILE_EF_CYCLIC:
			p[6] = 0x06;
			break;
		default:
			return -1;
		}
	p[7] = 0xFF;	/* allow Decrease and Increase */
	if (file->type == SC_FILE_TYPE_DF) {
		r = acl_to_ac(file->acl[SC_AC_OP_LIST_FILES]);
		SC_TEST_RET(card->ctx, r, "Invalid ACL value");
		p[8] = (r & 0x0F) << 8;
		r = acl_to_ac(file->acl[SC_AC_OP_DELETE]);
		SC_TEST_RET(card->ctx, r, "Invalid ACL value");
		r2 = acl_to_ac(file->acl[SC_AC_OP_CREATE]);
		SC_TEST_RET(card->ctx, r2, "Invalid ACL value");
		p[9] = ((r & 0x0F) << 8) | (r2 & 0x0F);
		p[10] = 0;
	} else {
		r = acl_to_ac(file->acl[SC_AC_OP_READ]);
		SC_TEST_RET(card->ctx, r, "Invalid ACL value");
		r2 = acl_to_ac(file->acl[SC_AC_OP_UPDATE]);
		SC_TEST_RET(card->ctx, r2, "Invalid ACL value");
		p[8] = ((r & 0x0F) << 8) | (r2 & 0x0F);
		p[9] = 0; /* FIXME */
		r = acl_to_ac(file->acl[SC_AC_OP_INVALIDATE]);
		SC_TEST_RET(card->ctx, r, "Invalid ACL value");
		r2 = acl_to_ac(file->acl[SC_AC_OP_REHABILITATE]);
		SC_TEST_RET(card->ctx, r2, "Invalid ACL value");
		p[10] = ((r & 0x0F) << 8) | (r2 & 0x0F);
	}
	p[11] = (file->status & SC_FILE_STATUS_INVALIDATED) ? 0x00 : 0x01;
	if (file->type != SC_FILE_TYPE_DF &&
	    (file->ef_structure == SC_FILE_EF_LINEAR_FIXED ||
	     file->ef_structure == SC_FILE_EF_CYCLIC))
		p[12] = 0x04;
	else
		p[12] = 0x03;
	p[13] = p[14] = p[15] = 0;	/* FIXME */
	if (p[12] == 0x04) {
		p[16] = file->record_length;
		*buflen = 17;
	} else
		*buflen = 16;

	return 0;
}

static int flex_create_file(struct sc_card *card, const struct sc_file *file)
{
	u8 sbuf[18];
	size_t sendlen;
	int r, rec_nr;
	struct sc_apdu apdu;
	
	r = encode_file_structure(card, file, sbuf, &sendlen);
	if (r) {
		error(card->ctx, "File structure encoding failed.\n");
		return SC_ERROR_INVALID_ARGUMENTS;
	}
	if (file->type != SC_FILE_TYPE_DF && file->ef_structure != SC_FILE_EF_TRANSPARENT)
		rec_nr = file->record_count;
	else
		rec_nr = 0;
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE0, 0x00, rec_nr);
	apdu.cla = 0xF0;
	apdu.data = sbuf;
	apdu.datalen = sendlen;
	apdu.lc = sendlen;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	return sc_sw_to_errorcode(card, apdu.sw1, apdu.sw2);
}

static const struct sc_card_driver * sc_get_driver(void)
{
	const struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	flex_ops = *iso_drv->ops;
	flex_ops.match_card = flex_match_card;
	flex_ops.init = flex_init;
        flex_ops.finish = flex_finish;
	flex_ops.select_file = flex_select_file;
	flex_ops.list_files = flex_list_files;
	flex_ops.delete_file = flex_delete_file;
	flex_ops.create_file = flex_create_file;

        return &flex_drv;
}

#if 1
const struct sc_card_driver * sc_get_flex_driver(void)
{
	return sc_get_driver();
}
#endif
