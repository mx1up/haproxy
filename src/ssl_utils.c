/*
 * Utility functions for SSL:
 * Mostly generic functions that retrieve information from certificates
 *
 * Copyright (C) 2012 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 * Copyright (C) 2020 HAProxy Technologies, William Lallemand <wlallemand@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */


#include <haproxy/api.h>
#include <haproxy/buf-t.h>
#include <haproxy/chunk.h>
#include <haproxy/openssl-compat.h>
#include <haproxy/ssl_sock.h>

/* fill a buffer with the algorithm and size of a public key */
int cert_get_pkey_algo(X509 *crt, struct buffer *out)
{
	int bits = 0;
	int sig = TLSEXT_signature_anonymous;
	int len = -1;
	EVP_PKEY *pkey;

	pkey = X509_get_pubkey(crt);
	if (pkey) {
		bits = EVP_PKEY_bits(pkey);
		switch(EVP_PKEY_base_id(pkey)) {
		case EVP_PKEY_RSA:
			sig = TLSEXT_signature_rsa;
			break;
		case EVP_PKEY_EC:
			sig = TLSEXT_signature_ecdsa;
			break;
		case EVP_PKEY_DSA:
			sig = TLSEXT_signature_dsa;
			break;
		}
		EVP_PKEY_free(pkey);
	}

	switch(sig) {
	case TLSEXT_signature_rsa:
		len = chunk_printf(out, "RSA%d", bits);
		break;
	case TLSEXT_signature_ecdsa:
		len = chunk_printf(out, "EC%d", bits);
		break;
	case TLSEXT_signature_dsa:
		len = chunk_printf(out, "DSA%d", bits);
		break;
	default:
		return 0;
	}
	if (len < 0)
		return 0;
	return 1;
}

/* Extract a serial from a cert, and copy it to a chunk.
 * Returns 1 if serial is found and copied, 0 if no serial found and
 * -1 if output is not large enough.
 */
int ssl_sock_get_serial(X509 *crt, struct buffer *out)
{
	ASN1_INTEGER *serial;

	serial = X509_get_serialNumber(crt);
	if (!serial)
		return 0;

	if (out->size < serial->length)
		return -1;

	memcpy(out->area, serial->data, serial->length);
	out->data = serial->length;
	return 1;
}

/* Extract a cert to der, and copy it to a chunk.
 * Returns 1 if the cert is found and copied, 0 on der conversion failure
 * and -1 if the output is not large enough.
 */
int ssl_sock_crt2der(X509 *crt, struct buffer *out)
{
	int len;
	unsigned char *p = (unsigned char *) out->area;

	len = i2d_X509(crt, NULL);
	if (len <= 0)
		return 1;

	if (out->size < len)
		return -1;

	i2d_X509(crt, &p);
	out->data = len;
	return 1;
}


/* Copy Date in ASN1_UTCTIME format in struct buffer out.
 * Returns 1 if serial is found and copied, 0 if no valid time found
 * and -1 if output is not large enough.
 */
int ssl_sock_get_time(ASN1_TIME *tm, struct buffer *out)
{
	if (tm->type == V_ASN1_GENERALIZEDTIME) {
		ASN1_GENERALIZEDTIME *gentm = (ASN1_GENERALIZEDTIME *)tm;

		if (gentm->length < 12)
			return 0;
		if (gentm->data[0] != 0x32 || gentm->data[1] != 0x30)
			return 0;
		if (out->size < gentm->length-2)
			return -1;

		memcpy(out->area, gentm->data+2, gentm->length-2);
		out->data = gentm->length-2;
		return 1;
	}
	else if (tm->type == V_ASN1_UTCTIME) {
		ASN1_UTCTIME *utctm = (ASN1_UTCTIME *)tm;

		if (utctm->length < 10)
			return 0;
		if (utctm->data[0] >= 0x35)
			return 0;
		if (out->size < utctm->length)
			return -1;

		memcpy(out->area, utctm->data, utctm->length);
		out->data = utctm->length;
		return 1;
	}

	return 0;
}

/* Extract an entry from a X509_NAME and copy its value to an output chunk.
 * Returns 1 if entry found, 0 if entry not found, or -1 if output not large enough.
 */
int ssl_sock_get_dn_entry(X509_NAME *a, const struct buffer *entry, int pos,
                          struct buffer *out)
{
	X509_NAME_ENTRY *ne;
	ASN1_OBJECT *obj;
	ASN1_STRING *data;
	const unsigned char *data_ptr;
	int data_len;
	int i, j, n;
	int cur = 0;
	const char *s;
	char tmp[128];
	int name_count;

	name_count = X509_NAME_entry_count(a);

	out->data = 0;
	for (i = 0; i < name_count; i++) {
		if (pos < 0)
			j = (name_count-1) - i;
		else
			j = i;

		ne = X509_NAME_get_entry(a, j);
		obj = X509_NAME_ENTRY_get_object(ne);
		data = X509_NAME_ENTRY_get_data(ne);
		data_ptr = ASN1_STRING_get0_data(data);
		data_len = ASN1_STRING_length(data);
		n = OBJ_obj2nid(obj);
		if ((n == NID_undef) || ((s = OBJ_nid2sn(n)) == NULL)) {
			i2t_ASN1_OBJECT(tmp, sizeof(tmp), obj);
			s = tmp;
		}

		if (chunk_strcasecmp(entry, s) != 0)
			continue;

		if (pos < 0)
			cur--;
		else
			cur++;

		if (cur != pos)
			continue;

		if (data_len > out->size)
			return -1;

		memcpy(out->area, data_ptr, data_len);
		out->data = data_len;
		return 1;
	}

	return 0;

}

/*
 * Extract the DN in the specified format from the X509_NAME and copy result to a chunk.
 * Currently supports rfc2253 for returning LDAP V3 DNs.
 * Returns 1 if dn entries exist, 0 if no dn entry was found.
 */
int ssl_sock_get_dn_formatted(X509_NAME *a, const struct buffer *format, struct buffer *out)
{
	BIO *bio = NULL;
	int ret = 0;
	int data_len = 0;

	if (chunk_strcmp(format, "rfc2253") == 0) {
		bio = BIO_new(BIO_s_mem());
		if (bio == NULL)
			goto out;

		if (X509_NAME_print_ex(bio, a, 0, XN_FLAG_RFC2253) < 0)
			goto out;

		if ((data_len = BIO_read(bio, out->area, out->size)) <= 0)
			goto out;

		out->data = data_len;

		ret = 1;
	}
out:
	if (bio)
		BIO_free(bio);
	return ret;
}

/* Extract and format full DN from a X509_NAME and copy result into a chunk
 * Returns 1 if dn entries exits, 0 if no dn entry found or -1 if output is not large enough.
 */
int ssl_sock_get_dn_oneline(X509_NAME *a, struct buffer *out)
{
	X509_NAME_ENTRY *ne;
	ASN1_OBJECT *obj;
	ASN1_STRING *data;
	const unsigned char *data_ptr;
	int data_len;
	int i, n, ln;
	int l = 0;
	const char *s;
	char *p;
	char tmp[128];
	int name_count;


	name_count = X509_NAME_entry_count(a);

	out->data = 0;
	p = out->area;
	for (i = 0; i < name_count; i++) {
		ne = X509_NAME_get_entry(a, i);
		obj = X509_NAME_ENTRY_get_object(ne);
		data = X509_NAME_ENTRY_get_data(ne);
		data_ptr = ASN1_STRING_get0_data(data);
		data_len = ASN1_STRING_length(data);
		n = OBJ_obj2nid(obj);
		if ((n == NID_undef) || ((s = OBJ_nid2sn(n)) == NULL)) {
			i2t_ASN1_OBJECT(tmp, sizeof(tmp), obj);
			s = tmp;
		}
		ln = strlen(s);

		l += 1 + ln + 1 + data_len;
		if (l > out->size)
			return -1;
		out->data = l;

		*(p++)='/';
		memcpy(p, s, ln);
		p += ln;
		*(p++)='=';
		memcpy(p, data_ptr, data_len);
		p += data_len;
	}

	if (!out->data)
		return 0;

	return 1;
}


extern int ssl_client_crt_ref_index;

/*
 * This function fetches the SSL certificate for a specific connection (either
 * client certificate or server certificate depending on the cert_peer
 * parameter).
 * When trying to get the peer certificate from the server side, we first try to
 * use the dedicated SSL_get_peer_certificate function, but we fall back to
 * trying to get the client certificate reference that might have been stored in
 * the SSL structure's ex_data during the verification process.
 * Returns NULL in case of failure.
 */
X509* ssl_sock_get_peer_certificate(SSL *ssl)
{
	X509* cert;

	cert = SSL_get_peer_certificate(ssl);
	/* Get the client certificate reference stored in the SSL
	 * structure's ex_data during the verification process. */
	if (!cert) {
		cert = SSL_get_ex_data(ssl, ssl_client_crt_ref_index);
		if (cert)
			X509_up_ref(cert);
	}

	return cert;
}

/*
 * Take an OpenSSL version in text format and return a numeric openssl version
 * Return 0 if it failed to parse the version
 *
 *  https://www.openssl.org/docs/man1.1.1/man3/OPENSSL_VERSION_NUMBER.html
 *
 *  MNNFFPPS: major minor fix patch status
 *
 *  The status nibble has one of the values 0 for development, 1 to e for betas
 *  1 to 14, and f for release.
 *
 *  for example
 *
 *   0x0090821f     0.9.8zh
 *   0x1000215f     1.0.2u
 *   0x30000000     3.0.0-alpha17
 *   0x30000002     3.0.0-beta2
 *   0x3000000e     3.0.0-beta14
 *   0x3000000f     3.0.0
 */
unsigned int openssl_version_parser(const char *version)
{
	unsigned int numversion;
	unsigned int major = 0, minor = 0, fix = 0, patch = 0, status = 0;
	char *p, *end;

	p = (char *)version;

	if (!p || !*p)
		return 0;

	major = strtol(p, &end, 10);
	if (*end != '.' || major > 0xf)
		goto error;
	p = end + 1;

	minor = strtol(p, &end, 10);
	if (*end != '.' || minor > 0xff)
		goto error;
	p = end + 1;

	fix = strtol(p, &end, 10);
	if (fix > 0xff)
		goto error;
	p = end;

	if (!*p) {
		/* end of the string, that's a release */
		status = 0xf;
	} else if (*p == '-') {
		/* after the hyphen, only the beta will increment the status
		 * counter, all others versions will be considered as "dev" and
		 * does not increment anything */
		p++;

		if (!strncmp(p, "beta", 4)) {
			p += 4;
			status = strtol(p, &end, 10);
			if (status > 14)
				goto error;
		}
	} else {
		/* that's a patch release */
		patch = 1;

		/* add the value of each letter */
		while (*p) {
			patch += (*p & ~0x20) - 'A';
			p++;
		}
		status = 0xf;
	}

end:
	numversion =  ((major & 0xf) << 28) | ((minor & 0xff) << 20) | ((fix & 0xff) << 12) | ((patch & 0xff) << 4) | (status & 0xf);
	return numversion;

error:
	return 0;

}

/* Exclude GREASE (RFC8701) values from input buffer */
void exclude_tls_grease(char *input, int len, struct buffer *output)
{
	int ptr = 0;

	while (ptr < len - 1) {
		if (input[ptr] != input[ptr+1] || (input[ptr] & 0x0f) != 0x0a) {
			if (output->data <= output->size - 2) {
				memcpy(output->area + output->data, input + ptr, 2);
				output->data += 2;
			} else
				break;
		}
		ptr += 2;
	}
	if (output->size - output->data > 0 && len - ptr > 0)
		output->area[output->data++] = input[ptr];
}
