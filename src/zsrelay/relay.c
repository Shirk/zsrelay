/*
   This file is part of zsrelay a srelay port for the iPhone.

   Copyright (C) 2008 Rene Koecher <shirk@bitspin.org>

   Based on srelay 0.4.6 source base Copyright (C) 2001 Tomo.M
   Destributed under the GPL license with original authors permission.

   zsrelay is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>. 
   */

#include "zsrelay.h"

#ifdef HAVE_LIBWRAP
# include <tcpd.h>
# ifdef LINUX
#  include <syslog.h>
int    allow_severity = LOG_AUTH|LOG_INFO;
int    deny_severity  = LOG_AUTH|LOG_NOTICE;
# endif /* LINUX */
extern int hosts_ctl __P((char *, char *, char *, char *));
#endif /* HAVE_LIBWRAP */

#define TIMEOUTSEC   30

#ifdef IPHONE_OS
pthread_mutex_t stat_lock;

long stat_traffic_in  = 0;
long stat_traffic_out = 0;
long stat_connections = 0;
#endif

typedef struct
{
    int from;
    int to;
    size_t nr;
    size_t nw;
    ssize_t nread;
    ssize_t nwritten;
    int oob;     /* flag OOB */
    char buf[BUFSIZE];
} rlyinfo;

typedef struct
{
    struct sockaddr_storage mys;
    socklen_t  mys_len;
    struct sockaddr_storage myc;
    socklen_t  myc_len;
    struct sockaddr_storage prs;
    socklen_t  prs_len;
    struct sockaddr_storage prc;
    socklen_t  prc_len;
    u_long upl;
    u_long dnl;
    u_long bc;
    struct timeval elp;
} loginfo;

int resolv_client;

/* proto types */
void readn __P((rlyinfo *));
void writen __P((rlyinfo *));
ssize_t forward __P((rlyinfo *));
int validate_access __P((char *, char *));
int set_sock_info __P((loginfo *, int, int));
void relay __P((int, int));
int log_transfer __P((loginfo *));

#if IPHONE_OS_RELEASE >= 2
extern void iphone_app_check_connection __P((void));
#endif

void
accumulate_traffic(long *traffic_in, long *traffic_out, long *connections)
{
    pthread_mutex_lock(&stat_lock);
    if (traffic_in != NULL)
      *traffic_in = stat_traffic_in;

    if (traffic_out != NULL)
      *traffic_out = stat_traffic_out;

    if (connections != NULL)
      *connections = stat_connections;

    printf("accumulate: %ld;%ld;%ld\n", stat_traffic_in, stat_traffic_out,
	   stat_connections);
    pthread_mutex_unlock(&stat_lock);
}

void
readn(rlyinfo *ri)
{
    ri->nread = 0;
    if (ri->oob == 0)
      ri->nread = read(ri->from, ri->buf, ri->nr);
    else
      ri->nread = recvfrom(ri->from, ri->buf, ri->nr, MSG_OOB, NULL, 0);

    if (ri->nread < 0)
      msg_out(warn, "read: %m");
}

void
writen(rlyinfo *ri)
{
    ri->nwritten = 0;
    if (ri->oob == 0)
      ri->nwritten = write(ri->to, ri->buf, ri->nw);
    else
      ri->nwritten = sendto(ri->to, ri->buf, ri->nw, MSG_OOB, NULL, 0);

    if (ri->nwritten <= 0)
      msg_out(warn, "write: %m");
}

ssize_t
forward(rlyinfo *ri)
{
    settimer(TIMEOUTSEC);
    readn(ri);
    if (ri->nread > 0)
      {
	ri->nw = ri->nread;
	writen(ri);
      }
    settimer(0);

    if (ri->nread == 0)
      return(0);           /* EOF */

    if (ri->nread < 0)
      return(-1);

    return(ri->nwritten);
}

int
validate_access(char *client_addr, char *client_name)
{
    int stat = 0;
#ifdef HAVE_LIBWRAP
    int i;

    /* proc ident pattern */
    stat = hosts_ctl(ident, client_name, client_addr, STRING_UNKNOWN);
    /* IP.PORT pattern */
    for (i = 0; i < serv_sock_ind; i++)
      {
	if (str_serv_sock[i] != NULL && str_serv_sock[i][0] != 0)
	  {
	    stat |= hosts_ctl(str_serv_sock[i],
			      client_name, client_addr, STRING_UNKNOWN);
	  }
      }
#else
    stat = 1;  /* allow access un-conditionaly */
#endif

    if (stat < 1)
      msg_out(warn, "%s[%s] access denied.\n", client_name, client_addr);

    return stat;
}

int
set_sock_info(loginfo *li, int cs, int ss)
{
    /* prepare sockaddr info for logging */
    int len;

    len = sizeof(struct sockaddr_storage);
    /* get socket name of upstream side */
    getsockname(ss, (struct sockaddr *)&li->mys, &len);
    li->mys_len = len;

    len = sizeof(struct sockaddr_storage);
    /* get socket name of downstream side */
    getsockname(cs, (struct sockaddr *)&li->myc, &len);
    li->myc_len = len;

    len = sizeof(struct sockaddr_storage);
    /* get socket ss peer name */
    getpeername(ss, (struct sockaddr *)&li->prs, &len);
    li->prs_len = len;

    len = sizeof(struct sockaddr_storage);
    /* get socket cs peer name */
    getpeername(cs, (struct sockaddr *)&li->prc, &len);
    li->prc_len = len;

    return 0;
}


u_long idle_timeout = IDLE_TIMEOUT;

void
udp_relay(int cs, int ss, struct sockaddr_storage *udp)
{
    fd_set sockfd, ctlfd;
    struct sockaddr_storage sender;
    struct timeval timeout;

    uint8_t buf[65535];    // BIG!!
    uint8_t outbuf[65535]; // BIG!!
    ssize_t recvlen = 0;
    socklen_t addrlen = 0;
    struct socks_udpmsg *udpmsg = NULL;

    int is_clientmsg = 0;
    int learn_client = 0;

#ifdef IPHONE_OS
    pthread_mutex_lock(&stat_lock);
    stat_connections++;
    pthread_mutex_unlock(&stat_lock);
#endif

    sender.ss_family = udp->ss_family;
    if (udp->ss_family == AF_INET6)
      {
	struct sockaddr_in6 *udp_ip6 = NULL;
	udp_ip6 = (struct sockaddr_in6*)udp;

	if (udp_ip6->sin6_port == 0)
	    learn_client = 1;
      }
    else
      {
	struct sockaddr_in *udp_ip4 = NULL;
	udp_ip4 = (struct sockaddr_in*)udp;

	if (udp_ip4->sin_port == 0)
	    learn_client = 1;
      }

    errno = 0;

    for (;;)
      {
	FD_ZERO(&ctlfd);
	FD_SET(cs, &ctlfd);

	timeout.tv_sec  = 0;
	timeout.tv_usec = 100;

	if (select(cs + 1, &ctlfd, NULL, NULL, &timeout) == 1)
	  {
	    char c;
	    if (recv(cs, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0)
	      {
		msg_out(norm, "UDP: control channel closed\n");
		break;
	      }
	  }

	FD_ZERO(&sockfd);
	FD_SET(ss, &sockfd);

	timeout.tv_sec  = 0;
	timeout.tv_usec = 100;

	if (select(ss + 1, &sockfd, NULL, NULL, &timeout) == 0)
	    continue;

	bzero(buf, sizeof(buf));
	bzero(&sender, sizeof(struct sockaddr_storage));
	if (sender.ss_family == AF_INET6)
	  addrlen = sizeof(struct sockaddr_in6);
	else
	  addrlen = sizeof(struct sockaddr_in);

	recvlen = recvfrom(ss, buf, sizeof(buf), 0,
			   (struct sockaddr*)&sender, &addrlen);
	if (recvlen <= 0)
	  {
	    msg_out(warn, "UDP: recvfrom() - %s\n", strerror(errno));
	    break;
	  }

	/* validate request - who sent it? */
	udpmsg = (struct socks_udpmsg*)buf;
	is_clientmsg = 0;
	if (udpmsg->rsv == 0)
	  {
	    switch (udpmsg->atyp)
	      {
	      case S5ATIPV4:
		  {
		    struct sockaddr_in *udp_ip4 = (struct sockaddr_in*)udp;
		    struct sockaddr_in *sender_ip4 = (struct sockaddr_in*)&sender;

		    if (bcmp(&sender_ip4->sin_addr.s_addr,
			     &udp_ip4->sin_addr.s_addr,
			     sizeof(udp_ip4->sin_addr)) == 0)
		      is_clientmsg = 1;
		  }
		break;
		
	      case S5ATIPV6:
		  {
		    struct sockaddr_in6 *udp_ip6 = (struct sockaddr_in6*)udp;
		    struct sockaddr_in6 *sender_ip6 = (struct sockaddr_in6*)&sender;

//		    if (sender_ip6->sin6_port == udp_ip6->sin6_port)
		    if (bcmp(sender_ip6->sin6_addr.s6_addr,
			     udp_ip6->sin6_addr.s6_addr,
			     sizeof(udp_ip6->sin6_addr.s6_addr)) == 0)
		      is_clientmsg = 1;
		  }
		break;

	      default:
		break;
	      }
	  }

	if (is_clientmsg == 1)
	  {
	    /* looks like our client sent it */
	    if (udpmsg->atyp == S5ATIPV4)
	      {
		struct sockaddr_in remote;
		bzero(&remote, sizeof(remote));

		remote.sin_addr.s_addr = udpmsg->_addr._ip4.addr;
		remote.sin_port        = udpmsg->_addr._ip4.port;
		remote.sin_family      = AF_INET;
		remote.sin_len         = sizeof(remote);
		addrlen = sizeof(remote);

		if (learn_client == 1)
		  {
		    msg_out(norm, "UDP: client is sending on port %d\n",
			    ntohs(((struct sockaddr_in*)&sender)->sin_port));
		    ((struct sockaddr_in*)udp)->sin_port = ((struct sockaddr_in*)&sender)->sin_port;

		    learn_client = 0;
		  }

		if (sendto(ss, &buf[SOCKS_UDPMSG_V4_SIZE(udpmsg)],
			   recvlen - SOCKS_UDPMSG_V4_SIZE(udpmsg), 0,
			   (struct sockaddr*)&remote, addrlen) < 0)
		  {
		    msg_out(crit, "UDP: error sending IPv4 request to server\n");
		    break;
		  }
	      }
	    else
	      {
		struct sockaddr_in6 remote;
		bzero(&remote, sizeof(remote));

		remote.sin6_addr   = udpmsg->_addr._ip6.addr;
		remote.sin6_port   = udpmsg->_addr._ip6.port;
		remote.sin6_family = AF_INET6;
		remote.sin6_len    = sizeof(remote);
		addrlen = sizeof(remote);

		if (learn_client == 1)
		  {
		    msg_out(norm, "UDP: client is sending on port %d\n",
			    ntohs(((struct sockaddr_in6*)&sender)->sin6_port));
		    ((struct sockaddr_in6*)udp)->sin6_port = ((struct sockaddr_in6*)&sender)->sin6_port;

		    learn_client = 0;
		  }
		if (sendto(ss, &buf[SOCKS_UDPMSG_V6_SIZE(udpmsg)],
			   recvlen - SOCKS_UDPMSG_V6_SIZE(udpmsg), 0,
			   (struct sockaddr*)&remote, addrlen) < 0)
		  {
		    msg_out(crit, "UDP: error sending IPv6 request to server\n");
		    break;
		  }
	      }
	  }
	else
	  {
	    /* possible server response */
	    /* TODO: Is it neccessary to distinguish IPv4/v6 ? */

	    bzero(&outbuf, sizeof(outbuf));
	    udpmsg = (struct socks_udpmsg*)outbuf;

	    if (sender.ss_family == AF_INET6)
	      {
		struct sockaddr_in6 remote;
		struct sockaddr_in6 *udp_ip6 = NULL;
		struct sockaddr_in6 *sender_ip6 = NULL;
		
		bzero(&remote, sizeof(remote));
		udp_ip6 = (struct sockaddr_in6*)udp;
		sender_ip6 = (struct sockaddr_in6*)&sender;

		udpmsg->rsv  = 0;
		udpmsg->frag = 0;
		udpmsg->atyp = S5ATIPV6;
		/* this part is unclear - do I have to use _my_ address or the original? */
		udpmsg->_addr._ip6.addr = sender_ip6->sin6_addr;
		udpmsg->_addr._ip6.port = sender_ip6->sin6_port;

		memcpy(&outbuf[SOCKS_UDPMSG_V6_SIZE(udpmsg)], buf, recvlen);
		remote.sin6_addr    = udp_ip6->sin6_addr;
		remote.sin6_port    = udp_ip6->sin6_port;
		remote.sin6_family  = AF_INET6;
		remote.sin6_len     = sizeof(remote);
		addrlen = sizeof(remote);

		if (learn_client == 1)
		  {
		    msg_out(warn, "UDP: dropping server message - client is unknown!\n");
		    continue;
		  }
		if (sendto(ss, outbuf, recvlen + SOCKS_UDPMSG_V6_SIZE(udpmsg),
			   0, (struct sockaddr*)&remote, addrlen) < 0)
		  {
		    msg_out(crit, "UDP: error sending IPv6 response to client\n");
		    break;
		  }
	      }
	    else
	      {
		struct sockaddr_in remote;
		struct sockaddr_in *udp_ip4 = NULL;
		struct sockaddr_in *sender_ip4 = NULL;
		
		bzero(&remote, sizeof(remote));
		udp_ip4 = (struct sockaddr_in*)udp;
		sender_ip4 = (struct sockaddr_in*)&sender;

		udpmsg->rsv  = 0;
		udpmsg->frag = 0;
		udpmsg->atyp = S5ATIPV4;
		/* this part is unclear - do I have to use _my_ address or the original? */
		udpmsg->_addr._ip4.addr = sender_ip4->sin_addr.s_addr;
		udpmsg->_addr._ip4.addr = sender_ip4->sin_port;

		memcpy(&outbuf[SOCKS_UDPMSG_V4_SIZE(udpmsg)], buf, recvlen);
		remote.sin_addr.s_addr = udp_ip4->sin_addr.s_addr;
		remote.sin_port        = udp_ip4->sin_port;
		remote.sin_family      = AF_INET;
		remote.sin_len         = sizeof(remote);
		addrlen = sizeof(remote);

		if (learn_client == 1)
		  {
		    msg_out(warn, "UDP: dropping server message - client is unknown!\n");
		    continue;
		  }
		if (sendto(ss, outbuf, recvlen + SOCKS_UDPMSG_V4_SIZE(udpmsg),
			   0, (struct sockaddr*)&remote, addrlen) < 0)
		  {
		    msg_out(crit, "UDP: error sending IPv4 response to client\n");
		    break;
		  }
	      }
	  }
      }

    close(ss);
    close(cs);

#ifdef IPHONE_OS
    pthread_mutex_lock(&stat_lock);
    stat_connections--;
    pthread_mutex_unlock(&stat_lock);
#endif
}

void
relay(int cs, int ss)
{
    fd_set   rfds, xfds;
    int      nfds, sfd;
    struct   timeval tv, ts, ots, elp;
    struct   timezone tz;
    ssize_t  wc;
    rlyinfo  ri;
    int      done;
    u_long   max_count = idle_timeout;
    u_long   timeout_count;
    loginfo  li;
    loginfo  li_old;

#ifdef IPHONE_OS
    pthread_mutex_lock(&stat_lock);
    stat_connections++;
    pthread_mutex_unlock(&stat_lock);

#endif

    memset(&ri, 0, sizeof(ri));
    ri.nr = BUFSIZE;

    memset(&li, 0, sizeof(li));
    set_sock_info(&li, cs, ss);

    nfds = (ss > cs ? ss : cs);
    setsignal(SIGALRM, timeout);
    gettimeofday(&ots, &tz);
    li.bc = li.upl = li.dnl = 0; ri.oob = 0; timeout_count = 0;
    li_old.upl = li_old.dnl = 0;

    for (;;)
      {
#ifdef IPHONE_OS
	pthread_mutex_lock(&stat_lock);
	stat_traffic_in  += li.dnl - li_old.dnl;
	stat_traffic_out += li.upl - li_old.upl;
	pthread_mutex_unlock(&stat_lock);

	memcpy(&li_old, &li, sizeof(li_old));
#endif

	FD_ZERO(&rfds);
	FD_SET(cs, &rfds); FD_SET(ss, &rfds);

	if (ri.oob == 0)
	  {
	    FD_ZERO(&xfds);
	    FD_SET(cs, &xfds); FD_SET(ss, &xfds);
	  }
	done = 0;

	/* idle timeout related setting. */
	tv.tv_sec = 60; tv.tv_usec = 0;   /* unit = 1 minute. */
	tz.tz_minuteswest = 0; tz.tz_dsttime = 0;
	sfd = select(nfds+1, &rfds, 0, &xfds, &tv);
	if (sfd > 0)
	  {
	    if (FD_ISSET(ss, &rfds)) 
	      {
		ri.from = ss; ri.to = cs; ri.oob = 0;

		if ((wc = forward(&ri)) <= 0)
		  done++;
		else
		  li.bc += wc; li.dnl += wc;

		FD_CLR(ss, &rfds);
	      }

	    if (FD_ISSET(ss, &xfds))
	      {
		ri.from = ss; ri.to = cs; ri.oob = 1;

		if ((wc = forward(&ri)) <= 0)
		  done++;
		else
		  li.bc += wc; li.dnl += wc;

		FD_CLR(ss, &xfds);
	      }

	    if (FD_ISSET(cs, &rfds))
	      {
		ri.from = cs; ri.to = ss; ri.oob = 0;

		if ((wc = forward(&ri)) <= 0)
		  done++;
		else
		  li.bc += wc; li.upl += wc;

		FD_CLR(cs, &rfds);
	      }

	    if (FD_ISSET(cs, &xfds))
	      {
		ri.from = cs; ri.to = ss; ri.oob = 1;

		if ((wc = forward(&ri)) <= 0)
		  done++;
		else
		  li.bc += wc; li.upl += wc;

		FD_CLR(cs, &xfds);
	      }

	    if (done > 0)
	      break;

	  }
	else if (sfd < 0)
	  {
	    if (errno != EINTR)
	      break;

	  }
	else
	  { /* sfd == 0 */
	    if (max_count != 0)
	      {
		timeout_count++;

		if (timeout_count > max_count)
		  break;
	      }
	  }
      }

    gettimeofday(&ts, &tz);
    if (ts.tv_usec < ots.tv_usec)
      ts.tv_sec--; ts.tv_usec += 1000000;

    elp.tv_sec = ts.tv_sec - ots.tv_sec;
    elp.tv_usec = ts.tv_usec - ots.tv_usec;
    li.elp = elp;

    log_transfer(&li);

    close(ss);
    close(cs);

#ifdef IPHONE_OS
    pthread_mutex_lock(&stat_lock);
    stat_connections--;
    pthread_mutex_unlock(&stat_lock);
#endif
}

#ifdef USE_THREAD
pthread_mutex_t mutex_select;
pthread_mutex_t mutex_gh0;
#endif

int
serv_loop(void)
{
    int    cs, ss = 0;
    struct sockaddr_storage cl, udp;
    fd_set readable;
    int    i, n, len;
    char   cl_addr[NI_MAXHOST];
    char   cl_name[NI_MAXHOST];
    int    error;
    pid_t  pid;

#ifdef USE_THREAD
    if (threading)
      {
	blocksignal(SIGHUP);
	blocksignal(SIGINT);
	blocksignal(SIGUSR1);
      }
#endif

    for (;;)
      {
	readable = allsock;

	MUTEX_LOCK(mutex_select);
	n = select(maxsock+1, &readable, 0, 0, 0);

	if (n <= 0)
	  {
	    if (n < 0 && errno != EINTR)
	      msg_out(warn, "select: %m");

	    MUTEX_UNLOCK(mutex_select);
	    continue;
	  }

#ifdef USE_THREAD
	if ( ! threading )
	  {
#endif
	    /* handle any queued signal flags */
	    if (FD_ISSET(sig_queue[0], &readable))
	      {
		if (ioctl(sig_queue[0], FIONREAD, &i) != 0)
		  {
		    msg_out(crit, "ioctl: %m");
		    exit(-1);
		  }
		while (--i >= 0)
		  {
		    char c;
		    if (read(sig_queue[0], &c, 1) != 1)
		      {
			msg_out(crit, "read: %m");
			exit(-1);
		      }

		    switch(c)
		      {
		      case 'H': /* sighup */
			reload();
			break;

		      case 'C': /* sigchld */
			reapchild();
			break;

		      case 'T': /* sigterm */
			cleanup();
			break;

		      default:
			break;
		      }
		  }
	      }
#ifdef USE_THREAD
	  }
#endif

	for ( i = 0; i < serv_sock_ind; i++ )
	  {
	    if (FD_ISSET(serv_sock[i], &readable))
	      {
		n--;
		break;
	      }
	  }

	if ( n < 0 || i >= serv_sock_ind )
	  {
	    MUTEX_UNLOCK(mutex_select);
	    continue;
	  }

	len = sizeof(struct sockaddr_storage);
	cs = accept(serv_sock[i], (struct sockaddr *)&cl, &len);

	if (cs < 0)
	  {
	    if (errno == EINTR
#ifdef SOLARIS
		|| errno == EPROTO
#endif
		|| errno == EWOULDBLOCK
		|| errno == ECONNABORTED)
	      {
		; /* ignore */
	      }
	    else
	      /* real accept error */
	      msg_out(warn, "accept: %m");

	    MUTEX_UNLOCK(mutex_select);
	    continue;
	  }
	MUTEX_UNLOCK(mutex_select);

#ifdef USE_THREAD
	if ( !threading )
	  {
#endif
	    if (max_child > 0 && cur_child >= max_child)
	      {
		msg_out(warn, "child: cur %d; exeedeing max(%d)\n",
			cur_child, max_child);
		close(cs);
		continue;
	      }
#ifdef USE_THREAD
	  }
#endif

	error = getnameinfo((struct sockaddr *)&cl, len,
			    cl_addr, sizeof(cl_addr),
			    NULL, 0,
			    NI_NUMERICHOST);

	if (resolv_client)
	  {
	    error = getnameinfo((struct sockaddr *)&cl, len,
				cl_name, sizeof(cl_name),
				NULL, 0, 0);
	    msg_out(norm, "%s[%s] connected\n", cl_name, cl_addr);
	  }
	else
	  {
	    msg_out(norm, "%s connected\n", cl_addr);
	    strncpy(cl_name, cl_addr, sizeof(cl_name));
	  }

	i = validate_access(cl_addr, cl_name);
	if (i < 1)
	  {
	    /* access denied */
	    close(cs);
	    continue;
	  }

	set_blocking(cs);

#ifdef USE_THREAD
	if (!threading )
	  {
#endif
	    blocksignal(SIGHUP);
	    blocksignal(SIGCHLD);
	    pid = fork();
	    switch (pid)
	      {
	      case -1:  /* fork child failed */
		break;

	      case 0:   /* i am child */
		for ( i = 0; i < serv_sock_ind; i++ )
		  close(serv_sock[i]);

		setsignal(SIGCHLD, SIG_DFL);
		setsignal(SIGHUP, SIG_DFL);
		releasesignal(SIGCHLD);
		releasesignal(SIGHUP);
#ifdef IPHONE_OS
#if IPHONE_OS_RELEASE >= 2
		iphone_app_check_connection();
#endif
#endif
		bzero(&udp, sizeof(struct sockaddr_storage));
		udp.ss_family = AF_UNSPEC;

		ss = proto_socks(cs, &udp);
		if ( ss == -1 )
		  {
		    close(cs);  /* may already be closed */
		    exit(1);
		  }
		if (udp.ss_family == AF_UNSPEC)
		  {
		    errno = 0;
		    perror("start udp_relay()\n");
		    udp_relay(cs, ss, &udp);
		  }
		else
		  {
		    errno = 0;
		    perror("start relay()\n");
		    relay(cs, ss);
		  }

		exit(0);

	      default: /* may be parent */
		proclist_add(pid);
		break;
	      }

	    close(cs);
	    releasesignal(SIGHUP);
	    releasesignal(SIGCHLD);
#ifdef USE_THREAD
	  }
	else
	  {
#ifdef IPHONE_OS
#if IPHONE_OS_RELEASE >= 2
	    iphone_app_check_connection();
#endif
#endif
	    bzero(&udp, sizeof(struct sockaddr_storage));
	    udp.ss_family = AF_UNSPEC;

	    ss = proto_socks(cs, &udp);
	    if ( ss == -1 )
	      {
		close(cs);  /* may already be closed */
		continue;
	      }
	    if (udp.ss_family != AF_UNSPEC)
	      {
		errno = 0;
		udp_relay(cs, ss, &udp);
	      }
	    else
	      {
		errno = 0;
		relay(cs, ss);
	      }
	  }
#endif
      }
}

int
log_transfer(loginfo *li)
{

    char    prc_ip[NI_MAXHOST], prs_ip[NI_MAXHOST];
    char    myc_port[NI_MAXSERV], mys_port[NI_MAXSERV];
    char    prc_port[NI_MAXSERV], prs_port[NI_MAXSERV];
    int     error = 0;

    error = getnameinfo((struct sockaddr *)&li->myc, li->myc_len,
			NULL, 0,
			myc_port, sizeof(myc_port),
			NI_NUMERICHOST|NI_NUMERICSERV);
    error = getnameinfo((struct sockaddr *)&li->mys, li->mys_len,
			NULL, 0,
			mys_port, sizeof(mys_port),
			NI_NUMERICHOST|NI_NUMERICSERV);
    error = getnameinfo((struct sockaddr *)&li->prc, li->prc_len,
			prc_ip, sizeof(prc_ip),
			prc_port, sizeof(prc_port),
			NI_NUMERICHOST|NI_NUMERICSERV);
    error = getnameinfo((struct sockaddr *)&li->prs, li->prs_len,
			prs_ip, sizeof(prs_ip),
			prs_port, sizeof(prs_port),
			NI_NUMERICHOST|NI_NUMERICSERV);

    msg_out(norm, "%s:%s-%s/%s-%s:%s %u(%u/%u) %u.%06u\n",
	    prc_ip, prc_port, myc_port,
	    mys_port, prs_ip, prs_port,
	    li->bc, li->upl, li->dnl,
	    li->elp.tv_sec, li->elp.tv_usec);

    return(0);
}

