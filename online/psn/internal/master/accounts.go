// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"regexp"
)

var onlineID = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_-]{2,15}$`)

const passwordRounds = 200000

// Accounts are provisioned by the operator. There is no unauthenticated registration API.
type Account struct {
	ID   uint64 `json:"id"`
	Salt string `json:"salt"`
	Hash string `json:"hash"`
}
type Accounts map[string]Account

// Name-only players need no registration. Their account ID stays consistent
// across reconnects and server restarts without an accounts file.
func (s *Server) account(name string) (Account, bool) {
	if s.cfg.Accounts != nil {
		a, ok := s.cfg.Accounts[name]
		return a, ok
	}
	if !onlineID.MatchString(name) {
		return Account{}, false
	}
	digest := sha256.Sum256([]byte("d3pc-player:" + name))
	id := binary.LittleEndian.Uint64(digest[:8]) & 0x7fffffffffffffff
	if id == 0 {
		id = 1
	}
	return Account{ID: id}, true
}

func LoadAccounts(path string) (Accounts, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var a Accounts
	if err = json.Unmarshal(b, &a); err != nil {
		return nil, err
	}
	ids := map[uint64]bool{}
	for name, v := range a {
		salt, e1 := hex.DecodeString(v.Salt)
		hash, e2 := hex.DecodeString(v.Hash)
		if !onlineID.MatchString(name) || v.ID == 0 || ids[v.ID] || e1 != nil || e2 != nil || len(salt) != 16 || len(hash) != 32 {
			return nil, errors.New("invalid accounts file")
		}
		ids[v.ID] = true
	}
	return a, nil
}
func (a Accounts) Authenticate(name, password string) (Account, bool) {
	v, ok := a[name]
	salt, _ := hex.DecodeString(v.Salt)
	if !ok {
		salt = make([]byte, 16)
	}
	key, _ := pbkdf2.Key(sha256.New, password, salt, passwordRounds, 32)
	expected, _ := hex.DecodeString(v.Hash)
	return v, subtle.ConstantTimeCompare(key, expected) == 1 && ok
}
func AddAccount(path, name, password string) error {
	if !onlineID.MatchString(name) || len(password) < 12 || len(password) > 256 {
		return errors.New("online ID must be 3–16 ASCII characters; password must be 12–256 bytes")
	}
	a, err := LoadAccounts(path)
	if os.IsNotExist(err) {
		a = Accounts{}
	} else if err != nil {
		return err
	}
	if _, exists := a[name]; exists {
		return errors.New("account already exists")
	}
	id := uint64(1)
	for _, v := range a {
		if v.ID >= id {
			id = v.ID + 1
		}
	}
	salt := make([]byte, 16)
	if _, err = rand.Read(salt); err != nil {
		return err
	}
	key, err := pbkdf2.Key(sha256.New, password, salt, passwordRounds, 32)
	if err != nil {
		return err
	}
	a[name] = Account{ID: id, Salt: hex.EncodeToString(salt), Hash: hex.EncodeToString(key)}
	b, err := json.MarshalIndent(a, "", "  ")
	if err != nil {
		return err
	}
	f, err := os.OpenFile(path+".new", os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	defer os.Remove(path + ".new")
	if _, err = f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(path+".new", path)
}
