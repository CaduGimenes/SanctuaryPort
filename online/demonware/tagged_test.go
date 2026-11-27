// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/hex"
	"errors"
	"math"
	"testing"
)

func TestNativeTaggedFieldsAndSentinels(t *testing.T) {
	// Service21 task5's query fields and SessionID blob use these encodings.
	golden, _ := hex.DecodeString("030507ffffffff14785634120812345678141308080000000102030405060708")
	// Correct the explicit i32 value: tag7 followed by LE(-1), then sentinel.
	golden, _ = hex.DecodeString("030507ffffffff140812345678141308080000000102030405060708")
	r := NewTaggedReader(golden)
	if r.U8() != 5 || r.I32() != -1 || r.I32() != math.MaxInt32 || r.U32() != 0x78563412 || r.U32() != math.MaxUint32 || !bytes.Equal(r.Blob(8), []byte{1, 2, 3, 4, 5, 6, 7, 8}) || r.Done() != nil {
		t.Fatal("native tagged fixture failed", r.Err())
	}
	w := NewTaggedWriter(100)
	w.U8(5)
	w.I32(-1)
	w.I32(math.MaxInt32)
	w.U32(0x78563412)
	w.U32(math.MaxUint32)
	w.Blob([]byte{1, 2, 3, 4, 5, 6, 7, 8}, 8)
	b, err := w.Bytes()
	if err != nil || !bytes.Equal(b, golden) {
		t.Fatalf("native tagged fixture differs: %x %v", b, err)
	}
	for n := 0; n < len(golden); n++ {
		r = NewTaggedReader(golden[:n])
		r.U8()
		r.I32()
		r.I32()
		r.U32()
		r.U32()
		r.Blob(8)
		if r.Done() == nil {
			t.Fatal("truncated fields accepted", n)
		}
	}
	r = NewTaggedReader([]byte{7, 0, 0, 0, 0})
	r.U32()
	if !errors.Is(r.Done(), ErrValue) {
		t.Fatal("wrong field type accepted")
	}
	r = NewTaggedReader([]byte{0x13, 0x14})
	r.Blob(255)
	if !errors.Is(r.Done(), ErrLimit) {
		t.Fatal("sentinel blob length allocated")
	}
}

func TestNativeTitleFieldFixtures(t *testing.T) {
	// Native dda420: uint16 tag 6, dd9f80: bool tag 1, dda7b0:
	// int64 tag 9/max sentinel, and publisher strings: tag 16 + NUL.
	golden, _ := hex.DecodeString("06ffff010109feffffffffffffff1410436f6e6669672e74787400")
	r := NewTaggedReader(golden)
	if r.U16() != 65535 || !r.Bool() || r.I64() != -2 || r.I64() != math.MaxInt64 || r.CString(127) != "Config.txt" || r.Done() != nil {
		t.Fatal("native title fields rejected", r.Err())
	}
	w := NewTaggedWriter(128)
	w.U16(65535)
	w.Bool(true)
	w.I64(-2)
	w.I64(math.MaxInt64)
	w.CString("Config.txt", 127)
	b, err := w.Bytes()
	if err != nil || !bytes.Equal(b, golden) {
		t.Fatalf("native title fields differ: %x %v", b, err)
	}
	for n := 0; n < len(golden); n++ {
		r := NewTaggedReader(golden[:n])
		r.U16()
		r.Bool()
		r.I64()
		r.I64()
		r.CString(127)
		if r.Done() == nil {
			t.Fatalf("truncated title fields accepted at %d", n)
		}
	}
	r = NewTaggedReader([]byte{1, 2})
	r.Bool()
	if !errors.Is(r.Done(), ErrValue) {
		t.Fatal("invalid bool accepted")
	}
	r = NewTaggedReader([]byte{5, 0, 0})
	r.U16()
	if !errors.Is(r.Done(), ErrValue) {
		t.Fatal("signed uint16 tag accepted")
	}
}
