// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Go's column of the live-path comparison: net/http plus coder/websocket, no framework.
// The same shape as node/live-bare.mjs, deliberately, so a reader can put the two files
// next to each other and see that the only difference is the runtime.
//
// No router and no framework, because this is a floor column: it exists to say what the
// fastest honest Go is, so that "SynQt is fast for a Qt thing" and "SynQt is fast" can be
// told apart. A router here would be measuring the router.
//
// Publisher and subscribers share one process, as they do in every other column, so this
// times an interval on one monotonic clock rather than across two. See COLUMN-CONTRACT.md.
package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/coder/websocket"
)

// The process's monotonic origin. Every stamp is microseconds since this, on both ends, so
// what is reported is an interval.
//
// The Go-specific trap this avoids: time.Now() is not monotonic across a clock change, but
// the *difference* between two time.Time values is, because Go carries a monotonic reading
// inside the value and subtraction uses it. Subtract; never convert to Unix and subtract
// that, which throws the monotonic reading away.
var origin = time.Now()

func nowMicros() uint64 {
	return uint64(time.Since(origin).Microseconds())
}

// Resident set size as the OS reports it, which is what every other column reports. Go's
// runtime.MemStats is the heap the runtime knows about and is a different measurement, so
// it does not go in this field (COLUMN-CONTRACT.md, "The output").
func residentBytes() uint64 {
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(data), "\n") {
		if !strings.HasPrefix(line, "VmRSS:") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return 0
		}
		kib, err := strconv.ParseUint(fields[1], 10, 64)
		if err != nil {
			return 0
		}
		return kib * 1024
	}
	return 0
}

// Process CPU in milliseconds, user plus system, to compare against the other columns'
// figure. All of them report "CPU this process burned", which is what a host is sized on.
func cpuMilliseconds() float64 {
	var usage syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &usage); err != nil {
		return 0
	}
	micros := func(t syscall.Timeval) float64 {
		return float64(t.Sec)*1e6 + float64(t.Usec)
	}
	return (micros(usage.Utime) + micros(usage.Stime)) / 1000
}

type distribution struct {
	Unit    string  `json:"unit"`
	Samples int     `json:"samples"`
	Min     float64 `json:"min"`
	P50     float64 `json:"p50"`
	P95     float64 `json:"p95"`
	P99     float64 `json:"p99"`
	Max     float64 `json:"max"`
	Mean    float64 `json:"mean"`
}

// Linear interpolation between ranks, exactly as measure.mjs does it. A nearest-rank
// percentile would disagree with every other column at small sample counts, which is the
// kind of difference that reads as a fact about the runtime.
func summarize(samples []float64) distribution {
	sorted := append([]float64(nil), samples...)
	sort.Float64s(sorted)
	at := func(fraction float64) float64 {
		if len(sorted) == 0 {
			return 0
		}
		rank := fraction * float64(len(sorted)-1)
		low := int(rank)
		high := low
		if rank > float64(low) {
			high = low + 1
		}
		if high >= len(sorted) {
			high = len(sorted) - 1
		}
		if low == high {
			return sorted[low]
		}
		return sorted[low] + (rank-float64(low))*(sorted[high]-sorted[low])
	}
	total := 0.0
	for _, value := range sorted {
		total += value
	}
	result := distribution{Unit: "ms", Samples: len(sorted)}
	if len(sorted) > 0 {
		result.Min = sorted[0]
		result.Max = sorted[len(sorted)-1]
		result.Mean = total / float64(len(sorted))
		result.P50 = at(0.5)
		result.P95 = at(0.95)
		result.P99 = at(0.99)
	}
	return result
}

type entry struct {
	Subscribers      int          `json:"subscribers"`
	Propagation      distribution `json:"propagation"`
	ThroughputPerSec float64      `json:"throughput_msgs_per_sec"`
	CPUMsPer1k       float64      `json:"cpu_ms_per_1k"`
	RSSPerConn       float64      `json:"rss_bytes_per_conn"`
	RSSTotal         uint64       `json:"rss_total_bytes"`
	Delivered        int          `json:"delivered"`
	Expected         int          `json:"expected"`
}

type result struct {
	Benchmark    string  `json:"benchmark"`
	Stack        string  `json:"stack"`
	Path         string  `json:"path"`
	GoVersion    string  `json:"go_version"`
	Host         string  `json:"host"`
	Arch         string  `json:"arch"`
	Recorded     string  `json:"recorded"`
	RSSAvailable bool    `json:"rss_available"`
	Hz           int     `json:"hz"`
	Saturated    bool    `json:"saturated"`
	Seconds      int     `json:"seconds"`
	PayloadBytes int     `json:"payload_bytes"`
	Sweep        []entry `json:"sweep"`
}

// One broadcast server: it accepts subscribers and hands the same bytes to every one of
// them. Nothing is read back from a subscriber, because in this workload they never send.
type broadcastServer struct {
	listener net.Listener
	http     *http.Server
	mu       sync.Mutex
	clients  []*websocket.Conn
}

func startBroadcastServer() (*broadcastServer, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	server := &broadcastServer{listener: listener}
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(writer http.ResponseWriter, request *http.Request) {
		conn, err := websocket.Accept(writer, request, nil)
		if err != nil {
			return
		}
		// The publisher writes a 256-byte payload every tick and nothing reads back, so the
		// default read limit is left alone and the connection is simply held.
		server.mu.Lock()
		server.clients = append(server.clients, conn)
		server.mu.Unlock()
		<-request.Context().Done()
	})
	server.http = &http.Server{Handler: mux}
	go func() {
		_ = server.http.Serve(listener)
	}()
	return server, nil
}

func (s *broadcastServer) port() int {
	return s.listener.Addr().(*net.TCPAddr).Port
}

func (s *broadcastServer) clientCount() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.clients)
}

// Build the frame once, then hand the same bytes to every socket, as every other column
// does. Writing per subscriber would measure the framing N times over.
func (s *broadcastServer) broadcast(ctx context.Context, frame []byte) {
	s.mu.Lock()
	clients := append([]*websocket.Conn(nil), s.clients...)
	s.mu.Unlock()
	for _, conn := range clients {
		_ = conn.Write(ctx, websocket.MessageBinary, frame)
	}
}

func (s *broadcastServer) close() {
	s.mu.Lock()
	clients := s.clients
	s.clients = nil
	s.mu.Unlock()
	for _, conn := range clients {
		_ = conn.CloseNow()
	}
	_ = s.http.Close()
}

func parseSizes(value string) []int {
	sizes := []int{}
	for _, part := range strings.Split(value, ",") {
		size, err := strconv.Atoi(strings.TrimSpace(part))
		if err == nil && size > 0 {
			sizes = append(sizes, size)
		}
	}
	return sizes
}

func main() {
	subscribersFlag := flag.String("subscribers", "10,50,100,250", "comma-separated subscriber counts")
	secondsFlag := flag.Int("seconds", 5, "measured window per size")
	hzFlag := flag.Int("hz", 30, "publish rate")
	payloadFlag := flag.Int("payload", 256, "payload bytes per frame")
	saturateFlag := flag.Bool("saturate", false, "publish as fast as the fleet keeps up")
	outFlag := flag.String("out", "", "write the result JSON here")
	flag.Parse()

	sizes := parseSizes(*subscribersFlag)
	seconds := max(1, *secondsFlag)
	hz := max(1, *hzFlag)
	payloadBytes := max(0, *payloadFlag)

	rate := "saturation"
	if !*saturateFlag {
		rate = fmt.Sprintf("%d Hz", hz)
	}
	fmt.Printf("Go (bare) live path: %ds at %s, %d byte payload, subscribers %s\n",
		seconds, rate, payloadBytes, *subscribersFlag)

	payload := make([]byte, payloadBytes)
	for i := range payload {
		payload[i] = 'x'
	}
	baselineRss := residentBytes()
	sweep := []entry{}
	ctx := context.Background()

	for _, subscriberCount := range sizes {
		server, err := startBroadcastServer()
		if err != nil {
			fmt.Fprintln(os.Stderr, "could not start the server:", err)
			os.Exit(1)
		}

		// One slice of samples per subscriber rather than one shared slice behind a mutex:
		// N goroutines contending on one lock inside the receive path would be measuring
		// the lock, and the lock is the harness rather than the runtime.
		samples := make([][]float64, subscriberCount)
		var measuring atomic.Bool
		var deliveredThisFrame atomic.Int64
		fleet := make(chan struct{}, 1)

		conns := make([]*websocket.Conn, 0, subscriberCount)
		var readers sync.WaitGroup
		for index := 0; index < subscriberCount; index++ {
			conn, _, err := websocket.Dial(ctx,
				fmt.Sprintf("ws://127.0.0.1:%d/", server.port()), nil)
			if err != nil {
				fmt.Fprintf(os.Stderr, "subscriber %d did not connect: %v\n", index, err)
				os.Exit(1)
			}
			conns = append(conns, conn)
			readers.Add(1)
			go func(index int, conn *websocket.Conn) {
				defer readers.Done()
				for {
					kind, frame, err := conn.Read(ctx)
					if err != nil {
						return
					}
					if kind != websocket.MessageBinary || len(frame) < 8 {
						continue
					}
					if !measuring.Load() {
						continue
					}
					stamp := binary.LittleEndian.Uint64(frame[:8])
					samples[index] = append(samples[index],
						float64(nowMicros()-stamp)/1000)
					if int(deliveredThisFrame.Add(1)) >= subscriberCount {
						select {
						case fleet <- struct{}{}:
						default:
						}
					}
				}
			}(index, conn)
		}

		deadline := time.Now().Add(30 * time.Second)
		for server.clientCount() < subscriberCount && time.Now().Before(deadline) {
			time.Sleep(10 * time.Millisecond)
		}
		if server.clientCount() < subscriberCount {
			fmt.Fprintf(os.Stderr, "subscribers did not come up at N=%d\n", subscriberCount)
			os.Exit(1)
		}
		connectedRss := residentBytes()

		frameOf := func() []byte {
			frame := make([]byte, 8+len(payload))
			binary.LittleEndian.PutUint64(frame[:8], nowMicros())
			copy(frame[8:], payload)
			return frame
		}

		// Warm up before measuring, for the same reason every other column does: the first
		// frames pay for lazily grown buffers on both ends and would otherwise be the whole
		// tail.
		warmupTicks := min(hz, 30)
		for tick := 0; tick < warmupTicks; tick++ {
			server.broadcast(ctx, frameOf())
			time.Sleep(time.Second / time.Duration(hz))
		}
		// The warm-up must not reach the statistics, and the readers only started counting
		// once `measuring` went true, so there is nothing to clear but the flag's other
		// side: turn it on now.
		measuring.Store(true)

		ticks := 0
		cpuBefore := cpuMilliseconds()
		startedAt := time.Now()
		if *saturateFlag {
			// Closed loop: publish, wait for the whole fleet to have it, publish again.
			// Open-looping at "maximum rate" would measure the send buffer rather than the
			// capacity, and the SynQt column cannot open-loop at all, so this is the mode
			// every column shares.
			for time.Since(startedAt) < time.Duration(seconds)*time.Second {
				deliveredThisFrame.Store(0)
				select {
				case <-fleet:
				default:
				}
				server.broadcast(ctx, frameOf())
				ticks++
				select {
				case <-fleet:
				case <-time.After(5 * time.Second):
					fmt.Fprintf(os.Stderr,
						"a frame never reached every subscriber at N=%d\n", subscriberCount)
					os.Exit(1)
				}
			}
		} else {
			ticks = seconds * hz
			for tick := 0; tick < ticks; tick++ {
				server.broadcast(ctx, frameOf())
				due := time.Duration(tick+1) * time.Second / time.Duration(hz)
				if wait := due - time.Since(startedAt); wait > 0 {
					time.Sleep(wait)
				}
			}
			// Let what is in flight land, or the tail of every run reads as loss that is
			// really the harness stopping first.
			time.Sleep(500 * time.Millisecond)
		}
		elapsedSeconds := time.Since(startedAt).Seconds()
		cpuMs := cpuMilliseconds() - cpuBefore
		measuring.Store(false)

		delivered := 0
		propagation := []float64{}
		for _, one := range samples {
			delivered += len(one)
			propagation = append(propagation, one...)
		}
		expected := ticks * subscriberCount
		rssPerConn := 0.0
		if subscriberCount > 0 && connectedRss > baselineRss {
			rssPerConn = float64(connectedRss-baselineRss) / float64(subscriberCount)
		}
		cpuPer1k := 0.0
		if delivered > 0 {
			cpuPer1k = (cpuMs * 1000) / float64(delivered)
		}
		one := entry{
			Subscribers:      subscriberCount,
			Propagation:      summarize(propagation),
			ThroughputPerSec: float64(delivered) / max(elapsedSeconds, 0.001),
			CPUMsPer1k:       cpuPer1k,
			RSSPerConn:       rssPerConn,
			RSSTotal:         connectedRss,
			Delivered:        delivered,
			Expected:         expected,
		}
		fmt.Printf("  N=%d  p50 %.3f ms  p99 %.3f ms  %.0f msg/s  delivered %d/%d\n",
			one.Subscribers, one.Propagation.P50, one.Propagation.P99,
			one.ThroughputPerSec, one.Delivered, one.Expected)
		sweep = append(sweep, one)

		for _, conn := range conns {
			_ = conn.CloseNow()
		}
		server.close()
		readers.Wait()
		time.Sleep(100 * time.Millisecond)
	}

	// "linux <kernel release>", which is what measure.mjs writes from os.platform() and
	// os.release(). The header line prints one column's host for the whole table, so two
	// columns that named the machine differently would make it a lottery which one it read.
	release, _ := os.ReadFile("/proc/sys/kernel/osrelease")
	document := result{
		Benchmark:    "vs-frameworks-live",
		Stack:        "go-bare",
		Path:         "net/http + coder/websocket",
		// runtime.Version() is "go1.26.5"; the header prints the runtime name itself, so
		// the "go" prefix here would read as "go go1.26.5".
		GoVersion:    strings.TrimPrefix(runtime.Version(), "go"),
		Host:         fmt.Sprintf("%s %s", runtime.GOOS, strings.TrimSpace(string(release))),
		Arch:         runtime.GOARCH,
		Recorded:     time.Now().UTC().Format(time.RFC3339),
		RSSAvailable: true,
		Hz:           hz,
		Saturated:    *saturateFlag,
		Seconds:      seconds,
		PayloadBytes: payloadBytes,
		Sweep:        sweep,
	}
	if *saturateFlag {
		document.Hz = 0
	}
	encoded, err := json.MarshalIndent(document, "", "  ")
	if err != nil {
		fmt.Fprintln(os.Stderr, "could not encode the result:", err)
		os.Exit(1)
	}
	if *outFlag != "" {
		if err := os.WriteFile(*outFlag, append(encoded, '\n'), 0o644); err != nil {
			fmt.Fprintln(os.Stderr, "could not write the result:", err)
			os.Exit(1)
		}
		fmt.Printf("\nwrote %s\n", *outFlag)
	}
}
