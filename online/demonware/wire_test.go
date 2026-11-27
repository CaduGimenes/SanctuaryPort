// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"testing"
)

func fixture(t *testing.T) []byte {
	t.Helper()
	b, err := hex.DecodeString("7f01cdab1234567808070605040302010000c03f416c6963650003000000000102")
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func consume(r *Reader) {
	r.U8()
	r.Bool()
	r.U16()
	r.U32()
	r.U64()
	r.F32()
	r.CString(16)
	r.Blob(16)
}
func TestWireFields(t *testing.T) {
	r := NewReader(fixture(t))
	if r.U8() != 0x7f || !r.Bool() || r.U16() != 0xabcd || r.U32() != 0x78563412 || r.U64() != 0x0102030405060708 || r.F32() != 1.5 || r.CString(16) != "Alice" || !bytes.Equal(r.Blob(16), []byte{0, 1, 2}) || r.Done() != nil {
		t.Fatal("little-endian fixture did not decode", r.Err())
	}
	w := NewWriter(100)
	w.U8(0x7f)
	w.Bool(true)
	w.U16(0xabcd)
	w.U32(0x78563412)
	w.U64(0x0102030405060708)
	w.F32(1.5)
	w.CString("Alice", 16)
	w.Blob([]byte{0, 1, 2}, 16)
	b, err := w.Bytes()
	if err != nil || !bytes.Equal(b, fixture(t)) {
		t.Fatalf("fixture mismatch: %x, %v", b, err)
	}
}
func TestTruncatedAndInvalidFields(t *testing.T) {
	b := fixture(t)
	for n := 0; n < len(b); n++ {
		r := NewReader(b[:n])
		consume(r)
		if r.Done() == nil {
			t.Fatalf("truncated request of %d bytes accepted", n)
		}
	}
	r := NewReader(append(b, 0xff))
	consume(r)
	if !errors.Is(r.Done(), ErrTrailing) {
		t.Fatal("trailing input accepted")
	}
	r = NewReader([]byte{2})
	r.Bool()
	if !errors.Is(r.Done(), ErrValue) {
		t.Fatal("non-canonical boolean accepted")
	}
	r = NewReader([]byte("abcdefghijklmnopq\x00"))
	r.CString(16)
	if !errors.Is(r.Done(), ErrLimit) {
		t.Fatal("overlong player name accepted")
	}
	r = NewReader([]byte("Alice"))
	r.CString(16)
	if !errors.Is(r.Done(), io.ErrUnexpectedEOF) {
		t.Fatal("unterminated player name accepted")
	}
	r = NewReader([]byte{0xff, 0xff, 0xff, 0xff})
	r.Blob(65536)
	if !errors.Is(r.Done(), ErrLimit) {
		t.Fatal("unbounded blob length accepted")
	}
	w := NewWriter(4)
	w.Blob([]byte{1}, 16)
	if _, err := w.Bytes(); !errors.Is(err, ErrLimit) {
		t.Fatal("total output limit ignored")
	}
	w = NewWriter(100)
	w.CString("Al\x00ice", 16)
	if _, err := w.Bytes(); !errors.Is(err, ErrValue) {
		t.Fatal("embedded string terminator accepted")
	}
}

type shortWriter struct{ bytes.Buffer }

func (w *shortWriter) Write(b []byte) (int, error) {
	if len(b) > 2 {
		b = b[:2]
	}
	return w.Buffer.Write(b)
}

type stuckWriter struct{}

func (stuckWriter) Write([]byte) (int, error) { return 0, nil }
func TestFramingFragmentationAndLimits(t *testing.T) {
	var wire shortWriter
	for _, b := range [][]byte{nil, fixture(t), []byte("next record")} {
		if err := WriteFrame(&wire, b, 100); err != nil {
			t.Fatal(err)
		}
	}
	reader := bytes.NewReader(wire.Bytes())
	for _, want := range [][]byte{nil, fixture(t), []byte("next record")} {
		got, err := ReadFrame(reader, 100)
		if err != nil || !bytes.Equal(got, want) {
			t.Fatalf("frame boundary lost: %x, %v", got, err)
		}
	}
	if _, err := ReadFrame(reader, 100); !errors.Is(err, io.EOF) {
		t.Fatal(err)
	}
	if err := WriteFrame(stuckWriter{}, []byte("test"), 100); !errors.Is(err, io.ErrShortWrite) {
		t.Fatal("zero writer hung or accepted", err)
	}
	for _, length := range []uint32{101, 0x80000000, 0xffffffff} {
		var h [4]byte
		binary.LittleEndian.PutUint32(h[:], length)
		reader = bytes.NewReader(append(h[:], 0xaa))
		if _, err := ReadFrame(reader, 100); !errors.Is(err, ErrLimit) || reader.Len() != 1 {
			t.Fatalf("invalid length consumed payload: %x, %v", length, err)
		}
	}
	for n := 0; n < 4; n++ {
		if _, err := ReadFrame(bytes.NewReader([]byte{1, 0, 0, 0}[:n]), 100); err == nil {
			t.Fatalf("short header %d accepted", n)
		}
	}
	if _, err := ReadFrame(bytes.NewReader([]byte{3, 0, 0, 0, 1, 2}), 100); !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatal("short body accepted", err)
	}
}
func FuzzReaderBounds(f *testing.F) {
	f.Add([]byte{})
	f.Add([]byte{0xff, 0xff, 0xff, 0xff})
	f.Add([]byte("Alice\x00"))
	f.Fuzz(func(t *testing.T, data []byte) {
		r := NewReader(data)
		r.U8()
		r.Bool()
		r.U16()
		r.U32()
		r.U64()
		r.F32()
		r.CString(16)
		r.Blob(65536)
		r.Raw(5)
		_ = r.Done()
		if r.Remaining() < 0 || r.Remaining() > len(data) {
			t.Fatal("reader escaped frame")
		}
	})
}
