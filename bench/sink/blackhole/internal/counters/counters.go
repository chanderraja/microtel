// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package counters

import (
	"sync/atomic"
	"time"
)

// Counters holds all atomic counters for the blackhole-sink.
// All methods are safe for concurrent use.
type Counters struct {
	spansReceived        atomic.Uint64
	bytesReceived        atomic.Uint64
	requestsReceived     atomic.Uint64
	httpRequestsReceived atomic.Uint64
	grpcRequestsReceived atomic.Uint64
	errorsTotal          atomic.Uint64
	responseBytes        atomic.Uint64
	startTime            time.Time
	lastErr              atomic.Value // stores string
}

// New returns a zero-valued Counters with start time set to now.
func New() *Counters {
	c := &Counters{startTime: time.Now()}
	c.lastErr.Store("")
	return c
}

// RecordHTTPExport records one successful OTLP/HTTP export.
// spans: number of spans in the request.
// reqBytes: raw HTTP request body length (handles Content-Length: -1 callers
//
//	by measuring after io.ReadAll, not from the header).
//
// respBytes: length of the serialized response body written to the client.
func (c *Counters) RecordHTTPExport(spans, reqBytes, respBytes uint64) {
	c.spansReceived.Add(spans)
	c.bytesReceived.Add(reqBytes)
	c.responseBytes.Add(respBytes)
	c.requestsReceived.Add(1)
	c.httpRequestsReceived.Add(1)
}

// RecordGRPCExport records one successful OTLP/gRPC export.
// spans: number of spans in the request.
// reqBytes: proto.Size of the request message — equals the gRPC framing
//
//	prefix length field, not the HTTP/2 DATA frame size.
//
// respBytes: proto.Size of the response message.
func (c *Counters) RecordGRPCExport(spans, reqBytes, respBytes uint64) {
	c.spansReceived.Add(spans)
	c.bytesReceived.Add(reqBytes)
	c.responseBytes.Add(respBytes)
	c.requestsReceived.Add(1)
	c.grpcRequestsReceived.Add(1)
}

// RecordError increments the error counter and stores the description
// of the most recent error. In-flight requests crossing a Reset boundary
// may produce post-reset increments; the driver must quiesce traffic
// before calling Reset.
func (c *Counters) RecordError(desc string) {
	c.errorsTotal.Add(1)
	c.lastErr.Store(desc)
}

// Reset atomically zeroes all counters. Does not reset uptime_seconds.
func (c *Counters) Reset() {
	c.spansReceived.Store(0)
	c.bytesReceived.Store(0)
	c.requestsReceived.Store(0)
	c.httpRequestsReceived.Store(0)
	c.grpcRequestsReceived.Store(0)
	c.errorsTotal.Store(0)
	c.responseBytes.Store(0)
	c.lastErr.Store("")
}

// Snapshot is the JSON-serializable snapshot returned by GET /stats.
type Snapshot struct {
	SpansReceived        uint64  `json:"spans_received"`
	BytesReceived        uint64  `json:"bytes_received"`
	RequestsReceived     uint64  `json:"requests_received"`
	HTTPRequestsReceived uint64  `json:"http_requests_received"`
	GRPCRequestsReceived uint64  `json:"grpc_requests_received"`
	Errors               uint64  `json:"errors"`
	ResponseBytes        uint64  `json:"response_bytes"`
	LastError            string  `json:"last_error"`
	UptimeSeconds        float64 `json:"uptime_seconds"`
}

// Snapshot returns a consistent point-in-time view of the counters.
// Individual fields are loaded atomically but not under a single lock,
// so concurrent increments may cause slight cross-field skew; this is
// acceptable for the bench driver's measurement use case.
func (c *Counters) Snapshot() Snapshot {
	lastErr, _ := c.lastErr.Load().(string)
	return Snapshot{
		SpansReceived:        c.spansReceived.Load(),
		BytesReceived:        c.bytesReceived.Load(),
		RequestsReceived:     c.requestsReceived.Load(),
		HTTPRequestsReceived: c.httpRequestsReceived.Load(),
		GRPCRequestsReceived: c.grpcRequestsReceived.Load(),
		Errors:               c.errorsTotal.Load(),
		ResponseBytes:        c.responseBytes.Load(),
		LastError:            lastErr,
		UptimeSeconds:        time.Since(c.startTime).Seconds(),
	}
}
