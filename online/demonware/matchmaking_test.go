// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/hex"
	"errors"
	"io"
	"math"
	"strings"
	"sync"
	"testing"
)

// Field order/tags from the installed game's 42d080/e277f0 serializers. This
// literal is independent of the server's result writer and stateful handler.
const nativeCreateHex = "13080300000001020308000000000804000000074600000007b80b000007ffffffff1308080000000001020304050607130810000000101112131415161718191a1b1c1d1e1f07030000000803000000072bb400000702000000070d000000070700000007010000000701000000"

func nativeCreate(t *testing.T) []byte {
	t.Helper()
	b, err := hex.DecodeString(nativeCreateHex)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func matchIdentity(id uint64, lease byte) Session {
	s := Session{Identity: Identity{AccountID: id, Name: "Player"}, TitleID: D3TitleID}
	s.Ticket[0] = lease
	return s
}

func createMatch(t *testing.T, m *Matchmaking, s Session, data []byte) [8]byte {
	t.Helper()
	result := m.Handle(s, 1, data)
	if result.Err != nil || len(result.Records) != 1 || result.Total != 1 {
		t.Fatalf("create: %#v", result)
	}
	r := NewTaggedReader(result.Records[0])
	id := matchSessionID(r)
	if r.Done() != nil || id == ([8]byte{}) {
		t.Fatal("bad created session ID", r.Err())
	}
	return id
}

func matchingQuery(id, offset, limit uint32, fields [12]int64) []byte {
	w := NewTaggedWriter(256)
	w.U32(id)
	w.U32(offset)
	w.U32(limit)
	for i, value := range fields {
		if i == 6 {
			w.U32(uint32(value))
		} else {
			w.I32(int32(value))
		}
	}
	b, err := w.Bytes()
	if err != nil {
		panic(err)
	}
	return b
}

func queryFixture() [12]int64 {
	return [12]int64{70, 65, 75, 3000, -1, 3, 1, 46123, 2, 13, 7, 1}
}

func sessionRequest(id [8]byte, info []byte, players *uint32) []byte {
	w := NewTaggedWriter(2048)
	w.Blob(id[:], 8)
	if players != nil {
		w.U32(*players)
	}
	w.Raw(info)
	b, err := w.Bytes()
	if err != nil {
		panic(err)
	}
	return b
}

func TestMatchmakingNativeRecordCreateSearch(t *testing.T) {
	m := NewMatchmaking(4)
	a, b := matchIdentity(1, 1), matchIdentity(2, 2)
	input := nativeCreate(t)
	id := createMatch(t, m, a, input)
	// Inputs are not retained by reference: transport buffers can be reused.
	input[6] = 99
	result := m.Handle(b, 5, matchingQuery(2, 0, 20, queryFixture()))
	if result.Err != nil || result.Total != 1 || len(result.Records) != 1 {
		t.Fatalf("search: %#v", result)
	}
	// Native result differs from the request by its ID blob and numPlayers=1.
	original := nativeCreate(t)
	var want []byte
	want = append(want, original[:9]...)
	want = append(want, []byte{0x13, 8, 8, 0, 0, 0}...)
	want = append(want, id[:]...)
	want = append(want, original[9:19]...)
	want = append(want, []byte{8, 1, 0, 0, 0}...)
	want = append(want, original[19:]...)
	if !bytes.Equal(want, result.Records[0]) {
		t.Fatalf("native search result\n got %x\nwant %x", result.Records[0], want)
	}
	self := m.Handle(a, 5, matchingQuery(2, 0, 20, queryFixture()))
	if self.Err != nil || self.Total != 0 || len(self.Records) != 0 {
		t.Fatal("owner discovered its own advertisement")
	}
}

func TestMatchmakingOwnerUpdatesDeleteAndLeases(t *testing.T) {
	m := NewMatchmaking(1)
	a, b := matchIdentity(1, 1), matchIdentity(2, 2)
	input := nativeCreate(t)
	id := createMatch(t, m, a, input)
	if got := m.Handle(b, 2, sessionRequest(id, input, nil)); got.Err != ErrMatchmakingForbidden {
		t.Fatal(got.Err)
	}
	wrongLease := matchIdentity(1, 3)
	if got := m.Handle(wrongLease, 3, sessionRequest(id, nil, nil)); got.Err != ErrMatchmakingForbidden {
		t.Fatal(got.Err)
	}
	// Change open slots from 3 to 0, preserving the opaque peer/security data.
	updated := append([]byte(nil), input...)
	updated[76] = 0 // U32 open-slot value in the independent native fixture.
	if got := m.Handle(a, 2, sessionRequest(id, updated, nil)); got.Err != nil {
		t.Fatal(got.Err)
	}
	if got := m.Handle(b, 5, matchingQuery(2, 0, 20, queryFixture())); got.Err != nil || got.Total != 0 {
		t.Fatal("full room advertised", got)
	}
	players := uint32(1)
	if got := m.Handle(a, 12, sessionRequest(id, input, &players)); got.Err != nil {
		t.Fatal(got.Err)
	}
	players = 2
	if got := m.Handle(a, 12, sessionRequest(id, input, &players)); got.Err != ErrMatchmakingInvalidRequest {
		t.Fatal("inconsistent player count accepted", got.Err)
	}
	if got := m.Handle(b, 5, matchingQuery(2, 0, 20, queryFixture())); got.Total != 1 {
		t.Fatal("updated room missing", got)
	}
	if got := m.Handle(a, 3, sessionRequest(id, nil, nil)); got.Err != nil {
		t.Fatal(got.Err)
	}
	if got := m.Handle(a, 3, sessionRequest(id, nil, nil)); got.Err != ErrMatchmakingInvalidSession {
		t.Fatal(got.Err)
	}
	createMatch(t, m, a, input)
	if got := m.Handle(b, 1, input); got.Err != ErrMatchmakingCapacity {
		t.Fatal("capacity not enforced", got.Err)
	}
	newID := createMatch(t, m, wrongLease, input) // same owner may replace at capacity
	m.OnDisconnect(a)
	if got := m.Handle(b, 5, matchingQuery(2, 0, 20, queryFixture())); got.Total != 1 {
		t.Fatal("stale close erased replacement", got)
	}
	if got := m.Handle(wrongLease, 2, sessionRequest(newID, input, nil)); got.Err != nil {
		t.Fatal(got.Err)
	}
	m.OnDisconnect(wrongLease)
	if got := m.Handle(b, 5, matchingQuery(2, 0, 20, queryFixture())); got.Total != 0 {
		t.Fatal("disconnect leaked room", got)
	}
}

func TestMatchmakingMalformedRequestsNeverMutate(t *testing.T) {
	m := NewMatchmaking(2)
	a := matchIdentity(1, 1)
	input := nativeCreate(t)
	for n := 0; n < len(input); n++ {
		if got := m.Handle(a, 1, input[:n]); got.Err != ErrMatchmakingInvalidRequest {
			t.Fatalf("truncated create %d: %v", n, got.Err)
		}
	}
	for _, data := range [][]byte{append(append([]byte(nil), input...), 0), {0x13, 0x14}, {0x13, 8, 0xff, 0xff, 0xff, 0x7f}} {
		if got := m.Handle(a, 1, data); got.Err != ErrMatchmakingInvalidRequest {
			t.Fatal("bad create accepted", got.Err)
		}
	}
	if len(m.rows) != 0 {
		t.Fatal("malformed creates changed registry")
	}
	if got := m.Handle(a, 99, input); got.Err != ErrMatchmakingUnsupported {
		t.Fatal(got.Err)
	}
	a.TitleID = 1
	if got := m.Handle(a, 1, input); got.Err != ErrMatchmakingForbidden {
		t.Fatal(got.Err)
	}
}

func TestMatchmakingTaskDiagnostics(t *testing.T) {
	m := NewMatchmaking(2)
	session := matchIdentity(1, 1)
	for _, row := range []struct {
		name          string
		service, task uint8
		data          []byte
		reason        string
	}{
		{"unsupported service", 99, 1, nil, "unsupported Demonware service 99"},
		{"unsupported task", 21, 99, nil, ErrMatchmakingUnsupported.Error()},
		{"truncated query", 21, 5, []byte{8}, "at byte 1 of 1"},
		{"invalid tag", 21, 5, []byte("PRIVATE-PAYLOAD"), "invalid Demonware field value"},
	} {
		t.Run(row.name, func(t *testing.T) {
			got := m.HandleTask(session, row.service, row.task, row.data)
			if got.ErrorCode != 3 || got.Reason == nil || !strings.Contains(got.Reason.Error(), row.reason) {
				t.Fatalf("missing refusal diagnostic: %#v", got)
			}
			if strings.Contains(got.Reason.Error(), "PRIVATE-PAYLOAD") {
				t.Fatal("request payload exposed in diagnostic")
			}
		})
	}
	got := m.HandleTask(session, 21, 3, sessionRequest([8]byte{1}, nil, nil))
	if got.ErrorCode != 0x835 || !errors.Is(got.Reason, ErrMatchmakingInvalidSession) {
		t.Fatalf("unknown-session mapping changed: %#v", got)
	}
	got = m.HandleTask(session, 21, 5, matchingQuery(3, 0, 20, queryFixture()))
	if got.ErrorCode != 0 || got.Reason != nil || got.Total != 0 {
		t.Fatalf("empty search is a success: %#v", got)
	}
}

func TestMatchmakingFiltersPaginationAndFailures(t *testing.T) {
	m := NewMatchmaking(4)
	a, b := matchIdentity(1, 1), matchIdentity(2, 2)
	input := nativeCreate(t)
	createMatch(t, m, a, input)
	query := queryFixture()
	for _, field := range []int{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11} {
		bad := query
		switch field {
		case 1:
			bad[field] = 71
		case 2:
			bad[field] = 69
		case 6:
			bad[field] = 4
		default:
			bad[field]++
		}
		if got := m.Handle(b, 5, matchingQuery(2, 0, 20, bad)); got.Err != nil || got.Total != 0 {
			t.Fatalf("field %d filter ignored: %#v", field, got)
		}
	}
	wild := [12]int64{70, 0, 0, -1, -1, -1, math.MaxUint32, -1, -1, -1, -1, -1}
	if got := m.Handle(b, 5, matchingQuery(3, math.MaxUint32, 20, wild)); got.Err != nil || got.Total != 1 || len(got.Records) != 0 {
		t.Fatal("pagination overflow", got)
	}
	if got := m.Handle(b, 5, matchingQuery(3, 0, 21, wild)); got.Err != ErrMatchmakingInvalidRequest {
		t.Fatal("unbounded query accepted", got)
	}
	m.random = errorReader{}
	if got := m.Handle(a, 1, input); got.Err != ErrMatchmakingUnavailable {
		t.Fatal("entropy failure accepted", got)
	}
	if got := m.Handle(b, 5, matchingQuery(2, 0, 20, query)); got.Total != 1 {
		t.Fatal("failed replacement deleted live room", got)
	}
}

type errorReader struct{}

func (errorReader) Read([]byte) (int, error) { return 0, io.ErrUnexpectedEOF }

func TestMatchmakingConcurrentPlayers(t *testing.T) {
	m := NewMatchmaking(32)
	input := nativeCreate(t)
	var wg sync.WaitGroup
	for i := uint64(1); i <= 16; i++ {
		wg.Add(1)
		go func(i uint64) {
			defer wg.Done()
			s := matchIdentity(i, byte(i))
			for n := 0; n < 20; n++ {
				created := m.Handle(s, 1, input)
				if created.Err != nil {
					t.Error(created.Err)
					return
				}
				idReader := NewTaggedReader(created.Records[0])
				id := matchSessionID(idReader)
				if got := m.Handle(s, 2, sessionRequest(id, input, nil)); got.Err != nil {
					t.Error(got.Err)
					return
				}
				if got := m.Handle(s, 5, matchingQuery(3, 0, 20, queryFixture())); got.Err != nil {
					t.Error(got.Err)
					return
				}
			}
			m.OnDisconnect(s)
		}(i)
	}
	wg.Wait()
	if len(m.rows) != 0 {
		t.Fatal("concurrent disconnects leaked sessions")
	}
}
