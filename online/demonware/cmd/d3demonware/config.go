// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// config describes the standalone Demonware title server. It never opens the
// PSN identity, Matching2 signaling or relay listeners; the PSN master remains
// the source of live player sessions.
type config struct {
	LobbyListen         string `json:"lobby_listen"`
	AuthListen          string `json:"auth_listen"`
	STUNListen          string `json:"stun_listen"`
	STUNAlternateListen string `json:"stun_alternate_listen"`
	PublicIP            string `json:"public_ip"`
	MasterURL           string `json:"master_url"`
	MaxClients          int    `json:"max_clients"`
	MaxRooms            int    `json:"max_rooms"`
	PublisherDir        string `json:"publisher_dir"`
	StateDir            string `json:"state_dir"`
}

func defaultConfig() config {
	return config{
		LobbyListen:         "0.0.0.0:3074",
		AuthListen:          "0.0.0.0:31316",
		STUNListen:          "0.0.0.0:31317",
		STUNAlternateListen: "0.0.0.0:31318",
		MasterURL:           "http://127.0.0.1:31315",
		MaxClients:          128,
		MaxRooms:            128,
		PublisherDir:        "publisher",
		StateDir:            "state",
	}
}

type options struct {
	config
	ConfigPath string
}

func readOptions(args []string, defaultPath string, output io.Writer) (options, error) {
	opts := options{config: defaultConfig()}
	flags := flag.NewFlagSet("d3demonware", flag.ContinueOnError)
	flags.SetOutput(output)
	flags.StringVar(&opts.ConfigPath, "config", defaultPath, "JSON config file; created with defaults if missing")
	flags.StringVar(&opts.LobbyListen, "lobby-listen", opts.LobbyListen, "IPv4 TCP address for the encrypted Demonware lobby")
	flags.StringVar(&opts.AuthListen, "auth-listen", opts.AuthListen, "IPv4 HTTP address for private title authentication (/auth/)")
	flags.StringVar(&opts.STUNListen, "stun-listen", opts.STUNListen, "IPv4 UDP address for NAT discovery")
	flags.StringVar(&opts.STUNAlternateListen, "stun-alternate-listen", opts.STUNAlternateListen, "IPv4 UDP address advertised for alternate NAT discovery")
	flags.StringVar(&opts.PublicIP, "public-ip", opts.PublicIP, "NAT discovery IPv4 to advertise; default a specific bind address or first local non-loopback IPv4")
	flags.StringVar(&opts.MasterURL, "master-url", opts.MasterURL, "PSN master HTTP base URL used to resolve live sessions")
	flags.IntVar(&opts.MaxClients, "max-clients", opts.MaxClients, "maximum simultaneous lobby connections")
	flags.IntVar(&opts.MaxRooms, "max-rooms", opts.MaxRooms, "maximum live matchmaking rooms")
	flags.StringVar(&opts.PublisherDir, "publisher-dir", opts.PublisherDir, "directory containing Config.txt, Seasons.txt, Blacklist.txt and ChallengeRifts.txt")
	flags.StringVar(&opts.StateDir, "state-dir", opts.StateDir, "directory containing persistent title counters, mailbox and leaderboard state")
	if err := flags.Parse(args); err != nil {
		return opts, err
	}
	if flags.NArg() != 0 {
		return opts, fmt.Errorf("unexpected argument: %s", flags.Arg(0))
	}
	cfg, err := loadConfig(opts.ConfigPath)
	if err != nil {
		return opts, err
	}
	// Only explicitly supplied flags override the file, including empty values.
	flags.Visit(func(f *flag.Flag) {
		switch f.Name {
		case "lobby-listen":
			cfg.LobbyListen = opts.LobbyListen
		case "auth-listen":
			cfg.AuthListen = opts.AuthListen
		case "stun-listen":
			cfg.STUNListen = opts.STUNListen
		case "stun-alternate-listen":
			cfg.STUNAlternateListen = opts.STUNAlternateListen
		case "public-ip":
			cfg.PublicIP = opts.PublicIP
		case "master-url":
			cfg.MasterURL = opts.MasterURL
		case "max-clients":
			cfg.MaxClients = opts.MaxClients
		case "max-rooms":
			cfg.MaxRooms = opts.MaxRooms
		case "publisher-dir":
			cfg.PublisherDir = opts.PublisherDir
		case "state-dir":
			cfg.StateDir = opts.StateDir
		}
	})
	if err := cfg.validate(); err != nil {
		return opts, fmt.Errorf("config %s: %w", opts.ConfigPath, err)
	}
	opts.config = cfg
	return opts, nil
}

func loadConfig(path string) (config, error) {
	cfg := defaultConfig()
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		data, err = json.MarshalIndent(cfg, "", "  ")
		if err != nil {
			return cfg, err
		}
		data = append(data, '\n')
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			return cfg, fmt.Errorf("create config directory: %w", err)
		}
		file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if errors.Is(err, os.ErrExist) {
			return loadConfig(path)
		}
		if err != nil {
			return cfg, fmt.Errorf("create config %s: %w", path, err)
		}
		_, writeErr := file.Write(data)
		if err := errors.Join(writeErr, file.Close()); err != nil {
			os.Remove(path)
			return cfg, fmt.Errorf("write config %s: %w", path, err)
		}
		cfg.resolvePaths(path)
		return cfg, nil
	}
	if err != nil {
		return cfg, fmt.Errorf("read config %s: %w", path, err)
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	value := &cfg
	if err := decoder.Decode(&value); err != nil {
		return cfg, fmt.Errorf("parse config %s: %w", path, err)
	}
	if value == nil {
		return cfg, fmt.Errorf("config %s must be a JSON object", path)
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return cfg, fmt.Errorf("config %s must contain exactly one JSON object", path)
	}
	cfg.resolvePaths(path)
	return cfg, nil
}

func (cfg *config) resolvePaths(path string) {
	for _, name := range []*string{&cfg.PublisherDir, &cfg.StateDir} {
		if *name != "" && !filepath.IsAbs(*name) {
			*name = filepath.Join(filepath.Dir(path), *name)
		}
	}
}

func (c config) validate() error {
	for _, endpoint := range []struct {
		name, address string
	}{
		{"lobby_listen", c.LobbyListen}, {"auth_listen", c.AuthListen},
		{"stun_listen", c.STUNListen}, {"stun_alternate_listen", c.STUNAlternateListen},
	} {
		if _, _, err := listenAddress(endpoint.name, endpoint.address); err != nil {
			return err
		}
	}
	stunPort, _, _ := listenAddress("stun_listen", c.STUNListen)
	alternatePort, _, _ := listenAddress("stun_alternate_listen", c.STUNAlternateListen)
	if stunPort == alternatePort {
		return errors.New("stun_listen and stun_alternate_listen must use different ports")
	}
	if c.PublicIP != "" {
		ip, err := netip.ParseAddr(c.PublicIP)
		if err != nil || !ip.Unmap().Is4() || ip.IsUnspecified() || ip.IsMulticast() ||
			ip.Unmap() == netip.AddrFrom4([4]byte{255, 255, 255, 255}) {
			return errors.New("public_ip must be a reachable IPv4 address")
		}
	}
	master, err := url.Parse(strings.TrimSuffix(strings.TrimSpace(c.MasterURL), "/"))
	if err != nil || master.Scheme != "http" && master.Scheme != "https" || master.Host == "" ||
		master.User != nil || master.Path != "" || master.RawQuery != "" || master.Fragment != "" {
		return errors.New("master_url must be an http:// or https:// base URL without a path")
	}
	if c.MaxClients < 1 || c.MaxRooms < 1 {
		return errors.New("max_clients and max_rooms must be positive")
	}
	if c.PublisherDir == "" || c.StateDir == "" {
		return errors.New("publisher_dir and state_dir must be nonempty")
	}
	return nil
}

// listenAddress validates an IPv4 host:port and returns its port and host.
func listenAddress(name, address string) (int, string, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return 0, "", fmt.Errorf("%s: %w", name, err)
	}
	p, err := strconv.Atoi(port)
	if err != nil || p < 1 || p > 65535 {
		return 0, "", fmt.Errorf("%s port must be 1–65535", name)
	}
	if host != "" {
		ip, err := netip.ParseAddr(host)
		if err != nil || !ip.Unmap().Is4() {
			return 0, "", fmt.Errorf("%s host must be an IPv4 address or empty", name)
		}
	}
	return p, host, nil
}

// advertisedIPv4 selects the address NAT discovery reports to players.
func advertisedIPv4(configured, bindHost string) (netip.Addr, error) {
	value := configured
	if value == "" {
		value = bindHost
		if strings.TrimSpace(value) == "" || value == "0.0.0.0" {
			addresses, err := net.InterfaceAddrs()
			if err != nil {
				return netip.Addr{}, fmt.Errorf("detect advertised IPv4; set public_ip in the JSON config: %w", err)
			}
			return localAdvertisedIPv4(addresses), nil
		}
	}
	address, err := netip.ParseAddr(value)
	if err != nil || !address.Unmap().Is4() || address.IsUnspecified() || address.IsMulticast() ||
		address.Unmap() == netip.AddrFrom4([4]byte{255, 255, 255, 255}) {
		return netip.Addr{}, errors.New("set public_ip in the JSON config (or -public-ip) to the server's reachable IPv4 address")
	}
	return address.Unmap(), nil
}

// Automatic wildcard hosting advertises a local address for LAN use. Internet
// hosts behind NAT must configure their public IPv4 explicitly.
func localAdvertisedIPv4(addresses []net.Addr) netip.Addr {
	for _, address := range addresses {
		prefix, err := netip.ParsePrefix(address.String())
		if err != nil {
			continue
		}
		ip := prefix.Addr().Unmap()
		if ip.Is4() && ip.IsGlobalUnicast() && !ip.IsLoopback() {
			return ip
		}
	}
	return netip.MustParseAddr("127.0.0.1")
}
