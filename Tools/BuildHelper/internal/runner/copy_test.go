package runner

import (
	"os"
	"path/filepath"
	"testing"
)

// TestCopyTreeSingleFile 验证单文件源能正确复制到指定目标名。
// （回归测试：曾因 WalkDir 跳过根节点导致单文件复制静默失败，
//
//	如 fastgltf 的 Debug 库改名 fastgltfd.lib、LICENSE/README 等文档规则。）
func TestCopyTreeSingleFile(t *testing.T) {
	srcDir := t.TempDir()
	dstDir := t.TempDir()

	src := filepath.Join(srcDir, "fastgltf.lib")
	if err := os.WriteFile(src, []byte("lib-data"), 0o644); err != nil {
		t.Fatal(err)
	}

	dst := filepath.Join(dstDir, "lib", "fastgltfd.lib") // to 为目标文件名
	if err := copyTree(src, dst, "*"); err != nil {
		t.Fatalf("copyTree 单文件复制失败: %v", err)
	}
	data, err := os.ReadFile(dst)
	if err != nil {
		t.Fatalf("目标文件不存在: %v", err)
	}
	if string(data) != "lib-data" {
		t.Fatalf("内容 = %q, 期望 lib-data", string(data))
	}
}

// TestCopyTreeDir 验证目录复制与 .git 跳过。
func TestCopyTreeDir(t *testing.T) {
	srcDir := t.TempDir()
	dstDir := t.TempDir()

	if err := os.MkdirAll(filepath.Join(srcDir, "include", "fastgltf"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(srcDir, ".git"), 0o755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(filepath.Join(srcDir, "include", "fastgltf", "core.hpp"), []byte("h"), 0o644)
	_ = os.WriteFile(filepath.Join(srcDir, ".git", "HEAD"), []byte("ref"), 0o644)

	if err := copyTree(srcDir, filepath.Join(dstDir, "out"), "*"); err != nil {
		t.Fatalf("copyTree 目录复制失败: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dstDir, "out", "include", "fastgltf", "core.hpp")); err != nil {
		t.Fatalf("头文件未复制: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dstDir, "out", ".git")); err == nil {
		t.Fatal(".git 不应被复制")
	}
}

// TestCopyTreePattern 验证 pattern 过滤。
func TestCopyTreePattern(t *testing.T) {
	srcDir := t.TempDir()
	dstDir := t.TempDir()
	_ = os.WriteFile(filepath.Join(srcDir, "a.lib"), []byte("1"), 0o644)
	_ = os.WriteFile(filepath.Join(srcDir, "b.txt"), []byte("2"), 0o644)

	if err := copyTree(srcDir, dstDir, "*.lib"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dstDir, "a.lib")); err != nil {
		t.Fatal("a.lib 未复制")
	}
	if _, err := os.Stat(filepath.Join(dstDir, "b.txt")); err == nil {
		t.Fatal("b.txt 不应被复制（pattern 过滤）")
	}
}
