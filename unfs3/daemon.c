
/*
 * UNFS3 server framework
 * Originally generated using rpcgen
 * Portions (C) 2004, Pascal Schmidt
 * see file LICENSE for license details
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <memory.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if HAVE_RPC_SVC_SOC_H == 1
# include <rpc/svc_soc.h>
#endif

#include "nfs.h"
#include "mount.h"
#include "xdr.h"
#include "fh.h"
#include "fh_cache.h"
#include "fd_cache.h"
#include "user.h"
#include "daemon.h"
#include "backend.h"
#include "Config/exports.h"

#ifndef SIG_PF
#define SIG_PF void(*)(int)
#endif

#define UNFS_NAME "UNFS3 unfsd 0.9.17 (C) 2006, Pascal Schmidt <unfs3-server@ewetel.net>\n"

/* write verifier */
writeverf3 wverf;

/* options and default values */
int opt_expire_writers = FALSE;
int opt_detach = TRUE;
char *opt_exports = "/etc/exports";
int opt_cluster = FALSE;
char *opt_cluster_path = "/";
int opt_tcponly = FALSE;
unsigned int opt_nfs_port = NFS_PORT;	/* 0 means RPC_ANYSOCK */
unsigned int opt_mount_port = NFS_PORT;
int opt_singleuser = FALSE;
int opt_brute_force = FALSE;
in_addr_t opt_bind_addr = INADDR_ANY;
int opt_readable_executables = FALSE;

/* Register with portmapper? */
int opt_portmapper = TRUE;

/*
 * output message to syslog or stdout
 */
void logmsg(int prio, const char *fmt, ...)
{
    va_list ap;

#if HAVE_VSYSLOG == 0
    char mesg[1024];
#endif

    va_start(ap, fmt);
    if (opt_detach) {
#if HAVE_VSYSLOG == 1
	vsyslog(prio, fmt, ap);
#else
	vsnprintf(mesg, 1024, fmt, ap);
	syslog(prio, mesg, 1024);
#endif
    } else {
	vprintf(fmt, ap);
	putchar('\n');
    }
    va_end(ap);
}

/*
 * return remote address from svc_req structure
 */
struct in_addr get_remote(struct svc_req *rqstp)
{
    return (svc_getcaller(rqstp->rq_xprt))->sin_addr;
}

/*
 * return remote port from svc_req structure
 */
short get_port(struct svc_req *rqstp)
{
    return (svc_getcaller(rqstp->rq_xprt))->sin_port;
}

/*
 * return the socket type of the request (SOCK_STREAM or SOCK_DGRAM)
 */
int get_socket_type(struct svc_req *rqstp)
{
    int v;
    socklen_t l;

    l = sizeof(v);

    if (getsockopt(rqstp->rq_xprt->xp_sock, SOL_SOCKET, SO_TYPE, &v, &l) < 0) {
	logmsg(LOG_CRIT, "unable to determine socket type");
	return -1;
    }

    return v;
}

/*
 * parse command line options
 */
static void parse_options(int argc, char **argv)
{
    int opt = 0;
    char *optstring = "bcC:de:hl:m:n:prstuw";

    while (opt != -1) {
	opt = getopt(argc, argv, optstring);
	switch (opt) {
	    case 'b':
		opt_brute_force = TRUE;
		break;
#ifdef WANT_CLUSTER
	    case 'c':
		opt_cluster = TRUE;
		break;
	    case 'C':
		opt_cluster_path = optarg;
		break;
#endif
	    case 'd':
		printf(UNFS_NAME);
		opt_detach = FALSE;
		break;
	    case 'e':
		if (optarg[0] != '/') {
		    fprintf(stderr, "Error: relative path to exports file\n");
		    exit(1);
		}
		opt_exports = optarg;
		break;
	    case 'h':
		printf(UNFS_NAME);
		printf("Usage: %s [options]\n", argv[0]);
		printf("\t-h          display this short option summary\n");
		printf("\t-w          expire writers from fd cache\n");
		printf("\t-u          use unprivileged port for services\n");
		printf("\t-d          do not detach from terminal\n");
		printf("\t-e <file>   file to use instead of /etc/exports\n");
#ifdef WANT_CLUSTER
		printf("\t-c          enable cluster extensions\n");
		printf("\t-C <path>   set path for cluster extensions\n");
#endif
		printf("\t-n <port>   port to use for NFS service\n");
		printf("\t-m <port>   port to use for MOUNT service\n");
		printf
		    ("\t-t          TCP only, do not listen on UDP ports\n");
		printf("\t-p          do not register with the portmapper\n");
		printf("\t-s          single user mode\n");
		printf("\t-b          enable brute force file searching\n");
		printf
		    ("\t-l <addr>   bind to interface with specified address\n");
		printf
		    ("\t-r          report unreadable executables as readable\n");
		exit(0);
		break;
	    case 'l':
		opt_bind_addr = inet_addr(optarg);
		if (opt_bind_addr == (unsigned) -1) {
		    fprintf(stderr, "Invalid bind address\n");
		    exit(1);
		}
		break;
	    case 'm':
		opt_mount_port = strtol(optarg, NULL, 10);
		if (opt_mount_port == 0) {
		    fprintf(stderr, "Invalid port\n");
		    exit(1);
		}
		break;
	    case 'n':
		opt_nfs_port = strtol(optarg, NULL, 10);
		if (opt_nfs_port == 0) {
		    fprintf(stderr, "Invalid port\n");
		    exit(1);
		}
		break;
	    case 'p':
		opt_portmapper = FALSE;
		break;
	    case 'r':
		opt_readable_executables = TRUE;
		break;
	    case 's':
		opt_singleuser = TRUE;
		if (backend_getuid() == 0) {
		    logmsg(LOG_WARNING,
			   "Warning: running as root with -s is dangerous");
		    logmsg(LOG_WARNING,
			   "All clients will have root access to all exported files!");
		}
		break;
	    case 't':
		opt_tcponly = TRUE;
		break;
	    case 'u':
		opt_nfs_port = 0;
		opt_mount_port = 0;
		break;
	    case 'w':
		opt_expire_writers = TRUE;
		break;
	    case '?':
		exit(1);
		break;
	}
    }
}

/*
 * signal handler and error exit function
 */
void daemon_exit(int error)
{
    if (error == SIGHUP) {
	get_squash_ids();
	exports_parse();
	return;
    }

    if (error == SIGUSR1) {
	if (fh_cache_use > 0)
	    logmsg(LOG_INFO, "fh entries %i access %i hit %i miss %i",
		   fh_cache_max, fh_cache_use, fh_cache_hit,
		   fh_cache_use - fh_cache_hit);
	else
	    logmsg(LOG_INFO, "fh cache unused");
	logmsg(LOG_INFO, "open file descriptors: read %i, write %i",
	       fd_cache_readers, fd_cache_writers);
	return;
    }

    if (opt_portmapper) {
	svc_unregister(MOUNTPROG, MOUNTVERS1);
	svc_unregister(MOUNTPROG, MOUNTVERS3);
    }

    if (opt_portmapper) {
	svc_unregister(NFS3_PROGRAM, NFS_V3);
    }

    if (error == SIGSEGV)
	logmsg(LOG_EMERG, "segmentation fault");

    fd_cache_purge();

    if (opt_detach)
	closelog();

    backend_shutdown();

    exit(1);
}

/*
 * NFS service dispatch function
 * generated by rpcgen
 */
static void nfs3_program_3(struct svc_req *rqstp, register SVCXPRT * transp)
{
    union {
	GETATTR3args nfsproc3_getattr_3_arg;
	SETATTR3args nfsproc3_setattr_3_arg;
	LOOKUP3args nfsproc3_lookup_3_arg;
	ACCESS3args nfsproc3_access_3_arg;
	READLINK3args nfsproc3_readlink_3_arg;
	READ3args nfsproc3_read_3_arg;
	WRITE3args nfsproc3_write_3_arg;
	CREATE3args nfsproc3_create_3_arg;
	MKDIR3args nfsproc3_mkdir_3_arg;
	SYMLINK3args nfsproc3_symlink_3_arg;
	MKNOD3args nfsproc3_mknod_3_arg;
	REMOVE3args nfsproc3_remove_3_arg;
	RMDIR3args nfsproc3_rmdir_3_arg;
	RENAME3args nfsproc3_rename_3_arg;
	LINK3args nfsproc3_link_3_arg;
	READDIR3args nfsproc3_readdir_3_arg;
	READDIRPLUS3args nfsproc3_readdirplus_3_arg;
	FSSTAT3args nfsproc3_fsstat_3_arg;
	FSINFO3args nfsproc3_fsinfo_3_arg;
	PATHCONF3args nfsproc3_pathconf_3_arg;
	COMMIT3args nfsproc3_commit_3_arg;
    } argument;
    char *result;
    xdrproc_t _xdr_argument, _xdr_result;
    char *(*local) (char *, struct svc_req *);

    switch (rqstp->rq_proc) {
	case NFSPROC3_NULL:
	    _xdr_argument = (xdrproc_t) xdr_void;
	    _xdr_result = (xdrproc_t) xdr_void;
	    local = (char *(*)(char *, struct svc_req *)) nfsproc3_null_3_svc;
	    break;

	case NFSPROC3_GETATTR:
	    _xdr_argument = (xdrproc_t) xdr_GETATTR3args;
	    _xdr_result = (xdrproc_t) xdr_GETATTR3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_getattr_3_svc;
	    break;

	case NFSPROC3_SETATTR:
	    _xdr_argument = (xdrproc_t) xdr_SETATTR3args;
	    _xdr_result = (xdrproc_t) xdr_SETATTR3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_setattr_3_svc;
	    break;

	case NFSPROC3_LOOKUP:
	    _xdr_argument = (xdrproc_t) xdr_LOOKUP3args;
	    _xdr_result = (xdrproc_t) xdr_LOOKUP3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_lookup_3_svc;
	    break;

	case NFSPROC3_ACCESS:
	    _xdr_argument = (xdrproc_t) xdr_ACCESS3args;
	    _xdr_result = (xdrproc_t) xdr_ACCESS3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_access_3_svc;
	    break;

	case NFSPROC3_READLINK:
	    _xdr_argument = (xdrproc_t) xdr_READLINK3args;
	    _xdr_result = (xdrproc_t) xdr_READLINK3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_readlink_3_svc;
	    break;

	case NFSPROC3_READ:
	    _xdr_argument = (xdrproc_t) xdr_READ3args;
	    _xdr_result = (xdrproc_t) xdr_READ3res;
	    local = (char *(*)(char *, struct svc_req *)) nfsproc3_read_3_svc;
	    break;

	case NFSPROC3_WRITE:
	    _xdr_argument = (xdrproc_t) xdr_WRITE3args;
	    _xdr_result = (xdrproc_t) xdr_WRITE3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_write_3_svc;
	    break;

	case NFSPROC3_CREATE:
	    _xdr_argument = (xdrproc_t) xdr_CREATE3args;
	    _xdr_result = (xdrproc_t) xdr_CREATE3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_create_3_svc;
	    break;

	case NFSPROC3_MKDIR:
	    _xdr_argument = (xdrproc_t) xdr_MKDIR3args;
	    _xdr_result = (xdrproc_t) xdr_MKDIR3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_mkdir_3_svc;
	    break;

	case NFSPROC3_SYMLINK:
	    _xdr_argument = (xdrproc_t) xdr_SYMLINK3args;
	    _xdr_result = (xdrproc_t) xdr_SYMLINK3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_symlink_3_svc;
	    break;

	case NFSPROC3_MKNOD:
	    _xdr_argument = (xdrproc_t) xdr_MKNOD3args;
	    _xdr_result = (xdrproc_t) xdr_MKNOD3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_mknod_3_svc;
	    break;

	case NFSPROC3_REMOVE:
	    _xdr_argument = (xdrproc_t) xdr_REMOVE3args;
	    _xdr_result = (xdrproc_t) xdr_REMOVE3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_remove_3_svc;
	    break;

	case NFSPROC3_RMDIR:
	    _xdr_argument = (xdrproc_t) xdr_RMDIR3args;
	    _xdr_result = (xdrproc_t) xdr_RMDIR3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_rmdir_3_svc;
	    break;

	case NFSPROC3_RENAME:
	    _xdr_argument = (xdrproc_t) xdr_RENAME3args;
	    _xdr_result = (xdrproc_t) xdr_RENAME3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_rename_3_svc;
	    break;

	case NFSPROC3_LINK:
	    _xdr_argument = (xdrproc_t) xdr_LINK3args;
	    _xdr_result = (xdrproc_t) xdr_LINK3res;
	    local = (char *(*)(char *, struct svc_req *)) nfsproc3_link_3_svc;
	    break;

	case NFSPROC3_READDIR:
	    _xdr_argument = (xdrproc_t) xdr_READDIR3args;
	    _xdr_result = (xdrproc_t) xdr_READDIR3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_readdir_3_svc;
	    break;

	case NFSPROC3_READDIRPLUS:
	    _xdr_argument = (xdrproc_t) xdr_READDIRPLUS3args;
	    _xdr_result = (xdrproc_t) xdr_READDIRPLUS3res;
	    local = (char *(*)(char *, struct svc_req *))
		nfsproc3_readdirplus_3_svc;
	    break;

	case NFSPROC3_FSSTAT:
	    _xdr_argument = (xdrproc_t) xdr_FSSTAT3args;
	    _xdr_result = (xdrproc_t) xdr_FSSTAT3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_fsstat_3_svc;
	    break;

	case NFSPROC3_FSINFO:
	    _xdr_argument = (xdrproc_t) xdr_FSINFO3args;
	    _xdr_result = (xdrproc_t) xdr_FSINFO3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_fsinfo_3_svc;
	    break;

	case NFSPROC3_PATHCONF:
	    _xdr_argument = (xdrproc_t) xdr_PATHCONF3args;
	    _xdr_result = (xdrproc_t) xdr_PATHCONF3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_pathconf_3_svc;
	    break;

	case NFSPROC3_COMMIT:
	    _xdr_argument = (xdrproc_t) xdr_COMMIT3args;
	    _xdr_result = (xdrproc_t) xdr_COMMIT3res;
	    local =
		(char *(*)(char *, struct svc_req *)) nfsproc3_commit_3_svc;
	    break;

	default:
	    svcerr_noproc(transp);
	    return;
    }
    memset((char *) &argument, 0, sizeof(argument));
    if (!svc_getargs(transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
	svcerr_decode(transp);
	return;
    }
    result = (*local) ((char *) &argument, rqstp);
    if (result != NULL &&
	!svc_sendreply(transp, (xdrproc_t) _xdr_result, result)) {
	svcerr_systemerr(transp);
	logmsg(LOG_CRIT, "unable to send RPC reply");
    }
    if (!svc_freeargs
	(transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
	logmsg(LOG_CRIT, "unable to free XDR arguments");
    }
    return;
}

/*
 * mount protocol dispatcher
 * generated by rpcgen
 */
static void mountprog_3(struct svc_req *rqstp, register SVCXPRT * transp)
{
    union {
	dirpath mountproc_mnt_3_arg;
	dirpath mountproc_umnt_3_arg;
    } argument;
    char *result;
    xdrproc_t _xdr_argument, _xdr_result;
    char *(*local) (char *, struct svc_req *);

    switch (rqstp->rq_proc) {
	case MOUNTPROC_NULL:
	    _xdr_argument = (xdrproc_t) xdr_void;
	    _xdr_result = (xdrproc_t) xdr_void;
	    local =
		(char *(*)(char *, struct svc_req *)) mountproc_null_3_svc;
	    break;

	case MOUNTPROC_MNT:
	    _xdr_argument = (xdrproc_t) xdr_dirpath;
	    _xdr_result = (xdrproc_t) xdr_mountres3;
	    local = (char *(*)(char *, struct svc_req *)) mountproc_mnt_3_svc;
	    break;

	case MOUNTPROC_DUMP:
	    _xdr_argument = (xdrproc_t) xdr_void;
	    _xdr_result = (xdrproc_t) xdr_mountlist;
	    local =
		(char *(*)(char *, struct svc_req *)) mountproc_dump_3_svc;
	    break;

	case MOUNTPROC_UMNT:
	    _xdr_argument = (xdrproc_t) xdr_dirpath;
	    _xdr_result = (xdrproc_t) xdr_void;
	    local =
		(char *(*)(char *, struct svc_req *)) mountproc_umnt_3_svc;
	    break;

	case MOUNTPROC_UMNTALL:
	    _xdr_argument = (xdrproc_t) xdr_void;
	    _xdr_result = (xdrproc_t) xdr_void;
	    local =
		(char *(*)(char *, struct svc_req *)) mountproc_umntall_3_svc;
	    break;

	case MOUNTPROC_EXPORT:
	    _xdr_argument = (xdrproc_t) xdr_void;
	    _xdr_result = (xdrproc_t) xdr_exports;
	    local =
		(char *(*)(char *, struct svc_req *)) mountproc_export_3_svc;
	    break;

	default:
	    svcerr_noproc(transp);
	    return;
    }
    memset((char *) &argument, 0, sizeof(argument));
    if (!svc_getargs(transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
	svcerr_decode(transp);
	return;
    }
    result = (*local) ((char *) &argument, rqstp);
    if (result != NULL &&
	!svc_sendreply(transp, (xdrproc_t) _xdr_result, result)) {
	svcerr_systemerr(transp);
	logmsg(LOG_CRIT, "unable to send RPC reply");
    }
    if (!svc_freeargs
	(transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
	logmsg(LOG_CRIT, "unable to free XDR arguments");
    }
    return;
}

static void register_nfs_service(SVCXPRT * udptransp, SVCXPRT * tcptransp)
{
    if (opt_portmapper) {
	pmap_unset(NFS3_PROGRAM, NFS_V3);
    }

    if (udptransp != NULL) {
	/* Register NFS service for UDP */
	if (!svc_register
	    (udptransp, NFS3_PROGRAM, NFS_V3, nfs3_program_3,
	     opt_portmapper ? IPPROTO_UDP : 0)) {
	    fprintf(stderr, "%s\n",
		    "unable to register (NFS3_PROGRAM, NFS_V3, udp).");
	    daemon_exit(0);
	}
    }

    if (tcptransp != NULL) {
	/* Register NFS service for TCP */
	if (!svc_register
	    (tcptransp, NFS3_PROGRAM, NFS_V3, nfs3_program_3,
	     opt_portmapper ? IPPROTO_TCP : 0)) {
	    fprintf(stderr, "%s\n",
		    "unable to register (NFS3_PROGRAM, NFS_V3, tcp).");
	    daemon_exit(0);
	}
    }
}

static void register_mount_service(SVCXPRT * udptransp, SVCXPRT * tcptransp)
{
    if (opt_portmapper) {
	pmap_unset(MOUNTPROG, MOUNTVERS1);
	pmap_unset(MOUNTPROG, MOUNTVERS3);
    }

    if (udptransp != NULL) {
	/* Register MOUNT service (v1) for UDP */
	if (!svc_register
	    (udptransp, MOUNTPROG, MOUNTVERS1, mountprog_3,
	     opt_portmapper ? IPPROTO_UDP : 0)) {
	    fprintf(stderr, "%s\n",
		    "unable to register (MOUNTPROG, MOUNTVERS1, udp).");
	    daemon_exit(0);
	}

	/* Register MOUNT service (v3) for UDP */
	if (!svc_register
	    (udptransp, MOUNTPROG, MOUNTVERS3, mountprog_3,
	     opt_portmapper ? IPPROTO_UDP : 0)) {
	    fprintf(stderr, "%s\n",
		    "unable to register (MOUNTPROG, MOUNTVERS3, udp).");
	    daemon_exit(0);
	}
    }

    if (tcptransp != NULL) {
	/* Register MOUNT service (v1) for TCP */
	if (!svc_register
	    (tcptransp, MOUNTPROG, MOUNTVERS1, mountprog_3,
	     opt_portmapper ? IPPROTO_TCP : 0)) {
	    fprintf(stderr, "%s\n",
		    "unable to register (MOUNTPROG, MOUNTVERS1, tcp).");
	    daemon_exit(0);
	}

	/* Register MOUNT service (v3) for TCP */
	if (!svc_register
	    (tcptransp, MOUNTPROG, MOUNTVERS3, mountprog_3,
	     opt_portmapper ? IPPROTO_TCP : 0)) {
	    fprintf(stderr, "%s\n",
		    "unable to register (MOUNTPROG, MOUNTVERS3, tcp).");
	    daemon_exit(0);
	}
    }
}

static SVCXPRT *create_udp_transport(unsigned int port)
{
    SVCXPRT *transp = NULL;
    struct sockaddr_in sin;
    int sock;
    const int on = 1;

    if (port == 0)
	sock = RPC_ANYSOCK;
    else {
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = opt_bind_addr;
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on));
	if (bind(sock, (struct sockaddr *) &sin, sizeof(struct sockaddr))) {
	    perror("bind");
	    fprintf(stderr, "Couldn't bind to udp port %d\n", port);
	    exit(1);
	}
    }

    transp = svcudp_bufcreate(sock, NFS_MAX_UDP_PACKET, NFS_MAX_UDP_PACKET);

    if (transp == NULL) {
	fprintf(stderr, "%s\n", "cannot create udp service.");
	daemon_exit(0);
    }

    return transp;
}

static SVCXPRT *create_tcp_transport(unsigned int port)
{
    SVCXPRT *transp = NULL;
    struct sockaddr_in sin;
    int sock;
    const int on = 1;

    if (port == 0)
	sock = RPC_ANYSOCK;
    else {
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = opt_bind_addr;
	sock = socket(PF_INET, SOCK_STREAM, 0);
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on));
	if (bind(sock, (struct sockaddr *) &sin, sizeof(struct sockaddr))) {
	    perror("bind");
	    fprintf(stderr, "Couldn't bind to tcp port %d\n", port);
	    exit(1);
	}
    }

    transp = svctcp_create(sock, 0, 0);

    if (transp == NULL) {
	fprintf(stderr, "%s\n", "cannot create tcp service.");
	daemon_exit(0);
    }

    return transp;
}

/*
 * Generate write verifier based on PID and current time
 */
void regenerate_write_verifier(void)
{
    *(wverf + 0) = (uint32) getpid();
    *(wverf + 0) ^= rand();
    *(wverf + 4) = (uint32) time(NULL);
}

/*
 * NFSD main function
 * originally generated by rpcgen
 * forking, logging, options, and signal handler stuff added
 */
int main(int argc, char **argv)
{
    register SVCXPRT *tcptransp = NULL, *udptransp = NULL;
    pid_t pid = 0;
    struct sigaction act;
    sigset_t actset;
    int res;

    parse_options(argc, argv);
    if (optind < argc) {
	fprintf(stderr, "Error: extra arguments on command line\n");
	exit(1);
    }

    /* init write verifier */
    regenerate_write_verifier();

    if (opt_detach) {
	/* prepare syslog access */
	openlog("unfsd", LOG_CONS | LOG_PID, LOG_DAEMON);
    } else {
	/* flush stdout after each newline */
	setvbuf(stdout, NULL, _IOLBF, 0);
    }

    /* NFS transports */
    if (!opt_tcponly)
	udptransp = create_udp_transport(opt_nfs_port);
    tcptransp = create_tcp_transport(opt_nfs_port);

    register_nfs_service(udptransp, tcptransp);

    /* MOUNT transports. If ports are equal, then the MOUNT service can reuse 
       the NFS transports. */
    if (opt_mount_port != opt_nfs_port) {
	if (!opt_tcponly)
	    udptransp = create_udp_transport(opt_mount_port);
	tcptransp = create_tcp_transport(opt_mount_port);
    }

    register_mount_service(udptransp, tcptransp);

    if (opt_detach) {
	pid = fork();
	if (pid == -1) {
	    fprintf(stderr, "could not fork into background\n");
	    daemon_exit(0);
	}
    }

    if (!opt_detach || pid == 0) {
	res = backend_init();
	if (res == -1) {
	    fprintf(stderr, "backend initialization failed\n");
	    daemon_exit(0);
	}

	sigemptyset(&actset);
	act.sa_handler = daemon_exit;
	act.sa_mask = actset;
	act.sa_flags = 0;
	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGQUIT, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGUSR1, &act, NULL);

	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, NULL);
	sigaction(SIGUSR2, &act, NULL);
	sigaction(SIGALRM, &act, NULL);

	/* don't make directory we started in busy */
	chdir("/");

	/* no umask to not screw up create modes */
	umask(0);

	/* detach from terminal */
	if (opt_detach) {
	    setsid();
	    fclose(stdin);
	    fclose(stdout);
	    fclose(stderr);
	}

	/* initialize internal stuff */
	fh_cache_init();
	fd_cache_init();
	get_squash_ids();
	exports_parse();

	svc_run();
	exit(1);
	/* NOTREACHED */
    }

    return 0;
}
