// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

const (
	maxTitleStoreBytes    = 4 << 20
	maxCounterAccounts    = 4096
	maxCountersPerAccount = 1024
	maxCounterBatch       = 64
	maxMailboxMessages    = 4096
	maxMailboxPerAccount  = 256
	maxMailboxCategories  = 64
)

func titleStoreFailure(err error) TaskResult { return TaskResult{ErrorCode: 3, Reason: err} }

func titleStoreSessionValid(session Session) bool {
	return session.TitleID == D3TitleID && session.Identity.AccountID != 0
}

// readTitleStore only accepts the current schema and one bounded JSON document.
// A missing file is an empty new store; corruption is never silently discarded.
func readTitleStore(path string, dest any) (bool, error) {
	if path == "" {
		return false, nil
	}
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, maxTitleStoreBytes+1))
	if err != nil {
		return false, err
	}
	if len(b) > maxTitleStoreBytes {
		return false, ErrLimit
	}
	if trimmed := bytes.TrimSpace(b); len(trimmed) == 0 || trimmed[0] != '{' {
		return false, ErrValue
	}
	d := json.NewDecoder(bytes.NewReader(b))
	d.DisallowUnknownFields()
	if err := d.Decode(dest); err != nil {
		return false, err
	}
	if err := d.Decode(new(any)); err != io.EOF {
		return false, ErrTrailing
	}
	return true, nil
}

// writeTitleStore publishes a complete snapshot through a same-directory rename.
// The caller updates its in-memory view only after the snapshot is committed.
func writeTitleStore(path string, value any) error {
	if path == "" {
		return nil
	}
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	if len(b) > maxTitleStoreBytes {
		return ErrLimit
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".d3-title-store-*")
	if err != nil {
		return err
	}
	name := f.Name()
	defer os.Remove(name)
	if _, err := f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(name, path)
}

type counterSnapshot struct {
	Version  int                         `json:"version"`
	Accounts map[string]map[string]int64 `json:"accounts"`
}

// CounterStore retains service-23/task-1 counts per authenticated title account.
// The wire request carries only counter IDs and deltas, never an account ID.
type CounterStore struct {
	mu   sync.Mutex
	path string
	data counterSnapshot
}

func NewCounterStore(path string) (*CounterStore, error) {
	s := &CounterStore{path: path}
	exists, err := readTitleStore(path, &s.data)
	if err != nil {
		return nil, fmt.Errorf("load title counters: %w", err)
	}
	if !exists {
		s.data = counterSnapshot{Version: 1, Accounts: make(map[string]map[string]int64)}
	}
	if s.data.Version != 1 || s.data.Accounts == nil || len(s.data.Accounts) > maxCounterAccounts {
		return nil, fmt.Errorf("load title counters: %w", ErrValue)
	}
	for account, counters := range s.data.Accounts {
		id, err := strconv.ParseUint(account, 10, 64)
		if err != nil || id == 0 || strconv.FormatUint(id, 10) != account || counters == nil || len(counters) > maxCountersPerAccount {
			return nil, fmt.Errorf("load title counters: %w", ErrValue)
		}
		for counter := range counters {
			id, err := strconv.ParseUint(counter, 10, 32)
			if err != nil || strconv.FormatUint(id, 10) != counter {
				return nil, fmt.Errorf("load title counters: %w", ErrValue)
			}
		}
	}
	if !exists {
		if err := writeTitleStore(path, s.data); err != nil {
			return nil, fmt.Errorf("create title counters: %w", err)
		}
	}
	return s, nil
}

func (s *CounterStore) HandleTask(session Session, data []byte) TaskResult {
	if !titleStoreSessionValid(session) {
		return titleStoreFailure(errors.New("counter account unavailable"))
	}
	type change struct {
		id    uint32
		delta int64
	}
	changes := make([]change, 0, 4)
	r := NewTaggedReader(data)
	for r.Remaining() != 0 && r.Err() == nil {
		if len(changes) >= maxCounterBatch {
			return titleStoreFailure(ErrLimit)
		}
		changes = append(changes, change{r.U32(), r.I64()})
	}
	if err := r.Done(); err != nil {
		return titleStoreFailure(fmt.Errorf("invalid counter increment fields: %w", err))
	}
	if len(changes) == 0 {
		return titleStoreFailure(ErrValue)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	account := strconv.FormatUint(session.Identity.AccountID, 10)
	old, exists := s.data.Accounts[account]
	if !exists && len(s.data.Accounts) >= maxCounterAccounts {
		return titleStoreFailure(ErrLimit)
	}
	next := make(map[string]int64, len(old)+len(changes))
	for id, value := range old {
		next[id] = value
	}
	for _, c := range changes {
		id := strconv.FormatUint(uint64(c.id), 10)
		value := next[id]
		if (c.delta > 0 && value > math.MaxInt64-c.delta) || (c.delta < 0 && value < math.MinInt64-c.delta) {
			return titleStoreFailure(errors.New("counter overflow"))
		}
		next[id] = value + c.delta
	}
	if len(next) > maxCountersPerAccount {
		return titleStoreFailure(ErrLimit)
	}
	snapshot := counterSnapshot{Version: 1, Accounts: make(map[string]map[string]int64, len(s.data.Accounts)+1)}
	for id, counters := range s.data.Accounts {
		snapshot.Accounts[id] = counters
	}
	snapshot.Accounts[account] = next
	if err := writeTitleStore(s.path, snapshot); err != nil {
		return titleStoreFailure(fmt.Errorf("persist counter increments: %w", err))
	}
	s.data = snapshot
	return TaskResult{}
}

// MailboxMessage preserves the native task-11 metadata fields. Fields whose
// meaning has not been established retain their native object offsets. Owner
// and context are private-backend routing metadata and are not sent on the wire.
type MailboxMessage struct {
	OwnerAccountID      uint64 `json:"owner_account_id"`
	Context             string `json:"context"`
	IdentityAccountID   uint64 `json:"identity_account_id"`
	IdentityAccountType string `json:"identity_account_type"`
	Name                string `json:"name"`
	MailID              uint64 `json:"mail_id"`
	Field88             uint32 `json:"field_88"`
	Field8C             uint32 `json:"field_8c"`
	Category            uint16 `json:"category"`
	Flag92              bool   `json:"flag_92"`
	Flag93              bool   `json:"flag_93"`
	Field94             uint32 `json:"field_94"`
	Field98             uint32 `json:"field_98"`
	Field9C             uint32 `json:"field_9c"`
}

type mailboxSnapshot struct {
	Version  int              `json:"version"`
	Messages []MailboxMessage `json:"messages"`
}

type MailboxStore struct {
	mu   sync.Mutex
	path string
	data mailboxSnapshot
}

func mailboxStringValid(s string, max int) bool {
	return len(s) <= max && strings.IndexByte(s, 0) < 0
}

func mailboxMessageValid(m MailboxMessage) bool {
	return m.OwnerAccountID != 0 && m.MailID != 0 && mailboxStringValid(m.Context, 15) &&
		mailboxStringValid(m.IdentityAccountType, 9) && mailboxStringValid(m.Name, 63)
}

func validateMailboxSnapshot(snapshot mailboxSnapshot) error {
	if snapshot.Version != 1 || snapshot.Messages == nil || len(snapshot.Messages) > maxMailboxMessages {
		return ErrValue
	}
	type key struct {
		owner   uint64
		context string
		id      uint64
	}
	seen := make(map[key]struct{}, len(snapshot.Messages))
	counts := make(map[uint64]int)
	for _, m := range snapshot.Messages {
		if !mailboxMessageValid(m) {
			return ErrValue
		}
		k := key{m.OwnerAccountID, m.Context, m.MailID}
		if _, duplicate := seen[k]; duplicate {
			return ErrValue
		}
		seen[k] = struct{}{}
		counts[m.OwnerAccountID]++
		if counts[m.OwnerAccountID] > maxMailboxPerAccount {
			return ErrLimit
		}
	}
	return nil
}

func NewMailboxStore(path string) (*MailboxStore, error) {
	s := &MailboxStore{path: path}
	exists, err := readTitleStore(path, &s.data)
	if err != nil {
		return nil, fmt.Errorf("load title mailbox: %w", err)
	}
	if !exists {
		s.data = mailboxSnapshot{Version: 1, Messages: make([]MailboxMessage, 0)}
	}
	if err := validateMailboxSnapshot(s.data); err != nil {
		return nil, fmt.Errorf("load title mailbox: %w", err)
	}
	if !exists {
		if err := writeTitleStore(path, s.data); err != nil {
			return nil, fmt.Errorf("create title mailbox: %w", err)
		}
	}
	return s, nil
}

// PutMessage is an internal/operator API, not a guest request. The listing task
// cannot create messages, claim rewards or choose another account's mailbox.
func (s *MailboxStore) PutMessage(message MailboxMessage) error {
	if !mailboxMessageValid(message) {
		return ErrValue
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	next := mailboxSnapshot{Version: 1, Messages: append(make([]MailboxMessage, 0, len(s.data.Messages)+1), s.data.Messages...)}
	replaced := false
	for i, old := range next.Messages {
		if old.OwnerAccountID == message.OwnerAccountID && old.Context == message.Context && old.MailID == message.MailID {
			next.Messages[i], replaced = message, true
			break
		}
	}
	if !replaced {
		next.Messages = append(next.Messages, message)
	}
	if err := validateMailboxSnapshot(next); err != nil {
		return err
	}
	if err := writeTitleStore(s.path, next); err != nil {
		return err
	}
	s.data = next
	return nil
}

func encodeMailboxMessage(m MailboxMessage) ([]byte, error) {
	w := NewTaggedWriter(512)
	w.U64(m.IdentityAccountID)
	w.CString(m.IdentityAccountType, 9)
	w.CString(m.Name, 63)
	w.U64(m.MailID)
	w.U32(m.Field88)
	w.U32(m.Field8C)
	w.U16(m.Category)
	w.Bool(m.Flag92)
	w.Bool(m.Flag93)
	w.U32(m.Field94)
	w.U32(m.Field98)
	w.U32(m.Field9C)
	return w.Bytes()
}

func (s *MailboxStore) HandleTask(session Session, data []byte) TaskResult {
	if !titleStoreSessionValid(session) {
		return titleStoreFailure(errors.New("mailbox account unavailable"))
	}
	r := NewTaggedReader(data)
	context := r.CString(15)
	offset, limit := r.U32(), r.U32()
	// Native dd9b20: array tag U16+100, typed byte length, raw count,
	// then untagged little-endian U16 elements. dd9c20 adds no end byte.
	if !r.expect(0x6a) {
		return titleStoreFailure(fmt.Errorf("invalid mailbox categories: %w", r.Err()))
	}
	arrayBytes, count := r.U32(), r.Reader.U32()
	if r.Err() != nil || count > maxMailboxCategories || arrayBytes != count*2 || limit == 0 || limit > 50 {
		return titleStoreFailure(ErrValue)
	}
	categories := make(map[uint16]struct{}, count)
	for i := uint32(0); i < count; i++ {
		categories[r.Reader.U16()] = struct{}{}
	}
	unsupportedFlag := r.Bool()
	// The optional bdUserAccountID filter may only select the authenticated
	// account. Its account-type string is bounded but never grants authority.
	if r.Remaining() != 0 && r.Err() == nil {
		id := r.U64()
		r.CString(9)
		if r.Err() == nil && id != session.Identity.AccountID {
			return titleStoreFailure(errors.New("mailbox filter belongs to another account"))
		}
	}
	if err := r.Done(); err != nil {
		return titleStoreFailure(fmt.Errorf("invalid mailbox listing fields: %w", err))
	}
	if unsupportedFlag {
		return titleStoreFailure(errors.New("mailbox listing flag true is unsupported by the recovered title contract"))
	}
	s.mu.Lock()
	rows := make([]MailboxMessage, 0)
	for _, m := range s.data.Messages {
		if m.OwnerAccountID != session.Identity.AccountID || m.Context != context {
			continue
		}
		if len(categories) != 0 {
			if _, selected := categories[m.Category]; !selected {
				continue
			}
		}
		rows = append(rows, m)
	}
	s.mu.Unlock()
	// Ascending mail ID is the private backend's deterministic pagination order.
	// The native Boolean's semantics remain unknown; the observed title sends
	// false. Unsupported true requests are rejected rather than guessed.
	sort.Slice(rows, func(i, j int) bool {
		return rows[i].MailID < rows[j].MailID
	})
	result := TaskResult{Total: uint32(len(rows))}
	start := uint64(offset)
	if start > uint64(len(rows)) {
		start = uint64(len(rows))
	}
	end := start + uint64(limit)
	if end > uint64(len(rows)) {
		end = uint64(len(rows))
	}
	for _, m := range rows[start:end] {
		encoded, err := encodeMailboxMessage(m)
		if err != nil {
			return titleStoreFailure(err)
		}
		result.Records = append(result.Records, encoded)
	}
	return result
}
