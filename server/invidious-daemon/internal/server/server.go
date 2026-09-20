package server

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"time"

	"invidious-daemon/internal/config"
	"invidious-daemon/internal/extractor"
)

// Server coordinates HTTP routing and graceful lifecycle
type Server struct {
	cfg        *config.Config
	extractor  *extractor.Extractor
	httpServer *http.Server
}

// NewServer initializes HTTP routes and configuration
func NewServer(cfg *config.Config, ext *extractor.Extractor) *Server {
	s := &Server{
		cfg:       cfg,
		extractor: ext,
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/stats", s.handleStats)
	mux.HandleFunc("/health", s.handleHealth)
	mux.HandleFunc("/api/v1/search", s.handleSearch)
	mux.HandleFunc("/api/v1/videos/", s.handleVideo)
	mux.HandleFunc("/api/v1/mixes/", s.handleMixes)
	mux.HandleFunc("/", s.handleRoot)

	addr := fmt.Sprintf("%s:%d", cfg.Host, cfg.Port)
	s.httpServer = &http.Server{
		Addr:         addr,
		Handler:      corsMiddleware(mux),
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 45 * time.Second,
	}

	return s
}

func corsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Origin, X-Requested-With")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusOK)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// Start runs ListenAndServe
func (s *Server) Start() error {
	log.Printf("Invidious Go Daemon listening on http://%s (extractor: %s, max workers: %d)",
		s.httpServer.Addr, s.cfg.YtDlpPath, s.cfg.MaxWorkers)
	return s.httpServer.ListenAndServe()
}

// Shutdown gracefully stops the HTTP server
func (s *Server) Shutdown(ctx context.Context) error {
	return s.httpServer.Shutdown(ctx)
}
