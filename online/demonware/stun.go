// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

// Diablo III's bdSTUNClient and bdNATTypeDiscoveryClient use small custom
// packets (retail UDP 3074; private bootstrap ports below). They are not RFC STUN messages or gameplay relay packets.
import (
	"encoding/binary"
	"errors"
	"net"
	"net/netip"
	"sync"
	"time"
)

// Separate discovery from the game's UDP 3074 bind so a master and game can
// coexist on one host. The runtime changes only the remote discovery port.
const BootstrapPort uint16 = 31317
const AlternateBootstrapPort uint16 = 31318

type stunRate struct {
	second int64
	count  uint16
}

// STUNServer reflects the source IPv4/UDP endpoint actually observed by each
// socket. Alternate-port probes let the native client distinguish a stable
// endpoint from destination-dependent mappings. A single public IPv4 cannot
// perform the protocol's change-IP test, so mode 3 is left unanswered rather
// than reporting an open NAT without evidence.
type STUNServer struct {
	advertised                 netip.Addr
	primaryPort, alternatePort uint16
	mu                         sync.Mutex
	rate                       map[netip.Addr]stunRate
	now                        func() time.Time
}

func NewSTUNServer(advertised netip.Addr, primaryPort, alternatePort uint16) (*STUNServer, error) {
	advertised = advertised.Unmap()
	if !advertised.Is4() || advertised.IsUnspecified() || advertised.IsMulticast() ||
		advertised == netip.AddrFrom4([4]byte{255, 255, 255, 255}) ||
		primaryPort == 0 || alternatePort == 0 || primaryPort == alternatePort {
		return nil, errors.New("Demonware bootstrap requires an advertised IPv4 address and two distinct UDP ports")
	}
	return &STUNServer{advertised: advertised, primaryPort: primaryPort, alternatePort: alternatePort,
		rate: make(map[netip.Addr]stunRate), now: time.Now}, nil
}

func appendDWAddress(out []byte, addr netip.AddrPort) []byte {
	ip := addr.Addr().Unmap().As4()
	out = append(out, ip[:]...)
	return binary.LittleEndian.AppendUint16(out, addr.Port())
}

// Reply is the verified D3 v1.46 bootstrap contract. Unknown/oversized requests,
// unsupported versions and change-IP requests receive no response. The caller
// must send the returned bytes from the same primary/alternate socket indicated
// by alternateSocket; the native NAT classifier checks source endpoints.
func (s *STUNServer) Reply(request []byte, observed netip.AddrPort, alternateSocket bool) []byte {
	if !observed.IsValid() || !observed.Addr().Unmap().Is4() || observed.Port() == 0 {
		return nil
	}
	observed = netip.AddrPortFrom(observed.Addr().Unmap(), observed.Port())
	if len(request) == 3 && request[0] == 0x1e && binary.LittleEndian.Uint16(request[1:]) == 3 && !alternateSocket {
		// The native response parser accepts types 0x1e..0x27. 0x1f is the
		// private response type; both endpoints report the observed client.
		out := appendDWAddress([]byte{0x1f, 3, 0}, observed)
		ip := observed.Addr().As4()
		value := binary.LittleEndian.Uint32(ip[:]) ^ 0x2112a442
		out = binary.LittleEndian.AppendUint32(out, value)
		return binary.LittleEndian.AppendUint16(out, observed.Port()^0x2112)
	}
	if len(request) != 4 || request[0] != 0x14 || binary.LittleEndian.Uint16(request[1:3]) != 2 {
		return nil
	}
	mode := request[3]
	if mode != 0 && mode != 2 || mode == 0 && alternateSocket || mode == 2 && !alternateSocket {
		return nil
	}
	out := appendDWAddress([]byte{0x15, 2, 0}, observed)
	return appendDWAddress(out, netip.AddrPortFrom(s.advertised, s.alternatePort))
}

func (s *STUNServer) allow(source netip.Addr) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	second := s.now().Unix()
	if len(s.rate) >= 1024 {
		for ip, value := range s.rate {
			if value.second != second {
				delete(s.rate, ip)
			}
		}
	}
	value, exists := s.rate[source]
	if !exists && len(s.rate) >= 1024 {
		return false
	}
	if value.second != second {
		value = stunRate{second: second}
	}
	if value.count >= 100 {
		return false
	}
	value.count++
	s.rate[source] = value
	return true
}

// Serve takes ownership of both UDP listeners and closes both when either
// receive loop ends. The secondary socket must use alternatePort. Gameplay
// packets are never accepted, rewritten or forwarded by this service.
func (s *STUNServer) Serve(primary, alternate net.PacketConn) error {
	if primary == nil || alternate == nil {
		return errors.New("two Demonware bootstrap UDP listeners are required")
	}
	portOf := func(conn net.PacketConn) uint16 {
		if addr, ok := conn.LocalAddr().(*net.UDPAddr); ok && addr.Port > 0 && addr.Port <= 65535 {
			return uint16(addr.Port)
		}
		return 0
	}
	if portOf(primary) != s.primaryPort || portOf(alternate) != s.alternatePort {
		return errors.New("Demonware bootstrap UDP listeners do not match configured ports")
	}
	defer primary.Close()
	defer alternate.Close()
	errs := make(chan error, 2)
	go func() { errs <- s.serveSocket(primary, false) }()
	go func() { errs <- s.serveSocket(alternate, true) }()
	return <-errs
}

func (s *STUNServer) serveSocket(conn net.PacketConn, alternate bool) error {
	var buffer [32]byte
	for {
		n, source, err := conn.ReadFrom(buffer[:])
		if err != nil {
			return err
		}
		addr, ok := source.(*net.UDPAddr)
		if !ok {
			continue
		}
		observed := addr.AddrPort()
		response := s.Reply(buffer[:n], observed, alternate)
		if response == nil || !s.allow(observed.Addr().Unmap()) {
			continue
		}
		if _, err := conn.WriteTo(response, source); err != nil {
			return err
		}
	}
}
