/* $OpenBSD: sshconnect.c,v 1.247 2014/04/01 03:34:10 djm Exp $ */
/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Code to connect to a remote host, and to perform the client side of the
 * login (authentication) dialog.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <paths.h>
#include <signal.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xmalloc.h"
#include "ssh.h"
#include "rsa.h"
#include "sshbuf.h"
#include "packet.h"
#include "uidswap.h"
#include "compat.h"
#include "key.h"
#include "sshconnect.h"
#include "hostfile.h"
#include "log.h"
#include "readconf.h"
#include "atomicio.h"
#include "misc.h"
#include "dns.h"
#include "roaming.h"
#include "monitor_fdpass.h"
#include "ssh2.h"
#include "version.h"
#include "err.h"

char *client_version_string = NULL;
char *server_version_string = NULL;

static int matching_host_key_dns = 0;

static pid_t proxy_command_pid = 0;

/* import */
extern Options options;
extern char *__progname;
extern uid_t original_real_uid;
extern uid_t original_effective_uid;

static int show_other_keys(struct hostkeys *, struct sshkey *);
static void warn_changed_key(struct sshkey *);

/* Expand a proxy command */
static char *
expand_proxy_command(const char *proxy_command, const char *user,
    const char *host, int port)
{
	char *tmp, *ret, strport[NI_MAXSERV];

	snprintf(strport, sizeof strport, "%d", port);
	xasprintf(&tmp, "exec %s", proxy_command);
	ret = percent_expand(tmp, "h", host, "p", strport,
	    "r", options.user, (char *)NULL);
	free(tmp);
	return ret;
}

/*
 * Connect to the given ssh server using a proxy command that passes a
 * a connected fd back to us.
 */
static struct ssh *
ssh_proxy_fdpass_connect(const char *host, u_short port,
    const char *proxy_command)
{
	struct ssh *ssh;
	char *command_string;
	int sp[2], sock;
	pid_t pid;
	char *shell;

	if ((shell = getenv("SHELL")) == NULL)
		shell = _PATH_BSHELL;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
		fatal("Could not create socketpair to communicate with "
		    "proxy dialer: %.100s", strerror(errno));

	command_string = expand_proxy_command(proxy_command, options.user,
	    host, port);
	debug("Executing proxy dialer command: %.500s", command_string);

	/* Fork and execute the proxy command. */
	if ((pid = fork()) == 0) {
		char *argv[10];

		/* Child.  Permanently give up superuser privileges. */
		permanently_drop_suid(original_real_uid);

		close(sp[1]);
		/* Redirect stdin and stdout. */
		if (sp[0] != 0) {
			if (dup2(sp[0], 0) < 0)
				perror("dup2 stdin");
		}
		if (sp[0] != 1) {
			if (dup2(sp[0], 1) < 0)
				perror("dup2 stdout");
		}
		if (sp[0] >= 2)
			close(sp[0]);

		/*
		 * Stderr is left as it is so that error messages get
		 * printed on the user's terminal.
		 */
		argv[0] = shell;
		argv[1] = "-c";
		argv[2] = command_string;
		argv[3] = NULL;

		/*
		 * Execute the proxy command.
		 * Note that we gave up any extra privileges above.
		 */
		execv(argv[0], argv);
		perror(argv[0]);
		exit(1);
	}
	/* Parent. */
	if (pid < 0)
		fatal("fork failed: %.100s", strerror(errno));
	close(sp[0]);
	free(command_string);

	if ((sock = mm_receive_fd(sp[1])) == -1)
		fatal("proxy dialer did not pass back a connection");

	while (waitpid(pid, NULL, 0) == -1)
		if (errno != EINTR)
			fatal("Couldn't wait for child: %s", strerror(errno));

	/* Set the connection file descriptors. */
	ssh = ssh_packet_set_connection(NULL, sock, sock);

	return ssh;
}

/*
 * Connect to the given ssh server using a proxy command.
 */
static struct ssh *
ssh_proxy_connect(const char *host, u_short port, const char *proxy_command)
{
	struct ssh *ssh;
	char *shell, *command_string;
	int pin[2], pout[2];
	pid_t pid;

	if ((shell = getenv("SHELL")) == NULL || *shell == '\0')
		shell = _PATH_BSHELL;

	/* Create pipes for communicating with the proxy. */
	if (pipe(pin) < 0 || pipe(pout) < 0)
		fatal("Could not create pipes to communicate with the proxy: %.100s",
		    strerror(errno));

	command_string = expand_proxy_command(proxy_command, options.user,
	    host, port);
	debug("Executing proxy command: %.500s", command_string);

	/* Fork and execute the proxy command. */
	if ((pid = fork()) == 0) {
		char *argv[10];

		/* Child.  Permanently give up superuser privileges. */
		permanently_drop_suid(original_real_uid);

		/* Redirect stdin and stdout. */
		close(pin[1]);
		if (pin[0] != 0) {
			if (dup2(pin[0], 0) < 0)
				perror("dup2 stdin");
			close(pin[0]);
		}
		close(pout[0]);
		if (dup2(pout[1], 1) < 0)
			perror("dup2 stdout");
		/* Cannot be 1 because pin allocated two descriptors. */
		close(pout[1]);

		/* Stderr is left as it is so that error messages get
		   printed on the user's terminal. */
		argv[0] = shell;
		argv[1] = "-c";
		argv[2] = command_string;
		argv[3] = NULL;

		/* Execute the proxy command.  Note that we gave up any
		   extra privileges above. */
		signal(SIGPIPE, SIG_DFL);
		execv(argv[0], argv);
		perror(argv[0]);
		exit(1);
	}
	/* Parent. */
	if (pid < 0)
		fatal("fork failed: %.100s", strerror(errno));
	else
		proxy_command_pid = pid; /* save pid to clean up later */

	/* Close child side of the descriptors. */
	close(pin[0]);
	close(pout[1]);

	/* Free the command name. */
	free(command_string);

	/* Set the connection file descriptors. */
	ssh = ssh_packet_set_connection(NULL, pout[0], pin[1]);

	/* Indicate OK return */
	return (ssh);
}

void
ssh_kill_proxy_command(void)
{
	/*
	 * Send SIGHUP to proxy command if used. We don't wait() in
	 * case it hangs and instead rely on init to reap the child
	 */
	if (proxy_command_pid > 1)
		kill(proxy_command_pid, SIGHUP);
}

/*
 * Creates a (possibly privileged) socket for use as the ssh connection.
 */
static int
ssh_create_socket(int privileged, struct addrinfo *ai)
{
	int sock, r, gaierr;
	struct addrinfo hints, *res = NULL;

	sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (sock < 0) {
		error("socket: %s", strerror(errno));
		return -1;
	}
	fcntl(sock, F_SETFD, FD_CLOEXEC);

	/* Bind the socket to an alternative local IP address */
	if (options.bind_address == NULL && !privileged)
		return sock;

	if (options.bind_address) {
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = ai->ai_family;
		hints.ai_socktype = ai->ai_socktype;
		hints.ai_protocol = ai->ai_protocol;
		hints.ai_flags = AI_PASSIVE;
		gaierr = getaddrinfo(options.bind_address, NULL, &hints, &res);
		if (gaierr) {
			error("getaddrinfo: %s: %s", options.bind_address,
			    ssh_gai_strerror(gaierr));
			close(sock);
			return -1;
		}
	}
	/*
	 * If we are running as root and want to connect to a privileged
	 * port, bind our own socket to a privileged port.
	 */
	if (privileged) {
		PRIV_START;
		r = bindresvport_sa(sock, res ? res->ai_addr : NULL);
		PRIV_END;
		if (r < 0) {
			error("bindresvport_sa: af=%d %s", ai->ai_family,
			    strerror(errno));
			goto fail;
		}
	} else {
		if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
			error("bind: %s: %s", options.bind_address,
			    strerror(errno));
 fail:
			close(sock);
			freeaddrinfo(res);
			return -1;
		}
	}
	if (res != NULL)
		freeaddrinfo(res);
	return sock;
}

static int
timeout_connect(int sockfd, const struct sockaddr *serv_addr,
    socklen_t addrlen, int *timeoutp)
{
	fd_set *fdset;
	struct timeval tv, t_start;
	socklen_t optlen;
	int optval, rc, result = -1;

	gettimeofday(&t_start, NULL);

	if (*timeoutp <= 0) {
		result = connect(sockfd, serv_addr, addrlen);
		goto done;
	}

	set_nonblock(sockfd);
	rc = connect(sockfd, serv_addr, addrlen);
	if (rc == 0) {
		unset_nonblock(sockfd);
		result = 0;
		goto done;
	}
	if (errno != EINPROGRESS) {
		result = -1;
		goto done;
	}

	fdset = (fd_set *)xcalloc(howmany(sockfd + 1, NFDBITS),
	    sizeof(fd_mask));
	FD_SET(sockfd, fdset);
	ms_to_timeval(&tv, *timeoutp);

	for (;;) {
		rc = select(sockfd + 1, NULL, fdset, NULL, &tv);
		if (rc != -1 || errno != EINTR)
			break;
	}

	switch (rc) {
	case 0:
		/* Timed out */
		errno = ETIMEDOUT;
		break;
	case -1:
		/* Select error */
		debug("select: %s", strerror(errno));
		break;
	case 1:
		/* Completed or failed */
		optval = 0;
		optlen = sizeof(optval);
		if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval,
		    &optlen) == -1) {
			debug("getsockopt: %s", strerror(errno));
			break;
		}
		if (optval != 0) {
			errno = optval;
			break;
		}
		result = 0;
		unset_nonblock(sockfd);
		break;
	default:
		/* Should not occur */
		fatal("Bogus return (%d) from select()", rc);
	}

	free(fdset);

 done:
 	if (result == 0 && *timeoutp > 0) {
		ms_subtract_diff(&t_start, timeoutp);
		if (*timeoutp <= 0) {
			errno = ETIMEDOUT;
			result = -1;
		}
	}

	return (result);
}

/*
 * Opens a TCP/IP connection to the remote server on the given host.
 * The address of the remote host will be returned in hostaddr.
 * If port is 0, the default port will be used.  If needpriv is true,
 * a privileged port will be allocated to make the connection.
 * This requires super-user privileges if needpriv is true.
 * Connection_attempts specifies the maximum number of tries (one per
 * second).  If proxy_command is non-NULL, it specifies the command (with %h
 * and %p substituted for host and port, respectively) to use to contact
 * the daemon.
 */
static struct ssh *
ssh_connect_direct(const char *host, struct addrinfo *aitop,
    struct sockaddr_storage *hostaddr, u_short port, int family,
    int connection_attempts, int *timeout_ms, int want_keepalive, int needpriv)
{
	struct ssh *ssh;
	int on = 1;
	int sock = -1, attempt;
	char ntop[NI_MAXHOST], strport[NI_MAXSERV];
	struct addrinfo *ai;

	debug2("ssh_connect: needpriv %d", needpriv);

	for (attempt = 0; attempt < connection_attempts; attempt++) {
		if (attempt > 0) {
			/* Sleep a moment before retrying. */
			sleep(1);
			debug("Trying again...");
		}
		/*
		 * Loop through addresses for this host, and try each one in
		 * sequence until the connection succeeds.
		 */
		for (ai = aitop; ai; ai = ai->ai_next) {
			if (ai->ai_family != AF_INET &&
			    ai->ai_family != AF_INET6)
				continue;
			if (getnameinfo(ai->ai_addr, ai->ai_addrlen,
			    ntop, sizeof(ntop), strport, sizeof(strport),
			    NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
				error("ssh_connect: getnameinfo failed");
				continue;
			}
			debug("Connecting to %.200s [%.100s] port %s.",
				host, ntop, strport);

			/* Create a socket for connecting. */
			sock = ssh_create_socket(needpriv, ai);
			if (sock < 0)
				/* Any error is already output */
				continue;

			if (timeout_connect(sock, ai->ai_addr, ai->ai_addrlen,
			    timeout_ms) >= 0) {
				/* Successful connection. */
				memcpy(hostaddr, ai->ai_addr, ai->ai_addrlen);
				break;
			} else {
				debug("connect to address %s port %s: %s",
				    ntop, strport, strerror(errno));
				close(sock);
				sock = -1;
			}
		}
		if (sock != -1)
			break;	/* Successful connection. */
	}

	/* Return failure if we didn't get a successful connection. */
	if (sock == -1) {
		error("ssh: connect to host %s port %s: %s",
		    host, strport, strerror(errno));
		return (NULL);
	}

	debug("Connection established.");

	/* Set SO_KEEPALIVE if requested. */
	if (want_keepalive &&
	    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on,
	    sizeof(on)) < 0)
		error("setsockopt SO_KEEPALIVE: %.100s", strerror(errno));

	/* Set the connection. */
	ssh = ssh_packet_set_connection(NULL, sock, sock);

	return (ssh);
}

struct ssh *
ssh_connect(const char *host, struct addrinfo *addrs,
    struct sockaddr_storage *hostaddr, u_short port, int family,
    int connection_attempts, int *timeout_ms, int want_keepalive, int needpriv)
{
	if (options.proxy_command == NULL) {
		return ssh_connect_direct(host, addrs, hostaddr, port, family,
		    connection_attempts, timeout_ms, want_keepalive, needpriv);
	} else if (strcmp(options.proxy_command, "-") == 0) {
		return ssh_packet_set_connection(NULL, STDIN_FILENO,
		    STDOUT_FILENO); /* Always succeeds */
	} else if (options.proxy_use_fdpass) {
		return ssh_proxy_fdpass_connect(host, port,
		    options.proxy_command);
	}
	return ssh_proxy_connect(host, port, options.proxy_command);
}

static void
send_client_banner(int connection_out, int minor1)
{
	/* Send our own protocol version identification. */
	if (compat20) {
		xasprintf(&client_version_string, "SSH-%d.%d-%.100s\r\n",
		    PROTOCOL_MAJOR_2, PROTOCOL_MINOR_2, SSH_VERSION);
	} else {
		xasprintf(&client_version_string, "SSH-%d.%d-%.100s\n",
		    PROTOCOL_MAJOR_1, minor1, SSH_VERSION);
	}
	if (roaming_atomicio(vwrite, connection_out, client_version_string,
	    strlen(client_version_string)) != strlen(client_version_string))
		fatal("write: %.100s", strerror(errno));
	chop(client_version_string);
	debug("Local version string %.100s", client_version_string);
}

/*
 * Waits for the server identification string, and sends our own
 * identification string.
 */
void
ssh_exchange_identification(struct ssh *ssh, int timeout_ms)
{
	char buf[256], remote_version[256];	/* must be same size! */
	int remote_major, remote_minor, mismatch;
	int connection_in = ssh_packet_get_connection_in(ssh);
	int connection_out = ssh_packet_get_connection_out(ssh);
	int minor1 = PROTOCOL_MINOR_1, client_banner_sent = 0;
	u_int i, n;
	size_t len;
	int fdsetsz, remaining, rc;
	struct timeval t_start, t_remaining;
	fd_set *fdset;

	fdsetsz = howmany(connection_in + 1, NFDBITS) * sizeof(fd_mask);
	fdset = xcalloc(1, fdsetsz);

	/*
	 * If we are SSH2-only then we can send the banner immediately and
	 * save a round-trip.
	 */
	if (options.protocol == SSH_PROTO_2) {
		enable_compat20();
		send_client_banner(connection_out, 0);
		client_banner_sent = 1;
	}

	/* Read other side's version identification. */
	remaining = timeout_ms;
	for (n = 0;;) {
		for (i = 0; i < sizeof(buf) - 1; i++) {
			if (timeout_ms > 0) {
				gettimeofday(&t_start, NULL);
				ms_to_timeval(&t_remaining, remaining);
				FD_SET(connection_in, fdset);
				rc = select(connection_in + 1, fdset, NULL,
				    fdset, &t_remaining);
				ms_subtract_diff(&t_start, &remaining);
				if (rc == 0 || remaining <= 0)
					fatal("Connection timed out during "
					    "banner exchange");
				if (rc == -1) {
					if (errno == EINTR)
						continue;
					fatal("ssh_exchange_identification: "
					    "select: %s", strerror(errno));
				}
			}

			len = roaming_atomicio(read, connection_in, &buf[i], 1);

			if (len != 1 && errno == EPIPE)
				fatal("ssh_exchange_identification: "
				    "Connection closed by remote host");
			else if (len != 1)
				fatal("ssh_exchange_identification: "
				    "read: %.100s", strerror(errno));
			if (buf[i] == '\r') {
				buf[i] = '\n';
				buf[i + 1] = 0;
				continue;		/**XXX wait for \n */
			}
			if (buf[i] == '\n') {
				buf[i + 1] = 0;
				break;
			}
			if (++n > 65536)
				fatal("ssh_exchange_identification: "
				    "No banner received");
		}
		buf[sizeof(buf) - 1] = 0;
		if (strncmp(buf, "SSH-", 4) == 0)
			break;
		debug("ssh_exchange_identification: %s", buf);
	}
	server_version_string = xstrdup(buf);
	free(fdset);

	/*
	 * Check that the versions match.  In future this might accept
	 * several versions and set appropriate flags to handle them.
	 */
	if (sscanf(server_version_string, "SSH-%d.%d-%[^\n]\n",
	    &remote_major, &remote_minor, remote_version) != 3)
		fatal("Bad remote protocol version identification: '%.100s'", buf);
	debug("Remote protocol version %d.%d, remote software version %.100s",
	    remote_major, remote_minor, remote_version);

	ssh->compat = compat_datafellows(remote_version);
	mismatch = 0;

	switch (remote_major) {
	case 1:
		if (remote_minor == 99 &&
		    (options.protocol & SSH_PROTO_2) &&
		    !(options.protocol & SSH_PROTO_1_PREFERRED)) {
			enable_compat20();
			break;
		}
		if (!(options.protocol & SSH_PROTO_1)) {
			mismatch = 1;
			break;
		}
		if (remote_minor < 3) {
			fatal("Remote machine has too old SSH software version.");
		} else if (remote_minor == 3 || remote_minor == 4) {
			/* We speak 1.3, too. */
			enable_compat13();
			minor1 = 3;
			if (options.forward_agent) {
				logit("Agent forwarding disabled for protocol 1.3");
				options.forward_agent = 0;
			}
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
	if (mismatch)
		fatal("Protocol major versions differ: %d vs. %d",
		    (options.protocol & SSH_PROTO_2) ? PROTOCOL_MAJOR_2 : PROTOCOL_MAJOR_1,
		    remote_major);
	if ((ssh->compat & SSH_BUG_DERIVEKEY) != 0)
		fatal("Server version \"%.100s\" uses unsafe key agreement; "
		    "refusing connection", remote_version);
	if ((ssh->compat & SSH_BUG_RSASIGMD5) != 0)
		logit("Server version \"%.100s\" uses unsafe RSA signature "
		    "scheme; disabling use of RSA keys", remote_version);
	if (!client_banner_sent)
		send_client_banner(connection_out, minor1);
	chop(server_version_string);
}

/* defaults to 'no' */
static int
confirm(const char *prompt)
{
	const char *msg, *again = "Please type 'yes' or 'no': ";
	char *p;
	int ret = -1;

	if (options.batch_mode)
		return 0;
	for (msg = prompt;;msg = again) {
		p = read_passphrase(msg, RP_ECHO);
		if (p == NULL ||
		    (p[0] == '\0') || (p[0] == '\n') ||
		    strncasecmp(p, "no", 2) == 0)
			ret = 0;
		if (p && strncasecmp(p, "yes", 3) == 0)
			ret = 1;
		free(p);
		if (ret != -1)
			return ret;
	}
}

static int
check_host_cert(const char *host, const struct sshkey *host_key)
{
	const char *reason;

	if (sshkey_cert_check_authority(host_key, 1, 0, host, &reason) != 0) {
		error("%s", reason);
		return 0;
	}
	if (sshbuf_len(host_key->cert->critical) != 0) {
		error("Certificate for %s contains unsupported "
		    "critical options(s)", host);
		return 0;
	}
	return 1;
}

static int
sockaddr_is_local(struct sockaddr *hostaddr)
{
	switch (hostaddr->sa_family) {
	case AF_INET:
		return (ntohl(((struct sockaddr_in *)hostaddr)->
		    sin_addr.s_addr) >> 24) == IN_LOOPBACKNET;
	case AF_INET6:
		return IN6_IS_ADDR_LOOPBACK(
		    &(((struct sockaddr_in6 *)hostaddr)->sin6_addr));
	default:
		return 0;
	}
}

/*
 * Prepare the hostname and ip address strings that are used to lookup
 * host keys in known_hosts files. These may have a port number appended.
 */
void
get_hostfile_hostname_ipaddr(char *hostname, struct sockaddr *hostaddr,
    u_short port, char **hostfile_hostname, char **hostfile_ipaddr)
{
	char ntop[NI_MAXHOST];

	/*
	 * We don't have the remote ip-address for connections
	 * using a proxy command
	 */
	if (hostfile_ipaddr != NULL) {
		if (options.proxy_command == NULL) {
			if (getnameinfo(hostaddr, hostaddr->sa_len,
			    ntop, sizeof(ntop), NULL, 0, NI_NUMERICHOST) != 0)
			fatal("check_host_key: getnameinfo failed");
			*hostfile_ipaddr = put_host_port(ntop, port);
		} else {
			*hostfile_ipaddr = xstrdup("<no hostip for proxy "
			    "command>");
		}
	}

	/*
	 * Allow the user to record the key under a different name or
	 * differentiate a non-standard port.  This is useful for ssh
	 * tunneling over forwarded connections or if you run multiple
	 * sshd's on different ports on the same machine.
	 */
	if (hostfile_hostname != NULL) {
		if (options.host_key_alias != NULL) {
			*hostfile_hostname = xstrdup(options.host_key_alias);
			debug("using hostkeyalias: %s", *hostfile_hostname);
		} else {
			*hostfile_hostname = put_host_port(hostname, port);
		}
	}
}

/*
 * check whether the supplied host key is valid, return -1 if the key
 * is not valid. user_hostfile[0] will not be updated if 'readonly' is true.
 */
#define RDRW	0
#define RDONLY	1
#define ROQUIET	2
static int
check_host_key(char *hostname, struct sockaddr *hostaddr, u_short port,
    struct sshkey *host_key, int readonly,
    char **user_hostfiles, u_int num_user_hostfiles,
    char **system_hostfiles, u_int num_system_hostfiles)
{
	HostStatus host_status;
	HostStatus ip_status;
	struct sshkey *raw_key = NULL;
	char *ip = NULL, *host = NULL;
	char hostline[1000], *hostp, *fp, *ra;
	char msg[1024];
	const char *type;
	const struct hostkey_entry *host_found, *ip_found;
	int len, cancelled_forwarding = 0;
	int local = sockaddr_is_local(hostaddr);
	int r, want_cert = sshkey_is_cert(host_key), host_ip_differ = 0;
	struct hostkeys *host_hostkeys, *ip_hostkeys;
	u_int i;

	/*
	 * Force accepting of the host key for loopback/localhost. The
	 * problem is that if the home directory is NFS-mounted to multiple
	 * machines, localhost will refer to a different machine in each of
	 * them, and the user will get bogus HOST_CHANGED warnings.  This
	 * essentially disables host authentication for localhost; however,
	 * this is probably not a real problem.
	 */
	if (options.no_host_authentication_for_localhost == 1 && local &&
	    options.host_key_alias == NULL) {
		debug("Forcing accepting of host key for "
		    "loopback/localhost.");
		return 0;
	}

	/*
	 * Prepare the hostname and address strings used for hostkey lookup.
	 * In some cases, these will have a port number appended.
	 */
	get_hostfile_hostname_ipaddr(hostname, hostaddr, port, &host, &ip);

	/*
	 * Turn off check_host_ip if the connection is to localhost, via proxy
	 * command or if we don't have a hostname to compare with
	 */
	if (options.check_host_ip && (local ||
	    strcmp(hostname, ip) == 0 || options.proxy_command != NULL))
		options.check_host_ip = 0;

	host_hostkeys = init_hostkeys();
	for (i = 0; i < num_user_hostfiles; i++)
		load_hostkeys(host_hostkeys, host, user_hostfiles[i]);
	for (i = 0; i < num_system_hostfiles; i++)
		load_hostkeys(host_hostkeys, host, system_hostfiles[i]);

	ip_hostkeys = NULL;
	if (!want_cert && options.check_host_ip) {
		ip_hostkeys = init_hostkeys();
		for (i = 0; i < num_user_hostfiles; i++)
			load_hostkeys(ip_hostkeys, ip, user_hostfiles[i]);
		for (i = 0; i < num_system_hostfiles; i++)
			load_hostkeys(ip_hostkeys, ip, system_hostfiles[i]);
	}

 retry:
	/* Reload these as they may have changed on cert->key downgrade */
	want_cert = sshkey_is_cert(host_key);
	type = sshkey_type(host_key);

	/*
	 * Check if the host key is present in the user's list of known
	 * hosts or in the systemwide list.
	 */
	host_status = check_key_in_hostkeys(host_hostkeys, host_key,
	    &host_found);

	/*
	 * Also perform check for the ip address, skip the check if we are
	 * localhost, looking for a certificate, or the hostname was an ip
	 * address to begin with.
	 */
	if (!want_cert && ip_hostkeys != NULL) {
		ip_status = check_key_in_hostkeys(ip_hostkeys, host_key,
		    &ip_found);
		if (host_status == HOST_CHANGED &&
		    (ip_status != HOST_CHANGED || 
		    (ip_found != NULL &&
		    !sshkey_equal(ip_found->key, host_found->key))))
			host_ip_differ = 1;
	} else
		ip_status = host_status;

	switch (host_status) {
	case HOST_OK:
		/* The host is known and the key matches. */
		debug("Host '%.200s' is known and matches the %s host %s.",
		    host, type, want_cert ? "certificate" : "key");
		debug("Found %s in %s:%lu", want_cert ? "CA key" : "key",
		    host_found->file, host_found->line);
		if (want_cert && !check_host_cert(hostname, host_key))
			goto fail;
		if (options.check_host_ip && ip_status == HOST_NEW) {
			if (readonly || want_cert)
				logit("%s host key for IP address "
				    "'%.128s' not in list of known hosts.",
				    type, ip);
			else if (!add_host_to_hostfile(user_hostfiles[0], ip,
			    host_key, options.hash_known_hosts))
				logit("Failed to add the %s host key for IP "
				    "address '%.128s' to the list of known "
				    "hosts (%.30s).", type, ip,
				    user_hostfiles[0]);
			else
				logit("Warning: Permanently added the %s host "
				    "key for IP address '%.128s' to the list "
				    "of known hosts.", type, ip);
		} else if (options.visual_host_key) {
			fp = sshkey_fingerprint(host_key, SSH_FP_MD5,
			    SSH_FP_HEX);
			ra = sshkey_fingerprint(host_key, SSH_FP_MD5,
			    SSH_FP_RANDOMART);
			logit("Host key fingerprint is %s\n%s\n", fp, ra);
			free(ra);
			free(fp);
		}
		break;
	case HOST_NEW:
		if (options.host_key_alias == NULL && port != 0 &&
		    port != SSH_DEFAULT_PORT) {
			debug("checking without port identifier");
			if (check_host_key(hostname, hostaddr, 0, host_key,
			    ROQUIET, user_hostfiles, num_user_hostfiles,
			    system_hostfiles, num_system_hostfiles) == 0) {
				debug("found matching key w/out port");
				break;
			}
		}
		if (readonly || want_cert)
			goto fail;
		/* The host is new. */
		if (options.strict_host_key_checking == 1) {
			/*
			 * User has requested strict host key checking.  We
			 * will not add the host key automatically.  The only
			 * alternative left is to abort.
			 */
			error("No %s host key is known for %.200s and you "
			    "have requested strict checking.", type, host);
			goto fail;
		} else if (options.strict_host_key_checking == 2) {
			char msg1[1024], msg2[1024];

			if (show_other_keys(host_hostkeys, host_key))
				snprintf(msg1, sizeof(msg1),
				    "\nbut keys of different type are already"
				    " known for this host.");
			else
				snprintf(msg1, sizeof(msg1), ".");
			/* The default */
			fp = sshkey_fingerprint(host_key, SSH_FP_MD5,
			    SSH_FP_HEX);
			ra = sshkey_fingerprint(host_key, SSH_FP_MD5,
			    SSH_FP_RANDOMART);
			msg2[0] = '\0';
			if (options.verify_host_key_dns) {
				if (matching_host_key_dns)
					snprintf(msg2, sizeof(msg2),
					    "Matching host key fingerprint"
					    " found in DNS.\n");
				else
					snprintf(msg2, sizeof(msg2),
					    "No matching host key fingerprint"
					    " found in DNS.\n");
			}
			snprintf(msg, sizeof(msg),
			    "The authenticity of host '%.200s (%s)' can't be "
			    "established%s\n"
			    "%s key fingerprint is %s.%s%s\n%s"
			    "Are you sure you want to continue connecting "
			    "(yes/no)? ",
			    host, ip, msg1, type, fp,
			    options.visual_host_key ? "\n" : "",
			    options.visual_host_key ? ra : "",
			    msg2);
			free(ra);
			free(fp);
			if (!confirm(msg))
				goto fail;
		}
		/*
		 * If not in strict mode, add the key automatically to the
		 * local known_hosts file.
		 */
		if (options.check_host_ip && ip_status == HOST_NEW) {
			snprintf(hostline, sizeof(hostline), "%s,%s", host, ip);
			hostp = hostline;
			if (options.hash_known_hosts) {
				/* Add hash of host and IP separately */
				r = add_host_to_hostfile(user_hostfiles[0],
				    host, host_key, options.hash_known_hosts) &&
				    add_host_to_hostfile(user_hostfiles[0], ip,
				    host_key, options.hash_known_hosts);
			} else {
				/* Add unhashed "host,ip" */
				r = add_host_to_hostfile(user_hostfiles[0],
				    hostline, host_key,
				    options.hash_known_hosts);
			}
		} else {
			r = add_host_to_hostfile(user_hostfiles[0], host,
			    host_key, options.hash_known_hosts);
			hostp = host;
		}

		if (!r)
			logit("Failed to add the host to the list of known "
			    "hosts (%.500s).", user_hostfiles[0]);
		else
			logit("Warning: Permanently added '%.200s' (%s) to the "
			    "list of known hosts.", hostp, type);
		break;
	case HOST_REVOKED:
		error("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
		error("@       WARNING: REVOKED HOST KEY DETECTED!               @");
		error("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
		error("The %s host key for %s is marked as revoked.", type, host);
		error("This could mean that a stolen key is being used to");
		error("impersonate this host.");

		/*
		 * If strict host key checking is in use, the user will have
		 * to edit the key manually and we can only abort.
		 */
		if (options.strict_host_key_checking) {
			error("%s host key for %.200s was revoked and you have "
			    "requested strict checking.", type, host);
			goto fail;
		}
		goto continue_unsafe;

	case HOST_CHANGED:
		if (want_cert) {
			/*
			 * This is only a debug() since it is valid to have
			 * CAs with wildcard DNS matches that don't match
			 * all hosts that one might visit.
			 */
			debug("Host certificate authority does not "
			    "match %s in %s:%lu", CA_MARKER,
			    host_found->file, host_found->line);
			goto fail;
		}
		if (readonly == ROQUIET)
			goto fail;
		if (options.check_host_ip && host_ip_differ) {
			char *key_msg;
			if (ip_status == HOST_NEW)
				key_msg = "is unknown";
			else if (ip_status == HOST_OK)
				key_msg = "is unchanged";
			else
				key_msg = "has a different value";
			error("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
			error("@       WARNING: POSSIBLE DNS SPOOFING DETECTED!          @");
			error("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
			error("The %s host key for %s has changed,", type, host);
			error("and the key for the corresponding IP address %s", ip);
			error("%s. This could either mean that", key_msg);
			error("DNS SPOOFING is happening or the IP address for the host");
			error("and its host key have changed at the same time.");
			if (ip_status != HOST_NEW)
				error("Offending key for IP in %s:%lu",
				    ip_found->file, ip_found->line);
		}
		/* The host key has changed. */
		warn_changed_key(host_key);
		error("Add correct host key in %.100s to get rid of this message.",
		    user_hostfiles[0]);
		error("Offending %s key in %s:%lu",
		    sshkey_type(host_found->key),
		    host_found->file, host_found->line);

		/*
		 * If strict host key checking is in use, the user will have
		 * to edit the key manually and we can only abort.
		 */
		if (options.strict_host_key_checking) {
			error("%s host key for %.200s has changed and you have "
			    "requested strict checking.", type, host);
			goto fail;
		}

 continue_unsafe:
		/*
		 * If strict host key checking has not been requested, allow
		 * the connection but without MITM-able authentication or
		 * forwarding.
		 */
		if (options.password_authentication) {
			error("Password authentication is disabled to avoid "
			    "man-in-the-middle attacks.");
			options.password_authentication = 0;
			cancelled_forwarding = 1;
		}
		if (options.kbd_interactive_authentication) {
			error("Keyboard-interactive authentication is disabled"
			    " to avoid man-in-the-middle attacks.");
			options.kbd_interactive_authentication = 0;
			options.challenge_response_authentication = 0;
			cancelled_forwarding = 1;
		}
		if (options.challenge_response_authentication) {
			error("Challenge/response authentication is disabled"
			    " to avoid man-in-the-middle attacks.");
			options.challenge_response_authentication = 0;
			cancelled_forwarding = 1;
		}
		if (options.forward_agent) {
			error("Agent forwarding is disabled to avoid "
			    "man-in-the-middle attacks.");
			options.forward_agent = 0;
			cancelled_forwarding = 1;
		}
		if (options.forward_x11) {
			error("X11 forwarding is disabled to avoid "
			    "man-in-the-middle attacks.");
			options.forward_x11 = 0;
			cancelled_forwarding = 1;
		}
		if (options.num_local_forwards > 0 ||
		    options.num_remote_forwards > 0) {
			error("Port forwarding is disabled to avoid "
			    "man-in-the-middle attacks.");
			options.num_local_forwards =
			    options.num_remote_forwards = 0;
			cancelled_forwarding = 1;
		}
		if (options.tun_open != SSH_TUNMODE_NO) {
			error("Tunnel forwarding is disabled to avoid "
			    "man-in-the-middle attacks.");
			options.tun_open = SSH_TUNMODE_NO;
			cancelled_forwarding = 1;
		}
		if (options.exit_on_forward_failure && cancelled_forwarding)
			fatal("Error: forwarding disabled due to host key "
			    "check failure");
		
		/*
		 * XXX Should permit the user to change to use the new id.
		 * This could be done by converting the host key to an
		 * identifying sentence, tell that the host identifies itself
		 * by that sentence, and ask the user if he/she wishes to
		 * accept the authentication.
		 */
		break;
	case HOST_FOUND:
		fatal("internal error");
		break;
	}

	if (options.check_host_ip && host_status != HOST_CHANGED &&
	    ip_status == HOST_CHANGED) {
		snprintf(msg, sizeof(msg),
		    "Warning: the %s host key for '%.200s' "
		    "differs from the key for the IP address '%.128s'"
		    "\nOffending key for IP in %s:%lu",
		    type, host, ip, ip_found->file, ip_found->line);
		if (host_status == HOST_OK) {
			len = strlen(msg);
			snprintf(msg + len, sizeof(msg) - len,
			    "\nMatching host key in %s:%lu",
			    host_found->file, host_found->line);
		}
		if (options.strict_host_key_checking == 1) {
			logit("%s", msg);
			error("Exiting, you have requested strict checking.");
			goto fail;
		} else if (options.strict_host_key_checking == 2) {
			strlcat(msg, "\nAre you sure you want "
			    "to continue connecting (yes/no)? ", sizeof(msg));
			if (!confirm(msg))
				goto fail;
		} else {
			logit("%s", msg);
		}
	}

	free(ip);
	free(host);
	if (host_hostkeys != NULL)
		free_hostkeys(host_hostkeys);
	if (ip_hostkeys != NULL)
		free_hostkeys(ip_hostkeys);
	return 0;

fail:
	if (want_cert && host_status != HOST_REVOKED) {
		/*
		 * No matching certificate. Downgrade cert to raw key and
		 * search normally.
		 */
		debug("No matching CA found. Retry with plain key");
		if ((r = sshkey_from_private(host_key, &raw_key)) != 0)
			fatal("%s: sshkey_from_private: %s",
			    __func__, ssh_err(r));
		if ((r = sshkey_drop_cert(raw_key)) != 0)
			fatal("Couldn't drop certificate: %s", ssh_err(r));
		host_key = raw_key;
		goto retry;
	}
	if (raw_key != NULL)
		sshkey_free(raw_key);
	free(ip);
	free(host);
	if (host_hostkeys != NULL)
		free_hostkeys(host_hostkeys);
	if (ip_hostkeys != NULL)
		free_hostkeys(ip_hostkeys);
	return -1;
}

/* returns 0 if key verifies or -1 if key does NOT verify */
int
verify_host_key(char *host, struct sockaddr *hostaddr, struct sshkey *host_key)
{
	int flags = 0;
	char *fp;
	Key *plain = NULL;

	fp = sshkey_fingerprint(host_key, SSH_FP_MD5, SSH_FP_HEX);
	debug("Server host key: %s %s", sshkey_type(host_key), fp);
	free(fp);

<<<<<<< sshconnect.c
	/* XXX certs are not yet supported for DNS */
	if (!sshkey_is_cert(host_key) && options.verify_host_key_dns &&
	    verify_host_key_dns(host, hostaddr, host_key, &flags) == 0) {
		if (flags & DNS_VERIFY_FOUND) {

			if (options.verify_host_key_dns == 1 &&
			    flags & DNS_VERIFY_MATCH &&
			    flags & DNS_VERIFY_SECURE)
				return 0;

			if (flags & DNS_VERIFY_MATCH) {
				matching_host_key_dns = 1;
			} else {
				warn_changed_key(host_key);
				error("Update the SSHFP RR in DNS with the new "
				    "host key to get rid of this message.");
=======
	if (options.verify_host_key_dns) {
		/*
		 * XXX certs are not yet supported for DNS, so downgrade
		 * them and try the plain key.
		 */
		plain = key_from_private(host_key);
		if (key_is_cert(plain))
			key_drop_cert(plain);
		if (verify_host_key_dns(host, hostaddr, plain, &flags) == 0) {
			if (flags & DNS_VERIFY_FOUND) {
				if (options.verify_host_key_dns == 1 &&
				    flags & DNS_VERIFY_MATCH &&
				    flags & DNS_VERIFY_SECURE) {
					key_free(plain);
					return 0;
				}
				if (flags & DNS_VERIFY_MATCH) {
					matching_host_key_dns = 1;
				} else {
					warn_changed_key(plain);
					error("Update the SSHFP RR in DNS "
					    "with the new host key to get rid "
					    "of this message.");
				}
>>>>>>> 1.247
			}
		}
		key_free(plain);
	}

	return check_host_key(host, hostaddr, options.port, host_key, RDRW,
	    options.user_hostfiles, options.num_user_hostfiles,
	    options.system_hostfiles, options.num_system_hostfiles);
}

/*
 * Starts a dialog with the server, and authenticates the current user on the
 * server.  This does not need any extra privileges.  The basic connection
 * to the server must already have been established before this is called.
 * If login fails, this function prints an error and never returns.
 * This function does not require super-user privileges.
 */
void
ssh_login(struct ssh *ssh, Sensitive *sensitive, const char *orighost,
    struct sockaddr *hostaddr, u_short port, struct passwd *pw, int timeout_ms)
{
	char *host;
	char *server_user, *local_user;

	local_user = xstrdup(pw->pw_name);
	server_user = options.user ? options.user : local_user;

	/* Convert the user-supplied hostname into all lowercase. */
	host = xstrdup(orighost);
	lowercase(host);

	/* Exchange protocol version identification strings with the server. */
	ssh_exchange_identification(ssh, timeout_ms);

	/* Put the connection into non-blocking mode. */
	ssh_packet_set_nonblocking(ssh);

	/* key exchange */
	/* authenticate user */
	ssh->host = host;
	ssh->hostaddr = hostaddr;

	if (compat20) {
		ssh_kex2(ssh, port);
		ssh_userauth2(ssh, local_user, server_user, sensitive);
	} else {
		ssh_kex(ssh, host, hostaddr);
		ssh_userauth1(ssh, local_user, server_user, host, sensitive);
	}
	free(local_user);
}

int
ssh_put_password(struct ssh *ssh, char *password)
{
	int r, size;
	char *padded;

	if (ssh->compat & SSH_BUG_PASSWORDPAD)
		return sshpkt_put_cstring(ssh, password);
	size = roundup(strlen(password) + 1, 32);
	if ((padded = calloc(1, size)) == NULL)
		return SSH_ERR_ALLOC_FAIL;
	strlcpy(padded, password, size);
	r = sshpkt_put_string(ssh, padded, size);
	explicit_bzero(padded, size);
	free(padded);
	return r;
}

/* print all known host keys for a given host, but skip keys of given type */
static int
show_other_keys(struct hostkeys *hostkeys, struct sshkey *key)
{
	int type[] = {
		KEY_RSA1,
		KEY_RSA,
		KEY_DSA,
		KEY_ECDSA,
		KEY_ED25519,
		-1
	};
	int i, ret = 0;
	char *fp, *ra;
	const struct hostkey_entry *found;

	for (i = 0; type[i] != -1; i++) {
		if (type[i] == key->type)
			continue;
		if (!lookup_key_in_hostkeys_by_type(hostkeys, type[i], &found))
			continue;
		fp = sshkey_fingerprint(found->key, SSH_FP_MD5, SSH_FP_HEX);
		ra = sshkey_fingerprint(found->key, SSH_FP_MD5,
		    SSH_FP_RANDOMART);
		logit("WARNING: %s key found for host %s\n"
		    "in %s:%lu\n"
		    "%s key fingerprint %s.",
		    sshkey_type(found->key),
		    found->host, found->file, found->line,
		    sshkey_type(found->key), fp);
		if (options.visual_host_key)
			logit("%s", ra);
		free(ra);
		free(fp);
		ret = 1;
	}
	return ret;
}

static void
warn_changed_key(struct sshkey *host_key)
{
	char *fp;

	fp = sshkey_fingerprint(host_key, SSH_FP_MD5, SSH_FP_HEX);

	error("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	error("@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @");
	error("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	error("IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!");
	error("Someone could be eavesdropping on you right now (man-in-the-middle attack)!");
	error("It is also possible that a host key has just been changed.");
	error("The fingerprint for the %s key sent by the remote host is\n%s.",
	    sshkey_type(host_key), fp);
	error("Please contact your system administrator.");

	free(fp);
}

/*
 * Execute a local command
 */
int
ssh_local_cmd(const char *args)
{
	char *shell;
	pid_t pid;
	int status;
	void (*osighand)(int);

	if (!options.permit_local_command ||
	    args == NULL || !*args)
		return (1);

	if ((shell = getenv("SHELL")) == NULL || *shell == '\0')
		shell = _PATH_BSHELL;

	osighand = signal(SIGCHLD, SIG_DFL);
	pid = fork();
	if (pid == 0) {
		signal(SIGPIPE, SIG_DFL);
		debug3("Executing %s -c \"%s\"", shell, args);
		execl(shell, shell, "-c", args, (char *)NULL);
		error("Couldn't execute %s -c \"%s\": %s",
		    shell, args, strerror(errno));
		_exit(1);
	} else if (pid == -1)
		fatal("fork failed: %.100s", strerror(errno));
	while (waitpid(pid, &status, 0) == -1)
		if (errno != EINTR)
			fatal("Couldn't wait for child: %s", strerror(errno));
	signal(SIGCHLD, osighand);

	if (!WIFEXITED(status))
		return (1);

	return (WEXITSTATUS(status));
}
