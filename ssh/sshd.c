/* $OpenBSD: sshd.c,v 1.418 2014/02/02 03:44:32 djm Exp $ */
/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * This program is the ssh daemon.  It listens for connections from clients,
 * and performs authentication, executes use commands or shell, and forwards
 * information to/from the application to the user client over an encrypted
 * connection.  This can also handle forwarding of X11, TCP/IP, and
 * authentication agent connections.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 *
 * SSH2 implementation:
 * Privilege Separation:
 *
 * Copyright (c) 2000, 2001, 2002 Markus Friedl.  All rights reserved.
 * Copyright (c) 2002 Niels Provos.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/tree.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/queue.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <paths.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/dh.h>
#include <openssl/bn.h>
#include <openssl/rand.h>

#include "xmalloc.h"
#include "ssh.h"
#include "ssh1.h"
#include "ssh2.h"
#include "rsa.h"
#include "sshpty.h"
#include "packet.h"
#include "log.h"
#include "sshbuf.h"
#include "servconf.h"
#include "uidswap.h"
#include "compat.h"
#include "cipher.h"
#include "digest.h"
#include "key.h"
#include "kex.h"
#include "dh.h"
#include "myproposal.h"
#include "authfile.h"
#include "pathnames.h"
#include "atomicio.h"
#include "canohost.h"
#include "hostfile.h"
#include "auth.h"
#include "authfd.h"
#include "misc.h"
#include "msg.h"
#include "dispatch.h"
#include "channels.h"
#include "session.h"
#include "monitor_mm.h"
#include "monitor.h"
#ifdef GSSAPI
#include "ssh-gss.h"
#endif
#include "monitor_wrap.h"
#include "roaming.h"
#include "ssh-sandbox.h"
#include "version.h"
#include "err.h"

#ifdef LIBWRAP
#include <tcpd.h>
#include <syslog.h>
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;
#endif /* LIBWRAP */

#ifndef O_NOCTTY
#define O_NOCTTY	0
#endif

/* Re-exec fds */
#define REEXEC_DEVCRYPTO_RESERVED_FD	(STDERR_FILENO + 1)
#define REEXEC_STARTUP_PIPE_FD		(STDERR_FILENO + 2)
#define REEXEC_CONFIG_PASS_FD		(STDERR_FILENO + 3)
#define REEXEC_MIN_FREE_FD		(STDERR_FILENO + 4)

extern char *__progname;

/* Server configuration options. */
ServerOptions options;

/* Name of the server configuration file. */
char *config_file_name = _PATH_SERVER_CONFIG_FILE;

/*
 * Debug mode flag.  This can be set on the command line.  If debug
 * mode is enabled, extra debugging output will be sent to the system
 * log, the daemon will not go to background, and will exit after processing
 * the first connection.
 */
int debug_flag = 0;

/* Flag indicating that the daemon should only test the configuration and keys. */
int test_flag = 0;

/* Flag indicating that the daemon is being started from inetd. */
int inetd_flag = 0;

/* Flag indicating that sshd should not detach and become a daemon. */
int no_daemon_flag = 0;

/* debug goes to stderr unless inetd_flag is set */
int log_stderr = 0;

/* Saved arguments to main(). */
char **saved_argv;

/* re-exec */
int rexeced_flag = 0;
int rexec_flag = 1;
int rexec_argc = 0;
char **rexec_argv;

/*
 * The sockets that the server is listening; this is used in the SIGHUP
 * signal handler.
 */
#define	MAX_LISTEN_SOCKS	16
int listen_socks[MAX_LISTEN_SOCKS];
int num_listen_socks = 0;

/*
 * the client's version string, passed by sshd2 in compat mode. if != NULL,
 * sshd will skip the version-number exchange
 */
char *client_version_string = NULL;
char *server_version_string = NULL;

/* Daemon's agent connection */
int auth_sock = -1;
int have_agent = 0;

/*
 * Any really sensitive data in the application is contained in this
 * structure. The idea is that this structure could be locked into memory so
 * that the pages do not get written into swap.  However, there are some
 * problems. The private key contains BIGNUMs, and we do not (in principle)
 * have access to the internals of them, and locking just the structure is
 * not very useful.  Currently, memory locking is not implemented.
 */
struct {
	struct sshkey *server_key;		/* ephemeral server key */
	struct sshkey *ssh1_host_key;		/* ssh1 host key */
	struct sshkey **host_keys;		/* all private host keys */
	struct sshkey **host_pubkeys;		/* all public host keys */
	struct sshkey **host_certificates;	/* all public host certs */
	int	have_ssh1_key;
	int	have_ssh2_key;
	u_char	ssh1_cookie[SSH_SESSION_KEY_LENGTH];
} sensitive_data;

/*
 * Flag indicating whether the RSA server key needs to be regenerated.
 * Is set in the SIGALRM handler and cleared when the key is regenerated.
 */
static volatile sig_atomic_t key_do_regen = 0;

/* This is set to true when a signal is received. */
static volatile sig_atomic_t received_sighup = 0;
static volatile sig_atomic_t received_sigterm = 0;

/* session identifier, used by RSA-auth */
u_char session_id[16];

/* same for ssh2 */
u_char *session_id2 = NULL;
u_int session_id2_len = 0;

/* record remote hostname or ip */
u_int utmp_len = MAXHOSTNAMELEN;

/* options.max_startup sized array of fd ints */
int *startup_pipes = NULL;
int startup_pipe;		/* in child */

/* variables used for privilege separation */
int use_privsep = -1;
struct monitor *pmonitor = NULL;
int privsep_is_preauth = 1;

/* global authentication context */
static struct authctxt *the_authctxt = NULL;	/* XXX */

/* sshd_config buffer */
struct sshbuf *cfg;

/* message to be displayed after login */
struct sshbuf *loginmsg = NULL;

/* Prototypes for various functions defined later in this file. */
void destroy_sensitive_data(void);
void demote_sensitive_data(void);

static void do_ssh1_kex(struct ssh *ssh);
static void do_ssh2_kex(struct ssh *ssh);

/*
 * Close all listening sockets
 */
static void
close_listen_socks(void)
{
	int i;

	for (i = 0; i < num_listen_socks; i++)
		close(listen_socks[i]);
	num_listen_socks = -1;
}

static void
close_startup_pipes(void)
{
	int i;

	if (startup_pipes)
		for (i = 0; i < options.max_startups; i++)
			if (startup_pipes[i] != -1)
				close(startup_pipes[i]);
}

/*
 * Signal handler for SIGHUP.  Sshd execs itself when it receives SIGHUP;
 * the effect is to reread the configuration file (and to regenerate
 * the server key).
 */

/*ARGSUSED*/
static void
sighup_handler(int sig)
{
	int save_errno = errno;

	received_sighup = 1;
	signal(SIGHUP, sighup_handler);
	errno = save_errno;
}

/*
 * Called from the main program after receiving SIGHUP.
 * Restarts the server.
 */
static void
sighup_restart(void)
{
	logit("Received SIGHUP; restarting.");
	close_listen_socks();
	close_startup_pipes();
	alarm(0);  /* alarm timer persists across exec */
	signal(SIGHUP, SIG_IGN); /* will be restored after exec */
	execv(saved_argv[0], saved_argv);
	logit("RESTART FAILED: av[0]='%.100s', error: %.100s.", saved_argv[0],
	    strerror(errno));
	exit(1);
}

/*
 * Generic signal handler for terminating signals in the master daemon.
 */
/*ARGSUSED*/
static void
sigterm_handler(int sig)
{
	received_sigterm = sig;
}

/*
 * SIGCHLD handler.  This is called whenever a child dies.  This will then
 * reap any zombies left by exited children.
 */
/*ARGSUSED*/
static void
main_sigchld_handler(int sig)
{
	int save_errno = errno;
	pid_t pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0 ||
	    (pid < 0 && errno == EINTR))
		;

	signal(SIGCHLD, main_sigchld_handler);
	errno = save_errno;
}

/*
 * Signal handler for the alarm after the login grace period has expired.
 */
/*ARGSUSED*/
static void
grace_alarm_handler(int sig)
{
	if (use_privsep && pmonitor != NULL && pmonitor->m_pid > 0)
		kill(pmonitor->m_pid, SIGALRM);

	/*
	 * Try to kill any processes that we have spawned, E.g. authorized
	 * keys command helpers.
	 */
	if (getpgid(0) == getpid()) {
		signal(SIGTERM, SIG_IGN);
		kill(0, SIGTERM);
	}

	/* Log error and exit. */
	sigdie("Timeout before authentication for %s",
	    ssh_remote_ipaddr(active_state));
}

/*
 * Signal handler for the key regeneration alarm.  Note that this
 * alarm only occurs in the daemon waiting for connections, and it does not
 * do anything with the private key or random state before forking.
 * Thus there should be no concurrency control/asynchronous execution
 * problems.
 */
static void
generate_ephemeral_server_key(void)
{
	int r;

	verbose("Generating %s%d bit RSA key.",
	    sensitive_data.server_key ? "new " : "", options.server_key_bits);
	if (sensitive_data.server_key != NULL)
		sshkey_free(sensitive_data.server_key);
	if ((r = sshkey_generate(KEY_RSA1, options.server_key_bits,
	    &sensitive_data.server_key)) != 0)
		fatal("RSA1 key ephemeral key generation failed: %s",
		    ssh_err(r));
	verbose("RSA key generation complete.");

	arc4random_buf(sensitive_data.ssh1_cookie, SSH_SESSION_KEY_LENGTH);
}

/*ARGSUSED*/
static void
key_regeneration_alarm(int sig)
{
	int save_errno = errno;

	signal(SIGALRM, SIG_DFL);
	errno = save_errno;
	key_do_regen = 1;
}

static void
sshd_exchange_identification(struct ssh *ssh, int sock_in, int sock_out)
{
	u_int i;
	int mismatch;
	int remote_major, remote_minor;
	int major, minor;
	char *s, *newline = "\n";
	char buf[256];			/* Must not be larger than remote_version. */
	char remote_version[256];	/* Must be at least as big as buf. */

	if ((options.protocol & SSH_PROTO_1) &&
	    (options.protocol & SSH_PROTO_2)) {
		major = PROTOCOL_MAJOR_1;
		minor = 99;
	} else if (options.protocol & SSH_PROTO_2) {
		major = PROTOCOL_MAJOR_2;
		minor = PROTOCOL_MINOR_2;
		newline = "\r\n";
	} else {
		major = PROTOCOL_MAJOR_1;
		minor = PROTOCOL_MINOR_1;
	}

	xasprintf(&server_version_string, "SSH-%d.%d-%.100s%s%s%s",
	    major, minor, SSH_VERSION,
	    *options.version_addendum == '\0' ? "" : " ",
	    options.version_addendum, newline);

	/* Send our protocol version identification. */
	if (roaming_atomicio(vwrite, sock_out, server_version_string,
	    strlen(server_version_string))
	    != strlen(server_version_string)) {
		logit("Could not write ident string to %s",
		    ssh_remote_ipaddr(ssh));
		cleanup_exit(255);
	}

	/* Read other sides version identification. */
	memset(buf, 0, sizeof(buf));
	for (i = 0; i < sizeof(buf) - 1; i++) {
		if (roaming_atomicio(read, sock_in, &buf[i], 1) != 1) {
			logit("Did not receive identification string from %s",
			    ssh_remote_ipaddr(ssh));
			cleanup_exit(255);
		}
		if (buf[i] == '\r') {
			buf[i] = 0;
			/* Kludge for F-Secure Macintosh < 1.0.2 */
			if (i == 12 &&
			    strncmp(buf, "SSH-1.5-W1.0", 12) == 0)
				break;
			continue;
		}
		if (buf[i] == '\n') {
			buf[i] = 0;
			break;
		}
	}
	buf[sizeof(buf) - 1] = 0;
	client_version_string = xstrdup(buf);

	/*
	 * Check that the versions match.  In future this might accept
	 * several versions and set appropriate flags to handle them.
	 */
	if (sscanf(client_version_string, "SSH-%d.%d-%[^\n]\n",
	    &remote_major, &remote_minor, remote_version) != 3) {
		s = "Protocol mismatch.\n";
		(void) atomicio(vwrite, sock_out, s, strlen(s));
		logit("Bad protocol version identification '%.100s' "
		    "from %s port %d", client_version_string,
		    ssh_remote_ipaddr(ssh), ssh_get_remote_port(ssh));
		close(sock_in);
		close(sock_out);
		cleanup_exit(255);
	}
	debug("Client protocol version %d.%d; client software version %.100s",
	    remote_major, remote_minor, remote_version);

	ssh->compat = compat_datafellows(remote_version);

	if ((ssh->compat & SSH_BUG_PROBE) != 0) {
		logit("probed from %s with %s.  Don't panic.",
		    ssh_remote_ipaddr(ssh), client_version_string);
		cleanup_exit(255);
	}

	if ((ssh->compat & SSH_BUG_SCANNER) != 0) {
		logit("scanned from %s with %s.  Don't panic.",
		    ssh_remote_ipaddr(ssh), client_version_string);
		cleanup_exit(255);
	}
	if ((ssh->compat & SSH_BUG_RSASIGMD5) != 0) {
		logit("Client version \"%.100s\" uses unsafe RSA signature "
		    "scheme; disabling use of RSA keys", remote_version);
	}
	if ((ssh->compat & SSH_BUG_DERIVEKEY) != 0) {
		fatal("Client version \"%.100s\" uses unsafe key agreement; "
		    "refusing connection", remote_version);
	}

	mismatch = 0;
	switch (remote_major) {
	case 1:
		if (remote_minor == 99) {
			if (options.protocol & SSH_PROTO_2)
				enable_compat20();
			else
				mismatch = 1;
			break;
		}
		if (!(options.protocol & SSH_PROTO_1)) {
			mismatch = 1;
			break;
		}
		if (remote_minor < 3) {
			ssh_packet_disconnect(ssh, "Your ssh version is too old and "
			    "is no longer supported.  Please install a newer version.");
		} else if (remote_minor == 3) {
			/* note that this disables agent-forwarding */
			enable_compat13();
		}
		break;
	case 2:
		if (options.protocol & SSH_PROTO_2) {
			enable_compat20();
			break;
		}
		/* FALLTHROUGH */
	default:
		mismatch = 1;
		break;
	}
	chop(server_version_string);
	debug("Local version string %.200s", server_version_string);

	if (mismatch) {
		s = "Protocol major versions differ.\n";
		(void) atomicio(vwrite, sock_out, s, strlen(s));
		close(sock_in);
		close(sock_out);
		logit("Protocol major versions differ for %s: %.200s vs. %.200s",
		    ssh_remote_ipaddr(ssh),
		    server_version_string, client_version_string);
		cleanup_exit(255);
	}
}

/* Destroy the host and server keys.  They will no longer be needed. */
void
destroy_sensitive_data(void)
{
	int i;

	if (sensitive_data.server_key) {
		sshkey_free(sensitive_data.server_key);
		sensitive_data.server_key = NULL;
	}
	for (i = 0; i < options.num_host_key_files; i++) {
		if (sensitive_data.host_keys[i]) {
			sshkey_free(sensitive_data.host_keys[i]);
			sensitive_data.host_keys[i] = NULL;
		}
		if (sensitive_data.host_certificates[i]) {
			sshkey_free(sensitive_data.host_certificates[i]);
			sensitive_data.host_certificates[i] = NULL;
		}
	}
	sensitive_data.ssh1_host_key = NULL;
	explicit_bzero(sensitive_data.ssh1_cookie, SSH_SESSION_KEY_LENGTH);
}

/* Demote private to public keys for network child */
void
demote_sensitive_data(void)
{
	struct sshkey *tmp;
	int i, r;

	if (sensitive_data.server_key) {
		if ((r = sshkey_demote(sensitive_data.server_key, &tmp)) != 0)
			fatal("could not demote server key: %s", ssh_err(r));
		sshkey_free(sensitive_data.server_key);
		sensitive_data.server_key = tmp;
	}

	for (i = 0; i < options.num_host_key_files; i++) {
		if (sensitive_data.host_keys[i]) {
			if ((r = sshkey_demote(sensitive_data.host_keys[i],
			    &tmp)) != 0)
				fatal("could not demote host %s key: %s",
				    sshkey_type(sensitive_data.host_keys[i]),
				    ssh_err(r));
			sshkey_free(sensitive_data.host_keys[i]);
			sensitive_data.host_keys[i] = tmp;
			if (tmp->type == KEY_RSA1)
				sensitive_data.ssh1_host_key = tmp;
		}
		/* Certs do not need demotion */
	}

	/* We do not clear ssh1_host key and cookie.  XXX - Okay Niels? */
}

static void
privsep_preauth_child(void)
{
	u_int32_t rnd[256];
	gid_t gidset[1];
	struct passwd *pw;

	/* Enable challenge-response authentication for privilege separation */
	privsep_challenge_enable();

	arc4random_buf(rnd, sizeof(rnd));
	RAND_seed(rnd, sizeof(rnd));
	explicit_bzero(rnd, sizeof(rnd));

	/* Demote the private keys to public keys. */
	demote_sensitive_data();

	if ((pw = getpwnam(SSH_PRIVSEP_USER)) == NULL)
		fatal("Privilege separation user %s does not exist",
		    SSH_PRIVSEP_USER);
	explicit_bzero(pw->pw_passwd, strlen(pw->pw_passwd));
	endpwent();

	/* Change our root directory */
	if (chroot(_PATH_PRIVSEP_CHROOT_DIR) == -1)
		fatal("chroot(\"%s\"): %s", _PATH_PRIVSEP_CHROOT_DIR,
		    strerror(errno));
	if (chdir("/") == -1)
		fatal("chdir(\"/\"): %s", strerror(errno));

	/* Drop our privileges */
	debug3("privsep user:group %u:%u", (u_int)pw->pw_uid,
	    (u_int)pw->pw_gid);
#if 0
	/* XXX not ready, too heavy after chroot */
	do_setusercontext(pw);
#else
	gidset[0] = pw->pw_gid;
	if (setgroups(1, gidset) < 0)
		fatal("setgroups: %.100s", strerror(errno));
	permanently_set_uid(pw);
#endif
}

static int
privsep_preauth(struct authctxt *authctxt)
{
	int r, status;
	pid_t pid;
	struct ssh_sandbox *box = NULL;

	/* Set up unprivileged child process to deal with network data */
	pmonitor = monitor_init();
	/* Store a pointer to the kex for later rekeying */
	pmonitor->m_pkex = &active_state->kex;

	if (use_privsep == PRIVSEP_ON)
		box = ssh_sandbox_init();
	pid = fork();
	if (pid == -1) {
		fatal("fork of unprivileged child failed");
	} else if (pid != 0) {
		debug2("Network child is on pid %ld", (long)pid);

		pmonitor->m_pid = pid;
		if (have_agent) {
			r = ssh_get_authentication_socket(&auth_sock);
			if (r != 0) {
				error("Could not get agent socket: %s",
				    ssh_err(r));
				have_agent = 0;
			}
		}
		if (box != NULL)
			ssh_sandbox_parent_preauth(box, pid);
		monitor_child_preauth(authctxt, pmonitor);

		/* Sync memory */
		monitor_sync(pmonitor);

		/* Wait for the child's exit status */
		while (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR)
				continue;
			pmonitor->m_pid = -1;
			fatal("%s: waitpid: %s", __func__, strerror(errno));
		}
		privsep_is_preauth = 0;
		pmonitor->m_pid = -1;
		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) != 0)
				fatal("%s: preauth child exited with status %d",
				    __func__, WEXITSTATUS(status));
		} else if (WIFSIGNALED(status))
			fatal("%s: preauth child terminated by signal %d",
			    __func__, WTERMSIG(status));
		if (box != NULL)
			ssh_sandbox_parent_finish(box);
		return 1;
	} else {
		/* child */
		close(pmonitor->m_sendfd);
		close(pmonitor->m_log_recvfd);

		/* Arrange for logging to be sent to the monitor */
		set_log_handler(mm_log_handler, pmonitor);

		/* Demote the child */
		if (getuid() == 0 || geteuid() == 0)
			privsep_preauth_child();
		setproctitle("%s", "[net]");
		if (box != NULL)
			ssh_sandbox_child(box);

		return 0;
	}
}

static void
privsep_postauth(struct ssh *ssh)
{
	struct authctxt *authctxt = ssh->authctxt;
	u_int32_t rnd[256];

	if (authctxt->pw->pw_uid == 0 || options.use_login) {
		/* File descriptor passing is broken or root login */
		use_privsep = 0;
		goto skip;
	}

	/* New socket pair */
	monitor_reinit(pmonitor);

	pmonitor->m_pid = fork();
	if (pmonitor->m_pid == -1)
		fatal("fork of unprivileged child failed");
	else if (pmonitor->m_pid != 0) {
		verbose("User child is on pid %ld", (long)pmonitor->m_pid);
		sshbuf_reset(loginmsg);
		monitor_child_postauth(pmonitor);

		/* NEVERREACHED */
		exit(0);
	}

	/* child */

	close(pmonitor->m_sendfd);
	pmonitor->m_sendfd = -1;

	/* Demote the private keys to public keys. */
	demote_sensitive_data();

	arc4random_buf(rnd, sizeof(rnd));
	RAND_seed(rnd, sizeof(rnd));
	explicit_bzero(rnd, sizeof(rnd));

	/* Drop privileges */
	do_setusercontext(authctxt->pw);

 skip:
	/* It is safe now to apply the key state */
	monitor_apply_keystate(pmonitor);

	/*
	 * Tell the packet layer that authentication was successful, since
	 * this information is not part of the key state.
	 */
	ssh_packet_set_authenticated(ssh);
}

static char *
list_hostkey_types(void)
{
	struct sshbuf *b;
	char *ret;
	int r, i;
	struct sshkey *key;

	if ((b = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	for (i = 0; i < options.num_host_key_files; i++) {
		key = sensitive_data.host_keys[i];
		if (key == NULL)
			continue;
		switch (key->type) {
		case KEY_RSA:
		case KEY_DSA:
		case KEY_ECDSA:
		case KEY_ED25519:
			if ((r = sshbuf_putf(b, "%s%s",
			    sshbuf_len(b) > 0 ? "," : "",
			    sshkey_ssh_name(key))) != 0)
				fatal("%s: buffer error: %s",
				    __func__, ssh_err(r));
			break;
		}
		/* If the private key has a cert peer, then list that too */
		key = sensitive_data.host_certificates[i];
		if (key == NULL)
			continue;
		switch (key->type) {
		case KEY_RSA_CERT_V00:
		case KEY_DSA_CERT_V00:
		case KEY_RSA_CERT:
		case KEY_DSA_CERT:
		case KEY_ECDSA_CERT:
		case KEY_ED25519_CERT:
			if ((r = sshbuf_putf(b, "%s%s",
			    sshbuf_len(b) > 0 ? "," : "",
			    sshkey_ssh_name(key))) != 0)
				fatal("%s: buffer error: %s",
				    __func__, ssh_err(r));
			break;
		}
	}
	if ((r = sshbuf_put_u8(b, 0)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	ret = xstrdup((const char *)sshbuf_ptr(b));
	sshbuf_free(b);
	debug("list_hostkey_types: %s", ret);
	return ret;
}

static struct sshkey *
get_hostkey_by_type(int type, int need_private)
{
	int i;
	struct sshkey *key;

	for (i = 0; i < options.num_host_key_files; i++) {
		switch (type) {
		case KEY_RSA_CERT_V00:
		case KEY_DSA_CERT_V00:
		case KEY_RSA_CERT:
		case KEY_DSA_CERT:
		case KEY_ECDSA_CERT:
		case KEY_ED25519_CERT:
			key = sensitive_data.host_certificates[i];
			break;
		default:
			key = sensitive_data.host_keys[i];
			if (key == NULL && !need_private)
				key = sensitive_data.host_pubkeys[i];
			break;
		}
		if (key != NULL && key->type == type)
			return need_private ?
			    sensitive_data.host_keys[i] : key;
	}
	return NULL;
}

struct sshkey *
get_hostkey_public_by_type(int type, struct ssh *ssh)
{
	return get_hostkey_by_type(type, 0);
}

struct sshkey *
get_hostkey_private_by_type(int type, struct ssh *ssh)
{
	return get_hostkey_by_type(type, 1);
}

struct sshkey *
get_hostkey_by_index(u_int ind, struct ssh *ssh)
{
	if (options.num_host_key_files < 0 ||
	    ind >= (u_int)options.num_host_key_files)
		return (NULL);
	return (sensitive_data.host_keys[ind]);
}

struct sshkey *
get_hostkey_public_by_index(int ind, struct ssh *ssh)
{
	if (ind < 0 || ind >= options.num_host_key_files)
		return (NULL);
	return (sensitive_data.host_pubkeys[ind]);
}

int
get_hostkey_index(struct sshkey *key, struct ssh *ssh)
{
	int i;

	for (i = 0; i < options.num_host_key_files; i++) {
		if (sshkey_is_cert(key)) {
			if (key == sensitive_data.host_certificates[i])
				return (i);
		} else {
			if (key == sensitive_data.host_keys[i])
				return (i);
			if (key == sensitive_data.host_pubkeys[i])
				return (i);
		}
	}
	return (-1);
}

/*
 * returns 1 if connection should be dropped, 0 otherwise.
 * dropping starts at connection #max_startups_begin with a probability
 * of (max_startups_rate/100). the probability increases linearly until
 * all connections are dropped for startups > max_startups
 */
static int
drop_connection(int startups)
{
	int p, r;

	if (startups < options.max_startups_begin)
		return 0;
	if (startups >= options.max_startups)
		return 1;
	if (options.max_startups_rate == 100)
		return 1;

	p  = 100 - options.max_startups_rate;
	p *= startups - options.max_startups_begin;
	p /= options.max_startups - options.max_startups_begin;
	p += options.max_startups_rate;
	r = arc4random_uniform(100);

	debug("drop_connection: p %d, r %d", p, r);
	return (r < p) ? 1 : 0;
}

static void
usage(void)
{
	fprintf(stderr, "%s, %s\n",
	    SSH_VERSION, SSLeay_version(SSLEAY_VERSION));
	fprintf(stderr,
"usage: sshd [-46DdeiqTt] [-b bits] [-C connection_spec] [-c host_cert_file]\n"
"            [-E log_file] [-f config_file] [-g login_grace_time]\n"
"            [-h host_key_file] [-k key_gen_time] [-o option] [-p port]\n"
"            [-u len]\n"
	);
	exit(1);
}

static void
send_rexec_state(int fd, struct sshbuf *conf)
{
	struct sshbuf *m;
	int r;

	debug3("%s: entering fd = %d config len %zu", __func__, fd,
	    sshbuf_len(conf));

	/*
	 * Protocol from reexec master to child:
	 *	string	configuration
	 *	u_int	ephemeral_key_follows
	 *	bignum	e		(only if ephemeral_key_follows == 1)
	 *	bignum	n			"
	 *	bignum	d			"
	 *	bignum	iqmp			"
	 *	bignum	p			"
	 *	bignum	q			"
	 */
	if ((m = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	/* servconf.c:load_server_config() ensures a \0 at the end of cfg */
	if ((r = sshbuf_put_cstring(m, (const char *)sshbuf_ptr(conf))) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));

	if (sensitive_data.server_key != NULL &&
	    sensitive_data.server_key->type == KEY_RSA1) {
		if ((r = sshbuf_put_u32(m, 1)) != 0 ||
		    (r = sshbuf_put_bignum1(m,
		    sensitive_data.server_key->rsa->e)) != 0 ||
		    (r = sshbuf_put_bignum1(m,
		    sensitive_data.server_key->rsa->n)) != 0 ||
		    (r = sshbuf_put_bignum1(m,
		    sensitive_data.server_key->rsa->d)) != 0 ||
		    (r = sshbuf_put_bignum1(m,
		    sensitive_data.server_key->rsa->iqmp)) != 0 ||
		    (r = sshbuf_put_bignum1(m,
		    sensitive_data.server_key->rsa->p)) != 0 ||
		    (r = sshbuf_put_bignum1(m,
		    sensitive_data.server_key->rsa->q)) != 0)
			fatal("%s: buffer error: %s", __func__, ssh_err(r));
	} else if ((r = sshbuf_put_u32(m, 0)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));

	if (ssh_msg_send(fd, 0, m) == -1)
		fatal("%s: ssh_msg_send failed", __func__);

	sshbuf_free(m);

	debug3("%s: done", __func__);
}

static void
recv_rexec_state(int fd, struct sshbuf *conf)
{
	struct sshbuf *m;
	char *cp;
	size_t len;
	int r;
	u_char ver;
	u_int key_follows;

	debug3("%s: entering fd = %d", __func__, fd);

	if ((m = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);

	if (ssh_msg_recv(fd, m) == -1)
		fatal("%s: ssh_msg_recv failed", __func__);
	if ((r = sshbuf_get_u8(m, &ver)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	if (ver != 0)
		fatal("%s: rexec version mismatch", __func__);
	if ((r = sshbuf_get_cstring(m, &cp, &len)) != 0 ||
	    (conf != NULL && (r = sshbuf_put(conf, cp, len + 1)) != 0) ||
	    (r = sshbuf_get_u32(m, &key_follows)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	free(cp);

	if (key_follows) {
		if (sensitive_data.server_key != NULL)
			sshkey_free(sensitive_data.server_key);
		sensitive_data.server_key = sshkey_new_private(KEY_RSA1);
		if (sensitive_data.server_key == NULL)
			fatal("%s: sshkey_new_private failed", __func__);
		if ((r = sshbuf_get_bignum1(m,
		    sensitive_data.server_key->rsa->e)) != 0 ||
		    (r = sshbuf_get_bignum1(m,
		    sensitive_data.server_key->rsa->n)) != 0 ||
		    (r = sshbuf_get_bignum1(m,
		    sensitive_data.server_key->rsa->d)) != 0 ||
		    (r = sshbuf_get_bignum1(m,
		    sensitive_data.server_key->rsa->iqmp)) != 0 ||
		    (r = sshbuf_get_bignum1(m,
		    sensitive_data.server_key->rsa->p)) != 0 ||
		    (r = sshbuf_get_bignum1(m,
		    sensitive_data.server_key->rsa->q)) != 0)
			fatal("%s: buffer error: %s", __func__, ssh_err(r));
		if ((r = rsa_generate_additional_parameters(
		    sensitive_data.server_key->rsa)) != 0)
			fatal("generate RSA parameters failed: %s", ssh_err(r));
	}
	sshbuf_free(m);

	debug3("%s: done", __func__);
}

/* Accept a connection from inetd */
static void
server_accept_inetd(int *sock_in, int *sock_out)
{
	int fd;

	startup_pipe = -1;
	if (rexeced_flag) {
		close(REEXEC_CONFIG_PASS_FD);
		*sock_in = *sock_out = dup(STDIN_FILENO);
		if (!debug_flag) {
			startup_pipe = dup(REEXEC_STARTUP_PIPE_FD);
			close(REEXEC_STARTUP_PIPE_FD);
		}
	} else {
		*sock_in = dup(STDIN_FILENO);
		*sock_out = dup(STDOUT_FILENO);
	}
	/*
	 * We intentionally do not close the descriptors 0, 1, and 2
	 * as our code for setting the descriptors won't work if
	 * ttyfd happens to be one of those.
	 */
	if ((fd = open(_PATH_DEVNULL, O_RDWR, 0)) != -1) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		if (!log_stderr)
			dup2(fd, STDERR_FILENO);
		if (fd > (log_stderr ? STDERR_FILENO : STDOUT_FILENO))
			close(fd);
	}
	debug("inetd sockets after dupping: %d, %d", *sock_in, *sock_out);
}

/*
 * Listen for TCP connections
 */
static void
server_listen(void)
{
	int ret, listen_sock, on = 1;
	struct addrinfo *ai;
	char ntop[NI_MAXHOST], strport[NI_MAXSERV];

	for (ai = options.listen_addrs; ai; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
			continue;
		if (num_listen_socks >= MAX_LISTEN_SOCKS)
			fatal("Too many listen sockets. "
			    "Enlarge MAX_LISTEN_SOCKS");
		if ((ret = getnameinfo(ai->ai_addr, ai->ai_addrlen,
		    ntop, sizeof(ntop), strport, sizeof(strport),
		    NI_NUMERICHOST|NI_NUMERICSERV)) != 0) {
			error("getnameinfo failed: %.100s",
			    ssh_gai_strerror(ret));
			continue;
		}
		/* Create socket for listening. */
		listen_sock = socket(ai->ai_family, ai->ai_socktype,
		    ai->ai_protocol);
		if (listen_sock < 0) {
			/* kernel may not support ipv6 */
			verbose("socket: %.100s", strerror(errno));
			continue;
		}
		if (set_nonblock(listen_sock) == -1) {
			close(listen_sock);
			continue;
		}
		/*
		 * Set socket options.
		 * Allow local port reuse in TIME_WAIT.
		 */
		if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
		    &on, sizeof(on)) == -1)
			error("setsockopt SO_REUSEADDR: %s", strerror(errno));

		debug("Bind to port %s on %s.", strport, ntop);

		/* Bind the socket to the desired port. */
		if (bind(listen_sock, ai->ai_addr, ai->ai_addrlen) < 0) {
			error("Bind to port %s on %s failed: %.200s.",
			    strport, ntop, strerror(errno));
			close(listen_sock);
			continue;
		}
		listen_socks[num_listen_socks] = listen_sock;
		num_listen_socks++;

		/* Start listening on the port. */
		if (listen(listen_sock, SSH_LISTEN_BACKLOG) < 0)
			fatal("listen on [%s]:%s: %.100s",
			    ntop, strport, strerror(errno));
		logit("Server listening on %s port %s.", ntop, strport);
	}
	freeaddrinfo(options.listen_addrs);

	if (!num_listen_socks)
		fatal("Cannot bind any address.");
}

/*
 * The main TCP accept loop. Note that, for the non-debug case, returns
 * from this function are in a forked subprocess.
 */
static void
server_accept_loop(int *sock_in, int *sock_out, int *newsock, int *config_s)
{
	fd_set *fdset;
	int i, j, ret, maxfd;
	int key_used = 0, startups = 0;
	int startup_p[2] = { -1 , -1 };
	struct sockaddr_storage from;
	socklen_t fromlen;
	pid_t pid;
	u_char rnd[256];

	/* setup fd set for accept */
	fdset = NULL;
	maxfd = 0;
	for (i = 0; i < num_listen_socks; i++)
		if (listen_socks[i] > maxfd)
			maxfd = listen_socks[i];
	/* pipes connected to unauthenticated childs */
	startup_pipes = xcalloc(options.max_startups, sizeof(int));
	for (i = 0; i < options.max_startups; i++)
		startup_pipes[i] = -1;

	/*
	 * Stay listening for connections until the system crashes or
	 * the daemon is killed with a signal.
	 */
	for (;;) {
		if (received_sighup)
			sighup_restart();
		if (fdset != NULL)
			free(fdset);
		fdset = (fd_set *)xcalloc(howmany(maxfd + 1, NFDBITS),
		    sizeof(fd_mask));

		for (i = 0; i < num_listen_socks; i++)
			FD_SET(listen_socks[i], fdset);
		for (i = 0; i < options.max_startups; i++)
			if (startup_pipes[i] != -1)
				FD_SET(startup_pipes[i], fdset);

		/* Wait in select until there is a connection. */
		ret = select(maxfd+1, fdset, NULL, NULL, NULL);
		if (ret < 0 && errno != EINTR)
			error("select: %.100s", strerror(errno));
		if (received_sigterm) {
			logit("Received signal %d; terminating.",
			    (int) received_sigterm);
			close_listen_socks();
			unlink(options.pid_file);
			exit(received_sigterm == SIGTERM ? 0 : 255);
		}
		if (key_used && key_do_regen) {
			generate_ephemeral_server_key();
			key_used = 0;
			key_do_regen = 0;
		}
		if (ret < 0)
			continue;

		for (i = 0; i < options.max_startups; i++)
			if (startup_pipes[i] != -1 &&
			    FD_ISSET(startup_pipes[i], fdset)) {
				/*
				 * the read end of the pipe is ready
				 * if the child has closed the pipe
				 * after successful authentication
				 * or if the child has died
				 */
				close(startup_pipes[i]);
				startup_pipes[i] = -1;
				startups--;
			}
		for (i = 0; i < num_listen_socks; i++) {
			if (!FD_ISSET(listen_socks[i], fdset))
				continue;
			fromlen = sizeof(from);
			*newsock = accept(listen_socks[i],
			    (struct sockaddr *)&from, &fromlen);
			if (*newsock < 0) {
				if (errno != EINTR && errno != EWOULDBLOCK &&
				    errno != ECONNABORTED)
					error("accept: %.100s",
					    strerror(errno));
				if (errno == EMFILE || errno == ENFILE)
					usleep(100 * 1000);
				continue;
			}
			if (unset_nonblock(*newsock) == -1) {
				close(*newsock);
				continue;
			}
			if (drop_connection(startups) == 1) {
				debug("drop connection #%d", startups);
				close(*newsock);
				continue;
			}
			if (pipe(startup_p) == -1) {
				close(*newsock);
				continue;
			}

			if (rexec_flag && socketpair(AF_UNIX,
			    SOCK_STREAM, 0, config_s) == -1) {
				error("reexec socketpair: %s",
				    strerror(errno));
				close(*newsock);
				close(startup_p[0]);
				close(startup_p[1]);
				continue;
			}

			for (j = 0; j < options.max_startups; j++)
				if (startup_pipes[j] == -1) {
					startup_pipes[j] = startup_p[0];
					if (maxfd < startup_p[0])
						maxfd = startup_p[0];
					startups++;
					break;
				}

			/*
			 * Got connection.  Fork a child to handle it, unless
			 * we are in debugging mode.
			 */
			if (debug_flag) {
				/*
				 * In debugging mode.  Close the listening
				 * socket, and start processing the
				 * connection without forking.
				 */
				debug("Server will not fork when running in debugging mode.");
				close_listen_socks();
				*sock_in = *newsock;
				*sock_out = *newsock;
				close(startup_p[0]);
				close(startup_p[1]);
				startup_pipe = -1;
				pid = getpid();
				if (rexec_flag) {
					send_rexec_state(config_s[0], cfg);
					close(config_s[0]);
				}
				break;
			}

			/*
			 * Normal production daemon.  Fork, and have
			 * the child process the connection. The
			 * parent continues listening.
			 */
			if ((pid = fork()) == 0) {
				/*
				 * Child.  Close the listening and
				 * max_startup sockets.  Start using
				 * the accepted socket. Reinitialize
				 * logging (since our pid has changed).
				 * We break out of the loop to handle
				 * the connection.
				 */
				startup_pipe = startup_p[1];
				close_startup_pipes();
				close_listen_socks();
				*sock_in = *newsock;
				*sock_out = *newsock;
				log_init(__progname,
				    options.log_level,
				    options.log_facility,
				    log_stderr);
				if (rexec_flag)
					close(config_s[0]);
				break;
			}

			/* Parent.  Stay in the loop. */
			if (pid < 0)
				error("fork: %.100s", strerror(errno));
			else
				debug("Forked child %ld.", (long)pid);

			close(startup_p[1]);

			if (rexec_flag) {
				send_rexec_state(config_s[0], cfg);
				close(config_s[0]);
				close(config_s[1]);
			}

			/*
			 * Mark that the key has been used (it
			 * was "given" to the child).
			 */
			if ((options.protocol & SSH_PROTO_1) &&
			    key_used == 0) {
				/* Schedule server key regeneration alarm. */
				signal(SIGALRM, key_regeneration_alarm);
				alarm(options.key_regeneration_time);
				key_used = 1;
			}

			close(*newsock);

			/*
			 * Ensure that our random state differs
			 * from that of the child
			 */
			arc4random_buf(rnd, sizeof(rnd));
			RAND_seed(rnd, sizeof(rnd));
			explicit_bzero(rnd, sizeof(rnd));
		}

		/* child process check (or debug mode) */
		if (num_listen_socks < 0)
			break;
	}
}


/*
 * Main program for the daemon.
 */
int
main(int ac, char **av)
{
	struct ssh *ssh;
	extern char *optarg;
	extern int optind;
	int opt, i, j, on = 1;
	int sock_in = -1, sock_out = -1, newsock = -1;
	const char *remote_ip;
	int r, remote_port;
	char *line, *logfile = NULL;
	int config_s[2] = { -1 , -1 };
	u_int n;
	u_int64_t ibytes, obytes;
	mode_t new_umask;
	struct sshkey *key, *pubkey;
	struct authctxt *authctxt;
	int keytype;
	struct connection_info *connection_info = get_connection_info(0, 0);

	/* Save argv. */
	saved_argv = av;
	rexec_argc = ac;

	/* Ensure that fds 0, 1 and 2 are open or directed to /dev/null */
	sanitise_stdfd();

	/* Initialize configuration options to their default values. */
	initialize_server_options(&options);

	/* Parse command-line arguments. */
	while ((opt = getopt(ac, av, "f:p:b:k:h:g:u:o:C:dDeE:iqrtQRT46")) != -1) {
		switch (opt) {
		case '4':
			options.address_family = AF_INET;
			break;
		case '6':
			options.address_family = AF_INET6;
			break;
		case 'f':
			config_file_name = optarg;
			break;
		case 'c':
			if (options.num_host_cert_files >= MAX_HOSTCERTS) {
				fprintf(stderr, "too many host certificates.\n");
				exit(1);
			}
			options.host_cert_files[options.num_host_cert_files++] =
			   derelativise_path(optarg);
			break;
		case 'd':
			if (debug_flag == 0) {
				debug_flag = 1;
				options.log_level = SYSLOG_LEVEL_DEBUG1;
			} else if (options.log_level < SYSLOG_LEVEL_DEBUG3)
				options.log_level++;
			break;
		case 'D':
			no_daemon_flag = 1;
			break;
		case 'E':
			logfile = xstrdup(optarg);
			/* FALLTHROUGH */
		case 'e':
			log_stderr = 1;
			break;
		case 'i':
			inetd_flag = 1;
			break;
		case 'r':
			rexec_flag = 0;
			break;
		case 'R':
			rexeced_flag = 1;
			inetd_flag = 1;
			break;
		case 'Q':
			/* ignored */
			break;
		case 'q':
			options.log_level = SYSLOG_LEVEL_QUIET;
			break;
		case 'b':
			options.server_key_bits = (int)strtonum(optarg, 256,
			    32768, NULL);
			break;
		case 'p':
			options.ports_from_cmdline = 1;
			if (options.num_ports >= MAX_PORTS) {
				fprintf(stderr, "too many ports.\n");
				exit(1);
			}
			options.ports[options.num_ports++] = a2port(optarg);
			if (options.ports[options.num_ports-1] <= 0) {
				fprintf(stderr, "Bad port number.\n");
				exit(1);
			}
			break;
		case 'g':
			if ((options.login_grace_time = convtime(optarg)) == -1) {
				fprintf(stderr, "Invalid login grace time.\n");
				exit(1);
			}
			break;
		case 'k':
			if ((options.key_regeneration_time = convtime(optarg)) == -1) {
				fprintf(stderr, "Invalid key regeneration interval.\n");
				exit(1);
			}
			break;
		case 'h':
			if (options.num_host_key_files >= MAX_HOSTKEYS) {
				fprintf(stderr, "too many host keys.\n");
				exit(1);
			}
			options.host_key_files[options.num_host_key_files++] =
			   derelativise_path(optarg);
			break;
		case 't':
			test_flag = 1;
			break;
		case 'T':
			test_flag = 2;
			break;
		case 'C':
			if (parse_server_match_testspec(connection_info,
			    optarg) == -1)
				exit(1);
			break;
		case 'u':
			utmp_len = (u_int)strtonum(optarg, 0, MAXHOSTNAMELEN+1, NULL);
			if (utmp_len > MAXHOSTNAMELEN) {
				fprintf(stderr, "Invalid utmp length.\n");
				exit(1);
			}
			break;
		case 'o':
			line = xstrdup(optarg);
			if (process_server_config_line(&options, line,
			    "command-line", 0, NULL, NULL) != 0)
				exit(1);
			free(line);
			break;
		case '?':
		default:
			usage();
			break;
		}
	}
	if (rexeced_flag || inetd_flag)
		rexec_flag = 0;
	if (!test_flag && (rexec_flag && (av[0] == NULL || *av[0] != '/')))
		fatal("sshd re-exec requires execution with an absolute path");
	if (rexeced_flag)
		closefrom(REEXEC_MIN_FREE_FD);
	else
		closefrom(REEXEC_DEVCRYPTO_RESERVED_FD);

	OpenSSL_add_all_algorithms();

	/* If requested, redirect the logs to the specified logfile. */
	if (logfile != NULL) {
		log_redirect_stderr_to(logfile);
		free(logfile);
	}
	/*
	 * Force logging to stderr until we have loaded the private host
	 * key (unless started from inetd)
	 */
	log_init(__progname,
	    options.log_level == SYSLOG_LEVEL_NOT_SET ?
	    SYSLOG_LEVEL_INFO : options.log_level,
	    options.log_facility == SYSLOG_FACILITY_NOT_SET ?
	    SYSLOG_FACILITY_AUTH : options.log_facility,
	    log_stderr || !inetd_flag);

	sensitive_data.server_key = NULL;
	sensitive_data.ssh1_host_key = NULL;
	sensitive_data.have_ssh1_key = 0;
	sensitive_data.have_ssh2_key = 0;

	/*
	 * If we're doing an extended config test, make sure we have all of
	 * the parameters we need.  If we're not doing an extended test,
	 * do not silently ignore connection test params.
	 */
	if (test_flag >= 2 && server_match_spec_complete(connection_info) == 0)
		fatal("user, host and addr are all required when testing "
		   "Match configs");
	if (test_flag < 2 && server_match_spec_complete(connection_info) >= 0)
		fatal("Config test connection parameter (-C) provided without "
		   "test mode (-T)");

	/* Fetch our configuration */
	if ((cfg = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	if (rexeced_flag)
		recv_rexec_state(REEXEC_CONFIG_PASS_FD, cfg);
	else
		load_server_config(config_file_name, cfg);

	parse_server_config(&options, rexeced_flag ? "rexec" : config_file_name,
	    cfg, NULL);

	/* Fill in default values for those options not explicitly set. */
	fill_default_server_options(&options);

	/* challenge-response is implemented via keyboard interactive */
	if (options.challenge_response_authentication)
		options.kbd_interactive_authentication = 1;

	/* Check that options are sensible */
	if (options.authorized_keys_command_user == NULL &&
	    (options.authorized_keys_command != NULL &&
	    strcasecmp(options.authorized_keys_command, "none") != 0))
		fatal("AuthorizedKeysCommand set without "
		    "AuthorizedKeysCommandUser");

	/*
	 * Check whether there is any path through configured auth methods.
	 * Unfortunately it is not possible to verify this generally before
	 * daemonisation in the presence of Match block, but this catches
	 * and warns for trivial misconfigurations that could break login.
	 */
	if (options.num_auth_methods != 0) {
		if ((options.protocol & SSH_PROTO_1))
			fatal("AuthenticationMethods is not supported with "
			    "SSH protocol 1");
		for (n = 0; n < options.num_auth_methods; n++) {
			if (auth2_methods_valid(options.auth_methods[n],
			    1) == 0)
				break;
		}
		if (n >= options.num_auth_methods)
			fatal("AuthenticationMethods cannot be satisfied by "
			    "enabled authentication methods");
	}

	/* set default channel AF */
	channel_set_af(options.address_family);

	/* Check that there are no remaining arguments. */
	if (optind < ac) {
		fprintf(stderr, "Extra argument %s.\n", av[optind]);
		exit(1);
	}

	debug("sshd version %s, %s", SSH_VERSION,
	    SSLeay_version(SSLEAY_VERSION));

	/* load host keys */
	sensitive_data.host_keys = xcalloc(options.num_host_key_files,
	    sizeof(struct sshkey *));
	sensitive_data.host_pubkeys = xcalloc(options.num_host_key_files,
	    sizeof(struct sshkey *));
	for (i = 0; i < options.num_host_key_files; i++) {
		sensitive_data.host_keys[i] = NULL;
		sensitive_data.host_pubkeys[i] = NULL;
	}

	if (options.host_key_agent) {
		if (strcmp(options.host_key_agent, SSH_AUTHSOCKET_ENV_NAME))
			setenv(SSH_AUTHSOCKET_ENV_NAME,
			    options.host_key_agent, 1);
		have_agent = ssh_get_authentication_socket(NULL);
	}

	for (i = 0; i < options.num_host_key_files; i++) {
		if ((r = sshkey_load_private(options.host_key_files[i], "",
		    &key, NULL)) != 0 && r != SSH_ERR_SYSTEM_ERROR)
			error("Error loading host key \"%s\": %s",
			    options.host_key_files[i], ssh_err(r));
		if ((r = sshkey_load_public(options.host_key_files[i],
		    &pubkey, NULL)) != 0 && r != SSH_ERR_SYSTEM_ERROR)
			error("Error loading host key \"%s\": %s",
			    options.host_key_files[i], ssh_err(r));

		if (key == NULL && pubkey != NULL && pubkey->type != KEY_RSA1 &&
		    have_agent) {
			debug("will rely on agent for hostkey %s",
			    options.host_key_files[i]);
			keytype = pubkey->type;
		} else if (key != NULL) {
			keytype = key->type;
		} else {
			error("Could not load host key: %s",
			    options.host_key_files[i]);
			sensitive_data.host_keys[i] = NULL;
			sensitive_data.host_pubkeys[i] = NULL;
			continue;
		}
		sensitive_data.host_keys[i] = key;
		sensitive_data.host_pubkeys[i] = pubkey;
		switch (keytype) {
		case KEY_RSA1:
			sensitive_data.ssh1_host_key = key;
			sensitive_data.have_ssh1_key = 1;
			break;
		case KEY_RSA:
		case KEY_DSA:
		case KEY_ECDSA:
		case KEY_ED25519:
			sensitive_data.have_ssh2_key = 1;
			break;
		}
		debug("private host key: #%d type %d %s", i, keytype,
		    sshkey_type(key ? key : pubkey));
	}
	if ((options.protocol & SSH_PROTO_1) && !sensitive_data.have_ssh1_key) {
		logit("Disabling protocol version 1. Could not load host key");
		options.protocol &= ~SSH_PROTO_1;
	}
	if ((options.protocol & SSH_PROTO_2) && !sensitive_data.have_ssh2_key) {
		logit("Disabling protocol version 2. Could not load host key");
		options.protocol &= ~SSH_PROTO_2;
	}
	if (!(options.protocol & (SSH_PROTO_1|SSH_PROTO_2))) {
		logit("sshd: no hostkeys available -- exiting.");
		exit(1);
	}

	/*
	 * Load certificates. They are stored in an array at identical
	 * indices to the public keys that they relate to.
	 */
	sensitive_data.host_certificates = xcalloc(options.num_host_key_files,
	    sizeof(struct sshkey *));
	for (i = 0; i < options.num_host_key_files; i++)
		sensitive_data.host_certificates[i] = NULL;

	for (i = 0; i < options.num_host_cert_files; i++) {
		if ((r = sshkey_load_public(options.host_cert_files[i],
		    &key, NULL)) != 0) {
			error("Could not load host certificate \"%s\": %s",
			    options.host_cert_files[i], ssh_err(r));
			continue;
		}
		if (!sshkey_is_cert(key)) {
			error("Certificate file is not a certificate: %s",
			    options.host_cert_files[i]);
			sshkey_free(key);
			continue;
		}
		/* Find matching private key */
		for (j = 0; j < options.num_host_key_files; j++) {
			if (sshkey_equal_public(key,
			    sensitive_data.host_keys[j])) {
				sensitive_data.host_certificates[j] = key;
				break;
			}
		}
		if (j >= options.num_host_key_files) {
			error("No matching private key for certificate: %s",
			    options.host_cert_files[i]);
			sshkey_free(key);
			continue;
		}
		sensitive_data.host_certificates[j] = key;
		debug("host certificate: #%d type %d %s", j, key->type,
		    sshkey_type(key));
	}
	/* Check certain values for sanity. */
	if (options.protocol & SSH_PROTO_1) {
		if (options.server_key_bits < 512 ||
		    options.server_key_bits > 32768) {
			fprintf(stderr, "Bad server key size.\n");
			exit(1);
		}
		/*
		 * Check that server and host key lengths differ sufficiently. This
		 * is necessary to make double encryption work with rsaref. Oh, I
		 * hate software patents. I dont know if this can go? Niels
		 */
		if (options.server_key_bits >
		    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n) -
		    SSH_KEY_BITS_RESERVED && options.server_key_bits <
		    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n) +
		    SSH_KEY_BITS_RESERVED) {
			options.server_key_bits =
			    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n) +
			    SSH_KEY_BITS_RESERVED;
			debug("Forcing server key to %d bits to make it differ from host key.",
			    options.server_key_bits);
		}
	}

	if (use_privsep) {
		struct stat st;

		if (getpwnam(SSH_PRIVSEP_USER) == NULL)
			fatal("Privilege separation user %s does not exist",
			    SSH_PRIVSEP_USER);
		if ((stat(_PATH_PRIVSEP_CHROOT_DIR, &st) == -1) ||
		    (S_ISDIR(st.st_mode) == 0))
			fatal("Missing privilege separation directory: %s",
			    _PATH_PRIVSEP_CHROOT_DIR);
		if (st.st_uid != 0 || (st.st_mode & (S_IWGRP|S_IWOTH)) != 0)
			fatal("%s must be owned by root and not group or "
			    "world-writable.", _PATH_PRIVSEP_CHROOT_DIR);
	}

	if (test_flag > 1) {
		if (server_match_spec_complete(connection_info) == 1)
			parse_server_match_config(&options, connection_info);
		dump_config(&options);
	}

	/* Configuration looks good, so exit if in test mode. */
	if (test_flag)
		exit(0);

	if (rexec_flag) {
		rexec_argv = xcalloc(rexec_argc + 2, sizeof(char *));
		for (i = 0; i < rexec_argc; i++) {
			debug("rexec_argv[%d]='%s'", i, saved_argv[i]);
			rexec_argv[i] = saved_argv[i];
		}
		rexec_argv[rexec_argc] = "-R";
		rexec_argv[rexec_argc + 1] = NULL;
	}

	/* Ensure that umask disallows at least group and world write */
	new_umask = umask(0077) | 0022;
	(void) umask(new_umask);

	/* Initialize the log (it is reinitialized below in case we forked). */
	if (debug_flag && (!inetd_flag || rexeced_flag))
		log_stderr = 1;
	log_init(__progname, options.log_level, options.log_facility, log_stderr);

	/*
	 * If not in debugging mode, and not started from inetd, disconnect
	 * from the controlling terminal, and fork.  The original process
	 * exits.
	 */
	if (!(debug_flag || inetd_flag || no_daemon_flag)) {
		int fd;

		if (daemon(0, 0) < 0)
			fatal("daemon() failed: %.200s", strerror(errno));

		/* Disconnect from the controlling tty. */
		fd = open(_PATH_TTY, O_RDWR | O_NOCTTY);
		if (fd >= 0) {
			(void) ioctl(fd, TIOCNOTTY, NULL);
			close(fd);
		}
	}
	/* Reinitialize the log (because of the fork above). */
	log_init(__progname, options.log_level, options.log_facility, log_stderr);

	/* Chdir to the root directory so that the current disk can be
	   unmounted if desired. */
	if (chdir("/") == -1)
		error("chdir(\"/\"): %s", strerror(errno));

	/* ignore SIGPIPE */
	signal(SIGPIPE, SIG_IGN);

	/* Get a connection, either from inetd or a listening TCP socket */
	if (inetd_flag) {
		server_accept_inetd(&sock_in, &sock_out);
	} else {
		server_listen();

		if (options.protocol & SSH_PROTO_1)
			generate_ephemeral_server_key();

		signal(SIGHUP, sighup_handler);
		signal(SIGCHLD, main_sigchld_handler);
		signal(SIGTERM, sigterm_handler);
		signal(SIGQUIT, sigterm_handler);

		/*
		 * Write out the pid file after the sigterm handler
		 * is setup and the listen sockets are bound
		 */
		if (!debug_flag) {
			FILE *f = fopen(options.pid_file, "w");

			if (f == NULL) {
				error("Couldn't create pid file \"%s\": %s",
				    options.pid_file, strerror(errno));
			} else {
				fprintf(f, "%ld\n", (long) getpid());
				fclose(f);
			}
		}

		/* Accept a connection and return in a forked child */
		server_accept_loop(&sock_in, &sock_out,
		    &newsock, config_s);
	}

	/* This is the child processing a new connection. */
	setproctitle("%s", "[accepted]");

	/*
	 * Create a new session and process group since the 4.4BSD
	 * setlogin() affects the entire process group.  We don't
	 * want the child to be able to affect the parent.
	 */
	if (!debug_flag && !inetd_flag && setsid() < 0)
		error("setsid: %.100s", strerror(errno));

	if (rexec_flag) {
		int fd;

		debug("rexec start in %d out %d newsock %d pipe %d sock %d",
		    sock_in, sock_out, newsock, startup_pipe, config_s[0]);
		dup2(newsock, STDIN_FILENO);
		dup2(STDIN_FILENO, STDOUT_FILENO);
		if (startup_pipe == -1)
			close(REEXEC_STARTUP_PIPE_FD);
		else if (startup_pipe != REEXEC_STARTUP_PIPE_FD) {
			dup2(startup_pipe, REEXEC_STARTUP_PIPE_FD);
			close(startup_pipe);
			startup_pipe = REEXEC_STARTUP_PIPE_FD;
		}

		dup2(config_s[1], REEXEC_CONFIG_PASS_FD);
		close(config_s[1]);

		execv(rexec_argv[0], rexec_argv);

		/* Reexec has failed, fall back and continue */
		error("rexec of %s failed: %s", rexec_argv[0], strerror(errno));
		recv_rexec_state(REEXEC_CONFIG_PASS_FD, NULL);
		log_init(__progname, options.log_level,
		    options.log_facility, log_stderr);

		/* Clean up fds */
		close(REEXEC_CONFIG_PASS_FD);
		newsock = sock_out = sock_in = dup(STDIN_FILENO);
		if ((fd = open(_PATH_DEVNULL, O_RDWR, 0)) != -1) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		debug("rexec cleanup in %d out %d newsock %d pipe %d sock %d",
		    sock_in, sock_out, newsock, startup_pipe, config_s[0]);
	}

	/* Executed child processes don't need these. */
	fcntl(sock_out, F_SETFD, FD_CLOEXEC);
	fcntl(sock_in, F_SETFD, FD_CLOEXEC);

	/*
	 * Disable the key regeneration alarm.  We will not regenerate the
	 * key since we are no longer in a position to give it to anyone. We
	 * will not restart on SIGHUP since it no longer makes sense.
	 */
	alarm(0);
	signal(SIGALRM, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);

	/*
	 * Register our connection.  This turns encryption off because we do
	 * not have a key.
	 */
	ssh = ssh_packet_set_connection(NULL, sock_in, sock_out);
	ssh_packet_set_server(ssh);
	active_state = ssh; /* XXX */

	/* Set SO_KEEPALIVE if requested. */
	if (options.tcp_keep_alive && ssh_packet_connection_is_on_socket(ssh) &&
	    setsockopt(sock_in, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0)
		error("setsockopt SO_KEEPALIVE: %.100s", strerror(errno));

	if ((remote_port = ssh_get_remote_port(ssh)) < 0) {
		debug("get_remote_port failed");
		cleanup_exit(255);
	}

	/*
	 * We use get_canonical_hostname with usedns = 0 instead of
	 * get_remote_ipaddr here so IP options will be checked.
	 */
	(void) get_canonical_hostname(0);
	/*
	 * The rest of the code depends on the fact that
	 * get_remote_ipaddr() caches the remote ip, even if
	 * the socket goes away.
	 */
	remote_ip = ssh_remote_ipaddr(ssh);

#ifdef LIBWRAP
	/* Check whether logins are denied from this host. */
	if (ssh_packet_connection_is_on_socket(ssh)) {
		struct request_info req;

		request_init(&req, RQ_DAEMON, __progname, RQ_FILE, sock_in, 0);
		fromhost(&req);

		if (!hosts_access(&req)) {
			debug("Connection refused by tcp wrapper");
			refuse(&req);
			/* NOTREACHED */
			fatal("libwrap refuse returns");
		}
	}
#endif /* LIBWRAP */

	/* Log the connection. */
	verbose("Connection from %s port %d on %s port %d",
	    remote_ip, remote_port,
	    get_local_ipaddr(sock_in), ssh_get_local_port(ssh));

	/*
	 * We don't want to listen forever unless the other side
	 * successfully authenticates itself.  So we set up an alarm which is
	 * cleared after successful authentication.  A limit of zero
	 * indicates no limit. Note that we don't set the alarm in debugging
	 * mode; it is just annoying to have the server exit just when you
	 * are about to discover the bug.
	 */
	signal(SIGALRM, grace_alarm_handler);
	if (!debug_flag)
		alarm(options.login_grace_time);

	sshd_exchange_identification(ssh, sock_in, sock_out);

	/* In inetd mode, generate ephemeral key only for proto 1 connections */
	if (!compat20 && inetd_flag && sensitive_data.server_key == NULL)
		generate_ephemeral_server_key();

	ssh_packet_set_nonblocking(ssh);

	/* allocate authentication context */
	authctxt = xcalloc(1, sizeof(*authctxt));

	/* XXX global for cleanup, access from other modules */
	the_authctxt = authctxt;
	ssh->authctxt = authctxt;

	/* prepare buffer to collect messages to display to user after login */
	if ((loginmsg = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	auth_debug_reset();

	if (use_privsep) {
		if (privsep_preauth(authctxt) == 1)
			goto authenticated;
	} else if (compat20 && have_agent) {
		if ((r = ssh_get_authentication_socket(&auth_sock)) != 0) {
			error("Unable to get agent socket: %s", ssh_err(r));
			have_agent = -1;
		}
	}

	/* perform the key exchange */
	/* authenticate user and start session */
	if (compat20) {
		do_ssh2_kex(ssh);
		do_authentication2(ssh);
	} else {
		do_ssh1_kex(ssh);
		do_authentication(ssh);
	}
	/*
	 * If we use privilege separation, the unprivileged child transfers
	 * the current keystate and exits
	 */
	if (use_privsep) {
		mm_send_keystate(pmonitor);
		exit(0);
	}

 authenticated:
	/*
	 * Cancel the alarm we set to limit the time taken for
	 * authentication.
	 */
	alarm(0);
	signal(SIGALRM, SIG_DFL);
	authctxt->authenticated = 1;
	if (startup_pipe != -1) {
		close(startup_pipe);
		startup_pipe = -1;
	}

	/*
	 * In privilege separation, we fork another child and prepare
	 * file descriptor passing.
	 */
	if (use_privsep) {
		privsep_postauth(ssh);
		/* the monitor process [priv] will not return */
		if (!compat20)
			destroy_sensitive_data();
	}

	ssh_packet_set_timeout(ssh, options.client_alive_interval,
	    options.client_alive_count_max);

	/* Start session. */
	do_authenticated(ssh);

	/* The connection has been terminated. */
	ssh_packet_get_bytes(ssh, &ibytes, &obytes);
	verbose("Transferred: sent %llu, received %llu bytes",
	    (unsigned long long)obytes, (unsigned long long)ibytes);

	verbose("Closing connection to %.500s port %d", remote_ip, remote_port);
	ssh_packet_close(ssh);

	if (use_privsep)
		mm_terminate();

	exit(0);
}

/*
 * Decrypt session_key_int using our private server key and private host key
 * (key with larger modulus first).
 */
int
ssh1_session_key(BIGNUM *session_key_int)
{
	int r, rsafail = 0;
	struct ssh *ssh = active_state; /* XXX */

	if (BN_cmp(sensitive_data.server_key->rsa->n,
	    sensitive_data.ssh1_host_key->rsa->n) > 0) {
		/* Server key has bigger modulus. */
		if (BN_num_bits(sensitive_data.server_key->rsa->n) <
		    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n) +
		    SSH_KEY_BITS_RESERVED) {
			fatal("do_connection: %s: "
			    "server_key %d < host_key %d + SSH_KEY_BITS_RESERVED %d",
			    ssh_remote_ipaddr(ssh),
			    BN_num_bits(sensitive_data.server_key->rsa->n),
			    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n),
			    SSH_KEY_BITS_RESERVED);
		}
		if ((r = rsa_private_decrypt(session_key_int, session_key_int,
		    sensitive_data.server_key->rsa) != 0)) {
			error("%s: rsa_private_decrypt: %s",
			    __func__, ssh_err(r));
			rsafail++;
		}
		if ((r = rsa_private_decrypt(session_key_int, session_key_int,
		    sensitive_data.ssh1_host_key->rsa) != 0)) {
			error("%s: rsa_private_decrypt: %s",
			    __func__, ssh_err(r));
			rsafail++;
		}
	} else {
		/* Host key has bigger modulus (or they are equal). */
		if (BN_num_bits(sensitive_data.ssh1_host_key->rsa->n) <
		    BN_num_bits(sensitive_data.server_key->rsa->n) +
		    SSH_KEY_BITS_RESERVED) {
			fatal("do_connection: %s: "
			    "host_key %d < server_key %d + SSH_KEY_BITS_RESERVED %d",
			    ssh_remote_ipaddr(ssh),
			    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n),
			    BN_num_bits(sensitive_data.server_key->rsa->n),
			    SSH_KEY_BITS_RESERVED);
		}
		if ((r = rsa_private_decrypt(session_key_int, session_key_int,
		    sensitive_data.ssh1_host_key->rsa)) != 0) {
			error("%s: rsa_private_decrypt: %s",
			    __func__, ssh_err(r));
			rsafail++;
		}
		if ((r = rsa_private_decrypt(session_key_int, session_key_int,
		    sensitive_data.server_key->rsa)) != 0) {
			error("%s: rsa_private_decrypt: %s",
			    __func__, ssh_err(r));
			rsafail++;
		}
	}
	return (rsafail);
}

/*
 * SSH1 key exchange
 */
static void
do_ssh1_kex(struct ssh *ssh)
{
	int i, len, r, rsafail = 0;
	BIGNUM *session_key_int;
	u_char session_key[SSH_SESSION_KEY_LENGTH], cookie[8], check[8];
	u_int auth_mask, protocol_flags;
	u_char cipher_type;

	/*
	 * Generate check bytes that the client must send back in the user
	 * packet in order for it to be accepted; this is used to defy ip
	 * spoofing attacks.  Note that this only works against somebody
	 * doing IP spoofing from a remote machine; any machine on the local
	 * network can still see outgoing packets and catch the random
	 * cookie.  This only affects rhosts authentication, and this is one
	 * of the reasons why it is inherently insecure.
	 */
	arc4random_buf(cookie, sizeof(cookie));

	auth_mask = 0;
	if (options.rhosts_rsa_authentication)
		auth_mask |= 1 << SSH_AUTH_RHOSTS_RSA;
	if (options.rsa_authentication)
		auth_mask |= 1 << SSH_AUTH_RSA;
	if (options.challenge_response_authentication == 1)
		auth_mask |= 1 << SSH_AUTH_TIS;
	if (options.password_authentication)
		auth_mask |= 1 << SSH_AUTH_PASSWORD;

	/*
	 * Send our public key.  We include in the packet 64 bits of random
	 * data that must be matched in the reply in order to prevent IP
	 * spoofing.
	 */
	if ((r = sshpkt_start(ssh, SSH_SMSG_PUBLIC_KEY)) != 0 ||
	    (r = sshpkt_put(ssh, cookie, sizeof(cookie))) != 0 ||
	    /* Store our public server RSA key. */
	    (r = sshpkt_put_u32(ssh,
	    BN_num_bits(sensitive_data.server_key->rsa->n))) != 0 ||
	    (r = sshpkt_put_bignum1(ssh,
	    sensitive_data.server_key->rsa->e)) != 0 ||
	    (r = sshpkt_put_bignum1(ssh,
	    sensitive_data.server_key->rsa->n)) != 0 ||
	    /* Store our public host RSA key. */
	    (r = sshpkt_put_u32(ssh,
	    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n))) != 0 ||
	    (r = sshpkt_put_bignum1(ssh,
	    sensitive_data.ssh1_host_key->rsa->e)) != 0 ||
	    (r = sshpkt_put_bignum1(ssh,
	    sensitive_data.ssh1_host_key->rsa->n)) != 0 ||
	    /* Put protocol flags. */
	    (r = sshpkt_put_u32(ssh, SSH_PROTOFLAG_HOST_IN_FWD_OPEN)) != 0 ||
	    /* Declare which ciphers we support. */
	    (r = sshpkt_put_u32(ssh, cipher_mask_ssh1(0))) != 0 ||
	    /* Declare supported authentication types. */
	    (r = sshpkt_put_u32(ssh, auth_mask)) != 0 ||
	    /* Send the packet and wait for it to be sent. */
	    (r = sshpkt_send(ssh)) != 0)
		fatal("%s: %s", __func__, ssh_err(r));
	ssh_packet_write_wait(ssh);

	debug("Sent %d bit server key and %d bit host key.",
	    BN_num_bits(sensitive_data.server_key->rsa->n),
	    BN_num_bits(sensitive_data.ssh1_host_key->rsa->n));

	/* Read clients reply (cipher type and session key). */
	ssh_packet_read_expect(ssh, SSH_CMSG_SESSION_KEY);

	if ((r = sshpkt_get_u8(ssh, &cipher_type)) != 0 ||
	    (r = sshpkt_get(ssh, check, sizeof(check))) != 0)
		fatal("%s: %s", __func__, ssh_err(r));

	/* Get cipher type and check whether we accept this. */
	if (!(cipher_mask_ssh1(0) & (1 << cipher_type)))
		ssh_packet_disconnect(ssh,
		    "Warning: client selects unsupported cipher.");

	/* Get check bytes from the packet.  These must match those we
	   sent earlier with the public key packet. */
        if (timingsafe_bcmp(check, cookie, sizeof(check)) != 0)
		ssh_packet_disconnect(ssh,
		    "IP Spoofing check bytes do not match.");

	debug("Encryption type: %.200s", cipher_name(cipher_type));

	/* Get the encrypted integer. */
	if ((session_key_int = BN_new()) == NULL)
		fatal("do_ssh1_kex: BN_new failed");
	if ((r = sshpkt_get_bignum1(ssh, session_key_int)) != 0 ||
	    (r = sshpkt_get_u32(ssh, &protocol_flags)) != 0 ||
	    (r = sshpkt_get_end(ssh)) != 0)
		fatal("%s: %s", __func__, ssh_err(r));
	ssh_packet_set_protocol_flags(ssh, protocol_flags);

	/* Decrypt session_key_int using host/server keys */
	rsafail = PRIVSEP(ssh1_session_key(session_key_int));

	/*
	 * Extract session key from the decrypted integer.  The key is in the
	 * least significant 256 bits of the integer; the first byte of the
	 * key is in the highest bits.
	 */
	if (!rsafail) {
		(void) BN_mask_bits(session_key_int, sizeof(session_key) * 8);
		len = BN_num_bytes(session_key_int);
		if (len < 0 || (u_int)len > sizeof(session_key)) {
			error("do_ssh1_kex: bad session key len from %s: "
			    "session_key_int %d > sizeof(session_key) %lu",
			    ssh_remote_ipaddr(ssh), len,
			    (u_long)sizeof(session_key));
			rsafail++;
		} else {
			explicit_bzero(session_key, sizeof(session_key));
			BN_bn2bin(session_key_int,
			    session_key + sizeof(session_key) - len);

			if ((r = derive_ssh1_session_id(
			    sensitive_data.ssh1_host_key->rsa->n,
			    sensitive_data.server_key->rsa->n,
			    cookie, session_id)) != 0)
				fatal("derive_ssh1_session_id: %s", ssh_err(r));
			/*
			 * Xor the first 16 bytes of the session key with the
			 * session id.
			 */
			for (i = 0; i < 16; i++)
				session_key[i] ^= session_id[i];
		}
	}
	if (rsafail) {
		int bytes = BN_num_bytes(session_key_int);
		u_char *buf = xmalloc(bytes);
		struct ssh_digest_ctx *md;

		logit("do_connection: generating a fake encryption key");
		BN_bn2bin(session_key_int, buf);
		if ((md = ssh_digest_start(SSH_DIGEST_MD5)) == NULL ||
		    ssh_digest_update(md, buf, bytes) < 0 ||
		    ssh_digest_update(md, sensitive_data.ssh1_cookie,
		    SSH_SESSION_KEY_LENGTH) < 0 ||
		    ssh_digest_final(md, session_key, sizeof(session_key)) < 0)
			fatal("%s: md5 failed", __func__);
		ssh_digest_free(md);
		if ((md = ssh_digest_start(SSH_DIGEST_MD5)) == NULL ||
		    ssh_digest_update(md, session_key, 16) < 0 ||
		    ssh_digest_update(md, sensitive_data.ssh1_cookie,
		    SSH_SESSION_KEY_LENGTH) < 0 ||
		    ssh_digest_final(md, session_key + 16,
		    sizeof(session_key) - 16) < 0)
			fatal("%s: md5 failed", __func__);
		ssh_digest_free(md);
		explicit_bzero(buf, bytes);
		free(buf);
		for (i = 0; i < 16; i++)
			session_id[i] = session_key[i] ^ session_key[i + 16];
	}
	/* Destroy the private and public keys. No longer. */
	destroy_sensitive_data();

	if (use_privsep)
		mm_ssh1_session_id(session_id);

	/* Destroy the decrypted integer.  It is no longer needed. */
	BN_clear_free(session_key_int);

	/* Set the session key.  From this on all communications will be encrypted. */
	ssh_packet_set_encryption_key(ssh, session_key, SSH_SESSION_KEY_LENGTH,
	    cipher_type);

	/* Destroy our copy of the session key.  It is no longer needed. */
	explicit_bzero(session_key, sizeof(session_key));

	debug("Received session key; encryption turned on.");

	/* Send an acknowledgment packet.  Note that this packet is sent encrypted. */
	if ((r = sshpkt_start(ssh, SSH_SMSG_SUCCESS)) != 0 ||
	    (r = sshpkt_send(ssh)) != 0)
		fatal("%s: %s", __func__, ssh_err(r));
	ssh_packet_write_wait(ssh);
}

int
sshd_hostkey_sign(struct sshkey *privkey, struct sshkey *pubkey,
    u_char **signature, size_t *slen, u_char *data, size_t dlen, u_int compat)
{
	if (privkey) {
		return PRIVSEP(sshkey_sign(privkey, signature, slen,
		    data, dlen, compat));
	} else if (use_privsep) {
		return mm_sshkey_sign(pubkey, signature, slen, data, dlen,
		    compat);
	} else {
		return ssh_agent_sign(auth_sock, pubkey, signature, slen, data,
		    dlen, compat);
	}
}

/*
 * SSH2 key exchange: diffie-hellman-group1-sha1
 */
static void
do_ssh2_kex(struct ssh *ssh)
{
	struct kex *kex;
	int r;

	if (options.ciphers != NULL) {
		myproposal[PROPOSAL_ENC_ALGS_CTOS] =
		myproposal[PROPOSAL_ENC_ALGS_STOC] = options.ciphers;
	}
	myproposal[PROPOSAL_ENC_ALGS_CTOS] =
	    compat_cipher_proposal(myproposal[PROPOSAL_ENC_ALGS_CTOS],
	    ssh->compat);
	myproposal[PROPOSAL_ENC_ALGS_STOC] =
	    compat_cipher_proposal(myproposal[PROPOSAL_ENC_ALGS_STOC],
	    ssh->compat);
	if (myproposal[PROPOSAL_ENC_ALGS_CTOS] == NULL ||
	    myproposal[PROPOSAL_ENC_ALGS_STOC] == NULL)
		fatal("no compatible ciphers found");

	if (options.macs != NULL) {
		myproposal[PROPOSAL_MAC_ALGS_CTOS] =
		myproposal[PROPOSAL_MAC_ALGS_STOC] = options.macs;
	}
	if (options.compression == COMP_NONE) {
		myproposal[PROPOSAL_COMP_ALGS_CTOS] =
		myproposal[PROPOSAL_COMP_ALGS_STOC] = "none";
	} else if (options.compression == COMP_DELAYED) {
		myproposal[PROPOSAL_COMP_ALGS_CTOS] =
		myproposal[PROPOSAL_COMP_ALGS_STOC] = "none,zlib@openssh.com";
	}
	if (options.kex_algorithms != NULL)
		myproposal[PROPOSAL_KEX_ALGS] = options.kex_algorithms;

	if (options.rekey_limit || options.rekey_interval)
		ssh_packet_set_rekey_limits(ssh,
		    (u_int32_t)options.rekey_limit,
		    (time_t)options.rekey_interval);

	myproposal[PROPOSAL_SERVER_HOST_KEY_ALGS] = compat_pkalg_proposal(
	    list_hostkey_types(), ssh->compat);

	/* start key exchange */
	if ((r = kex_setup(ssh, myproposal)) != 0)
		fatal("kex_setup: %s", ssh_err(r));
	kex = ssh->kex;
	kex->kex[KEX_DH_GRP1_SHA1] = kexdh_server;
	kex->kex[KEX_DH_GRP14_SHA1] = kexdh_server;
	kex->kex[KEX_DH_GEX_SHA1] = kexgex_server;
	kex->kex[KEX_DH_GEX_SHA256] = kexgex_server;
	kex->kex[KEX_ECDH_SHA2] = kexecdh_server;
	kex->kex[KEX_C25519_SHA256] = kexc25519_server;
	kex->server = 1;
	kex->client_version_string=client_version_string;
	kex->server_version_string=server_version_string;
	kex->load_host_public_key=&get_hostkey_public_by_type;
	kex->load_host_private_key=&get_hostkey_private_by_type;
	kex->host_key_index=&get_hostkey_index;
	kex->sign = sshd_hostkey_sign;

	if ((r = ssh_dispatch_run(ssh, DISPATCH_BLOCK,
	    &kex->done)) != 0)
		fatal("%s: key exchange failed: %s", __func__, ssh_err(r));

	session_id2 = kex->session_id;
	session_id2_len = kex->session_id_len;

#ifdef DEBUG_KEXDH
	/* send 1st encrypted/maced/compressed message */
	if ((r = sshpkt_start(ssh, SSH2_MSG_IGNORE)) != 0 ||
	    (r = sshpkt_put_cstring(ssh, "markus")) != 0 ||
	    (r = sshpkt_send(ssh)) != 0)
		fatal("%s: %s", __func__, ssh_err(r));
	ssh_packet_write_wait(ssh);
#endif
	debug("KEX done");
}

/* server specific fatal cleanup */
void
cleanup_exit(int i)
{
	channel_close_all();

	if (the_authctxt) {
		do_cleanup(the_authctxt);
		if (use_privsep && privsep_is_preauth && pmonitor->m_pid > 1) {
			debug("Killing privsep child %d", pmonitor->m_pid);
			if (kill(pmonitor->m_pid, SIGKILL) != 0 &&
			    errno != ESRCH)
				error("%s: kill(%d): %s", __func__,
				    pmonitor->m_pid, strerror(errno));
		}
	}
	_exit(i);
}
