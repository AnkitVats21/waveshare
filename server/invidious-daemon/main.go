package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

// InvidiousTrack represents a track item in search or playlist responses
type InvidiousTrack struct {
	VideoID       string `json:"videoId"`
	Title         string `json:"title"`
	Author        string `json:"author"`
	LengthSeconds int    `json:"lengthSeconds"`
}

// AdaptiveFormat represents an Opus/WebM audio stream
type AdaptiveFormat struct {
	Type      string `json:"type"`
	URL       string `json:"url"`
	Bitrate   int    `json:"bitrate"`
	Itag      string `json:"itag"`
	Container string `json:"container,omitempty"`
}

// VideoDetails represents the response for /api/v1/videos/:id
type VideoDetails struct {
	VideoID           string           `json:"videoId"`
	Title             string           `json:"title"`
	AdaptiveFormats   []AdaptiveFormat `json:"adaptiveFormats"`
	RecommendedVideos []InvidiousTrack `json:"recommendedVideos,omitempty"`
}

// StatsResponse represents health stats
type StatsResponse struct {
	Version  string `json:"version"`
	Software struct {
		Name    string `json:"name"`
		Version string `json:"version"`
	} `json:"software"`
	Status string `json:"status"`
}

// CacheItem stores data with expiration
type CacheItem struct {
	Data      any
	ExpiresAt time.Time
}

// AppState manages caching, locks, and concurrency limiters
type AppState struct {
	mu           sync.RWMutex
	cache        map[string]CacheItem
	resolvingMu  sync.Mutex
	resolving    map[string]chan struct{}
	workerSem    chan struct{}
	ytdlpPath    string
}

func NewAppState() *AppState {
	ytdlp := "yt-dlp"
	if path, err := exec.LookPath("yt-dlp"); err == nil {
		ytdlp = path
	}

	return &AppState{
		cache:       make(map[string]CacheItem),
		resolving:   make(map[string]chan struct{}),
		workerSem:   make(chan struct{}, 4), // max 4 concurrent extractions
		ytdlpPath:   ytdlp,
	}
}

func (s *AppState) GetCache(key string) (any, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	item, ok := s.cache[key]
	if !ok || time.Now().After(item.ExpiresAt) {
		return nil, false
	}
	return item.Data, true
}

func (s *AppState) SetCache(key string, data any, ttl time.Duration) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cache[key] = CacheItem{
		Data:      data,
		ExpiresAt: time.Now().Add(ttl),
	}
}

func (s *AppState) Search(ctx context.Context, query string) ([]InvidiousTrack, error) {
	cacheKey := "search:" + query
	if cached, ok := s.GetCache(cacheKey); ok {
		return cached.([]InvidiousTrack), nil
	}

	s.workerSem <- struct{}{}
	defer func() { <-s.workerSem }()

	cmdCtx, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()

	cmd := exec.CommandContext(cmdCtx, s.ytdlpPath,
		"--no-warnings",
		"--skip-download",
		"--flat-playlist",
		"--dump-json",
		fmt.Sprintf("ytsearch5:%s", query),
	)

	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("search command failed: %w", err)
	}

	var tracks []InvidiousTrack
	lines := strings.Split(string(out), "\n")
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var item map[string]any
		if err := json.Unmarshal([]byte(line), &item); err == nil {
			vid, _ := item["id"].(string)
			title, _ := item["title"].(string)
			author, _ := item["uploader"].(string)
			if author == "" {
				author, _ = item["channel"].(string)
			}
			duration := 0
			if d, ok := item["duration"].(float64); ok {
				duration = int(d)
			}
			if vid != "" && title != "" {
				tracks = append(tracks, InvidiousTrack{
					VideoID:       vid,
					Title:         title,
					Author:        author,
					LengthSeconds: duration,
				})
			}
		}
	}

	if len(tracks) > 0 {
		s.SetCache(cacheKey, tracks, 1*time.Hour)
		// Eager background pre-fetch of the #1 track!
		go func(topID string) {
			bgCtx, bgCancel := context.WithTimeout(context.Background(), 45*time.Second)
			defer bgCancel()
			_, _ = s.ResolveVideo(bgCtx, topID)
		}(tracks[0].VideoID)
	}

	return tracks, nil
}

func (s *AppState) ResolveVideo(ctx context.Context, videoID string) (*VideoDetails, error) {
	cacheKey := "video:" + videoID
	if cached, ok := s.GetCache(cacheKey); ok {
		return cached.(*VideoDetails), nil
	}

	// Deduplication: if another request is already extracting this videoID, wait for it
	s.resolvingMu.Lock()
	ch, inProgress := s.resolving[videoID]
	if !inProgress {
		ch = make(chan struct{})
		s.resolving[videoID] = ch
	}
	s.resolvingMu.Unlock()

	if inProgress {
		select {
		case <-ch:
			if cached, ok := s.GetCache(cacheKey); ok {
				return cached.(*VideoDetails), nil
			}
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}

	defer func() {
		s.resolvingMu.Lock()
		delete(s.resolving, videoID)
		close(ch)
		s.resolvingMu.Unlock()
	}()

	s.workerSem <- struct{}{}
	defer func() { <-s.workerSem }()

	cmdCtx, cancel := context.WithTimeout(ctx, 35*time.Second)
	defer cancel()

	cmd := exec.CommandContext(cmdCtx, s.ytdlpPath,
		"--no-warnings",
		"-f", "ba[ext=webm]/ba",
		"--print", "%(title)s",
		"--print", "%(url)s",
		"--print", "%(abr)s",
		fmt.Sprintf("https://www.youtube.com/watch?v=%s", videoID),
	)

	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("video extraction failed: %w", err)
	}

	rawLines := strings.Split(strings.TrimSpace(string(out)), "\n")
	var lines []string
	for _, l := range rawLines {
		if t := strings.TrimSpace(l); t != "" {
			lines = append(lines, t)
		}
	}

	if len(lines) < 2 {
		return nil, fmt.Errorf("insufficient output lines from yt-dlp: %d", len(lines))
	}

	title := lines[0]
	streamURL := lines[1]
	bitrate := 130000
	if len(lines) > 2 {
		if abr, err := strconv.ParseFloat(lines[2], 64); err == nil && abr > 0 {
			bitrate = int(abr * 1000)
		}
	}

	details := &VideoDetails{
		VideoID: videoID,
		Title:   title,
		AdaptiveFormats: []AdaptiveFormat{
			{
				Type:      "audio/webm; codecs=\"opus\"",
				URL:       streamURL,
				Bitrate:   bitrate,
				Itag:      "251",
				Container: "webm",
			},
		},
	}

	s.SetCache(cacheKey, details, 1*time.Hour)
	return details, nil
}

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(data)
}

func main() {
	host := flag.String("host", "127.0.0.1", "Host interface to listen on")
	port := flag.Int("port", 8765, "HTTP port to listen on")
	flag.Parse()

	state := NewAppState()
	addr := fmt.Sprintf("%s:%d", *host, *port)

	mux := http.NewServeMux()

	// 1. Health check: /api/v1/stats, /health
	mux.HandleFunc("/api/v1/stats", func(w http.ResponseWriter, r *http.Request) {
		var stats StatsResponse
		stats.Version = "2.0-go-daemon"
		stats.Software.Name = "invidious-go-daemon"
		stats.Software.Version = "1.0"
		stats.Status = "healthy"
		writeJSON(w, http.StatusOK, stats)
	})
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
	})

	// 2. Search: /api/v1/search?q=...
	mux.HandleFunc("/api/v1/search", func(w http.ResponseWriter, r *http.Request) {
		q := strings.TrimSpace(r.URL.Query().Get("q"))
		if q == "" {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "Missing 'q' query parameter"})
			return
		}
		log.Printf("[SEARCH] '%s' from %s", q, r.RemoteAddr)
		tracks, err := state.Search(r.Context(), q)
		if err != nil {
			log.Printf("[ERROR] Search '%s' failed: %v", q, err)
			writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
			return
		}
		if tracks == nil {
			tracks = []InvidiousTrack{}
		}
		log.Printf("[SEARCH RESULT] Found %d tracks for '%s'", len(tracks), q)
		writeJSON(w, http.StatusOK, tracks)
	})

	// 3. Video metadata & streams: /api/v1/videos/<id>
	mux.HandleFunc("/api/v1/videos/", func(w http.ResponseWriter, r *http.Request) {
		parts := strings.Split(r.URL.Path, "/")
		if len(parts) < 5 || parts[4] == "" {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "Missing video ID"})
			return
		}
		videoID := parts[4]
		log.Printf("[RESOLVE] videoId '%s' from %s", videoID, r.RemoteAddr)

		details, err := state.ResolveVideo(r.Context(), videoID)
		if err != nil {
			log.Printf("[ERROR] Resolve '%s' failed: %v", videoID, err)
			writeJSON(w, http.StatusNotFound, map[string]string{"error": err.Error()})
			return
		}

		log.Printf("[RESOLVE RESULT] '%s' -> %s (%d bps)", videoID, details.AdaptiveFormats[0].Type, details.AdaptiveFormats[0].Bitrate)
		writeJSON(w, http.StatusOK, details)
	})

	// Root status
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/plain")
		_, _ = w.Write([]byte("Invidious Go Daemon for ESP32 Media Player is running.\n"))
	})

	server := &http.Server{
		Addr:         addr,
		Handler:      mux,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 45 * time.Second,
	}

	log.Printf("Invidious Go Daemon listening on http://%s (extractor: %s)", addr, state.ytdlpPath)
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("Server error: %v", err)
	}
}
