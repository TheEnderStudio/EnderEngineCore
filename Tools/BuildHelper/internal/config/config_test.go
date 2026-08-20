package config

import (
	"path/filepath"
	"testing"
)

// TestLoadRealDeps 验证仓库根目录 deps.yaml 能被正确解析且字段完整。
func TestLoadRealDeps(t *testing.T) {
	cfg, err := Load(filepath.Join("..", "..", "deps.yaml"))
	if err != nil {
		t.Fatalf("加载 deps.yaml 失败: %v", err)
	}
	if len(cfg.Dependencies) != 15 {
		t.Fatalf("依赖数量 = %d, 期望 15", len(cfg.Dependencies))
	}
	for _, d := range cfg.Dependencies {
		if d.Name == "" || d.Target == "" {
			t.Fatalf("依赖 %+v 缺少 name/target", d)
		}
		switch d.Kind {
		case "git", "archive", "userpath":
		default:
			t.Fatalf("依赖 %s 非法 kind: %q", d.Name, d.Kind)
		}
		if len(d.Layout) == 0 {
			t.Fatalf("依赖 %s 没有 layout 规则", d.Name)
		}
		for _, r := range d.Layout {
			if r.From == "" || r.To == "" {
				t.Fatalf("依赖 %s 的规则 %+v 缺少 from/to", d.Name, r)
			}
		}
	}
	// PhysX 必须是 userpath（用户手动构建，工具按路径导入）
	physx := cfg.Find("PhysX")
	if physx == nil {
		t.Fatal("缺少 PhysX 依赖")
	}
	if physx.Kind != "userpath" {
		t.Fatalf("PhysX kind = %q, 期望 userpath", physx.Kind)
	}
	if physx.Build != "none" {
		t.Fatalf("PhysX build = %q, 期望 none", physx.Build)
	}
}

// TestDefaultParses 验证内置默认配置可解析。
func TestDefaultParses(t *testing.T) {
	cfg := Default()
	if len(cfg.Dependencies) != 15 {
		t.Fatalf("内置默认依赖数量 = %d, 期望 15", len(cfg.Dependencies))
	}
	if cfg.Engine.Generator != "" {
		t.Fatalf("默认生成器应为空（自动检测），得到 %q", cfg.Engine.Generator)
	}
}
