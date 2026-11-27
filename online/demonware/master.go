// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

// defaultIdentityCache bounds how long a closed PSN session can still be used
// by an established lobby. The native lobby calls this for every task, so an
// uncached lookup would put an HTTP round trip in the gameplay path.
const defaultIdentityCache = time.Second

// MasterResolver resolves a private PSN master session through the master's
// /internal/identity route. The session token itself authenticates the lookup;
// no additional shared secret or account database is involved.
type MasterResolver struct {
	endpoint string
	client   *http.Client
	logger   *slog.Logger
	now      func() time.Time
	// TTL > 0 caches live identities. Zero checks every call; a negative value
	// is treated as zero.
	TTL time.Duration

	mu    sync.Mutex
	cache map[string]cachedIdentity
}

type cachedIdentity struct {
	identity Identity
	expires  time.Time
}

// NewMasterResolver validates the PSN master's HTTP base URL. A trailing slash
// is accepted; paths, queries, credentials and fragments are not.
func NewMasterResolver(endpoint string, client *http.Client) (*MasterResolver, error) {
	base, err := url.Parse(strings.TrimSuffix(strings.TrimSpace(endpoint), "/"))
	if err != nil || base.Scheme != "http" && base.Scheme != "https" || base.Host == "" ||
		base.User != nil || base.Path != "" || base.RawQuery != "" || base.Fragment != "" {
		return nil, errors.New("master URL must be an http:// or https:// base URL without a path")
	}
	if client == nil {
		client = &http.Client{Timeout: 3 * time.Second}
	}
	return &MasterResolver{endpoint: base.String(), client: client, logger: slog.Default(),
		now: time.Now, TTL: defaultIdentityCache, cache: make(map[string]cachedIdentity)}, nil
}

// Resolve reports the live account and display name for a master session
// token. Failures and expired sessions are not cached, so a reconnect can be
// observed immediately once a positive entry ages out.
func (r *MasterResolver) Resolve(ctx context.Context, token string) (Identity, bool) {
	if r == nil || !validSessionToken(token) {
		return Identity{}, false
	}
	now := r.now()
	if r.TTL > 0 {
		r.mu.Lock()
		entry, ok := r.cache[token]
		r.mu.Unlock()
		if ok && now.Before(entry.expires) {
			return entry.identity, true
		}
	}
	identity, status, err := r.lookup(ctx, token)
	if err != nil {
		if r.logger != nil {
			r.logger.Debug("private master identity lookup failed", "error", err)
		}
		return Identity{}, false
	}
	if identity.AccountID == 0 || len(identity.Name) < 1 || len(identity.Name) > 16 ||
		strings.IndexByte(identity.Name, 0) >= 0 {
		if r.logger != nil {
			r.logger.Warn("private master returned an invalid identity", "status", status)
		}
		return Identity{}, false
	}
	if r.TTL > 0 {
		expires := now.Add(r.TTL)
		r.mu.Lock()
		r.store(token, cachedIdentity{identity: identity, expires: expires}, now)
		r.mu.Unlock()
	}
	return identity, true
}

func (r *MasterResolver) lookup(ctx context.Context, token string) (Identity, int, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, r.endpoint+"/internal/identity", nil)
	if err != nil {
		return Identity{}, 0, err
	}
	request.Header.Set("Authorization", "Bearer "+token)
	response, err := r.client.Do(request)
	if err != nil {
		return Identity{}, 0, err
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, 4096))
	if err != nil {
		return Identity{}, response.StatusCode, err
	}
	if response.StatusCode != http.StatusOK {
		return Identity{}, response.StatusCode, errors.New("master rejected the session")
	}
	var identity struct {
		AccountID uint64 `json:"account_id"`
		Name      string `json:"name"`
	}
	decoder := json.NewDecoder(bytes.NewReader(body))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&identity); err != nil || decoder.Decode(new(any)) != io.EOF {
		return Identity{}, response.StatusCode, errors.New("invalid identity response")
	}
	return Identity{AccountID: identity.AccountID, Name: identity.Name}, response.StatusCode, nil
}

// store replaces expired entries before inserting. The cache is bounded by the
// number of live lobby sessions; a full cache is cleared rather than growing.
func (r *MasterResolver) store(token string, entry cachedIdentity, now time.Time) {
	const limit = 4096
	if len(r.cache) >= limit {
		for key, value := range r.cache {
			if !now.Before(value.expires) {
				delete(r.cache, key)
			}
		}
		if len(r.cache) >= limit {
			clear(r.cache)
		}
	}
	r.cache[token] = entry
}

func validSessionToken(token string) bool {
	if len(token) != 64 {
		return false
	}
	for _, c := range token {
		if c >= '0' && c <= '9' || c >= 'a' && c <= 'f' {
			continue
		}
		return false
	}
	return true
}

// Close releases idle HTTP connections to the PSN master.
func (r *MasterResolver) Close() {
	if r != nil && r.client != nil {
		r.client.CloseIdleConnections()
	}
}
