// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"io"
	"math/big"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestNativeTLSProtobufAndUDP(t *testing.T) {
	binary := os.Getenv("D3_NATIVE_TRANSPORT_TEST")
	if binary == "" {
		t.Skip("set D3_NATIVE_TRANSPORT_TEST to the CMake d3-master-transport-test executable")
	}
	dir := t.TempDir()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tmpl := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "localhost"}, DNSNames: []string{"localhost"}, NotBefore: time.Now().Add(-time.Minute), NotAfter: time.Now().Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}, IsCA: true, BasicConstraintsValid: true}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	pair, err := tls.X509KeyPair(certPEM, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}))
	if err != nil {
		t.Fatal(err)
	}
	ca := filepath.Join(dir, "ca.pem")
	if err = os.WriteFile(ca, certPEM, 0600); err != nil {
		t.Fatal(err)
	}
	s := fixture(t)
	for _, test := range []struct{ host, result string }{{"localhost", "accept"}, {"wrong-host", "reject"}} {
		t.Run(test.host, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, binary, "--stdio", test.host, ca, test.result)
			input, err := cmd.StdinPipe()
			if err != nil {
				t.Fatal(err)
			}
			output, err := cmd.StdoutPipe()
			if err != nil {
				t.Fatal(err)
			}
			var stderr bytes.Buffer
			cmd.Stderr = &stderr
			if err = cmd.Start(); err != nil {
				t.Fatal(err)
			}
			conn := &pipeConn{ReadCloser: output, input: input}
			done := make(chan struct{})
			go func() {
				defer close(done)
				s.ServeConn(tls.Server(remoteConn{conn}, &tls.Config{MinVersion: tls.VersionTLS13, Certificates: []tls.Certificate{pair}}))
			}()
			if err := cmd.Wait(); err != nil {
				t.Fatalf("native TLS/protobuf: %s: %v", stderr.String(), err)
			}
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("native TLS connection did not clean up")
			}
		})
	}
	a := login(t, s, "Alice")
	output := filepath.Join(dir, "signed.bin")
	cmd := exec.Command(binary, "--sign", output, a.token)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("native HMAC: %s %v", out, err)
	}
	signed, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	addr := &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 3658}
	reply, _ := s.Datagram(signed, addr, time.Now())
	if len(reply) != 6 {
		t.Fatal("Go rejected the native runtime's signed STUN packet")
	}
}

type pipeConn struct {
	io.ReadCloser
	input io.WriteCloser
}

func TestNativeNameOnlyPlaintextLogin(t *testing.T) {
	binary := os.Getenv("D3_NATIVE_TRANSPORT_TEST")
	if binary == "" {
		t.Skip("native transport fixture not configured")
	}
	s := New(Config{})
	defer s.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, binary, "--plaintext")
	input, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err = cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { defer close(done); s.ServeConn(&pipeConn{ReadCloser: output, input: input}) }()
	if err = cmd.Wait(); err != nil {
		t.Fatalf("native name-only login: %s: %v", stderr.String(), err)
	}
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("name-only session did not clean up")
	}
}

func (c *pipeConn) Write(b []byte) (int, error) { return c.input.Write(b) }
func (c *pipeConn) Close() error                { c.input.Close(); return c.ReadCloser.Close() }
func (c *pipeConn) LocalAddr() net.Addr         { return &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 31313} }
func (c *pipeConn) RemoteAddr() net.Addr {
	return &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40000}
}
func (c *pipeConn) SetDeadline(d time.Time) error { c.SetReadDeadline(d); return c.SetWriteDeadline(d) }
func (c *pipeConn) SetReadDeadline(d time.Time) error {
	return c.ReadCloser.(*os.File).SetReadDeadline(d)
}
func (c *pipeConn) SetWriteDeadline(d time.Time) error { return c.input.(*os.File).SetWriteDeadline(d) }
