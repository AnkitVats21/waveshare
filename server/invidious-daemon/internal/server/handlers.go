package server

import (
	"encoding/json"
	"log"
	"net/http"
	"strings"

	"invidious-daemon/internal/models"
)

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
	w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(data)
}

func (s *Server) handleStats(w http.ResponseWriter, r *http.Request) {
	var stats models.StatsResponse
	stats.Version = "2.0-go-daemon"
	stats.Software.Name = "invidious-go-daemon"
	stats.Software.Version = "1.1"
	stats.Status = "healthy"
	writeJSON(w, http.StatusOK, stats)
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) handleSearch(w http.ResponseWriter, r *http.Request) {
	q := strings.TrimSpace(r.URL.Query().Get("q"))
	if q == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "Missing 'q' query parameter"})
		return
	}
	log.Printf("[SEARCH] '%s' from %s", q, r.RemoteAddr)
	tracks, err := s.extractor.Search(r.Context(), q)
	if err != nil {
		log.Printf("[ERROR] Search '%s' failed: %v", q, err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	if tracks == nil {
		tracks = []models.InvidiousTrack{}
	}
	log.Printf("[SEARCH RESULT] Found %d tracks for '%s'", len(tracks), q)
	writeJSON(w, http.StatusOK, tracks)
}

func (s *Server) handleVideo(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(r.URL.Path, "/")
	if len(parts) < 5 || parts[4] == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "Missing video ID"})
		return
	}
	videoID := parts[4]
	log.Printf("[RESOLVE] videoId '%s' from %s", videoID, r.RemoteAddr)

	details, err := s.extractor.ResolveVideo(r.Context(), videoID)
	if err != nil {
		log.Printf("[ERROR] Resolve '%s' failed: %v", videoID, err)
		writeJSON(w, http.StatusNotFound, map[string]string{"error": err.Error()})
		return
	}

	bitrate := 0
	formatType := "unknown"
	if len(details.AdaptiveFormats) > 0 {
		bitrate = details.AdaptiveFormats[0].Bitrate
		formatType = details.AdaptiveFormats[0].Type
	}
	log.Printf("[RESOLVE RESULT] '%s' -> %s (%d bps, %d recs)", videoID, formatType, bitrate, len(details.RecommendedVideos))
	writeJSON(w, http.StatusOK, details)
}

func (s *Server) handleMixes(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(r.URL.Path, "/")
	if len(parts) < 5 || parts[4] == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "Missing mix ID"})
		return
	}
	mixID := parts[4]
	videoID := strings.TrimPrefix(mixID, "RD")
	log.Printf("[MIX] Request for mix '%s' (video '%s') from %s", mixID, videoID, r.RemoteAddr)

	tracks := s.extractor.GetRecommendations(r.Context(), videoID)
	if tracks == nil {
		tracks = []models.InvidiousTrack{}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"title":  "Mix",
		"mixId":  mixID,
		"videos": tracks,
	})
}

func (s *Server) handleRoot(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	_, _ = w.Write([]byte("Invidious Go Daemon for ESP32 Media Player is running.\n"))
}
