/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2010 Intel Corporation.
 * Copyright © 2008 Nick Andrew <nick@nick-andrew.net>
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to:
 *
 *   Free Software Foundation, Inc.
 *   51 Franklin Street, Fifth Floor,
 *   Boston, MA 02110-1301 USA
 */

#define _BSD_SOURCE
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "openconnect-internal.h"

/*
 * Data packets are encapsulated in the SSL stream as follows:
 *
 * 0000: Magic "STF\x1"
 * 0004: Big-endian 16-bit length (not including 8-byte header)
 * 0006: Byte packet type (see openconnect-internal.h)
 * 0008: data payload
 */

static char data_hdr[8] = {
	'S', 'T', 'F', 1,
	0, 0,		/* Length */
	AC_PKT_DATA,	/* Type */
	0		/* Unknown */
};

static struct pkt keepalive_pkt = {
	.hdr = { 'S', 'T', 'F', 1, 0, 0, AC_PKT_KEEPALIVE, 0 },
};

static struct pkt dpd_pkt = {
	.hdr = { 'S', 'T', 'F', 1, 0, 0, AC_PKT_DPD_OUT, 0 },
};

static struct pkt dpd_resp_pkt = {
	.hdr = { 'S', 'T', 'F', 1, 0, 0, AC_PKT_DPD_RESP, 0 },
};


static int start_cstp_connection(struct openconnect_info *vpninfo)
{
	char buf[65536];
	int i;
	int retried = 0, sessid_found = 0;
	struct vpn_option **next_dtls_option = &vpninfo->dtls_options;
	struct vpn_option **next_cstp_option = &vpninfo->cstp_options;
	struct vpn_option *old_cstp_opts = vpninfo->cstp_options;
	struct vpn_option *old_dtls_opts = vpninfo->dtls_options;
	const char *old_addr = vpninfo->vpn_addr;
	const char *old_netmask = vpninfo->vpn_netmask;
	const char *old_addr6 = vpninfo->vpn_addr6;
	const char *old_netmask6 = vpninfo->vpn_netmask6;
	struct split_include *inc;

	/* Clear old options which will be overwritten */
	vpninfo->vpn_addr = vpninfo->vpn_netmask = NULL;
	vpninfo->vpn_addr6 = vpninfo->vpn_netmask6 = NULL;
	vpninfo->cstp_options = vpninfo->dtls_options = NULL;
	vpninfo->vpn_domain = vpninfo->vpn_proxy_pac = NULL;
	vpninfo->banner = NULL;

	for (i=0; i<3; i++)
		vpninfo->vpn_dns[i] = vpninfo->vpn_nbns[i] = NULL;

	for (inc = vpninfo->split_includes; inc; ) {
		struct split_include *next = inc->next;
		free(inc);
		inc = next;
	}
	for (inc = vpninfo->split_excludes; inc; ) {
		struct split_include *next = inc->next;
		free(inc);
		inc = next;
	}
	vpninfo->split_includes = vpninfo->split_excludes = NULL;

	/* Create (new) random master key for DTLS connection, if needed */
	if (vpninfo->dtls_times.last_rekey + vpninfo->dtls_times.rekey <
	    time(NULL) + 300 &&
	    RAND_bytes(vpninfo->dtls_secret, sizeof(vpninfo->dtls_secret)) != 1) {
		fprintf(stderr, _("Failed to initialise DTLS secret\n"));
		exit(1);
	}

 retry:
	openconnect_SSL_printf(vpninfo->https_ssl, "CONNECT /CSCOSSLC/tunnel HTTP/1.1\r\n");
	openconnect_SSL_printf(vpninfo->https_ssl, "Host: %s\r\n", vpninfo->hostname);
	openconnect_SSL_printf(vpninfo->https_ssl, "User-Agent: %s\r\n", vpninfo->useragent);
	openconnect_SSL_printf(vpninfo->https_ssl, "Cookie: webvpn=%s\r\n", vpninfo->cookie);
	openconnect_SSL_printf(vpninfo->https_ssl, "X-CSTP-Version: 1\r\n");
	openconnect_SSL_printf(vpninfo->https_ssl, "X-CSTP-Hostname: %s\r\n", vpninfo->localname);
	if (vpninfo->deflate)
		openconnect_SSL_printf(vpninfo->https_ssl, "X-CSTP-Accept-Encoding: deflate;q=1.0\r\n");
	openconnect_SSL_printf(vpninfo->https_ssl, "X-CSTP-MTU: %d\r\n", vpninfo->mtu);
	openconnect_SSL_printf(vpninfo->https_ssl, "X-CSTP-Address-Type: %s\r\n",
			       vpninfo->disable_ipv6?"IPv4":"IPv6,IPv4");
	openconnect_SSL_printf(vpninfo->https_ssl, "X-DTLS-Master-Secret: ");
	for (i = 0; i < sizeof(vpninfo->dtls_secret); i++)
		openconnect_SSL_printf(vpninfo->https_ssl, "%02X", vpninfo->dtls_secret[i]);
	openconnect_SSL_printf(vpninfo->https_ssl, "\r\nX-DTLS-CipherSuite: %s\r\n\r\n",
			       vpninfo->dtls_ciphers?:"AES256-SHA:AES128-SHA:DES-CBC3-SHA:DES-CBC-SHA");

	if (openconnect_SSL_gets(vpninfo->https_ssl, buf, 65536) < 0) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Error fetching HTTPS response\n"));
		if (!retried) {
			retried = 1;
			openconnect_close_https(vpninfo);

			if (openconnect_open_https(vpninfo)) {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Failed to open HTTPS connection to %s\n"),
					     vpninfo->hostname);
				exit(1);
			}
			goto retry;
		}
		return -EINVAL;
	}

	if (strncmp(buf, "HTTP/1.1 200 ", 13)) {
		if (!strncmp(buf, "HTTP/1.1 503 ", 13)) {
			/* "Service Unavailable. Why? */
			const char *reason = "<unknown>";
			while ((i = openconnect_SSL_gets(vpninfo->https_ssl, buf, sizeof(buf)))) {
				if (!strncmp(buf, "X-Reason: ", 10)) {
					reason = buf + 10;
					break;
				}
			}
			vpn_progress(vpninfo, PRG_ERR,
				     _("VPN service unavailable; reason: %s\n"),
				     reason);
			return -EINVAL;
		}
		vpn_progress(vpninfo, PRG_ERR,
			     _("Got inappropriate HTTP CONNECT response: %s\n"),
			     buf);
		if (!strncmp(buf, "HTTP/1.1 401 ", 13))
			exit(2);
		return -EINVAL;
	}

	vpn_progress(vpninfo, PRG_INFO, _("Got CONNECT response: %s\n"), buf);

	/* We may have advertised it, but we only do it if the server agrees */
	vpninfo->deflate = 0;

	while ((i = openconnect_SSL_gets(vpninfo->https_ssl, buf, sizeof(buf)))) {
		struct vpn_option *new_option;
		char *colon = strchr(buf, ':');
		if (!colon)
			continue;

		*colon = 0;
		colon++;
		if (*colon == ' ')
			colon++;

		if (strncmp(buf, "X-DTLS-", 7) &&
		    strncmp(buf, "X-CSTP-", 7))
			continue;

		new_option = malloc(sizeof(*new_option));
		if (!new_option) {
			vpn_progress(vpninfo, PRG_ERR, _("No memory for options\n"));
			return -ENOMEM;
		}
		new_option->option = strdup(buf);
		new_option->value = strdup(colon);
		new_option->next = NULL;

		if (!new_option->option || !new_option->value) {
			vpn_progress(vpninfo, PRG_ERR, _("No memory for options\n"));
			return -ENOMEM;
		}

		vpn_progress(vpninfo, PRG_TRACE, "%s: %s\n", buf, colon);

		if (!strncmp(buf, "X-DTLS-", 7)) {
			*next_dtls_option = new_option;
			next_dtls_option = &new_option->next;

			if (!strcmp(buf + 7, "Session-ID")) {
				if (strlen(colon) != 64) {
					vpn_progress(vpninfo, PRG_ERR,
						     _("X-DTLS-Session-ID not 64 characters; is: \"%s\"\n"),
						     colon);
					vpninfo->dtls_attempt_period = 0;
					return -EINVAL;
				}
				for (i = 0; i < 64; i += 2)
					vpninfo->dtls_session_id[i/2] = unhex(colon + i);
				sessid_found = 1;
				time(&vpninfo->dtls_times.last_rekey);
			}
			continue;
		}
		/* CSTP options... */
		*next_cstp_option = new_option;
		next_cstp_option = &new_option->next;


		if (!strcmp(buf + 7, "Keepalive")) {
			vpninfo->ssl_times.keepalive = atol(colon);
		} else if (!strcmp(buf + 7, "DPD")) {
			int j = atol(colon);
			if (j && (!vpninfo->ssl_times.dpd || j < vpninfo->ssl_times.dpd))
				vpninfo->ssl_times.dpd = j;
		} else if (!strcmp(buf + 7, "Rekey-Time")) {
			vpninfo->ssl_times.rekey = atol(colon);
		} else if (!strcmp(buf + 7, "Content-Encoding")) {
			if (!strcmp(colon, "deflate"))
				vpninfo->deflate = 1;
			else {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Unknown CSTP-Content-Encoding %s\n"),
					     colon);
				return -EINVAL;
			}
		} else if (!strcmp(buf + 7, "MTU")) {
			vpninfo->mtu = atol(colon);
		} else if (!strcmp(buf + 7, "Address")) {
			if (strchr(new_option->value, ':'))
				vpninfo->vpn_addr6 = new_option->value;
			else
				vpninfo->vpn_addr = new_option->value;
		} else if (!strcmp(buf + 7, "Netmask")) {
			if (strchr(new_option->value, ':'))
				vpninfo->vpn_netmask6 = new_option->value;
			else
				vpninfo->vpn_netmask = new_option->value;
		} else if (!strcmp(buf + 7, "DNS")) {
			int j;
			for (j = 0; j < 3; j++) {
				if (!vpninfo->vpn_dns[j]) {
					vpninfo->vpn_dns[j] = new_option->value;
					break;
				}
			}
		} else if (!strcmp(buf + 7, "NBNS")) {
			int j;
			for (j = 0; j < 3; j++) {
				if (!vpninfo->vpn_nbns[j]) {
					vpninfo->vpn_nbns[j] = new_option->value;
					break;
				}
			}
		} else if (!strcmp(buf + 7, "Default-Domain")) {
			vpninfo->vpn_domain = new_option->value;
		} else if (!strcmp(buf + 7, "MSIE-Proxy-PAC-URL")) {
			vpninfo->vpn_proxy_pac = new_option->value;
		} else if (!strcmp(buf + 7, "Banner")) {
			vpninfo->banner = new_option->value;
		} else if (!strcmp(buf + 7, "Split-Include")) {
			struct split_include *inc = malloc(sizeof(*inc));
			if (!inc)
				continue;
			inc->route = new_option->value;
			inc->next = vpninfo->split_includes;
			vpninfo->split_includes = inc;
		} else if (!strcmp(buf + 7, "Split-Exclude")) {
			struct split_include *exc = malloc(sizeof(*exc));
			if (!exc)
				continue;
			exc->route = new_option->value;
			exc->next = vpninfo->split_excludes;
			vpninfo->split_excludes = exc;
		}
	}

	if (!vpninfo->vpn_addr && !vpninfo->vpn_addr6) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("No IP address received. Aborting\n"));
		return -EINVAL;
	}
	if (old_addr) {
		if (strcmp(old_addr, vpninfo->vpn_addr)) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Reconnect gave different Legacy IP address (%s != %s)\n"),
				     vpninfo->vpn_addr, old_addr);
			return -EINVAL;
		}
	}
	if (old_netmask) {
		if (strcmp(old_netmask, vpninfo->vpn_netmask)) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Reconnect gave different Legacy IP netmask (%s != %s)\n"),
				     vpninfo->vpn_netmask, old_netmask);
			return -EINVAL;
		}
	}
	if (old_addr6) {
		if (strcmp(old_addr6, vpninfo->vpn_addr6)) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Reconnect gave different IPv6 address (%s != %s)\n"),
				     vpninfo->vpn_addr6, old_addr6);
			return -EINVAL;
		}
	}
	if (old_netmask6) {
		if (strcmp(old_netmask6, vpninfo->vpn_netmask6)) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Reconnect gave different IPv6 netmask (%s != %s)\n"),
				     vpninfo->vpn_netmask6, old_netmask6);
			return -EINVAL;
		}
	}

	while (old_dtls_opts) {
		struct vpn_option *tmp = old_dtls_opts;
		old_dtls_opts = old_dtls_opts->next;
		free(tmp->value);
		free(tmp->option);
		free(tmp);
	}
	while (old_cstp_opts) {
		struct vpn_option *tmp = old_cstp_opts;
		old_cstp_opts = old_cstp_opts->next;
		free(tmp->value);
		free(tmp->option);
		free(tmp);
	}
	vpn_progress(vpninfo, PRG_INFO, _("CSTP connected. DPD %d, Keepalive %d\n"),
		     vpninfo->ssl_times.dpd, vpninfo->ssl_times.keepalive);

	BIO_set_nbio(SSL_get_rbio(vpninfo->https_ssl), 1);
	BIO_set_nbio(SSL_get_wbio(vpninfo->https_ssl), 1);

	fcntl(vpninfo->ssl_fd, F_SETFL, fcntl(vpninfo->ssl_fd, F_GETFL) | O_NONBLOCK);
	if (vpninfo->select_nfds <= vpninfo->ssl_fd)
		vpninfo->select_nfds = vpninfo->ssl_fd + 1;

	FD_SET(vpninfo->ssl_fd, &vpninfo->select_rfds);
	FD_SET(vpninfo->ssl_fd, &vpninfo->select_efds);

	if (!sessid_found)
		vpninfo->dtls_attempt_period = 0;

	vpninfo->ssl_times.last_rekey = vpninfo->ssl_times.last_rx =
		vpninfo->ssl_times.last_tx = time(NULL);
	return 0;
}


int make_cstp_connection(struct openconnect_info *vpninfo)
{
	int ret;

	if (!vpninfo->https_ssl && (ret = openconnect_open_https(vpninfo)))
		return ret;

	if (vpninfo->deflate) {
		vpninfo->deflate_adler32 = 1;
		vpninfo->inflate_adler32 = 1;

		if (inflateInit2(&vpninfo->inflate_strm, -12) ||
		    deflateInit2(&vpninfo->deflate_strm, Z_DEFAULT_COMPRESSION,
				 Z_DEFLATED, -12, 9, Z_DEFAULT_STRATEGY)) {
			vpn_progress(vpninfo, PRG_ERR, _("Compression setup failed\n"));
			vpninfo->deflate = 0;
		}

		if (!vpninfo->deflate_pkt) {
			vpninfo->deflate_pkt = malloc(sizeof(struct pkt) + 2048);
			if (!vpninfo->deflate_pkt) {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Allocation of deflate buffer failed\n"));
				vpninfo->deflate = 0;
			}
			memset(vpninfo->deflate_pkt, 0, sizeof(struct pkt));
			memcpy(vpninfo->deflate_pkt->hdr, data_hdr, 8);
			vpninfo->deflate_pkt->hdr[6] = AC_PKT_COMPRESSED;
		}
	}

	return start_cstp_connection(vpninfo);
}

int cstp_reconnect(struct openconnect_info *vpninfo)
{
	int ret;
	int timeout;
	int interval;

	openconnect_close_https(vpninfo);

	/* It's already deflated in the old stream. Extremely
	   non-trivial to reconstitute it; just throw it away */
	if (vpninfo->current_ssl_pkt == vpninfo->deflate_pkt)
		vpninfo->current_ssl_pkt = NULL;

	timeout = vpninfo->reconnect_timeout;
	interval = vpninfo->reconnect_interval;

	while ((ret = make_cstp_connection(vpninfo))) {
		if (timeout <= 0)
			return ret;
		vpn_progress(vpninfo, PRG_INFO,
			     _("sleep %ds, remaining timeout %ds\n"),
			     interval, timeout);
		sleep(interval);
		if (killed)
			return 1;
		timeout -= interval;
		interval += vpninfo->reconnect_interval;
		if (interval > RECONNECT_INTERVAL_MAX)
			interval = RECONNECT_INTERVAL_MAX;
	}
	script_reconnect(vpninfo);
	return 0;
}

static int inflate_and_queue_packet(struct openconnect_info *vpninfo,
				    unsigned char *buf, int len)
{
	struct pkt *new = malloc(sizeof(struct pkt) + vpninfo->mtu);
	uint32_t pkt_sum;

	if (!new)
		return -ENOMEM;

	new->next = NULL;

	vpninfo->inflate_strm.next_in = buf;
	vpninfo->inflate_strm.avail_in = len - 4;

	vpninfo->inflate_strm.next_out = new->data;
	vpninfo->inflate_strm.avail_out = vpninfo->mtu;
	vpninfo->inflate_strm.total_out = 0;

	if (inflate(&vpninfo->inflate_strm, Z_SYNC_FLUSH)) {
		vpn_progress(vpninfo, PRG_ERR, _("inflate failed\n"));
		free(new);
		return -EINVAL;
	}

	new->len = vpninfo->inflate_strm.total_out;

	vpninfo->inflate_adler32 = adler32(vpninfo->inflate_adler32,
					   new->data, new->len);

	pkt_sum = buf[len - 1] | (buf[len - 2] << 8) |
		(buf[len - 3] << 16) | (buf[len - 4] << 24);

	if (vpninfo->inflate_adler32 != pkt_sum) {
		vpninfo->quit_reason = "Compression (inflate) adler32 failure";
	}

	vpn_progress(vpninfo, PRG_TRACE,
		     _("Received compressed data packet of %ld bytes\n"),
		     vpninfo->inflate_strm.total_out);

	queue_packet(&vpninfo->incoming_queue, new);
	return 0;
}

int cstp_mainloop(struct openconnect_info *vpninfo, int *timeout)
{
	unsigned char buf[16384];
	int len, ret;
	int work_done = 0;

	/* FIXME: The poll() handling here is fairly simplistic. Actually,
	   if the SSL connection stalls it could return a WANT_WRITE error
	   on _either_ of the SSL_read() or SSL_write() calls. In that case,
	   we should probably remove POLLIN from the events we're looking for,
	   and add POLLOUT. As it is, though, it'll just chew CPU time in that
	   fairly unlikely situation, until the write backlog clears. */
	while ( (len = SSL_read(vpninfo->https_ssl, buf, sizeof(buf))) > 0) {
		int payload_len;

		if (buf[0] != 'S' || buf[1] != 'T' ||
		    buf[2] != 'F' || buf[3] != 1 || buf[7])
			goto unknown_pkt;

		payload_len = (buf[4] << 8) + buf[5];
		if (len != 8 + payload_len) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Unexpected packet length. SSL_read returned %d but packet is\n"),
				     len);
			vpn_progress(vpninfo, PRG_ERR,
				     "%02x %02x %02x %02x %02x %02x %02x %02x\n",
				     buf[0], buf[1], buf[2], buf[3],
				     buf[4], buf[5], buf[6], buf[7]);
			continue;
		}
		vpninfo->ssl_times.last_rx = time(NULL);
		switch(buf[6]) {
		case AC_PKT_DPD_OUT:
			vpn_progress(vpninfo, PRG_TRACE,
				     _("Got CSTP DPD request\n"));
			vpninfo->owe_ssl_dpd_response = 1;
			continue;

		case AC_PKT_DPD_RESP:
			vpn_progress(vpninfo, PRG_TRACE,
				     _("Got CSTP DPD response\n"));
			continue;

		case AC_PKT_KEEPALIVE:
			vpn_progress(vpninfo, PRG_TRACE,
				     _("Got CSTP Keepalive\n"));
			continue;

		case AC_PKT_DATA:
			vpn_progress(vpninfo, PRG_TRACE,
				     _("Received uncompressed data packet of %d bytes\n"),
				     payload_len);
			queue_new_packet(&vpninfo->incoming_queue, buf + 8,
					 payload_len);
			work_done = 1;
			continue;

		case AC_PKT_DISCONN: {
			int i;
			for (i = 0; i < payload_len; i++) {
				if (!isprint(buf[payload_len + 8 + i]))
					buf[payload_len + 8 + i] = '.';
			}
			buf[payload_len + 8] = 0;
			vpn_progress(vpninfo, PRG_ERR,
				     _("Received server disconnect: %02x '%s'\n"),
				     buf[8], buf + 9);
			vpninfo->quit_reason = "Server request";
			return 1;
		}
		case AC_PKT_COMPRESSED:
			if (!vpninfo->deflate) {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Compressed packet received in !deflate mode\n"));
				goto unknown_pkt;
			}
			inflate_and_queue_packet(vpninfo, buf + 8, payload_len);
			work_done = 1;
			continue;

		case AC_PKT_TERM_SERVER:
			vpn_progress(vpninfo, PRG_ERR, _("received server terminate packet\n"));
			vpninfo->quit_reason = "Server request";
			return 1;
		}

	unknown_pkt:
		vpn_progress(vpninfo, PRG_ERR,
			     _("Unknown packet %02x %02x %02x %02x %02x %02x %02x %02x\n"),
			     buf[0], buf[1], buf[2], buf[3],
			     buf[4], buf[5], buf[6], buf[7]);
		vpninfo->quit_reason = "Unknown packet received";
		return 1;
	}

	ret = SSL_get_error(vpninfo->https_ssl, len);
	if (ret == SSL_ERROR_SYSCALL || ret == SSL_ERROR_ZERO_RETURN) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("SSL read error %d (server probably closed connection); reconnecting.\n"),
			     ret);
			goto do_reconnect;
	}


	/* If SSL_write() fails we are expected to try again. With exactly
	   the same data, at exactly the same location. So we keep the
	   packet we had before.... */
	if (vpninfo->current_ssl_pkt) {
	handle_outgoing:
		vpninfo->ssl_times.last_tx = time(NULL);
		FD_CLR(vpninfo->ssl_fd, &vpninfo->select_wfds);
		ret = SSL_write(vpninfo->https_ssl,
				vpninfo->current_ssl_pkt->hdr,
				vpninfo->current_ssl_pkt->len + 8);
		if (ret <= 0) {
			ret = SSL_get_error(vpninfo->https_ssl, ret);
			switch (ret) {
			case SSL_ERROR_WANT_WRITE:
				/* Waiting for the socket to become writable -- it's
				   probably stalled, and/or the buffers are full */
				FD_SET(vpninfo->ssl_fd, &vpninfo->select_wfds);

			case SSL_ERROR_WANT_READ:
				if (ka_stalled_dpd_time(&vpninfo->ssl_times, timeout))
					goto peer_dead;
				return work_done;
			default:
				vpn_progress(vpninfo, PRG_ERR, _("SSL_write failed: %d\n"), ret);
				report_ssl_errors(vpninfo);
				goto do_reconnect;
			}
		}
		if (ret != vpninfo->current_ssl_pkt->len + 8) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("SSL wrote too few bytes! Asked for %d, sent %d\n"),
				     vpninfo->current_ssl_pkt->len + 8, ret);
			vpninfo->quit_reason = "Internal error";
			return 1;
		}
		/* Don't free the 'special' packets */
		if (vpninfo->current_ssl_pkt != vpninfo->deflate_pkt &&
		    vpninfo->current_ssl_pkt != &dpd_pkt &&
		    vpninfo->current_ssl_pkt != &dpd_resp_pkt &&
		    vpninfo->current_ssl_pkt != &keepalive_pkt)
			free(vpninfo->current_ssl_pkt);

		vpninfo->current_ssl_pkt = NULL;
	}

	if (vpninfo->owe_ssl_dpd_response) {
		vpninfo->owe_ssl_dpd_response = 0;
		vpninfo->current_ssl_pkt = &dpd_resp_pkt;
		goto handle_outgoing;
	}

	switch (keepalive_action(&vpninfo->ssl_times, timeout)) {
	case KA_REKEY:
		/* Not that this will ever happen; we don't even process
		   the setting when we're asked for it. */
		vpn_progress(vpninfo, PRG_INFO, _("CSTP rekey due\n"));
		goto do_reconnect;
		break;

	case KA_DPD_DEAD:
	peer_dead:
		vpn_progress(vpninfo, PRG_ERR,
			     _("CSTP Dead Peer Detection detected dead peer!\n"));
	do_reconnect:
		if (cstp_reconnect(vpninfo)) {
			vpn_progress(vpninfo, PRG_ERR, _("Reconnect failed\n"));
			vpninfo->quit_reason = "CSTP reconnect failed";
			return 1;
		}
		/* I think we can leave DTLS to its own devices; when we reconnect
		   with the same master secret, we do seem to get the same sessid */
		return 1;

	case KA_DPD:
		vpn_progress(vpninfo, PRG_TRACE, _("Send CSTP DPD\n"));

		vpninfo->current_ssl_pkt = &dpd_pkt;
		goto handle_outgoing;

	case KA_KEEPALIVE:
		/* No need to send an explicit keepalive
		   if we have real data to send */
		if (vpninfo->dtls_fd == -1 && vpninfo->outgoing_queue)
			break;

		vpn_progress(vpninfo, PRG_TRACE, _("Send CSTP Keepalive\n"));

		vpninfo->current_ssl_pkt = &keepalive_pkt;
		goto handle_outgoing;

	case KA_NONE:
		;
	}

	/* Service outgoing packet queue, if no DTLS */
	while (vpninfo->dtls_fd == -1 && vpninfo->outgoing_queue) {
		struct pkt *this = vpninfo->outgoing_queue;
		vpninfo->outgoing_queue = this->next;
		vpninfo->outgoing_qlen--;

		if (vpninfo->deflate) {
			unsigned char *adler;
			int ret;

			vpninfo->deflate_strm.next_in = this->data;
			vpninfo->deflate_strm.avail_in = this->len;
			vpninfo->deflate_strm.next_out = (void *)vpninfo->deflate_pkt->data;
			vpninfo->deflate_strm.avail_out = 2040;
			vpninfo->deflate_strm.total_out = 0;

			ret = deflate(&vpninfo->deflate_strm, Z_SYNC_FLUSH);
			if (ret) {
				vpn_progress(vpninfo, PRG_ERR, _("deflate failed %d\n"), ret);
				goto uncompr;
			}

			vpninfo->deflate_pkt->hdr[4] = (vpninfo->deflate_strm.total_out + 4) >> 8;
			vpninfo->deflate_pkt->hdr[5] = (vpninfo->deflate_strm.total_out + 4) & 0xff;

			/* Add ongoing adler32 to tail of compressed packet */
			vpninfo->deflate_adler32 = adler32(vpninfo->deflate_adler32,
							   this->data, this->len);

			adler = &vpninfo->deflate_pkt->data[vpninfo->deflate_strm.total_out];
			*(adler++) =  vpninfo->deflate_adler32 >> 24;
			*(adler++) = (vpninfo->deflate_adler32 >> 16) & 0xff;
			*(adler++) = (vpninfo->deflate_adler32 >> 8) & 0xff;
			*(adler)   =  vpninfo->deflate_adler32 & 0xff;

			vpninfo->deflate_pkt->len = vpninfo->deflate_strm.total_out + 4;

			vpn_progress(vpninfo, PRG_TRACE,
				     _("Sending compressed data packet of %d bytes\n"),
				     this->len);

			vpninfo->current_ssl_pkt = vpninfo->deflate_pkt;
		} else {
		uncompr:
			memcpy(this->hdr, data_hdr, 8);
			this->hdr[4] = this->len >> 8;
			this->hdr[5] = this->len & 0xff;

			vpn_progress(vpninfo, PRG_TRACE,
				     _("Sending uncompressed data packet of %d bytes\n"),
				     this->len);

			vpninfo->current_ssl_pkt = this;
		}
		goto handle_outgoing;
	}

	/* Work is not done if we just got rid of packets off the queue */
	return work_done;
}

int cstp_bye(struct openconnect_info *vpninfo, const char *reason)
{
	unsigned char *bye_pkt;
	int reason_len;

	/* already lost connection? */
	if (!vpninfo->https_ssl)
		return 0;

	reason_len = strlen(reason);
	bye_pkt = malloc(reason_len + 9);
	if (!bye_pkt)
		return -ENOMEM;

	memcpy(bye_pkt, data_hdr, 8);
	memcpy(bye_pkt + 9, reason, reason_len);

	bye_pkt[4] = (reason_len + 1) >> 8;
	bye_pkt[5] = (reason_len + 1) & 0xff;
	bye_pkt[6] = AC_PKT_DISCONN;
	bye_pkt[8] = 0xb0;

	SSL_write(vpninfo->https_ssl, bye_pkt, reason_len + 9);
	free(bye_pkt);

	vpn_progress(vpninfo, PRG_INFO,
		     _("Send BYE packet: %s\n"), reason);

	return 0;
}
