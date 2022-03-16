/* $OpenBSD: ssh-pkcs11-client.c,v 1.17 2020/10/18 11:32:02 djm Exp $ */
/*
 * Copyright (c) 2010 Markus Friedl.  All rights reserved.
 * Copyright (c) 2014 Pedro Martelletto. All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "includes.h"

#ifdef ENABLE_PKCS11

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <sys/socket.h>

#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <openssl/ecdsa.h>
#include <openssl/rsa.h>

#include "openbsd-compat/openssl-compat.h"

#include "pathnames.h"
#include "xmalloc.h"
#include "sshbuf.h"
#include "log.h"
#include "misc.h"
#include "sshkey.h"
#include "authfd.h"
#include "atomicio.h"
#include "ssh-pkcs11.h"
#include "ssherr.h"

#ifdef WINDOWS
#include "openbsd-compat/sys-queue.h"
#define CRYPTOKI_COMPAT
#include "pkcs11.h"

static char module_path[PATH_MAX + 1];
extern char* sshagent_con_username;
extern HANDLE sshagent_client_primary_token;

struct pkcs11_provider {
	char			*name;
	TAILQ_ENTRY(pkcs11_provider) next;
};

TAILQ_HEAD(, pkcs11_provider) pkcs11_providers;

struct pkcs11_keyinfo {
	struct sshkey	*key;
	char		*providername, *label;
	TAILQ_ENTRY(pkcs11_keyinfo) next;
};

TAILQ_HEAD(, pkcs11_keyinfo) pkcs11_keylist;

#define MAX_MSG_LENGTH		10240 /*XXX*/

/* input and output queue */
struct sshbuf *iqueue;
struct sshbuf *oqueue;

void
add_key(struct sshkey *k, char *name)
{
	struct pkcs11_keyinfo *ki;

	ki = xcalloc(1, sizeof(*ki));
	ki->providername = xstrdup(name);
	ki->key = k;
	TAILQ_INSERT_TAIL(&pkcs11_keylist, ki, next);
}

void
del_all_keys()
{
	struct pkcs11_keyinfo *ki, *nxt;

	for (ki = TAILQ_FIRST(&pkcs11_keylist); ki; ki = nxt) {
		nxt = TAILQ_NEXT(ki, next);
		TAILQ_REMOVE(&pkcs11_keylist, ki, next);
		free(ki->providername);
		sshkey_free(ki->key);
		free(ki);
	}
}

/* lookup matching 'private' key */
struct sshkey *
lookup_key(const struct sshkey *k)
{
	struct pkcs11_keyinfo *ki;

	TAILQ_FOREACH(ki, &pkcs11_keylist, next) {
		debug("check %p %s %s", ki, ki->providername, ki->label);
		if (sshkey_equal(k, ki->key))
			return (ki->key);
	}
	return (NULL);
}

static char *
find_helper_in_module_path(void)
{
	wchar_t path[PATH_MAX + 1];
	DWORD n;
	char *ep;

	memset(module_path, 0, sizeof(module_path));
	memset(path, 0, sizeof(path));
	if ((n = GetModuleFileNameW(NULL, path, PATH_MAX)) == 0 ||
		n >= PATH_MAX) {
		error_f("GetModuleFileNameW failed");
		return NULL;
	}
	if (wcstombs_s(NULL, module_path, sizeof(module_path), path,
		sizeof(module_path) - 1) != 0) {
		error_f("wcstombs_s failed");
		return NULL;
	}
	if ((ep = strrchr(module_path, '\\')) == NULL) {
		error_f("couldn't locate trailing \\");
		return NULL;
	}
	*(++ep) = '\0'; /* trim */
	strlcat(module_path, "ssh-pkcs11-helper.exe", sizeof(module_path) - 1);

	return module_path;
}

static char *
find_helper(void)
{
	char *helper;
	char module_path[PATH_MAX + 1];
	char *ep;
	DWORD n;

	if ((helper = getenv("SSH_PKCS11_HELPER")) == NULL || strlen(helper) == 0) {
		if ((helper = find_helper_in_module_path()) == NULL)
			helper = _PATH_SSH_PKCS11_HELPER;
	}
	if (!path_absolute(helper)) {
		error_f("helper \"%s\" unusable: path not absolute", helper);
		return NULL;
	}
	debug_f("using \"%s\" as helper", helper);

	return helper;
}

#endif /* WINDOWS */

/* borrows code from sftp-server and ssh-agent */

static int fd = -1;
static pid_t pid = -1;

static void
send_msg(struct sshbuf *m)
{
	u_char buf[4];
	size_t mlen = sshbuf_len(m);
	int r;

	POKE_U32(buf, mlen);
	if (atomicio(vwrite, fd, buf, 4) != 4 ||
	    atomicio(vwrite, fd, sshbuf_mutable_ptr(m),
	    sshbuf_len(m)) != sshbuf_len(m))
		error("write to helper failed");
	if ((r = sshbuf_consume(m, mlen)) != 0)
		fatal_fr(r, "consume");
}

static int
recv_msg(struct sshbuf *m)
{
	u_int l, len;
	u_char c, buf[1024];
	int r;

	if ((len = atomicio(read, fd, buf, 4)) != 4) {
		error("read from helper failed: %u", len);
		return (0); /* XXX */
	}
	len = PEEK_U32(buf);
	if (len > 256 * 1024)
		fatal("response too long: %u", len);
	/* read len bytes into m */
	sshbuf_reset(m);
	while (len > 0) {
		l = len;
		if (l > sizeof(buf))
			l = sizeof(buf);
		if (atomicio(read, fd, buf, l) != l) {
			error("response from helper failed.");
			return (0); /* XXX */
		}
		if ((r = sshbuf_put(m, buf, l)) != 0)
			fatal_fr(r, "sshbuf_put");
		len -= l;
	}
	if ((r = sshbuf_get_u8(m, &c)) != 0)
		fatal_fr(r, "parse type");
	return c;
}

int
pkcs11_init(int interactive)
{
#ifdef WINDOWS
	TAILQ_INIT(&pkcs11_providers);
	TAILQ_INIT(&pkcs11_keylist);
#endif /* WINDOWS */
	return (0);
}

void
pkcs11_terminate(void)
{
#ifdef WINDOWS
	struct pkcs11_provider *p;

	while ((p = TAILQ_FIRST(&pkcs11_providers)) != NULL) {
		// Send message to helper to gracefully unload providers
		pkcs11_del_provider(p->name);
		TAILQ_REMOVE(&pkcs11_providers, p, next);
	}

	if (pid != -1) {
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		pid = -1;
	}
#endif /* WINDOWS */

	if (fd >= 0)
		close(fd);

#ifdef WINDOWS
	fd = -1;
#endif
}

static int
rsa_encrypt(int flen, const u_char *from, u_char *to, RSA *rsa, int padding)
{
	struct sshkey *key = NULL;
	struct sshbuf *msg = NULL;
	u_char *blob = NULL, *signature = NULL;
	size_t blen, slen = 0;
	int r, ret = -1;

	if (padding != RSA_PKCS1_PADDING)
		goto fail;
	key = sshkey_new(KEY_UNSPEC);
	if (key == NULL) {
		error_f("sshkey_new failed");
		goto fail;
	}
	key->type = KEY_RSA;
	RSA_up_ref(rsa);
	key->rsa = rsa;
	if ((r = sshkey_to_blob(key, &blob, &blen)) != 0) {
		error_fr(r, "encode key");
		goto fail;
	}
	if ((msg = sshbuf_new()) == NULL)
		fatal_f("sshbuf_new failed");
	if ((r = sshbuf_put_u8(msg, SSH2_AGENTC_SIGN_REQUEST)) != 0 ||
	    (r = sshbuf_put_string(msg, blob, blen)) != 0 ||
	    (r = sshbuf_put_string(msg, from, flen)) != 0 ||
	    (r = sshbuf_put_u32(msg, 0)) != 0)
		fatal_fr(r, "compose");
	send_msg(msg);
	sshbuf_reset(msg);

	if (recv_msg(msg) == SSH2_AGENT_SIGN_RESPONSE) {
		if ((r = sshbuf_get_string(msg, &signature, &slen)) != 0)
			fatal_fr(r, "parse");
		if (slen <= (size_t)RSA_size(rsa)) {
			memcpy(to, signature, slen);
			ret = slen;
		}
		free(signature);
	}
 fail:
	free(blob);
	sshkey_free(key);
	sshbuf_free(msg);
	return (ret);
}

#if defined(OPENSSL_HAS_ECC) && defined(HAVE_EC_KEY_METHOD_NEW)
static ECDSA_SIG *
ecdsa_do_sign(const unsigned char *dgst, int dgst_len, const BIGNUM *inv,
    const BIGNUM *rp, EC_KEY *ec)
{
	struct sshkey *key = NULL;
	struct sshbuf *msg = NULL;
	ECDSA_SIG *ret = NULL;
	const u_char *cp;
	u_char *blob = NULL, *signature = NULL;
	size_t blen, slen = 0;
	int r, nid;

	nid = sshkey_ecdsa_key_to_nid(ec);
	if (nid < 0) {
		error_f("couldn't get curve nid");
		goto fail;
	}

	key = sshkey_new(KEY_UNSPEC);
	if (key == NULL) {
		error_f("sshkey_new failed");
		goto fail;
	}
	key->ecdsa = ec;
	key->ecdsa_nid = nid;
	key->type = KEY_ECDSA;
	EC_KEY_up_ref(ec);

	if ((r = sshkey_to_blob(key, &blob, &blen)) != 0) {
		error_fr(r, "encode key");
		goto fail;
	}
	if ((msg = sshbuf_new()) == NULL)
		fatal_f("sshbuf_new failed");
	if ((r = sshbuf_put_u8(msg, SSH2_AGENTC_SIGN_REQUEST)) != 0 ||
	    (r = sshbuf_put_string(msg, blob, blen)) != 0 ||
	    (r = sshbuf_put_string(msg, dgst, dgst_len)) != 0 ||
	    (r = sshbuf_put_u32(msg, 0)) != 0)
		fatal_fr(r, "compose");
	send_msg(msg);
	sshbuf_reset(msg);

	if (recv_msg(msg) == SSH2_AGENT_SIGN_RESPONSE) {
		if ((r = sshbuf_get_string(msg, &signature, &slen)) != 0)
			fatal_fr(r, "parse");
		cp = signature;
		ret = d2i_ECDSA_SIG(NULL, &cp, slen);
		free(signature);
	}

 fail:
	free(blob);
	sshkey_free(key);
	sshbuf_free(msg);
	return (ret);
}
#endif /* OPENSSL_HAS_ECC && HAVE_EC_KEY_METHOD_NEW */

static RSA_METHOD	*helper_rsa;
#if defined(OPENSSL_HAS_ECC) && defined(HAVE_EC_KEY_METHOD_NEW)
static EC_KEY_METHOD	*helper_ecdsa;
#endif /* OPENSSL_HAS_ECC && HAVE_EC_KEY_METHOD_NEW */

/* redirect private key crypto operations to the ssh-pkcs11-helper */
static void
wrap_key(struct sshkey *k)
{
	if (k->type == KEY_RSA)
		RSA_set_method(k->rsa, helper_rsa);
#if defined(OPENSSL_HAS_ECC) && defined(HAVE_EC_KEY_METHOD_NEW)
	else if (k->type == KEY_ECDSA)
		EC_KEY_set_method(k->ecdsa, helper_ecdsa);
#endif /* OPENSSL_HAS_ECC && HAVE_EC_KEY_METHOD_NEW */
	else
		fatal_f("unknown key type");
}

static int
pkcs11_start_helper_methods(void)
{
	if (helper_rsa != NULL)
		return (0);

#if defined(OPENSSL_HAS_ECC) && defined(HAVE_EC_KEY_METHOD_NEW)
	int (*orig_sign)(int, const unsigned char *, int, unsigned char *,
	    unsigned int *, const BIGNUM *, const BIGNUM *, EC_KEY *) = NULL;
	if (helper_ecdsa != NULL)
		return (0);
	helper_ecdsa = EC_KEY_METHOD_new(EC_KEY_OpenSSL());
	if (helper_ecdsa == NULL)
		return (-1);
	EC_KEY_METHOD_get_sign(helper_ecdsa, &orig_sign, NULL, NULL);
	EC_KEY_METHOD_set_sign(helper_ecdsa, orig_sign, NULL, ecdsa_do_sign);
#endif /* OPENSSL_HAS_ECC && HAVE_EC_KEY_METHOD_NEW */

	if ((helper_rsa = RSA_meth_dup(RSA_get_default_method())) == NULL)
		fatal_f("RSA_meth_dup failed");
	if (!RSA_meth_set1_name(helper_rsa, "ssh-pkcs11-helper") ||
	    !RSA_meth_set_priv_enc(helper_rsa, rsa_encrypt))
		fatal_f("failed to prepare method");

	return (0);
}

static int
pkcs11_start_helper(void)
{
	int pair[2];
	char *helper, *verbosity = NULL;
#ifdef WINDOWS
	int r, actions_inited = 0;
	char *av[3];
	posix_spawn_file_actions_t actions;
	HANDLE client_token = NULL, client_process_handle = NULL;

	r = SSH_ERR_SYSTEM_ERROR;
	pair[0] = pair[1] = -1;

	if ((helper = find_helper()) == NULL)
		goto out;
#endif /* WINDOWS */


#ifdef DEBUG_PKCS11
	verbosity = "-vvv";
#endif

	if (log_level_get() >= SYSLOG_LEVEL_DEBUG1)
		verbosity = "-vvv";

	if (pkcs11_start_helper_methods() == -1) {
		error("pkcs11_start_helper_methods failed");
		return (-1);
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1) {
		error("socketpair: %s", strerror(errno));
		return (-1);
	}
#ifdef WINDOWS
	if (posix_spawn_file_actions_init(&actions) != 0) {
		error_f("posix_spawn_file_actions_init failed");
		goto out;
	}
	actions_inited = 1;
	if (posix_spawn_file_actions_adddup2(&actions, pair[1],
		STDIN_FILENO) != 0 ||
		posix_spawn_file_actions_adddup2(&actions, pair[1],
			STDOUT_FILENO) != 0) {
		error_f("posix_spawn_file_actions_adddup2 failed");
		goto out;
	}

	av[0] = helper;
	av[1] = verbosity;
	av[2] = NULL;

	if (!sshagent_con_username) {
		error_f("sshagent_con_username is NULL");
		goto out;
	}

	if (!sshagent_client_primary_token) {
		error_f("sshagent_client_primary_token is NULL for user:%s", sshagent_con_username);
		goto out;
	}

	if (posix_spawnp_as_user((pid_t *)&pid, av[0], &actions, NULL, av, NULL, sshagent_client_primary_token) != 0) {
		error_f("failed to spwan process %s", av[0]);
		goto out;
	}
	fd = pair[0];
	r = 0;
#else
	if ((pid = fork()) == -1) {
		error("fork: %s", strerror(errno));
		return (-1);
	} else if (pid == 0) {
		if ((dup2(pair[1], STDIN_FILENO) == -1) ||
		    (dup2(pair[1], STDOUT_FILENO) == -1)) {
			fprintf(stderr, "dup2: %s\n", strerror(errno));
			_exit(1);
		}
		close(pair[0]);
		close(pair[1]);
		helper = getenv("SSH_PKCS11_HELPER");
		if (helper == NULL || strlen(helper) == 0)
			helper = _PATH_SSH_PKCS11_HELPER;
		debug_f("starting %s %s", helper,
		    verbosity == NULL ? "" : verbosity);
		execlp(helper, helper, verbosity, (char *)NULL);
		fprintf(stderr, "exec: %s: %s\n", helper, strerror(errno));
		_exit(1);
	}
	close(pair[1]);
	fd = pair[0];
	return (0);
#endif
	/* success */
	debug3_f("started pid=%ld", (long)pid);
out:
	if (client_token)
		CloseHandle(client_token);
	return r;
}

int
pkcs11_add_provider(char *name, char *pin, struct sshkey ***keysp,
    char ***labelsp)
{
	struct sshkey *k;
	int r, type;
	u_char *blob;
	char *label;
	size_t blen;
	u_int nkeys, i;
	struct sshbuf *msg;
	struct pkcs11_provider *p;

	if (fd < 0 && pkcs11_start_helper() < 0)
		return (-1);

	if ((msg = sshbuf_new()) == NULL)
		fatal_f("sshbuf_new failed");
	if ((r = sshbuf_put_u8(msg, SSH_AGENTC_ADD_SMARTCARD_KEY)) != 0 ||
	    (r = sshbuf_put_cstring(msg, name)) != 0 ||
	    (r = sshbuf_put_cstring(msg, pin)) != 0)
		fatal_fr(r, "compose");
	send_msg(msg);
	sshbuf_reset(msg);

	type = recv_msg(msg);
	if (type == SSH2_AGENT_IDENTITIES_ANSWER) {
		if ((r = sshbuf_get_u32(msg, &nkeys)) != 0)
			fatal_fr(r, "parse nkeys");
		*keysp = xcalloc(nkeys, sizeof(struct sshkey *));
		if (labelsp)
			*labelsp = xcalloc(nkeys, sizeof(char *));
		for (i = 0; i < nkeys; i++) {
			/* XXX clean up properly instead of fatal() */
			if ((r = sshbuf_get_string(msg, &blob, &blen)) != 0 ||
			    (r = sshbuf_get_cstring(msg, &label, NULL)) != 0)
				fatal_fr(r, "parse key");
			if ((r = sshkey_from_blob(blob, blen, &k)) != 0)
				fatal_fr(r, "decode key");
			wrap_key(k);
			(*keysp)[i] = k;
			if (labelsp)
				(*labelsp)[i] = label;
			else
				free(label);
			free(blob);
		}
	} else if (type == SSH2_AGENT_FAILURE) {
		if ((r = sshbuf_get_u32(msg, &nkeys)) != 0)
			nkeys = -1;
	} else {
		nkeys = -1;
	}

#ifdef WINDOWS
	p = xcalloc(1, sizeof(*p));
	p->name = xstrdup(name);
	TAILQ_INSERT_TAIL(&pkcs11_providers, p, next);
#endif /* WINDOWS */
	sshbuf_free(msg);
	return (nkeys);
}

int
pkcs11_del_provider(char *name)
{
	int r, ret = -1;
	struct sshbuf *msg;

	if ((msg = sshbuf_new()) == NULL)
		fatal_f("sshbuf_new failed");
	if ((r = sshbuf_put_u8(msg, SSH_AGENTC_REMOVE_SMARTCARD_KEY)) != 0 ||
	    (r = sshbuf_put_cstring(msg, name)) != 0 ||
	    (r = sshbuf_put_cstring(msg, "")) != 0)
		fatal_fr(r, "compose");
	send_msg(msg);
	sshbuf_reset(msg);

	if (recv_msg(msg) == SSH_AGENT_SUCCESS)
		ret = 0;
	sshbuf_free(msg);
	return (ret);
}

#endif /* ENABLE_PKCS11 */
