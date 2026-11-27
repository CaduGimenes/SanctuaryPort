// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import "math"

// This build's bdByteBuffer writers are observed at eboot virtual addresses
// dd99d0 (u8), dd9ad0 (u32), dda520 (i32), ddaf70 (blob). Integer maximum values
// use the one-byte sentinel 0x14. Blob lengths are themselves typed u32 fields.
type TaggedReader struct{ *Reader }

func NewTaggedReader(data []byte) *TaggedReader { return &TaggedReader{NewReader(data)} }
func (r *TaggedReader) expect(tag uint8) bool {
	got := r.Reader.U8()
	if r.err != nil {
		return false
	}
	if got != tag {
		r.err = ErrValue
		return false
	}
	return true
}
func (r *TaggedReader) U8() uint8 {
	if !r.expect(3) {
		return 0
	}
	return r.Reader.U8()
}
func (r *TaggedReader) U16() uint16 {
	if !r.expect(6) {
		return 0
	}
	return r.Reader.U16()
}
func (r *TaggedReader) Bool() bool {
	if !r.expect(1) {
		return false
	}
	return r.Reader.Bool()
}
func (r *TaggedReader) I64() int64 {
	tag := r.Reader.U8()
	if r.err != nil {
		return 0
	}
	if tag == 0x14 {
		return math.MaxInt64
	}
	if tag != 9 {
		r.err = ErrValue
		return 0
	}
	return int64(r.Reader.U64())
}
func (r *TaggedReader) CString(limit int) string {
	if !r.expect(16) {
		return ""
	}
	return r.Reader.CString(limit)
}
func (r *TaggedReader) U32() uint32 {
	tag := r.Reader.U8()
	if r.err != nil {
		return 0
	}
	if tag == 0x14 {
		return math.MaxUint32
	}
	if tag != 8 {
		r.err = ErrValue
		return 0
	}
	return r.Reader.U32()
}
func (r *TaggedReader) I32() int32 {
	tag := r.Reader.U8()
	if r.err != nil {
		return 0
	}
	if tag == 0x14 {
		return math.MaxInt32
	}
	if tag != 7 {
		r.err = ErrValue
		return 0
	}
	return int32(r.Reader.U32())
}
func (r *TaggedReader) U64() uint64 {
	tag := r.Reader.U8()
	if r.err != nil {
		return 0
	}
	if tag == 0x14 {
		return math.MaxUint64
	}
	if tag != 10 {
		r.err = ErrValue
		return 0
	}
	return r.Reader.U64()
}
func (r *TaggedReader) Blob(limit uint32) []byte {
	if !r.expect(0x13) {
		return nil
	}
	n := r.U32()
	if r.err != nil {
		return nil
	}
	if n > limit {
		r.err = ErrLimit
		return nil
	}
	return r.Raw(int(n))
}

type TaggedWriter struct{ *Writer }

func NewTaggedWriter(limit int) *TaggedWriter { return &TaggedWriter{NewWriter(limit)} }
func (w *TaggedWriter) U8(v uint8)            { w.Writer.U8(3); w.Writer.U8(v) }
func (w *TaggedWriter) U16(v uint16)          { w.Writer.U8(6); w.Writer.U16(v) }
func (w *TaggedWriter) Bool(v bool)           { w.Writer.U8(1); w.Writer.Bool(v) }
func (w *TaggedWriter) I64(v int64) {
	if v == math.MaxInt64 {
		w.Writer.U8(0x14)
		return
	}
	w.Writer.U8(9)
	w.Writer.U64(uint64(v))
}
func (w *TaggedWriter) CString(v string, limit int) { w.Writer.U8(16); w.Writer.CString(v, limit) }
func (w *TaggedWriter) U64(v uint64) {
	if v == math.MaxUint64 {
		w.Writer.U8(0x14)
		return
	}
	w.Writer.U8(10)
	w.Writer.U64(v)
}
func (w *TaggedWriter) U32(v uint32) {
	if v == math.MaxUint32 {
		w.Writer.U8(0x14)
		return
	}
	w.Writer.U8(8)
	w.Writer.U32(v)
}
func (w *TaggedWriter) I32(v int32) {
	if v == math.MaxInt32 {
		w.Writer.U8(0x14)
		return
	}
	w.Writer.U8(7)
	w.Writer.U32(uint32(v))
}
func (w *TaggedWriter) Blob(data []byte, limit uint32) {
	if uint64(len(data)) > uint64(limit) {
		w.err = ErrLimit
		return
	}
	w.Writer.U8(0x13)
	w.U32(uint32(len(data)))
	w.Raw(data)
}
