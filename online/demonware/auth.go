// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

const D3TitleID = 5464

// The native auth builder (de42b0) asks its JSON writer (ea00a0) to
// quote these unsigned decimal fields. Also accept JSON numbers used by
// existing private clients, without accepting null, signs or fractional values.
type authUint32 uint32

func (v *authUint32) UnmarshalJSON(data []byte) error {
	value := string(data)
	if len(data) > 0 && data[0] == '"' {
		if err := json.Unmarshal(data, &value); err != nil {
			return ErrValue
		}
	}
	if len(value) == 0 || len(value) > 10 {
		return ErrValue
	}
	for _, c := range value {
		if c < '0' || c > '9' {
			return ErrValue
		}
	}
	n, err := strconv.ParseUint(value, 10, 32)
	if err != nil {
		return ErrValue
	}
	*v = authUint32(n)
	return nil
}

// EncodeClientTicket implements this build's packed 128-byte bdAuthTicket:
// fields are unaligned on the wire. The native parser recognizes the plaintext
// magic before its legacy ticket decryption branch. The private response HMAC
// is verified by the runtime before the game receives these bytes.
func EncodeClientTicket(in TicketInput) ([]byte, error) {
	if len(in.SessionKey) != 24 || in.TitleID != D3TitleID || in.Identity.AccountID == 0 || len(in.Identity.Name) < 1 || len(in.Identity.Name) > 16 || strings.IndexByte(in.Identity.Name, 0) >= 0 || in.Issued.Unix() < 0 || in.Expires.Unix() > int64(^uint32(0)) || !in.Expires.After(in.Issued) {
		return nil, ErrValue
	}
	b := make([]byte, 128)
	binary.LittleEndian.PutUint32(b, 0xefbdadde)
	binary.LittleEndian.PutUint32(b[5:], in.TitleID)
	binary.LittleEndian.PutUint32(b[9:], uint32(in.Issued.Unix()))
	binary.LittleEndian.PutUint32(b[13:], uint32(in.Expires.Unix()))
	binary.LittleEndian.PutUint64(b[17:], in.Identity.AccountID)
	binary.LittleEndian.PutUint64(b[25:], in.Identity.AccountID)
	copy(b[33:97], in.Identity.Name)
	copy(b[97:121], in.SessionKey)
	copy(b[121:124], []byte{0x55, 0x33, 0x22})
	return b, nil
}

// Identity comes from the already authenticated, live private master session.
// The HTTP endpoint never provisions an account or accepts a second password.
type Identity struct {
	AccountID uint64
	Name      string
}

// TicketInput is the authenticated state to serialize into the game's client
// ticket. Ticket encoding is supplied only after that build's layout is known.
type TicketInput struct {
	Identity        Identity
	TitleID         uint32
	SessionKey      []byte
	Issued, Expires time.Time
}

type AuthConfig struct {
	Resolve func(context.Context, string) (Identity, bool)
	// Ready must confirm a working private lobby handler, not just master login.
	Ready              func() bool
	EncodeClientTicket func(TicketInput) ([]byte, error)
	TitleID, ReplyTask uint32
	SessionKeyBytes    int
	MaxSessions        int
	Lifetime           time.Duration
}

// Session binds a server ticket and key to one master identity. Server tickets
// are opaque random values; Sony credentials and fixed shared keys are unused.
type Session struct {
	Identity        Identity
	TitleID         uint32
	Key             []byte
	Ticket          [128]byte
	Issued, Expires time.Time
	masterToken     string
}

type AuthService struct {
	cfg      AuthConfig
	mu       sync.Mutex
	sessions map[[32]byte]Session
	now      func() time.Time
	random   io.Reader
}

func NewAuthService(cfg AuthConfig) (*AuthService, error) {
	if cfg.TitleID == 0 {
		cfg.TitleID = D3TitleID
	}
	if cfg.ReplyTask == 0 {
		cfg.ReplyTask = 47
	}
	if cfg.SessionKeyBytes == 0 {
		cfg.SessionKeyBytes = 24
	}
	if cfg.EncodeClientTicket == nil {
		cfg.EncodeClientTicket = EncodeClientTicket
	}
	if cfg.Resolve == nil || cfg.Ready == nil || cfg.TitleID != D3TitleID || cfg.ReplyTask != 47 || cfg.SessionKeyBytes != 24 {
		return nil, errors.New("incomplete private Demonware authentication configuration")
	}
	if cfg.MaxSessions <= 0 {
		cfg.MaxSessions = 128
	}
	if cfg.Lifetime <= 0 {
		cfg.Lifetime = time.Hour
	}
	return &AuthService{cfg: cfg, sessions: make(map[[32]byte]Session), now: time.Now, random: rand.Reader}, nil
}

// Lookup validates the exact 128-byte opaque ticket presented by the lobby.
// It returns independent storage so a connection cannot mutate registry state.
func (s *AuthService) Lookup(ticket []byte) (Session, bool) {
	if len(ticket) != 128 {
		return Session{}, false
	}
	s.mu.Lock()
	key := sha256.Sum256(ticket)
	session, ok := s.sessions[key]
	if !ok {
		s.mu.Unlock()
		return Session{}, false
	}
	if !s.now().Before(session.Expires) {
		delete(s.sessions, key)
		s.mu.Unlock()
		return Session{}, false
	}
	s.mu.Unlock()
	identity, live := s.cfg.Resolve(context.Background(), session.masterToken)
	if !live || identity != session.Identity {
		s.mu.Lock()
		delete(s.sessions, key)
		s.mu.Unlock()
		return Session{}, false
	}
	session.Key = append([]byte(nil), session.Key...)
	return session, true
}

// Ticket expiry limits new lobby handshakes. An established encrypted lobby
// stays authenticated while its original master login is live; an expiring
// bootstrap JWT must not disconnect an ongoing game every fifteen minutes.
func (s *AuthService) IsLive(session Session) bool {
	if session.Identity.AccountID == 0 || session.TitleID != s.cfg.TitleID {
		return false
	}
	identity, ok := s.cfg.Resolve(context.Background(), session.masterToken)
	return ok && identity == session.Identity
}

func (s *AuthService) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	slog.Info("Demonware title auth request", "remote", r.RemoteAddr, "path", r.URL.Path, "method", r.Method)
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "application/json")
	if r.URL.Path != "/auth/" {
		authError(w, 404, "unsupported title-service path")
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		authError(w, 405, "POST required")
		return
	}
	var request struct {
		Task      authUint32 `json:"auth_task"`
		IVSeed    authUint32 `json:"iv_seed"`
		TitleID   authUint32 `json:"title_id"`
		Identity  string     `json:"identity"`
		ExtraData string     `json:"extra_data"`
	}
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16384))
	dec.DisallowUnknownFields()
	if dec.Decode(&request) != nil || dec.Decode(new(any)) != io.EOF || request.Task != 46 || uint32(request.TitleID) != s.cfg.TitleID || request.Identity != "356c4bc3" {
		authError(w, 400, "invalid title authentication request")
		return
	}
	var extra struct {
		Token    string `json:"token"`
		OnlineID string `json:"online_id"`
		Extended bool   `json:"extended_data"`
	}
	var token struct {
		Scope string `json:"scope"`
		Token string `json:"id_token"`
	}
	if json.Unmarshal([]byte(request.ExtraData), &extra) != nil || json.Unmarshal([]byte(extra.Token), &token) != nil || token.Scope != "openid,openid:duid" {
		authError(w, 400, "invalid private identity token")
		return
	}
	identity, masterToken, tokenExpiry, err := s.identityToken(r.Context(), token.Token)
	if err != nil || extra.OnlineID != identity.Name {
		slog.Warn("Demonware title auth identity mismatch", "err", err, "token_online_id", extra.OnlineID,
			"master_name", identity.Name, "master_account", identity.AccountID)
		authError(w, 401, "active master session required")
		return
	}
	if !s.cfg.Ready() {
		authError(w, 503, "private lobby service unavailable")
		return
	}
	now := s.now().UTC()
	expires := now.Add(s.cfg.Lifetime)
	if tokenExpiry.Before(expires) {
		expires = tokenExpiry
	}
	session := Session{Identity: identity, TitleID: uint32(request.TitleID), Issued: now, Expires: expires, Key: make([]byte, s.cfg.SessionKeyBytes), masterToken: masterToken}
	if _, err := io.ReadFull(s.random, session.Key); err != nil {
		authError(w, 503, "session generation unavailable")
		return
	}
	if _, err := io.ReadFull(s.random, session.Ticket[:]); err != nil {
		authError(w, 503, "session generation unavailable")
		return
	}
	clientTicket, err := s.cfg.EncodeClientTicket(TicketInput{Identity: identity, TitleID: session.TitleID, SessionKey: append([]byte(nil), session.Key...), Issued: session.Issued, Expires: session.Expires})
	if err != nil || len(clientTicket) != 128 {
		authError(w, 503, "client ticket unavailable")
		return
	}
	reply := struct {
		Version      uint32 `json:"version"`
		Code         uint32 `json:"code"`
		Task         uint32 `json:"auth_task"`
		IVSeed       uint32 `json:"iv_seed"`
		ClientTicket string `json:"client_ticket"`
		ServerTicket string `json:"server_ticket"`
		Identity     bool   `json:"identity"`
		ExtraData    string `json:"extra_data"`
	}{1, 700, s.cfg.ReplyTask, uint32(request.IVSeed), base64.StdEncoding.EncodeToString(clientTicket), base64.StdEncoding.EncodeToString(session.Ticket[:]), true, `{"extended_data":""}`}
	body, err := json.Marshal(reply)
	if err != nil {
		authError(w, 503, "authentication response unavailable")
		return
	}
	s.mu.Lock()
	for key, old := range s.sessions {
		if !now.Before(old.Expires) || old.Identity.AccountID == identity.AccountID {
			delete(s.sessions, key)
		}
	}
	if len(s.sessions) >= s.cfg.MaxSessions {
		s.mu.Unlock()
		authError(w, 503, "title service capacity reached")
		return
	}
	s.sessions[sha256.Sum256(session.Ticket[:])] = session
	s.mu.Unlock()
	slog.Info("Demonware title auth accepted", "player", identity.Name, "title_id", session.TitleID)
	signature := hmac.New(sha256.New, []byte(masterToken))
	_, _ = signature.Write(body)
	w.Header().Set("X-D3-Signature", hex.EncodeToString(signature.Sum(nil)))
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

func (s *AuthService) identityToken(ctx context.Context, token string) (Identity, string, time.Time, error) {
	invalid := func() (Identity, string, time.Time, error) { return Identity{}, "", time.Time{}, ErrValue }
	if len(token) > 8192 {
		return invalid()
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return invalid()
	}
	header, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return invalid()
	}
	var h struct {
		Algorithm string `json:"alg"`
	}
	if json.Unmarshal(header, &h) != nil || h.Algorithm != "HS256" {
		return invalid()
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return invalid()
	}
	var claims struct {
		Issuer   string `json:"iss"`
		Subject  string `json:"sub"`
		Session  string `json:"d3pc_session"`
		IssuerID uint32 `json:"env_iss_id"`
		DUID     string `json:"duid"`
		Issued   int64  `json:"iat"`
		Expires  int64  `json:"exp"`
	}
	if json.Unmarshal(payload, &claims) != nil || claims.Issuer != "d3pc" || claims.IssuerID != 256 || len(claims.Session) != 64 || claims.Expires <= s.now().Unix() || claims.Issued > s.now().Add(5*time.Minute).Unix() || claims.Expires <= claims.Issued {
		slog.Warn("Demonware token rejected", "stage", "claims", "issuer", claims.Issuer,
			"session_len", len(claims.Session), "iat", claims.Issued, "exp", claims.Expires)
		return invalid()
	}
	identity, ok := s.cfg.Resolve(ctx, claims.Session)
	if !ok || identity.AccountID == 0 || len(identity.Name) < 1 || len(identity.Name) > 16 || claims.Subject != strconv.FormatUint(identity.AccountID, 10) {
		slog.Warn("Demonware token rejected", "stage", "identity", "resolved", ok,
			"account", identity.AccountID, "name", identity.Name, "subject", claims.Subject)
		return invalid()
	}
	duid := sha256.Sum256([]byte("d3pc:" + identity.Name))
	if claims.DUID != hex.EncodeToString(duid[:]) {
		slog.Warn("Demonware token rejected", "stage", "duid", "expected", hex.EncodeToString(duid[:]), "got", claims.DUID)
		return invalid()
	}
	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return invalid()
	}
	mac := hmac.New(sha256.New, []byte(claims.Session))
	_, _ = mac.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(signature, mac.Sum(nil)) {
		slog.Warn("Demonware token rejected", "stage", "signature")
		return invalid()
	}
	return identity, claims.Session, time.Unix(claims.Expires, 0), nil
}
func authError(w http.ResponseWriter, status int, message string) {
	slog.Warn("Demonware title auth refused", "status", status, "reason", message)
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": message})
}
