/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#ifdef ZT_ENABLE_NETCON

#include <algorithm>
#include <utility>
#include <dlfcn.h>

#include "NetconEthernetTap.hpp"

#include "../node/Utils.hpp"
#include "../osdep/OSUtils.hpp"
#include "../osdep/Phy.hpp"

#include "lwip/tcp_impl.h"
#include "netif/etharp.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/ip_frag.h"
#include "lwip/tcp.h"

#include "LWIPStack.hpp"
#include "NetconService.hpp"
#include "Intercept.h"
#include "NetconUtilities.hpp"

#define APPLICATION_POLL_FREQ 1

namespace ZeroTier {


NetconEthernetTap::NetconEthernetTap(
	const char *homePath,
	const MAC &mac,
	unsigned int mtu,
	unsigned int metric,
	uint64_t nwid,
	const char *friendlyName,
	void (*handler)(void *,uint64_t,const MAC &,const MAC &,unsigned int,unsigned int,const void *,unsigned int),
	void *arg) :
	_phy(this,false,true),
	_unixListenSocket((PhySocket *)0),
	_handler(handler),
	_arg(arg),
	_nwid(nwid),
	_mac(mac),
	_homePath(homePath),
	_mtu(mtu),
	_enabled(true),
	_run(true)
{
	char sockPath[4096];
	Utils::snprintf(sockPath,sizeof(sockPath),"/tmp/.ztnc_%.16llx",(unsigned long long)nwid);
	_dev = sockPath;

	lwipstack = new LWIPStack("/root/dev/netcon/liblwip.so");
	if(!lwipstack) // TODO double check this check
		throw std::runtime_error("unable to load lwip lib.");
	lwipstack->lwip_init();

	_unixListenSocket = _phy.unixListen(sockPath,(void *)this);
	if (!_unixListenSocket)
		throw std::runtime_error(std::string("unable to bind to ")+sockPath);
	_thread = Thread::start(this);
}

NetconEthernetTap::~NetconEthernetTap()
{
	_run = false;
	_phy.whack();
	_phy.whack();
	Thread::join(_thread);
	_phy.close(_unixListenSocket,false);
}

void NetconEthernetTap::setEnabled(bool en)
{
	_enabled = en;
}

bool NetconEthernetTap::enabled() const
{
	return _enabled;
}

bool NetconEthernetTap::addIp(const InetAddress &ip)
{
	Mutex::Lock _l(_ips_m);
	if (std::find(_ips.begin(),_ips.end(),ip) == _ips.end()) {
		_ips.push_back(ip);
		std::sort(_ips.begin(),_ips.end());

		if (ip.isV4()) {
			Mutex::Lock _l2(_arp_m);
			_arp.addLocal((uint32_t)(reinterpret_cast<const struct sockaddr_in *>(&ip)->sin_addr.s_addr),_mac);
		}

		// Set IP
		static ip_addr_t ipaddr, netmask, gw;
		IP4_ADDR(&gw,0,0,0,0);
		ipaddr.addr = *((u32_t *)_ips[0].rawIpData());
		netmask.addr = *((u32_t *)_ips[0].netmask().rawIpData());

		// Set up the lwip-netif for LWIP's sake
		fprintf(stderr, "initializing interface\n");
		lwipstack->netif_add(&interface,&ipaddr, &netmask, &gw, NULL, tapif_init, lwipstack->ethernet_input);
		interface.state = this;
		interface.output = lwipstack->etharp_output;
		_mac.copyTo(interface.hwaddr, 6);
		interface.mtu = _mtu;
		interface.name[0] = 't';
		interface.name[1] = 'p';
		interface.linkoutput = low_level_output;
		interface.hwaddr_len = 6;
		interface.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP;
		lwipstack->netif_set_default(&interface);
		lwipstack->netif_set_up(&interface);
	}
	return true;
}

bool NetconEthernetTap::removeIp(const InetAddress &ip)
{
	Mutex::Lock _l(_ips_m);
	std::vector<InetAddress>::iterator i(std::find(_ips.begin(),_ips.end(),ip));
	if (i == _ips.end())
		return false;

	_ips.erase(i);

	if (ip.isV4()) {
		Mutex::Lock _l2(_arp_m);
		_arp.remove((uint32_t)(reinterpret_cast<const struct sockaddr_in *>(&ip)->sin_addr.s_addr));
	}

	// TODO: dealloc IP from LWIP

	return true;
}

std::vector<InetAddress> NetconEthernetTap::ips() const
{
	Mutex::Lock _l(_ips_m);
	return _ips;
}

void NetconEthernetTap::put(const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
{
	if (!_enabled)
		return;

	struct pbuf *p, *q;
  const char *bufptr;
	struct eth_hdr *ethhdr = NULL;

	// We allocate a pbuf chain of pbufs from the pool.
	p = lwipstack->pbuf_alloc(PBUF_RAW, len+sizeof(struct eth_hdr), PBUF_POOL);

	if(p != NULL) {
		/* We iterate over the pbuf chain until we have read the entire
			 packet into the pbuf. */
		bufptr = (const char *)data;
		for(q = p; q != NULL; q = q->next) {
			/* Read enough bytes to fill this pbuf in the chain. The
				 available data in the pbuf is given by the q->len
				 variable. */
			/* read data into(q->payload, q->len); */
			char *pload = (char*)q->payload;
			int plen = q->len;
			if (!ethhdr) {
				ethhdr = (struct eth_hdr *)p->payload;
				pload += sizeof(struct eth_hdr);
				plen -= sizeof(struct eth_hdr);
			}
			memcpy(pload, bufptr, plen);
			bufptr += plen;
		}
		/* acknowledge that packet has been read(); */
	} else {
		return;
		/* drop packet(); */
	}
	from.copyTo(ethhdr->src.addr, 6);
	_mac.copyTo(ethhdr->dest.addr, 6);
	ethhdr->type = Utils::hton((uint16_t)etherType);

	if(interface.input(p, &interface) != ERR_OK) {
		fprintf(stderr, "Error while RXing packet (netif->input)\n");
	}
}

std::string NetconEthernetTap::deviceName() const
{
	return _dev;
}

void NetconEthernetTap::setFriendlyName(const char *friendlyName)
{
}

void NetconEthernetTap::scanMulticastGroups(std::vector<MulticastGroup> &added,std::vector<MulticastGroup> &removed)
{
	fprintf(stderr, "scanMulticastGroups\n");
	std::vector<MulticastGroup> newGroups;
	Mutex::Lock _l(_multicastGroups_m);

	// TODO: get multicast subscriptions from LWIP

	std::vector<InetAddress> allIps(ips());
	for(std::vector<InetAddress>::iterator ip(allIps.begin());ip!=allIps.end();++ip)
		newGroups.push_back(MulticastGroup::deriveMulticastGroupForAddressResolution(*ip));

	std::sort(newGroups.begin(),newGroups.end());
	std::unique(newGroups.begin(),newGroups.end());

	for(std::vector<MulticastGroup>::iterator m(newGroups.begin());m!=newGroups.end();++m) {
		if (!std::binary_search(_multicastGroups.begin(),_multicastGroups.end(),*m))
			added.push_back(*m);
	}
	for(std::vector<MulticastGroup>::iterator m(_multicastGroups.begin());m!=_multicastGroups.end();++m) {
		if (!std::binary_search(newGroups.begin(),newGroups.end(),*m))
			removed.push_back(*m);
	}

	_multicastGroups.swap(newGroups);
}

NetconConnection *NetconEthernetTap::getConnectionByPCB(struct tcp_pcb *pcb)
{
	NetconConnection *c;
	for(size_t i=0; i<clients.size(); i++) {
		c = clients[i]->containsPCB(pcb);
		if(c) {
			return c;
		}
	}
	return NULL;
}

NetconConnection *NetconEthernetTap::getConnectionByThisFD(int fd)
{
	for(size_t i=0; i<clients.size(); i++) {
		for(size_t j=0; j<clients[i]->connections.size(); j++) {
			if(_phy.getDescriptor(clients[i]->connections[j]->sock) == fd) {
				return clients[i]->connections[j];
			}
		}
	}
	return NULL;
}

NetconClient *NetconEthernetTap::getClientByPCB(struct tcp_pcb *pcb)
{
	for(size_t i=0; i<clients.size(); i++) {
		if(clients[i]->containsPCB(pcb)) {
			return clients[i];
		}
	}
	return NULL;
}

void NetconEthernetTap::closeClient(NetconClient *client)
{
	//fprintf(stderr, "closeClient\n");
	NetconConnection *temp_conn;
	closeConnection(client->rpc);
	for(size_t i=0; i<client->connections.size(); i++) {
		temp_conn = client->connections[i];
		closeConnection(client->connections[i]);
		delete temp_conn;
	}
	delete client;
}

void NetconEthernetTap::closeAllClients()
{
	for(int i=0; i<clients.size(); i++){
		closeClient(clients[i]);
	}
}

void NetconEthernetTap::closeConnection(NetconConnection *conn)
{
	//fprintf(stderr, "closeConnection\n");
	NetconClient *client = conn->owner;
	_phy.close(conn->sock);
	lwipstack->tcp_close(conn->pcb);
	client->removeConnection(conn->sock);
}


/*------------------------------------------------------------------------------
------------------------ low-level Interface functions -------------------------
------------------------------------------------------------------------------*/


void NetconEthernetTap::threadMain()
	throw()
{
	unsigned long tcp_time = ARP_TMR_INTERVAL / 5000;
  unsigned long etharp_time = IP_TMR_INTERVAL / 1000;
  unsigned long prev_tcp_time = 0;
  unsigned long prev_etharp_time = 0;
  unsigned long curr_time;
  unsigned long since_tcp;
  unsigned long since_etharp;
	struct timeval tv;

	// Main timer loop
	while (_run) {
		gettimeofday(&tv, NULL);
	  curr_time = (unsigned long)(tv.tv_sec) * 1000 + (unsigned long)(tv.tv_usec) / 1000;
	  since_tcp = curr_time - prev_tcp_time;
	  since_etharp = curr_time - prev_etharp_time;
	  int min_time = min(since_tcp, since_etharp) * 1000; // usec

	  if(since_tcp > tcp_time)
	  {
	    prev_tcp_time = curr_time+1;
	    lwipstack->tcp_tmr();
	  }
		if(since_etharp > etharp_time)
		{
			prev_etharp_time = curr_time;
			lwipstack->etharp_tmr();
		}
		_phy.poll(min_time / 1000); // conversion from usec to millisec, TODO: double check
	}
	closeAllClients();
	// TODO: cleanup -- destroy LWIP state, kill any clients, unload .so, etc.
}

void NetconEthernetTap::phyOnSocketPairEndpointClose(PhySocket *sock, void **uptr)
{
	fprintf(stderr, "phyOnSocketPairEndpointClose\n");
	_phy.setNotifyWritable(sock, false);
	NetconClient *client = (NetconClient*)*uptr;
	closeConnection(client->getConnection(sock));
}

void NetconEthernetTap::phyOnSocketPairEndpointData(PhySocket *sock, void **uptr, void *buf, unsigned long n)
{
	fprintf(stderr, "phyOnSocketPairEndpointData\n");
	int r;
	NetconConnection *c = ((NetconClient*)*uptr)->getConnection(sock);
	if(c) {
		if(c->idx < DEFAULT_READ_BUFFER_SIZE) {
			if((r = read(_phy.getDescriptor(c->sock), (&c->buf)+c->idx, DEFAULT_READ_BUFFER_SIZE-(c->idx))) > 0) {
				c->idx += r;
				handle_write(c);
			}
		}
	}
}

void NetconEthernetTap::phyOnSocketPairEndpointWritable(PhySocket *sock, void **uptr)
{
	_phy.setNotifyWritable(sock, false);
}

// Unused -- no UDP or TCP from this thread/Phy<>
void NetconEthernetTap::phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *from,void *data,unsigned long len) {}

void NetconEthernetTap::phyOnTcpConnect(PhySocket *sock,void **uptr,bool success) {}
void NetconEthernetTap::phyOnTcpAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN,const struct sockaddr *from) {}
void NetconEthernetTap::phyOnTcpClose(PhySocket *sock,void **uptr) {}
void NetconEthernetTap::phyOnTcpData(PhySocket *sock,void **uptr,void *data,unsigned long len) {}
void NetconEthernetTap::phyOnTcpWritable(PhySocket *sock,void **uptr) {}

void NetconEthernetTap::phyOnUnixAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN)
{
	fprintf(stderr, "phyOnUnixAccept\n");
	NetconClient *newClient = new NetconClient();
	newClient->rpc = newClient->addConnection(RPC, sockN);
	*uptrN = newClient;
}

void NetconEthernetTap::phyOnUnixClose(PhySocket *sock,void **uptr)
{
	_phy.setNotifyWritable(sock, false);
	//fprintf(stderr, "phyOnUnixClose\n");
	closeClient(((NetconClient*)*uptr));
}

void NetconEthernetTap::phyOnUnixData(PhySocket *sock,void **uptr,void *data,unsigned long len)
{
	unsigned char *buf = (unsigned char*)data;
	NetconClient *client = (NetconClient*)*uptr;
	if(!client)
		fprintf(stderr, "!client\n");

	switch(buf[0])
	{
		case RPC_SOCKET:
			fprintf(stderr, "RPC_SOCKET\n");
	    struct socket_st socket_rpc;
	    memcpy(&socket_rpc, &buf[1], sizeof(struct socket_st));
	    client->tid = socket_rpc.__tid;
	    handle_socket(client, &socket_rpc);
			break;
	  case RPC_LISTEN:
			fprintf(stderr, "RPC_LISTEN\n");
	    struct listen_st listen_rpc;
	    memcpy(&listen_rpc, &buf[1], sizeof(struct listen_st));
	    client->tid = listen_rpc.__tid;
	    handle_listen(client, &listen_rpc);
			break;
	  case RPC_BIND:
			fprintf(stderr, "RPC_BIND\n");
	    struct bind_st bind_rpc;
	    memcpy(&bind_rpc, &buf[1], sizeof(struct bind_st));
	    client->tid = bind_rpc.__tid;
	    handle_bind(client, &bind_rpc);
			break;
	  case RPC_KILL_INTERCEPT:
			fprintf(stderr, "RPC_KILL_INTERCEPT\n");
	    closeClient(client);
			break;
  	case RPC_CONNECT:
			fprintf(stderr, "RPC_CONNECT\n");
	    struct connect_st connect_rpc;
	    memcpy(&connect_rpc, &buf[1], sizeof(struct connect_st));
	    client->tid = connect_rpc.__tid;
	    handle_connect(client, &connect_rpc);
			break;
	  case RPC_FD_MAP_COMPLETION:
			fprintf(stderr, "RPC_FD_MAP_COMPLETION\n");
	    handle_retval(client, buf);
			break;
		default:
			break;
	}
}

void NetconEthernetTap::phyOnUnixWritable(PhySocket *sock,void **uptr)
{
}

int NetconEthernetTap::send_return_value(NetconClient *client, int retval)
{
	fprintf(stderr, "send_return_value\n");
  if(!client->waiting_for_retval){
    fprintf(stderr, "intercept isn't waiting for return value. Why are we here?\n");
    return 0;
  }
  char retmsg[4];
  memset(&retmsg, '\0', sizeof(retmsg));
  retmsg[0]=RPC_RETVAL;
  memcpy(&retmsg[1], &retval, sizeof(retval));
  int n = write(_phy.getDescriptor(client->rpc->sock), &retmsg, sizeof(retmsg));

  if(n > 0) {
		// signal that we've satisfied this requirement
    client->waiting_for_retval = false;
  }
  else {
    fprintf(stderr, "unable to send return value to the intercept\n");
		closeClient(client);
  }
  return n;
}

/*------------------------------------------------------------------------------
--------------------------------- LWIP callbacks -------------------------------
------------------------------------------------------------------------------*/

err_t NetconEthernetTap::nc_poll(void* arg, struct tcp_pcb *tpcb)
{
	fprintf(stderr, "nc_poll\n");
	Larg *l = (Larg*)arg;
	NetconConnection *c = l->tap->getConnectionByPCB(tpcb);
	NetconEthernetTap *tap = l->tap;
	if(c)
		tap->handle_write(c);
	return ERR_OK;
}

err_t NetconEthernetTap::nc_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	fprintf(stderr, "nc_accept\n");

	Larg *l = (Larg*)arg;
	NetconEthernetTap *tap = l->tap;
	NetconConnection *c = tap->getConnectionByPCB(newpcb);
	NetconClient *client = c->owner;

  if(c && client) {
		int their_fd;
		NetconConnection *new_conn = client->addConnection(BUFFER, tap->_phy.createSocketPair(their_fd, client));
		new_conn->their_fd = their_fd;
		new_conn->pcb = newpcb;
		PhySocket *sock = client->rpc->sock;
		int send_fd = tap->_phy.getDescriptor(sock);

    int n = write(tap->_phy.getDescriptor(new_conn->sock), "z", 1);
    if(n > 0) {
			sock_fd_write(send_fd, their_fd);
			client->unmapped_conn = new_conn;
    }
    else {
      //dwr(c->owner->tid, "nc_accept() - unknown error writing signal byte to listening socket\n");
      return -1;
    }
    tap->lwipstack->tcp_arg(newpcb, (void*)(intptr_t)(tap->_phy.getDescriptor(new_conn->sock)));
    tap->lwipstack->tcp_recv(newpcb, nc_recved);
    tap->lwipstack->tcp_err(newpcb, nc_err);
    tap->lwipstack->tcp_sent(newpcb, nc_sent);
    tap->lwipstack->tcp_poll(newpcb, nc_poll, 1);
    tcp_accepted(c->pcb);

		return ERR_OK;
  }
  else {
    //dwr("can't locate Connection object for PCB\n");
  }
  return -1;


	return ERR_OK;
}

err_t NetconEthernetTap::nc_recved(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
	fprintf(stderr, "nc_recved\n");
	Larg *l = (Larg*)arg;
	NetconConnection *c = l->tap->getConnectionByPCB(tpcb);
	NetconEthernetTap *tap = l->tap;

	int n;
  struct pbuf* q = p;
	int our_fd = tap->_phy.getDescriptor(c->sock);

  if(!c) {
    return ERR_OK; // ?
  }
  if(p == NULL) {
    if(c) {
      nc_close(tpcb);
      close(our_fd); // TODO: Check logic
			tap->closeConnection(c);
    }
    else {
      fprintf(stderr, "can't locate connection via (arg)\n");
    }
    return err;
  }
  q = p;
  while(p != NULL) { // Cycle through pbufs and write them to the socket
    if(p->len <= 0)
      break; // ?
    if((n = write(our_fd, p->payload, p->len)) > 0) {
      if(n < p->len) {
        fprintf(stderr, "ERROR: unable to write entire pbuf to buffer\n");
				//tap->_phy.setNotifyWritable(l->sock, true);
      }
      tap->lwipstack->tcp_recved(tpcb, n);
    }
    else {
      fprintf(stderr, "Error: No data written to intercept buffer\n");
    }
    p = p->next;
  }
  tap->lwipstack->pbuf_free(q); // free pbufs
  return ERR_OK;
}

void NetconEthernetTap::nc_err(void *arg, err_t err)
{
	fprintf(stderr, "nc_err\n");
	Larg *l = (Larg*)arg;
	NetconEthernetTap *tap = l->tap;
	NetconConnection *c = tap->getConnectionByThisFD(tap->_phy.getDescriptor(l->sock));
  if(c) {
    tap->closeConnection(c);
  }
  else {
    fprintf(stderr, "can't locate connection object for PCB\n");
  }
}

void NetconEthernetTap::nc_close(struct tcp_pcb* tpcb)
{
	fprintf(stderr, "nc_close\n");
	//closeConnection(getConnectionByPCB(tpcb));
	/*
  lwipstack->tcp_arg(tpcb, NULL);
  lwipstack->tcp_sent(tpcb, NULL);
  lwipstack->tcp_recv(tpcb, NULL);
  lwipstack->tcp_err(tpcb, NULL);
  lwipstack->tcp_poll(tpcb, NULL, 0);
  lwipstack->tcp_close(tpcb);
	*/
}

err_t NetconEthernetTap::nc_send(struct tcp_pcb *tpcb)
{
	fprintf(stderr, "nc_send\n");
	return ERR_OK;
}

err_t NetconEthernetTap::nc_sent(void* arg, struct tcp_pcb *tpcb, u16_t len)
{
	fprintf(stderr, "nc_sent\n");
	return len;
}

err_t NetconEthernetTap::nc_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
	fprintf(stderr, "nc_connected\n");
	Larg *l = (Larg*)arg;
	NetconEthernetTap *tap = l->tap;
	for(size_t i=0; i<tap->clients.size(); i++) {
		if(tap->clients[i]->containsPCB(tpcb)) {
			tap->send_return_value(tap->clients[i],err);
		}
	}
	return err;
}



/*------------------------------------------------------------------------------
----------------------------- RPC Handler functions ----------------------------
------------------------------------------------------------------------------*/

void NetconEthernetTap::handle_bind(NetconClient *client, struct bind_st *bind_rpc)
{
	// FIXME: Is this hack still needed?
  struct sockaddr_in *connaddr;
  connaddr = (struct sockaddr_in *) &bind_rpc->addr;
  int conn_port = lwipstack->ntohs(connaddr->sin_port);
  ip_addr_t conn_addr;
  //IP4_ADDR(&conn_addr, 192,168,0,2);
	conn_addr.addr = *((u32_t *)_ips[0].rawIpData());


	/*
  int ip = connaddr->sin_addr.s_addr;
  unsigned char bytes[4];
  bytes[0] = ip & 0xFF;
  bytes[1] = (ip >> 8) & 0xFF;
  bytes[2] = (ip >> 16) & 0xFF;
  bytes[3] = (ip >> 24) & 0xFF;
  "binding to: %d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]
  */

	fprintf(stderr, "PORT = %d\n", conn_port);
	NetconConnection *c = client->getConnectionByTheirFD(bind_rpc->sockfd);
  if(c) {
    if(c->pcb->state == CLOSED){
      int err = lwipstack->tcp_bind(c->pcb, &conn_addr, conn_port);
      if(err != ERR_OK) {
        fprintf(stderr, "error while binding to addr/port\n");
      }
      else {
        fprintf(stderr, "bind successful\n");
      }
    }
    else {
      fprintf(stderr, "PCB not in CLOSED state. Ignoring BIND request.\n");
    }
  }
  else {
    fprintf(stderr, "can't locate connection for PCB\n");
  }
}

void NetconEthernetTap::handle_listen(NetconClient *client, struct listen_st *listen_rpc)
{
	NetconConnection *c = client->getConnectionByTheirFD(listen_rpc->sockfd);
  if(c) {
    if(c->pcb->state == LISTEN) {
      fprintf(stderr, "PCB is already in listening state.\n");
      return;
    }
    struct tcp_pcb* listening_pcb = lwipstack->tcp_listen(c->pcb);
    if(listening_pcb != NULL) {
      c->pcb = listening_pcb;
      lwipstack->tcp_accept(listening_pcb, nc_accept);
			lwipstack->tcp_arg(listening_pcb, new Larg(this, c->sock));
      client->waiting_for_retval=true;
    }
    else {
			fprintf(stderr, "unable to allocate memory for new listening PCB\n");
    }
  }
  else {
    fprintf(stderr, "can't locate connection for PCB\n");
  }
}

void NetconEthernetTap::handle_retval(NetconClient *client, unsigned char* buf)
{
	if(client->unmapped_conn != NULL) {
		memcpy(&(client->unmapped_conn->their_fd), &buf[1], sizeof(int));
		client->connections.push_back(client->unmapped_conn);
		client->unmapped_conn = NULL;
	}
}

void NetconEthernetTap::handle_socket(NetconClient *client, struct socket_st* socket_rpc)
{
	struct tcp_pcb *pcb = lwipstack->tcp_new();
  if(pcb != NULL) {
		int their_fd;
		NetconConnection *new_conn = client->addConnection(BUFFER, _phy.createSocketPair(their_fd, client));
		new_conn->their_fd = their_fd;
		new_conn->pcb = pcb;
		PhySocket *sock = client->rpc->sock;
		int send_fd = _phy.getDescriptor(sock);
    sock_fd_write(send_fd, their_fd);
    client->unmapped_conn = new_conn;
  }
  else {
    fprintf(stderr, "Memory not available for new PCB\n");
  }
}

void NetconEthernetTap::handle_connect(NetconClient *client, struct connect_st* connect_rpc)
{
	// FIXME: Parse out address information -- Probably a more elegant way to do this
	struct sockaddr_in *connaddr;
	connaddr = (struct sockaddr_in *) &connect_rpc->__addr;
	int conn_port = lwipstack->ntohs(connaddr->sin_port);
	ip_addr_t conn_addr = convert_ip((struct sockaddr_in *)&connect_rpc->__addr);

	fprintf(stderr, "getConnectionByTheirFD(%d)\n", connect_rpc->__fd);
	NetconConnection *c = client->getConnectionByTheirFD(connect_rpc->__fd);

	if(c!= NULL) {
		lwipstack->tcp_sent(c->pcb, nc_sent); // FIXME: Move?
		lwipstack->tcp_recv(c->pcb, nc_recved);
		lwipstack->tcp_err(c->pcb, nc_err);
		lwipstack->tcp_poll(c->pcb, nc_poll, APPLICATION_POLL_FREQ);
		lwipstack->tcp_arg(c->pcb, new Larg(this, c->sock));

		int err = 0;
		if((err = lwipstack->tcp_connect(c->pcb,&conn_addr,conn_port, nc_connected)) < 0)
		{
			// dwr(h->tid, "tcp_connect() = %s\n", lwiperror(err));
			// We should only return a value if failure happens immediately
			// Otherwise, we still need to wait for a callback from lwIP.
			// - This is because an ERR_OK from tcp_connect() only verifies
			//   that the SYN packet was enqueued onto the stack properly,
			//   that's it!
			// - Most instances of a retval for a connect() should happen
			//   in the nc_connect() and nc_err() callbacks!
			//fprintf(stderr, "failed to connect: %s\n", lwiperror(err));
			send_return_value(client, err);
		}
		// Everything seems to be ok, but we don't have enough info to retval
		client->waiting_for_retval=true;
	}
	else {
		fprintf(stderr, "could not locate PCB based on their fd\n");
	}
}

void NetconEthernetTap::handle_write(NetconConnection *c)
{
	fprintf(stderr, "handle_write");
	if(c) {
		int sndbuf = c->pcb->snd_buf;
		float avail = (float)sndbuf;
		float max = (float)TCP_SND_BUF;
		float load = 1.0 - (avail / max);

		if(load >= 0.9) {
			return;
		}
		int write_allowance =  sndbuf < c->idx ? sndbuf : c->idx;
		int sz;

		if(write_allowance > 0) {
			int err = lwipstack->tcp_write(c->pcb, &c->buf, write_allowance, TCP_WRITE_FLAG_COPY);
			if(err != ERR_OK) {
				fprintf(stderr, "error while writing to PCB\n");
				return;
			}
			else {
				sz = (c->idx)-write_allowance;
				if(sz) {
					memmove(&c->buf, (c->buf+write_allowance), sz);
				}
				c->idx -= write_allowance;
				//c->data_sent += write_allowance;
				return;
			}
		}
		else {
			fprintf(stderr, "lwIP stack full\n");
			return;
		}
	}
	else {
		fprintf(stderr, "could not locate connection for this fd\n");
	}
}

} // namespace ZeroTier

#endif // ZT_ENABLE_NETCON