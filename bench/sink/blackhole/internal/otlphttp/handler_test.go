// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlphttp_test

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"testing"

	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	otlptrace "go.opentelemetry.io/proto/otlp/trace/v1"
	"google.golang.org/protobuf/proto"

	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/counters"
	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/otlphttp"
)

func buildTraceRequest(nSpans int) []byte {
	spans := make([]*otlptrace.Span, nSpans)
	for i := range spans {
		spans[i] = &otlptrace.Span{Name: "bench-span"}
	}
	req := &tracepb.ExportTraceServiceRequest{
		ResourceSpans: []*otlptrace.ResourceSpans{
			{ScopeSpans: []*otlptrace.ScopeSpans{{Spans: spans}}},
		},
	}
	b, _ := proto.Marshal(req)
	return b
}

func newTestHandler() (http.Handler, *counters.Counters) {
	c := counters.New()
	return otlphttp.NewHandler(c), c
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

func TestHTTP_ValidRequest_CountsSpans(t *testing.T) {
	h, c := newTestHandler()
	body := buildTraceRequest(3)

	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.SpansReceived != 3 {
		t.Errorf("spans_received: want 3, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != uint64(len(body)) {
		t.Errorf("bytes_received: want %d, got %d", len(body), snap.BytesReceived)
	}
	if snap.HTTPRequestsReceived != 1 {
		t.Errorf("http_requests_received: want 1, got %d", snap.HTTPRequestsReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d", snap.Errors)
	}
}

func TestHTTP_EmptyBody_ZeroSpans(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	if c.Snapshot().SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", c.Snapshot().SpansReceived)
	}
}

func TestHTTP_MultipleRequests_Accumulate(t *testing.T) {
	h, c := newTestHandler()
	for _, n := range []int{2, 5, 1} {
		body := buildTraceRequest(n)
		req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/x-protobuf")
		h.ServeHTTP(httptest.NewRecorder(), req)
	}
	if c.Snapshot().SpansReceived != 8 {
		t.Errorf("spans_received: want 8, got %d", c.Snapshot().SpansReceived)
	}
	if c.Snapshot().RequestsReceived != 3 {
		t.Errorf("requests_received: want 3, got %d", c.Snapshot().RequestsReceived)
	}
}

func TestHTTP_ResponseContentType(t *testing.T) {
	h, _ := newTestHandler()
	body := buildTraceRequest(1)
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if ct := rec.Header().Get("Content-Type"); ct != "application/x-protobuf" {
		t.Errorf("response Content-Type: want application/x-protobuf, got %q", ct)
	}
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

func TestHTTP_WrongMethod_Returns405(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodGet, "/v1/traces", nil)
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("status: want 405, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

func TestHTTP_WrongContentType_Returns415(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader([]byte("hello")))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusUnsupportedMediaType {
		t.Errorf("status: want 415, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

func TestHTTP_InvalidProto_Returns400(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader([]byte("not protobuf!!!")))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status: want 400, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

// ---------------------------------------------------------------------------
// Stub paths (metrics, logs)
// ---------------------------------------------------------------------------

func TestHTTP_MetricsStub_Returns200(t *testing.T) {
	h, _ := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/metrics", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
}

func TestHTTP_LogsStub_Returns200(t *testing.T) {
	h, _ := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/logs", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
}
