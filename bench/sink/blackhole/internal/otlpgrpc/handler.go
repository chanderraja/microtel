// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlpgrpc

import (
	"context"
	"time"

	metricpb "go.opentelemetry.io/proto/otlp/collector/metrics/v1"
	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	"google.golang.org/protobuf/proto"

	"github.com/chanderraja/microtel/bench/sink/blackhole/internal/counters"
)

// TraceHandler implements the OTLP TraceService gRPC endpoint.
type TraceHandler struct {
	tracepb.UnimplementedTraceServiceServer
	c       *counters.Counters
	delayMs int
}

// New returns a TraceHandler that records exports into c.
func New(c *counters.Counters, delayMs int) *TraceHandler {
	return &TraceHandler{c: c, delayMs: delayMs}
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

	if h.delayMs > 0 {
		time.Sleep(time.Duration(h.delayMs) * time.Millisecond)
	}
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

// MetricHandler implements the OTLP MetricsService gRPC endpoint.
// It counts requests and bytes but does not decode metric data points (B0 stub).
type MetricHandler struct {
	metricpb.UnimplementedMetricsServiceServer
	c       *counters.Counters
	delayMs int
}

// NewMetricHandler returns a MetricHandler that records exports into c.
func NewMetricHandler(c *counters.Counters, delayMs int) *MetricHandler {
	return &MetricHandler{c: c, delayMs: delayMs}
}

// Export counts bytes and returns an empty success response.
func (h *MetricHandler) Export(
	_ context.Context,
	req *metricpb.ExportMetricsServiceRequest,
) (*metricpb.ExportMetricsServiceResponse, error) {
	reqBytes := uint64(proto.Size(req))
	resp := &metricpb.ExportMetricsServiceResponse{}
	respBytes := uint64(proto.Size(resp))
	if h.delayMs > 0 {
		time.Sleep(time.Duration(h.delayMs) * time.Millisecond)
	}
	h.c.RecordGRPCExport(0, reqBytes, respBytes)
	return resp, nil
}
