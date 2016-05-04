/*
 * SDU Protection Cryptographic Policy Set (includes a combination of
 * compression, Message Authentication Codes and Encryption mechanisms)
 *
 *    Ondrej Lichtner <ilichtner@fit.vutbr.cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/hashtable.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/random.h>
#include <crypto/hash.h>

#define RINA_PREFIX "sdup-crypto-ps-default"

#include "logs.h"
#include "policies.h"
#include "rds/rmem.h"
#include "sdup-crypto-ps-default.h"
#include "debug.h"

struct sdup_crypto_ps_default_data {
	struct crypto_blkcipher * tx_blkcipher;
	struct crypto_blkcipher * rx_blkcipher;

	struct crypto_shash * tx_shash;
	struct crypto_shash * rx_shash;

	struct crypto_comp * compress;
	unsigned int comp_scratch_size;
	char * comp_scratch;

	bool 		enable_encryption;
	bool		enable_decryption;

	/*next seq num to be used on tx*/
	unsigned int    tx_seq_num;
	/*highest received seq number (most significant bit of bitmap)*/
	unsigned int    rx_seq_num;
	/*bitmap of received seq numbers*/
	unsigned char * seq_bmap;
	unsigned int seq_bmap_len;
	unsigned int seq_win_size;

	string_t *      enc_key_tx;
	string_t *      enc_key_rx;
	string_t *      mac_key_tx;
	string_t *      mac_key_rx;

	string_t * 	encryption_cipher;
	string_t * 	message_digest;
	string_t * 	compress_alg;
};

static struct sdup_crypto_ps_default_data * priv_data_create(void)
{
	struct sdup_crypto_ps_default_data * data =
		rkmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->tx_blkcipher = NULL;
	data->rx_blkcipher = NULL;

	data->tx_shash = NULL;
	data->rx_shash = NULL;

	data->compress = NULL;
	data->comp_scratch = NULL;
	data->comp_scratch_size = 0;

	data->enable_decryption = false;
	data->enable_encryption = false;

	data->enc_key_tx = NULL;
	data->enc_key_rx = NULL;
	data->mac_key_tx = NULL;
	data->mac_key_rx = NULL;

	data->encryption_cipher = NULL;
	data->message_digest = NULL;
	data->compress_alg = NULL;

	data->tx_seq_num = 0;
	data->rx_seq_num = 0;
	data->seq_bmap = NULL;
	data->seq_win_size = 0;
	data->seq_bmap_len = 0;
	return data;
}

static void priv_data_destroy(struct sdup_crypto_ps_default_data * data)
{
	if (!data)
		return;

	if (data->tx_blkcipher)
		crypto_free_blkcipher(data->tx_blkcipher);
	if (data->rx_blkcipher)
		crypto_free_blkcipher(data->rx_blkcipher);

	if (data->tx_shash)
		crypto_free_shash(data->tx_shash);
	if (data->rx_shash)
		crypto_free_shash(data->rx_shash);

	if (data->compress)
		crypto_free_comp(data->compress);
	if (data->comp_scratch)
		rkfree(data->comp_scratch);

	if (data->enc_key_tx) {
		rkfree(data->enc_key_tx);
		data->enc_key_tx = NULL;
	}
	if (data->enc_key_rx) {
		rkfree(data->enc_key_rx);
		data->enc_key_rx = NULL;
	}

	if (data->mac_key_tx) {
		rkfree(data->mac_key_tx);
		data->mac_key_tx = NULL;
	}
	if (data->mac_key_rx) {
		rkfree(data->mac_key_rx);
		data->mac_key_rx = NULL;
	}

	if (data->compress_alg) {
		rkfree(data->compress_alg);
		data->compress_alg = NULL;
	}

	if (data->encryption_cipher) {
		rkfree(data->encryption_cipher);
		data->encryption_cipher = NULL;
	}

	if (data->message_digest) {
		rkfree(data->message_digest);
		data->message_digest = NULL;
	}

	if (data->seq_bmap) {
		rkfree(data->seq_bmap);
		data->seq_bmap = NULL;
	}

	rkfree(data);
}

static int add_padding(struct sdup_crypto_ps_default_data * priv_data,
		       struct pdu_ser * pdu)
{
	struct buffer * buf;
	unsigned int	buffer_size;
	unsigned int	padded_size;
	unsigned int	blk_size;
	char *		data;
	int i;

	if (!priv_data || !pdu){
		LOG_ERR("Encryption arguments not initialized!");
		return -1;
	}

	/* encryption and therefore padding is disabled */
	if (!priv_data->enable_encryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	blk_size = crypto_blkcipher_blocksize(priv_data->tx_blkcipher);
	buffer_size = buffer_length(buf);
	padded_size = buffer_size + blk_size - (buffer_size % blk_size);

	if (pdu_ser_tail_grow_gfp(pdu, padded_size - buffer_size)){
		LOG_ERR("Failed to grow ser PDU");
		return -1;
	}

	/* PADDING */
	data = buffer_data_rw(buf);
	for (i=padded_size-1; i>=buffer_size; i--){
		data[i] = padded_size - buffer_size;
	}

	return 0;
}

static int remove_padding(struct sdup_crypto_ps_default_data * priv_data,
			  struct pdu_ser * pdu)
{
	struct buffer *	buf;
	const char *	data;
	unsigned int	len;
	unsigned int	pad_len;
	int		i;

	if (!priv_data || !pdu){
		LOG_ERR("Encryption arguments not initialized!");
		return -1;
	}

	/* decryption and therefore padding is disabled */
	if (!priv_data->enable_decryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	data = buffer_data_ro(buf);
	len = buffer_length(buf);
	pad_len = data[len-1];

	//check padding
	for (i=len-1; i >= len-pad_len; --i){
		if (data[i] != pad_len){
			LOG_ERR("Padding check failed!");
			return -1;
		}
	}

	//remove padding
	if (pdu_ser_tail_shrink_gfp(pdu, pad_len)){
		LOG_ERR("Failed to shrink serialized PDU");
		return -1;
	}

	return 0;
}

static int encrypt(struct sdup_crypto_ps_default_data * priv_data,
		   struct pdu_ser * pdu)
{
	struct blkcipher_desc	desc;
	struct scatterlist	sg;
	struct buffer *		buf;
	unsigned int		buffer_size;
	void *			data;
	char *                  iv;
	unsigned int		ivsize;

	if (!priv_data || !pdu){
		LOG_ERR("Encryption arguments not initialized!");
		return -1;
	}

	/* encryption is disabled */
	if (priv_data->tx_blkcipher == NULL ||
	    !priv_data->enable_encryption)
		return 0;

	desc.flags = 0;
	desc.tfm = priv_data->tx_blkcipher;

	buf = pdu_ser_buffer(pdu);
	buffer_size = buffer_length(buf);
	data = buffer_data_rw(buf);

	iv = NULL;
	ivsize = crypto_blkcipher_ivsize(priv_data->tx_blkcipher);
	if (ivsize) {
		iv = rkzalloc(ivsize, GFP_KERNEL);
		if (!iv){
			LOG_ERR("IV allocation failed!");
		}
		get_random_bytes(iv, ivsize);
	}

	sg_init_one(&sg, data, buffer_size);

	if (iv)
		crypto_blkcipher_set_iv(priv_data->tx_blkcipher, iv, ivsize);

	if (crypto_blkcipher_encrypt(&desc, &sg, &sg, buffer_size)) {
		LOG_ERR("Encryption failed!");
		if (iv)
			rkfree(iv);
		return -1;
	}

	if (pdu_ser_head_grow_gfp(GFP_ATOMIC, pdu, ivsize)){
		LOG_ERR("Failed to grow ser PDU for IV");
		if (iv)
			rkfree(iv);
		return -1;
	}

	data = buffer_data_rw(buf);
	memcpy(data, iv, ivsize);

	if (iv)
		rkfree(iv);
	return 0;
}

static int decrypt(struct sdup_crypto_ps_default_data * priv_data,
		   struct pdu_ser * pdu)
{
	struct blkcipher_desc	desc;
	struct scatterlist	sg;
	struct buffer *		buf;
	unsigned int		buffer_size;
	void *			data;
	char *                  iv;
	unsigned int		ivsize;

	if (!priv_data || !pdu){
		LOG_ERR("Failed decryption");
		return -1;
	}

	/* decryption is disabled */
	if (priv_data->rx_blkcipher == NULL ||
	    !priv_data->enable_decryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	data = buffer_data_rw(buf);

	iv = NULL;
	ivsize = crypto_blkcipher_ivsize(priv_data->rx_blkcipher);
	if (ivsize) {
		iv = rkzalloc(ivsize, GFP_KERNEL);
		if (!iv){
			LOG_ERR("IV allocation failed!");
		}
		memcpy(iv, data, ivsize);

		if (pdu_ser_head_shrink_gfp(GFP_ATOMIC, pdu, ivsize)){
			LOG_ERR("Failed to shrink ser PDU by IV");
			if (iv)
				rkfree(iv);
			return -1;
		}
	}

	desc.flags = 0;
	desc.tfm = priv_data->rx_blkcipher;
	buf = pdu_ser_buffer(pdu);
	buffer_size = buffer_length(buf);
	data = buffer_data_rw(buf);

	sg_init_one(&sg, data, buffer_size);

	if (iv)
		crypto_blkcipher_set_iv(priv_data->rx_blkcipher, iv, ivsize);

	if (crypto_blkcipher_decrypt(&desc, &sg, &sg, buffer_size)) {
		LOG_ERR("Decryption failed!");
		if (iv)
			rkfree(iv);
		return -1;
	}

	if (iv)
		rkfree(iv);
	return 0;
}

static int add_hmac(struct sdup_crypto_ps_default_data * priv_data,
		    struct pdu_ser * pdu)
{
	struct buffer *		buf;
	unsigned int		buffer_size;
	void *			data;
	unsigned int		digest_size;
	SHASH_DESC_ON_STACK(shash, priv_data->tx_shash);

	if (!priv_data || !pdu){
		LOG_ERR("HMAC arguments not initialized!");
		return -1;
	}

	/* encryption is disabled so hmac is disabled*/
	if (priv_data->tx_shash == NULL ||
	    !priv_data->enable_encryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	buffer_size = buffer_length(buf);
	digest_size = crypto_shash_digestsize(priv_data->tx_shash);

	if (pdu_ser_tail_grow_gfp(pdu, digest_size)){
		LOG_ERR("Failed to grow ser PDU for HMAC");
		return -1;
	}

	data = buffer_data_rw(buf);

	shash->flags = 0;
	shash->tfm = priv_data->tx_shash;

	if (crypto_shash_digest(shash, data, buffer_size, data+buffer_size)) {
		LOG_ERR("HMAC calculation failed!");
		return -1;
	}

	return 0;
}

static int check_hmac(struct sdup_crypto_ps_default_data * priv_data,
		      struct pdu_ser * pdu)
{
	struct buffer *		buf;
	unsigned int		buffer_size;
	void *			data;
	unsigned int		digest_size;
	char *			verify_digest;
	SHASH_DESC_ON_STACK(shash, priv_data->tx_shash);

	if (!priv_data || !pdu){
		LOG_ERR("HMAC arguments not initialized!");
		return -1;
	}

	/* decryption is disabled so hmac is disabled*/
	if (priv_data->rx_shash == NULL ||
	    !priv_data->enable_decryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	buffer_size = buffer_length(buf);
	data = buffer_data_rw(buf);

	digest_size = crypto_shash_digestsize(priv_data->tx_shash);
	verify_digest = rkzalloc(digest_size, GFP_KERNEL);
	if(!verify_digest){
		LOG_ERR("HMAC allocation failed!");
		return -1;
	}

	shash->flags = 0;
	shash->tfm = priv_data->rx_shash;

	if (crypto_shash_digest(shash, data, buffer_size-digest_size, verify_digest)) {
		LOG_ERR("HMAC calculation failed!");
		rkfree(verify_digest);
		return -1;
	}

	if (memcmp(verify_digest, data+buffer_size-digest_size, digest_size)){
		LOG_ERR("HMAC verification FAILED!");
		rkfree(verify_digest);
		return -1;
	}

	if (pdu_ser_tail_shrink_gfp(pdu, digest_size)){
		LOG_ERR("Failed to shrink serialized PDU");
		rkfree(verify_digest);
		return -1;
	}

	rkfree(verify_digest);
	return 0;
}

static int compress(struct sdup_crypto_ps_default_data * priv_data,
		    struct pdu_ser * pdu)
{
	struct buffer *		buf;
	unsigned int		buffer_size;
	void *			data;
	unsigned int		compressed_size;
	char *                  compressed_data;
	char *                  tmp;
	int err;

	if (!priv_data || !pdu){
		LOG_ERR("Compression arguments not initialized!");
		return -1;
	}

	/* encryption is disabled so compression is disabled*/
	if (priv_data->compress == NULL ||
	    !priv_data->enable_encryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	buffer_size = buffer_length(buf);
	data = buffer_data_rw(buf);

	compressed_size = priv_data->comp_scratch_size;
	compressed_data = priv_data->comp_scratch;

	err = crypto_comp_compress(priv_data->compress,
				   data, buffer_size,
				   compressed_data, &compressed_size);

	if (err){
		LOG_ERR("Failed compression!");
		return -1;
	}

	tmp = rkzalloc(compressed_size, GFP_KERNEL);
	if (!tmp){
		LOG_ERR("Failed allocation of new buffer!");
		return -1;
	}
	memcpy(tmp, compressed_data, compressed_size);

	buffer_assign(buf, tmp, compressed_size);

	return 0;
}

static int decompress(struct sdup_crypto_ps_default_data * priv_data,
		      u_int32_t max_pdu_size,
		      struct pdu_ser * pdu)
{
	struct buffer *		buf;
	unsigned int		buffer_size;
	void *			data;
	void *			decompressed_data;
	unsigned int		decompressed_size;
	int err;

	if (!priv_data || !pdu){
		LOG_ERR("Decompression arguments not initialized!");
		return -1;
	}

	/* decryption is disabled so decompression is disabled*/
	if (priv_data->compress == NULL ||
	    !priv_data->enable_decryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	buffer_size = buffer_length(buf);
	data = buffer_data_rw(buf);

	decompressed_size = max_pdu_size;
	decompressed_data = rkzalloc(decompressed_size, GFP_KERNEL);
	if (!decompressed_data){
		LOG_ERR("Failed to alloc data for compression");
		return -1;
	}

	err = crypto_comp_decompress(priv_data->compress,
				     data, buffer_size,
				     decompressed_data, &decompressed_size);

	if (err){
		LOG_ERR("Failed decompression!");
		rkfree(decompressed_data);
		return -1;
	}

	buffer_assign(buf, decompressed_data, decompressed_size);

	return 0;
}

static int add_seq_num(struct sdup_crypto_ps_default_data * priv_data,
		       struct pdu_ser * pdu)
{
	struct buffer * buf;
	char *		data;

	if (!priv_data || !pdu){
		LOG_ERR("Encryption arguments not initialized!");
		return -1;
	}

	/* encryption and therefore sequence numbers are disabled */
	if (!priv_data->enable_encryption)
		return 0;

	buf = pdu_ser_buffer(pdu);

	if (pdu_ser_head_grow_gfp(GFP_ATOMIC,
				  pdu,
				  sizeof(priv_data->tx_seq_num))){
		LOG_ERR("Failed to grow ser PDU");
		return -1;
	}

	data = buffer_data_rw(buf);
	memcpy(data, &priv_data->tx_seq_num, sizeof(priv_data->tx_seq_num));

	LOG_DBG("Added sequence number %u", priv_data->tx_seq_num);

	priv_data->tx_seq_num += 1;

	return 0;
}

static int del_seq_num(struct sdup_crypto_ps_default_data * priv_data,
		       struct pdu_ser * pdu)
{
	struct buffer * buf;
	char *		data;
	unsigned int    seq_num;
	long int	min_seq_num;
	int		i, shift_length;
	unsigned char * bmap;
	unsigned int	bit_pos, byte_pos;

	if (!priv_data || !pdu){
		LOG_ERR("Encryption arguments not initialized!");
		return -1;
	}

	/* decryption and therefore sequence numbers are disabled */
	if (!priv_data->enable_decryption)
		return 0;

	buf = pdu_ser_buffer(pdu);
	data = buffer_data_rw(buf);

	memcpy(&seq_num, data, sizeof(priv_data->tx_seq_num));

	if (pdu_ser_head_shrink_gfp(GFP_ATOMIC,
				    pdu,
				    sizeof(priv_data->tx_seq_num))){
		LOG_ERR("Failed to grow ser PDU");
		return -1;
	}

	LOG_DBG("Got sequence number %u", seq_num);

	if (priv_data->seq_win_size == 0)
		return 0;

	min_seq_num = (long int)priv_data->rx_seq_num - priv_data->seq_win_size;
	if (seq_num < min_seq_num){
		LOG_ERR("Sequence number %u is too old", seq_num);
		return -1;
	} else {
		//shift bitmap to right
		shift_length = seq_num - priv_data->rx_seq_num;
		bmap = priv_data->seq_bmap;
		while (shift_length-- > 0){
			for (i = priv_data->seq_bmap_len - 1; i>=0; i--){
				bmap[i] >>= 1;
				if (i>0 && bmap[i-1] & 1){
					bmap[i] |= 1<<7;
				}
			}
		}

		//set new highest received sequence number
		if (seq_num > priv_data->rx_seq_num)
			priv_data->rx_seq_num = seq_num;

		bit_pos = priv_data->rx_seq_num - seq_num;
		byte_pos = bit_pos / 8;
		bit_pos = 7 - (bit_pos % 8);

		//check bitmap for duplicate sequence number
		if (bmap[byte_pos] & (1 << bit_pos)){
			LOG_ERR("Sequence number %u already received.",
				seq_num);
			return -1;
		} else {
			bmap[byte_pos] |= 1 << bit_pos;
			return 0;
		}
	}
	return 0;
}

int default_sdup_apply_crypto(struct sdup_crypto_ps * ps,
			      struct pdu_ser * pdu)
{
	int result = 0;
	struct sdup_crypto_ps_default_data * priv_data = ps->priv;


	result = compress(priv_data, pdu);
	if (result)
		return result;

	result = add_seq_num(priv_data, pdu);
	if (result)
		return result;

	result = add_hmac(priv_data, pdu);
	if (result)
		return result;

	result = add_padding(priv_data, pdu);
	if (result)
		return result;

	result = encrypt(priv_data, pdu);
	if (result)
		return result;

	return result;
}
EXPORT_SYMBOL(default_sdup_apply_crypto);

int default_sdup_remove_crypto(struct sdup_crypto_ps * ps,
			       struct pdu_ser * pdu)
{
	int result = 0;
	struct sdup_crypto_ps_default_data * priv_data = ps->priv;
	struct sdup_port * port = ps->dm;
	struct dt_cons * dt_cons = port->dt_cons;

	result = decrypt(priv_data, pdu);
	if (result)
		return result;

	result = remove_padding(priv_data, pdu);
	if (result)
		return result;

	result = check_hmac(priv_data, pdu);
	if (result)
		return result;

	result = del_seq_num(priv_data, pdu);
	if (result)
		return result;

	result = decompress(priv_data, dt_cons->max_pdu_size, pdu);
	if (result)
		return result;

	return result;
}
EXPORT_SYMBOL(default_sdup_remove_crypto);


int default_sdup_update_crypto_state(struct sdup_crypto_ps * ps,
				     struct sdup_crypto_state * state)
{
	struct sdup_crypto_ps_default_data * priv_data;

	if (!ps || !state) {
		LOG_ERR("Bogus input parameters passed");
		return -1;
	}

	if (!state->encrypt_key_tx) {
		LOG_ERR("Bogus tx encryption key passed");
		return -1;
	}
	if (!state->encrypt_key_rx) {
		LOG_ERR("Bogus rx encryption key passed");
		return -1;
	}

	if (!state->mac_key_tx) {
		LOG_ERR("Bogus tx mac key passed");
		return -1;
	}
	if (!state->mac_key_rx) {
		LOG_ERR("Bogus rx mac key passed");
		return -1;
	}

	priv_data = ps->priv;

	if (!priv_data->tx_blkcipher) {
		LOG_ERR("TX Block cipher is not set for N-1 port %d",
			ps->dm->port_id);
		return -1;
	}

	if (!priv_data->rx_blkcipher) {
		LOG_ERR("RX Block cipher is not set for N-1 port %d",
			ps->dm->port_id);
		return -1;
	}

	if (!priv_data->tx_shash) {
		LOG_ERR("TX shash is not set for N-1 port %d",
			ps->dm->port_id);
		return -1;
	}

	if (!priv_data->rx_shash) {
		LOG_ERR("RX shash is not set for N-1 port %d",
			ps->dm->port_id);
		return -1;
	}

	if (!priv_data->enable_decryption && state->enable_crypto_rx){
		priv_data->enable_decryption = state->enable_crypto_rx;
		if (crypto_blkcipher_setkey(priv_data->rx_blkcipher,
					    buffer_data_ro(state->encrypt_key_rx),
					    buffer_length(state->encrypt_key_rx))) {
			LOG_ERR("Could not set decryption key for N-1 port %d",
				ps->dm->port_id);
			return -1;
		}

		if (crypto_shash_setkey(priv_data->rx_shash,
					buffer_data_ro(state->mac_key_rx),
					buffer_length(state->mac_key_rx))) {
			LOG_ERR("Could not set rx mac key for N-1 port %d",
				ps->dm->port_id);
			return -1;
		}
	}
	if (!priv_data->enable_encryption && state->enable_crypto_tx){
		priv_data->enable_encryption = state->enable_crypto_tx;
		if (crypto_blkcipher_setkey(priv_data->tx_blkcipher,
					    buffer_data_ro(state->encrypt_key_tx),
					    buffer_length(state->encrypt_key_tx))) {
			LOG_ERR("Could not set encryption key for N-1 port %d",
				ps->dm->port_id);
			return -1;
		}

		if (crypto_shash_setkey(priv_data->tx_shash,
					buffer_data_ro(state->mac_key_tx),
					buffer_length(state->mac_key_tx))) {
			LOG_ERR("Could not set tx mac key for N-1 port %d",
				ps->dm->port_id);
			return -1;
		}
	}

	LOG_DBG("Crypto rx enabled state: %d", state->enable_crypto_rx);
	LOG_DBG("Crypto tx enabled state: %d", state->enable_crypto_tx);

	return 0;
}
EXPORT_SYMBOL(default_sdup_update_crypto_state);

struct ps_base * sdup_crypto_ps_default_create(struct rina_component * component)
{
	struct dup_config_entry * conf;
	struct sdup_comp * sdup_comp;
	struct sdup_crypto_ps * ps;
	struct sdup_port * sdup_port;
	struct sdup_crypto_ps_default_data * data;
	struct policy_parm * parameter;
	const string_t * aux;

	sdup_comp = sdup_comp_from_component(component);
	if (!sdup_comp)
		return NULL;

	sdup_port = sdup_comp->parent;
	if (!sdup_port)
		return NULL;

	conf = sdup_port->conf;
	if (!conf)
		return NULL;

	ps = rkzalloc(sizeof(*ps), GFP_KERNEL);
	if (!ps)
		return NULL;

	data = priv_data_create();
	if (!data) {
		rkfree(ps);
		return NULL;
	}

	ps->dm          = sdup_port;
	ps->priv        = data;

	/* Parse policy parameters */
	if (conf->crypto_policy) {
		parameter = policy_param_find(conf->crypto_policy,
					      "encryptAlg");
		if (!parameter) {
			LOG_ERR("Could not find 'encryptAlg' in crypto policy");
			rkfree(ps);
			priv_data_destroy(data);
			return NULL;
		}

		aux = policy_param_value(parameter);
		if (string_cmp(aux, "AES128") == 0 ||
		    string_cmp(aux, "AES256") == 0) {
			if (string_dup("cbc(aes)",
				       &data->encryption_cipher)) {
				LOG_ERR("Problems copying 'encryptAlg' value");
				rkfree(ps);
				priv_data_destroy(data);
				return NULL;
			}
			LOG_DBG("Encryption cipher is %s",
				data->encryption_cipher);
		} else {
			LOG_DBG("Unsupported encryption cipher %s", aux);
		}

		parameter = policy_param_find(conf->crypto_policy,
					      "macAlg");
		if (!parameter) {
			LOG_ERR("Could not find 'macAlg' in crypto policy");
			rkfree(ps);
			priv_data_destroy(data);
			return NULL;
		}

		aux = policy_param_value(parameter);
		if (string_cmp(aux, "SHA256") == 0) {
			if (string_dup("hmac(sha256)", &data->message_digest)) {
				LOG_ERR("Problems copying 'digest' value");
				rkfree(ps);
				priv_data_destroy(data);
				return NULL;
			}
			LOG_DBG("Message digest is %s", data->message_digest);
		} else if (string_cmp(aux, "MD5") == 0) {
			if (string_dup("hmac(md5)", &data->message_digest)) {
				LOG_ERR("Problems copying 'digest' value)");
				rkfree(ps);
				priv_data_destroy(data);
				return NULL;
			}
			LOG_DBG("Message digest is %s", data->message_digest);
		} else {
			LOG_DBG("Unsupported message digest %s", aux);
		}

		parameter = policy_param_find(conf->crypto_policy,
					      "compressAlg");
		if (!parameter) {
			LOG_ERR("Could not find 'compressAlg' in crypto policy");
			rkfree(ps);
			priv_data_destroy(data);
			return NULL;
		}

		aux = policy_param_value(parameter);
		if (string_cmp(aux, "deflate") == 0) {
			if (string_dup("deflate", &data->compress_alg)) {
				LOG_ERR("Problems copying 'compressAlg' value");
				rkfree(ps);
				priv_data_destroy(data);
				return NULL;
			}
			LOG_DBG("Compress algorighm is %s", data->compress_alg);
		} else {
			LOG_DBG("Unsupported compress algorithm %s", aux);
		}

		parameter = policy_param_find(conf->crypto_policy,
					      "seq_win_size");
		if (!parameter) {
			data->seq_win_size = 0;
		} else {
			aux = policy_param_value(parameter);
			if (kstrtoint(aux, 10, &data->seq_win_size)) {
				LOG_ERR("Problems copying 'seq_win_size' value");
				rkfree(ps);
				priv_data_destroy(data);
				return NULL;
			}

			data->seq_bmap_len = data->seq_win_size / 8;
			if (data->seq_win_size % 8) {
				data->seq_bmap_len += 1;
			}

			data->seq_bmap = rkzalloc(data->seq_bmap_len,
						  GFP_KERNEL);
			if (!data->seq_bmap) {
				LOG_ERR("Problems allocating sequence number window.");
				rkfree(ps);
				priv_data_destroy(data);
				return NULL;
			}

			LOG_DBG("Sequence number window size is %d",
				data->seq_win_size);
		}

	} else {
		LOG_ERR("Bogus configuration passed");
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}

	/* Instantiate block cipher */
	data->tx_blkcipher = crypto_alloc_blkcipher(data->encryption_cipher,
						    0,0);
	data->rx_blkcipher = crypto_alloc_blkcipher(data->encryption_cipher,
						    0,0);

	data->tx_shash = crypto_alloc_shash(data->message_digest, 0,0);
	data->rx_shash = crypto_alloc_shash(data->message_digest, 0,0);

	data->compress = crypto_alloc_comp(data->compress_alg, 0,0);

	if (IS_ERR(data->tx_blkcipher)) {
		LOG_ERR("could not allocate tx blkcipher handle for %s\n",
			data->encryption_cipher);
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}
	if (IS_ERR(data->rx_blkcipher)) {
		LOG_ERR("could not allocate rx blkcipher handle for %s\n",
			data->encryption_cipher);
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}

	if (IS_ERR(data->tx_shash)) {
		LOG_ERR("could not allocate tx shash handle for %s\n",
			data->message_digest);
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}
	if (IS_ERR(data->rx_shash)) {
		LOG_ERR("could not allocate rx shash handle for %s\n",
			data->message_digest);
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}

	if (IS_ERR(data->compress)) {
		LOG_ERR("could not allocate compress handle for %s\n",
			data->compress_alg);
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}

	data->comp_scratch_size = sdup_port->dt_cons->max_pdu_size +
		MAX_COMP_INFLATION;
	data->comp_scratch = rkzalloc(data->comp_scratch_size, GFP_KERNEL);
	if (!data->comp_scratch) {
		LOG_ERR("could not allocate scratch space for compression");
		rkfree(ps);
		priv_data_destroy(data);
		return NULL;
	}

	/* SDUP policy functions*/
	ps->sdup_apply_crypto		= default_sdup_apply_crypto;
	ps->sdup_remove_crypto		= default_sdup_remove_crypto;
	ps->sdup_update_crypto_state	= default_sdup_update_crypto_state;

	return &ps->base;
}
EXPORT_SYMBOL(sdup_crypto_ps_default_create);

void sdup_crypto_ps_default_destroy(struct ps_base * bps)
{
	struct sdup_crypto_ps_default_data * data;
	struct sdup_crypto_ps *ps;

	ps = container_of(bps, struct sdup_crypto_ps, base);
	data = ps->priv;

	if (bps) {
		if (data)
			priv_data_destroy(data);
		rkfree(ps);
	}
}
EXPORT_SYMBOL(sdup_crypto_ps_default_destroy);
