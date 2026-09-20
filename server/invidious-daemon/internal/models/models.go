package models

// InvidiousTrack represents a track item in search or playlist responses
type InvidiousTrack struct {
	VideoID       string `json:"videoId"`
	Title         string `json:"title"`
	Author        string `json:"author"`
	LengthSeconds int    `json:"lengthSeconds"`
}

// AdaptiveFormat represents an Opus/WebM or AAC audio stream
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
	Author            string           `json:"author,omitempty"`
	LengthSeconds     int              `json:"lengthSeconds"`
	AdaptiveFormats   []AdaptiveFormat `json:"adaptiveFormats"`
	RecommendedVideos []InvidiousTrack `json:"recommendedVideos,omitempty"`
}

// StatsResponse represents health and software stats
type StatsResponse struct {
	Version  string `json:"version"`
	Software struct {
		Name    string `json:"name"`
		Version string `json:"version"`
	} `json:"software"`
	Status string `json:"status"`
}
