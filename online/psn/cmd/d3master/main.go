// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"syscall"
	"time"

	"sanctuaryport/online/psn/internal/master"
)

func main() {
	if err := run(); err != nil && !errors.Is(err, flag.ErrHelp) {
		slog.Error("master stopped", "error", err)
		os.Exit(1)
	}
}
func run() error {
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	opts, err := readOptions(os.Args[1:], filepath.Join(filepath.Dir(executable), "d3master.json"), os.Stderr)
	if err != nil {
		return err
	}
	slog.Info("D3 PSN master configuration loaded", "config", opts.ConfigPath, "build", "0.7-split")
	if opts.legacyDemonwareSettings() {
		slog.Warn("ignoring pre-split Demonware settings; run the separate d3demonware server for lobby, STUN and title services")
	}
	if opts.AddUser != "" {
		if opts.Users == "" {
			opts.Users = "users.json"
		}
		return master.AddAccount(opts.Users, opts.AddUser, os.Getenv("D3_PASSWORD"))
	}
	var accounts master.Accounts
	if opts.Users != "" {
		var err error
		accounts, err = master.LoadAccounts(opts.Users)
		if err != nil {
			return fmt.Errorf("accounts: %w", err)
		}
	}
	var tlsConfig *tls.Config
	if !opts.Plaintext && (opts.Cert != "" || opts.Key != "") {
		pair, e := tls.LoadX509KeyPair(opts.Cert, opts.Key)
		if e != nil {
			return fmt.Errorf("TLS certificate: %w", e)
		}
		tlsConfig = &tls.Config{MinVersion: tls.VersionTLS13, Certificates: []tls.Certificate{pair}}
	}
	host, port, err := net.SplitHostPort(opts.Listen)
	if err != nil {
		return err
	}
	p, err := strconv.Atoi(port)
	if err != nil || p < 1 || p > 65535 {
		return errors.New("TCP port must be 1–65535")
	}
	if opts.WebListen == "" {
		opts.WebListen = net.JoinHostPort(host, strconv.Itoa(p+2))
	}
	if opts.SignalingListen == "" {
		opts.SignalingListen = net.JoinHostPort(host, strconv.Itoa(p+1))
	}
	tcp, err := net.Listen("tcp4", opts.Listen)
	if err != nil {
		return err
	}
	defer tcp.Close()
	if tlsConfig != nil {
		tcp = tls.NewListener(tcp, tlsConfig)
	}
	sigAddr, err := net.ResolveUDPAddr("udp4", opts.SignalingListen)
	if err != nil {
		return err
	}
	signaling, err := net.ListenUDP("udp4", sigAddr)
	if err != nil {
		return err
	}
	defer signaling.Close()
	relayAddr, err := net.ResolveUDPAddr("udp4", opts.Relay)
	if err != nil {
		return err
	}
	relayConn, err := net.ListenUDP("udp4", relayAddr)
	if err != nil {
		return err
	}
	defer relayConn.Close()
	server := master.New(master.Config{Accounts: accounts, MaxClients: opts.MaxClients, MaxRooms: opts.MaxRooms})
	defer server.Close()
	web, err := net.Listen("tcp4", opts.WebListen)
	if err != nil {
		return err
	}
	defer web.Close()
	webHandlers := http.NewServeMux()
	// The optional Demonware server reuses live master sessions through this
	// route. It is authenticated by the presented session token itself.
	webHandlers.Handle("/internal/identity", server.IdentityHandler())
	webHandlers.Handle("/", server.WebHandler())
	webServer := &http.Server{Handler: webHandlers, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 15 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 << 10}
	defer webServer.Close()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	errs := make(chan error, 4)
	go func() { errs <- server.Serve(tcp) }()
	go func() { errs <- server.ServeUDP(signaling) }()
	go func() { errs <- server.ServeUDP(relayConn) }()
	go func() { errs <- webServer.Serve(web) }()
	slog.Info("D3 PSN master listening", "tcp", tcp.Addr(), "signaling", signaling.LocalAddr(), "relay", relayConn.LocalAddr(), "webapi", web.Addr(), "name_only", accounts == nil, "tls", tlsConfig != nil)
	select {
	case <-ctx.Done():
		return nil
	case err := <-errs:
		return err
	}
}
