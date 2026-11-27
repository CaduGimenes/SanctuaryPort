// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/hex"
	"math"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func counterFields(values ...int64) []byte {
	w := NewTaggedWriter(4096)
	for i := 0; i < len(values); i += 2 {
		w.U32(uint32(values[i]))
		w.I64(values[i+1])
	}
	b, err := w.Bytes()
	if err != nil {
		panic(err)
	}
	return b
}

func TestCounterNativeIncrementAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "counters.json")
	s, err := NewCounterStore(path)
	if err != nil {
		t.Fatal(err)
	}
	// Independent native field sequence: typed U32 ID 7; typed I64 delta -2.
	fixture, _ := hex.DecodeString("080700000009feffffffffffffff")
	if got := s.HandleTask(matchIdentity(42, 1), fixture); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	if got := s.HandleTask(matchIdentity(42, 2), counterFields(7, 5)); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	if got := s.HandleTask(matchIdentity(43, 1), counterFields(7, 9)); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	reopened, err := NewCounterStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if reopened.data.Accounts["42"]["7"] != 3 || reopened.data.Accounts["43"]["7"] != 9 {
		t.Fatal("counter values lost or accounts mixed", reopened.data)
	}
}

func TestCounterBatchMalformedOverflowAndDiskFailureAreAtomic(t *testing.T) {
	s, err := NewCounterStore(filepath.Join(t.TempDir(), "counters.json"))
	if err != nil {
		t.Fatal(err)
	}
	session := matchIdentity(42, 1)
	if got := s.HandleTask(session, counterFields(1, 4, 2, math.MaxInt64)); got.ErrorCode != 0 {
		t.Fatal(got.Reason)
	}
	initial, err := os.ReadFile(s.path)
	if err != nil {
		t.Fatal(err)
	}
	for _, fields := range [][]byte{
		nil,
		counterFields(1, 5, 2, 1),
		append(counterFields(1, 5), 8),
		append(counterFields(1, 5), 0),
		{8, 1, 0, 0, 0, 9},
	} {
		if got := s.HandleTask(session, fields); got.ErrorCode == 0 {
			t.Fatalf("invalid batch accepted %x", fields)
		}
		if s.data.Accounts["42"]["1"] != 4 || s.data.Accounts["42"]["2"] != math.MaxInt64 {
			t.Fatal("failed batch changed counters")
		}
		current, err := os.ReadFile(s.path)
		if err != nil || string(current) != string(initial) {
			t.Fatal("failed batch changed persisted counters", err)
		}
	}
	s.path = t.TempDir() // Cannot replace a directory with a snapshot file.
	if got := s.HandleTask(session, counterFields(1, 1)); got.ErrorCode == 0 {
		t.Fatal("failed write reported success")
	}
	if s.data.Accounts["42"]["1"] != 4 {
		t.Fatal("failed write changed memory")
	}
}

func TestCounterLimitsAuthenticationAndConcurrentUpdates(t *testing.T) {
	s, err := NewCounterStore("")
	if err != nil {
		t.Fatal(err)
	}
	for _, session := range []Session{{}, {Identity: Identity{AccountID: 42}, TitleID: 1}} {
		if got := s.HandleTask(session, counterFields(1, 1)); got.ErrorCode == 0 {
			t.Fatal("invalid identity accepted")
		}
	}
	tooMany := make([]int64, 2*(maxCounterBatch+1))
	for i := 0; i < len(tooMany); i += 2 {
		tooMany[i], tooMany[i+1] = int64(i/2), 1
	}
	if got := s.HandleTask(matchIdentity(42, 1), counterFields(tooMany...)); got.ErrorCode == 0 {
		t.Fatal("oversize batch accepted")
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 25; j++ {
				if got := s.HandleTask(matchIdentity(42, 1), counterFields(1, 1)); got.ErrorCode != 0 {
					t.Error(got.Reason)
					return
				}
			}
		}()
	}
	wg.Wait()
	if s.data.Accounts["42"]["1"] != 200 {
		t.Fatal("concurrent updates lost", s.data)
	}
}

func TestCounterRejectsCorruptStore(t *testing.T) {
	path := filepath.Join(t.TempDir(), "counters.json")
	for _, doc := range []string{"null", "{}", `{"version":1,"accounts":null}`, `{"version":2,"accounts":{}}`, `{"version":1,"accounts":{"0":{}}}`, `{"version":1,"accounts":{"42":{"01":1}}}`, `{"version":1,"accounts":{},"unexpected":true}`, `{"version":1,"accounts":{}} {}`} {
		if err := os.WriteFile(path, []byte(doc), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := NewCounterStore(path); err == nil {
			t.Fatal("corrupt store accepted", doc)
		}
	}
}

func mailboxFields(context string, offset, limit uint32, categories []uint16, newestFirst bool, identity *Identity) []byte {
	w := NewTaggedWriter(1024)
	w.CString(context, 15)
	w.U32(offset)
	w.U32(limit)
	w.Writer.U8(0x6a)
	w.U32(uint32(len(categories)) * 2)
	w.Writer.U32(uint32(len(categories)))
	for _, c := range categories {
		w.Writer.U16(c)
	}
	w.Bool(newestFirst)
	if identity != nil {
		w.U64(identity.AccountID)
		w.CString(identity.Name, 9)
	}
	b, err := w.Bytes()
	if err != nil {
		panic(err)
	}
	return b
}

func mailboxFixture(owner, id uint64, context string, category uint16) MailboxMessage {
	return MailboxMessage{OwnerAccountID: owner, Context: context, IdentityAccountID: 7,
		IdentityAccountType: "psn", Name: "gift", MailID: id, Field88: 123, Field8C: 456,
		Category: category, Flag92: true, Field94: 1, Field98: 2, Field9C: 3}
}

func TestMailboxNativeRequestResultAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "mailbox.json")
	s, err := NewMailboxStore(path)
	if err != nil {
		t.Fatal(err)
	}
	fixture, _ := hex.DecodeString("1000080000000008320000006a08020000000100000002000100")
	if len(fixture) != 26 {
		t.Fatal("native request size")
	}
	if got := s.HandleTask(matchIdentity(42, 1), fixture); got.ErrorCode != 0 || got.Total != 0 || len(got.Records) != 0 {
		t.Fatal("new inbox not empty", got)
	}
	message := mailboxFixture(42, 99, "", 2)
	if err := s.PutMessage(message); err != nil {
		t.Fatal(err)
	}
	if err := s.PutMessage(mailboxFixture(43, 100, "", 2)); err != nil {
		t.Fatal(err)
	}
	reopened, err := NewMailboxStore(path)
	if err != nil {
		t.Fatal(err)
	}
	got := reopened.HandleTask(matchIdentity(42, 1), fixture)
	if got.ErrorCode != 0 || got.Total != 1 || len(got.Records) != 1 {
		t.Fatal(got)
	}
	// Literal independent native metadata layout, including identity, tagged
	// category and Boolean fields. Private owner/context metadata is absent.
	want, _ := hex.DecodeString("0a07000000000000001070736e001067696674000a6300000000000000087b00000008c801000006020001010100080100000008020000000803000000")
	if !bytes.Equal(got.Records[0], want) {
		t.Fatalf("metadata wire mismatch\n got %x\nwant %x", got.Records[0], want)
	}
}

func TestMailboxFilteringPagingOwnershipAndReplacement(t *testing.T) {
	s, err := NewMailboxStore("")
	if err != nil {
		t.Fatal(err)
	}
	for _, row := range []MailboxMessage{
		mailboxFixture(42, 1, "", 2), mailboxFixture(42, 2, "", 3), mailboxFixture(42, 3, "", 2),
		mailboxFixture(43, 4, "", 2), mailboxFixture(42, 5, "other", 2),
	} {
		if err := s.PutMessage(row); err != nil {
			t.Fatal(err)
		}
	}
	session := matchIdentity(42, 1)
	for _, row := range []struct {
		context       string
		offset, limit uint32
		categories    []uint16
		newestFirst   bool
		wantIDs       []uint64
		total         uint32
	}{
		{"", 0, 50, []uint16{2}, false, []uint64{1, 3}, 2},
		{"", 1, 1, []uint16{2}, false, []uint64{3}, 2},
		{"", 0, 50, nil, false, []uint64{1, 2, 3}, 3},
		{"other", 0, 50, []uint16{2}, false, []uint64{5}, 1},
		{"", math.MaxUint32, 50, []uint16{2}, false, nil, 2},
	} {
		got := s.HandleTask(session, mailboxFields(row.context, row.offset, row.limit, row.categories, row.newestFirst, &Identity{AccountID: 42, Name: "psn"}))
		if got.ErrorCode != 0 || got.Total != row.total || len(got.Records) != len(row.wantIDs) {
			t.Fatalf("bad filtered result %#v", got)
		}
		for i, encoded := range got.Records {
			r := NewTaggedReader(encoded)
			r.U64()
			r.CString(9)
			r.CString(63)
			if id := r.U64(); id != row.wantIDs[i] || r.Err() != nil {
				t.Fatal("bad mailbox ordering", id, r.Err())
			}
		}
	}
	if got := s.HandleTask(session, mailboxFields("", 0, 50, nil, false, &Identity{AccountID: 43, Name: "psn"})); got.ErrorCode == 0 {
		t.Fatal("foreign mailbox filter accepted")
	}
	if got := s.HandleTask(session, mailboxFields("", 0, 50, nil, true, nil)); got.ErrorCode == 0 || got.Reason == nil {
		t.Fatal("unsupported true listing flag accepted")
	}
	replacement := mailboxFixture(42, 1, "", 2)
	replacement.Name = "updated"
	if err := s.PutMessage(replacement); err != nil {
		t.Fatal(err)
	}
	if len(s.data.Messages) != 5 || s.data.Messages[0].Name != "updated" {
		t.Fatal("replacement duplicated or failed")
	}
}

func TestMailboxMalformedFieldsAndFailedPersistence(t *testing.T) {
	s, err := NewMailboxStore(filepath.Join(t.TempDir(), "mailbox.json"))
	if err != nil {
		t.Fatal(err)
	}
	good, _ := hex.DecodeString("1000080000000008320000006a08020000000100000002000100")
	for n := 0; n < len(good); n++ {
		if got := s.HandleTask(matchIdentity(42, 1), good[:n]); got.ErrorCode == 0 {
			t.Fatal("truncated query accepted", n)
		}
	}
	for _, at := range []int{0, 2, 7, 12, 13, 14, 18, 24, 25} {
		bad := append([]byte(nil), good...)
		bad[at] = 0xff
		if got := s.HandleTask(matchIdentity(42, 1), bad); got.ErrorCode == 0 {
			t.Fatal("invalid query accepted", at)
		}
	}
	for _, query := range [][]byte{
		append(append([]byte(nil), good...), 0),
		mailboxFields("", 0, 0, nil, false, nil),
		mailboxFields("", 0, 51, nil, false, nil),
		mailboxFields("", 0, 50, make([]uint16, maxMailboxCategories+1), false, nil),
	} {
		if got := s.HandleTask(matchIdentity(42, 1), query); got.ErrorCode == 0 {
			t.Fatal("unbounded/trailing query accepted")
		}
	}
	if got := s.HandleTask(Session{}, good); got.ErrorCode == 0 {
		t.Fatal("unauthenticated query accepted")
	}
	badMessage := mailboxFixture(42, 1, "", 2)
	badMessage.Name = "invalid\x00name"
	if err := s.PutMessage(badMessage); err == nil {
		t.Fatal("invalid metadata persisted")
	}
	s.path = t.TempDir()
	if err := s.PutMessage(mailboxFixture(42, 1, "", 2)); err == nil {
		t.Fatal("failed write reported success")
	}
	if len(s.data.Messages) != 0 {
		t.Fatal("failed write changed live inbox")
	}
}

func TestMailboxRejectsCorruptStore(t *testing.T) {
	path := filepath.Join(t.TempDir(), "mailbox.json")
	for _, doc := range []string{"null", "{}", `{"version":1,"messages":null}`, `{"version":2,"messages":[]}`, `{"version":1,"messages":[],"unexpected":true}`, `{"version":1,"messages":[]} {}`} {
		if err := os.WriteFile(path, []byte(doc), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := NewMailboxStore(path); err == nil {
			t.Fatal("corrupt store accepted", doc)
		}
	}
}
