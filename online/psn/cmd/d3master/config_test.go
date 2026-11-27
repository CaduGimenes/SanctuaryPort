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
	path := filepath.Join(t.TempDir(), "new", "d3master.json")
	opts, err := readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.Listen != "0.0.0.0:31313" || opts.Relay != "0.0.0.0:3478" || opts.MaxClients != 128 {
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
	if len(fields) != 10 || fields["listen"] != "0.0.0.0:31313" {
		t.Fatalf("incomplete config: %v", fields)
	}
	// Real user edits are loaded on the next invocation, without rewriting them.
	edited := `{"listen":"0.0.0.0:32000","web_listen":"0.0.0.0:32002","max_clients":42}`
	if err := os.WriteFile(path, []byte(edited), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err = readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.Listen != "0.0.0.0:32000" || opts.WebListen != "0.0.0.0:32002" || opts.MaxClients != 42 || opts.MaxRooms != 128 {
		t.Fatalf("JSON settings/defaults were not loaded: %+v", opts)
	}
	after, err := os.ReadFile(path)
	if err != nil || string(after) != edited {
		t.Fatalf("existing config was changed: %s (%v)", after, err)
	}
}

// Pre-split configurations remain loadable so upgrading does not stop the PSN
// server. The Demonware values are ignored and reported to the operator.
func TestPreSplitDemonwareSettingsAreAcceptedAndIgnored(t *testing.T) {
	path := filepath.Join(t.TempDir(), "d3master.json")
	contents := `{"listen":"0.0.0.0:32000","lobby_port":3074,"stun_port":31317,"stun_alternate_port":31318,"dw_public_ip":"203.0.113.4","publisher_dir":"publisher","state_dir":"state"}`
	if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err := readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.Listen != "0.0.0.0:32000" || !opts.legacyDemonwareSettings() {
		t.Fatalf("pre-split config not reported: %+v", opts)
	}
	if opts.PublisherDir != "publisher" || opts.StateDir != "state" || filepath.IsAbs(opts.PublisherDir) {
		t.Fatal("ignored Demonware paths must not be resolved or used")
	}
	if after, err := os.ReadFile(path); err != nil || string(after) != contents {
		t.Fatalf("existing config was changed: %s (%v)", after, err)
	}
}

func TestExplicitFlagsOverrideJSONWithoutSaving(t *testing.T) {
	path := filepath.Join(t.TempDir(), "custom.json")
	contents := `{"listen":"0.0.0.0:32000","relay":"0.0.0.0:3479","max_rooms":12,"plaintext":true,"users":"accounts.json"}`
	if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
	unusedPath := filepath.Join(t.TempDir(), "unused.json")
	opts, err := readOptions([]string{"-config", path, "-listen", "127.0.0.1:40000", "-max-rooms", "7", "-plaintext=false", "-users=", "-add-user", "PlayerOne"}, unusedPath, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.Listen != "127.0.0.1:40000" || opts.MaxRooms != 7 || opts.Plaintext || opts.Users != "" || opts.AddUser != "PlayerOne" || opts.Relay != "0.0.0.0:3479" {
		t.Fatalf("incorrect override precedence: %+v", opts)
	}
	if data, err := os.ReadFile(path); err != nil || string(data) != contents {
		t.Fatalf("overrides modified config: %s (%v)", data, err)
	}
	if _, err := os.Stat(unusedPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("unused default path was created: %v", err)
	}
}

func TestConfigPathsAreRelativeToConfigFileAndCLIPathsToWorkingDirectory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "d3master.json")
	if err := os.WriteFile(path, []byte(`{"users":"accounts/users.json","cert":"tls/cert.pem","key":"tls/key.pem"}`), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err := readOptions([]string{"-users", "cli-users.json"}, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.Users != "cli-users.json" || opts.Cert != filepath.Join(filepath.Dir(path), "tls/cert.pem") || opts.Key != filepath.Join(filepath.Dir(path), "tls/key.pem") {
		t.Fatalf("incorrect relative path resolution: %+v", opts)
	}
}

func TestInvalidConfigIsReportedAndPreserved(t *testing.T) {
	for _, contents := range []string{
		`{`, `null`, `[]`, `{} {}`, `{} trailing`, `{"max_client":2}`, `{"max_clients":"2"}`,
		`{"listen":"bad"}`, `{"listen":"0.0.0.0:65534"}`, `{"relay":"0.0.0.0:0"}`,
		`{"web_listen":"0.0.0.0:65536"}`, `{"max_clients":0}`, `{"max_rooms":-1}`,
		`{"signaling_listen":"0.0.0.0:0"}`,
		`{"cert":"cert.pem"}`, `{"key":"key.pem"}`,
	} {
		t.Run(contents, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "d3master.json")
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

func TestPSNServicesCanBeConfiguredIndependently(t *testing.T) {
	path := filepath.Join(t.TempDir(), "d3master.json")
	contents := `{"listen":"0.0.0.0:65535","web_listen":"0.0.0.0:42001","signaling_listen":"0.0.0.0:42002","relay":"0.0.0.0:42003"}`
	if err := os.WriteFile(path, []byte(contents), 0600); err != nil {
		t.Fatal(err)
	}
	opts, err := readOptions(nil, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.Listen != "0.0.0.0:65535" || opts.WebListen != "0.0.0.0:42001" || opts.SignalingListen != "0.0.0.0:42002" || opts.Relay != "0.0.0.0:42003" {
		t.Fatalf("custom ports were not loaded: %+v", opts)
	}
	opts, err = readOptions([]string{"-signaling-listen", "0.0.0.0:43000", "-web-listen", "0.0.0.0:43001"}, path, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if opts.SignalingListen != "0.0.0.0:43000" || opts.WebListen != "0.0.0.0:43001" {
		t.Fatalf("custom port flags were not applied: %+v", opts)
	}
}

func TestHelpAndBadArgumentsDoNotCreateConfig(t *testing.T) {
	for _, args := range [][]string{{"-help"}, {"-unknown"}, {"unexpected"}} {
		path := filepath.Join(t.TempDir(), "d3master.json")
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
