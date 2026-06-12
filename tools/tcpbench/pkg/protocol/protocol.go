// Package protocol implements a simple length-prefixed frame protocol.
// Each frame: 2-byte big-endian length + payload of that length.
// Maximum payload size is 65535 bytes (uint16 max).
package protocol

import (
	"encoding/binary"
	"io"
	"sync"
)

const (
	// HeaderSize is the size of the length prefix in bytes.
	HeaderSize = 2
	// MaxPayloadSize is the maximum payload size in bytes.
	MaxPayloadSize = 65535
)

// bufferPool provides reusable byte slices for reading frame payloads.
// Using a pool eliminates allocations in the hot path of the echo server.
var bufferPool = sync.Pool{
	New: func() interface{} {
		buf := make([]byte, MaxPayloadSize)
		return &buf
	},
}

// GetBuffer returns a pooled buffer for reading payload data.
// The buffer has capacity MaxPayloadSize.
func GetBuffer() *[]byte {
	return bufferPool.Get().(*[]byte)
}

// PutBuffer returns a buffer to the pool for reuse.
func PutBuffer(buf *[]byte) {
	bufferPool.Put(buf)
}

// ReadFrame reads a complete frame and returns its payload.
// It allocates a new []byte for the payload.
func ReadFrame(r io.Reader) ([]byte, error) {
	var header [HeaderSize]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint16(header[:])
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

// ReadFrameInto reads a frame, placing the payload into the provided pooled buffer.
// The buffer must have at least 'length' bytes of capacity.
// Returns a slice of buf pointing to the payload data.
func ReadFrameInto(r io.Reader, buf *[]byte) ([]byte, error) {
	var header [HeaderSize]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}
	length := binary.BigEndian.Uint16(header[:])
	payload := (*buf)[:length]
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

// WriteFrame writes a complete frame (header + payload) to the writer.
func WriteFrame(w io.Writer, payload []byte) error {
	var header [HeaderSize]byte
	binary.BigEndian.PutUint16(header[:], uint16(len(payload)))
	if _, err := w.Write(header[:]); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}
