// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"net"
	"strings"
	"time"
)

// UDP uses the native module's STUN echo and BBR1 relay framing. D3 clients
// append a timestamp and HMAC-SHA256 to STUN and registration packets. Legacy
// unauthenticated registration is deliberately rejected by this server.
func (s *Server) ServeUDP(conn *net.UDPConn) error {
	b := make([]byte, 2049)
	for {
		n, from, err := conn.ReadFromUDP(b)
		if err != nil {
			return err
		}
		if n > 2048 {
			continue
		}
		out, dst := s.Datagram(b[:n], from, time.Now())
		if len(out) > 0 {
			conn.WriteToUDP(out, dst)
		}
	}
}
func paddedID(b []byte) string { return strings.TrimRight(string(b), "\x00") }
func sameUDP(a, b *net.UDPAddr) bool {
	return a != nil && b != nil && a.Port == b.Port && a.IP.Equal(b.IP)
}
func (s *Server) Datagram(b []byte, from *net.UDPAddr, now time.Time) ([]byte, *net.UDPAddr) {
	if from == nil || from.IP.To4() == nil {
		return nil, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	// Signaling publication: original 21-byte ping + timestamp + HMAC-SHA256.
	if len(b) == 61 && b[0] == 1 {
		c := s.sessions[paddedID(b[1:17])]
		if !udpAuth(c, b, from, now) {
			return nil, nil
		}
		if !udpBudget(c, now) {
			return nil, nil
		}
		c.udp = &net.UDPAddr{IP: append(net.IP(nil), from.IP...), Port: from.Port}
		c.udpSeen = now
		return endpoint(from), from
	}
	if len(b) < 5 || !bytes.Equal(b[:4], []byte("BBR1")) {
		return nil, nil
	}
	if b[4] == 1 && len(b) == 61 {
		c := s.sessions[paddedID(b[5:21])]
		if !udpAuth(c, b, from, now) || !udpBudget(c, now) {
			return nil, nil
		}
		c.udp = &net.UDPAddr{IP: append(net.IP(nil), from.IP...), Port: from.Port}
		c.udpSeen = now
		return []byte("BBR1\x02"), from
	}
	if b[4] != 3 || len(b) < 43 {
		return nil, nil
	}
	dst := s.sessions[paddedID(b[5:21])]
	src := s.sessions[paddedID(b[21:37])]
	if src == nil || dst == nil || src.room == 0 || src.room != dst.room || !sameUDP(src.udp, from) || dst.udp == nil || now.Sub(src.udpSeen) > 30*time.Second || now.Sub(dst.udpSeen) > 30*time.Second || !udpBudget(src, now) {
		return nil, nil
	}
	// Use the server-observed endpoint rather than the caller-supplied ep[6].
	out := append([]byte("BBR1\x04"), b[21:37]...)
	out = append(out, endpoint(from)...)
	out = append(out, b[43:]...)
	return out, dst.udp
}
func udpAuth(c *client, b []byte, from *net.UDPAddr, now time.Time) bool {
	if c == nil || c.ctx == 0 || len(b) != 61 {
		return false
	}
	stamp := int64(binary.BigEndian.Uint64(b[21:29]))
	delta := now.Unix() - stamp
	if delta < -30 || delta > 30 {
		return false
	}
	mac := hmac.New(sha256.New, []byte(c.token))
	mac.Write(b[:29])
	if !hmac.Equal(mac.Sum(nil), b[29:]) {
		return false
	}
	host, _, err := net.SplitHostPort(c.conn.RemoteAddr().String())
	return err == nil && net.ParseIP(host).Equal(from.IP)
}
func udpBudget(c *client, now time.Time) bool {
	if now.Sub(c.udpWindow) >= time.Second {
		c.udpWindow = now
		c.udpCount = 0
	}
	c.udpCount++
	return c.udpCount <= 4000
}
func endpoint(a *net.UDPAddr) []byte {
	b := make([]byte, 6)
	copy(b, a.IP.To4())
	binary.BigEndian.PutUint16(b[4:], uint16(a.Port))
	return b
}
