package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"invidious-daemon/internal/cache"
	"invidious-daemon/internal/config"
	"invidious-daemon/internal/extractor"
	"invidious-daemon/internal/server"
)

func main() {
	cfg := config.Load()
	c := cache.NewMemoryCache()
	ext := extractor.NewExtractor(cfg, c)
	srv := server.NewServer(cfg, ext)

	// Graceful shutdown channel
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)

	go func() {
		if err := srv.Start(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("Server startup failed: %v", err)
		}
	}()

	<-stop
	log.Println("Shutting down Invidious Daemon gracefully...")

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := srv.Shutdown(shutdownCtx); err != nil {
		log.Printf("Server shutdown error: %v", err)
	} else {
		log.Println("Invidious Daemon stopped cleanly.")
	}
}
