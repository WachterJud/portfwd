/*
  forward.c

  $Id: forward.cc,v 1.1 2001/05/15 00:25:04 evertonm Exp $
 */

#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include "portfwd.h"
#include "forward.h"
#include "util.h"
#include "solve.h"
#include "host_map.hpp"
#include "iterator.hpp"

int grandchild_pid[MAX_FD];

/*
 * Returns -1 on failure; file descriptor on success.
 */
int tcp_connect(const struct ip_addr *ip, int port)
{
  int rsd = socket(PF_INET, SOCK_STREAM, get_protonumber(P_TCP));
  if (rsd == -1) {
    syslog(LOG_ERR, "tcp_connect: Can't create TCP socket: %m");
    return -1;
  }
  DEBUGFD(syslog(LOG_DEBUG, "socket open: FD %d", rsd));

  struct sockaddr_in sa;
  sa.sin_family      = PF_INET;
  sa.sin_port        = htons(port);
  sa.sin_addr.s_addr = *((unsigned int *) ip->addr);
  memset((char *) sa.sin_zero, 0, sizeof(sa.sin_zero));

  if (connect(rsd, (struct sockaddr *) &sa, sizeof(sa))) {
    syslog(LOG_ERR, "tcp_connect: Can't connect to %s:%d: %m", inet_ntoa(sa.sin_addr), port);
    socket_close(rsd);
    return -1;
  }

  return rsd;
}

/*
 * Returns host_map success; NULL on failure.
 */
const host_map *tcp_match(const vector<host_map*> *map_list, const struct ip_addr *ip, int port)
{
  const host_map *hm;
  iterator<vector<host_map*>,host_map*> it(*map_list);
  for (it.start(); it.cont(); it.next()) {
    hm = it.get();
    if (hm->tcp_match(ip, port))
      return hm;
  }
  return 0;
}

void quit_handler(int sig)
{
  ONVERBOSE(syslog(LOG_DEBUG, "child: quit_handler: Grandchild with PID %d exiting under request", getpid()));

  exit(0);
}

int simple_buf_copy(int src_fd, int trg_fd)
{
  char buf[BUF_SZ];
  int rd = read(src_fd, buf, BUF_SZ);
  if (!rd)
    return -1;
  if (rd < 0) {
    syslog(LOG_ERR, "simple_copy: Failure reading from socket: %m");
    return -1;
  }

  int wr = write(trg_fd, buf, rd);
  if (wr == -1) {
    if (errno == EPIPE)
      ONVERBOSE2(syslog(LOG_DEBUG, "simple_copy: Broken pipe: %m"));
    return -1;
  }
  if (wr < rd) {
    ONVERBOSE(syslog(LOG_WARNING, "simple_copy: Partial write to socket: %m"));
    return -1;
  }

  return 0;
}

void simple_tcp_forward(int sd, struct ip_addr *remote_ip, int remote_port)
{
  int dest_fd[MAX_FD];

  fd_set fds, tmp_fds;
  int fd_set_len = sizeof(fd_set);
  FD_ZERO(&fds);
  int maxfd = 0;
  fdset(sd, &fds, &maxfd);
  int nd;

  for (;;) { /* forever */
    /*
     * Restores tmp_fds from fds.
     */
    memcpy(&tmp_fds, &fds, fd_set_len);

    /*
     * Wait for event: connection on sd or data on anyone else.
     */
    DEBUGFD(syslog(LOG_DEBUG, "simple_forward: TCP select(): %d FDs", maxfd));
    nd = select(maxfd, &tmp_fds, 0, 0, 0);
    if (nd == -1) {
      syslog(LOG_ERR, "simple_forward: TCP select() failed: %m");
      continue;
    }

    /*
     * Let's handle sd here (mother socket).
     */
    if (FD_ISSET(sd, &tmp_fds)) {

      --nd;

      /*
       * Clear sd so it's ignored below in the
       * loop for the client sockets.
       */
      FD_CLR(sd, &tmp_fds); 

      struct sockaddr_in cli_sa;
      unsigned int cli_sa_len = sizeof(cli_sa);
      int csd = accept(sd, (struct sockaddr *) &cli_sa, &cli_sa_len);
      if (csd < 0)
	syslog(LOG_ERR, "simple_forward: Can't accept TCP socket: %m");
      else {
	int cli_port = ntohs(cli_sa.sin_port);
	ONVERBOSE(syslog(LOG_DEBUG, "simple_forward: TCP connection from %s:%d", inet_ntoa(cli_sa.sin_addr), cli_port));

	/*
	 * Connect to destination.
	 */
	  
	int rsd = tcp_connect(remote_ip, remote_port);
	if (rsd != -1) {
	  if ((csd >= MAX_FD) || (rsd >= MAX_FD)) {
	    syslog(LOG_ERR, "simple_forward: Destination socket descriptors overflow");
	    socket_close(csd);
	    socket_close(rsd);
	  }
	  else {
	    /*
	     * Add pair of communicating sockets.
	     */
	    fdset(csd, &fds, &maxfd);
	    fdset(rsd, &fds, &maxfd);
	  
	    /*
	     * Save peers so they can be remembered later.
	     */
	    dest_fd[csd] = rsd;
	    dest_fd[rsd] = csd;
	  }
	}
      }

    } /* sd (mother socket) handled */

    /*
     * Now all other sockets.
     */
    for (int src_fd = 0; nd; ++src_fd)
      if (FD_ISSET(src_fd, &tmp_fds)) {
	--nd;
	int trg_fd = dest_fd[src_fd];
	/*
	 * Copy data.
	 */
	int fail = simple_buf_copy(src_fd, trg_fd);
	if (fail) {
	  /*
	   * Remove pair of communicating sockets.
	   */
	  DEBUGFD(syslog(LOG_DEBUG, "simple_forward: closed socket (FD %d or %d)", src_fd, trg_fd));

	  fdclear(src_fd, &fds, &maxfd);
	  fdclear(trg_fd, &fds, &maxfd);
	  socket_close(src_fd);
	  socket_close(trg_fd);
	}
      }

  } /* main loop */

}

int ftp_spawn(struct ip_addr *local_ip, int *local_port, struct ip_addr *remote_ip, int remote_port)
{
  int sd = tcp_listen(local_ip, local_port, 3);
  if (sd == -1) {
    syslog(LOG_ERR, "FTP spawn: Can't listen: %m");
    return -1;
  }

  pid_t pid = fork();
  if (pid) {
    /* Child */

    if (pid < 0)
      syslog(LOG_ERR, "FTP spawn: fork() failed: %m");

    socket_close(sd);

    return pid;
  }

  /* Grandchild */

  void (*prev_handler)(int);
  prev_handler = signal(SIGCHLD, SIG_IGN);
  if (prev_handler == SIG_ERR) {
    syslog(LOG_ERR, "signal() failed on ignore: %m");
    exit(1);
  }
  prev_handler = signal(SIGUSR1, quit_handler);
  if (prev_handler == SIG_ERR) {
    syslog(LOG_ERR, "signal() failed for grandchild's quit handler: %m");
    exit(1);
  }

  simple_tcp_forward(sd, remote_ip, remote_port);

  syslog(LOG_ERR, "Grandchild exiting (!)");
  exit(1);
}

void gc_clean(int fd)
{
  pid_t pid = grandchild_pid[fd];
  if (pid != -1) {
    ONVERBOSE(syslog(LOG_DEBUG, "Requesting termination of PID %d for FD %d", pid, fd));

    if (kill(pid, SIGUSR1))
      syslog(LOG_ERR, "Can't request grandchild termination for PID: %d: %m", pid);
    grandchild_pid[fd] = -1;
  }
}

void gc_fill(int fd, pid_t pid)
{
  gc_clean(fd);

  grandchild_pid[fd] = pid;

  ONVERBOSE(syslog(LOG_DEBUG, "PID %d stored for termination request on FD %d", pid, fd));
}

int ftp_active(const struct ip_addr *actv_ip, char *buf, int *rd, int src_fd, int trg_fd)
{
  if (strncasecmp(buf, "port", 4))
    return 0;

  ONVERBOSE(syslog(LOG_DEBUG, "Active FTP request detected: %s", buf));

  char *i = strchr(buf, ' ');
  if (!i) {
    syslog(LOG_ERR, "Missing remote address in active FTP request");
    return -1;
  }
  ++i;

  int ad[4];
  int port[2];
  
  if (sscanf(i, "%d,%d,%d,%d,%d,%d", 
	    &ad[0], &ad[1], &ad[2], &ad[3],
	    &port[0], &port[1]) < 6) {
    syslog(LOG_ERR, "Active FTP request mismatch");
    return -1;
  }

  /*
   * Remote address.
   */

  char addr[4];
  addr[0] = ad[0];
  addr[1] = ad[1];
  addr[2] = ad[2];
  addr[3] = ad[3];

  struct ip_addr remote_ip;
  remote_ip.len = addr_len;
  remote_ip.addr = addr;
  int remote_port = (port[0] << 8) | port[1];

  ONVERBOSE(syslog(LOG_DEBUG, "Remote address extracted: %s:%d", addrtostr(&remote_ip), remote_port));

  /*
   * Local address (0.0.0.0).
   */

  int laddr = INADDR_ANY;
  struct ip_addr local_ip;
  local_ip.len = addr_len;
  local_ip.addr = (char *) &laddr;

  /*
   * FTP forwarder.
   */
    
  int local_port = 0;

  pid_t pid = ftp_spawn(&local_ip, &local_port, &remote_ip, remote_port);
  if (pid == -1) {
    syslog(LOG_ERR, "Can't spawn FTP forwarder");
    /* free(local_ip.addr); */
    return -1;
  }

  /*
   * Address rewriting (IP given by ftp-active-mode-on).
   */
  const char *local_addr = actv_ip->addr;
  if (snprintf(buf, BUF_SZ, "PORT %u,%u,%u,%u,%u,%u\n", 
	       (unsigned char) local_addr[0], 
	       (unsigned char) local_addr[1], 
	       (unsigned char) local_addr[2], 
	       (unsigned char) local_addr[3],
	       (local_port >> 8) & 0xFF, local_port & 0xFF) == -1)
    syslog(LOG_WARNING, "Active FTP request truncated");

  *rd = strlen(buf);

  /*
   * Store forwarder's PID so it can be terminated.
   */
  gc_fill(src_fd, pid);
  gc_fill(trg_fd, pid);

  return 0;
}

int ftp_passive(const struct ip_addr *pasv_ip, char *buf, int *rd, int src_fd, int trg_fd)
{
  if (memcmp(buf, "227", 3))
    return 0;

  ONVERBOSE(syslog(LOG_DEBUG, "Passive FTP reply detected: %s", buf));

  char *i = strchr(buf, '(');
  if (!i) {
    syslog(LOG_ERR, "Missing remote address in passive FTP reply");
    return -1;
  }
  ++i;

  int ad[4];
  int port[2];
  
  if (sscanf(i, "%d,%d,%d,%d,%d,%d", 
	    &ad[0], &ad[1], &ad[2], &ad[3],
	    &port[0], &port[1]) < 6) {
    syslog(LOG_ERR, "Passive FTP reply mismatch");
    return -1;
  }

  /*
   * Remote address.
   */

  char addr[4];
  addr[0] = ad[0];
  addr[1] = ad[1];
  addr[2] = ad[2];
  addr[3] = ad[3];

  struct ip_addr remote_ip;
  remote_ip.len = addr_len;
  remote_ip.addr = addr;
  int remote_port = (port[0] << 8) | port[1];

  ONVERBOSE(syslog(LOG_DEBUG, "Remote address extracted: %s:%d", addrtostr(&remote_ip), remote_port));

  /*
   * Local address (0.0.0.0).
   */

  int laddr = INADDR_ANY;
  struct ip_addr local_ip;
  local_ip.len = addr_len;
  local_ip.addr = (char *) &laddr;

  /*
   * FTP forwarder.
   */
    
  int local_port = 0;

  pid_t pid = ftp_spawn(&local_ip, &local_port, &remote_ip, remote_port);
  if (pid == -1) {
    syslog(LOG_ERR, "Can't spawn FTP forwarder");
    return -1;
  }

  /*
   * Address rewriting (IP given by ftp-passive-mode-on).
   */
  const char *local_addr = pasv_ip->addr;
  if (snprintf(buf, BUF_SZ, "227 Portfwd Passive Mode (%u,%u,%u,%u,%u,%u)\n", 
	       (unsigned char) local_addr[0], 
	       (unsigned char) local_addr[1], 
	       (unsigned char) local_addr[2], 
	       (unsigned char) local_addr[3],
	       (local_port >> 8) & 0xFF, local_port & 0xFF) == -1)
    syslog(LOG_WARNING, "Passive FTP reply truncated");

  *rd = strlen(buf);

  /*
   * Store forwarder's PID so it can be terminated.
   */
  gc_fill(src_fd, pid);
  gc_fill(trg_fd, pid);

  return 0;
}

int buf_copy(int src_fd, int trg_fd, const struct ip_addr *actv_ip, const struct ip_addr *pasv_ip)
{
  char buf[BUF_SZ];
  int rd = read(src_fd, buf, BUF_SZ);
  if (!rd)
    return -1;
  if (rd < 0) {
    syslog(LOG_ERR, "copy: Failure reading from socket: %m");
    return -1;
  }

  if (actv_ip)
    if (ftp_active(actv_ip, buf, &rd, src_fd, trg_fd))
      return -1;

  if (pasv_ip)
    if (ftp_passive(pasv_ip, buf, &rd, src_fd, trg_fd))
      return -1;

  int wr = write(trg_fd, buf, rd);
  if (wr == -1) {
    if (errno == EPIPE)
      ONVERBOSE2(syslog(LOG_DEBUG, "copy: Broken pipe: %m"));
    return -1;
  }
  if (wr < rd) {
    ONVERBOSE(syslog(LOG_WARNING, "copy: Partial write to socket: %m"));
    return -1;
  }

  return 0;
}

int drop_privileges(int uid, int gid)
{
  if (gid != -1)
    if (setregid(gid, gid)) {
      syslog(LOG_ERR, "Can't switch to group: %d: %m", gid);
      return -1;
    }

  if (uid != -1)
    if (setreuid(uid, uid)) {
      syslog(LOG_ERR, "Can't switch to user: %d: %m", uid);
      return -1;
    }

  return 0;
}

int tcp_listen(const struct ip_addr *ip, int *port, int queue)
{
  int sd = socket(PF_INET, SOCK_STREAM, get_protonumber(P_TCP));
  if (sd == -1) {
    syslog(LOG_ERR, "listen: Can't create TCP socket: %m");
    return -1;
  }
  DEBUGFD(syslog(LOG_DEBUG, "socket open: FD %d", sd));
    
  int prt = port ? *port : 0;
  
  struct sockaddr_in sa;
  unsigned int sa_len = sizeof(sa);
  sa.sin_family      = PF_INET;
  sa.sin_port        = htons(prt);
  sa.sin_addr.s_addr = *((unsigned int *) ip->addr);
  memset((char *) sa.sin_zero, 0, sizeof(sa.sin_zero));

  if (bind(sd, (struct sockaddr *) &sa, sa_len)) {
    syslog(LOG_ERR, "listen: Can't bind TCP socket: %m: %s:%d", inet_ntoa(sa.sin_addr), prt);
    socket_close(sd);
    return -1;
  }

  if (getsockname(sd, (struct sockaddr *) &sa, &sa_len)) {
    syslog(LOG_ERR, "listen: Can't get local sockname: %m");
    return -1;
  }
  prt = ntohs(sa.sin_port);

  if (listen(sd, queue)) {
    syslog(LOG_ERR, "listen: Can't listen TCP socket: %m");
    socket_close(sd);
    return -1;
  }

  ONVERBOSE(syslog(LOG_DEBUG, "Listening TCP connection on %s:%d", inet_ntoa(sa.sin_addr), prt));

  if (port)
    *port = prt;

  return sd;
}

void close_sockets(vector<int> *port_list) 
{
  iterator<vector<int>,int> it(*port_list);
  for (it.start(); it.cont(); it.next())
    close(it.get());
  DEBUGFD(syslog(LOG_DEBUG, "close_sockets(): Sockets closed: %d", port_list->get_size()));
}

void mother_socket(int sd, fd_set *fds, int dest_fd[], int *maxfd, vector<host_map*> *map_list)
{
  struct sockaddr_in cli_sa;
  unsigned int cli_sa_len = sizeof(cli_sa);

  /*
   * Accept new client.
   */
  int csd = accept(sd, (struct sockaddr *) &cli_sa, &cli_sa_len);
  if (csd < 0) {
    syslog(LOG_ERR, "Can't accept TCP socket: %m");
    return;
  }
	
  int cli_port = ntohs(cli_sa.sin_port);
  ONVERBOSE(syslog(LOG_DEBUG, "TCP connection from %s:%d", inet_ntoa(cli_sa.sin_addr), cli_port));

  /*
   * Open socket for remote host
   */
  int rsd = socket(PF_INET, SOCK_STREAM, get_protonumber(P_TCP));
  if (rsd == -1) {
    syslog(LOG_ERR, "mother_socket: Can't create TCP socket: %m");
    return;
  }
  DEBUGFD(syslog(LOG_DEBUG, "mother_socket: socket open: FD %d", rsd));

  /*
   * Perform transparent proxy
   */
  if (transparent_proxy) {

    struct sockaddr_in local_sa;  
    unsigned int local_sa_len = sizeof(local_sa);

    /*
     * Copy client address
     */
    memcpy(&local_sa, &cli_sa, cli_sa_len);
    local_sa.sin_port = htons(0);

    ONVERBOSE(syslog(LOG_ERR, "mother_socket: Transparent proxy - Binding to local address: %s:%d", inet_ntoa(local_sa.sin_addr), htons(local_sa.sin_port)));

    /*
     * Bind local socket to client address
     */
    if (bind(rsd, (struct sockaddr *) &local_sa, local_sa_len)) {
      syslog(LOG_ERR, "mother_socket: Can't bind TCP socket to client address: %m: %s:%d", inet_ntoa(local_sa.sin_addr), ntohs(local_sa.sin_port));
      socket_close(rsd);
      return;
    }

  }
	  
  /*
   * Connect to destination.
   */
  struct ip_addr ip;
  ip.addr = (char *) &(cli_sa.sin_addr.s_addr);
  ip.len  = addr_len;

  /*
   * Check client address (ip, port).
   */
  const host_map *hm = tcp_match(map_list, &ip, cli_port);
  if (!hm) {
    ONVERBOSE(syslog(LOG_DEBUG, "Address miss"));
    socket_close(csd);
    socket_close(rsd);
    return;
  }
  ONVERBOSE(syslog(LOG_DEBUG, "Address match"));

  /*
   * Connect to destination
   */
  if (hm->pipe(rsd, &ip, cli_port)) {
    ONVERBOSE(syslog(LOG_DEBUG, "Could not connect to remote destination"));
    socket_close(csd);
    socket_close(rsd);
    return;
  }
  
  if ((csd >= MAX_FD) || (rsd >= MAX_FD)) {
    syslog(LOG_ERR, "Destination socket descriptors overflow");
    socket_close(csd);
    socket_close(rsd);
    return;
  }

  /*
   * Add pair of communicating sockets.
   */
  fdset(csd, fds, maxfd);
  fdset(rsd, fds, maxfd);
  
  /*
   * Save peers so they can be remembered later.
   */
  dest_fd[csd] = rsd;
  dest_fd[rsd] = csd;
}

void client_socket(int src_fd, fd_set *fds, int dest_fd[], int *maxfd, const struct ip_addr *actv_ip, const struct ip_addr *pasv_ip)
{
  /*
   * Copy data.
   */
  int trg_fd = dest_fd[src_fd];
  int fail = buf_copy(src_fd, trg_fd, actv_ip, pasv_ip);
  if (fail) {
    /*
     * Remove pair of communicating sockets.
     */
    DEBUGFD(syslog(LOG_DEBUG, "client_socket: closed socket (FD %d or %d)", src_fd, trg_fd));

    fdclear(src_fd, fds, maxfd);
    fdclear(trg_fd, fds, maxfd);
    socket_close(src_fd);
    socket_close(trg_fd);
    gc_clean(src_fd);
    gc_clean(trg_fd);
  }
}

void tcp_forward(const struct ip_addr *local, vector<int> *port_list, vector<host_map*> *map_list, const struct ip_addr *actv_ip, const struct ip_addr *pasv_ip, int uid, int gid)
{
  fd_set fds, tmp_fds, ms_fds;
  int fd_set_len = sizeof(fd_set);
  int maxfd = 0;

  FD_ZERO(&fds);
  FD_ZERO(&ms_fds);

  iterator<vector<int>,int> it(*port_list);
  for (it.start(); it.cont(); it.next()) {

    int port = it.get();
    int sd = tcp_listen(local, &port, 3);
    if (sd == -1) {
      close_sockets(port_list);
      return;
    }

    fdset(sd, &fds, &maxfd); /* Add sd to set for select() */
    FD_SET(sd, &ms_fds);     /* Mark sd as mother socket */
  }

  if (drop_privileges(uid, gid)) {
    close_sockets(port_list);
    return;
  }

  int dest_fd[MAX_FD];

  for (int fd = 0; fd < MAX_FD; ++fd)
    grandchild_pid[fd] = -1;

  for (;;) { /* forever */
    /*
     * Restores tmp_fds from fds.
     */
    memcpy(&tmp_fds, &fds, fd_set_len);

    /*
     * Wait for event: connection on mother sockets or data on anything else.
     */
    DEBUGFD(syslog(LOG_DEBUG, "TCP select(): %d FDs", maxfd));
    int nd = select(maxfd, &tmp_fds, 0, 0, 0);
    if (nd == -1) {
      if (errno != EINTR)
	syslog(LOG_ERR, "TCP select() failed: %m");
      continue;
    }

    for (int fd = 0; nd; ++fd)
      if (FD_ISSET(fd, &tmp_fds)) {
	--nd;
	if (FD_ISSET(fd, &ms_fds))
	  mother_socket(fd, &fds, dest_fd, &maxfd, map_list);
	else
	  client_socket(fd, &fds, dest_fd, &maxfd, actv_ip, pasv_ip);
      }

  } /* main loop */

}

void do_udp_forward(const struct sockaddr_in *sa, vector<host_map*> *map_list, const char *buf, int buf_len)
{
  int            port = ntohs(sa->sin_port);
  struct ip_addr ip;

  ONVERBOSE(syslog(LOG_DEBUG, "UDP packet from: %s:%d\n", 
		   inet_ntoa(sa->sin_addr), 
		   ntohs(sa->sin_port)));

  ip.addr = (char *) &(sa->sin_addr.s_addr);
  ip.len  = addr_len;

  iterator<vector<host_map*>,host_map*> it(*map_list);
  for (it.start(); it.cont(); it.next()) {
    const host_map *hm = it.get();
    if (hm->udp_match(&ip, port, buf, buf_len)) {
      hm->udp_forward(sa, &ip, port, buf, buf_len);
      break;
    }
  }
}

int udp_listen(const struct ip_addr *ip, int port)
{
  int sd = socket(PF_INET, SOCK_DGRAM, get_protonumber(P_UDP));
  if (sd == -1) {
    syslog(LOG_ERR, "Can't create UDP socket: %m");
    return -1;
  }
  DEBUGFD(syslog(LOG_DEBUG, "socket open: FD %d", sd));
    
  struct sockaddr_in sa;
  sa.sin_family      = PF_INET;
  sa.sin_port        = htons(port);
  sa.sin_addr.s_addr = *((unsigned int *) ip->addr);
  memset((char *) sa.sin_zero, 0, sizeof(sa.sin_zero));

  if (bind(sd, (struct sockaddr *) &sa, sizeof(sa))) {
    syslog(LOG_ERR, "Can't bind UDP socket: %m: %s:%d", inet_ntoa(sa.sin_addr), port);
    socket_close(sd);
    return -1;
  }

  ONVERBOSE(syslog(LOG_DEBUG, "Waiting UDP packet on %s:%d", inet_ntoa(sa.sin_addr), port));

  return sd;
}

void udp_forward(const struct ip_addr *local, vector<int> *port_list, vector<host_map*> *map_list, int uid, int gid)
{
  fd_set fds, tmp_fds;
  int fd_set_len = sizeof(fd_set);
  int maxfd = 0;

  FD_ZERO(&fds);

  iterator<vector<int>,int> it(*port_list);
  for (it.start(); it.cont(); it.next()) {

    int port = it.get();
    int sd = udp_listen(local, port);
    if (sd == -1) {
      close_sockets(port_list);
      return;
    }

    fdset(sd, &fds, &maxfd);
  }

  if (drop_privileges(uid, gid)) {
    close_sockets(port_list);
    return;
  }

  char buf[BUF_SZ];
  struct sockaddr_in cli_sa;
  unsigned int cli_sa_len = sizeof(struct sockaddr_in);

  for (;;) { /* forever */
    /*
     * Restores tmp_fds from fds.
     */
    memcpy(&tmp_fds, &fds, fd_set_len);

    /*
     * Wait for data.
     */
    DEBUGFD(syslog(LOG_DEBUG, "UDP select(): %d FDs", maxfd));
    int nd = select(maxfd, &tmp_fds, 0, 0, 0);
    if (nd == -1) {
      if (errno != EINTR)
	syslog(LOG_ERR, "UDP select() failed: %m");
      continue;
    }

    for (int fd = 0; nd; ++fd)
      if (FD_ISSET(fd, &tmp_fds)) {
	--nd;

	int rd = recvfrom(fd, buf, BUF_SZ, 0, (struct sockaddr *) &cli_sa, &cli_sa_len);
	if (rd == -1) {
	  syslog(LOG_ERR, "Can't receive UDP packet: %m");
	  continue;
	}

	do_udp_forward(&cli_sa, map_list, buf, rd);
      }

  } /* main loop */

}
