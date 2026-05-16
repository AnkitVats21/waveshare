package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

const (
	ESP32IP      = "192.168.1.19"
	UDPPort      = 5006
	SampleRate   = 16000
	ChunkSamples = 640                    // 40ms @ 16kHz
	ChunkSize    = ChunkSamples * 2       // s16le mono
	StationsFile = "stations.json"

	AudioQueueSize = 128 // ~5 sec buffer
)

type Station struct {
	Name string `json:"name"`
	URL  string `json:"url"`
}

type ChannelManager struct {
	mu       sync.RWMutex
	channels map[string]string
	current  string
}

func NewChannelManager() *ChannelManager {

	manager := &ChannelManager{
		channels: map[string]string{},
	}

	err := manager.LoadStations()

	if err != nil {

		fmt.Println("[INFO] creating default stations.json")

		manager.channels = map[string]string{
			"hungama": "https://stream-289.zeno.fm/rm4i9pdex3cuv?zt=...",
			"redfm":   "https://stream-175.zeno.fm/9phrkb1e3v8uv?zt=...",
			"bigfm":   "https://stream-280.zeno.fm/dbstwo3dvhhtv?zt=...",
		}

		manager.current = "hungama"

		manager.SaveStations()
	}

	if manager.current == "" {

		for k := range manager.channels {
			manager.current = k
			break
		}
	}

	return manager
}

func (c *ChannelManager) LoadStations() error {

	data, err := os.ReadFile(StationsFile)

	if err != nil {
		return err
	}

	var stations []Station

	err = json.Unmarshal(data, &stations)

	if err != nil {
		return err
	}

	for _, s := range stations {
		c.channels[s.Name] = s.URL
	}

	return nil
}

func (c *ChannelManager) SaveStations() error {

	c.mu.RLock()
	defer c.mu.RUnlock()

	var stations []Station

	for k, v := range c.channels {

		stations = append(stations, Station{
			Name: k,
			URL:  v,
		})
	}

	data, err := json.MarshalIndent(
		stations,
		"",
		"  ",
	)

	if err != nil {
		return err
	}

	return os.WriteFile(
		StationsFile,
		data,
		0644,
	)
}

func (c *ChannelManager) Add(name, url string) error {

	c.mu.Lock()

	c.channels[name] = url

	c.mu.Unlock()

	return c.SaveStations()
}

func (c *ChannelManager) Switch(name string) bool {

	c.mu.Lock()
	defer c.mu.Unlock()

	if _, ok := c.channels[name]; !ok {
		return false
	}

	c.current = name

	return true
}

func (c *ChannelManager) Current() (string, string) {

	c.mu.RLock()
	defer c.mu.RUnlock()

	return c.current, c.channels[c.current]
}

func (c *ChannelManager) List() map[string]string {

	c.mu.RLock()
	defer c.mu.RUnlock()

	out := make(map[string]string)

	for k, v := range c.channels {
		out[k] = v
	}

	return out
}

type RTPStreamer struct {
	conn *net.UDPConn

	seqNum    uint16
	timestamp uint32
	ssrc      uint32

	process *exec.Cmd
	stdout  io.ReadCloser

	manager *ChannelManager

	currentChannel string

	audioQueue chan []byte

	stopChan chan struct{}
}

func NewRTPStreamer(
	ip string,
	manager *ChannelManager,
) *RTPStreamer {

	addr, err := net.ResolveUDPAddr(
		"udp",
		fmt.Sprintf("%s:%d", ip, UDPPort),
	)

	if err != nil {
		panic(err)
	}

	conn, err := net.DialUDP(
		"udp",
		nil,
		addr,
	)

	if err != nil {
		panic(err)
	}

	return &RTPStreamer{
		conn:       conn,
		ssrc:       0x12345678,
		manager:    manager,
		stopChan:   make(chan struct{}),
		audioQueue: make(chan []byte, AudioQueueSize),
	}
}

func isM3U8(url string) bool {

	if strings.Contains(url, ".m3u8") {
		return true
	}

	client := &http.Client{
		Timeout: 5 * time.Second,
	}

	resp, err := client.Head(url)

	if err != nil {
		return false
	}

	defer resp.Body.Close()

	contentType := strings.ToLower(
		resp.Header.Get("Content-Type"),
	)

	return strings.Contains(contentType, "mpegurl") ||
		strings.Contains(contentType, "application/vnd.apple.mpegurl")
}

func (r *RTPStreamer) buildFFmpegArgs(url string) []string {

	args := []string{
		"-loglevel", "warning",

		// reconnect handling
		"-reconnect", "1",
		"-reconnect_streamed", "1",
		"-reconnect_delay_max", "5",
	}

	if isM3U8(url) {

		fmt.Println("[INFO] M3U8/HLS stream detected")

		args = append(args,

			// low latency
			"-fflags", "nobuffer",
			"-flags", "low_delay",
			"-probesize", "32",
			"-analyzeduration", "0",
		)
	}

	args = append(args,

		"-i", url,

		// output raw PCM
		"-f", "s16le",
		"-acodec", "pcm_s16le",
		"-ac", "1",
		"-ar", fmt.Sprintf("%d", SampleRate),

		"-",
	)

	return args
}

func (r *RTPStreamer) clearAudioQueue() {

	for {
		select {

		case <-r.audioQueue:

		default:
			return
		}
	}
}

func (r *RTPStreamer) startFFmpeg(url string) error {

	r.stopFFmpeg()

	r.clearAudioQueue()

	args := r.buildFFmpegArgs(url)

	cmd := exec.Command("ffmpeg", args...)

	stdout, err := cmd.StdoutPipe()

	if err != nil {
		return err
	}

	stderr, err := cmd.StderrPipe()

	if err != nil {
		return err
	}

	go func() {

		scanner := bufio.NewScanner(stderr)

		for scanner.Scan() {

			line := scanner.Text()

			if strings.TrimSpace(line) != "" {
				fmt.Println("[ffmpeg]", line)
			}
		}
	}()

	err = cmd.Start()

	if err != nil {
		return err
	}

	r.process = cmd
	r.stdout = stdout

	fmt.Println("[STREAM] Started")

	return nil
}

func (r *RTPStreamer) stopFFmpeg() {

	if r.process == nil {
		return
	}

	_ = r.process.Process.Kill()
	_, _ = r.process.Process.Wait()

	r.process = nil
	r.stdout = nil
}

func (r *RTPStreamer) buildRTPHeader() []byte {

	buf := make([]byte, 12)

	buf[0] = 0x80
	buf[1] = 96

	binary.BigEndian.PutUint16(
		buf[2:],
		r.seqNum,
	)

	binary.BigEndian.PutUint32(
		buf[4:],
		r.timestamp,
	)

	binary.BigEndian.PutUint32(
		buf[8:],
		r.ssrc,
	)

	return buf
}

func (r *RTPStreamer) sendPacket(audio []byte) error {

	header := r.buildRTPHeader()

	packet := append(header, audio...)

	_, err := r.conn.Write(packet)

	if err != nil {
		return err
	}

	r.seqNum++
	r.timestamp += ChunkSamples

	return nil
}

func (r *RTPStreamer) checkChannelSwitch() error {

	channel, url := r.manager.Current()

	if channel != r.currentChannel {

		fmt.Printf(
			"\n[CHANNEL] Switching to: %s\n",
			channel,
		)

		r.currentChannel = channel

		return r.startFFmpeg(url)
	}

	return nil
}

//
// PRODUCER
// Reads audio from ffmpeg
// Pushes into jitter buffer
//
func (r *RTPStreamer) producer() {

	buffer := make([]byte, ChunkSize)

	for {

		select {

		case <-r.stopChan:
			return

		default:
		}

		err := r.checkChannelSwitch()

		if err != nil {

			fmt.Println(
				"[ERROR] ffmpeg:",
				err,
			)

			time.Sleep(2 * time.Second)

			continue
		}

		if r.stdout == nil {

			time.Sleep(
				500 * time.Millisecond,
			)

			continue
		}

		_, err = io.ReadFull(
			r.stdout,
			buffer,
		)

		if err != nil {

			fmt.Println(
				"[WARN] stream reconnect",
			)

			time.Sleep(1 * time.Second)

			continue
		}

		packet := make([]byte, ChunkSize)

		copy(packet, buffer)

		select {

		case r.audioQueue <- packet:

		default:

			// queue full
			// drop oldest

			<-r.audioQueue

			r.audioQueue <- packet
		}
	}
}

//
// CONSUMER
// Sends RTP exactly every 40ms
//
func (r *RTPStreamer) consumer() {

	ticker := time.NewTicker(
		40 * time.Millisecond,
	)

	defer ticker.Stop()

	silence := make([]byte, ChunkSize)

	for {

		select {

		case <-r.stopChan:
			return

		case <-ticker.C:

			var audio []byte

			select {

			case audio = <-r.audioQueue:

			default:

				// underrun
				audio = silence

				fmt.Println(
					"[WARN] buffer underrun",
				)
			}

			err := r.sendPacket(audio)

			if err != nil {

				fmt.Println(
					"[ERROR] RTP send:",
					err,
				)
			}
		}
	}
}

func (r *RTPStreamer) Stop() {

	close(r.stopChan)

	r.stopFFmpeg()

	r.conn.Close()
}

func console(manager *ChannelManager) {

	scanner := bufio.NewScanner(os.Stdin)

	fmt.Println(`
Commands:
  list
  switch <name>
  add <name> <url>
  current
  exit
`)

	for {

		fmt.Print("radio> ")

		if !scanner.Scan() {
			return
		}

		line := strings.TrimSpace(
			scanner.Text(),
		)

		if line == "" {
			continue
		}

		parts := strings.SplitN(
			line,
			" ",
			3,
		)

		switch parts[0] {

		case "list":

			fmt.Println()

			for k, v := range manager.List() {

				fmt.Printf(
					"- %s\n  %s\n\n",
					k,
					v,
				)
			}

		case "switch":

			if len(parts) < 2 {

				fmt.Println(
					"usage: switch <channel>",
				)

				continue
			}

			if manager.Switch(parts[1]) {

				fmt.Println("switched")

			} else {

				fmt.Println(
					"channel not found",
				)
			}

		case "add":

			if len(parts) < 3 {

				fmt.Println(
					"usage: add <name> <url>",
				)

				continue
			}

			err := manager.Add(
				parts[1],
				parts[2],
			)

			if err != nil {

				fmt.Println(
					"save error:",
					err,
				)

			} else {

				fmt.Println(
					"channel added",
				)
			}

		case "current":

			ch, url := manager.Current()

			fmt.Println("Current:", ch)
			fmt.Println(url)

		case "exit":

			os.Exit(0)

		default:

			fmt.Println(
				"unknown command",
			)
		}
	}
}

func main() {

	fmt.Println(`
=================================
      RTP RADIO STREAMER
=================================
`)

	manager := NewChannelManager()

	streamer := NewRTPStreamer(
		ESP32IP,
		manager,
	)

	// audio producer
	go streamer.producer()

	// precise RTP sender
	go streamer.consumer()

	// interactive console
	go console(manager)

	sigChan := make(chan os.Signal, 1)

	signal.Notify(
		sigChan,
		syscall.SIGINT,
		syscall.SIGTERM,
	)

	<-sigChan

	fmt.Println("\nStopping...")

	streamer.Stop()
}