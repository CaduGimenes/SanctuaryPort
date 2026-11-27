// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"log/slog"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestLobbyCryptoIndependentVectors(t *testing.T) {
	key := make([]byte, 24)
	for i := range key {
		key[i] = byte(i)
	}
	keys, err := deriveLobbyKeys(key, []byte("D3 lobby transcript"))
	if err != nil {
		t.Fatal(err)
	}
	proof := append(append([]byte{}, keys.clientProof[:]...), keys.serverProof[:]...)
	data := append(append(append(append([]byte{}, keys.clientMAC[:]...), keys.serverMAC[:]...), keys.clientAES[:]...), keys.serverAES[:]...)
	// Independent Python hashlib/hmac HKDF and OpenSSL AES-CBC output,
	// including the native identity-context expansion before transcript extraction.
	if hex.EncodeToString(proof) != "acb6925f33c56c8c3315ac7d837d65b8" || hex.EncodeToString(data) != "35aaa759dadfc6e835747a61c5b1ea539991097f602dd3035391162fc11260c05c37734498d49e35ce46413254203d08db72b403747b454d3799a11a641fe0943eb07800991ad8c8" {
		t.Fatal("native identity/transcript key derivation mismatch")
	}
	iv := make([]byte, 16)
	for i := range iv {
		iv[i] = byte(i)
	}
	record, err := encryptLobbyRecord([]byte{21, 3, 1}, 0x86, 1, keys.clientAES[:], keys.clientMAC[:], bytes.NewReader(iv))
	if err != nil || hex.EncodeToString(record) != "2e000000ab8501000000000102030405060708090a0b0c0d0e0f46bc755acbc24ecc2cb3c08394a719765c6d2945a3b6bbdb" {
		t.Fatalf("AES/MAC record mismatch %x %v", record, err)
	}
	payload, kind, err := decryptLobbyRecord(record, 1, keys.clientAES[:], keys.clientMAC[:])
	if err != nil || kind != 0x86 || !bytes.Equal(payload, []byte{21, 3, 1}) {
		t.Fatal("valid record rejected", err)
	}
	for _, at := range []int{0, 4, 6, 10, 26, len(record) - 1} {
		bad := append([]byte{}, record...)
		bad[at] ^= 1
		if _, _, err := decryptLobbyRecord(bad, 1, keys.clientAES[:], keys.clientMAC[:]); err == nil {
			t.Fatal("modified record accepted", at)
		}
	}
	if _, _, err := decryptLobbyRecord(record, 2, keys.clientAES[:], keys.clientMAC[:]); err == nil {
		t.Fatal("replayed counter accepted")
	}
	if _, _, err := decryptLobbyRecord(record, 1, keys.serverAES[:], keys.serverMAC[:]); err == nil {
		t.Fatal("wrong direction keys accepted")
	}
}

func TestLobbyIdentityContext(t *testing.T) {
	hash := sha256.Sum256([]byte(lobbyIdentityContext))
	if len(lobbyIdentityContext) != 294 || hex.EncodeToString(hash[:]) != "356c4bc39a664dd9a14964a3a44258d898a0dbf54cfb10835cbdd63000c9a6b2" {
		t.Fatal("identity context no longer matches the native auth identity")
	}
	key := make([]byte, 24)
	for i := range key {
		key[i] = byte(i)
	}
	if got := hex.EncodeToString(expandSHA1(key, lobbyIdentityContext, 24)); got != "97ac7878c6e8185a8905978a804afd8b7c43395f5f495294" {
		t.Fatal("native intermediate key mismatch", got)
	}
}

func TestLobbyTaskEnvelope(t *testing.T) {
	empty, err := encodeTaskResult(42, 3, TaskResult{})
	if err != nil || hex.EncodeToString(empty) != "0a2a00000000000000080000000003030800000000" {
		t.Fatalf("empty envelope %x %v", empty, err)
	}
	failure, err := encodeTaskResult(42, 3, TaskResult{ErrorCode: 0x835, Reason: errors.New("server-only diagnostic")})
	if err != nil || hex.EncodeToString(failure) != "0a2a000000000000000835080000" {
		t.Fatalf("failure envelope %x %v", failure, err)
	}
	if _, err = encodeTaskResult(42, 5, TaskResult{Records: [][]byte{{1}, {2}}, Total: 1}); err == nil {
		t.Fatal("invalid result counts accepted")
	}
}

func TestLobbyProbeFailureIsVisible(t *testing.T) {
	auth, _, _ := authFixture(t)
	lobby := NewLobbyServer(auth, func(Session, uint8, uint8, []byte) TaskResult { return TaskResult{} }, 1)
	var log bytes.Buffer
	lobby.Logger = slog.New(slog.NewTextHandler(&log, nil))
	server, client := net.Pipe()
	defer client.Close()
	client.SetDeadline(time.Now().Add(3 * time.Second))
	done := make(chan struct{})
	go func() {
		defer close(done)
		lobby.ServeConn(server)
	}()
	probe := make([]byte, 28)
	for i, v := range []uint32{200, 200, 210, 220, 256} {
		binary.LittleEndian.PutUint32(probe[i*4:], v)
	}
	if err := writeLobbyBytes(client, probe); err != nil {
		t.Fatal(err)
	}
	var response [1]byte
	if _, err := client.Read(response[:]); !errors.Is(err, io.EOF) {
		t.Fatalf("invalid probe did not close connection: %v", err)
	}
	<-done
	for _, want := range []string{"Demonware lobby connected", "level=WARN", "Demonware lobby handshake failed", "receive_limit=256"} {
		if !strings.Contains(log.String(), want) {
			t.Fatalf("missing %q in handshake diagnostic: %s", want, &log)
		}
	}
}

func lobbyAuthBits(title, seed uint32, ticket []byte) []byte {
	b := make([]byte, 139)
	at := 0
	put := func(v uint64, n int) {
		for i := 0; i < n; i++ {
			b[at/8] |= byte((v>>uint(i))&1) << uint(at%8)
			at++
		}
	}
	put(7, 8)
	put(1, 1)
	put(8, 5)
	put(uint64(title), 32)
	put(8, 5)
	put(uint64(seed), 32)
	for _, v := range ticket {
		put(uint64(v), 8)
	}
	return b
}

type lobbyTestClient struct {
	conn          net.Conn
	keys          lobbyKeys
	send, receive uint32
}

func connectLobby(t *testing.T, s *LobbyServer, session Session, badProof bool) *lobbyTestClient {
	t.Helper()
	server, client := net.Pipe()
	t.Cleanup(func() { client.Close() })
	client.SetDeadline(time.Now().Add(3 * time.Second))
	go s.ServeConn(server)
	probe := make([]byte, 28)
	for i, v := range []uint32{200, 200, 210, 220, 0x1fffff} {
		binary.LittleEndian.PutUint32(probe[i*4:], v)
	}
	copy(probe[20:], []byte("nonce123"))
	if err := writeLobbyBytes(client, probe); err != nil {
		t.Fatal(err)
	}
	challenge, err := ReadFrame(client, 2048)
	if err != nil || len(challenge) != 22 || !bytes.Equal(challenge[:2], []byte{0xab, 0x81}) || binary.LittleEndian.Uint32(challenge[2:]) != 210 {
		t.Fatal("bad challenge", err)
	}
	auth := append([]byte{0xab, 0x82}, lobbyAuthBits(D3TitleID, 42, session.Ticket[:])...)
	auth = append(auth, make([]byte, 8)...)
	clientFrame := framedLobbyRecord(auth)
	transcript := append(append(append([]byte{}, probe[8:]...), framedLobbyRecord(challenge)...), clientFrame[:len(clientFrame)-8]...)
	keys, err := deriveLobbyKeys(session.Key, transcript)
	if err != nil {
		t.Fatal(err)
	}
	copy(auth[len(auth)-8:], keys.clientProof[:])
	if badProof {
		auth[len(auth)-1] ^= 1
	}
	if err := WriteFrame(client, auth, 2048); err != nil {
		t.Fatal(err)
	}
	done, err := ReadFrame(client, 2048)
	if badProof {
		if err == nil {
			t.Fatal("bad client proof received server acceptance")
		}
		return nil
	}
	if err != nil || !bytes.Equal(done, append([]byte{0xab, 0x83}, keys.serverProof[:]...)) {
		t.Fatal("server proof failed", err)
	}
	return &lobbyTestClient{client, keys, 1, 1}
}

func (c *lobbyTestClient) task(t *testing.T, service, task byte, data []byte) (*TaggedReader, uint32, uint32) {
	t.Helper()
	c.conn.SetDeadline(time.Now().Add(3 * time.Second))
	request := append([]byte{service, 3, task}, data...)
	// Native RemoteTaskManager de54b0 calls dda710 to append the raw
	// bdByteBuffer end marker after the service-specific fields.
	request = append(request, 0)
	encoded, err := encryptLobbyRecord(request, 0x86, c.send, c.keys.clientAES[:], c.keys.clientMAC[:], strings.NewReader(strings.Repeat("v", 16)))
	if err != nil {
		t.Fatal(err)
	}
	c.send++
	if err = writeLobbyBytes(c.conn, encoded); err != nil {
		t.Fatal(err)
	}
	frame, err := ReadFrame(c.conn, DefaultMaxFrame)
	if err != nil {
		t.Fatal(err)
	}
	payload, kind, err := decryptLobbyRecord(framedLobbyRecord(frame), c.receive, c.keys.serverAES[:], c.keys.serverMAC[:])
	c.receive++
	if err != nil || kind != 1 {
		t.Fatal("bad encrypted task result", err, kind)
	}
	r := NewTaggedReader(payload)
	if r.U64() == 0 {
		t.Fatal("missing transaction ID")
	}
	code := r.U32()
	if code != 0 {
		if r.Done() != nil {
			t.Fatal("failure trailing data")
		}
		return r, 0, code
	}
	if r.U8() != task {
		t.Fatal("wrong task correlation")
	}
	count := r.U32()
	if count > 0 && r.U32() < count {
		t.Fatal("invalid total")
	}
	return r, count, code
}

func TestEncryptedLobbyMatchmakingAndDisconnect(t *testing.T) {
	auth, _, _ := authFixture(t)
	auth.cfg.EncodeClientTicket = EncodeClientTicket
	aliceTicket := authTicket(t, authCall(auth, authBody(t, 46, D3TitleID, testToken)))
	alice, ok := auth.Lookup(aliceTicket)
	if !ok {
		t.Fatal("missing auth session")
	}
	// A second independently authenticated master identity, with its own ticket/key.
	bobby := Session{Identity: Identity{43, "Bobby"}, TitleID: D3TitleID, Key: bytes.Repeat([]byte{0xb3}, 24), Expires: time.Now().Add(time.Minute), masterToken: strings.Repeat("b", 64)}
	for i := range bobby.Ticket {
		bobby.Ticket[i] = byte(i)
	}
	auth.cfg.Resolve = func(_ context.Context, token string) (Identity, bool) {
		if token == testToken {
			return alice.Identity, true
		}
		return bobby.Identity, token == bobby.masterToken
	}
	auth.mu.Lock()
	auth.sessions[sha256.Sum256(bobby.Ticket[:])] = bobby
	auth.mu.Unlock()
	matching := NewMatchmaking(4)
	lobby := NewLobbyServer(auth, matching.HandleTask, 4)
	lobby.OnDisconnect = matching.OnDisconnect
	lobby.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	t.Cleanup(lobby.Close)
	a := connectLobby(t, lobby, alice, false)
	b := connectLobby(t, lobby, bobby, false)
	r, n, code := a.task(t, 21, 1, nativeCreate(t))
	if code != 0 || n != 1 {
		t.Fatal("create failed", code, n)
	}
	id := matchSessionID(r)
	if r.Done() != nil {
		t.Fatal("bad create record")
	}
	r, n, code = b.task(t, 21, 5, matchingQuery(2, 0, 20, queryFixture()))
	if code != 0 || n != 1 {
		t.Fatal("search failed", code, n)
	}
	if !bytes.Equal(r.Blob(255), []byte{1, 2, 3}) || matchSessionID(r) != id || r.U32() != 0 || r.U32() != 4 || r.U32() != 1 {
		t.Fatal("peer metadata corrupted")
	}
	// Only the envelope marker is removed. A second zero is still an
	// unexpected task field, even though both look like end markers.
	_, _, code = b.task(t, 21, 5, append(matchingQuery(2, 0, 20, queryFixture()), 0))
	if code == 0 {
		t.Fatal("extra trailing search byte accepted")
	}
	// A foreign player cannot update/delete the host's room.
	_, _, code = b.task(t, 21, 3, sessionRequest(id, nil, nil))
	if code == 0 {
		t.Fatal("foreign delete accepted")
	}
	_, _, code = a.task(t, 21, 2, sessionRequest(id, nativeCreate(t), nil))
	if code != 0 {
		t.Fatal("update failed", code)
	}
	_, _, code = a.task(t, 21, 3, sessionRequest(id, nil, nil))
	if code != 0 {
		t.Fatal("delete failed", code)
	}
	_, n, code = b.task(t, 21, 5, matchingQuery(2, 0, 20, queryFixture()))
	if code != 0 || n != 0 {
		t.Fatal("deleted room still visible")
	}
	_, _, code = a.task(t, 255, 1, nil)
	if code == 0 {
		t.Fatal("unsupported service reported success")
	}
	_, _, code = a.task(t, 21, 1, nativeCreate(t))
	if code != 0 {
		t.Fatal(code)
	}
	a.conn.Close()
	deadline := time.Now().Add(time.Second)
	for {
		matching.mu.Lock()
		empty := len(matching.rows) == 0
		matching.mu.Unlock()
		if empty {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("disconnect retained room")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestLobbyRejectsMalformedTaskFrames(t *testing.T) {
	for _, row := range []struct {
		name    string
		payload []byte
	}{
		{"empty", nil},
		{"missing end marker", []byte{21, 3, 5}},
		{"nonzero end marker", []byte{21, 3, 5, 1}},
		{"invalid task tag", []byte{21, 8, 5, 0}},
	} {
		t.Run(row.name, func(t *testing.T) {
			auth, _, _ := authFixture(t)
			ticket := authTicket(t, authCall(auth, authBody(t, 46, D3TitleID, testToken)))
			session, _ := auth.Lookup(ticket)
			lobby := NewLobbyServer(auth, func(Session, uint8, uint8, []byte) TaskResult {
				t.Error("malformed task reached handler")
				return TaskResult{}
			}, 1)
			lobby.Logger = nil
			t.Cleanup(lobby.Close)
			client := connectLobby(t, lobby, session, false)
			record, err := encryptLobbyRecord(row.payload, 0x86, 1, client.keys.clientAES[:], client.keys.clientMAC[:], strings.NewReader(strings.Repeat("v", 16)))
			if err != nil {
				t.Fatal(err)
			}
			if err := writeLobbyBytes(client.conn, record); err != nil {
				t.Fatal(err)
			}
			if _, err := ReadFrame(client.conn, DefaultMaxFrame); !errors.Is(err, io.EOF) {
				t.Fatalf("malformed task did not close connection: %v", err)
			}
		})
	}
}

func TestLobbyRejectsForgedProof(t *testing.T) {
	auth, _, _ := authFixture(t)
	ticket := authTicket(t, authCall(auth, authBody(t, 46, D3TitleID, testToken)))
	session, _ := auth.Lookup(ticket)
	lobby := NewLobbyServer(auth, func(Session, uint8, uint8, []byte) TaskResult {
		t.Error("unauthenticated dispatch")
		return TaskResult{}
	}, 2)
	lobby.Logger = nil
	t.Cleanup(lobby.Close)
	connectLobby(t, lobby, session, true)
}

func TestLobbyReconnectWaitsForRoomCleanup(t *testing.T) {
	for _, closeFirst := range []bool{false, true} {
		name := "replacement_closes_old"
		if closeFirst {
			name = "old_already_cleaning_up"
		}
		t.Run(name, func(t *testing.T) {
			auth, _, _ := authFixture(t)
			ticket := authTicket(t, authCall(auth, authBody(t, 46, D3TitleID, testToken)))
			session, ok := auth.Lookup(ticket)
			if !ok {
				t.Fatal("missing title ticket")
			}
			matching := NewMatchmaking(4)
			lobby := NewLobbyServer(auth, matching.HandleTask, 4)
			lobby.Logger = nil
			t.Cleanup(lobby.Close)
			cleanupStarted, allowCleanup := make(chan struct{}), make(chan struct{})
			var cleanupOnce, releaseOnce sync.Once
			release := func() { releaseOnce.Do(func() { close(allowCleanup) }) }
			t.Cleanup(release)
			lobby.OnDisconnect = func(s Session) {
				cleanupOnce.Do(func() {
					close(cleanupStarted)
					<-allowCleanup
				})
				matching.OnDisconnect(s)
			}
			old := connectLobby(t, lobby, session, false)
			if _, _, code := old.task(t, 21, 1, nativeCreate(t)); code != 0 {
				t.Fatal("initial room creation failed", code)
			}
			if closeFirst {
				old.conn.Close()
				select {
				case <-cleanupStarted:
				case <-time.After(time.Second):
					t.Fatal("old cleanup did not start")
				}
			}
			// Reuse the exact same ticket, as a reconnecting native client can.
			next := connectLobby(t, lobby, session, false)
			select {
			case <-cleanupStarted:
			case <-time.After(time.Second):
				t.Fatal("replacement did not close the old connection")
			}
			ping := make(chan error, 1)
			go func() {
				err := WriteFrame(next.conn, nil, 2048)
				if err == nil {
					_, err = ReadFrame(next.conn, 2048)
				}
				ping <- err
			}()
			select {
			case err := <-ping:
				t.Fatalf("replacement dispatched before old cleanup finished: %v", err)
			case <-time.After(50 * time.Millisecond):
			}
			release()
			select {
			case err := <-ping:
				if err != nil {
					t.Fatal(err)
				}
			case <-time.After(time.Second):
				t.Fatal("replacement did not resume after cleanup")
			}
			if _, _, code := next.task(t, 21, 1, nativeCreate(t)); code != 0 {
				t.Fatal("replacement room creation failed", code)
			}
			// Searches exclude the caller's own room, so use an independent
			// identity to check that the new advertisement survived cleanup.
			observer := session
			observer.Identity = Identity{43, "Bobby"}
			found := matching.Handle(observer, 5, matchingQuery(2, 0, 20, queryFixture()))
			if found.Err != nil || len(found.Records) != 1 {
				t.Fatal("replacement room lost after reconnect", found.Err, len(found.Records))
			}
		})
	}
}
