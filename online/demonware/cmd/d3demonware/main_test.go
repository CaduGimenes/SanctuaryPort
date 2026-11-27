// SPDX-License-Identifier: GPL-2.0-or-later
package main

import (
	"net"
	"testing"
)

func TestAdvertisedIPv4(t *testing.T) {
	for _, row := range [][3]string{{"", "127.0.0.1", "127.0.0.1"}, {"203.0.113.4", "0.0.0.0", "203.0.113.4"}, {"198.51.100.8", "master.example", "198.51.100.8"}} {
		got, err := advertisedIPv4(row[0], row[1])
		if err != nil || got.String() != row[2] {
			t.Fatalf("%v: %v %v", row, got, err)
		}
	}
	for _, bad := range []string{"::", "master.example", "::1", "224.0.0.1", "255.255.255.255"} {
		if _, err := advertisedIPv4("", bad); err == nil {
			t.Fatalf("accepted %q", bad)
		}
	}
	if _, err := advertisedIPv4("0.0.0.0", "127.0.0.1"); err == nil {
		t.Fatal("accepted an explicitly unspecified advertised address")
	}
}

func TestLocalAdvertisedIPv4(t *testing.T) {
	addresses := []net.Addr{}
	for _, value := range []string{"127.0.0.1/8", "::1/128", "fe80::1/64", "169.254.1.1/16", "192.168.1.20/24", "10.0.0.2/8"} {
		ip, network, err := net.ParseCIDR(value)
		if err != nil {
			t.Fatal(err)
		}
		network.IP = ip
		addresses = append(addresses, network)
	}
	if got := localAdvertisedIPv4(addresses).String(); got != "192.168.1.20" {
		t.Fatalf("advertised %s, want first usable local IPv4", got)
	}
	if got := localAdvertisedIPv4(addresses[:4]).String(); got != "127.0.0.1" {
		t.Fatalf("advertised %s, want loopback fallback", got)
	}
}
