// TCP Benchmark Client — high-performance TCP stress testing tool.
//
// Connects to a server using N concurrent connections, each pipelining M requests,
// for a total of N*M in-flight requests. Measures throughput and latency distribution.
//
// Usage:
//
//	client -addr 127.0.0.1:7777 -conn 100 -size 64 -dur 10s
package main

import (
	"bufio"
	"crypto/rand"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"time"

	"tcpbench/pkg/protocol"
)

// ── Command-line flags ──────────────────────────────────────────

var (
	serverAddr   = flag.String("addr", "127.0.0.1:7777", "Server address (host:port)")
	connections  = flag.Int("conn", 100, "Number of concurrent TCP connections")
	payloadSize  = flag.Int("size", 64, "Payload size in bytes (1-65535)")
	duration     = flag.Duration("dur", 10*time.Second, "Benchmark measurement duration")
	pipelineSize = flag.Int("pipeline", 100, "Pipeline depth — requests sent in one batch per connection")
	warmupDur    = flag.Duration("warmup", 1*time.Second, "Warm-up duration (sends traffic but doesn't record metrics)")
	bufferSize   = flag.Int("buffer", 256*1024, "Socket read/write buffer size in bytes")
)

// ── Latency Histogram (lock-free with atomic buckets) ───────────

// LatencyHistogram tracks the distribution of request latencies.
// It uses predefined time buckets with atomic counters for lock-free recording.
type LatencyHistogram struct {
	bounds  []time.Duration // bucket upper bounds (exclusive)
	buckets []int64         // atomic counters per bucket; last bucket = overflow
	count   int64           // total number of recorded latencies
	totalNs int64           // sum of all latencies in nanoseconds (for avg)
}

// newLatencyHistogram creates a histogram with predefined microsecond-to-millisecond buckets.
func newLatencyHistogram() *LatencyHistogram {
	bounds := []time.Duration{
		50 * time.Microsecond,
		100 * time.Microsecond,
		200 * time.Microsecond,
		500 * time.Microsecond,
		1 * time.Millisecond,
		2 * time.Millisecond,
		5 * time.Millisecond,
		10 * time.Millisecond,
		50 * time.Millisecond,
		100 * time.Millisecond,
		500 * time.Millisecond,
	}
	return &LatencyHistogram{
		bounds:  bounds,
		buckets: make([]int64, len(bounds)+1), // +1 for overflow (>500ms)
	}
}

// Record adds a latency observation to the histogram. Safe for concurrent use.
func (h *LatencyHistogram) Record(d time.Duration) {
	atomic.AddInt64(&h.count, 1)
	atomic.AddInt64(&h.totalNs, d.Nanoseconds())

	for i, bound := range h.bounds {
		if d <= bound {
			atomic.AddInt64(&h.buckets[i], 1)
			return
		}
	}
	// Falls into the overflow bucket
	atomic.AddInt64(&h.buckets[len(h.buckets)-1], 1)
}

// Count returns the total number of recorded observations.
func (h *LatencyHistogram) Count() int64 {
	return atomic.LoadInt64(&h.count)
}

// Avg returns the arithmetic mean of recorded latencies.
func (h *LatencyHistogram) Avg() time.Duration {
	c := atomic.LoadInt64(&h.count)
	if c == 0 {
		return 0
	}
	return time.Duration(atomic.LoadInt64(&h.totalNs) / c)
}

// Percentile returns the approximate p-th percentile latency (0.0 to 1.0).
// Uses the histogram bucket boundaries as approximation points.
func (h *LatencyHistogram) Percentile(p float64) time.Duration {
	total := atomic.LoadInt64(&h.count)
	if total == 0 {
		return 0
	}

	target := int64(float64(total) * p)
	if target >= total {
		target = total - 1
	}

	var cum int64
	for i, bound := range h.bounds {
		cum += atomic.LoadInt64(&h.buckets[i])
		if cum > target {
			return bound
		}
	}
	// Overflow bucket
	return h.bounds[len(h.bounds)-1]
}

// ── Metrics ─────────────────────────────────────────────────────

// Metrics holds all benchmark counters. All fields are accessed atomically.
type Metrics struct {
	totalReqs  int64
	totalBytes int64
	totalErrs  int64
	histogram  *LatencyHistogram
}

// ── Main Entry Point ────────────────────────────────────────────

func main() {
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	// ── Validate arguments ──
	if *payloadSize < 1 || *payloadSize > protocol.MaxPayloadSize {
		log.Fatalf("Payload size must be between 1 and %d bytes", protocol.MaxPayloadSize)
	}
	if *connections < 1 {
		log.Fatal("Number of connections (-conn) must be at least 1")
	}
	if *pipelineSize < 1 {
		log.Fatal("Pipeline size (-pipeline) must be at least 1")
	}

	// ── Print configuration ──
	fmt.Println("╔══════════════════════════════════════════════╗")
	fmt.Println("║          TCP BENCHMARK CLIENT               ║")
	fmt.Println("╚══════════════════════════════════════════════╝")
	fmt.Printf("  Server:        %s\n", *serverAddr)
	fmt.Printf("  Connections:   %d\n", *connections)
	fmt.Printf("  Payload:       %d bytes\n", *payloadSize)
	fmt.Printf("  Pipeline:      %d reqs/batch\n", *pipelineSize)
	fmt.Printf("  Total inflight: %d reqs\n", *connections**pipelineSize)
	fmt.Printf("  Duration:      %s\n", *duration)
	fmt.Printf("  Warmup:        %s\n", *warmupDur)
	fmt.Printf("  Socket buffer: %d bytes\n", *bufferSize)
	fmt.Println()

	metrics := &Metrics{
		histogram: newLatencyHistogram(),
	}

	// ── Generate random payload ──
	payload := make([]byte, *payloadSize)
	if _, err := rand.Read(payload); err != nil {
		log.Fatalf("Failed to generate random payload: %v", err)
	}

	// ── Timing ──
	warmupEnd := time.Now().Add(*warmupDur)
	benchEnd := time.Now().Add(*warmupDur + *duration)

	log.Printf("Warming up for %s...", *warmupDur)

	// ── Periodic stats reporter (runs in background) ──
	done := make(chan struct{})
	go reportLoop(metrics, done, warmupEnd)

	// ── Handle Ctrl+C ──
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)
	go func() {
		<-sigCh
		fmt.Println("\n\nInterrupted. Draining in-flight requests...")
		time.Sleep(200 * time.Millisecond)
		os.Exit(1)
	}()

	// ── Launch connection workers ──
	var wg sync.WaitGroup
	for i := 0; i < *connections; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			runWorker(id, payload, benchEnd, warmupEnd, metrics)
		}(i)
	}

	wg.Wait()
	close(done)

	// ── Final report ──
	printFinalReport(metrics)
}

// ── Connection Worker ───────────────────────────────────────────

// runWorker manages a single TCP connection to the server.
// It pipelines requests: sends a batch, flushes, then reads the batch of responses.
func runWorker(id int, payload []byte, benchEnd, warmupEnd time.Time, m *Metrics) {
	// Connect with retry
	var conn net.Conn
	var err error
	for attempt := 0; attempt < 3; attempt++ {
		conn, err = net.DialTimeout("tcp", *serverAddr, 5*time.Second)
		if err == nil {
			break
		}
		if attempt < 2 {
			time.Sleep(100 * time.Millisecond)
		}
	}
	if err != nil {
		log.Printf("[w%d] Failed to connect: %v", id, err)
		return
	}
	defer conn.Close()

	// Configure TCP for low latency and high throughput
	if tcpConn, ok := conn.(*net.TCPConn); ok {
		tcpConn.SetNoDelay(true)
		tcpConn.SetReadBuffer(*bufferSize)
		tcpConn.SetWriteBuffer(*bufferSize)
	}

	br := bufio.NewReaderSize(conn, *bufferSize)
	bw := bufio.NewWriterSize(conn, *bufferSize)

	// Inflight timestamp ring — records send time for each request in the current batch
	inflight := make([]time.Time, *pipelineSize)

	for time.Now().Before(benchEnd) {
		inMeasurement := time.Now().After(warmupEnd)

		// ── Send pipeline ──
		for i := 0; i < *pipelineSize; i++ {
			if inMeasurement {
				inflight[i] = time.Now()
			}
			if err := protocol.WriteFrame(bw, payload); err != nil {
				atomic.AddInt64(&m.totalErrs, 1)
				return
			}
		}
		if err := bw.Flush(); err != nil {
			atomic.AddInt64(&m.totalErrs, 1)
			return
		}

		// ── Receive pipeline ──
		for i := 0; i < *pipelineSize; i++ {
			_, err := protocol.ReadFrame(br)
			if err != nil {
				if err != io.EOF {
					atomic.AddInt64(&m.totalErrs, 1)
				}
				return
			}

			// Only record metrics after warmup
			if inMeasurement {
				elapsed := time.Since(inflight[i])
				atomic.AddInt64(&m.totalReqs, 1)
				atomic.AddInt64(&m.totalBytes, int64(*payloadSize+protocol.HeaderSize))
				m.histogram.Record(elapsed)
			}
		}
	}
}

// ── Periodic Reporter ───────────────────────────────────────────

// reportLoop prints throughput and latency stats every second.
func reportLoop(m *Metrics, done chan struct{}, warmupEnd time.Time) {
	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	var lastReqs, lastBytes int64

	for {
		select {
		case <-ticker.C:
			reqs := atomic.LoadInt64(&m.totalReqs)
			bytes := atomic.LoadInt64(&m.totalBytes)
			errs := atomic.LoadInt64(&m.totalErrs)

			reqRate := reqs - lastReqs
			byteRate := bytes - lastBytes
			lastReqs = reqs
			lastBytes = bytes

			if time.Now().Before(warmupEnd) {
				fmt.Printf("[%s] ⏳ warming up...\n", time.Now().Format("15:04:05"))
			} else if reqRate > 0 {
				fmt.Printf("[%s] %s r/s | %s/s | avg: %s | p99: %s | errs: %d | total: %s\n",
					time.Now().Format("15:04:05"),
					formatNum(reqRate),
					formatBytes(byteRate),
					m.histogram.Avg().Round(time.Microsecond),
					m.histogram.Percentile(0.99).Round(time.Microsecond),
					errs,
					formatNum(reqs),
				)
			}
		case <-done:
			return
		}
	}
}

// ── Final Report ────────────────────────────────────────────────

func printFinalReport(m *Metrics) {
	totalReqs := atomic.LoadInt64(&m.totalReqs)
	totalErrs := atomic.LoadInt64(&m.totalErrs)
	totalBytes := atomic.LoadInt64(&m.totalBytes)

	// Calculate actual measurement duration and rates
	actualSec := (*duration).Seconds()
	if actualSec < 0.001 {
		actualSec = 0.001
	}

	reqRate := int64(float64(totalReqs) / actualSec)
	byteRate := int64(float64(totalBytes) / actualSec)

	fmt.Println()
	fmt.Println("╔══════════════════════════════════════════════╗")
	fmt.Println("║              FINAL REPORT                   ║")
	fmt.Println("╚══════════════════════════════════════════════╝")
	fmt.Printf("  Measurement duration: %s\n", *duration)
	fmt.Printf("  Total requests:       %s\n", formatNum(totalReqs))
	if totalErrs > 0 {
		fmt.Printf("  Total errors:         %s", formatNum(totalErrs))
		if totalReqs > 0 {
			fmt.Printf(" (%.4f%%)", float64(totalErrs)/float64(totalReqs+totalErrs)*100)
		}
		fmt.Println()
	} else {
		fmt.Println("  Total errors:         0 ✓")
	}
	fmt.Printf("  Avg throughput:       %s req/s\n", formatNum(reqRate))
	fmt.Printf("  Avg bandwidth:        %s/s\n", formatBytes(byteRate))
	fmt.Println()

	if m.histogram.Count() > 0 {
		fmt.Println("  ── Latency Distribution ──")
		fmt.Printf("  Avg:     %s\n", m.histogram.Avg().Round(time.Microsecond))
		fmt.Printf("  P50:     %s\n", m.histogram.Percentile(0.50).Round(time.Microsecond))
		fmt.Printf("  P75:     %s\n", m.histogram.Percentile(0.75).Round(time.Microsecond))
		fmt.Printf("  P90:     %s\n", m.histogram.Percentile(0.90).Round(time.Microsecond))
		fmt.Printf("  P99:     %s\n", m.histogram.Percentile(0.99).Round(time.Microsecond))
		fmt.Printf("  P99.9:   %s\n", m.histogram.Percentile(0.999).Round(time.Microsecond))
		fmt.Println()
	}
}

// ── Formatting Helpers ──────────────────────────────────────────

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
