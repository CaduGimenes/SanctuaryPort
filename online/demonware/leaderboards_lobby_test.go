// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"context"
	"crypto/sha256"
	"io"
	"log/slog"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestEncryptedLeaderboardsPersistAndProtectOwnership(t *testing.T) {
	root := t.TempDir()
	cfg := TitleConfig{PublisherDir: filepath.Join(root, "publisher"), StateDir: filepath.Join(root, "state")}
	title, err := NewTitleServices(cfg)
	if err != nil {
		t.Fatal(err)
	}
	auth, _, _ := authFixture(t)
	ticket := authTicket(t, authCall(auth, authBody(t, 46, D3TitleID, testToken)))
	alice, _ := auth.Lookup(ticket)
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
	start := func(title *TitleServices) *LobbyServer {
		lobby := NewLobbyServer(auth, title.HandleTask, 4)
		lobby.OnDisconnect = title.OnDisconnect
		lobby.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
		t.Cleanup(lobby.Close)
		return lobby
	}
	lobby := start(title)
	a, b := connectLobby(t, lobby, alice, false), connectLobby(t, lobby, bobby, false)
	write := func(entity uint64) []byte {
		w := NewTaggedWriter(256)
		w.U32(401)
		w.U64(entity)
		w.U8(4)
		w.I64(12345)
		w.U64(321)
		w.I64(-17)
		w.Blob([]byte{9, 8, 0}, 8192)
		data, err := w.Bytes()
		if err != nil {
			t.Fatal(err)
		}
		return data
	}
	matrix := NewTaggedWriter(256)
	matrix.U32(1)
	matrix.U64(alice.Identity.AccountID)
	matrix.U32(2)
	matrix.U32(401)
	matrix.U32(402)
	matrixData, _ := matrix.Bytes()
	// A new private board returns a decodable absent cell for every requested
	// entity/board pair, allowing the title's asynchronous summaries to finish.
	r, n, code := a.task(t, 4, 11, matrixData)
	if code != 0 || n != 2 {
		t.Fatal("empty matrix failed", code, n)
	}
	for _, board := range []uint32{401, 402} {
		if r.U32() != board || r.U32() == 0 || r.U64() != alice.Identity.AccountID {
			t.Fatal("invalid absent matrix cell")
		}
	}
	if r.Done() != nil {
		t.Fatal("matrix trailing bytes")
	}
	if r, n, code := a.task(t, 4, 1, write(alice.Identity.AccountID)); code != 0 || n != 0 || r.Done() != nil {
		t.Fatal("encrypted submission failed", code, n)
	}
	if _, _, code := b.task(t, 4, 1, write(alice.Identity.AccountID)); code == 0 {
		t.Fatal("foreign account overwrote a leaderboard row")
	}
	read := NewTaggedWriter(256)
	read.U32(401)
	read.U32(1)
	read.U64(alice.Identity.AccountID)
	read.U32(0)
	readData, _ := read.Bytes()
	checkRow := func(client *lobbyTestClient) {
		t.Helper()
		r, n, code := client.task(t, 4, 13, readData)
		if code != 0 || n != 1 || r.U64() != alice.Identity.AccountID || r.I64() != 12345 || r.U64() != 1 || r.CString(63) != alice.Identity.Name || r.U32() != 0 || r.U64() != 321 || r.I64() != -17 || !bytes.Equal(r.Blob(8192), []byte{9, 8, 0}) || r.Done() != nil {
			t.Fatal("encrypted public row read failed", code, n, r.Err())
		}
	}
	checkRow(b)
	a.conn.Close()
	b.conn.Close()
	lobby.Close()
	restarted, err := NewTitleServices(cfg)
	if err != nil {
		t.Fatal(err)
	}
	checkRow(connectLobby(t, start(restarted), bobby, false))
}
