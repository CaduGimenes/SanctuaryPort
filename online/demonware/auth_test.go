// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http/httptest"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

const testToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func authFixture(t *testing.T) (*AuthService, *bool, *bool) {
	t.Helper()
	ready, live := true, true
	s, err := NewAuthService(AuthConfig{TitleID: 5464, ReplyTask: 47, SessionKeyBytes: 24, Lifetime: time.Minute,
		Resolve: func(ctx context.Context, token string) (Identity, bool) {
			return Identity{42, "Alice"}, live && token == testToken
		},
		Ready: func() bool { return ready }, EncodeClientTicket: func(in TicketInput) ([]byte, error) {
			// This is a test callback, not a claim about the native ticket layout.
			out := make([]byte, 128)
			copy(out[:], in.SessionKey)
			return out, nil
		}})
	if err != nil {
		t.Fatal(err)
	}
	return s, &ready, &live
}
func authBody(t *testing.T, task, title uint32, token string) []byte {
	t.Helper()
	duid := sha256.Sum256([]byte("d3pc:Alice"))
	claims, _ := json.Marshal(map[string]any{"iss": "d3pc", "sub": "42", "d3pc_session": token, "env_iss_id": 256, "duid": hex.EncodeToString(duid[:]), "iat": time.Now().Unix(), "exp": time.Now().Add(time.Hour).Unix()})
	header := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	input := header + "." + base64.RawURLEncoding.EncodeToString(claims)
	mac := hmac.New(sha256.New, []byte(token))
	mac.Write([]byte(input))
	jwt := input + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	inner, _ := json.Marshal(map[string]any{"scope": "openid,openid:duid", "id_token": jwt})
	extra, _ := json.Marshal(map[string]any{"token": string(inner), "online_id": "Alice", "extended_data": true})
	body, err := json.Marshal(map[string]any{"auth_task": task, "title_id": title, "iv_seed": uint32(0xfedcba98), "identity": "356c4bc3", "extra_data": string(extra)})
	if err != nil {
		t.Fatal(err)
	}
	return body
}
func authCall(s *AuthService, body []byte) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	s.ServeHTTP(w, httptest.NewRequest("POST", "/auth/", bytes.NewReader(body)))
	return w
}
func authTicket(t *testing.T, w *httptest.ResponseRecorder) []byte {
	t.Helper()
	if w.Code != 200 {
		t.Fatalf("auth failed: %d %s", w.Code, w.Body.String())
	}
	var reply map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &reply); err != nil {
		t.Fatal(err)
	}
	if reply["code"] != float64(700) || reply["auth_task"] != float64(47) || reply["iv_seed"] != float64(0xfedcba98) {
		t.Fatal(reply)
	}
	client, err := base64.StdEncoding.DecodeString(reply["client_ticket"].(string))
	if err != nil || len(client) != 128 {
		t.Fatal("invalid client ticket")
	}
	ticket, err := base64.StdEncoding.DecodeString(reply["server_ticket"].(string))
	if err != nil || len(ticket) != 128 {
		t.Fatal("invalid server ticket")
	}
	return ticket
}
func TestPrivateAuthSignedReplyAndTicketLifetime(t *testing.T) {
	s, _, live := authFixture(t)
	w := authCall(s, authBody(t, 46, 5464, testToken))
	ticket := authTicket(t, w)
	mac := hmac.New(sha256.New, []byte(testToken))
	mac.Write(w.Body.Bytes())
	if !hmac.Equal([]byte(w.Header().Get("X-D3-Signature")), []byte(hex.EncodeToString(mac.Sum(nil)))) {
		t.Fatal("response signature does not cover exact body")
	}
	session, ok := s.Lookup(ticket)
	if !ok || session.Identity.AccountID != 42 || session.Identity.Name != "Alice" || len(session.Key) != 24 {
		t.Fatal("issued session lost")
	}
	session.Key[0] ^= 0xff
	unchanged, _ := s.Lookup(ticket)
	if unchanged.Key[0] == session.Key[0] {
		t.Fatal("session lookup aliases key storage")
	}
	other := append([]byte(nil), ticket...)
	other[0] ^= 1
	if _, ok = s.Lookup(other); ok {
		t.Fatal("modified ticket accepted")
	}
	*live = false
	if _, ok = s.Lookup(ticket); ok {
		t.Fatal("disconnected master session accepted")
	}
	*live = true
	ticket = authTicket(t, authCall(s, authBody(t, 46, 5464, testToken)))
	s.now = func() time.Time { return time.Now().Add(2 * time.Minute) }
	if _, ok = s.Lookup(ticket); ok {
		t.Fatal("expired ticket accepted")
	}
}
func TestAuthRequiresLiveIdentityAndReadyLobby(t *testing.T) {
	s, ready, _ := authFixture(t)
	*ready = false
	if w := authCall(s, authBody(t, 46, 5464, testToken)); w.Code != 503 {
		t.Fatal("unready lobby reported auth success")
	}
	if len(s.sessions) != 0 {
		t.Fatal("unready lobby issued session")
	}
	*ready = true
	for _, body := range [][]byte{nil, []byte("{}"), authBody(t, 45, 5464, testToken), authBody(t, 46, 1, testToken), bytes.Repeat([]byte("x"), 16385)} {
		if w := authCall(s, body); w.Code != 400 {
			t.Fatalf("invalid request accepted: %d", w.Code)
		}
	}
	if w := authCall(s, authBody(t, 46, 5464, strings.Repeat("f", 64))); w.Code != 401 {
		t.Fatal("unknown token accepted")
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, httptest.NewRequest("GET", "/auth/", nil))
	if w.Code != 405 {
		t.Fatal("GET accepted")
	}
	s.random = io.LimitReader(bytes.NewReader([]byte{1}), 1)
	if w := authCall(s, authBody(t, 46, 5464, testToken)); w.Code != 503 {
		t.Fatal("failed entropy reported success")
	}
}

func TestNativeAuthQuotedIntegerFields(t *testing.T) {
	// de42b0 passes quote=true to the game's ea00a0 JSON integer writer
	// for all three fields. Keep this independent of the server's decoder.
	for _, seed := range []string{"0", "2147483648", "4294967295"} {
		t.Run(seed, func(t *testing.T) {
			s, _, _ := authFixture(t)
			var body map[string]json.RawMessage
			if err := json.Unmarshal(authBody(t, 46, D3TitleID, testToken), &body); err != nil {
				t.Fatal(err)
			}
			body["auth_task"] = json.RawMessage(`"46"`)
			body["title_id"] = json.RawMessage(`"5464"`)
			body["iv_seed"] = json.RawMessage(`"` + seed + `"`)
			encoded, _ := json.Marshal(body)
			w := authCall(s, encoded)
			if w.Code != 200 {
				t.Fatalf("native quoted integers rejected: %d %s", w.Code, w.Body.String())
			}
			var reply map[string]json.RawMessage
			if err := json.Unmarshal(w.Body.Bytes(), &reply); err != nil {
				t.Fatal(err)
			}
			if string(reply["iv_seed"]) != seed {
				t.Fatalf("seed changed: got %s, want %s", reply["iv_seed"], seed)
			}
		})
	}
}

func TestAuthIntegerFieldsRejectInvalidValues(t *testing.T) {
	s, _, _ := authFixture(t)
	for _, field := range []string{"auth_task", "title_id", "iv_seed"} {
		for _, value := range []string{`null`, `true`, `[]`, `{}`, `-1`, `1.5`, `1e2`, `4294967296`, `"4294967296"`, `"-1"`, `"+46"`, `"1.5"`, `"1e2"`, `""`, `" 46 "`} {
			t.Run(field+"/"+value, func(t *testing.T) {
				var body map[string]json.RawMessage
				if err := json.Unmarshal(authBody(t, 46, D3TitleID, testToken), &body); err != nil {
					t.Fatal(err)
				}
				body[field] = json.RawMessage(value)
				encoded, _ := json.Marshal(body)
				if w := authCall(s, encoded); w.Code != 400 {
					t.Fatalf("invalid %s accepted: %d", field, w.Code)
				}
				if len(s.sessions) != 0 {
					t.Fatal("invalid request issued a session")
				}
			})
		}
	}
}
func TestAuthReplacesOldTicketAndBoundsRegistry(t *testing.T) {
	s, _, _ := authFixture(t)
	old := authTicket(t, authCall(s, authBody(t, 46, 5464, testToken)))
	newTicket := authTicket(t, authCall(s, authBody(t, 46, 5464, testToken)))
	if bytes.Equal(old, newTicket) {
		t.Fatal("server ticket reused")
	}
	if _, ok := s.Lookup(old); ok {
		t.Fatal("old ticket survived replacement")
	}
	if len(s.sessions) != 1 {
		t.Fatal("auth retries grew session registry")
	}
	if _, err := NewAuthService(AuthConfig{}); err == nil {
		t.Fatal("unconfigured service accepted")
	}
}

func TestEstablishedLobbyUsesLiveMasterIdentity(t *testing.T) {
	s, _, live := authFixture(t)
	ticket := authTicket(t, authCall(s, authBody(t, 46, D3TitleID, testToken)))
	session, ok := s.Lookup(ticket)
	if !ok {
		t.Fatal("missing session")
	}
	s.now = func() time.Time { return session.Expires.Add(time.Second) }
	if _, ok := s.Lookup(ticket); ok {
		t.Fatal("expired ticket accepted for a new handshake")
	}
	if !s.IsLive(session) {
		t.Fatal("bootstrap expiry disconnected a live authenticated lobby")
	}
	*live = false
	if s.IsLive(session) {
		t.Fatal("master logout retained lobby authentication")
	}
}

func TestPackedClientTicket(t *testing.T) {
	key := bytes.Repeat([]byte{0x9a}, 24)
	issued := time.Unix(1700000000, 0)
	b, err := EncodeClientTicket(TicketInput{Identity: Identity{0x1122334455667788, "Alice"}, TitleID: D3TitleID, SessionKey: key, Issued: issued, Expires: issued.Add(time.Hour)})
	if err != nil || len(b) != 128 {
		t.Fatal(err)
	}
	// These offsets come from the game's unaligned bdAuthTicket deserializer.
	if !bytes.Equal(b[:4], []byte{0xde, 0xad, 0xbd, 0xef}) || !bytes.Equal(b[5:9], []byte{0x58, 0x15, 0, 0}) || !bytes.Equal(b[25:33], []byte{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}) || string(b[33:39]) != "Alice\x00" || !bytes.Equal(b[97:121], key) || !bytes.Equal(b[121:124], []byte{0x55, 0x33, 0x22}) {
		t.Fatal("native client ticket field offsets changed")
	}
	if _, err = EncodeClientTicket(TicketInput{}); err == nil {
		t.Fatal("invalid client ticket accepted")
	}
}

func TestNativeIdentityTokenAcceptedByPrivateAuth(t *testing.T) {
	fixture := os.Getenv("D3_NATIVE_TRANSPORT_TEST")
	if fixture == "" {
		t.Skip("native fixture not selected")
	}
	input, _ := json.Marshal(map[string]any{"account_id": uint64(42), "name": "Alice", "bearer": testToken, "now": time.Now().Unix()})
	cmd := exec.Command(fixture, "--id-token")
	cmd.Stdin = bytes.NewReader(input)
	jwt, err := cmd.Output()
	if err != nil {
		t.Fatal("native identity fixture failed", err)
	}
	inner, _ := json.Marshal(map[string]any{"scope": "openid,openid:duid", "id_token": strings.TrimSpace(string(jwt))})
	extra, _ := json.Marshal(map[string]any{"token": string(inner), "online_id": "Alice", "extended_data": true})
	body, _ := json.Marshal(map[string]any{"auth_task": 46, "title_id": D3TitleID, "iv_seed": 42, "identity": "356c4bc3", "extra_data": string(extra)})
	s, _, _ := authFixture(t)
	w := authCall(s, body)
	if w.Code != 200 {
		t.Fatalf("native JWT rejected: %d %s", w.Code, w.Body.String())
	}
	var reply map[string]any
	if json.Unmarshal(w.Body.Bytes(), &reply) != nil || reply["identity"] != true || reply["extra_data"] != `{"extended_data":""}` {
		t.Fatal("native auth response shape mismatch")
	}
	// A token modified after native signing must never issue a server ticket.
	parts := strings.Split(strings.TrimSpace(string(jwt)), ".")
	parts[2] = "AAAA"
	inner, _ = json.Marshal(map[string]any{"scope": "openid,openid:duid", "id_token": strings.Join(parts, ".")})
	extra, _ = json.Marshal(map[string]any{"token": string(inner), "online_id": "Alice", "extended_data": true})
	body, _ = json.Marshal(map[string]any{"auth_task": 46, "title_id": D3TitleID, "iv_seed": 42, "identity": "356c4bc3", "extra_data": string(extra)})
	if w = authCall(s, body); w.Code != 401 {
		t.Fatal("modified native JWT accepted")
	}
}
