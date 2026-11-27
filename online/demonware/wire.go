// SPDX-License-Identifier: GPL-2.0-or-later
// Package demonware implements the private title service's transport building
// blocks. Game-specific authentication and task handlers must validate their
// own message schemas; these primitives do not acknowledge unsupported tasks.
package demonware

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"strings"
)

var (
	ErrLimit    = errors.New("Demonware message exceeds limit")
	ErrValue    = errors.New("invalid Demonware field value")
	ErrTrailing = errors.New("unexpected trailing Demonware fields")
)

const DefaultMaxFrame = 1 << 20

// ReadFrame reads one little-endian length-prefixed lobby record. An empty
// record is retained for the caller to process as a keepalive. The size is
// checked before allocation, including signed-negative wire lengths.
func ReadFrame(r io.Reader, limit uint32) ([]byte, error) {
	if limit == 0 {
		limit = DefaultMaxFrame
	}
	var header [4]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	n := binary.LittleEndian.Uint32(header[:])
	if n > limit || n > math.MaxInt32 {
		return nil, ErrLimit
	}
	data := make([]byte, int(n))
	if _, err := io.ReadFull(r, data); err != nil {
		return nil, err
	}
	return data, nil
}

// WriteFrame writes a complete record even when the stream accepts partial
// writes. A broken writer returning (0, nil) cannot cause a busy loop.
func WriteFrame(w io.Writer, data []byte, limit uint32) error {
	if limit == 0 {
		limit = DefaultMaxFrame
	}
	if uint64(len(data)) > uint64(limit) || len(data) > math.MaxInt32 {
		return ErrLimit
	}
	var header [4]byte
	binary.LittleEndian.PutUint32(header[:], uint32(len(data)))
	for _, part := range [][]byte{header[:], data} {
		for len(part) != 0 {
			n, err := w.Write(part)
			if n < 0 || n > len(part) {
				return io.ErrShortWrite
			}
			part = part[n:]
			if err != nil {
				return err
			}
			if n == 0 {
				return io.ErrShortWrite
			}
		}
	}
	return nil
}

// Reader decodes untagged little-endian fields and bounded strings/blobs. A
// caller parsing a typed bdByteBuffer must explicitly consume and check tags.
// Field errors are sticky so a truncated request cannot become valid zeros.
type Reader struct {
	data []byte
	at   int
	err  error
}

func NewReader(data []byte) *Reader { return &Reader{data: data} }
func (r *Reader) take(n int) []byte {
	if r.err != nil {
		return nil
	}
	if n < 0 || n > len(r.data)-r.at {
		r.err = io.ErrUnexpectedEOF
		return nil
	}
	b := r.data[r.at : r.at+n]
	r.at += n
	return b
}
func (r *Reader) Remaining() int { return len(r.data) - r.at }
func (r *Reader) Err() error     { return r.err }
func (r *Reader) Done() error {
	if r.err != nil {
		return r.err
	}
	if r.Remaining() != 0 {
		return ErrTrailing
	}
	return nil
}
func (r *Reader) U8() uint8 {
	b := r.take(1)
	if b == nil {
		return 0
	}
	return b[0]
}
func (r *Reader) Bool() bool {
	v := r.U8()
	if v > 1 {
		r.err = ErrValue
	}
	return v == 1
}
func (r *Reader) U16() uint16 {
	b := r.take(2)
	if b == nil {
		return 0
	}
	return binary.LittleEndian.Uint16(b)
}
func (r *Reader) U32() uint32 {
	b := r.take(4)
	if b == nil {
		return 0
	}
	return binary.LittleEndian.Uint32(b)
}
func (r *Reader) U64() uint64 {
	b := r.take(8)
	if b == nil {
		return 0
	}
	return binary.LittleEndian.Uint64(b)
}
func (r *Reader) F32() float32     { return math.Float32frombits(r.U32()) }
func (r *Reader) Raw(n int) []byte { return r.take(n) }
func (r *Reader) CString(limit int) string {
	if r.err != nil {
		return ""
	}
	if limit < 0 {
		r.err = ErrLimit
		return ""
	}
	end := len(r.data) - r.at
	if end > limit && limit < math.MaxInt {
		end = limit + 1
	}
	n := bytes.IndexByte(r.data[r.at:r.at+end], 0)
	if n < 0 {
		if len(r.data)-r.at > limit {
			r.err = ErrLimit
		} else {
			r.err = io.ErrUnexpectedEOF
		}
		return ""
	}
	value := string(r.take(n + 1))
	return value[:n]
}
func (r *Reader) Blob(limit uint32) []byte {
	n := r.U32()
	if r.err != nil {
		return nil
	}
	if n > limit {
		r.err = ErrLimit
		return nil
	}
	return r.take(int(n))
}

// Writer bounds the total encoded message, including field headers.
type Writer struct {
	data  []byte
	limit int
	err   error
}

func NewWriter(limit int) *Writer {
	if limit <= 0 {
		limit = DefaultMaxFrame
	}
	return &Writer{limit: limit}
}
func (w *Writer) reserve(n int) []byte {
	if w.err != nil {
		return nil
	}
	if n < 0 || n > w.limit-len(w.data) {
		w.err = ErrLimit
		return nil
	}
	at := len(w.data)
	w.data = append(w.data, make([]byte, n)...)
	return w.data[at:]
}
func (w *Writer) Bytes() ([]byte, error) {
	if w.err != nil {
		return nil, w.err
	}
	return w.data, nil
}
func (w *Writer) U8(v uint8) {
	b := w.reserve(1)
	if b != nil {
		b[0] = v
	}
}
func (w *Writer) Bool(v bool) {
	if v {
		w.U8(1)
	} else {
		w.U8(0)
	}
}
func (w *Writer) U16(v uint16) {
	b := w.reserve(2)
	if b != nil {
		binary.LittleEndian.PutUint16(b, v)
	}
}
func (w *Writer) U32(v uint32) {
	b := w.reserve(4)
	if b != nil {
		binary.LittleEndian.PutUint32(b, v)
	}
}
func (w *Writer) U64(v uint64) {
	b := w.reserve(8)
	if b != nil {
		binary.LittleEndian.PutUint64(b, v)
	}
}
func (w *Writer) F32(v float32) { w.U32(math.Float32bits(v)) }
func (w *Writer) Raw(v []byte)  { b := w.reserve(len(v)); copy(b, v) }
func (w *Writer) CString(v string, limit int) {
	if len(v) > limit || strings.IndexByte(v, 0) >= 0 {
		w.err = ErrValue
		return
	}
	b := w.reserve(len(v) + 1)
	if b != nil {
		copy(b, v)
	}
}
func (w *Writer) Blob(v []byte, limit uint32) {
	if uint64(len(v)) > uint64(limit) || uint64(len(v)) > math.MaxUint32 {
		w.err = ErrLimit
		return
	}
	w.U32(uint32(len(v)))
	w.Raw(v)
}
