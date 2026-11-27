// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func identityCall(s *Server, method, path, token string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, nil)
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	s.IdentityHandler().ServeHTTP(w, r)
	return w
}

func TestIdentityHandlerResolvesOnlyLiveSessions(t *testing.T) {
	s := webFixture(t)
	alice := login(t, s, "Alice")
	if w := identityCall(s, http.MethodGet, "/internal/identity", alice.token); w.Code != http.StatusMethodNotAllowed || w.Header().Get("Allow") != "POST" {
		t.Fatalf("GET: %d %q %s", w.Code, w.Header().Get("Allow"), w.Body.String())
	}
	if w := identityCall(s, http.MethodPost, "/internal/other", alice.token); w.Code != http.StatusNotFound {
		t.Fatalf("wrong path: %d %s", w.Code, w.Body.String())
	}
	for _, token := range []string{"", strings.Repeat("0", 64), strings.Repeat("0", 63)} {
		if w := identityCall(s, http.MethodPost, "/internal/identity", token); w.Code != http.StatusUnauthorized {
			t.Fatalf("token %q: %d %s", token, w.Code, w.Body.String())
		}
	}
	w := identityCall(s, http.MethodPost, "/internal/identity", alice.token)
	if w.Code != http.StatusOK {
		t.Fatalf("live session: %d %s", w.Code, w.Body.String())
	}
	var identity struct {
		AccountID uint64 `json:"account_id"`
		Name      string `json:"name"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &identity); err != nil || identity.AccountID == 0 || identity.Name != "Alice" {
		t.Fatalf("identity %+v (%v)", identity, err)
	}
	if w.Header().Get("Cache-Control") != "no-store" {
		t.Fatal("identity response is cacheable")
	}
	alice.conn.Close()
	deadline := time.Now().Add(time.Second)
	for identityCall(s, http.MethodPost, "/internal/identity", alice.token).Code != http.StatusUnauthorized {
		if time.Now().After(deadline) {
			t.Fatal("closed master session still resolved")
		}
		time.Sleep(time.Millisecond)
	}
}
