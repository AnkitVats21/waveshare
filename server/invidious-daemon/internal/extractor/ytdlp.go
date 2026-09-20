package extractor

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"

	"invidious-daemon/internal/cache"
	"invidious-daemon/internal/config"
	"invidious-daemon/internal/models"
)

// Extractor handles extracting metadata and audio streams via yt-dlp
type Extractor struct {
	cfg       *config.Config
	cache     *cache.MemoryCache
	flight    *cache.SingleFlight
	workerSem chan struct{}
}

// NewExtractor creates a new Extractor instance
func NewExtractor(cfg *config.Config, c *cache.MemoryCache) *Extractor {
	return &Extractor{
		cfg:       cfg,
		cache:     c,
		flight:    cache.NewSingleFlight(),
		workerSem: make(chan struct{}, cfg.MaxWorkers),
	}
}

// Search executes a YouTube search for the query and returns up to 5 tracks
func (e *Extractor) Search(ctx context.Context, query string) ([]models.InvidiousTrack, error) {
	cacheKey := "search:" + query
	if cached, ok := e.cache.Get(cacheKey); ok {
		return cached.([]models.InvidiousTrack), nil
	}

	e.workerSem <- struct{}{}
	defer func() { <-e.workerSem }()

	cmdCtx, cancel := context.WithTimeout(ctx, 25*time.Second)
	defer cancel()

	cmd := exec.CommandContext(cmdCtx, e.cfg.YtDlpPath,
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

	var tracks []models.InvidiousTrack
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
				tracks = append(tracks, models.InvidiousTrack{
					VideoID:       vid,
					Title:         title,
					Author:        author,
					LengthSeconds: duration,
				})
			}
		}
	}

	if len(tracks) > 0 {
		e.cache.Set(cacheKey, tracks, e.cfg.SearchTTL)
		// Eager background pre-fetch of the #1 track
		go func(topID string) {
			bgCtx, bgCancel := context.WithTimeout(context.Background(), 45*time.Second)
			defer bgCancel()
			_, _ = e.ResolveVideo(bgCtx, topID)
		}(tracks[0].VideoID)
	}

	return tracks, nil
}

// GetRecommendations fetches algorithmic Radio Mix tracks for a given video ID
func (e *Extractor) GetRecommendations(ctx context.Context, videoID string) []models.InvidiousTrack {
	cacheKey := "recs:" + videoID
	if cached, ok := e.cache.Get(cacheKey); ok {
		return cached.([]models.InvidiousTrack)
	}

	cmdCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()

	cmd := exec.CommandContext(cmdCtx, e.cfg.YtDlpPath,
		"--no-warnings",
		"--skip-download",
		"--flat-playlist",
		"--playlist-end", "8",
		"--dump-json",
		fmt.Sprintf("https://www.youtube.com/watch?v=%s&list=RD%s", videoID, videoID),
	)

	out, err := cmd.Output()
	if err != nil {
		log.Printf("[WARN] Recommendations extraction failed for %s: %v", videoID, err)
		return nil
	}

	var tracks []models.InvidiousTrack
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
			if vid != "" && title != "" && vid != videoID {
				tracks = append(tracks, models.InvidiousTrack{
					VideoID:       vid,
					Title:         title,
					Author:        author,
					LengthSeconds: duration,
				})
			}
		}
	}

	if len(tracks) > 0 {
		e.cache.Set(cacheKey, tracks, e.cfg.RecsTTL)
	}
	return tracks
}

// ResolveVideo extracts audio stream formats and recommendations for a video ID
func (e *Extractor) ResolveVideo(ctx context.Context, videoID string) (*models.VideoDetails, error) {
	cacheKey := "video:" + videoID
	if cached, ok := e.cache.Get(cacheKey); ok {
		return cached.(*models.VideoDetails), nil
	}

	val, err := e.flight.Do(cacheKey, func() (any, error) {
		// Double check cache
		if cached, ok := e.cache.Get(cacheKey); ok {
			return cached.(*models.VideoDetails), nil
		}

		e.workerSem <- struct{}{}
		defer func() { <-e.workerSem }()

		// Concurrently extract recommendations via YouTube Radio Mix
		var recs []models.InvidiousTrack
		var recWg sync.WaitGroup
		recWg.Add(1)
		go func() {
			defer recWg.Done()
			recs = e.GetRecommendations(ctx, videoID)
		}()

		cmdCtx, cancel := context.WithTimeout(ctx, 35*time.Second)
		defer cancel()

		cmd := exec.CommandContext(cmdCtx, e.cfg.YtDlpPath,
			"--no-warnings",
			"-f", "ba[ext=webm]/ba",
			"--print", "%(title)s",
			"--print", "%(url)s",
			"--print", "%(abr)s",
			"--print", "%(duration)s",
			"--print", "%(uploader)s",
			fmt.Sprintf("https://www.youtube.com/watch?v=%s", videoID),
		)

		out, err := cmd.Output()
		if err != nil {
			recWg.Wait()
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
			recWg.Wait()
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
		duration := 0
		if len(lines) > 3 {
			if d, err := strconv.ParseFloat(lines[3], 64); err == nil && d > 0 {
				duration = int(d)
			}
		}
		author := ""
		if len(lines) > 4 {
			author = lines[4]
		}

		recWg.Wait()

		// Fallback: If YouTube RD mix didn't yield recommendations, use related search
		if len(recs) == 0 {
			fbQuery := author
			if fbQuery == "" {
				fbQuery = title
			}
			if fbQuery != "" {
				if fbTracks, err := e.Search(ctx, fbQuery); err == nil {
					for _, t := range fbTracks {
						if t.VideoID != videoID {
							recs = append(recs, t)
						}
					}
				}
			}
		}

		details := &models.VideoDetails{
			VideoID:       videoID,
			Title:         title,
			Author:        author,
			LengthSeconds: duration,
			AdaptiveFormats: []models.AdaptiveFormat{
				{
					Type:      "audio/webm; codecs=\"opus\"",
					URL:       streamURL,
					Bitrate:   bitrate,
					Itag:      "251",
					Container: "webm",
				},
			},
			RecommendedVideos: recs,
		}

		e.cache.Set(cacheKey, details, e.cfg.VideoTTL)
		return details, nil
	})

	if err != nil {
		return nil, err
	}
	return val.(*models.VideoDetails), nil
}
