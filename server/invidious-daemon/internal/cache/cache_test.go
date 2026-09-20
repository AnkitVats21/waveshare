package cache

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestMemoryCache(t *testing.T) {
	c := NewMemoryCache()

	// Test Set and Get
	c.Set("key1", "val1", 100*time.Millisecond)
	val, ok := c.Get("key1")
	if !ok || val != "val1" {
		t.Fatalf("expected val1, got %v (ok=%v)", val, ok)
	}

	// Test Expiration
	time.Sleep(120 * time.Millisecond)
	_, ok = c.Get("key1")
	if ok {
		t.Fatalf("expected key1 to expire")
	}

	// Test Delete
	c.Set("key2", "val2", 1*time.Hour)
	c.Delete("key2")
	_, ok = c.Get("key2")
	if ok {
		t.Fatalf("expected key2 to be deleted")
	}
}

func TestSingleFlight(t *testing.T) {
	sf := NewSingleFlight()
	var execCount int32

	var wg sync.WaitGroup
	for i := 0; i < 10; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			val, err := sf.Do("test_call", func() (any, error) {
				atomic.AddInt32(&execCount, 1)
				time.Sleep(50 * time.Millisecond)
				return "result", nil
			})
			if err != nil || val != "result" {
				t.Errorf("unexpected result: %v, err: %v", val, err)
			}
		}()
	}
	wg.Wait()

	if count := atomic.LoadInt32(&execCount); count != 1 {
		t.Fatalf("expected function to execute once, executed %d times", count)
	}
}
