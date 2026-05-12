// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlpgrpc

import (
	"context"

	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	"google.golang.org/protobuf/proto"

	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/counters"
)

// TraceHandler implements the OTLP TraceService gRPC endpoint.
type TraceHandler struct {
	tracepb.UnimplementedTraceServiceServer
	c *counters.Counters
}

// New returns a TraceHandler that records exports into c.
func New(c *counters.Counters) *TraceHandler {
	return &TraceHandler{c: c}
}

// Export counts spans and bytes, then returns an empty success response.
func (h *TraceHandler) Export(
	_ context.Context,
	req *tracepb.ExportTraceServiceRequest,
) (*tracepb.ExportTraceServiceResponse, error) {
	reqBytes := uint64(proto.Size(req))

	spans := countSpans(req)

	resp := &tracepb.ExportTraceServiceResponse{}
	respBytes := uint64(proto.Size(resp))

	h.c.RecordGRPCExport(spans, reqBytes, respBytes)
	return resp, nil
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
