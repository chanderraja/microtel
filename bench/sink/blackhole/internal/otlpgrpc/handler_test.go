// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlpgrpc_test

import (
	"context"
	"net"
	"testing"

	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	otlptrace "go.opentelemetry.io/proto/otlp/trace/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"

	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/counters"
	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/otlpgrpc"
)

const bufSize = 1 << 20 // 1 MiB

func newTestServer(t *testing.T) (tracepb.TraceServiceClient, *counters.Counters) {
	t.Helper()
	c := counters.New()
	lis := bufconn.Listen(bufSize)
	srv := grpc.NewServer()
	tracepb.RegisterTraceServiceServer(srv, otlpgrpc.New(c, 0))
	t.Cleanup(func() { srv.Stop() })
	go srv.Serve(lis) //nolint:errcheck

	conn, err := grpc.NewClient("passthrough://bufnet",
		grpc.WithContextDialer(func(_ context.Context, _ string) (net.Conn, error) {
			return lis.Dial()
		}),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("grpc.NewClient: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	return tracepb.NewTraceServiceClient(conn), c
}

func buildTraceRequest(nSpans int) *tracepb.ExportTraceServiceRequest {
	spans := make([]*otlptrace.Span, nSpans)
	for i := range spans {
		spans[i] = &otlptrace.Span{Name: "bench-span"}
	}
	return &tracepb.ExportTraceServiceRequest{
		ResourceSpans: []*otlptrace.ResourceSpans{
			{ScopeSpans: []*otlptrace.ScopeSpans{{Spans: spans}}},
		},
	}
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

func TestGRPC_ValidRequest_CountsSpans(t *testing.T) {
	client, c := newTestServer(t)

	req := buildTraceRequest(4)
	_, err := client.Export(context.Background(), req)
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 4 {
		t.Errorf("spans_received: want 4, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d", snap.Errors)
	}
}

func TestGRPC_BytesRecorded(t *testing.T) {
	client, c := newTestServer(t)

	req := buildTraceRequest(2)
	_, err := client.Export(context.Background(), req)
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.BytesReceived == 0 {
		t.Errorf("bytes_received: want > 0, got 0")
	}
}

func TestGRPC_EmptyRequest_ZeroSpans(t *testing.T) {
	client, c := newTestServer(t)

	_, err := client.Export(context.Background(), &tracepb.ExportTraceServiceRequest{})
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
}

func TestGRPC_MultipleRequests_Accumulate(t *testing.T) {
	client, c := newTestServer(t)

	for _, n := range []int{3, 2, 5} {
		_, err := client.Export(context.Background(), buildTraceRequest(n))
		if err != nil {
			t.Fatalf("Export: %v", err)
		}
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 10 {
		t.Errorf("spans_received: want 10, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 3 {
		t.Errorf("grpc_requests_received: want 3, got %d", snap.GRPCRequestsReceived)
	}
	if snap.RequestsReceived != 3 {
		t.Errorf("requests_received: want 3, got %d", snap.RequestsReceived)
	}
}

func TestGRPC_ReturnsEmptyResponse(t *testing.T) {
	client, _ := newTestServer(t)

	resp, err := client.Export(context.Background(), buildTraceRequest(1))
	if err != nil {
		t.Fatalf("Export: %v", err)
	}
	if resp == nil {
		t.Fatal("response: want non-nil, got nil")
	}
	if resp.PartialSuccess != nil && resp.PartialSuccess.RejectedSpans != 0 {
		t.Errorf("rejected_spans: want 0, got %d", resp.PartialSuccess.RejectedSpans)
	}
}
