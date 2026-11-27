// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/hex"
	"net"
	"net/netip"
	"sync"
	"testing"
	"time"
)

func stunFixture(t *testing.T) *STUNServer {
	t.Helper()
	s, err := NewSTUNServer(netip.MustParseAddr("203.0.113.10"), 3074, 3075)
	if err != nil {
		t.Fatal(err)
	}
	return s
}
func stunHex(t *testing.T, value string) []byte {
	t.Helper()
	b, err := hex.DecodeString(value)
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func TestD3STUNWireContract(t *testing.T) {
	s := stunFixture(t)
	observed := netip.MustParseAddrPort("198.51.100.27:40000")
	// e8aa40/e8aa50 serialize type0x1e and LE16 version3. The native
	// e8a4e0 parser reads two six-byte endpoints and XORs the second with
	// LE32 0x2112a442 / LE16 0x2112. Its address serializer copies IP bytes
	// without conversion, followed by the host-order little-endian port.
	want := stunHex(t, "1f0300c633641b409c8497763a52bd")
	if got := s.Reply([]byte{0x1e, 3, 0}, observed, false); !bytes.Equal(got, want) {
		t.Fatalf("native STUN response: %x, want %x", got, want)
	}
	for _, input := range [][]byte{nil, {0x1e}, {0x1e, 3}, {0x1e, 3, 0, 0}, {0x1e, 2, 0}, {0x1e, 4, 0}, {0x1f, 3, 0}} {
		if got := s.Reply(input, observed, false); got != nil {
			t.Fatalf("unsupported request %x produced %x", input, got)
		}
	}
	if got := s.Reply([]byte{0x1e, 3, 0}, observed, true); got != nil {
		t.Fatal("STUN changed source socket")
	}
}
func TestD3NATObservedEndpoints(t *testing.T) {
	s := stunFixture(t)
	observed := netip.MustParseAddrPort("198.51.100.27:40000")
	want := stunHex(t, "150200c633641b409ccb00710a030c")
	if got := s.Reply([]byte{0x14, 2, 0, 0}, observed, false); !bytes.Equal(got, want) {
		t.Fatalf("NAT initial response %x, want %x", got, want)
	}
	// Native state3 compares this observed endpoint with the first probe.
	// A destination-dependent NAT mapping must remain distinguishable.
	changed := netip.MustParseAddrPort("198.51.100.27:40002")
	changedWant := stunHex(t, "150200c633641b429ccb00710a030c")
	if got := s.Reply([]byte{0x14, 2, 0, 2}, changed, true); !bytes.Equal(got, changedWant) {
		t.Fatalf("changed NAT mapping was lost: %x", got)
	}
	for _, alternate := range []bool{false, true} {
		if got := s.Reply([]byte{0x14, 2, 0, 3}, observed, alternate); got != nil {
			t.Fatal("single-IP service claimed a change-IP observation")
		}
	}
	if s.Reply([]byte{0x14, 2, 0, 0}, observed, true) != nil ||
		s.Reply([]byte{0x14, 2, 0, 2}, observed, false) != nil ||
		s.Reply([]byte{0x14, 2, 0, 1}, observed, false) != nil {
		t.Fatal("probe mode used the wrong native socket")
	}
}
func TestSTUNConfigurationAndRateBounds(t *testing.T) {
	for _, ip := range []string{"0.0.0.0", "::1", "224.0.0.1", "255.255.255.255"} {
		if _, err := NewSTUNServer(netip.MustParseAddr(ip), 3074, 3075); err == nil {
			t.Fatal("invalid advertised IP accepted", ip)
		}
	}
	for _, ports := range [][2]uint16{{0, 3075}, {3074, 0}, {3074, 3074}} {
		if _, err := NewSTUNServer(netip.MustParseAddr("127.0.0.1"), ports[0], ports[1]); err == nil {
			t.Fatal("invalid configured ports accepted", ports)
		}
	}
	s := stunFixture(t)
	now := time.Unix(100, 0)
	s.now = func() time.Time { return now }
	ip := netip.MustParseAddr("192.0.2.1")
	for i := 0; i < 100; i++ {
		if !s.allow(ip) {
			t.Fatal("publication rate limited early")
		}
	}
	if s.allow(ip) {
		t.Fatal("publication rate remained unbounded")
	}
	now = now.Add(time.Second)
	if !s.allow(ip) {
		t.Fatal("rate window did not recover")
	}
}

type stunDatagram struct {
	body []byte
	addr net.Addr
}
type stunPacketConn struct {
	port int
	in   chan stunDatagram
	out  chan stunDatagram
	done chan struct{}
	once sync.Once
}

func newSTUNPacketConn(port int) *stunPacketConn {
	return &stunPacketConn{port: port, in: make(chan stunDatagram, 4), out: make(chan stunDatagram, 4), done: make(chan struct{})}
}
func (c *stunPacketConn) ReadFrom(b []byte) (int, net.Addr, error) {
	select {
	case message := <-c.in:
		return copy(b, message.body), message.addr, nil
	case <-c.done:
		return 0, nil, net.ErrClosed
	}
}
func (c *stunPacketConn) WriteTo(b []byte, addr net.Addr) (int, error) {
	select {
	case c.out <- stunDatagram{append([]byte(nil), b...), addr}:
		return len(b), nil
	case <-c.done:
		return 0, net.ErrClosed
	}
}
func (c *stunPacketConn) Close() error                     { c.once.Do(func() { close(c.done) }); return nil }
func (c *stunPacketConn) LocalAddr() net.Addr              { return &net.UDPAddr{IP: net.IPv4zero, Port: c.port} }
func (c *stunPacketConn) SetDeadline(time.Time) error      { return nil }
func (c *stunPacketConn) SetReadDeadline(time.Time) error  { return nil }
func (c *stunPacketConn) SetWriteDeadline(time.Time) error { return nil }

func TestSTUNServeUsesObservedSourceAndClosesBothSockets(t *testing.T) {
	s := stunFixture(t)
	a, b := newSTUNPacketConn(3074), newSTUNPacketConn(3075)
	done := make(chan error, 1)
	go func() { done <- s.Serve(a, b) }()
	from := &net.UDPAddr{IP: net.IPv4(198, 51, 100, 27), Port: 40000}
	a.in <- stunDatagram{[]byte{0x1e, 3, 0}, from}
	select {
	case reply := <-a.out:
		if reply.addr != from || !bytes.Equal(reply.body, stunHex(t, "1f0300c633641b409c8497763a52bd")) {
			t.Fatal("source endpoint was not reflected", reply)
		}
	case <-time.After(time.Second):
		t.Fatal("bootstrap response did not arrive")
	}
	fromAlt := &net.UDPAddr{IP: net.IPv4(198, 51, 100, 27), Port: 40002}
	b.in <- stunDatagram{[]byte{0x14, 2, 0, 2}, fromAlt}
	select {
	case reply := <-b.out:
		if reply.addr != fromAlt || !bytes.Equal(reply.body, stunHex(t, "150200c633641b429ccb00710a030c")) {
			t.Fatal("alternate endpoint was not reflected", reply)
		}
	case <-time.After(time.Second):
		t.Fatal("alternate response did not arrive")
	}
	a.Close()
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("closed socket was not reported")
		}
	case <-time.After(time.Second):
		t.Fatal("service did not stop after socket close")
	}
	select {
	case <-b.done:
	default:
		t.Fatal("alternate listener leaked after primary failure")
	}
}
