package ui

import (
	"testing"
	"time"

	"buildhelper/internal/runner"
)

// TestDrainEventsIdle 验证通道空闲时 drainEvents 立即返回，不会无限空转。
// （回归测试：曾因 select 里的普通 break 不退出 for 循环，导致
//
//	pump 单核 100% 空转、日志冻结。）
func TestDrainEventsIdle(t *testing.T) {
	ch := make(chan runner.Event, 8)
	ch <- runner.Event{Dep: "a", Type: runner.EvLog, Message: "1"}
	ch <- runner.Event{Dep: "a", Type: runner.EvLog, Message: "2"}
	ch <- runner.Event{Dep: "a", Type: runner.EvLog, Message: "3"}

	done := make(chan []runner.Event, 1)
	go func() {
		done <- drainEvents(ch, 256)
	}()

	select {
	case batch := <-done:
		if len(batch) != 3 {
			t.Fatalf("批量大小 = %d, 期望 3", len(batch))
		}
	case <-time.After(2 * time.Second):
		t.Fatal("drainEvents 空转超时（livelock 回归！）")
	}
}

// TestDrainEventsCap 验证事件超过上限时按上限批量返回。
func TestDrainEventsCap(t *testing.T) {
	ch := make(chan runner.Event, 512)
	for i := 0; i < 300; i++ {
		ch <- runner.Event{Dep: "a", Type: runner.EvLog, Message: "x"}
	}
	batch := drainEvents(ch, 256)
	if len(batch) != 256 {
		t.Fatalf("批量大小 = %d, 期望 256", len(batch))
	}
}

// TestDrainEventsClosed 验证通道关闭时返回 nil。
func TestDrainEventsClosed(t *testing.T) {
	ch := make(chan runner.Event)
	close(ch)
	if batch := drainEvents(ch, 256); batch != nil {
		t.Fatalf("关闭通道应返回 nil, 得到 %d 条", len(batch))
	}
}
