// EnderEngine BuildHelper 入口。
//
// 用法:
//
//	BuildHelper.exe                # 自动查找 deps.yaml 并启动图形界面
//	BuildHelper.exe -config X.yaml # 指定配置文件
//	BuildHelper.exe -root D:\Eng   # 指定引擎根目录
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"buildhelper/internal/config"
	"buildhelper/internal/ui"
)

func main() {
	var cfgPath, root string
	flag.StringVar(&cfgPath, "config", "", "依赖配置文件路径（默认自动查找 deps.yaml）")
	flag.StringVar(&root, "root", "", "引擎根目录（默认自动推断）")
	flag.Parse()

	if cfgPath == "" {
		cfgPath = findConfig()
	}
	if cfgPath == "" {
		fmt.Fprintln(os.Stderr, "未找到 deps.yaml，请使用 -config 指定，或在 BuildHelper 目录下运行")
		os.Exit(1)
	}
	cfg, err := config.Load(cfgPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "加载配置失败:", err)
		os.Exit(1)
	}
	if root == "" {
		root = cfg.Engine.Root
	}
	a := ui.New(cfg, cfgPath, root)
	a.Run()
}

// findConfig 在可执行文件目录、当前目录及其上级目录中查找 deps.yaml。
func findConfig() string {
	seen := map[string]bool{}
	add := func(d string) {
		if d == "" || seen[d] {
			return
		}
		seen[d] = true
	}
	if exe, err := os.Executable(); err == nil {
		add(filepath.Dir(exe))
	}
	if wd, err := os.Getwd(); err == nil {
		add(wd)
		for p := filepath.Dir(wd); p != filepath.Dir(p); p = filepath.Dir(p) {
			add(p)
		}
	}
	for d := range seen {
		c := filepath.Join(d, "deps.yaml")
		if fi, err := os.Stat(c); err == nil && !fi.IsDir() {
			return c
		}
	}
	return ""
}
