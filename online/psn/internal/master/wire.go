// SPDX-License-Identifier: GPL-2.0-or-later
package master

import (
	"encoding/binary"
	"errors"
	"io"

	pb "sanctuaryport/online/psn/internal/protocol"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
)

const headerSize = 15
const maxPacket = 256 * 1024

type packet struct {
	kind    byte
	command uint16
	id      uint64
	body    []byte
}

func readPacket(r io.Reader) (packet, error) {
	var h [headerSize]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return packet{}, err
	}
	size := binary.LittleEndian.Uint32(h[3:7])
	if size < headerSize || size > maxPacket {
		return packet{}, errors.New("invalid packet size")
	}
	p := packet{kind: h[0], command: binary.LittleEndian.Uint16(h[1:3]), id: binary.LittleEndian.Uint64(h[7:]), body: make([]byte, int(size)-headerSize)}
	_, err := io.ReadFull(r, p.body)
	return p, err
}

func writePacket(w io.Writer, p packet) error {
	if len(p.body) > 8*1024*1024-headerSize {
		return errors.New("reply too large")
	}
	b := make([]byte, headerSize+len(p.body))
	b[0] = p.kind
	binary.LittleEndian.PutUint16(b[1:], p.command)
	binary.LittleEndian.PutUint32(b[3:], uint32(len(b)))
	binary.LittleEndian.PutUint64(b[7:], p.id)
	copy(b[headerSize:], p.body)
	for len(b) > 0 {
		n, err := w.Write(b)
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		b = b[n:]
	}
	return nil
}

func blob(m proto.Message) []byte {
	b, err := proto.Marshal(m)
	if err != nil {
		panic(err)
	} // only server-owned generated messages
	out := make([]byte, 4+len(b))
	binary.LittleEndian.PutUint32(out, uint32(len(b)))
	copy(out[4:], b)
	return out
}

func decode(b []byte, m proto.Message) error {
	if len(b) < 4 || int(binary.LittleEndian.Uint32(b)) != len(b)-4 {
		return errors.New("invalid protobuf envelope")
	}
	if err := (proto.UnmarshalOptions{DiscardUnknown: true, RecursionLimit: 32}).Unmarshal(b[4:], m); err != nil {
		return err
	}
	if !validMessage(m.ProtoReflect()) {
		return errors.New("attribute or list exceeds protocol limits")
	}
	return nil
}

func validMessage(m protoreflect.Message) bool {
	switch a := m.Interface().(type) {
	case *pb.MatchingBinAttr:
		if a.AttrId < 0x54 || a.AttrId > 0x5f || len(a.Data) > 4096 {
			return false
		}
	case *pb.MatchingIntAttr:
		if a.AttrId < 0x4c || a.AttrId > 0x53 {
			return false
		}
	}
	valid := true
	m.Range(func(f protoreflect.FieldDescriptor, v protoreflect.Value) bool {
		if f.IsList() {
			list := v.List()
			if list.Len() > 128 {
				valid = false
				return false
			}
			if f.Message() != nil {
				for i := 0; i < list.Len(); i++ {
					if !validMessage(list.Get(i).Message()) {
						valid = false
						return false
					}
				}
			}
		}
		if !f.IsList() && f.Message() != nil && !validMessage(v.Message()) {
			valid = false
			return false
		}
		return true
	})
	return valid
}
