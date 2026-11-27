// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
)

// IdentityHandler exposes a live master session to the separate Demonware
// server. The caller presents the session token itself, so this route adds no
// shared secret and cannot enumerate names or accounts without a token. It
// returns the same identity the pre-split combined server resolved in process.
func (s *Server) IdentityHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path != "/internal/identity" {
			identityError(w, http.StatusNotFound, "unsupported internal route")
			return
		}
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", "POST")
			identityError(w, http.StatusMethodNotAllowed, "POST required")
			return
		}
		const prefix = "Bearer "
		header := r.Header.Get("Authorization")
		token := strings.TrimPrefix(header, prefix)
		if !strings.HasPrefix(header, prefix) || len(token) != 64 {
			identityError(w, http.StatusUnauthorized, "active master session required")
			return
		}
		account, name, ok := s.ResolvePrivateIdentity(token)
		if !ok {
			slog.Warn("PSN internal identity refused", "remote", r.RemoteAddr)
			identityError(w, http.StatusUnauthorized, "active master session required")
			return
		}
		slog.Info("PSN internal identity accepted", "remote", r.RemoteAddr, "player", name)
		_ = json.NewEncoder(w).Encode(struct {
			AccountID uint64 `json:"account_id"`
			Name      string `json:"name"`
		}{account, name})
	})
}

func identityError(w http.ResponseWriter, status int, message string) {
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": message})
}
