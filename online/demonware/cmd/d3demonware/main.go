// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	dw "sanctuaryport/online/demonware"
)

func main() {
	if err := run(); err != nil && !errors.Is(err, flag.ErrHelp) {
		slog.Error("demonware stopped", "error", err)
		os.Exit(1)
	}
}
func run() error {
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	opts, err := readOptions(os.Args[1:], filepath.Join(filepath.Dir(executable), "d3demonware.json"), os.Stderr)
	if err != nil {
		return err
	}
	slog.Info("D3 Demonware configuration loaded", "config", opts.ConfigPath, "build", "0.7-split")
	resolver, err := dw.NewMasterResolver(opts.MasterURL, nil)
	if err != nil {
		return fmt.Errorf("master URL: %w", err)
	}
	defer resolver.Close()
	stunPort, stunHost, _ := listenAddress("stun_listen", opts.STUNListen)
	alternatePort, _, _ := listenAddress("stun_alternate_listen", opts.STUNAlternateListen)
	advertised, err := advertisedIPv4(opts.PublicIP, stunHost)
	if err != nil {
		return err
	}
	title, err := dw.NewTitleServices(dw.TitleConfig{PublisherDir: opts.PublisherDir, StateDir: opts.StateDir, MaxRooms: opts.MaxRooms})
	if err != nil {
		return fmt.Errorf("title services: %w", err)
	}
	slog.Info("D3 publisher files loaded", "directory", opts.PublisherDir, "state", opts.StateDir)
	var lobby *dw.LobbyServer
	auth, err := dw.NewAuthService(dw.AuthConfig{
		Resolve: resolver.Resolve,
		Ready:   func() bool { return lobby != nil && lobby.Ready() },
	})
	if err != nil {
		return err
	}
	lobby = dw.NewLobbyServer(auth, func(session dw.Session, service, task uint8, payload []byte) dw.TaskResult {
		result := title.HandleTask(session, service, task, payload)
		if result.ErrorCode != 0 {
			slog.Warn("Demonware task refused", "player", session.Identity.Name, "service", service, "task", task,
				"error", result.ErrorCode, "reason", result.Reason, "request_bytes", len(payload))
		} else if service == 21 && (task == 1 || task == 3 || task == 5) {
			slog.Info("Demonware matchmaking completed", "player", session.Identity.Name, "service", service, "task", task,
				"results", len(result.Records), "total", result.Total)
		} else if service == 4 {
			slog.Info("Demonware leaderboard completed", "player", session.Identity.Name, "service", service, "task", task,
				"results", len(result.Records), "total", result.Total)
		} else if service != 21 {
			slog.Info("Demonware title task completed", "player", session.Identity.Name, "service", service, "task", task,
				"results", len(result.Records), "total", result.Total)
		}
		return result
	}, opts.MaxClients)
	lobby.OnDisconnect = title.OnDisconnect
	defer lobby.Close()
	lobbyListener, err := net.Listen("tcp4", opts.LobbyListen)
	if err != nil {
		return fmt.Errorf("Demonware TCP lobby: %w", err)
	}
	defer lobbyListener.Close()
	stun, err := dw.NewSTUNServer(advertised, uint16(stunPort), uint16(alternatePort))
	if err != nil {
		return err
	}
	primaryAddress, err := net.ResolveUDPAddr("udp4", opts.STUNListen)
	if err != nil {
		return err
	}
	primary, err := net.ListenUDP("udp4", primaryAddress)
	if err != nil {
		return fmt.Errorf("Demonware UDP bootstrap: %w", err)
	}
	defer primary.Close()
	alternateAddress, err := net.ResolveUDPAddr("udp4", opts.STUNAlternateListen)
	if err != nil {
		return err
	}
	alternate, err := net.ListenUDP("udp4", alternateAddress)
	if err != nil {
		return fmt.Errorf("Demonware alternate UDP bootstrap: %w", err)
	}
	defer alternate.Close()
	authListener, err := net.Listen("tcp4", opts.AuthListen)
	if err != nil {
		return fmt.Errorf("Demonware title auth: %w", err)
	}
	defer authListener.Close()
	authHandlers := http.NewServeMux()
	authHandlers.Handle("/auth/", auth)
	authHandlers.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.Method != http.MethodGet {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		ready := lobby.Ready()
		status := http.StatusOK
		if !ready {
			status = http.StatusServiceUnavailable
		}
		w.WriteHeader(status)
		_ = json.NewEncoder(w).Encode(map[string]any{"service": "d3demonware", "ready": ready})
	})
	authServer := &http.Server{Handler: authHandlers, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 15 * time.Second, WriteTimeout: 15 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 << 10}
	defer authServer.Close()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	errs := make(chan error, 3)
	go func() { errs <- lobby.Serve(lobbyListener) }()
	go func() { errs <- stun.Serve(primary, alternate) }()
	go func() { errs <- authServer.Serve(authListener) }()
	slog.Info("D3 Demonware listening", "lobby", lobbyListener.Addr(), "title_auth", authListener.Addr(),
		"dw_stun", primary.LocalAddr(), "dw_nat_alternate", alternate.LocalAddr(),
		"public_ip", advertised, "master", opts.MasterURL)
	select {
	case <-ctx.Done():
		return nil
	case err := <-errs:
		return err
	}
}
