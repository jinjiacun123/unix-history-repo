/*
 *	      PPP OS Layer Interface Module
 *
 *	    Written by Toshiharu OHNO (tony-o@iij.ad.jp)
 *
 *   Copyright (C) 1993, Internet Initiative Japan, Inc. All rights reserverd.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the Internet Initiative Japan, Inc.  The name of the
 * IIJ may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * $Id: os.c,v 1.2 1995/02/26 12:17:48 amurai Exp $
 *
 */
#include "fsm.h"
#include <sys/param.h>
#include <sys/socket.h>
#include <net/route.h>
#if BSD >= 199206 || _BSDI_VERSION >= 199312
#include <sys/select.h>
#endif
#include <sys/ioctl.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_tun.h>
#include <errno.h>
#include "ipcp.h"
#include "os.h"
#include "vars.h"

static struct ifaliasreq ifra;
static struct ifreq ifrq;
static struct in_addr oldmine, oldhis;
static int linkup;

#ifdef bsdi
extern char *inet_ntoa();
#endif
extern void HangupModem();

char *IfDevName;

static int
SetIpDevice(myaddr, hisaddr, netmask, updown)
struct in_addr myaddr, hisaddr, netmask;
int updown;
{
  struct sockaddr_in *sin;
  int s;
  int changeaddr = 0;
  u_long mask, addr;

  s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    perror("socket");
    return(-1);
  }

  if (updown == 0) {
    if (Enabled(ConfProxy))
      cifproxyarp(s, oldhis.s_addr);
    if (oldmine.s_addr == 0 && oldhis.s_addr == 0) {
      close(s);
      return(0);
    }
    bzero(&ifra.ifra_addr, sizeof(ifra.ifra_addr));
    bzero(&ifra.ifra_broadaddr, sizeof(ifra.ifra_addr));
    bzero(&ifra.ifra_mask, sizeof(ifra.ifra_addr));
#ifdef DEBUG
    logprintf("DIFADDR\n");
#endif
    if (ioctl(s, SIOCDIFADDR, &ifra) < 0) {
      perror("SIOCDIFADDR");
      close(s);
      return(-1);
    }

    oldmine.s_addr = oldhis.s_addr = 0;
  } else {
    /*
     * If given addresses are alreay set, then ignore this request.
     */
    if (oldmine.s_addr == myaddr.s_addr && oldhis.s_addr == hisaddr.s_addr)
      return(0);
    /*
     * If different address has been set, then delete it first.
     */
    if (oldmine.s_addr || oldhis.s_addr) {
      changeaddr = 1;
    }
    /*
     *  Set interface address
     */
    sin = (struct sockaddr_in *)&(ifra.ifra_addr);
    sin->sin_family = AF_INET;
    sin->sin_addr = oldmine = myaddr;
    sin->sin_len = sizeof(*sin);
    /*
     *  Set destination address
     */
    sin = (struct sockaddr_in *)&(ifra.ifra_broadaddr);
    sin->sin_family = AF_INET;
    sin->sin_addr = oldhis = hisaddr;
    sin->sin_len = sizeof(*sin);
    /*
     */
    addr = ntohl(myaddr.s_addr);
    if (IN_CLASSA(addr))
      mask = IN_CLASSA_NET;
    else if (IN_CLASSB(addr))
      mask = IN_CLASSB_NET;
    else
      mask = IN_CLASSC_NET;
    /*
     *  if subnet mask is given, use it instead of class mask.
     */
    if (netmask.s_addr && (ntohl(netmask.s_addr) & mask) == mask)
      mask = ntohl(netmask.s_addr);

    sin = (struct sockaddr_in *)&(ifra.ifra_mask);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(mask);
    sin->sin_len = sizeof(*sin);

    if (changeaddr) {
      /*
       * Interface already exists. Just change the address.
       */
      bcopy(&ifra.ifra_addr, &ifrq.ifr_addr, sizeof(struct sockaddr));
      if (ioctl(s, SIOCSIFADDR, &ifra) < 0)
	perror("SIFADDR");;
      bcopy(&ifra.ifra_broadaddr, &ifrq.ifr_dstaddr, sizeof(struct sockaddr));
      if (ioctl(s, SIOCSIFDSTADDR, &ifrq) < 0)
	perror("SIFDSTADDR");;
#ifdef notdef
      bcopy(&ifra.ifra_mask, &ifrq.ifr_broadaddr, sizeof(struct sockaddr));
      if (ioctl(s, SIOCSIFBRDADDR, &ifrq) < 0)
	perror("SIFBRDADDR");
#endif
    } else if (ioctl(s, SIOCAIFADDR, &ifra) < 0) {
      perror("SIOCAIFADDR");
      close(s);
      return(-1);
    }
    if (Enabled(ConfProxy))
      sifproxyarp(s, hisaddr.s_addr);
  }
  close(s);
  return(0);
}

int
OsSetIpaddress(myaddr, hisaddr, netmask)
struct in_addr myaddr, hisaddr, netmask;
{
  return(SetIpDevice(myaddr, hisaddr, netmask, 1));
}

static struct in_addr peer_addr;
struct in_addr defaddr;

void
OsLinkup()
{
  char *s;

  if (linkup == 0) {
    if (setuid(0) < 0)
	logprintf("setuid failed\n");
    peer_addr = IpcpInfo.his_ipaddr;
    s = (char *)inet_ntoa(peer_addr);
    LogPrintf(LOG_PHASE, "OsLinkup: %s\n", s);

    if (SelectSystem(inet_ntoa(IpcpInfo.want_ipaddr), LINKFILE) < 0) {
      if (dstsystem) {
        if (SelectSystem(dstsystem, LINKFILE) < 0)
          SelectSystem("MYADDR", LINKFILE);
      } else
        SelectSystem("MYADDR", LINKFILE);
    }
    linkup = 1;
  }
}

void
OsLinkdown()
{
  char *s;

  if (linkup) {
    s = (char *)inet_ntoa(peer_addr);
    LogPrintf(LOG_PHASE, "OsLinkdown: %s\n", s);
    if (!(mode & MODE_AUTO))
      DeleteIfRoutes(0);
    linkup = 0;
  }
}

int
OsInterfaceDown(final)
int final;
{
  struct in_addr zeroaddr;
  int s;

  OsLinkdown();
  if (!final && (mode & MODE_AUTO))	/* We still want interface alive */
    return(0);
  s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    perror("socket");
    return(-1);
  }
  ifrq.ifr_flags &= ~IFF_UP;
  if (ioctl(s, SIOCSIFFLAGS, &ifrq) < 0) {
    perror("SIOCSIFFLAGS");
    close(s);
    return(-1);
  }

  zeroaddr.s_addr = 0;
  SetIpDevice(zeroaddr, zeroaddr, zeroaddr, 0);

  close(s);
  return(0);
}

void
OsSetInterfaceParams(type, mtu, speed)
int type, mtu, speed;
{
  struct tuninfo info;

  info.type = type;
  info.mtu = mtu;
  info.baudrate = speed;
  if (ioctl(tun_out, TUNSIFINFO, &info) < 0)
    perror("TUNSIFINFO");
}

/*
 *  Open tunnel device and returns its descriptor
 */
int
OpenTunnel(ptun)
int *ptun;
{
  int s;
  char *cp;
  char *suffix = "0123456789";
  char ifname[IFNAMSIZ];
  char devname[12];

  strcpy(devname, "/dev/tun0");
  for (cp = suffix; *cp; cp++) {
    devname[8] = *cp;
    tun_out = open(devname, O_RDWR);
    if (tun_out >= 0)
      break;
  }
  *ptun = cp - suffix;
  if (*cp == '\0') {
    fprintf(stderr, "No tunnel device is available.\n");
    return(-1);
  }

  /*
   * At first, name the interface.
   */
  strcpy(ifname, devname + 5);

  bzero((char *)&ifra, sizeof(ifra));
  bzero((char *)&ifrq, sizeof(ifrq));

  strncpy(ifrq.ifr_name, ifname, IFNAMSIZ);
  strncpy(ifra.ifra_name, ifname, IFNAMSIZ);

  s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    perror("socket");
    return(-1);
  }

  /*
   *  Now, bring up the interface.
   */
  if (ioctl(s, SIOCGIFFLAGS, &ifrq) < 0) {
    perror("SIOCGIFFLAGS");
    close(s);
    return(-1);
  }

  ifrq.ifr_flags |= IFF_UP;
  if (ioctl(s, SIOCSIFFLAGS, &ifrq) < 0) {
    perror("SIOCSIFFLAGS");
    close(s);
    return(-1);
  }

  tun_in = tun_out;
  IfDevName = devname + 5;
  if (GetIfIndex(IfDevName) < 0) {
    fprintf(stderr, "can't find ifindex.\n");
    close(s);
    return(-1);
  }
  printf("Using interface: %s\r\n", IfDevName);
  LogPrintf(LOG_PHASE, "Using interface: %s\n", IfDevName);
  close(s);
  return(0);
}

void
OsCloseLink(flag)
int flag;
{
  HangupModem(flag);
}

void
OsAddInOctets(cnt)
int cnt;
{
}

void
OsAddOutOctets(cnt)
int cnt;
{
}

