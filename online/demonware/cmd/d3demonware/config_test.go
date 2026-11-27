// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestMissingConfigCreatedAndReloaded(t *testing.T) {
	path := filepath.Join(t.TempDir(), "new", "d3demonware.json")
	opts, err := readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.LobbyListen != "0.0.0.0:3074" || opts.AuthListen != "0.0.0.0:31316" || opts.MasterURL != "http://127.0.0.1:31315" || opts.MaxClients != 128 {
		t.Fatalf("unexpected defaults: %+v", opts)
	}
	data, err := os.ReadFile(path)
	if err != nil || !json.Valid(data) {
		t.Fatalf("default JSON was not created: %s (%v)", data, err)
	}
	var fields map[string]any
	if err := json.Unmarshal(data, &fields); err != nil {
		t.Fatal(err)
	}
	if len(fields) != 10 || fields["lobby_listen"] != "0.0.0.0:3074" {
		t.Fatalf("incomplete config: %v", fields)
	}
	edited := `{"lobby_listen":"0.0.0.0:32000","auth_listen":"0.0.0.0:32002","master_url":"http://master.example:32002","max_clients":42}`
	if err := os.WriteFile(path, []byte(edited), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err = readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.LobbyListen != "0.0.0.0:32000" || opts.AuthListen != "0.0.0.0:32002" || opts.MasterURL != "http://master.example:32002" || opts.MaxClients != 42 || opts.MaxRooms != 128 {
		t.Fatalf("JSON settings/defaults were not loaded: %+v", opts)
	}
	after, err := os.ReadFile(path)
	if err != nil || string(after) != edited {
		t.Fatalf("existing config was changed: %s (%v)", after, err)
	}
}

func TestExplicitFlagsOverrideJSONWithoutSaving(t *testing.T) {
	path := filepath.Join(t.TempDir(), "custom.json")
	contents := `{"lobby_listen":"0.0.0.0:32000","max_rooms":12,"master_url":"http://127.0.0.1:31315"}`
	if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
	unusedPath := filepath.Join(t.TempDir(), "unused.json")
	opts, err := readOptions([]string{"-config", path, "-lobby-listen", "127.0.0.1:40000", "-max-rooms", "7", "-public-ip", "203.0.113.4", "-master-url", "https://psn.example:31315"}, unusedPath, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.LobbyListen != "127.0.0.1:40000" || opts.MaxRooms != 7 || opts.PublicIP != "203.0.113.4" || opts.MasterURL != "https://psn.example:31315" || opts.AuthListen != "0.0.0.0:31316" {
		t.Fatalf("incorrect override precedence: %+v", opts)
	}
	if data, err := os.ReadFile(path); err != nil || string(data) != contents {
		t.Fatalf("overrides modified config: %s (%v)", data, err)
	}
	if _, err := os.Stat(unusedPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("unused default path was created: %v", err)
	}
}

func TestTitleDataPathsAreRelativeToConfigFileAndCLIToWorkingDirectory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "d3demonware.json")
	if err := os.WriteFile(path, []byte(`{"publisher_dir":"custom-publisher","state_dir":"custom-state"}`), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err := readOptions(nil, path, io.Discard)
	if err != nil || opts.PublisherDir != filepath.Join(filepath.Dir(path), "custom-publisher") || opts.StateDir != filepath.Join(filepath.Dir(path), "custom-state") {
		t.Fatalf("title paths not config-relative: %+v (%v)", opts, err)
	}
	opts, err = readOptions([]string{"-publisher-dir", "cli-publisher", "-state-dir", "cli-state"}, path, io.Discard)
	if err != nil || opts.PublisherDir != "cli-publisher" || opts.StateDir != "cli-state" {
		t.Fatalf("title CLI paths not working-directory relative: %+v (%v)", opts, err)
	}
}

func TestInvalidConfigIsReportedAndPreserved(t *testing.T) {
	for _, contents := range []string{
		`{`, `null`, `[]`, `{} {}`, `{} trailing`, `{"max_client":2}`, `{"max_clients":"2"}`,
		`{"lobby_listen":"bad"}`, `{"lobby_listen":"0.0.0.0:0"}`, `{"auth_listen":"0.0.0.0:65536"}`,
		`{"stun_listen":"[::1]:31317"}`, `{"stun_listen":"0.0.0.0:31318"}`,
		`{"stun_alternate_listen":"0.0.0.0:0"}`,
		`{"public_ip":"example.com"}`, `{"public_ip":"::1"}`, `{"public_ip":"0.0.0.0"}`,
		`{"master_url":""}`, `{"master_url":"ftp://master"}`, `{"master_url":"http://master/path"}`,
		`{"max_clients":0}`, `{"max_rooms":-1}`, `{"publisher_dir":""}`, `{"state_dir":""}`,
	} {
		t.Run(contents, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "d3demonware.json")
			if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
				t.Fatal(err)
			}
			_, err := readOptions(nil, path, io.Discard)
			if err == nil || !strings.Contains(err.Error(), path) {
				t.Fatalf("expected error identifying config, got %v", err)
			}
			if after, err := os.ReadFile(path); err != nil || string(after) != contents {
				t.Fatalf("invalid config was overwritten: %s (%v)", after, err)
			}
		})
	}
}

func TestAllListenersCanBeConfiguredIndependently(t *testing.T) {
	path := filepath.Join(t.TempDir(), "d3demonware.json")
	contents := `{"lobby_listen":"127.0.0.1:42001","auth_listen":"127.0.0.1:42002","stun_listen":"127.0.0.1:42003","stun_alternate_listen":"127.0.0.1:42004"}`
	if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err := readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.LobbyListen != "127.0.0.1:42001" || opts.AuthListen != "127.0.0.1:42002" || opts.STUNListen != "127.0.0.1:42003" || opts.STUNAlternateListen != "127.0.0.1:42004" {
		t.Fatalf("custom listeners were not loaded: %+v", opts)
	}
	opts, err = readOptions([]string{"-stun-listen", "0.0.0.0:43001", "-stun-alternate-listen", "0.0.0.0:43002"}, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.STUNListen != "0.0.0.0:43001" || opts.STUNAlternateListen != "0.0.0.0:43002" {
		t.Fatalf("custom listener flags were not applied: %+v", opts)
	}
}

func TestHelpAndBadArgumentsDoNotCreateConfig(t *testing.T) {
	for _, args := range [][]string{{"-help"}, {"-unknown"}, {"unexpected"}} {
		path := filepath.Join(t.TempDir(), "d3demonware.json")
		_, err := readOptions(args, path, io.Discard)
		if err == nil || args[0] == "-help" && !errors.Is(err, flag.ErrHelp) {
			t.Fatalf("%v: unexpected error %v", args, err)
		}
		if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
			t.Fatalf("config created for %v: %v", args, err)
		}
	}
}

func TestUnreadableConfigReturnsError(t *testing.T) {
	path := t.TempDir() // Reading a directory must fail, not generate defaults.
	if _, err := readOptions(nil, path, io.Discard); err == nil {
		t.Fatal("accepted unreadable config")
	}
}
