package config

import (
	"flag"
	"os"
	"os/exec"
	"strconv"
	"time"
)

// Config holds runtime configuration for the Invidious Daemon
type Config struct {
	Host        string
	Port        int
	YtDlpPath   string
	MaxWorkers  int
	SearchTTL   time.Duration
	VideoTTL    time.Duration
	RecsTTL     time.Duration
}

// Load parses CLI flags and environment variables
func Load() *Config {
	defaultHost := "127.0.0.1"
	if env := os.Getenv("INVIDIOUS_HOST"); env != "" {
		defaultHost = env
	}

	defaultPort := 8765
	if env := os.Getenv("INVIDIOUS_PORT"); env != "" {
		if p, err := strconv.Atoi(env); err == nil {
			defaultPort = p
		}
	}

	defaultYtDlp := "yt-dlp"
	if env := os.Getenv("YTDLP_PATH"); env != "" {
		defaultYtDlp = env
	} else if path, err := exec.LookPath("yt-dlp"); err == nil {
		defaultYtDlp = path
	}

	defaultWorkers := 4
	if env := os.Getenv("MAX_WORKERS"); env != "" {
		if w, err := strconv.Atoi(env); err == nil && w > 0 {
			defaultWorkers = w
		}
	}

	host := flag.String("host", defaultHost, "Host interface to listen on")
	port := flag.Int("port", defaultPort, "HTTP port to listen on")
	ytdlp := flag.String("ytdlp", defaultYtDlp, "Path to yt-dlp binary")
	workers := flag.Int("workers", defaultWorkers, "Max concurrent extraction workers")

	// Parse flags only if not already parsed (useful for tests)
	if !flag.Parsed() {
		flag.Parse()
	}

	return &Config{
		Host:       *host,
		Port:       *port,
		YtDlpPath:  *ytdlp,
		MaxWorkers: *workers,
		SearchTTL:  1 * time.Hour,
		VideoTTL:   1 * time.Hour,
		RecsTTL:    2 * time.Hour,
	}
}
