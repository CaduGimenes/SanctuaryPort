/* LAN networking (opt-in, SANCTUARY_LAN=1): the game's own "LAN Play" network mode.
 *
 * Without the switch nothing here resolves and runtime_services.c keeps answering "network cable
 * unplugged". With it, the console has a wired connection to the local network:
 *   - NetCtl is connected (IP obtained) and reports this machine's LAN address;
 *   - libSceNet sockets are host sockets (BSD layout and FreeBSD numbers translated);
 *   - libSceNet epoll is host epoll (Linux) or WSAPoll over the registered sockets (Windows);
 *   - hostname lookups and HTTP still fail, so nothing reaches Sony or Blizzard servers;
 *   - only local destinations are reachable (loopback, 10/8, 172.16/12, 192.168/16, 169.254/16,
 *     100.64/10 for VPNs such as Tailscale, and broadcast): any other address is refused, so even
 *     a hard-coded internet address cannot be reached;
 *   - UPnP (SSDP, port 1900) is refused: LAN play needs no port forward on the router;
 *   - PSN stays signed out (LAN Play does not need it).
 * Diablo III uses UDP 3074 (game) and UDP 1200 (LAN discovery broadcasts).
 * SANCTUARY_LAN_IP=a.b.c.d picks the address to report (default: the machine's LAN IPv4).
 * Testing on one machine: SANCTUARY_LAN_IP=127.0.0.2, 127.0.0.3, ... gives each instance its own
 * loopback address; sockets bind to it and broadcasts go to 127.0.0.2..127.0.0.9.
 * Every call is logged with "Net:" while this is being brought up. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif
typedef SOCKET HostSocket;
#define host_close closesocket
#define MSG_NOSIGNAL 0
static int host_error(void) { return WSAGetLastError(); }
static const char *host_error_text(int e) { static __thread char t[16]; snprintf(t,sizeof(t),"WSA %d",e); return t; }
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
typedef int HostSocket;
#define host_close close
static int host_error(void) { return errno; }
static const char *host_error_text(int e) { return strerror(e); }
#endif

#define NET_ERROR(e) ((int32_t)(UINT32_C(0x80410100)|(uint32_t)(e)))
/* FreeBSD errno numbers */
#define G_EBADF 9
#define G_EINVAL 22
#define G_EMFILE 24
#define G_EAGAIN 35
#define G_EINPROGRESS 36
#define G_ENOPROTOOPT 42
#define G_EAFNOSUPPORT 47
#define G_ENETUNREACH 51
#define G_ECONNABORTED 53
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)

static __thread int32_t net_errno;
static int lan_enabled(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SANCTUARY_LAN"); v=e && e[0]=='1'; if (v) puts("Net: LAN mode on (SANCTUARY_LAN=1)"); }
    return v;
}

static int32_t guest_net_errno(int e) {
#ifdef _WIN32
    if (e==WSAEWOULDBLOCK) return G_EAGAIN;
    if (e>10000 && e<10100) return e-10000; /* WSAExxx = 10000 + the BSD number */
    return 5;
#else
    switch (e) {
    case EWOULDBLOCK: return G_EAGAIN;
    case EINPROGRESS: return G_EINPROGRESS; case EALREADY: return 37; case ENOTSOCK: return 38;
    case EDESTADDRREQ: return 39; case EMSGSIZE: return 40; case EPROTOTYPE: return 41;
    case ENOPROTOOPT: return 42; case EPROTONOSUPPORT: return 43; case EOPNOTSUPP: return 45;
    case EAFNOSUPPORT: return 47; case EADDRINUSE: return 48; case EADDRNOTAVAIL: return 49;
    case ENETDOWN: return 50; case ENETUNREACH: return 51; case ECONNABORTED: return 53;
    case ECONNRESET: return 54; case ENOBUFS: return 55; case EISCONN: return 56;
    case ENOTCONN: return 57; case ETIMEDOUT: return 60; case ECONNREFUSED: return 61;
    case EHOSTDOWN: return 64; case EHOSTUNREACH: return 65;
    default: return runtime_guest_errno(e);
    }
#endif
}
static int32_t fail(int host_errno) { net_errno=guest_net_errno(host_errno); return NET_ERROR(net_errno); }
static int32_t fail_guest(int guest_errno) { net_errno=guest_errno; return NET_ERROR(guest_errno); }
static struct in_addr lan_ip, lan_mask;
/* Why an address is not local, or NULL: loopback, private LAN ranges, link-local, the VPN range
 * (Tailscale and similar), and broadcast. */
static const char *not_local(struct in_addr address) {
    uint32_t ip=ntohl(address.s_addr);
    if ((ip>>24)==127 || (ip>>24)==10 || (ip>>20)==0xac1 || (ip>>16)==0xc0a8 || (ip>>16)==0xa9fe ||
        (ip>>22)==(0x64400000u>>22) || ip==0xffffffffu) return NULL;
    uint32_t mask=ntohl(lan_mask.s_addr), own=ntohl(lan_ip.s_addr);
    if (mask && (ip&mask)==(own&mask) && (ip|mask)==0xffffffffu) return NULL; /* LAN broadcast */
    return (ip>>28)==0xe ? "multicast" : "not on the local network";
}
/* Why a destination is refused, or NULL: only the local network is reachable. */
static const char *refused(const struct sockaddr_in *a) {
    /* UPnP/SSDP (DemonWare's router port mapping): never reaches the router. */
    if (ntohs(a->sin_port)==1900) return "UPnP";
    return not_local(a->sin_addr);
}
/* The first contact with each destination is always logged (the traffic log stops at 400). */
static void note_destination(const struct sockaddr_in *a, const char *what) {
    static uint64_t seen[256]; static unsigned count; static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    uint64_t key=((uint64_t)a->sin_addr.s_addr<<16)|a->sin_port;
    pthread_mutex_lock(&lock);
    unsigned i=0;
    while (i<count && seen[i]!=key) ++i;
    int fresh=i==count;
    if (fresh && count<256) seen[count++]=key;
    pthread_mutex_unlock(&lock);
    if (fresh) {
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&a->sin_addr,ip,sizeof(ip));
        printf("Net: new destination %s:%u (%s)\n",ip,ntohs(a->sin_port),what);
    }
}

/* ---- Non-blocking state: Winsock cannot read it back, so it is tracked for every socket.
 * Winsock handles are large multiples of 4 (Wine: ~360000); the slot is a hash of the value. ---- */
#define SOCKET_SLOTS 65536
static uint8_t nonblocking[SOCKET_SLOTS];
static size_t slot(HostSocket s) {
#ifdef _WIN32
    return (size_t)((uint64_t)s>>2)%SOCKET_SLOTS;
#else
    return (size_t)(uint64_t)s%SOCKET_SLOTS;
#endif
}
static void set_nonblocking(HostSocket s, int on) {
#ifdef _WIN32
    u_long v=on ? 1 : 0; ioctlsocket(s,FIONBIO,&v);
#else
    int f=fcntl(s,F_GETFL); fcntl(s,F_SETFL,on ? f|O_NONBLOCK : f&~O_NONBLOCK);
#endif
    nonblocking[slot(s)]=(uint8_t)(on!=0);
}
static int is_nonblocking(HostSocket s) { return nonblocking[slot(s)]; }

/* ---- Addresses: FreeBSD sockaddr_in {len, family, port, addr, vport, zero[6]} ---- */
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; } GuestSockaddrIn;
static int to_host_addr(const void *guest, uint32_t size, struct sockaddr_in *host) {
    if (!guest || size<8) return -1;
    const GuestSockaddrIn *g=guest;
    if (g->family!=2) return -1;
    memset(host,0,sizeof(*host));
    host->sin_family=AF_INET; host->sin_port=g->port; host->sin_addr.s_addr=g->addr;
    return 0;
}
static void to_guest_addr(const struct sockaddr_in *host, void *guest, uint32_t *size) {
    if (!guest || !size) return;
    GuestSockaddrIn g={sizeof(GuestSockaddrIn),2,host->sin_port,host->sin_addr.s_addr,0,{0}};
    uint32_t n=*size<sizeof(g) ? *size : (uint32_t)sizeof(g);
    memcpy(guest,&g,n);
    *size=sizeof(g);
}
static const char *addr_text(const struct sockaddr_in *a, char *out, size_t size) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&a->sin_addr,ip,sizeof(ip));
    snprintf(out,size,"%s:%u",ip,ntohs(a->sin_port));
    return out;
}

/* ---- The LAN address reported to the game ---- */
static uint8_t lan_mac[6];
static pthread_once_t lan_once=PTHREAD_ONCE_INIT;
/* The address the host would use for the local network: a UDP "connect" sends nothing. */
static struct in_addr routed_address(void) {
    struct in_addr none={0};
    HostSocket s=socket(AF_INET,SOCK_DGRAM,0);
#ifdef _WIN32
    if (s==INVALID_SOCKET) return none;
#else
    if (s<0) return none;
#endif
    struct sockaddr_in to={0}; to.sin_family=AF_INET; to.sin_port=htons(9);
    inet_pton(AF_INET,"192.168.0.1",&to.sin_addr);
    struct sockaddr_in local={0}; socklen_t n=sizeof(local);
    if (connect(s,(struct sockaddr *)&to,sizeof(to))<0 || getsockname(s,(struct sockaddr *)&local,&n)<0) local.sin_addr=none;
    host_close(s);
    return local.sin_addr;
}
static void lan_address_init(void) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa);
#endif
    const char *want=getenv("SANCTUARY_LAN_IP");
    if (want) inet_pton(AF_INET,want,&lan_ip);
#ifndef _WIN32
    struct ifaddrs *list=NULL;
    if (getifaddrs(&list)==0) {
        struct in_addr routed=want ? lan_ip : routed_address();
        for (struct ifaddrs *i=list;i;i=i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family!=AF_INET || (i->ifa_flags&IFF_LOOPBACK)) continue;
            struct in_addr a=((struct sockaddr_in *)i->ifa_addr)->sin_addr;
            if (routed.s_addr && a.s_addr!=routed.s_addr) continue;
            lan_ip=a;
            if (i->ifa_netmask) lan_mask=((struct sockaddr_in *)i->ifa_netmask)->sin_addr;
            break;
        }
        freeifaddrs(list);
    }
#endif
    if (!lan_ip.s_addr) lan_ip=routed_address();
    if (!lan_ip.s_addr) inet_pton(AF_INET,"127.0.0.1",&lan_ip);
    if (!lan_mask.s_addr) lan_mask.s_addr=(ntohl(lan_ip.s_addr)>>24)==127 ? htonl(0xff000000) : htonl(0xffffff00);
    /* A MAC unique per address and process: two instances on one machine must differ. */
#ifdef _WIN32
    uint32_t pid=(uint32_t)GetCurrentProcessId();
#else
    uint32_t pid=(uint32_t)getpid();
#endif
    uint32_t ip=ntohl(lan_ip.s_addr);
    lan_mac[0]=0x02; lan_mac[1]=(uint8_t)(pid>>8); lan_mac[2]=(uint8_t)pid;
    lan_mac[3]=(uint8_t)(ip>>16); lan_mac[4]=(uint8_t)(ip>>8); lan_mac[5]=(uint8_t)ip;
    char text[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&lan_ip,text,sizeof(text)); inet_ntop(AF_INET,&lan_mask,mask,sizeof(mask));
    printf("Net: LAN address %s netmask %s\n",text,mask);
}
static void lan_address(void) { pthread_once(&lan_once,lan_address_init); }

/* ---- NetCtl: connected, IP obtained ---- */
static ABI int32_t netctl_state(int32_t *state) {
    if (!state) return NET_CTL_INVALID_ADDR;
    *state=3; /* IPOBTAINED */
    return 0;
}
static ABI int32_t netctl_info(int32_t code, void *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    lan_address();
    char *text=info; uint32_t *value=info;
    switch (code) {
    case 1: *value=0; break;                                   /* device: wired */
    case 2: memcpy(info,lan_mac,6); break;                     /* ether address */
    case 3: *value=1500; break;                                /* MTU */
    case 4: *value=1; break;                                   /* link: connected */
    case 11: *value=0; break;                                  /* IP config: DHCP */
    case 12: case 13: text[0]=0; break;                        /* DHCP host name, PPPoE name */
    case 14: inet_ntop(AF_INET,&lan_ip,text,16); break;        /* IP address */
    case 15: inet_ntop(AF_INET,&lan_mask,text,16); break;      /* netmask */
    case 16: case 17: case 18: inet_ntop(AF_INET,&lan_ip,text,16); break; /* route, DNS: ourselves */
    case 19: *value=0; break;                                  /* no HTTP proxy */
    default: printf("Net: NetCtlGetInfo code %d not answered\n",code); return NET_CTL_NOT_CONNECTED;
    }
    return 0;
}
static ABI int32_t netctl_result(int32_t event, int32_t *error) { (void)event; if (error) *error=0; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    lan_address();
    info[1]=1; info[2]=2; info[3]=lan_ip.s_addr; /* STUN ok, NAT type 2, mapped = LAN address */
    return 0;
}
static ABI int32_t net_mac(uint8_t *mac, int32_t flags) {
    (void)flags;
    if (!mac) return NET_ERROR(G_EINVAL);
    lan_address();
    memcpy(mac,lan_mac,6);
    return 0;
}

/* ---- Sockets ---- */
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_socket(const char *name, int32_t family, int32_t type, int32_t protocol) {
    lan_address();
    if (!name) name="";
    if (family!=2) { printf("Net: socket '%s' family %d refused\n",name,family); return fail_guest(G_EAFNOSUPPORT); }
    int host_type;
    switch (type) {
    case 1: host_type=SOCK_STREAM; break;
    case 2: host_type=SOCK_DGRAM; break;
    case 3: host_type=SOCK_RAW; break;
    case 6: host_type=SOCK_DGRAM; printf("Net: socket '%s' is DGRAM_P2P (mapped to UDP)\n",name); break;
    case 10: host_type=SOCK_STREAM; printf("Net: socket '%s' is STREAM_P2P (mapped to TCP)\n",name); break;
    default: printf("Net: socket '%s' type %d refused\n",name,type); return fail_guest(G_EINVAL);
    }
#ifdef _WIN32
    HostSocket s=WSASocketW(AF_INET,host_type,protocol,NULL,0,WSA_FLAG_NO_HANDLE_INHERIT);
    if (s==INVALID_SOCKET) return fail(host_error());
    if (s>=0x7fffffff) { closesocket(s); return fail_guest(G_EMFILE); }
    if (host_type==SOCK_DGRAM) {
        /* An ICMP "port unreachable" must not make the next recvfrom fail (BSD ignores it). */
        BOOL off=FALSE; DWORD n=0;
        WSAIoctl(s,SIO_UDP_CONNRESET,&off,sizeof(off),NULL,0,&n,NULL,NULL);
    }
#else
    HostSocket s=socket(AF_INET,host_type|SOCK_CLOEXEC,protocol);
    if (s<0) return fail(host_error());
    if (host_type==SOCK_DGRAM) {
        /* Two instances on one machine share the LAN discovery port. */
        int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    }
#endif
    nonblocking[slot(s)]=0;
    printf("Net: socket '%s' type %d proto %d -> %d\n",name,type,protocol,(int)s);
    return (int32_t)s;
}
static ABI int32_t net_close(int32_t s) {
    printf("Net: close %d\n",s);
    return host_close((HostSocket)s)<0 ? fail(host_error()) : 0;
}
static ABI int32_t net_abort(int32_t s, int32_t flags) {
    printf("Net: abort %d flags %d\n",s,flags);
#ifdef _WIN32
    shutdown((HostSocket)s,SD_BOTH);
#else
    shutdown(s,SHUT_RDWR);
#endif
    return 0;
}
/* Loopback test mode (SANCTUARY_LAN_IP=127.x.y.z): one machine, one address per instance. */
static int loopback_mode(void) { lan_address(); return (ntohl(lan_ip.s_addr)>>24)==127; }
static void bind_local(HostSocket s) {
    struct sockaddr_in a; socklen_t n=sizeof(a);
    if (getsockname(s,(struct sockaddr *)&a,&n)==0 && a.sin_port) return;
    memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr=lan_ip;
    bind(s,(struct sockaddr *)&a,sizeof(a));
}
static int is_broadcast(const struct sockaddr_in *a) {
    uint32_t ip=a->sin_addr.s_addr;
    return ip==INADDR_BROADCAST || (lan_mask.s_addr && (ip|lan_mask.s_addr)==INADDR_BROADCAST);
}
static ABI int32_t net_bind(int32_t s, const void *addr, uint32_t size) {
    struct sockaddr_in a; char t[32];
    if (to_host_addr(addr,size,&a)<0) return fail_guest(G_EINVAL);
    if (a.sin_addr.s_addr==INADDR_ANY && loopback_mode()) a.sin_addr=lan_ip;
    int r=bind((HostSocket)s,(struct sockaddr *)&a,sizeof(a));
    int e=r<0 ? host_error() : 0;
    printf("Net: bind %d %s -> %s\n",s,addr_text(&a,t,sizeof(t)),r<0 ? host_error_text(e) : "ok");
    return r<0 ? fail(e) : 0;
}
static ABI int32_t net_connect(int32_t s, const void *addr, uint32_t size) {
    struct sockaddr_in a; char t[32];
    if (to_host_addr(addr,size,&a)<0) return fail_guest(G_EINVAL);
    lan_address();
    const char *why=refused(&a);
    if (why) { printf("Net: connect %d %s refused (%s)\n",s,addr_text(&a,t,sizeof(t)),why); return fail_guest(G_ENETUNREACH); }
    note_destination(&a,"connect");
    if (loopback_mode()) bind_local((HostSocket)s);
    int r=connect((HostSocket)s,(struct sockaddr *)&a,sizeof(a));
    int e=r<0 ? host_error() : 0;
    printf("Net: connect %d %s -> %s\n",s,addr_text(&a,t,sizeof(t)),r<0 ? host_error_text(e) : "ok");
    if (r<0) {
#ifdef _WIN32
        if (e==WSAEWOULDBLOCK) return fail_guest(G_EINPROGRESS); /* BSD: a connect in progress */
#endif
        return fail(e);
    }
    return 0;
}
static ABI int32_t net_listen(int32_t s, int32_t backlog) {
    printf("Net: listen %d\n",s);
    return listen((HostSocket)s,backlog)<0 ? fail(host_error()) : 0;
}
static ABI int32_t net_accept(int32_t s, void *addr, uint32_t *size) {
    struct sockaddr_in a; socklen_t n=sizeof(a); char t[32];
#ifdef _WIN32
    HostSocket fd=accept((HostSocket)s,(struct sockaddr *)&a,&n);
    if (fd==INVALID_SOCKET) return fail(host_error());
    nonblocking[slot(fd)]=(uint8_t)is_nonblocking((HostSocket)s); /* inherited */
#else
    HostSocket fd=accept4(s,(struct sockaddr *)&a,&n,SOCK_CLOEXEC);
    if (fd<0) return fail(host_error());
    nonblocking[slot(fd)]=0;
#endif
    /* Only the local network may connect in: anything else is dropped as an aborted connection
     * (BSD's answer for a connection that went away before it was accepted). */
    lan_address();
    const char *why=not_local(a.sin_addr);
    if (why) {
        printf("Net: accept %d from %s refused (%s)\n",s,addr_text(&a,t,sizeof(t)),why);
        host_close(fd);
        return fail_guest(G_ECONNABORTED);
    }
    to_guest_addr(&a,addr,size);
    printf("Net: accept %d from %s -> %d\n",s,addr_text(&a,t,sizeof(t)),(int)fd);
    return (int32_t)fd;
}
static int host_flags(int32_t flags) {
    int f=MSG_NOSIGNAL;
    if (flags&0x1) f|=MSG_OOB;
    if (flags&0x2) f|=MSG_PEEK;
    if (flags&0x4) f|=MSG_DONTROUTE;
    if (flags&0x40) f|=MSG_WAITALL;
#ifndef _WIN32
    if (flags&0x80) f|=MSG_DONTWAIT;
#endif
    return f;
}
/* MSG_DONTWAIT (0x80): Winsock has no such flag; the socket is non-blocking for the call. */
static int dontwait_begin(HostSocket s, int32_t flags) {
#ifdef _WIN32
    if ((flags&0x80) && !is_nonblocking(s)) { u_long v=1; ioctlsocket(s,FIONBIO,&v); return 1; }
#else
    (void)s; (void)flags;
#endif
    return 0;
}
static void dontwait_end(HostSocket s, int toggled) {
#ifdef _WIN32
    if (toggled) { u_long v=0; ioctlsocket(s,FIONBIO,&v); }
#else
    (void)s; (void)toggled;
#endif
}
static unsigned traffic_log;
static int log_traffic(void) { return __atomic_add_fetch(&traffic_log,1,__ATOMIC_RELAXED)<=400; }
static ABI int64_t net_send(int32_t s, const void *data, uint64_t size, int32_t flags) {
    int toggled=dontwait_begin((HostSocket)s,flags);
    int64_t r=send((HostSocket)s,data,(int)size,host_flags(flags));
    int e=r<0 ? host_error() : 0;
    dontwait_end((HostSocket)s,toggled);
    if (log_traffic()) printf("Net: send %d %llu bytes -> %lld\n",s,(unsigned long long)size,(long long)r);
    return r<0 ? fail(e) : r;
}
static ABI int64_t net_sendto(int32_t s, const void *data, uint64_t size, int32_t flags, const void *addr, uint32_t addr_size) {
    if (!addr) return net_send(s,data,size,flags);
    struct sockaddr_in a; char t[32];
    if (to_host_addr(addr,addr_size,&a)<0) return fail_guest(G_EINVAL);
    lan_address();
    const char *why=refused(&a);
    if (why) { printf("Net: sendto %d %s refused (%s)\n",s,addr_text(&a,t,sizeof(t)),why); return fail_guest(G_ENETUNREACH); }
    note_destination(&a,"sendto");
    if (loopback_mode()) {
        bind_local((HostSocket)s);
        if (is_broadcast(&a)) {
            for (uint32_t i=2;i<=9;++i) {
                struct sockaddr_in to=a; to.sin_addr.s_addr=htonl(0x7f000000u|i);
                sendto((HostSocket)s,data,(int)size,host_flags(flags),(struct sockaddr *)&to,sizeof(to));
            }
            if (log_traffic()) printf("Net: sendto %d %s %llu bytes (loopback broadcast)\n",s,addr_text(&a,t,sizeof(t)),(unsigned long long)size);
            return (int64_t)size;
        }
    }
    int toggled=dontwait_begin((HostSocket)s,flags);
    int64_t r=sendto((HostSocket)s,data,(int)size,host_flags(flags),(struct sockaddr *)&a,sizeof(a));
    int e=r<0 ? host_error() : 0;
    dontwait_end((HostSocket)s,toggled);
    if (log_traffic()) printf("Net: sendto %d %s %llu bytes -> %lld%s%s\n",s,addr_text(&a,t,sizeof(t)),(unsigned long long)size,(long long)r,r<0 ? " " : "",r<0 ? host_error_text(e) : "");
    return r<0 ? fail(e) : r;
}
static ABI int64_t net_recv(int32_t s, void *data, uint64_t size, int32_t flags) {
    int toggled=dontwait_begin((HostSocket)s,flags);
    int64_t r=recv((HostSocket)s,data,(int)size,host_flags(flags));
    int e=r<0 ? host_error() : 0;
    dontwait_end((HostSocket)s,toggled);
    if (r<0) return fail(e);
    if (log_traffic()) printf("Net: recv %d %lld bytes\n",s,(long long)r);
    return r;
}
static ABI int64_t net_recvfrom(int32_t s, void *data, uint64_t size, int32_t flags, void *addr, uint32_t *addr_size) {
    struct sockaddr_in a; socklen_t n=sizeof(a); char t[32];
    memset(&a,0,sizeof(a));
    int toggled=dontwait_begin((HostSocket)s,flags);
    int64_t r=recvfrom((HostSocket)s,data,(int)size,host_flags(flags),(struct sockaddr *)&a,&n);
    int e=r<0 ? host_error() : 0;
    dontwait_end((HostSocket)s,toggled);
#ifdef _WIN32
    if (r<0 && e==WSAEMSGSIZE) { r=(int64_t)size; e=0; } /* BSD: the datagram is truncated */
#endif
    if (r<0) return fail(e);
    to_guest_addr(&a,addr,addr_size);
    if (log_traffic()) printf("Net: recvfrom %d %s %lld bytes\n",s,addr_text(&a,t,sizeof(t)),(long long)r);
    return r;
}
static ABI int32_t net_shutdown(int32_t s, int32_t how) {
    printf("Net: shutdown %d %d\n",s,how);
    return shutdown((HostSocket)s,how)<0 ? fail(host_error()) : 0; /* 0/1/2 on both */
}
static ABI int32_t net_getsockname(int32_t s, void *addr, uint32_t *size) {
    struct sockaddr_in a; socklen_t n=sizeof(a);
    if (getsockname((HostSocket)s,(struct sockaddr *)&a,&n)<0) return fail(host_error());
    if (a.sin_addr.s_addr==INADDR_ANY) { lan_address(); a.sin_addr=lan_ip; }
    to_guest_addr(&a,addr,size);
    return 0;
}
static ABI int32_t net_getpeername(int32_t s, void *addr, uint32_t *size) {
    struct sockaddr_in a; socklen_t n=sizeof(a);
    if (getpeername((HostSocket)s,(struct sockaddr *)&a,&n)<0) return fail(host_error());
    to_guest_addr(&a,addr,size);
    return 0;
}

/* Socket options: FreeBSD numbers. Timeouts are int microseconds. SO_NBIO (0x1200) is Sony's. */
static int map_option(int32_t level, int32_t name, int *host_level, int *host_name) {
    if (level==0xffff) {
        *host_level=SOL_SOCKET;
        switch (name) {
        case 0x4: *host_name=SO_REUSEADDR; return 0;
        case 0x8: *host_name=SO_KEEPALIVE; return 0;
        case 0x20: *host_name=SO_BROADCAST; return 0;
        case 0x80: *host_name=SO_LINGER; return 0;
#ifndef _WIN32
        case 0x200: *host_name=SO_REUSEPORT; return 0;
#endif
        case 0x1001: *host_name=SO_SNDBUF; return 0;
        case 0x1002: *host_name=SO_RCVBUF; return 0;
        case 0x1005: *host_name=SO_SNDTIMEO; return 0;
        case 0x1006: *host_name=SO_RCVTIMEO; return 0;
        case 0x1007: *host_name=SO_ERROR; return 0;
        case 0x1008: *host_name=SO_TYPE; return 0;
        }
    } else if (level==0) {
        *host_level=IPPROTO_IP;
        switch (name) {
        case 3: *host_name=IP_TOS; return 0;
        case 4: *host_name=IP_TTL; return 0;
        case 9: *host_name=IP_MULTICAST_IF; return 0;
        case 10: *host_name=IP_MULTICAST_TTL; return 0;
        case 11: *host_name=IP_MULTICAST_LOOP; return 0;
        case 12: *host_name=IP_ADD_MEMBERSHIP; return 0;
        case 13: *host_name=IP_DROP_MEMBERSHIP; return 0;
        }
    } else if (level==6) {
        *host_level=IPPROTO_TCP;
        if (name==1) { *host_name=TCP_NODELAY; return 0; }
#ifndef _WIN32
        if (name==0x200) { *host_name=TCP_MAXSEG; return 0; }
#endif
    }
    return -1;
}
static ABI int32_t net_setsockopt(int32_t s, int32_t level, int32_t name, const void *value, uint32_t size) {
    if (level==0xffff && name==0x1200) { /* SO_NBIO */
        int on=value && size>=4 && *(const int32_t *)value;
        set_nonblocking((HostSocket)s,on);
        printf("Net: setsockopt %d NBIO %d\n",s,on);
        return 0;
    }
    int hl,hn;
    if (map_option(level,name,&hl,&hn)<0) {
        printf("Net: setsockopt %d level 0x%x name 0x%x ignored\n",s,level,name);
        return 0;
    }
    int r;
    if (hl==SOL_SOCKET && (hn==SO_SNDTIMEO || hn==SO_RCVTIMEO)) {
        int32_t us=value && size>=4 ? *(const int32_t *)value : 0;
#ifdef _WIN32
        DWORD ms=(DWORD)(us/1000); if (us>0 && !ms) ms=1;
        r=setsockopt((HostSocket)s,hl,hn,(const char *)&ms,sizeof(ms));
#else
        struct timeval tv={us/1000000,us%1000000};
        r=setsockopt(s,hl,hn,&tv,sizeof(tv));
#endif
    } else if (hl==SOL_SOCKET && hn==SO_LINGER) {
        struct linger l={0,0};
        if (value && size>=8) { l.l_onoff=((const int32_t *)value)[0]; l.l_linger=((const int32_t *)value)[1]; }
        r=setsockopt((HostSocket)s,hl,hn,(const void *)&l,sizeof(l));
    } else {
        r=setsockopt((HostSocket)s,hl,hn,value,(socklen_t)size);
    }
    int e=r<0 ? host_error() : 0;
    printf("Net: setsockopt %d level 0x%x name 0x%x -> %s\n",s,level,name,r<0 ? host_error_text(e) : "ok");
    return r<0 ? fail(e) : 0;
}
static ABI int32_t net_getsockopt(int32_t s, int32_t level, int32_t name, void *value, uint32_t *size) {
    if (!value || !size) return fail_guest(G_EINVAL);
    if (level==0xffff && name==0x1200) {
        *(int32_t *)value=is_nonblocking((HostSocket)s); *size=4; return 0;
    }
    int hl,hn;
    if (map_option(level,name,&hl,&hn)<0) { printf("Net: getsockopt %d level 0x%x name 0x%x refused\n",s,level,name); return fail_guest(G_ENOPROTOOPT); }
    if (hl==SOL_SOCKET && (hn==SO_SNDTIMEO || hn==SO_RCVTIMEO)) {
#ifdef _WIN32
        DWORD ms=0; socklen_t n=sizeof(ms);
        if (getsockopt((HostSocket)s,hl,hn,(char *)&ms,&n)<0) return fail(host_error());
        *(int32_t *)value=(int32_t)(ms*1000);
#else
        struct timeval tv; socklen_t n=sizeof(tv);
        if (getsockopt(s,hl,hn,&tv,&n)<0) return fail(host_error());
        *(int32_t *)value=(int32_t)(tv.tv_sec*1000000+tv.tv_usec);
#endif
        *size=4; return 0;
    }
    if (hl==SOL_SOCKET && hn==SO_ERROR) {
        int e=0; socklen_t n=sizeof(e);
        if (getsockopt((HostSocket)s,hl,hn,(void *)&e,&n)<0) return fail(host_error());
        *(int32_t *)value=e ? guest_net_errno(e) : 0; *size=4; return 0;
    }
    if (hl==SOL_SOCKET && hn==SO_TYPE) {
        int t=0; socklen_t n=sizeof(t);
        if (getsockopt((HostSocket)s,hl,hn,(void *)&t,&n)<0) return fail(host_error());
        *(int32_t *)value=t==SOCK_STREAM ? 1 : t==SOCK_DGRAM ? 2 : 3; *size=4; return 0;
    }
    socklen_t n=(socklen_t)*size;
    if (getsockopt((HostSocket)s,hl,hn,value,&n)<0) return fail(host_error());
    *size=(uint32_t)n;
    return 0;
}

/* ---- Epoll: {u32 events, u32 reserved, u64 ident, u64 data}; IN 1, OUT 2, ERR 8, HUP 0x10 ---- */
typedef struct { uint32_t events, reserved; uint64_t ident, data; } GuestEpollEvent;
#ifdef _WIN32
/* Windows: an epoll is the list of its sockets, waited on with WSAPoll. Ids start at 0x4000. */
#define MAX_EPOLL 64
#define EPOLL_SOCKETS 64
#define EPOLL_BASE 0x4000
typedef struct { int used, count; HostSocket socket[EPOLL_SOCKETS]; uint32_t events[EPOLL_SOCKETS]; uint64_t ident[EPOLL_SOCKETS], data[EPOLL_SOCKETS]; } Epoll;
static Epoll epolls[MAX_EPOLL];
static pthread_mutex_t epoll_lock=PTHREAD_MUTEX_INITIALIZER;
static Epoll *epoll_get(int32_t eid) {
    int i=eid-EPOLL_BASE;
    return i>=0 && i<MAX_EPOLL && epolls[i].used ? &epolls[i] : NULL;
}
static ABI int32_t net_epoll_create(const char *name, int32_t flags) {
    (void)flags;
    pthread_mutex_lock(&epoll_lock);
    int i=0;
    while (i<MAX_EPOLL && epolls[i].used) ++i;
    if (i<MAX_EPOLL) { memset(&epolls[i],0,sizeof(Epoll)); epolls[i].used=1; }
    pthread_mutex_unlock(&epoll_lock);
    if (i==MAX_EPOLL) return fail_guest(G_EMFILE);
    printf("Net: epoll '%s' -> %d\n",name ? name : "",EPOLL_BASE+i);
    return EPOLL_BASE+i;
}
static ABI int32_t net_epoll_destroy(int32_t eid) {
    printf("Net: epoll destroy %d\n",eid);
    pthread_mutex_lock(&epoll_lock);
    Epoll *e=epoll_get(eid);
    if (e) e->used=0;
    pthread_mutex_unlock(&epoll_lock);
    return e ? 0 : fail_guest(G_EBADF);
}
static ABI int32_t net_epoll_control(int32_t eid, int32_t op, int32_t id, const GuestEpollEvent *event) {
    pthread_mutex_lock(&epoll_lock);
    Epoll *e=epoll_get(eid);
    int32_t r=0;
    if (!e) r=fail_guest(G_EBADF);
    else {
        int k=0;
        while (k<e->count && e->socket[k]!=(HostSocket)id) ++k;
        if (op==1 || op==2) {
            if (k==e->count) { if (op==2 || k==EPOLL_SOCKETS) r=fail_guest(op==2 ? 2 : 55); else e->count++; }
            if (!r) { e->socket[k]=(HostSocket)id; e->events[k]=event ? event->events : 0;
                      e->ident[k]=event ? event->ident : 0; e->data[k]=event ? event->data : 0; }
        } else if (op==3) {
            if (k<e->count) { --e->count; e->socket[k]=e->socket[e->count]; e->events[k]=e->events[e->count];
                              e->ident[k]=e->ident[e->count]; e->data[k]=e->data[e->count]; }
        } else r=fail_guest(G_EINVAL);
    }
    pthread_mutex_unlock(&epoll_lock);
    printf("Net: epoll %d op %d socket %d events 0x%x -> %s\n",eid,op,id,event ? event->events : 0,r ? "error" : "ok");
    return r;
}
static ABI int32_t net_epoll_wait(int32_t eid, GuestEpollEvent *events, int32_t max, int32_t timeout_us) {
    if (!events || max<1) return fail_guest(G_EINVAL);
    WSAPOLLFD fds[EPOLL_SOCKETS]; uint64_t ident[EPOLL_SOCKETS], data[EPOLL_SOCKETS];
    pthread_mutex_lock(&epoll_lock);
    Epoll *e=epoll_get(eid);
    int n=e ? e->count : -1;
    for (int k=0;k<n;++k) {
        fds[k].fd=e->socket[k]; fds[k].revents=0;
        fds[k].events=(short)((e->events[k]&1 ? POLLRDNORM : 0)|(e->events[k]&2 ? POLLWRNORM : 0));
        ident[k]=e->ident[k]; data[k]=e->data[k];
    }
    pthread_mutex_unlock(&epoll_lock);
    if (n<0) return fail_guest(G_EBADF);
    int ms=timeout_us<0 ? -1 : (timeout_us+999)/1000;
    if (!n) { Sleep(ms<0 ? 10 : (DWORD)ms); return 0; }
    int ready=WSAPoll(fds,(ULONG)n,ms);
    if (ready<0) return fail(host_error());
    int out=0;
    for (int k=0;k<n && out<max;++k) {
        short v=fds[k].revents;
        if (!v) continue;
        events[out].events=(v&(POLLRDNORM|POLLRDBAND) ? 1u : 0)|(v&POLLWRNORM ? 2u : 0)|(v&(POLLERR|POLLNVAL) ? 8u : 0)|(v&POLLHUP ? 0x10u : 0);
        events[out].reserved=0; events[out].ident=ident[k]; events[out].data=data[k];
        ++out;
    }
    return out;
}
#else
#define MAX_FD 4096
typedef struct { uint64_t ident[MAX_FD], data[MAX_FD]; } EpollData;
static EpollData *epoll_data[MAX_FD];
static pthread_mutex_t epoll_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t to_host_events(uint32_t g) {
    return (g&1 ? EPOLLIN : 0)|(g&2 ? EPOLLOUT : 0)|(g&8 ? EPOLLERR : 0)|(g&0x10 ? EPOLLHUP : 0)|(g&0x40000000 ? EPOLLET : 0);
}
static uint32_t to_guest_events(uint32_t h) {
    return (h&EPOLLIN ? 1u : 0)|(h&EPOLLOUT ? 2u : 0)|(h&EPOLLERR ? 8u : 0)|(h&(EPOLLHUP|EPOLLRDHUP) ? 0x10u : 0);
}
static ABI int32_t net_epoll_create(const char *name, int32_t flags) {
    (void)flags;
    int fd=epoll_create1(EPOLL_CLOEXEC);
    if (fd<0) return fail(errno);
    if (fd>=MAX_FD) { close(fd); return fail_guest(G_EMFILE); }
    pthread_mutex_lock(&epoll_lock);
    if (!epoll_data[fd]) epoll_data[fd]=calloc(1,sizeof(EpollData));
    pthread_mutex_unlock(&epoll_lock);
    printf("Net: epoll '%s' -> %d\n",name ? name : "",fd);
    return fd;
}
static ABI int32_t net_epoll_destroy(int32_t eid) {
    printf("Net: epoll destroy %d\n",eid);
    return close(eid)<0 ? fail(errno) : 0;
}
static ABI int32_t net_epoll_control(int32_t eid, int32_t op, int32_t id, const GuestEpollEvent *event) {
    if (eid<0 || eid>=MAX_FD || id<0 || id>=MAX_FD || !epoll_data[eid]) return fail_guest(G_EBADF);
    struct epoll_event e={0};
    e.data.fd=id;
    if (event) { e.events=to_host_events(event->events); epoll_data[eid]->ident[id]=event->ident; epoll_data[eid]->data[id]=event->data; }
    int host_op=op==1 ? EPOLL_CTL_ADD : op==2 ? EPOLL_CTL_MOD : op==3 ? EPOLL_CTL_DEL : -1;
    if (host_op<0) return fail_guest(G_EINVAL);
    int r=epoll_ctl(eid,host_op,id,&e);
    printf("Net: epoll %d op %d socket %d events 0x%x -> %s\n",eid,op,id,event ? event->events : 0,r<0 ? strerror(errno) : "ok");
    return r<0 ? fail(errno) : 0;
}
static ABI int32_t net_epoll_wait(int32_t eid, GuestEpollEvent *events, int32_t max, int32_t timeout_us) {
    if (eid<0 || eid>=MAX_FD || !epoll_data[eid] || !events || max<1) return fail_guest(G_EINVAL);
    struct epoll_event host[64];
    if (max>64) max=64;
    int ms=timeout_us<0 ? -1 : (timeout_us+999)/1000;
    int n=epoll_wait(eid,host,max,ms);
    if (n<0) return errno==EINTR ? 0 : fail(errno);
    for (int i=0;i<n;++i) {
        int id=host[i].data.fd;
        events[i].events=to_guest_events(host[i].events); events[i].reserved=0;
        events[i].ident=epoll_data[eid]->ident[id]; events[i].data=epoll_data[eid]->data[id];
    }
    return n;
}
#endif
static ABI int32_t net_epoll_abort(int32_t eid, int32_t flags) { printf("Net: epoll abort %d flags %d (ignored)\n",eid,flags); return 0; }

static const RuntimeExport exports[]={
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlGetResult",netctl_result}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceNetGetMacAddress",net_mac}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetSocket",net_socket}, {"sceNetSocketClose",net_close}, {"sceNetSocketAbort",net_abort},
    {"sceNetBind",net_bind}, {"sceNetConnect",net_connect}, {"sceNetListen",net_listen},
    {"sceNetAccept",net_accept}, {"sceNetSend",net_send}, {"sceNetSendto",net_sendto},
    {"sceNetRecv",net_recv}, {"sceNetRecvfrom",net_recvfrom}, {"sceNetShutdown",net_shutdown},
    {"sceNetGetsockname",net_getsockname}, {"sceNetGetpeername",net_getpeername},
    {"sceNetSetsockopt",net_setsockopt}, {"sceNetGetsockopt",net_getsockopt},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetEpollControl",net_epoll_control}, {"sceNetEpollWait",net_epoll_wait},
    {"sceNetEpollAbort",net_epoll_abort},
};
uintptr_t runtime_net_resolve(const char *name) {
    if (!lan_enabled()) return 0;
    return RUNTIME_LOOKUP(exports,name);
}
