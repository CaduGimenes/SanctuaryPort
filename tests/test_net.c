/* LAN networking contracts (SANCTUARY_LAN=1): only the local network is reachable, in both
 * directions. The checks run against this machine's real sockets on loopback. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_net.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
uintptr_t runtime_online_resolve(const char *name) { (void)name; return 0; }
int32_t runtime_guest_errno(int e) { return e; }

static struct sockaddr_in at(const char *ip, unsigned port) {
    struct sockaddr_in a={0};
    a.sin_family=AF_INET; a.sin_port=htons((uint16_t)port);
    assert(inet_pton(AF_INET,ip,&a.sin_addr)==1);
    return a;
}
static GuestSockaddrIn guest(const char *ip, unsigned port) {
    struct sockaddr_in a=at(ip,port);
    GuestSockaddrIn g={sizeof(GuestSockaddrIn),2,a.sin_port,a.sin_addr.s_addr,0,{0}};
    return g;
}
static int allowed(const char *ip) { struct sockaddr_in a=at(ip,3074); return refused(&a)==NULL; }

int main(void) {
    /* The console's LAN: 192.168.4.10 on a /22 (broadcast 192.168.7.255). */
    lan_address();
    inet_pton(AF_INET,"192.168.4.10",&lan_ip);
    inet_pton(AF_INET,"255.255.252.0",&lan_mask);

    /* Local destinations: loopback, the private ranges, link-local, the VPN range, broadcast. */
    const char *local[]={"127.0.0.1","127.0.0.3","10.0.0.1","10.255.255.255","172.16.0.1","172.31.255.254",
                         "192.168.0.1","192.168.4.1","169.254.10.20","100.64.0.1","100.127.255.254",
                         "255.255.255.255","192.168.7.255"};
    for (size_t i=0;i<sizeof(local)/sizeof(*local);++i) { if (!allowed(local[i])) printf("not allowed: %s\n",local[i]); assert(allowed(local[i])); }
    /* Internet addresses, including the edges just outside each local range. */
    const char *internet[]={"8.8.8.8","1.1.1.1","203.0.113.10","172.15.255.255","172.32.0.1",
                            "192.169.0.1","169.253.255.255","100.63.255.255","100.128.0.1","11.0.0.1",
                            "126.255.255.255","128.0.0.1"};
    for (size_t i=0;i<sizeof(internet)/sizeof(*internet);++i) {
        struct sockaddr_in a=at(internet[i],3074);
        if (allowed(internet[i])) printf("allowed: %s\n",internet[i]);
        assert(!strcmp(refused(&a),"not on the local network"));
    }
    { struct sockaddr_in a=at("224.0.0.251",5353); assert(!strcmp(refused(&a),"multicast")); }
    { struct sockaddr_in a=at("239.255.255.250",1900); assert(!strcmp(refused(&a),"UPnP")); }
    { struct sockaddr_in a=at("192.168.4.1",1900); assert(!strcmp(refused(&a),"UPnP")); } /* even the router */

    /* sendto: an internet address never reaches the host socket; a local one does. */
    int32_t udp=net_socket("test",2,2,0);
    assert(udp>=0);
    GuestSockaddrIn loop_any=guest("127.0.0.1",0);
    assert(net_bind(udp,&loop_any,sizeof(loop_any))==0);
    GuestSockaddrIn far=guest("8.8.8.8",3074);
    assert(net_sendto(udp,"x",1,0,&far,sizeof(far))==NET_ERROR(G_ENETUNREACH));
    assert(*net_errno_loc()==G_ENETUNREACH);
    struct sockaddr_in self; socklen_t n=sizeof(self);
    assert(getsockname(udp,(struct sockaddr *)&self,&n)==0);
    GuestSockaddrIn back={sizeof(GuestSockaddrIn),2,self.sin_port,self.sin_addr.s_addr,0,{0}};
    assert(net_sendto(udp,"y",1,0,&back,sizeof(back))==1);
    char byte=0;
    assert(recv(udp,&byte,1,0)==1 && byte=='y'); /* only the local packet arrived */
    /* connect: refused the same way. */
    assert(net_connect(udp,&far,sizeof(far))==NET_ERROR(G_ENETUNREACH));
    assert(net_close(udp)==0);

    /* accept: a connection from the local network is accepted. */
    int32_t listener=net_socket("listen",2,1,0);
    assert(listener>=0);
    assert(net_bind(listener,&loop_any,sizeof(loop_any))==0);
    assert(net_listen(listener,4)==0);
    n=sizeof(self);
    assert(getsockname(listener,(struct sockaddr *)&self,&n)==0);
    int client=socket(AF_INET,SOCK_STREAM,0);
    assert(connect(client,(struct sockaddr *)&self,sizeof(self))==0);
    GuestSockaddrIn peer; uint32_t peer_size=sizeof(peer);
    int32_t accepted=net_accept(listener,&peer,&peer_size);
    assert(accepted>=0 && peer.family==2 && peer.addr==htonl(INADDR_LOOPBACK));
    close(client); net_close(accepted); net_close(listener);

    puts("net test: ok");
    return 0;
}
