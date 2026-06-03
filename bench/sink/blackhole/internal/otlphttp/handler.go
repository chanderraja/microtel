// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlphttp

import (
	"io"
	"net/http"
	"time"

	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	"google.golang.org/protobuf/proto"

	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/counters"
)

// NewHandler returns an http.Handler for the OTLP/HTTP listener on :4318.
// Routes: POST /v1/traces (parsed), POST /v1/metrics and /v1/logs (stubbed).
// The catch-all "/" route records unknown-path requests as errors so the bench
// driver can distinguish "no requests arriving" from "requests hitting wrong path".
func NewHandler(c *counters.Counters, delayMs int) http.Handler {
	mux := http.NewServeMux()
	mux.Handle("/v1/traces", &traceHandler{c: c, delayMs: delayMs})
	mux.HandleFunc("/v1/metrics", stubOK)
	mux.HandleFunc("/v1/logs", stubOK)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		c.RecordError("unknown path: " + r.URL.Path)
		http.NotFound(w, r)
	})
	return mux
}

// stubOK accepts any POST and returns 200 with an empty protobuf body.
// Used for metrics and logs in B0 (traces-only milestone).
func stubOK(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/x-protobuf")
	w.WriteHeader(http.StatusOK)
}

type traceHandler struct {
	c       *counters.Counters
	delayMs int
}

func (h *traceHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		h.c.RecordError("wrong method: " + r.Method)
		return
	}
	if ct := r.Header.Get("Content-Type"); ct != "application/x-protobuf" {
		http.Error(w, "unsupported media type", http.StatusUnsupportedMediaType)
		h.c.RecordError("wrong content-type: " + ct)
		return
	}

	// Read the full body first so reqBytes reflects actual wire bytes,
	// not Content-Length (which may be -1 for chunked transfers).
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "failed to read body", http.StatusInternalServerError)
		h.c.RecordError("read body: " + err.Error())
		return
	}
	reqBytes := uint64(len(body))

	var req tracepb.ExportTraceServiceRequest
	if err := proto.Unmarshal(body, &req); err != nil {
		http.Error(w, "invalid protobuf", http.StatusBadRequest)
		h.c.RecordError("unmarshal: " + err.Error())
		return
	}

	spans := countSpans(&req)

	// An empty ExportTraceServiceResponse serializes to zero bytes, which
	// is a valid OTLP/HTTP response body per the spec.
	resp := &tracepb.ExportTraceServiceResponse{}
	respBody, _ := proto.Marshal(resp)

	if h.delayMs > 0 {
		time.Sleep(time.Duration(h.delayMs) * time.Millisecond)
	}
	w.Header().Set("Content-Type", "application/x-protobuf")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(respBody)

	h.c.RecordHTTPExport(spans, reqBytes, uint64(len(respBody)))
}

func countSpans(req *tracepb.ExportTraceServiceRequest) uint64 {
	var n uint64
	for _, rs := range req.ResourceSpans {
		for _, ss := range rs.ScopeSpans {
			n += uint64(len(ss.Spans))
		}
	}
	return n
}
