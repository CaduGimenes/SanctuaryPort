// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"net/textproto"
	"strconv"
	"strings"
	"testing"
	"time"
)

func webFixture(t *testing.T) *Server {
	t.Helper()
	s := New(Config{Logger: slog.New(slog.NewTextHandler(io.Discard, nil))})
	t.Cleanup(s.Close)
	return s
}
func webCall(t *testing.T, s *Server, c *testClient, method, path, ct string, body []byte, status int) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	if c != nil {
		r.Header.Set("Authorization", "Bearer "+c.token)
	}
	if ct != "" {
		r.Header.Set("Content-Type", ct)
	}
	w := httptest.NewRecorder()
	s.WebHandler().ServeHTTP(w, r)
	if w.Code != status {
		t.Fatalf("%s %s: got %d %s, want %d", method, path, w.Code, w.Body.String(), status)
	}
	return w
}
func webDecode(t *testing.T, w *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var out map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &out); err != nil {
		t.Fatal(err)
	}
	return out
}
func TestWebAuthenticationAndLiveProfiles(t *testing.T) {
	s := webFixture(t)
	a := login(t, s, "Alice")
	b := login(t, s, "Bobby")
	webCall(t, s, nil, "GET", "/v1/users/me/friendList", "", nil, 401)
	wrong := &testClient{token: strings.Repeat("0", 64)}
	webCall(t, s, wrong, "GET", "/v1/users/me/friendList", "", nil, 401)
	out := webDecode(t, webCall(t, s, a, "GET", "/v1/users/me/friendList?offset=0&limit=2", "", nil, 200))
	list := out["friendList"].([]any)
	if out["totalResults"] != float64(1) || len(list) != 1 || list[0].(map[string]any)["user"].(map[string]any)["onlineId"] != "Bobby" {
		t.Fatalf("profiles not derived from live players: %#v", out)
	}
	webCall(t, s, a, "PUT", "/v1/users/me/presence/inGamePresence", "application/json", []byte(`{"gameStatus":"Act I","gameData":"AQIDBA=="}`), 204)
	peer := webDecode(t, webCall(t, s, b, "GET", "/v1/users/Alice/presence", "", nil, 200))
	presence := peer["presence"].(map[string]any)
	info := presence["incontextInfoList"].([]any)[0].(map[string]any)
	if info["gameStatus"] != "Act I" || info["gameData"] != "AQIDBA==" {
		t.Fatalf("presence lost: %#v", peer)
	}
	if info["gameTitleInfo"].(map[string]any)["npTitleId"] != "CUSA00242_00" {
		t.Fatal("presence uses the CUSA serial instead of the PSN title ID")
	}
	idBytes, err := base64.StdEncoding.DecodeString(peer["npId"].(string))
	if err != nil || len(idBytes) != 36 || string(bytes.TrimRight(idBytes, "\x00")) != "Alice" {
		t.Fatalf("invalid runtime NP identity: %x, %v", idBytes, err)
	}
	if len(presence["platformInfoList"].([]any)) != 1 {
		t.Fatal("missing platform presence variant")
	}
	webCall(t, s, a, "DELETE", "/v1/users/me/presence/gameStatus", "", nil, 204)
	cleared := webDecode(t, webCall(t, s, b, "GET", "/v1/users/Alice/presence", "", nil, 200))["presence"].(map[string]any)
	clearedInfo := cleared["platformInfoList"].([]any)[0].(map[string]any)
	if clearedInfo["gameStatus"] != "" || clearedInfo["gameData"] != "AQIDBA==" {
		t.Fatal("clearing status changed game data")
	}
	lists := webDecode(t, webCall(t, s, a, "GET", "/v1/friendOnlineIdLists?accountIds=Alice", "", nil, 200))["friendOnlineIdLists"].([]any)
	friends := lists[0].(map[string]any)["friendOnlineIdList"].([]any)
	if len(friends) != 1 || friends[0].(map[string]any)["onlineId"] != "Bobby" {
		t.Fatal("friend identity lists do not match private peers")
	}
	// The shipped Toolkit uses a 500-entry page for its initial friends query.
	webCall(t, s, a, "GET", "/v1/users/me/friendList?limit=500&offset=0", "", nil, 200)
	webCall(t, s, b, "PUT", "/v1/users/Alice/presence/gameStatus", "application/json", []byte(`{"gameStatus":"spoofed"}`), 403)
	webCall(t, s, a, "GET", "/v1/users/me/friendList?limit=99999", "", nil, 400)
	webCall(t, s, a, "POST", "/v1/unknown", "application/json", []byte(`{}`), 404)
	webCall(t, s, a, "GET", "/v1/users/Missing/profile", "", nil, 404)
	s.mu.Lock()
	id := strconv.FormatUint(s.sessions["Bobby"].account, 10)
	s.mu.Unlock()
	out = webDecode(t, webCall(t, s, a, "GET", "/v1/profiles?accountIds="+id, "", nil, 200))
	if out["size"] != float64(1) {
		t.Fatal(out)
	}
	b.conn.Close()
	deadline := time.Now().Add(time.Second)
	for {
		s.mu.Lock()
		gone := s.sessions["Bobby"] == nil
		s.mu.Unlock()
		if gone {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("disconnect did not invalidate HTTP identity")
		}
		time.Sleep(time.Millisecond)
	}
	webCall(t, s, b, "GET", "/v1/users/me/profile", "", nil, 401)
	out = webDecode(t, webCall(t, s, a, "GET", "/v1/users/me/friendList", "", nil, 200))
	if out["totalResults"] != float64(0) {
		t.Fatal(out)
	}
}

func sessionBody(t *testing.T, privacy, kind string) (string, []byte) {
	t.Helper()
	var body bytes.Buffer
	m := multipart.NewWriter(&body)
	parts := []struct {
		name, ct string
		body     []byte
	}{
		{"session-request", "application/json", []byte(fmt.Sprintf(`{"sessionPrivacy":%q,"sessionType":%q,"sessionMaxUser":2,"sessionName":"Act I party","sessionStatus":"In town","availablePlatforms":["PS4"],"sessionLockFlag":false}`, privacy, kind))},
		{"session-data", "application/octet-stream", []byte{0, 1, 2, 3, 255}},
		{"changeable-session-data", "application/octet-stream", []byte("room=1")},
	}
	for _, p := range parts {
		h := textproto.MIMEHeader{}
		h.Set("Content-Type", p.ct)
		h.Set("Content-Description", p.name)
		w, err := m.CreatePart(h)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = w.Write(p.body); err != nil {
			t.Fatal(err)
		}
	}
	if err := m.Close(); err != nil {
		t.Fatal(err)
	}
	return "multipart/mixed; boundary=" + m.Boundary(), body.Bytes()
}
func webCreate(t *testing.T, s *Server, c *testClient, privacy, kind string) string {
	t.Helper()
	ct, b := sessionBody(t, privacy, kind)
	out := webDecode(t, webCall(t, s, c, "POST", "/v1/sessions", ct, b, 201))
	return out["sessionId"].(string)
}

// The shipped Toolkit builds these headers itself (45cfc..45d23), but passes
// the length before appending the closing MIME boundary to SendRequest at
// 533a7..533af. The final attachment is delimited by its Content-Length.
func toolkitSessionBody() (string, []byte) {
	const boundary = "7341db8eb3e39bdb87a19a069106223c"
	description := []byte("{\"sessionPrivacy\":\"public\",\"sessionMaxUser\":4,\"sessionName\":\"Diablo III\",\"sessionStatus\":\"In town\",\"sessionType\":\"owner-bind\",\"availablePlatforms\":[\"PS4\"],\"sessionLockFlag\":false}\r\n")
	var body bytes.Buffer
	fmt.Fprintf(&body, "--%s\r\nContent-Type:application/json; charset=utf-8\r\nContent-Description:session-request\r\nContent-Length:%d\r\n\r\n%s\r\n", boundary, len(description), description)
	for _, part := range []struct {
		name, contentType string
		data              []byte
	}{
		{"session-image", "image/jpeg", []byte{0xff, 0xd8, 0xff, 0xd9}},
		{"session-data", "application/octet-stream", []byte{0, 1, 2, 0xff, 0}},
		{"changeable-session-data", "application/octet-stream", []byte{9, 0, 8}},
	} {
		if part.name != "session-image" {
			body.WriteString("\r\n")
		}
		fmt.Fprintf(&body, "--%s\r\nContent-Type:%s\r\nContent-Description:%s\r\nContent-Disposition:attachment\r\nContent-Length:%d\r\n\r\n", boundary, part.contentType, part.name, len(part.data))
		body.Write(part.data)
	}
	return "multipart/mixed; boundary=\"" + boundary + "\"", body.Bytes()
}

func TestToolkitSessionRequestWithoutClosingBoundary(t *testing.T) {
	s := webFixture(t)
	c := login(t, s, "Alice")
	ct, body := toolkitSessionBody()
	response := webDecode(t, webCall(t, s, c, "POST", "/v1/sessions", ct, body, 201))
	path := "/v1/sessions/" + response["sessionId"].(string)
	for endpoint, expected := range map[string][]byte{
		"sessionData":           {0, 1, 2, 0xff, 0},
		"changeableSessionData": {9, 0, 8},
		"sessionImage":          {0xff, 0xd8, 0xff, 0xd9},
	} {
		got := webCall(t, s, c, "GET", path+"/"+endpoint, "", nil, 200)
		if !bytes.Equal(got.Body.Bytes(), expected) {
			t.Fatalf("Toolkit attachment %s changed: %x", endpoint, got.Body.Bytes())
		}
	}
}

func TestToolkitSessionRejectsIncompleteAttachments(t *testing.T) {
	s := webFixture(t)
	c := login(t, s, "Alice")
	ct, body := toolkitSessionBody()
	for name, invalid := range map[string][]byte{
		"truncated":           body[:len(body)-1],
		"extra_byte":          append(append([]byte{}, body...), 7),
		"missing_length":      bytes.Replace(body, []byte("Content-Length:3\r\n"), nil, 1),
		"negative_length":     bytes.Replace(body, []byte("Content-Length:3\r\n"), []byte("Content-Length:-3\r\n"), 1),
		"duplicate_length":    bytes.Replace(body, []byte("Content-Length:3\r\n"), []byte("Content-Length:3\r\nContent-Length:3\r\n"), 1),
		"wrong_length_closed": append(bytes.Replace(body, []byte("Content-Length:3\r\n"), []byte("Content-Length:4\r\n"), 1), []byte("\r\n--7341db8eb3e39bdb87a19a069106223c--\r\n")...),
	} {
		t.Run(name, func(t *testing.T) {
			webCall(t, s, c, "POST", "/v1/sessions", ct, invalid, 400)
		})
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.webSessions) != 0 {
		t.Fatal("invalid upload created a session")
	}
}
func TestWebSessionLifecycleAndOpaqueData(t *testing.T) {
	s := webFixture(t)
	a := login(t, s, "Alice")
	b := login(t, s, "Bobby")
	c := login(t, s, "Carol")
	id := webCreate(t, s, a, "public", "owner-migration")
	path := "/v1/sessions/" + id
	out := webDecode(t, webCall(t, s, b, "GET", "/v1/users/me/friends/sessions", "", nil, 200))
	if out["totalResults"] != float64(1) {
		t.Fatal(out)
	}
	data := webCall(t, s, b, "GET", path+"/sessionData", "", nil, 200)
	if !bytes.Equal(data.Body.Bytes(), []byte{0, 1, 2, 3, 255}) {
		t.Fatal("opaque session data changed")
	}
	webCall(t, s, b, "POST", path+"/members", "", nil, 204)
	webCall(t, s, c, "POST", path+"/members", "", nil, 409)
	webCall(t, s, b, "PUT", path, "application/json", []byte(`{"sessionName":"stolen"}`), 403)
	webCall(t, s, b, "DELETE", path, "", nil, 403)
	webCall(t, s, a, "PUT", path, "application/json", []byte(`{"sessionMaxUser":1}`), 400)
	webCall(t, s, a, "PUT", path, "application/json", []byte(`{"sessionLockFlag":true,"sessionName":"Act II"}`), 204)
	out = webDecode(t, webCall(t, s, b, "GET", path, "", nil, 200))
	if out["sessionName"] != "Act II" || out["sessionLockFlag"] != true || out["memberCount"] != float64(2) {
		t.Fatal(out)
	}
	webCall(t, s, a, "PUT", path+"/changeableSessionData", "application/octet-stream", []byte{5, 0, 255}, 204)
	data = webCall(t, s, b, "GET", path+"/changeableSessionData", "", nil, 200)
	if !bytes.Equal(data.Body.Bytes(), []byte{5, 0, 255}) {
		t.Fatal("changeable data changed")
	}
	webCall(t, s, a, "DELETE", path+"/members/me", "", nil, 204)
	out = webDecode(t, webCall(t, s, b, "GET", path, "", nil, 200))
	if out["sessionCreator"].(map[string]any)["onlineId"] != "Bobby" {
		t.Fatal("owner not migrated", out)
	}
	webCall(t, s, b, "DELETE", path, "", nil, 204)
	webCall(t, s, b, "GET", path, "", nil, 404)
	private := webCreate(t, s, a, "private", "owner-bind")
	webCall(t, s, b, "GET", "/v1/sessions/"+private, "", nil, 404)
	out = webDecode(t, webCall(t, s, b, "GET", "/v1/users/me/friends/sessions", "", nil, 200))
	if out["totalResults"] != float64(0) {
		t.Fatal("private session leaked", out)
	}
	// A disconnected owner-bind session disappears rather than listing a dead host.
	a.conn.Close()
	deadline := time.Now().Add(time.Second)
	for {
		s.mu.Lock()
		gone := s.webSessions[private] == nil
		s.mu.Unlock()
		if gone {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("owner session not cleaned")
		}
		time.Sleep(time.Millisecond)
	}
}
func TestWebRequestBounds(t *testing.T) {
	s := webFixture(t)
	a := login(t, s, "Alice")
	webCall(t, s, a, http.MethodPost, "/v1/sessions", "application/json", []byte(`{"sessionPrivacy":"public","sessionType":"owner-bind","sessionMaxUser":8}`), 400)
	webCall(t, s, a, http.MethodPost, "/v1/sessions", "multipart/mixed; boundary=absent", []byte("broken body"), 400)
	webCall(t, s, a, http.MethodPut, "/v1/users/me/presence/gameData", "application/json", bytes.Repeat([]byte("x"), webBodyLimit+1), 413)
}

func TestWebInvitationsPrivateJoinAndPush(t *testing.T) {
	s := webFixture(t)
	a := login(t, s, "Alice")
	b := login(t, s, "Bobby")
	c := login(t, s, "Carol")
	id := webCreate(t, s, a, "private", "owner-bind")
	path := "/v1/sessions/" + id
	webCall(t, s, b, "GET", path, "", nil, 404)
	webCall(t, s, b, "POST", path+"/invitations", "application/json", []byte(`{"to":["Carol"]}`), 404)
	webCall(t, s, a, "POST", path+"/invitations", "application/json", []byte(`{"to":["Bobby"],"message":"Join Act I"}`), 204)
	b.conn.SetReadDeadline(time.Now().Add(time.Second))
	push, err := readPacket(b.conn)
	if err != nil {
		t.Fatal(err)
	}
	if push.kind != 2 || push.command != 17 {
		t.Fatalf("not native WebAPI push: %#v", push)
	}
	payload := push.body
	take := func() string {
		t.Helper()
		if len(payload) < 4 {
			t.Fatal("short push field")
		}
		n := int(binary.LittleEndian.Uint32(payload))
		payload = payload[4:]
		if n > len(payload) {
			t.Fatal("short push string")
		}
		v := string(payload[:n])
		payload = payload[n:]
		return v
	}
	if got := take(); got != "sessionInvitation" {
		t.Fatal(got)
	}
	if len(payload) < 4 || binary.LittleEndian.Uint32(payload) != 0 {
		t.Fatal("missing service label")
	}
	payload = payload[4:]
	if got := take(); got != "np:service:invitation" {
		t.Fatal(got)
	}
	var inv map[string]any
	if err = json.Unmarshal([]byte(take()), &inv); err != nil {
		t.Fatal(err)
	}
	invID := inv["invitationId"].(string)
	if take() != "Alice" || take() != "Bobby" {
		t.Fatal("push identity mismatch")
	}
	if len(payload) < 4 || binary.LittleEndian.Uint32(payload) != 3 {
		t.Fatal("missing extended invitation fields")
	}
	payload = payload[4:]
	ext := map[string]string{}
	for i := 0; i < 3; i++ {
		k := take()
		ext[k] = take()
	}
	if ext["sessionId"] != id || ext["invitationId"] != invID || len(payload) != 16 {
		t.Fatalf("invalid push extras: %#v", ext)
	}
	s.mu.Lock()
	from := s.sessions["Alice"].account
	to := s.sessions["Bobby"].account
	s.mu.Unlock()
	if binary.LittleEndian.Uint64(payload) != from || binary.LittleEndian.Uint64(payload[8:]) != to {
		t.Fatal("push account IDs mismatch")
	}
	out := webDecode(t, webCall(t, s, b, "GET", "/v1/users/me/invitations", "", nil, 200))
	if out["totalResults"] != float64(1) {
		t.Fatal(out)
	}
	webCall(t, s, c, "GET", "/v1/users/me/invitations/"+invID, "", nil, 404)
	webCall(t, s, b, "GET", path, "", nil, 200)
	webCall(t, s, b, "PUT", "/v1/users/me/invitations/"+invID, "application/json", []byte(`{"usedFlag":true}`), 204)
	out = webDecode(t, webCall(t, s, b, "GET", "/v1/users/me/invitations", "", nil, 200))
	if out["totalResults"] != float64(0) {
		t.Fatal("used invitation retained in pending list", out)
	}
	webCall(t, s, b, "POST", path+"/members", "", nil, 204)
	webCall(t, s, c, "POST", path+"/members", "", nil, 404)
	webCall(t, s, b, "DELETE", "/v1/users/me/invitations/"+invID, "", nil, 204)
	webCall(t, s, b, "GET", path, "", nil, 200)
}
