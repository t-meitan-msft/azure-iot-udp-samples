/*******************************************************************************
 * Copyright (c) 2014 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Sergio R. Caprile - "commonalization" from prior samples and/or documentation extension
 *******************************************************************************/

#include <sys/types.h>

#if !defined(SOCKET_ERROR)
/** error in socket operation */
#define SOCKET_ERROR -1
#endif

#if defined(WIN32)
/* default on Windows is 64 - increase to make Linux and Windows the same */
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#define MAXHOSTNAMELEN 256
#define EAGAIN WSAEWOULDBLOCK
#define EINTR WSAEINTR
#define EINVAL WSAEINVAL
#define EINPROGRESS WSAEINPROGRESS
#define EWOULDBLOCK WSAEWOULDBLOCK
#define ENOTCONN WSAENOTCONN
#define ECONNRESET WSAECONNRESET
#define ioctl ioctlsocket
#define socklen_t int
#else
#define INVALID_SOCKET SOCKET_ERROR
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(WIN32)
#include <Iphlpapi.h>
#else
#include <net/if.h>
#include <sys/ioctl.h>
#endif

#define SRC_PORT 1234

/**
This simple low-level implementation assumes a single connection for a single thread. Thus, a static
variable is used for that connection.
On other scenarios, the user must solve this by taking into account that the current implementation
of MQTTSNPacket_read() has a function pointer for a function call to get the data to a buffer, but
no provisions to know the caller or other indicator (the socket id): int (*getfn)(unsigned char*,
int)
*/
static int mysock = INVALID_SOCKET;

#ifdef SRC_PORT
struct sockaddr_in addr, srcaddr;
#endif

int Socket_error(char* aString, int sock)
{
#if defined(WIN32)
  int errno;
#endif

#if defined(WIN32)
  errno = WSAGetLastError();
#endif
  if (errno != EINTR && errno != EAGAIN && errno != EINPROGRESS && errno != EWOULDBLOCK)
  {
    if (strcmp(aString, "shutdown") != 0 || (errno != ENOTCONN && errno != ECONNRESET))
    {
      int orig_errno = errno;
      char* errmsg = strerror(errno);

      printf("Socket error %d (%s) in %s for socket %d\n", orig_errno, errmsg, aString, sock);
    }
  }
  return errno;
}

int transport_sendPacketBuffer(char* host, int port, unsigned char* buf, int buflen)
{
  struct sockaddr_in cliaddr;
  int rc = 0;

  memset(&cliaddr, 0, sizeof(cliaddr));
  cliaddr.sin_family = AF_INET;
  cliaddr.sin_addr.s_addr = inet_addr(host);
  cliaddr.sin_port = htons(port);

  if ((rc = sendto(mysock, buf, buflen, 0, (const struct sockaddr*)&cliaddr, sizeof(cliaddr)))
      == SOCKET_ERROR)
    Socket_error("sendto", mysock);
  else
    rc = 0;
  return rc;
}

int transport_getdata(unsigned char* buf, int count)
{
  int rc = recvfrom(mysock, buf, count, 0, NULL, NULL);
  // printf("received %d bytes count %d\n", rc, (int)count);
  return rc;
}

/**
return >=0 for a socket descriptor, <0 for an error code
*/
int transport_open()
{
  mysock = socket(AF_INET, SOCK_DGRAM, 0);
  if (mysock == INVALID_SOCKET)
    return Socket_error("socket", mysock);

  // set custom source port
#ifdef SRC_PORT
  memset(&srcaddr, 0, sizeof(srcaddr));
  srcaddr.sin_family = AF_INET;
  srcaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  srcaddr.sin_port = htons(SRC_PORT);

  if (bind(mysock, (struct sockaddr*)&srcaddr, sizeof(srcaddr)) < 0)
  {
    return Socket_error("socket", mysock);
  }
#endif

  return mysock;
}

int transport_close()
{
  int rc;

  rc = shutdown(mysock, SHUT_WR);
  rc = close(mysock);

  return rc;
}
