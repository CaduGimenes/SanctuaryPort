// SPDX-License-Identifier: GPL-2.0-or-later
package demonware

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/sha1"
	"encoding/binary"
	"errors"
	"io"
)

// This title negotiates lobby protocol 210..220. Use 210, whose encrypted
// records have no protocol-220 compression/multipacket extension.
const lobbyProtocol = 210

var ErrLobbyProtocol = errors.New("invalid private Demonware lobby protocol")

// Public identity context at native eboot 1115eb0 (294-byte DER public key).
// Its SHA-256 starts with 356c4bc3, the auth request's identity. The game uses
// these public bytes as HKDF info, not as a private signing key. Every private
// auth reply includes identity, selecting this derivation in native e17540.
const lobbyIdentityContext = "\x30\x82\x01\x22\x30\x0d\x06\x09\x2a\x86\x48\x86" +
	"\xf7\x0d\x01\x01\x01\x05\x00\x03\x82\x01\x0f\x00" +
	"\x30\x82\x01\x0a\x02\x82\x01\x01\x00\xc0\xa2\x0b" +
	"\x1f\x6c\xb8\x1b\x12\x70\xed\x1a\xef\x30\x6c\x75" +
	"\x9d\xc1\x08\x89\x99\xf0\x2a\xc8\xac\x2f\xc7\xd5" +
	"\xd0\x3b\x61\x29\x39\xf3\x8f\x62\x39\xda\xf1\x20" +
	"\x11\xe7\x92\xe9\x16\x24\x22\x96\x09\x9e\xac\x19" +
	"\xcd\x24\x3e\x58\xc6\x40\x86\x78\xd7\xdf\x70\x77" +
	"\xcb\xde\x80\x42\xb1\x38\xf3\x1d\x6a\x3c\x98\xe4" +
	"\x85\xdb\xfb\x53\x3a\x86\x47\xce\x58\xb1\xd3\xd7" +
	"\x0b\x83\x3d\x14\x6b\xda\x40\x24\x1f\x16\x2b\x0e" +
	"\x49\x22\xe4\xb7\x63\xff\xaa\x40\xc2\x44\xdf\xdc" +
	"\x3f\x8c\x1e\x60\xb4\x6f\x3e\xda\xb2\x4e\x50\xca" +
	"\xfc\x62\x4b\x62\xc7\xe1\x77\x5e\x83\xcd\xe0\xb5" +
	"\xfc\xc6\xaa\xa0\xc2\x6b\x28\xcc\x8a\xa7\x95\x7b" +
	"\x1e\x67\xe0\x5b\xaf\xc6\x54\x49\xe6\xac\x7a\x8d" +
	"\x1d\xe6\x7d\x12\x04\x94\xc3\x23\x4a\x00\x60\x58" +
	"\x33\x6f\xe7\x94\x19\xff\xf6\xe0\xc6\x40\x50\xb7" +
	"\x9d\x0e\xcd\xdf\xe7\x92\x5d\x84\x94\x13\x06\x61" +
	"\xbc\x44\x75\x54\x70\x54\x77\x4c\xc0\x28\x7d\xfc" +
	"\xc9\x9a\x92\x38\xd4\xd5\xee\xf3\x27\x44\x66\x13" +
	"\x2c\x06\xf0\x64\xe7\xec\xf8\x75\xfd\x15\xd4\x1b" +
	"\x91\x45\x9d\x4a\x3f\x40\xe9\x35\x53\x7f\xfc\x96" +
	"\x61\xe1\x48\x74\x21\xf0\x04\x20\x41\x30\x02\xd2" +
	"\xf9\x02\x03\x01\x00\x01"

type lobbyKeys struct {
	clientProof, serverProof [8]byte
	clientMAC, serverMAC     [20]byte
	clientAES, serverAES     [16]byte
}

func hmacSHA1(key, data []byte) []byte {
	h := hmac.New(sha1.New, key)
	_, _ = h.Write(data)
	return h.Sum(nil)
}

// The game's dddd90/dddef0 implement HKDF-SHA1. Its identity branch first
// expands the ticket key with the public identity context, then extracts with
// the transcript hash as salt and expands CLIENTCHAL and BDDATA.
func expandSHA1(key []byte, label string, length int) []byte {
	result := make([]byte, 0, length+sha1.Size)
	var previous []byte
	for counter := byte(1); len(result) < length; counter++ {
		data := append(append(append([]byte(nil), previous...), label...), counter)
		previous = hmacSHA1(key, data)
		result = append(result, previous...)
	}
	return result[:length]
}

func deriveLobbyKeys(key, transcript []byte) (lobbyKeys, error) {
	var out lobbyKeys
	if len(key) != 24 || len(transcript) > 4096 {
		return out, ErrLobbyProtocol
	}
	hash := sha1.Sum(transcript)
	identityKey := expandSHA1(key, lobbyIdentityContext, 24)
	prk := hmacSHA1(hash[:], identityKey)
	proof := expandSHA1(prk, "CLIENTCHAL", 16)
	data := expandSHA1(prk, "BDDATA", 72)
	copy(out.clientProof[:], proof[:8])
	copy(out.serverProof[:], proof[8:])
	copy(out.clientMAC[:], data[:20])
	copy(out.serverMAC[:], data[20:40])
	copy(out.clientAES[:], data[40:56])
	copy(out.serverAES[:], data[56:72])
	return out, nil
}

func framedLobbyRecord(payload []byte) []byte {
	frame := make([]byte, 4+len(payload))
	binary.LittleEndian.PutUint32(frame, uint32(len(payload)))
	copy(frame[4:], payload)
	return frame
}

func encryptLobbyRecord(payload []byte, kind byte, count uint32, key, mac []byte, random io.Reader) ([]byte, error) {
	if len(key) != 16 || len(mac) != 20 || len(payload) > DefaultMaxFrame-64 || count == 0 {
		return nil, ErrLobbyProtocol
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}
	clear := make([]byte, (len(payload)+5+15)&^15)
	binary.LittleEndian.PutUint32(clear, uint32(len(payload)))
	clear[4] = kind
	copy(clear[5:], payload)
	frame := make([]byte, 26+len(clear)+8)
	binary.LittleEndian.PutUint32(frame, uint32(len(frame)-4))
	frame[4], frame[5] = 0xab, 0x85
	binary.LittleEndian.PutUint32(frame[6:], count)
	if _, err := io.ReadFull(random, frame[10:26]); err != nil {
		return nil, err
	}
	cipher.NewCBCEncrypter(block, frame[10:26]).CryptBlocks(frame[26:len(frame)-8], clear)
	copy(frame[len(frame)-8:], hmacSHA1(mac, frame[:len(frame)-8])[:8])
	return frame, nil
}

func decryptLobbyRecord(frame []byte, count uint32, key, mac []byte) ([]byte, byte, error) {
	if len(key) != 16 || len(mac) != 20 || len(frame) < 50 || len(frame) > DefaultMaxFrame ||
		binary.LittleEndian.Uint32(frame) != uint32(len(frame)-4) || frame[4] != 0xab ||
		frame[5] != 0x85 || binary.LittleEndian.Uint32(frame[6:]) != count || count == 0 ||
		(len(frame)-34)%16 != 0 {
		return nil, 0, ErrLobbyProtocol
	}
	if !hmac.Equal(frame[len(frame)-8:], hmacSHA1(mac, frame[:len(frame)-8])[:8]) {
		return nil, 0, ErrLobbyProtocol
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, 0, err
	}
	clear := make([]byte, len(frame)-34)
	cipher.NewCBCDecrypter(block, frame[10:26]).CryptBlocks(clear, frame[26:len(frame)-8])
	n := binary.LittleEndian.Uint32(clear)
	if uint64(n) > uint64(len(clear)-5) || len(clear)-5-int(n) >= 16 {
		return nil, 0, ErrLobbyProtocol
	}
	for _, b := range clear[5+int(n):] {
		if b != 0 {
			return nil, 0, ErrLobbyProtocol
		}
	}
	return clear[5 : 5+int(n)], clear[4], nil
}

// bdBitBuffer writes least-significant bits first, with five-bit type tags.
// The auth record contains a leading 0x07, a true bit, two typed u32 values,
// and the unaligned 128-byte opaque server ticket (e17540/dd7f70/dd8540).
func parseLobbyAuth(payload []byte) (title, seed uint32, ticket []byte, err error) {
	if len(payload) != 139 {
		return 0, 0, nil, ErrLobbyProtocol
	}
	at := 0
	bits := func(n int) uint64 {
		var value uint64
		for i := 0; i < n; i++ {
			value |= uint64((payload[at/8]>>uint(at%8))&1) << uint(i)
			at++
		}
		return value
	}
	if bits(8) != 7 || bits(1) != 1 || bits(5) != 8 {
		return 0, 0, nil, ErrLobbyProtocol
	}
	title = uint32(bits(32))
	if bits(5) != 8 {
		return 0, 0, nil, ErrLobbyProtocol
	}
	seed = uint32(bits(32))
	ticket = make([]byte, 128)
	for i := range ticket {
		ticket[i] = byte(bits(8))
	}
	return title, seed, ticket, nil
}
