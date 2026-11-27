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
	"os"
	"path/filepath"
	"strconv"
)

// config describes the standalone PSN master. Demonware settings accepted
// here only for compatibility with pre-split files are ignored; d3demonware
// owns those listeners and directories.
type config struct {
	Listen          string `json:"listen"`
	Relay           string `json:"relay"`
	WebListen       string `json:"web_listen"`
	SignalingListen string `json:"signaling_listen"`
	Users           string `json:"users"`
	MaxClients      int    `json:"max_clients"`
	MaxRooms        int    `json:"max_rooms"`
	Cert            string `json:"cert"`
	Key             string `json:"key"`
	Plaintext       bool   `json:"plaintext"`

	LobbyPort         int    `json:"lobby_port,omitempty"`
	STUNPort          int    `json:"stun_port,omitempty"`
	STUNAlternatePort int    `json:"stun_alternate_port,omitempty"`
	DWPublicIP        string `json:"dw_public_ip,omitempty"`
	PublisherDir      string `json:"publisher_dir,omitempty"`
	StateDir          string `json:"state_dir,omitempty"`
}

func defaultConfig() config {
	return config{Listen: "0.0.0.0:31313", Relay: "0.0.0.0:3478", MaxClients: 128, MaxRooms: 128}
}

// legacyDemonwareSettings reports whether a pre-split config still carries
// settings now owned by the d3demonware server.
func (c config) legacyDemonwareSettings() bool {
	return c.LobbyPort != 0 || c.STUNPort != 0 || c.STUNAlternatePort != 0 ||
		c.DWPublicIP != "" || c.PublisherDir != "" || c.StateDir != ""
}

type options struct {
	config
	ConfigPath string
	AddUser    string
}

func readOptions(args []string, defaultPath string, output io.Writer) (options, error) {
	opts := options{config: defaultConfig()}
	flags := flag.NewFlagSet("d3master", flag.ContinueOnError)
	flags.SetOutput(output)
	flags.StringVar(&opts.ConfigPath, "config", defaultPath, "JSON config file; created with defaults if missing")
	flags.StringVar(&opts.Listen, "listen", opts.Listen, "IPv4 TCP address; signaling UDP uses TCP port + 1")
	flags.StringVar(&opts.Relay, "relay", opts.Relay, "IPv4 P2P relay UDP address")
	flags.StringVar(&opts.WebListen, "web-listen", opts.WebListen, "NP Toolkit HTTP address; default TCP host and port + 2")
	flags.StringVar(&opts.SignalingListen, "signaling-listen", opts.SignalingListen, "signaling UDP address; default TCP host and port + 1")
	flags.StringVar(&opts.Users, "users", opts.Users, "optional account database; omit for player-name-only login")
	flags.StringVar(&opts.AddUser, "add-user", "", "create account using D3_PASSWORD from the environment, then exit")
	flags.IntVar(&opts.MaxClients, "max-clients", opts.MaxClients, "maximum simultaneous connections")
	flags.IntVar(&opts.MaxRooms, "max-rooms", opts.MaxRooms, "maximum live rooms")
	flags.StringVar(&opts.Cert, "cert", opts.Cert, "optional TLS certificate PEM; omit for plain TCP")
	flags.StringVar(&opts.Key, "key", opts.Key, "TLS private key PEM")
	flags.BoolVar(&opts.Plaintext, "plaintext", opts.Plaintext, "use plain TCP even when certificates are configured")
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
	// Only explicitly supplied flags override the file, including false/empty values.
	flags.Visit(func(f *flag.Flag) {
		switch f.Name {
		case "listen":
			cfg.Listen = opts.Listen
		case "relay":
			cfg.Relay = opts.Relay
		case "web-listen":
			cfg.WebListen = opts.WebListen
		case "signaling-listen":
			cfg.SignalingListen = opts.SignalingListen
		case "users":
			cfg.Users = opts.Users
		case "max-clients":
			cfg.MaxClients = opts.MaxClients
		case "max-rooms":
			cfg.MaxRooms = opts.MaxRooms
		case "cert":
			cfg.Cert = opts.Cert
		case "key":
			cfg.Key = opts.Key
		case "plaintext":
			cfg.Plaintext = opts.Plaintext
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
	for _, name := range []*string{&cfg.Users, &cfg.Cert, &cfg.Key} {
		if *name != "" && !filepath.IsAbs(*name) {
			*name = filepath.Join(filepath.Dir(path), *name)
		}
	}
}

func (c config) validate() error {
	for _, endpoint := range []struct {
		name, address string
	}{{"listen", c.Listen}, {"relay", c.Relay}, {"web_listen", c.WebListen}, {"signaling_listen", c.SignalingListen}} {
		if (endpoint.name == "web_listen" || endpoint.name == "signaling_listen") && endpoint.address == "" {
			continue
		}
		_, port, err := net.SplitHostPort(endpoint.address)
		if err != nil {
			return fmt.Errorf("%s: %w", endpoint.name, err)
		}
		p, err := strconv.Atoi(port)
		if err != nil || p < 1 || p > 65535 {
			return fmt.Errorf("%s port must be 1–65535", endpoint.name)
		}
	}
	_, tcpPort, _ := net.SplitHostPort(c.Listen)
	p, _ := strconv.Atoi(tcpPort)
	if c.WebListen == "" && p > 65533 || c.SignalingListen == "" && p > 65534 {
		return errors.New("set web_listen and/or signaling_listen explicitly when the derived port would exceed 65535")
	}
	if c.MaxClients < 1 || c.MaxRooms < 1 {
		return errors.New("max_clients and max_rooms must be positive")
	}
	if !c.Plaintext && (c.Cert == "") != (c.Key == "") {
		return errors.New("cert and key must both be set to enable TLS")
	}
	return nil
}
