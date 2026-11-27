// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestMasterResolverValidatesEndpoint(t *testing.T) {
	for _, endpoint := range []string{"", "master.example:31315", "ftp://master", "http://user:pass@master", "http://master/path", "http://master?x=1", "http://master#x"} {
		if _, err := NewMasterResolver(endpoint, nil); err == nil {
			t.Fatalf("accepted %q", endpoint)
		}
	}
	if _, err := NewMasterResolver("http://master:31315/", nil); err != nil {
		t.Fatal(err)
	}
}

func TestMasterResolverResolvesAndCachesLiveSessions(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.Method != http.MethodPost || r.URL.Path != "/internal/identity" || r.Header.Get("Authorization") != "Bearer "+strings.Repeat("a", 64) {
			t.Errorf("unexpected request %s %s %q", r.Method, r.URL.Path, r.Header.Get("Authorization"))
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"account_id":42,"name":"Alice"}`))
	}))
	defer server.Close()
	r, err := NewMasterResolver(server.URL, nil)
	if err != nil {
		t.Fatal(err)
	}
	token := strings.Repeat("a", 64)
	identity, ok := r.Resolve(context.Background(), token)
	if !ok || identity != (Identity{AccountID: 42, Name: "Alice"}) {
		t.Fatalf("resolution failed: %+v %v", identity, ok)
	}
	if _, ok := r.Resolve(context.Background(), token); !ok {
		t.Fatal("cached resolution failed")
	}
	if calls.Load() != 1 {
		t.Fatalf("cached identity made %d requests", calls.Load())
	}
	// Expiry is checked with the resolver clock.
	now := time.Now()
	r.now = func() time.Time { return now.Add(time.Hour) }
	if _, ok := r.Resolve(context.Background(), token); !ok || calls.Load() != 2 {
		t.Fatalf("expired identity was not re-resolved: %d", calls.Load())
	}
	r.TTL = 0
	if _, ok := r.Resolve(context.Background(), token); !ok || calls.Load() != 3 {
		t.Fatalf("uncached resolution did not refresh: %d", calls.Load())
	}
}

func TestMasterResolverRejectsUnusableTokensAndResponses(t *testing.T) {
	var calls atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		switch r.Header.Get("Authorization") {
		case "Bearer " + strings.Repeat("b", 64):
			w.WriteHeader(http.StatusUnauthorized)
		default:
			_, _ = w.Write([]byte(`{"account_id":0,"name":""}`))
		}
	}))
	defer server.Close()
	r, err := NewMasterResolver(server.URL, nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, token := range []string{"", "abc", strings.Repeat("A", 64), strings.Repeat("x", 64)} {
		if _, ok := r.Resolve(context.Background(), token); ok {
			t.Fatalf("accepted token %q", token)
		}
	}
	if calls.Load() != 0 {
		t.Fatalf("invalid tokens reached the master: %d", calls.Load())
	}
	if _, ok := r.Resolve(context.Background(), strings.Repeat("b", 64)); ok {
		t.Fatal("accepted an unauthorized session")
	}
	if _, ok := r.Resolve(context.Background(), strings.Repeat("c", 64)); ok {
		t.Fatal("accepted an invalid identity")
	}
	server.Close()
	if _, ok := r.Resolve(context.Background(), strings.Repeat("d", 64)); ok {
		t.Fatal("accepted a failed request")
	}
}

func TestMasterResolverCacheIsBoundedAndPurged(t *testing.T) {
	r, err := NewMasterResolver("http://127.0.0.1:1", nil)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	r.mu.Lock()
	for i := 0; i < 5000; i++ {
		r.store(strings.Repeat("0", 60)+string(rune('a'+i%26)), cachedIdentity{identity: Identity{AccountID: uint64(i + 1), Name: "Player"}, expires: now.Add(time.Hour)}, now)
	}
	size := len(r.cache)
	r.mu.Unlock()
	if size > 4096 || size == 0 {
		t.Fatalf("cache size %d", size)
	}
}
