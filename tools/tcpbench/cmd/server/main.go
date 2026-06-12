// TCP Echo Server — high-performance server for benchmarking.
// Uses the 2-byte length-prefixed frame protocol. Echoes received frames back to clients.
//
// Usage:
//
//	server -port 7777 -buffer 262144
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"sync/atomic"
	"time"

	"tcpbench/pkg/protocol"
)

var (
	port       = flag.Int("port", 7777, "TCP listen port")
	bufferSize = flag.Int("buffer", 256*1024, "Socket read/write buffer size in bytes")
)

// Global counters — all accessed atomically for lock-free operation.
var (
	totalConns   int64
	currentConns int64
	totalReqs    int64
	totalBytes   int64
)

func main() {
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	addr := fmt.Sprintf(":%d", *port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("Failed to listen on %s: %v", addr, err)
	}
	defer ln.Close()

	log.Printf("=== TCP Echo Server ===")
	log.Printf("Listening on %s", addr)
	log.Printf("Socket buffer: %d bytes", *bufferSize)
	log.Printf("Protocol: 2-byte length + payload (max %d bytes)", protocol.MaxPayloadSize)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Handle Ctrl+C gracefully
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	go func() {
		<-sigCh
		log.Println("Shutting down...")
		cancel()
		ln.Close()
	}()

	// Periodic stats reporter
	go reportLoop(ctx)

	// Accept loop
	for {
		conn, err := ln.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				log.Println("Server stopped.")
				return
			default:
				continue
			}
		}
		atomic.AddInt64(&totalConns, 1)
		atomic.AddInt64(&currentConns, 1)
		go handleConn(conn)
	}
}

// handleConn processes a single client connection.
// It reads frames and echoes them back until the connection closes.
func handleConn(conn net.Conn) {
	defer func() {
		conn.Close()
		atomic.AddInt64(&currentConns, -1)
	}()

	// Configure TCP for low latency
	if tcpConn, ok := conn.(*net.TCPConn); ok {
		tcpConn.SetNoDelay(true)                // disable Nagle
		tcpConn.SetReadBuffer(*bufferSize)      // large receive window
		tcpConn.SetWriteBuffer(*bufferSize)     // large send buffer
	}

	br := bufio.NewReaderSize(conn, *bufferSize)
	bw := bufio.NewWriterSize(conn, *bufferSize)

	// Reuse one pooled buffer for all frames on this connection
	bufPtr := protocol.GetBuffer()
	defer protocol.PutBuffer(bufPtr)

	for {
		// Read 2-byte length header
		var header [protocol.HeaderSize]byte
		if _, err := io.ReadFull(br, header[:]); err != nil {
			return // client disconnected — normal, no log needed
		}
		length := uint16(header[0])<<8 | uint16(header[1])

		// Read payload into pooled buffer
		payload := (*bufPtr)[:length]
		if _, err := io.ReadFull(br, payload); err != nil {
			return
		}

		// Echo: write header + payload back
		if _, err := bw.Write(header[:]); err != nil {
			return
		}
		if _, err := bw.Write(payload); err != nil {
			return
		}
		if err := bw.Flush(); err != nil {
			return
		}

		// Update counters atomically
		atomic.AddInt64(&totalReqs, 1)
		atomic.AddInt64(&totalBytes, int64(protocol.HeaderSize+int(length)))
	}
}

// reportLoop prints server statistics every second.
func reportLoop(ctx context.Context) {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	var lastReqs, lastBytes int64

	for {
		select {
		case <-ticker.C:
			reqs := atomic.LoadInt64(&totalReqs)
			bytes := atomic.LoadInt64(&totalBytes)

			reqRate := reqs - lastReqs
			byteRate := bytes - lastBytes
			lastReqs = reqs
			lastBytes = bytes

			if reqRate > 0 {
				log.Printf("REQS: %s/s | DATA: %s/s | CONNS: %d/%d | TOTAL: %s",
					formatNum(reqRate),
					formatBytes(byteRate),
					atomic.LoadInt64(&currentConns),
					atomic.LoadInt64(&totalConns),
					formatNum(reqs),
				)
			}
		case <-ctx.Done():
			return
		}
	}
}

// formatNum formats an integer with K/M/G suffix for readability.
func formatNum(n int64) string {
	switch {
	case n >= 1000000000:
		return fmt.Sprintf("%.2fG", float64(n)/1e9)
	case n >= 1000000:
		return fmt.Sprintf("%.2fM", float64(n)/1e6)
	case n >= 1000:
		return fmt.Sprintf("%.2fK", float64(n)/1e3)
	default:
		return fmt.Sprintf("%d", n)
	}
}

// formatBytes formats bytes with binary (KiB/MiB/GiB) suffixes.
func formatBytes(n int64) string {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.2f GiB", float64(n)/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.2f MiB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.2f KiB", float64(n)/(1<<10))
	default:
		return fmt.Sprintf("%d B", n)
	}
}
